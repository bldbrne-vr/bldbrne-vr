// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

// Beast Claw dismemberment: the rules of what the cut's worker keeps read (its enemy models, a
// read ahead of a kill), pure and bounded for the tests (bloodborne_sever.cpp holds the data).
namespace VR::Sever::Cache {

// How many of the oldest models go (their sizes oldest first) for at most `max_count` to stay,
// together at most `budget` bytes: never the newest (the one in use), however big it is.
template <typename Range, typename SizeOf = std::identity>
std::size_t TrimCount(const Range& items, std::size_t max_count, std::size_t budget,
                      SizeOf size_of = {}) {
    // Walk the retained suffix directly. The newest stays even if it exceeds either limit.
    std::size_t first = items.size();
    while (first > 0) {
        const std::size_t bytes = size_of(items[first - 1]);
        if (first < items.size() && (items.size() - first >= max_count || bytes > budget)) {
            break;
        }
        --first;
        if (bytes > budget) {
            break;
        }
        budget -= bytes;
    }
    return first;
}

// The read-aheads waiting (under the worker's queue lock): never two of one model, never more
// than N (more are dropped: the kill reads its own), the newest taken first.
template <std::size_t N>
struct Wanted {
    std::array<std::int32_t, N> chr{};
    std::size_t count{0};

    bool Add(std::int32_t c) {
        if (c <= 0 || count >= N ||
            std::find(chr.begin(), chr.begin() + count, c) != chr.begin() + count) {
            return false;
        }
        chr[count++] = c;
        return true;
    }
    bool Take(std::int32_t& c) {
        if (count == 0) {
            return false;
        }
        c = chr[--count];
        return true;
    }
    bool Empty() const {
        return count == 0;
    }
};

// The models the hit hook asked for lately (any thread): each asked once, the oldest forgotten
// after N more. Relaxed: a race costs one more ask, which Wanted drops.
template <std::size_t N>
struct Recent {
    std::array<std::atomic<std::int32_t>, N> chr{};
    std::atomic<std::uint32_t> next{0};

    bool Has(std::int32_t c) const {
        for (const auto& a : chr) {
            if (a.load(std::memory_order_relaxed) == c) {
                return true;
            }
        }
        return false;
    }
    void Note(std::int32_t c) {
        chr[next.fetch_add(1, std::memory_order_relaxed) % N].store(c, std::memory_order_relaxed);
    }
    void Forget(std::int32_t c) {
        for (auto& slot : chr) {
            if (slot.load(std::memory_order_relaxed) == c) {
                auto expected = c;
                slot.compare_exchange_strong(expected, 0, std::memory_order_relaxed);
            }
        }
    }
    void Clear() {
        for (auto& a : chr) {
            a.store(0, std::memory_order_relaxed);
        }
    }
};

// Where models can never be cut (the kill hook's test: no bite for a cut that cannot come). One
// word a slot, the oldest replaced: a model's character (the top 16 bits) and its sites (the low
// 48), so a reader never pairs one model's character with another's sites (a code review,
// 2026-11-16: two atomics, a slot replaced between the reads). One writer (the worker), any
// readers; no locks, no allocation.
template <std::size_t N>
struct Uncuttable {
    static constexpr std::uint64_t AllSites = (std::uint64_t{1} << 48) - 1;
    std::array<std::atomic<std::uint64_t>, N> slots{};
    std::size_t next{0}; // (the writer's)

    static constexpr bool Packable(std::int32_t chr) {
        return chr > 0 && chr <= 0xFFFF;
    }
    void Mark(std::int32_t chr, std::uint64_t sites) {
        if (!Packable(chr)) {
            return;
        }
        const std::uint64_t who = static_cast<std::uint64_t>(chr) << 48;
        for (auto& slot : slots) {
            const std::uint64_t word = slot.load(std::memory_order_relaxed); // (its own writes)
            if ((word & ~AllSites) == who) {
                slot.store(word | (sites & AllSites), std::memory_order_release);
                return;
            }
        }
        slots[next++ % N].store(who | (sites & AllSites), std::memory_order_release);
    }
    bool Has(std::int32_t chr, std::uint64_t site_bit) const {
        if (!Packable(chr)) {
            return false;
        }
        const std::uint64_t who = static_cast<std::uint64_t>(chr) << 48;
        for (const auto& slot : slots) {
            const std::uint64_t word = slot.load(std::memory_order_acquire);
            if ((word & ~AllSites) == who) {
                return (word & site_bit & AllSites) != 0;
            }
        }
        return false;
    }
};

// Where one struck body of a model can never be cut while its others can (a creature's segment cut
// by its own bone's name, one head of many: Uncuttable's whole-limb bit would block those): its id
// (sever_policy.h StruckId, never 0), the oldest replaced. The kill hook's test beside Uncuttable.
// One writer, any readers; no locks, no allocation.
template <std::size_t N>
struct FailedHits {
    std::array<std::atomic<std::uint64_t>, N> slots{};
    std::size_t next{0}; // (the writer's)

    void Mark(std::uint64_t id) {
        if (id != 0 && !Has(id)) {
            slots[next++ % N].store(id, std::memory_order_release);
        }
    }
    bool Has(std::uint64_t id) const {
        if (id == 0) {
            return false;
        }
        for (const auto& slot : slots) {
            if (slot.load(std::memory_order_acquire) == id) {
                return true;
            }
        }
        return false;
    }
};

} // namespace VR::Sever::Cache
