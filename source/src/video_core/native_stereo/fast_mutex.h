// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
// Experimental Windows mutex primitive. Guest recursion/ownership stays in PthreadMutex.
#include <atomic>
#include <chrono>
#include <limits>
#include <system_error>
#include <windows.h>

namespace ScratchMutex {
class FastMutex {
    // 0 free, 1 held without known waiters, 2 held with possible waiters.
    std::atomic<unsigned> state{0};
    HANDLE event;
public:
    FastMutex() : event(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
        if (!event) throw std::system_error(GetLastError(), std::system_category());
    }
    ~FastMutex() { CloseHandle(event); }
    FastMutex(const FastMutex&) = delete;
    FastMutex& operator=(const FastMutex&) = delete;
    bool try_lock() {
        unsigned expected=0;
        return state.compare_exchange_strong(expected,1,std::memory_order_acquire,
                                             std::memory_order_relaxed);
    }
    void lock() {
        if (try_lock()) return;
        for (;;) {
            if (state.exchange(2,std::memory_order_acquire)==0) return;
            const auto result=WaitForSingleObjectEx(event,INFINITE,TRUE);
            if (result==WAIT_FAILED)
                throw std::system_error(GetLastError(),std::system_category());
            // APCs and stale event signals both retry the atomic ownership test.
        }
    }
    void unlock() {
        if (state.exchange(0,std::memory_order_release)==2) SetEvent(event);
    }
    template<class Clock,class Duration>
    bool try_lock_until(const std::chrono::time_point<Clock,Duration>& deadline) {
        for (;;) {
            const auto now=Clock::now();
            // Match the existing Windows path: an already expired deadline fails.
            if (deadline<=now) {
                // A timed waiter may have consumed the wake just as its deadline expired.
                // Pass the wake on so another sleeper can acquire an otherwise free mutex.
                SetEvent(event);
                return false;
            }
            if (state.exchange(2,std::memory_order_acquire)==0) return true;
            const auto ms=std::chrono::ceil<std::chrono::milliseconds>(deadline-now).count();
            const auto max_wait=static_cast<decltype(ms)>(INFINITE-1);
            const DWORD wait=static_cast<DWORD>(ms>max_wait ? max_wait : ms);
            const auto result=WaitForSingleObjectEx(event,wait,TRUE);
            if (result==WAIT_FAILED)
                throw std::system_error(GetLastError(),std::system_category());
        }
    }
};
}
