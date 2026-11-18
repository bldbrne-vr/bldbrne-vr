// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>

namespace Vulkan {
// Called only for the active scene depth before reconstruction. Direct small draws keep
// the existing fullscreen/sprite exclusion. Indirect counts live in GPU argument buffers;
// the CPU registers do not describe them (bbport's indirect path has no count filter).
inline std::array<float, 2> TemporalDrawJitter(bool indirect, uint32_t count, uint32_t instances,
                                             std::array<float, 2> jitter) {
    return indirect || count > 6 || instances > 1 ? jitter : std::array<float, 2>{};
}
} // namespace Vulkan
