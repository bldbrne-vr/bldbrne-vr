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
#include <cstdlib>
#include "common/types.h"
#include "common/logging/log.h"
namespace ScratchPacing {
inline bool Enabled() { static const bool on=std::getenv("VR_PACING_AUDIT")!=nullptr; return on; }
inline void Sleep(u32 usec, const void* caller) {
    struct Entry { const void* caller{}; u32 minimum{}, maximum{}; u64 calls{}, total_usec{}; };
    struct Stats {
        std::array<Entry,16> entries{};
        u64 calls{}, overflow{};
        std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();
    };
    static thread_local Stats stats;
    ++stats.calls;
    bool stored=false;
    for (auto& e:stats.entries) {
        if (!e.calls || e.caller==caller) {
            if (!e.calls) LOG_INFO(Kernel_Vmm,"VR pacing sleep first: caller={} requested={}us",caller,usec);
            e.minimum=e.calls ? (usec<e.minimum ? usec : e.minimum) : usec;
            e.maximum=usec>e.maximum ? usec : e.maximum;
            e.caller=caller; e.total_usec+=usec; ++e.calls; stored=true; break;
        }
    }
    if (!stored) ++stats.overflow;
    if (stats.calls%300==0) {
        const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-stats.start).count();
        for (const auto& e:stats.entries) if(e.calls)
            LOG_INFO(Kernel_Vmm,"VR pacing sleep totals: elapsed={:.3f}s caller={} requested_min={}us max={}us total={}us calls={} overflow={}",elapsed,e.caller,e.minimum,e.maximum,e.total_usec,e.calls,stats.overflow);
    }
}
// Counts rapid gettimeofday callers using the timestamp already fetched by the API.
inline void ClockPoll(const void* caller, u64 ticks) {
    struct Entry { const void* caller{}; u64 count{}; };
    static thread_local std::array<Entry,16> entries{};
    static thread_local u64 began{};
    if (!began) began=ticks;
    for (auto& entry:entries) {
        if (!entry.count || entry.caller==caller) {
            entry.caller=caller; ++entry.count; break;
        }
    }
    if (ticks-began>=50000000) {
        for(const auto& entry:entries) if(entry.count)
            LOG_INFO(Kernel_Vmm,"VR clock polling: caller={} count={} over {:.3f}s",entry.caller,entry.count,double(ticks-began)/10000000.0);
        entries={}; began=ticks;
    }
}
struct WaitStats {
    std::array<double,7> milliseconds{};
    std::array<u64,7> calls{};
    std::chrono::steady_clock::time_point last=std::chrono::steady_clock::now();
};
inline WaitStats& WaitData() { static thread_local WaitStats data; return data; }
struct Wait {
    unsigned kind;
    bool on;
    WaitStats* stats{};
    std::chrono::steady_clock::time_point start;
    explicit Wait(unsigned k):kind(k),on(Enabled()) {
        if(on) { stats=&WaitData(); start=std::chrono::steady_clock::now(); }
    }
    ~Wait() {
        if(!on) return;
        auto& waits=*stats;
        const auto now=std::chrono::steady_clock::now();
        waits.milliseconds[kind]+=std::chrono::duration<double,std::milli>(now-start).count();
        ++waits.calls[kind];
        const double elapsed=std::chrono::duration<double>(now-waits.last).count();
        if(elapsed>=5) {
            constexpr const char* names[]={"Vblank","Equeue","EventFlag","Semaphore","GnmSubmission","Condvar","Mutex"};
            for(unsigned i=0;i<7;++i) if(waits.calls[i])
                LOG_INFO(Kernel_Vmm,"VR pacing wait: {:.3f}s {} {:.3f}ms / {} calls (wall time, not CPU service)",elapsed,names[i],waits.milliseconds[i],waits.calls[i]);
            waits.milliseconds={}; waits.calls={}; waits.last=now;
        }
    }
};

}
