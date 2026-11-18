// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace Vulkan {

// The NVIDIA driver profile of this executable (vr_nvidia_profile.cpp, after bbport's
// platform/win32/nvidia_profile.cpp). Called once before the Vulkan driver loads. A no-op without
// an NVIDIA driver and off Windows.
// vr.ini nvidia_max_power=1 (default): "Power management mode: Prefer maximum performance" unless
// the player chose a mode for the executable. =0: the mode this set is taken back out.
// VR_NV_POWER=0/1 overrides vr.ini; VR_NV_POWER=check only logs what it would do.
void ApplyNvidiaPowerProfile();

} // namespace Vulkan
