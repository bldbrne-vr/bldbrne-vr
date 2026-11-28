// SPDX-FileCopyrightText: 2013 Dolphin Emulator Project
// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include "common/types.h"

namespace Common {

enum class ThreadPriority : u32 {
    Low = 0,
    Normal = 1,
    High = 2,
    VeryHigh = 3,
    Critical = 4,
};

void SetCurrentThreadRealtime(std::chrono::nanoseconds period_ns);

void SetCurrentThreadPriority(ThreadPriority new_priority);

/// Windows: asks that this process never be power-throttled (EcoQoS). Windows 11 throttles a
/// process whose window is not in the foreground, which in VR is always the case, and on hybrid
/// Intel CPUs (12th generation and later) that moves its threads to the efficiency cores. Also
/// keeps its timer resolution requests. Elsewhere, and on Windows versions without it, nothing.
/// Returns what Windows accepted: 2 speed and timers, 1 speed only, 0 nothing.
int DisableProcessPowerThrottling();

/// Windows: on CPUs with cores of different speed (Intel 12th generation and later), asks that the
/// calling thread run on the fastest cores. A preference, not a pin: Windows still uses the other
/// cores when the fast ones are busy. Where every core is the same (AMD, older Intel) nothing
/// changes. Returns how many cores the thread now prefers, 0 when nothing changed.
int PreferPerformanceCores();

void SetCurrentThreadName(const char* name);

void SetThreadName(void* thread, const char* name);

bool AccurateSleep(std::chrono::nanoseconds duration, std::chrono::nanoseconds* remaining,
                   bool interruptible);

/// Sleeps until `deadline` with sub-millisecond accuracy whatever the process's timer resolution:
/// on Windows a high-resolution waitable timer (one per thread) to `spin` before it, then a spin;
/// elsewhere sleep_until. (2026-11-21: a plain sleep here can overshoot by a 15.6 ms timer tick.)
void PreciseSleepUntil(std::chrono::steady_clock::time_point deadline,
                       std::chrono::nanoseconds spin = std::chrono::microseconds(300));

class AccurateTimer {
    std::chrono::nanoseconds target_interval{};
    std::chrono::nanoseconds total_wait{};

    std::chrono::high_resolution_clock::time_point start_time;

public:
    explicit AccurateTimer(std::chrono::nanoseconds target_interval);

    void Start();

    void End();

    std::chrono::nanoseconds GetTotalWait() const {
        return total_wait;
    }
};

std::string GetCurrentThreadName();

/// The OS thread id (a freeze hunt matches it to a stack dump of the process); 0 where unknown.
u32 CurrentThreadId();

} // namespace Common
