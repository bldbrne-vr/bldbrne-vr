// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <vulkan/vulkan_core.h>

namespace Vulkan {
// A timeout is retryable. Device loss and other errors are terminal; the caller
// must handle them without publishing completion or reusing GPU resources.
template <typename Wait>
VkResult WaitForTimelineResult(Wait&& wait) {
    VkResult result;
    do {
        result = wait();
    } while (result == VK_TIMEOUT);
    return result;
}
} // namespace Vulkan
