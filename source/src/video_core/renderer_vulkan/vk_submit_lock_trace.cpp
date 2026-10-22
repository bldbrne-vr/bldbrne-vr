// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "common/thread.h"
#include "video_core/renderer_vulkan/vk_submit_lock_trace.h"

namespace Vulkan::LockTrace {

namespace {

constexpr s64 LogHoldNs = 2'000'000; // holds logged by their holder
constexpr s64 LogWaitNs = 2'000'000; // waits logged with what they waited behind
constexpr s64 BigWaitNs = 5'000'000; // the summary's ">= 5 ms" waits

s64 Ns(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

// The last holds, oldest first from ring_next. Written and read only inside a Hold (the queue lock
// is held then), so in the order the lock was released: end_ns never decreases.
constexpr u32 RingSize = 2048;
std::array<Rec, RingSize> ring{};
u64 ring_next = 0;

thread_local Hold* current = nullptr;
thread_local const char* thread_name = nullptr;
// This thread's trace lines not yet written (FlushLogs, after the lock is released).
thread_local std::vector<std::string> pending_logs;

// Common::Log's sink timing hook: a log line written while this thread holds the queue lock (the
// logger is synchronous with log.sync, the console's writes included) is a phase of the hold.
void OnLogWrite(long long ns) {
    if (current == nullptr) {
        return;
    }
    Phase::AddTo(current, "log write (sync logger)", ns);
}

// Interned thread names: a record may outlive nothing it points to.
std::mutex names_mutex;
std::deque<std::string> names;

const char* ThreadName() {
    if (thread_name == nullptr) {
        std::string name = Common::GetCurrentThreadName();
        constexpr std::string_view prefix = "shadPS4:";
        if (name.starts_with(prefix)) {
            name.erase(0, prefix.size());
        }
        std::scoped_lock lk{names_mutex};
        thread_name = names.emplace_back(std::move(name)).c_str();
    }
    return thread_name;
}

bool IsFrameThread() {
    return std::strcmp(ThreadName(), "OpenXRFrameLoop") == 0;
}

// The phases worth naming (0.05 ms or more, the longest first, at most `max`).
std::string PhaseText(const Rec& r, u32 max = 5) {
    std::array<const PhaseRec*, MaxPhases> order{};
    u32 n = 0;
    for (u32 i = 0; i < r.phase_count; ++i) {
        if (r.phases[i].ns >= 50'000) {
            order[n++] = &r.phases[i];
        }
    }
    std::sort(order.begin(), order.begin() + n,
              [](const PhaseRec* a, const PhaseRec* b) { return a->ns > b->ns; });
    std::string out;
    for (u32 i = 0; i < std::min(n, max); ++i) {
        out += fmt::format("{}{} {:.2f} ms", out.empty() ? "" : ", ", order[i]->name,
                           static_cast<double>(order[i]->ns) * 1e-6);
        if (order[i]->count > 1) {
            out += fmt::format(" x{}", order[i]->count);
        }
    }
    return out;
}

struct Key {
    const char* site;
    const char* thread;
    bool operator==(const Key&) const = default;
};

struct SiteStats {
    Key key{};
    u64 holds{};
    u64 long_holds{};
    s64 sum_ns{};
    s64 max_ns{};
    s64 long_ns{};
    std::string max_text; // the longest hold's phases
};

struct Behind {
    Key key{};
    s64 ns{};
    u32 holds{};
};

struct Window {
    std::vector<SiteStats> sites;
    u64 frame_waits_2{}, frame_waits_5{};
    s64 frame_wait_max_ns{}, frame_wait_sum_ns{};
    std::vector<Behind> behind; // frame-thread waits of 2 ms or more, by holder
    s64 unexplained_ns{};
    u64 other_waits_2{}; // other threads' waits of 2 ms or more
    s64 other_wait_max_ns{};
    const char* other_wait_max_site{};
};

std::mutex stats_mutex;
Window window;
// The frame thread's long waits since TakeFrameWaitText (written and read by the frame thread only).
std::string frame_wait_text;

SiteStats& StatsFor(Window& w, Key key) {
    for (auto& s : w.sites) {
        if (s.key == key) {
            return s;
        }
    }
    return w.sites.emplace_back(SiteStats{key});
}

// A wait of LogWaitNs or more ended (the lock is held): the holds it waited behind are the ones
// released after it began.
void ReportWait(const Rec& waiter, s64 wait_start, s64 now) {
    struct Item {
        Key key;
        s64 ns;
        u32 holds;
        const Rec* longest;
        s64 longest_ns;
    };
    std::vector<Item> items;
    s64 covered = 0;
    for (u32 k = 1; k <= RingSize && k <= ring_next; ++k) {
        const Rec& r = ring[(ring_next - k) % RingSize];
        if (r.end_ns <= wait_start) {
            break;
        }
        const s64 overlap = std::min(r.end_ns, now) - std::max(r.start_ns, wait_start);
        if (overlap <= 0) {
            continue;
        }
        covered += overlap;
        const Key key{r.site, r.thread};
        auto it = std::find_if(items.begin(), items.end(), [&](const Item& i) { return i.key == key; });
        if (it == items.end()) {
            items.push_back({key, overlap, 1, &r, overlap});
        } else {
            it->ns += overlap;
            ++it->holds;
            if (overlap > it->longest_ns) {
                it->longest = &r;
                it->longest_ns = overlap;
            }
        }
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.ns > b.ns; });
    const s64 wait = now - wait_start;
    std::string text;
    for (const Item& i : items) {
        text += fmt::format("{}{} ({}) {:.2f} ms", text.empty() ? "" : "; ", i.key.site,
                            i.key.thread, static_cast<double>(i.ns) * 1e-6);
        if (i.holds > 1) {
            text += fmt::format(" in {} holds", i.holds);
        }
        const std::string phases = PhaseText(*i.longest, 4);
        if (!phases.empty()) {
            text += fmt::format(" [{}{}]", i.holds > 1 ? "longest: " : "", phases);
        }
    }
    const s64 unexplained = std::max<s64>(wait - covered, 0);
    pending_logs.push_back(fmt::format(
        "VR lock: wait {} {:.2f} ms behind: {}; {:.2f} ms with no traced holder", waiter.site,
        static_cast<double>(wait) * 1e-6, text.empty() ? "nothing traced" : text,
        static_cast<double>(unexplained) * 1e-6));
    const bool frame = IsFrameThread();
    if (frame) {
        frame_wait_text += fmt::format("{}{} {:.2f} ms behind {}", frame_wait_text.empty() ? "" : " | ",
                                       waiter.site, static_cast<double>(wait) * 1e-6,
                                       text.empty() ? "nothing traced" : text);
    }
    std::scoped_lock lk{stats_mutex};
    if (!frame) {
        ++window.other_waits_2;
        if (wait > window.other_wait_max_ns) {
            window.other_wait_max_ns = wait;
            window.other_wait_max_site = waiter.site;
        }
        return;
    }
    for (const Item& i : items) {
        auto it = std::find_if(window.behind.begin(), window.behind.end(),
                               [&](const Behind& b) { return b.key == i.key; });
        if (it == window.behind.end()) {
            window.behind.push_back({i.key, i.ns, i.holds});
        } else {
            it->ns += i.ns;
            it->holds += i.holds;
        }
    }
    window.unexplained_ns += unexplained;
}

} // Anonymous namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("VR_LOCK_TRACE");
        const bool on = value != nullptr && *value != '\0' && *value != '0';
        if (on) {
            Common::Log::SetSinkTimingHook(&OnLogWrite);
            LOG_INFO(Render_Vulkan, "VR lock trace: on (VR_LOCK_TRACE): queue-lock holds and waits "
                                    "of 2 ms or more are logged");
        }
        return on;
    }();
    return enabled;
}

