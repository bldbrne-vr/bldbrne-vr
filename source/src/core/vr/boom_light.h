// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace VR::BoomLight {
// Retail wp_a_1705_2 dummy 206 is on the lit furnace (part 2 => 12000).
// One stock local light supplies a modest native flicker; no extra flame mesh or custom
// lighting pass. A private non-repeating slot expires when no longer refreshed.
inline constexpr std::int32_t Dummy = 12206;
inline constexpr std::int32_t Ffx = 900116;
inline constexpr std::int32_t Slot = 30031;

constexpr bool Lit(std::int32_t weapon, std::int32_t style) {
    return weapon >= 0 && weapon / 100000 == 280 && style == 5;
}
} // namespace VR::BoomLight
