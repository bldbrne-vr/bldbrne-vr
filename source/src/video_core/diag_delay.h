// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <immintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

// FAST_DIAG_DELAY_NS=<a>,<b>,<r> (diagnostic only; read once at start-up): a busy wait of that
// many nanoseconds after each draw-pipe packet stage A pushes (a), after each packet stage B
// handles (b), and after each command packet the recorder replays (r). A causal profile: the
// thread whose added time costs the most frame rate is the one on the critical path. Unset,
// every call is one load and a branch.
namespace FastRender::DiagDelay {

enum Stage : unsigned { A, B, R };

struct Config {
    std::array<std::uint64_t, 3> ticks{};
    bool any{};
};

inline Config Read() {
    Config config;
    const char* value = std::getenv("FAST_DIAG_DELAY_NS");
    if (!value || !*value) {
        return config;
    }
    // TSC ticks per nanosecond, measured over 20 ms.
    const auto start = std::chrono::steady_clock::now();
    const std::uint64_t tsc_start = __rdtsc();
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(20)) {
        _mm_pause();
    }
    const std::uint64_t tsc_end = __rdtsc();
    const double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count());
    const double per_ns = double(tsc_end - tsc_start) / ns;
    char* end = const_cast<char*>(value);
    for (unsigned stage = 0; stage < 3 && *end; ++stage) {
        const char* at = stage == 0 ? end : end + 1;
        const double delay = std::strtod(at, &end);
        if (end == at) {
            break;
        }
        config.ticks[stage] = delay > 0 ? std::uint64_t(delay * per_ns) : 0;
        config.any |= config.ticks[stage] != 0;
        if (*end != ',') {
            break;
        }
    }
    return config;
}

inline const Config g_config = Read();

inline void At(Stage stage) {
    if (!g_config.any) [[likely]] {
        return;
    }
    const std::uint64_t ticks = g_config.ticks[stage];
    if (!ticks) {
        return;
    }
    const std::uint64_t until = __rdtsc() + ticks;
    while (__rdtsc() < until) {
        _mm_pause();
    }
}

} // namespace FastRender::DiagDelay