Hold::Hold(const char* site, Clock::time_point wait_start) {
    if (!Enabled()) {
        return;
    }
    on = true;
    const s64 now = Ns(Clock::now());
    rec.site = site;
    rec.thread = ThreadName();
    rec.start_ns = now;
    const bool timed = wait_start != Clock::time_point{};
    rec.wait_ns = timed ? now - Ns(wait_start) : -1;
    prev = current;
    current = this;
    if (timed) {
        if (IsFrameThread()) {
            std::scoped_lock lk{stats_mutex};
            window.frame_wait_sum_ns += rec.wait_ns;
            window.frame_wait_max_ns = std::max(window.frame_wait_max_ns, rec.wait_ns);
            window.frame_waits_2 += rec.wait_ns >= LogWaitNs ? 1 : 0;
            window.frame_waits_5 += rec.wait_ns >= BigWaitNs ? 1 : 0;
        }
        if (rec.wait_ns >= LogWaitNs) {
            ReportWait(rec, Ns(wait_start), now);
        }
    }
}

Hold::~Hold() {
    if (!on) {
        return;
    }
    current = prev;
    rec.end_ns = Ns(Clock::now());
    ring[ring_next % RingSize] = rec;
    ++ring_next;
    const s64 held = rec.end_ns - rec.start_ns;
    const bool long_hold = held >= LogHoldNs;
    std::string phases;
    if (long_hold) {
        phases = PhaseText(rec);
    }
    {
        std::scoped_lock lk{stats_mutex};
        SiteStats& s = StatsFor(window, {rec.site, rec.thread});
        ++s.holds;
        s.sum_ns += held;
        if (held > s.max_ns) {
            s.max_ns = held;
            s.max_text = phases;
        }
        if (long_hold) {
            ++s.long_holds;
            s.long_ns += held;
        }
    }
    if (long_hold) {
        pending_logs.push_back(fmt::format("VR lock: hold {} {:.2f} ms{}{}", rec.site,
                                           static_cast<double>(held) * 1e-6,
                                           phases.empty() ? "" : ": ", phases));
    }
}

