// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

// Stutter hunt (2026-10-18, logs-on builds only; nothing in a BBVR_NO_LOGS build): the emulator
// calls on the asset-loading path are timed. A single call past its threshold is logged with its
// thread and size, and a game frame longer than 30 ms logs what each kind took in that frame
// (the sewers' load stutters: 50-114 ms frames as map pieces and enemies stream in, with no
// pipeline compile or wait reported).

#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include "common/logging/log.h"
#include "common/thread.h"
#include "common/types.h"

namespace Common::Slow {

enum Kind : u32 {
    FileRead,
    MapMemory,
    UnmapMemory,
    Protect,
    PoolCommit,
    GuestInvalidate,
    CreateBuffer,
    RefreshImage,
    BufferAlloc,
    StereoTwin,
    ImageAlloc,
    Count
};
inline constexpr std::array<const char*, Count> Names{
    "file read", "map", "unmap", "protect", "pool commit", "guest invalidate", "create buffer",
    "texture upload", "buffer alloc", "stereo twin", "image alloc"};

/// Logs-on builds: each tracked thread group's CPU time since the previous call (GetThreadTimes),
/// as " | label ms" parts when `report` (slow_scope.cpp; empty elsewhere).
/// Called by the publishing thread outside rendering locks. Thread discovery is asynchronous.
std::string ThreadTimes(bool report);
/// The stall sampler (VR_STALL_SAMPLER=1, logs-on builds; slow_scope.cpp): StartSampler starts it
/// once; RequestSamples writes the samples of the last `ms` to stall_samples.txt.
void StartSampler();
void RequestSamples(double ms);

#ifdef BBVR_NO_LOGS
class Scope {
public:
    Scope(Kind, u64 = 0, u64 = 0) {}
};
inline void FrameBoundary(double) {}
#else
struct Totals {
    std::array<std::atomic<u64>, Count> ns{};
    std::array<std::atomic<u64>, Count> calls{};
    std::array<std::atomic<u64>, Count> bytes{};
};
inline Totals g_totals;

inline s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Times the enclosing call; past `log_us` it is logged on its own (at most 200 such lines).
class Scope {
public:
    explicit Scope(Kind kind, u64 bytes = 0, u64 log_us = 4000)
        : kind{kind}, bytes{bytes}, log_us{log_us}, start{NowNs()} {}
    ~Scope() {
        const u64 ns = static_cast<u64>(NowNs() - start);
        g_totals.ns[kind].fetch_add(ns, std::memory_order_relaxed);
        g_totals.calls[kind].fetch_add(1, std::memory_order_relaxed);
        g_totals.bytes[kind].fetch_add(bytes, std::memory_order_relaxed);
        if (ns >= log_us * 1000) {
            static std::atomic<u32> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 200) {
                LOG_WARNING(Debug, "SLOW {}: {:.2f} ms, {} KB, thread {} ({})", Names[kind],
                            static_cast<double>(ns) * 1e-6, bytes >> 10, CurrentThreadId(),
                            GetCurrentThreadName());
            }
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Kind kind;
    u64 bytes;
    u64 log_us;
    s64 start;
};

/// The presenter, at each game frame it gets (`ms`: the interval since the previous one): a frame
/// over 30 ms logs what each kind took since the previous frame (at most 300 lines).
inline void FrameBoundary(double ms) {
    static std::array<u64, Count> last_ns{}, last_calls{}, last_bytes{};
    std::array<u64, Count> ns{}, calls{}, bytes{};
    for (u32 k = 0; k < Count; ++k) {
        ns[k] = g_totals.ns[k].load(std::memory_order_relaxed);
        calls[k] = g_totals.calls[k].load(std::memory_order_relaxed);
        bytes[k] = g_totals.bytes[k].load(std::memory_order_relaxed);
    }
    StartSampler();
    static u32 logged = 0;
    const bool report = ms > 30.0 && logged < 300;
    if (report) {
        RequestSamples(ms);
    }
    const std::string threads = ThreadTimes(report); // sampled every frame: per-frame deltas
    if (report) {
        ++logged;
        std::string parts;
        for (u32 k = 0; k < Count; ++k) {
            const u64 n = calls[k] - last_calls[k];
            if (n == 0) {
                continue;
            }
            parts += fmt::format(" | {} {:.1f} ms x{} {} KB", Names[k],
                                 static_cast<double>(ns[k] - last_ns[k]) * 1e-6, n,
                                 (bytes[k] - last_bytes[k]) >> 10);
        }
        LOG_WARNING(Debug, "SLOW FRAME {:.1f} ms (summed across threads):{} || thread CPU ms:{}", ms,
                    parts.empty() ? std::string{" nothing timed"} : parts,
                    threads.empty() ? std::string{" (not sampled yet)"} : threads);
    }
    last_ns = ns;
    last_calls = calls;
    last_bytes = bytes;
}
#endif

} // namespace Common::Slow
