// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>

namespace VR::Tricks {
// Retail 1.09: a116_042630 invokes behavior 101100930 / attack 1100930.
// Its sphere uses body dummy 60. The per-instance attack spec is copied to
// entry+0x54 by 0x185D988; spec+0x134/+0x138 are its dummy ids (0x1F1439D).
// The caller verifies those instruction signatures before using this layout.
inline bool RelocateStakeBlast(std::span<std::uint8_t> entry) {
    if (entry.size() < 0x190) return false;
    std::int32_t behavior{}, first{}, second{};
    float radius{};
    std::memcpy(&behavior, entry.data() + 0x8C, sizeof(behavior));
    std::memcpy(&radius, entry.data() + 0xA0, sizeof(radius));
    std::memcpy(&first, entry.data() + 0x188, sizeof(first));
    std::memcpy(&second, entry.data() + 0x18C, sizeof(second));
    if (behavior != 101100930 || (first != 60 && first != 11206) || second != -1 ||
        !std::isfinite(radius) || std::abs(radius - 1.0f) > 0.02f) return false;
    const std::int32_t tip = 11206;
    std::memcpy(entry.data() + 0x188, &tip, sizeof(tip));
    return true;
}
} // namespace VR::Tricks
