// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include "common/types.h"
#include "core/vr/combat_channel.h"
#include "core/vr/melee_moves.h"
#include <span>

// Bloodborne (v1.09 eboot): the physical whip (vr.ini whip=physical; core/vr/whip_chain.h has the
// pure chain). The Beast Cutter's form B is a chain of 26 bones (Bone_Chain_1..26, 3.4 cm apart,
// the 13 blade segments on every second one, the hit dummies 12100 tip / 12130 mid / 12110 root
// blade / 12120 handle on them). The game only ever animates it: its idle clip holds it as a rigid
// 1.17 m line, its attack clips stretch the links up to x5-7 and lash them (no physics, no cloth).
//
// The chain pays out with the swing's speed (core/vr/whip_spin.h): fast, toward the game's own reach;
// it stays out while the hand keeps moving (a spin overhead circles it like a helicopter blade) and
// reels back in after whip_hold_s of slow motion. Its attack waits for the chain's tip to be fast
// and ends when it slows (only a moving chain hurts); each revolution of a spin is a swing of its own
// (the presenter's SpinCounter, paid from the combo budget).
//
// Here the chain is simulated on the hunter's hand instead, and the simulated pose IS the weapon's
// pose: a 26-byte detour at the tail of the weapon parts' animation update (0x1CA6E30, after the
// part's clip was sampled into its hkaPose locals, before its jump to syncModelSpace 0x6C4F20; every
// part of every character comes through, the stub compares the pose node with ours first) writes
// the chain bones' locals. The model pose, the part's world bones, the skinning, the dummy polys and
// so the game's own hit capsules all follow it: what you see is what hits. The swing's attack is
// physical melee's (core/vr/bloodborne_melee.h) on the whip's form-B rows (core/vr/whip_moves.h):
// its capsules ride the simulated chain; while the window is open the chain pays out to the game's
// own reach for that move and lashes.
//
// It drives the chain only while the part plays its idle clip (999): a transform's clip, a visceral
// or any other clip the game hands the weapon keeps its own pose (blended over 0.15 s both ways).
namespace VR::Bloodborne {

struct MeleeBody;

// OnEbootLoaded: checks the sites; with whip=physical (or VR_WHIP_LOG for the observing trace)
// writes the detour. Logs one line.
void InstallWhip(VAddr base, u64 size);

// The body's pose-copy wrapper (MeleePoseTick, loop thread), main player, every update: finds the
// right weapon's whip part, decides whether the chain is driven, logs (VR_WHIP_LOG; VR_WHIP_REF:
// the game's own whip clips traced, the reference).
void WhipPoseTick(u8* chr, const MeleeBody& body);

// Physical melee's rows for the right weapon (ReadWeapon): `base` with the whip form's rows when
// the physical whip is on for that weapon, else `base`.
[[nodiscard]] const VR::Melee::WeaponMoves* WhipMovesFor(s32 variation,
                                                         const VR::Melee::WeaponMoves* base);

// True when `row` is one of the whip's (its hit dummy may be metres out on the chain).
[[nodiscard]] bool IsWhipRow(const VR::Melee::MoveRow* row);

// The swing's window closed but the whip is still out and lashing fast: its attack stays alive.
[[nodiscard]] bool WhipHoldsWindow(const Combat::MeleeWindow& w, s64 now_ns);

// The chain of the whip in the hand (Game:Main). `applies`: the physical whip drives it now (its
// attacks are on the chain); `fast`: its tip was fast enough to hurt (core/vr/whip_spin.h
// HurtTipSpeed) within the last 0.15 s: its attack waits for that and ends without it; `tip`: the
// tip's fastest (m/s) over the last 0.12 s, a hit's speed factor.
struct WhipChainState {
    bool applies{false};
    bool fast{false};
    float tip{0.0f};
    s32 variation{-1};
    VR::Body::Vec3 grip{}; // world-space grip, sampled with the simulated chain
};
[[nodiscard]] WhipChainState WhipChainNow(s64 now_ns);

// Render pose, Game:Main: blade-chain bone origins in world space, root -> tip.
// True means a physical whip is equipped; count == 0 means its pose is unavailable
// (do not fall back to an invisible grip-to-tip chord). Excludes the bare handle.
bool WhipWipePoints(u8* chr, std::span<VR::Body::Vec3> points, int& count);

} // namespace VR::Bloodborne
