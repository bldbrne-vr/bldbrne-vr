// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <chrono>
#include <cstdlib>
#include "common/logging/log.h"
#include "common/thread.h"
#include "core/vr/vr_session.h"

namespace Vulkan::CompileTrace {
using Clock = std::chrono::steady_clock;
inline bool Enabled() {
    static const bool enabled = [] {
        const char* text = std::getenv("FAST_COMPILE_TRACE");
        return text && text[0] == '1';
    }();
    return enabled;
}
struct FrameState {
    Clock::time_point previous{};
    u64 count{};
    double stall_ms{};
};
inline thread_local FrameState frame;
class Scope {
public:
    Scope(const char* kind_, u64 hash_) : kind{kind_}, hash{hash_}, active{Enabled()} {
        if (active) {
            gameplay = VR::GameplaySeconds();
            start = Clock::now();
        }
    }
    ~Scope() {
        if (!active)
            return;
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        LOG_INFO(Render_Vulkan,
                 "FAST compile: kind={} hash={:#x} thread={} gameplay_s={:.3f} ms={:.3f}", kind,
                 hash, Common::GetCurrentThreadName(), gameplay, ms);
        if (gameplay >= 20.0) {
            ++frame.count;
            frame.stall_ms += ms;
        }
    }

private:
    const char* kind;
    u64 hash;
    bool active;
    double gameplay{};
    Clock::time_point start{};
};
inline void EndFrame() {
    if (!Enabled())
        return;
    const auto now = Clock::now();
    if (frame.count) {
        const double interval =
            frame.previous == Clock::time_point{}
                ? -1.0
                : std::chrono::duration<double, std::milli>(now - frame.previous).count();
        const float hz = VR::GetSettings().refresh_hz;
        const double deadline = hz > 0 ? 1000.0 / hz : 0.0;
        LOG_INFO(Render_Vulkan,
                 "FAST compile frame: thread={} gameplay_s={:.3f} count={} stall_ms={:.3f} "
                 "completion_ms={:.3f} deadline_ms={:.3f} missed={}",
                 Common::GetCurrentThreadName(), VR::GameplaySeconds(), frame.count, frame.stall_ms,
                 interval, deadline, interval >= 0 && deadline > 0 && interval > deadline);
    }
    frame.previous = now;
    frame.count = 0;
    frame.stall_ms = 0;
}
} // namespace Vulkan::CompileTrace
