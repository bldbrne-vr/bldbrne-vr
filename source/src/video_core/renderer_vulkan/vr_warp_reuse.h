// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace Vulkan::VrWarpReuse {

// Metadata belongs to the last released warp, including the tracking space of its eye poses.
// A reused image keeps that metadata even when a newer capture or head sample is available.
struct Frame {
    std::uint64_t frame{};
    std::uint32_t space{};
    bool valid{};

    bool CanReuse(std::uint64_t now, std::uint32_t current_space,
                  std::uint64_t max_age) const {
        return valid && space == current_space && now >= frame && now - frame <= max_age;
    }
};

} // namespace Vulkan::VrWarpReuse
