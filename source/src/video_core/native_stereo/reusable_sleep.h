// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <cstdlib>
#include <windows.h>
namespace ScratchSleep {
inline bool Enabled() {
    static const bool on=[] { const char* v=std::getenv("VR_REUSE_SLEEP_TIMER"); return !v || v[0]!='0'; }();
    return on;
}
inline HANDLE Create(bool* high_resolution=nullptr) {
    HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,
        CREATE_WAITABLE_TIMER_MANUAL_RESET|CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_MODIFY_STATE|SYNCHRONIZE);
    if(high_resolution) *high_resolution=timer!=nullptr;
    if(!timer) timer=CreateWaitableTimerW(nullptr,TRUE,nullptr);
    return timer;
}
struct CachedTimer {
    bool high_resolution{};
    HANDLE timer=Create(&high_resolution);
    bool active{};
    ~CachedTimer() { if(timer) CloseHandle(timer); }
};
inline CachedTimer& Cache() { static thread_local CachedTimer cache; return cache; }
struct Lease {
    CachedTimer& cache=Cache();
    bool borrowed=!cache.active;
    HANDLE timer{};
    Lease() {
        if(borrowed) {cache.active=true;timer=cache.timer;}
        else timer=Create(); // Nested alertable APC sleep must not reset the outer timer.
    }
    ~Lease() {
        if(timer) CancelWaitableTimer(timer);
        if(borrowed) cache.active=false;
        else if(timer) CloseHandle(timer);
    }
    Lease(const Lease&)=delete;
    Lease& operator=(const Lease&)=delete;
};
inline DWORD Wait(LARGE_INTEGER interval,bool interruptible) {
    Lease lease;
    if(!lease.timer || !SetWaitableTimer(lease.timer,&interval,0,nullptr,nullptr,FALSE)) return WAIT_FAILED;
    return WaitForSingleObjectEx(lease.timer,INFINITE,interruptible);
}
}
