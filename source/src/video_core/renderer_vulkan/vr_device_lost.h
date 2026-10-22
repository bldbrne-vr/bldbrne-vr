// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

// The GPU device lost, as the VR presenter sees it: VK_ERROR_DEVICE_LOST from any Vulkan call of
// the presenter, its overlay, late warp and VR page, or the XR queue's ordering (vr_xr_queue.h).
// On SteamVR (2026-10-15) a driver reset (TDR) went by unnoticed: the frame thread kept submitting
// the last layers and the log said nothing until the exit's "GPU idle 0.0 (ErrorDeviceLost)". One
// flag for all of them, logged once at critical level. Nothing exits on it (only the runtime's
// Quit or the window's close end the emulator): the frames go on, and with the base layer
// (vr_xr_layers_policy.h) the headset shows black, not the last images.

#include <atomic>
#include <string_view>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::VrDeviceLost {

inline std::atomic<bool> lost{false};

[[nodiscard]] inline bool Lost() {
    return lost.load(std::memory_order_relaxed);
}

// `result` of a Vulkan call (`where`: what it was): true when it says the device is lost (the
// first time also logged). Any other result: false, nothing done.
inline bool Note(vk::Result result, std::string_view where) {
    if (result != vk::Result::eErrorDeviceLost) {
        return false;
    }
    if (!lost.exchange(true, std::memory_order_relaxed)) {
        LOG_CRITICAL(Render_Vulkan,
                     "VR: the GPU device was lost ({}: VK_ERROR_DEVICE_LOST, a driver reset or "
                     "TDR). Nothing the GPU draws reaches the headset any more; the emulator keeps "
                     "running until it is closed (or Quit in the runtime)",
                     where);
    }
    return true;
}

} // namespace Vulkan::VrDeviceLost