void FlushLogs() {
    if (pending_logs.empty()) {
        return;
    }
    std::vector<std::string> lines = std::exchange(pending_logs, {});
    for (const std::string& line : lines) {
        LOG_INFO(Render_Vulkan, "{}", line);
    }
}

Phase::Phase(const char* name_) {
    if (!Enabled() || current == nullptr) {
        return;
    }
    hold = current;
    name = name_;
    start = Clock::now();
}

Phase::~Phase() {
    if (hold == nullptr) {
        return;
    }
    const s64 ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
    AddTo(hold, name, ns, counted ? count : 1); // Count(): its items, else one more time through
}

void Phase::AddTo(Hold* hold, const char* name, s64 ns, u32 items) {
    Rec& r = hold->rec;
    for (u32 i = 0; i < r.phase_count; ++i) {
        if (r.phases[i].name == name) {
            r.phases[i].ns += ns;
            r.phases[i].count += items;
            return;
        }
    }
    if (r.phase_count < MaxPhases) {
        r.phases[r.phase_count++] = {name, ns, items};
    }
}

void Phase::Count(u32 n) {
    counted = true;
    count += n;
}

std::string TakeSummary(double seconds) {
    if (!Enabled()) {
        return {};
    }
    Window w;
    {
        std::scoped_lock lk{stats_mutex};
        w = std::exchange(window, Window{});
    }
    std::string out = fmt::format(
        "{:.1f} s: frame-thread waits >= 2 ms {}, >= 5 ms {} (max {:.2f} ms, all waits {:.2f} ms)",
        seconds, w.frame_waits_2, w.frame_waits_5, static_cast<double>(w.frame_wait_max_ns) * 1e-6,
        static_cast<double>(w.frame_wait_sum_ns) * 1e-6);
    if (w.frame_waits_2 != 0) {
        std::sort(w.behind.begin(), w.behind.end(),
                  [](const Behind& a, const Behind& b) { return a.ns > b.ns; });
        out += ", behind:";
        for (std::size_t i = 0; i < std::min<std::size_t>(w.behind.size(), 5); ++i) {
            out += fmt::format(" {} ({}) {:.2f} ms in {} holds;", w.behind[i].key.site,
                               w.behind[i].key.thread, static_cast<double>(w.behind[i].ns) * 1e-6,
                               w.behind[i].holds);
        }
        out += fmt::format(" no traced holder {:.2f} ms", static_cast<double>(w.unexplained_ns) * 1e-6);
    }
    if (w.other_waits_2 != 0) {
        out += fmt::format("; other waits >= 2 ms {} (max {:.2f} ms, {})", w.other_waits_2,
                           static_cast<double>(w.other_wait_max_ns) * 1e-6,
                           w.other_wait_max_site ? w.other_wait_max_site : "?");
    }
    std::sort(w.sites.begin(), w.sites.end(),
              [](const SiteStats& a, const SiteStats& b) { return a.max_ns > b.max_ns; });
    out += "; holds by site (n, avg / max ms, >= 2 ms):";
    for (const SiteStats& s : w.sites) {
        out += fmt::format(" {} ({}) {} {:.3f} / {:.2f} {}{}{}{};", s.key.site, s.key.thread, s.holds,
                           s.holds ? static_cast<double>(s.sum_ns) * 1e-6 / static_cast<double>(s.holds)
                                   : 0.0,
                           static_cast<double>(s.max_ns) * 1e-6, s.long_holds,
                           s.max_text.empty() ? "" : " [max: ", s.max_text,
                           s.max_text.empty() ? "" : "]");
    }
    return out;
}

std::string TakeFrameWaitText() {
    return std::exchange(frame_wait_text, std::string{});
}

} // namespace Vulkan::LockTrace
