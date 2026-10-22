// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include "video_core/native_stereo/profile_scopes.h"

namespace Vulkan {
// Diagnostic only: CPU scopes require VR_CPU_PROFILE (optionally its mask);
// GPU scopes require FAST_GPU_PASS_PROFILE and fast bit 38. Neither waits for
// query results. GPU segments include intervening barriers, not just shaders.
namespace TemporalProfile {
constexpr int SlotWait = 47;
constexpr int CommandBorrow = 48;
constexpr int Provider = 49;
constexpr int Prepare = 50;
constexpr int Motion = 51;

class Scope {
public:
    Scope(Scheduler& scheduler, int id, const char* label)
        : cpu(id, label), gpu(scheduler, 0x7E3B'1000'0000ull + id,
                              GpuPassProfiler::Group::Temporal, label) {}
private:
    ScratchProfile::Cpu cpu;
    GpuPassProfiler::Scope gpu;
};
} // namespace TemporalProfile
} // namespace Vulkan
