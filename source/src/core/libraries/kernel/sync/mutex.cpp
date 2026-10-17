// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "mutex.h"
#include <cstdlib>
#include <cstring>

#include "common/assert.h"

namespace Libraries::Kernel {

TimedMutex::TimedMutex() {
#ifdef _WIN64
    const char* enabled = std::getenv("VR_FAST_MUTEX");
    if (!enabled || enabled[0] != '0') {
        fast = std::make_unique<ScratchMutex::FastMutex>();
        return;
    }
    mtx = CreateMutex(nullptr, false, nullptr);
    ASSERT(mtx);
#endif
}

TimedMutex::~TimedMutex() {
#ifdef _WIN64
    if (mtx) CloseHandle(mtx);
#endif
}

void TimedMutex::lock() {
#ifdef _WIN64
    if (fast) { fast->lock(); return; }
    for (;;) {
        u64 res = WaitForSingleObjectEx(mtx, INFINITE, true);
        if (res == WAIT_OBJECT_0) {
            return;
        }
    }
#else
    mtx.lock();
#endif
}

bool TimedMutex::try_lock() {
#ifdef _WIN64
    if (fast) return fast->try_lock();
    return WaitForSingleObjectEx(mtx, 0, true) == WAIT_OBJECT_0;
#else
    return mtx.try_lock();
#endif
}

void TimedMutex::unlock() {
#ifdef _WIN64
    if (fast) { fast->unlock(); return; }
    ReleaseMutex(mtx);
#else
    mtx.unlock();
#endif
}

} // namespace Libraries::Kernel