// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include "core/vr/melee_moves.h"

// Simon's Bowblade (variation 3200): two moves of our own (2026-11-12).
//  - The sword form's charged swing ("just hold trigger, play the DING! and then act as if it's a
//    sword with a charged attack"). The game's charged R2 in that form is the bow shot, so
//    melee_moves.h leaves it out and a full charge opened the heavy slash with no DING. Now the
//    heavy slash's own row (judge 150: its capsule, reach and hit) is also the charged move, so a
//    full charge DINGs and rings like any sword's, and its damage goes from the heavy's motion
//    value to ChargedMv. It is still the heavy's attack to the game, so no charged back stagger
//    (that comes with the game's charged rows, here the bow shot's): no visceral from behind.
//  - The bow form's arrow: the arrow in the string hand, before it is nocked, hits like a light
//    melee attack. The sword's light row (judge 0) with its hit moved onto the arrow for that
//    attack only (as the Kos Parasite's punch, core/vr/kos_punch.h), at ArrowDamagePct of a light
//    slash. Only with the physical bow (vr.ini bow) on: without it the bow form keeps the game's
//    R1 / R2 shots. OFF until the arrow's dummies are measured (ArrowMeasured, below).
namespace VR::Melee::Bowblade {

inline constexpr std::int32_t Variation = 3200;
inline constexpr std::int16_t ChargedMv = 170;      // the heavy's 130, the light's 100
inline constexpr std::int16_t ChargedStamina = 55;  // the heavy's 40, the light's 20
inline constexpr std::int16_t ArrowDamagePct = 60;  // of the light slash
inline constexpr float ArrowRadiusM = 0.10f;        // round the arrow's line
// The arrow's dummies (its part, WP_A_1709_2, on the string hand): its base and its tip. NOT
// MEASURED YET (2026-11-12: no game run could be made): 0 keeps the arrow move off, the bow form
// as before. Measure them with VR_SOLID_SHAPE_LOG=1 in the bow form (LogWeaponDummies: group 12
// if the arrow is part 2), set them, and the arrow tests run instead of skipping.
inline constexpr std::int32_t ArrowBaseDmy = 0;
inline constexpr std::int32_t ArrowTipDmy = 0;
inline constexpr bool ArrowMeasured = ArrowBaseDmy > 0 && ArrowTipDmy > 0;
// The sword's light row's hit, the one the arrow takes over (melee_moves.h, judge 0).
inline constexpr std::int32_t SwordLightDmy1 = 10100;
inline constexpr std::int32_t SwordLightDmy2 = 10110;

// `moves`: the generated table's. `arrow`: the arrow move too (the physical bow on, and the
// attack's hit can be moved: the retail layout checked).
inline WeaponMoves WithOwnMoves(WeaponMoves moves, bool arrow) {
    if (moves.variation != Variation) {
        return moves;
    }
    const MoveRow light = moves.rows[0];
    const MoveRow heavy = moves.rows[1];
    if (heavy.Valid() && !moves.rows[2].Valid()) {
        MoveRow charged = heavy;
        charged.charge = true;
        charged.mv = ChargedMv;
        charged.stamina = ChargedStamina;
        moves.rows[2] = charged;
    }
    if (arrow && ArrowMeasured && light.Valid() &&
        light.dmy1 == SwordLightDmy1 && light.dmy2 == SwordLightDmy2 && !moves.rows[3].Valid()) {
        MoveRow stab = light; // its judge, stamina, radius (the attack's check) and swing sound
        stab.dmy1 = ArrowBaseDmy;
        stab.dmy2 = ArrowTipDmy;
        stab.mv = static_cast<std::int16_t>(light.mv * ArrowDamagePct / 100);
        stab.trail_ffx = stab.trail_base = stab.trail_tip = -1; // no trail (SetSwingFx)
        stab.blade = 0.0f; // no blade fit: ArrowRadiusM
        stab.span = 0.0f;
        moves.rows[3] = stab;
    }
    return moves;
}

// The moves in use for `generated`'s weapon (FindMoves'): ours for the Bowblade (with the arrow),
// else the same table. For the presenter's top move (the DING, the combo budget), which the game
// update's moves decide: core/vr/bloodborne_melee.cpp ReadWeapon.
inline const WeaponMoves* InUse(const WeaponMoves* generated) {
    if (generated == nullptr || generated->variation != Variation) {
        return generated;
    }
    static const WeaponMoves own = WithOwnMoves(*generated, true);
    return &own;
}

// An arrow attack: the bow form's light row (only the arrow has one there).
constexpr bool IsArrowAttack(std::int32_t variation, bool form_b, std::int32_t judge) {
    return variation == Variation && form_b && judge == 0;
}

// The damage scale of a row of ours against the game's row it opens: its motion value over the
// generated row's of the same judge in the sword form (the charged: 170 / 130; the arrow: 60 / 100).
// 1 for every other weapon and row.
inline float DamageScale(std::int32_t variation, const MoveRow& row, const WeaponMoves* table) {
    if (variation != Variation || table == nullptr || row.mv <= 0) {
        return 1.0f;
    }
    for (std::size_t k = 0; k < 3; ++k) {
        const MoveRow& game = table->rows[k];
        if (game.Valid() && game.judge == row.judge && game.mv > 0) {
            return static_cast<float>(row.mv) / static_cast<float>(game.mv);
        }
    }
    return 1.0f;
}

// Moves an arrow attack's hit onto the arrow: the attack instance's two dummies (the sword's light
// row's, 10100 -> 10110) become the arrow's. Same per-instance layout as RelocateKosPunch; the
// caller checks the retail signatures. Never the shared game params.
inline bool RelocateArrow(std::span<std::uint8_t> entry) {
    if (entry.size() < 0x190 || !ArrowMeasured) {
        return false;
    }
    std::int32_t bid{}, first{}, second{};
    std::memcpy(&bid, entry.data() + 0x8C, sizeof(bid));
    std::memcpy(&first, entry.data() + 0x188, sizeof(first));
    std::memcpy(&second, entry.data() + 0x18C, sizeof(second));
    const std::int32_t light_bid = 100000000 + Variation * 1000;
    if (bid != light_bid || first != SwordLightDmy1 || second != SwordLightDmy2) {
        return false;
    }
    std::memcpy(entry.data() + 0x188, &ArrowBaseDmy, sizeof(ArrowBaseDmy));
    std::memcpy(entry.data() + 0x18C, &ArrowTipDmy, sizeof(ArrowTipDmy));
    return true;
}

} // namespace VR::Melee::Bowblade
