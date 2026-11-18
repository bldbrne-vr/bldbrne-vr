// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include "common/spin_lock.h"

#if _MSC_VER
#include <intrin.h>
#if _M_AMD64
#define __x86_64__ 1
#endif
#if _M_ARM64
#define __aarch64__ 1
#endif
#else
#if __x86_64__
#include <xmmintrin.h>
#endif
#endif

namespace {

void ThreadPause() {
#if __x86_64__
    _mm_pause();
#elif __aarch64__ && _MSC_VER
    __yield();
#elif __aarch64__
    asm("yield");
#endif
}

// FAST_SPIN_PARK=0: the old lock, a waiter spins until the holder lets go. With fewer host
// threads than busy guest and emulator threads (4 threads: 2026-10-29) the write-fault handler's
// region locks left five game workers spinning on a descheduled holder for whole time slices.
bool Parks() {
    static const bool on = [] {
        const char* value = std::getenv("FAST_SPIN_PARK");
        return !(value && value[0] == '0');
    }();
    return on;
}

// About 2-5 us of pauses: longer than the short holds, shorter than a time slice.
constexpr int SpinLimit = 128;

} // Anonymous namespace

namespace Common {

void SpinLock::lock() {
    std::uint32_t expected = 0;
    if (state.compare_exchange_strong(expected, 1, std::memory_order_acquire)) [[likely]] {
        return;
    }
    LockSlow();
}

void SpinLock::LockSlow() {
    if (!Parks()) {
        while (state.exchange(1, std::memory_order_acquire) != 0) {
            ThreadPause();
        }
        return;
    }
    std::uint32_t seen = 1;
    for (int i = 0; i < SpinLimit; ++i) {
        ThreadPause();
        seen = state.load(std::memory_order_relaxed);
        if (seen == 0 &&
            state.compare_exchange_weak(seen, 1, std::memory_order_acquire,
                                        std::memory_order_relaxed)) {
            return;
        }
    }
    // Mark the lock contended and sleep until an unlock wakes a waiter. A waiter that takes it
    // keeps the mark (2): another may still be parked, and its unlock must wake that one.
    if (seen != 2) {
        seen = state.exchange(2, std::memory_order_acquire);
    }
    while (seen != 0) {
        state.wait(2, std::memory_order_relaxed);
        seen = state.exchange(2, std::memory_order_acquire);
    }
}

void SpinLock::unlock() {
    if (state.exchange(0, std::memory_order_release) == 2) [[unlikely]] {
        state.notify_one();
    }
}

bool SpinLock::try_lock() {
    std::uint32_t expected = 0;
    return state.compare_exchange_strong(expected, 1, std::memory_order_acquire);
}

} // namespace Common
