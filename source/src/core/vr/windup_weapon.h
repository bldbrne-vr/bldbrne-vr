// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

#include "core/vr/whip_moves.h"
#include "core/vr/whip_spin.h"

// Which wind-up rule (core/vr/swing_detector.h WindupFor) the weapon in each hand takes (the user,
// 2026-11-11: softer for everyone, softer still for the Amygdalan Arm's lash, none for the whips).
// Header-only and pure: tests/test_swing_arc.cpp.
namespace VR::Melee {

// The Kos Parasite's tendrils (the Milkweed rune's weapon; core/vr/trick_weapons.h
// KosRuneVariation, checked equal where both are in scope) and the Amygdalan Arm.
inline constexpr std::int32_t KosTendrilVariation = 3800;
inline constexpr std::int32_t AmygdalanArmVariation = 2500;

struct WindupWeapon {
    bool whip{false};     // no wind-up rule
    bool arm_lash{false}; // the Amygdalan Arm's lash: softer still
};

// The right hand: on the physical melee route only (`physical_melee`; on the R1 / R2 route a
// stroke's verdict presses the button, and a wind-up must not). Whips: the Threaded Cane's and the
// Beast Cutter's whip forms with physical whips (`physical_whips`), not the Cutter held compact by
// its trigger (a club then), and the Kos Parasite's tendrils in either form. The Arm: its form B
// with physical whips.
inline WindupWeapon RightWindupWeapon(std::int32_t variation, bool form_b, bool physical_melee,
                                      bool physical_whips, bool trigger) {
    if (!physical_melee) {
        return {};
    }
    const bool whip_form = physical_whips && form_b && VR::Whip::FindWhip(variation) != nullptr &&
                           !VR::Whip::CompactCutter(variation, form_b, physical_whips, trigger);
    return {whip_form || variation == KosTendrilVariation,
            physical_whips && form_b && variation == AmygdalanArmVariation};
}

// The left hand: a whip only with the Kos Parasite's form-B tendrils on it (no gun there); a gun or
// an off-hand blade has the general rule, whatever the right hand holds.
constexpr bool LeftWindupWhip(std::int32_t variation, bool form_b, bool left_gun) {
    return variation == KosTendrilVariation && form_b && !left_gun;
}

} // namespace VR::Melee
