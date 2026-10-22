// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include "common/types.h"

// Bloodborne (v1.09 eboot) instant item pickups in VR (vr.ini pickup_instant, default 1; env
// VR_PICKUP_INSTANT overrides it): picking up an item (Cross / B on "Pick Up Item", "Search
// Corpse", "Search Body") in first-person VR grants it at once, with the game's own "picked up X"
// pop-up and sound, and without the pickup animation (so no crouch, no cinematic flat period, and
// the hunter keeps his arms and his movement).
//
// The item checker (0x12FD880, once per frame) asks ActionButtonMan::Check for each of its three
// prompt kinds; when Cross is accepted it calls ItemDropMan::PickUp (0x17D8570: the glow removed,
// its event flag set, the whole lot added through the inventory or sent to storage when full, the
// pop-up and the sound) and THEN pushes the event (0x0C, 4010, n) through 0x132E960 (0x132EAD0
// with the treasure's own animation) whose handlers OnEvent_4010_1 / _2 / _1001_PlayAnim /
// _1002_ForcePlayAnim play the crouch (101140). The three call sites (PickUp + the event push) are
// patched: the stub calls PickUp exactly as before and leaves the event out while the gate
// (core/vr/pickup_gate.h) says so; otherwise it pushes it as the game does. Every pickup is
// logged ("VR pickup: ..."): the lot, the inventory counts before and after, the glow removed, and
// instant or why the animation played. Signature-checked; a mismatch leaves the game's own pickup.
//
// VR_PICKUP_SCAN=1 (tests only): also logs the active item glows near the hunter (the checker's
// own lists, read on its thread) when they change, to find items to test with.
namespace VR::Bloodborne {

// OnEbootLoaded (VR enabled; before guest threads exist): checks and patches. Logs one line.
void InstallPickup(VAddr base, u64 size);

// Pickups granted without the animation this session (logs, tests).
[[nodiscard]] u32 InstantPickups();

} // namespace VR::Bloodborne
