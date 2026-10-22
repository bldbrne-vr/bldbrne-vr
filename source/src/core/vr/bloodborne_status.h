// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include "common/types.h"

// Bloodborne (v1.09 eboot): status build-ups in VR.
//
// What the game does: PlayerIns +0x3B0 (modules) -> +0x28 is the SprjChrResistModule: +0x10..+0x20
// the REMAINING resistance (s32: slow poison, rapid poison, bleed, frenzy, beasthood), +0x24..+0x34
// the max; build-up = max - current. Its Update (0x1A5C440, once per character update: 0x18BDCEF
// in ChrIns's common update, from PlayerIns::Update at 0x18F6BA3) adds RATE * dt to each current
// (clamped to max, the fraction kept), RATE one s32 global for every type and character
// (eboot+0x512838C = 5 points per game second). A hit lowers current (AddBuildup 0x1A5C610); at
// <= 0 the status triggers and current goes back to max (beasthood stays at 0).
// The HUD's meter (fe.gfx ConditionGaugeList) is filled each frame into a static block (the HUD
// update's data, eboot+0x553EA20 by the lea at 0x1F62EA7) at +0x13F0 + i * 0x28: +0x08 the icon
// (-1 = row off), +0x18 the bar's length (L %).
//
// Here:
//  - status_drain_pct (default 10): the main player's slow poison, rapid poison, bleed and frenzy
//    build-ups drain that much faster (VR::StatusDrain: +10% of the game's own rate, 5 -> 5.5
//    points / s, only while falling, never above max, never beasthood), in Site A (the main
//    player's update, after the game's own recovery in the same update). Enemies, NPCs and other
//    players never reach Site A.
//  - VR::Conditions::Latest(): the meter's rows for the UI layer's piece (ui_conditions).
// The module's offsets, the rate and the row block are checked against signatures at install;
// a mismatch turns that part off (logged).
namespace VR::Bloodborne {

// OnEbootLoaded (VR enabled): checks the signatures, resolves the rate and the row block. Logs one
// line.
void InstallStatus(VAddr base, u64 size);

// Main player update (Site A): the extra drain (status_drain_pct, live).
void StatusSiteA(u8* player);

} // namespace VR::Bloodborne
