// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <mutex>

#include "core/vr/inframe.h"
#include "core/vr/seqlock.h"

namespace VR::InFrame {

namespace {

constexpr std::size_t RingSize = 16; // up to 8 camera records x the producers
// Available(): this many lists in a row submitted and none drawn means the renderer cannot draw
// into this game's frame; it is tried again after RetryNs.
constexpr std::uint64_t RefusedStreak = 30;
constexpr std::int64_t RetryNs = 20'000'000'000;

struct Ring {
    std::mutex mutex;
    std::array<List, RingSize> lists{};
    std::size_t next{0};
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> taken{0};
    std::atomic<std::uint64_t> missed{0};
    std::atomic<std::uint64_t> drawn_at{0}; // `submitted` when a list was last drawn
    std::atomic<bool> disabled{false};
    std::atomic<std::int64_t> refused_ns{0}; // when Available() first said no (0: it did not)
};

Ring& R() {
    static Ring ring;
    return ring;
}

SeqLock<ProbeResult> g_probe;

// Appends `from`'s items and labels to `out` (its label indices moved past out's own; an item
// whose label did not fit is left out).
void Append(List& out, const List& from) {
    const std::uint32_t base = out.label_count;
    std::uint32_t labels = 0;
    for (std::uint32_t i = 0; i < from.label_count && out.label_count < MaxLabels; ++i) {
        out.labels[out.label_count++] = from.labels[i];
        ++labels;
    }
    for (std::uint32_t i = 0; i < from.count && out.count < MaxItems; ++i) {
        Item item = from.items[i];
        if (item.tex == Tex::Label) {
            if (item.tex_key >= labels) {
                continue;
            }
            item.tex_key += base;
        }
        if (item.fit_label >= 0) {
            if (static_cast<std::uint32_t>(item.fit_label) >= labels) {
                continue;
            }
            item.fit_label = static_cast<std::int8_t>(item.fit_label + static_cast<int>(base));
        }
        out.items[out.count++] = item;
    }
}

} // Anonymous namespace

void Submit(const List& list) {
    auto& r = R();
    {
        std::scoped_lock lock{r.mutex};
        std::size_t slot = r.next;
        for (std::size_t i = 0; i < RingSize; ++i) {
            if (r.lists[i].record == list.record && r.lists[i].producer == list.producer) {
                slot = i; // the same camera update again (an AER eye's second build)
                break;
            }
        }
        if (slot == r.next) {
            r.next = (r.next + 1) % RingSize;
        }
        r.lists[slot] = list;
    }
    r.submitted.fetch_add(1, std::memory_order_relaxed);
}

namespace {
std::mutex g_hand_carry_mutex;
HandCarry g_hand_carry{};
} // Anonymous namespace

void SetHandCarry(const HandCarry& carry) {
    std::scoped_lock lock{g_hand_carry_mutex};
    g_hand_carry = carry;
}

bool GetHandCarry(std::uint64_t record, HandCarry& out) {
    if (record == 0) {
        return false;
    }
    std::scoped_lock lock{g_hand_carry_mutex};
    if (g_hand_carry.record != record) {
        return false;
    }
    out = g_hand_carry;
    return true;
}

std::uint32_t Carry(std::uint64_t record, const ReView& rv) {
    if (record == 0) {
        return 0;
    }
    auto& r = R();
    std::uint32_t moved = 0;
    std::scoped_lock lock{r.mutex};
    for (auto& list : r.lists) {
        if (list.record != record) {
            continue;
        }
        static List out; // (the ring's mutex is held: one at a time; ~30 KB off the stack)
        if (!RebaseList(list, &list, rv, record, out)) {
            continue;
        }
        for (std::uint32_t i = 0; i < out.count; ++i) {
            moved += out.items[i].hand ? 1u : 0u;
        }
        list = out;
    }
    return moved;
}

bool Take(std::uint64_t record, List& out) {
    auto& r = R();
    if (record == 0) {
        return false;
    }
    std::scoped_lock lock{r.mutex};
    std::array<const List*, static_cast<std::size_t>(Producer::Count)> parts{};
    for (const auto& list : r.lists) {
        const auto p = static_cast<std::size_t>(list.producer);
        if (list.record == record && p < parts.size()) {
            parts[p] = &list;
        }
    }
    const List* quick = parts[static_cast<std::size_t>(Producer::Quick)];
    const List* vitals = parts[static_cast<std::size_t>(Producer::Vitals)];
    if (quick == nullptr && vitals == nullptr) {
        return false;
    }
    if (vitals == nullptr) {
        out = *quick;
    } else {
        // The gauge first (under any card of the arc), then the quick items.
        out = *vitals;
        out.producer = Producer::Quick;
        if (quick != nullptr) {
            out.exposure = quick->exposure;
            out.steady_ns = std::max(out.steady_ns, quick->steady_ns);
            Append(out, *quick);
        }
    }
    r.taken.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void PublishProbe(const ProbeResult& result) {
    g_probe.Store(result);
}

bool LatestProbe(ProbeResult& out) {
    return g_probe.Load(out);
}

bool Has(std::uint64_t record) {
    auto& r = R();
    if (record == 0) {
        return false;
    }
    std::scoped_lock lock{r.mutex};
    for (const auto& list : r.lists) {
        if (list.record == record) {
            return true;
        }
    }
    return false;
}

Stats GetStats() {
    auto& r = R();
    return {r.submitted.load(std::memory_order_relaxed), r.taken.load(std::memory_order_relaxed),
            r.missed.load(std::memory_order_relaxed)};
}

void NoteMiss() {
    R().missed.fetch_add(1, std::memory_order_relaxed);
}

void NoteDrawn() {
    auto& r = R();
    r.drawn_at.store(r.submitted.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

void NoteDisabled() {
    R().disabled.store(true, std::memory_order_relaxed);
}

bool Available() {
    auto& r = R();
    if (r.disabled.load(std::memory_order_relaxed)) {
        return false;
    }
    const std::uint64_t submitted = r.submitted.load(std::memory_order_relaxed);
    if (submitted - r.drawn_at.load(std::memory_order_relaxed) < RefusedStreak) {
        r.refused_ns.store(0, std::memory_order_relaxed);
        return true;
    }
    const std::int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
    const std::int64_t since = r.refused_ns.load(std::memory_order_relaxed);
    if (since == 0) {
        r.refused_ns.store(now, std::memory_order_relaxed);
        return false;
    }
    if (now - since >= RetryNs) {
        r.drawn_at.store(submitted, std::memory_order_relaxed); // one more try
        r.refused_ns.store(0, std::memory_order_relaxed);
        return true;
    }
    return false;
}

} // namespace VR::InFrame
