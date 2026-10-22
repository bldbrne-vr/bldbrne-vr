// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "core/vr/melee_moves.h"

// The whip forms' moves for the physical whip (vr.ini whip=physical; core/vr/whip_chain.h,
// core/vr/bloodborne_whip.h). core/vr/melee_moves.h leaves the whips' form B out (swung without the
// game's attack clip their chain is a rigid stick); with the physical whip the chain is simulated,
// so a swing opens these rows on it.
//
// GENERATED offline (by a tool not included here) from the 1.09 params and TAEs; do not edit by hand.
// Per whip (behaviour variation): form B's light / heavy / charged row as the game's own clips use
// them (the first R1 040000, the R2 041000, the charged R2 041100: each clip's InvokeAttackBehavior
// judge), with the AtkParam's hit 0 (the outer capsule, tip 12100 to mid 12130; the row's hits 1-3
// run on down the chain to the handle, and InvokeAttack makes them all), stamina, motion value, hit
// stop, the clip's whoosh and trail (-1: the defaults), and `reach`: how far the clip's tip dummy
// (12100) gets from the grip during that hit window (the links stretched; the physical whip pays
// out to it), `idle_tip` where it is in the idle clip a999 (offline analysis).
// `recover`: the game's recovery of each move (its TAE's right-hand attack cancel, ChrActionFlag 4,
// in the same clips: the combo budget's pace, as core/vr/melee_moves.h Paces for the blades;
// offline analysis). `recover` and `idle_tip` were added by hand (2026-10-06).
// `tested`: swung on the arena's beast in the mock (the others need VR_WHIP_UNTESTED).
namespace VR::Whip {

struct WhipMoves {
    std::int32_t variation;
    std::string_view name;
    std::array<VR::Melee::MoveRow, 3> rows; // form B: light, heavy, charged
    std::array<float, 3> reach;             // m from the grip, the clip's tip in its hit window
    std::array<float, 3> recover;           // s: the move's own recovery (its R1 / R2 cancel)
    float idle_tip;                         // m: the tip dummy 12100 from the grip in the idle clip
    bool tested;
};

// clang-format off
inline constexpr std::array<WhipMoves, 2> Whips = {{
    {2400, "Beast Cutter", {{{400, 30, 95, 12100, 12130, 0.350f, 0.090f, false, 896, -1, -1, -1, 0.000f, 0.000f}, {500, 50, 130, 12100, 12130, 0.150f, 0.120f, false, 890, -1, -1, -1, 0.000f, 0.000f}, {510, 65, 150, 12100, 12130, 0.150f, 0.120f, false, 890, -1, -1, -1, 0.000f, 0.000f}}}, {{3.70f, 5.46f, 6.18f}}, {{1.233f, 2.067f, 2.100f}}, 1.171f, true},
    {2200, "Threaded Cane", {{{400, 17, 95, 12100, 12130, 0.150f, 0.090f, false, 770, 250, 12100, 12130, 0.000f, 0.000f}, {500, 32, 125, 12100, 12130, 0.150f, 0.090f, false, -1, 250, 12100, 12130, 0.000f, 0.000f}, {510, 32, 125, 12100, 12130, 0.150f, 0.090f, false, 5001, 250, 12100, 12130, 0.000f, 0.000f}}}, {{3.74f, 2.88f, 3.26f}}, {{0.767f, 1.333f, 1.267f}}, 0.892f, true},
}};
// clang-format on

inline const WhipMoves* FindWhip(std::int32_t variation) {
    for (const WhipMoves& w : Whips) {
        if (w.variation == variation) {
            return &w;
        }
    }
    return nullptr;
}

// The weapon's six rows with its whip form's three: form A as `base`, form B the whip's. A row
// whose clip plays no whoosh of its own (the Threaded Cane's R2) takes the light row's: the whip's
// own sound, not the generic blade's.
inline VR::Melee::WeaponMoves WithWhipRows(const VR::Melee::WeaponMoves& base, const WhipMoves& w) {
    VR::Melee::WeaponMoves out = base;
    for (std::size_t k = 0; k < 3; ++k) {
        out.rows[3 + k] = w.rows[k];
        if (out.rows[3 + k].whoosh < 0) {
            out.rows[3 + k].whoosh = w.rows[0].whoosh;
        }
    }
    return out;
}

// The whip's recovery of a move (s; 0: not a whip, or no such move).
inline float WhipRecover(std::int32_t variation, std::size_t kind) {
    const WhipMoves* const w = FindWhip(variation);
    return w != nullptr && kind < 3 ? w->recover[kind] : 0.0f;
}

// The recovery the whip damage pace (whip_damage.h PaceMeter) holds a move to: the game's, the
// Beast Cutter's 7% shorter (2026-10-15: its charges come back a little sooner, gently; its damage
// per second may rise as much, no more).
inline float WhipPaceRecover(std::int32_t variation, std::size_t kind) {
    return WhipRecover(variation, kind) * (variation == 2400 ? 0.93f : 1.0f);
}

// The flick into the whip form's transform attack (the game's trick attack A->B, judge 390): its
// capsules hang on the chain too (12100 .. 12120), so it rides the simulated chain and lashes.
inline const VR::Melee::MoveRow* WhipTransformRow(std::int32_t variation) {
    return FindWhip(variation) != nullptr ? VR::Melee::FindTransformMove(variation, true) : nullptr;
}

} // namespace VR::Whip
