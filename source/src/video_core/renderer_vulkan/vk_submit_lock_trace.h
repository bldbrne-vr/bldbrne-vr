// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <array>
#include <chrono>
#include <string>
#include "common/types.h"

// VR_LOCK_TRACE=1 (profiling, off by default): who holds Scheduler::submit_mutex (the queue lock
// shared by the game's submits, the desktop present, the VR presenter's copies and the OpenXR frame
// thread), for how long and doing what. Every hold is timed by its site and by the phases inside it
// (vkQueueSubmit, the deferred operations, xrWaitSwapchainImage, xrEndFrame...):
//  - a hold of 2 ms or more is logged by its holder (`VR lock: hold ...`);
//  - a wait of 2 ms or more is logged by the waiter with the holds it waited behind (`VR lock: wait
//    ...`), each with its phases, and the part no traced hold explains;
//  - every 5 s the frame thread logs a summary (`VR lock trace:`): its waits of 2 / 5 ms and more,
//    whom they waited behind, the long holds by site.
// Off: Now() reads no clock and Hold / Phase do nothing but test a cached flag.
namespace Vulkan::LockTrace {

bool Enabled();

using Clock = std::chrono::steady_clock;

// Clock::now() while tracing, else a zero time point (no clock read).
inline Clock::time_point Now() {
    return Enabled() ? Clock::now() : Clock::time_point{};
}

// The trace's own log lines of this thread, written once the queue lock is released (the logger
// is synchronous with log.sync: writing them inside the hold would lengthen it).
void FlushLogs();

// Declared just BEFORE the lock guard: the wait's start (now, or `at`), and, being destroyed after
// the guard, writes the hold's log lines once the lock is released.
class Start {
public:
    Start() : t{Now()} {}
    explicit Start(Clock::time_point at) : t{Enabled() ? at : Clock::time_point{}} {}
    ~Start() {
        if (Enabled()) {
            FlushLogs();
        }
    }
    Start(const Start&) = delete;
    Start& operator=(const Start&) = delete;
    operator Clock::time_point() const {
        return t;
    }

private:
    Clock::time_point t;
};

struct PhaseRec {
    const char* name{};
    s64 ns{};
    u32 count{};
};

inline constexpr u32 MaxPhases = 10;

struct Rec {
    const char* site{};
    const char* thread{}; // the holder thread's name (kept for the process's lifetime)
    s64 start_ns{};       // lock taken (steady clock)
    s64 end_ns{};         // lock about to be released
    s64 wait_ns{};        // the holder's own wait for the lock (-1: not timed)
    std::array<PhaseRec, MaxPhases> phases{};
    u32 phase_count{};
};

// One hold of the queue lock. Constructed right after the lock is taken (`wait_start`: the clock
// before the lock call, from Now(); a zero time point = not timed) and declared AFTER the lock guard,
// so it is destroyed before the lock is released: every record is written with the lock held.
class Hold {
public:
    explicit Hold(const char* site, Clock::time_point wait_start = {});
    ~Hold();
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;

private:
    friend class Phase;
    bool on{false};
    Hold* prev{nullptr};
    Rec rec{};
};

// Times one piece of work inside the current thread's hold (nothing without one), summed by name.
class Phase {
public:
    explicit Phase(const char* name);
    ~Phase();
    Phase(const Phase&) = delete;
    Phase& operator=(const Phase&) = delete;
    // Counts items (deferred operations run, swapchain images...) under this phase.
    void Count(u32 n);
    // Adds `ns` under `name` to `hold` (Phase's destructor; the logger's write times).
    static void AddTo(Hold* hold, const char* name, s64 ns, u32 items = 1);

private:
    Hold* hold{nullptr};
    const char* name{};
    Clock::time_point start{};
    u32 count{};
    bool counted{false};
};

// The frame thread's window summary (empty while off). Resets the window.
std::string TakeSummary(double seconds);

// Frame thread: its waits of 2 ms or more since the last call, with what each waited behind (for
// the late display slot lines); clears them.
std::string TakeFrameWaitText();

} // namespace Vulkan::LockTrace
