// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include "common/types.h"
#include "core/vr/bloodborne_melee.h"

// Bloodborne (v1.09 eboot): the weapon-only transform (vr.ini transform=native; core/vr/transform.h
// has the pure logic, core/vr/transform_clips.h the clips).
//
// Vanilla L1 plays a000_19xxx on the whole hunter; the weapon model plays a clip of the same id in
// its own anibnd, its time copied every frame from the hunter's TimeAct entry (PlayerIns vt+0x118
// 0x18F84F0, Game:Main), and the clip's TAE event 32 SetWeaponStyle switches the arm style (the
// form) at a fixed time. Here the right grip sends no L1. Instead:
//  - a 15-byte detour at 0x18F894D (vt+0x118, after it picked the TimeAct id and time for the
//    weapon parts) hands the main player's weapon parts the transform clip at our clock's time
//    (and every part that clip, none forced to its default: loop 3 at 0x18F8A80) while a transform
//    runs; otherwise the game's own choice goes on untouched;
//  - at the clip's event-32 time the main player's update (site A, bloodborne_heal.cpp) calls the
//    game's own TAE listener ([chr+0x68], vtable 0x5335150, vt+0x18 = 0x1A216B0) with a synthetic
//    {type 32, {style}} event: the game writes the style, its HKS mirror and the save copy
//    (PlayerGameData +0x248), tells the ChrCtrl and drops the other hand's buffs as for L1, and the
//    model, the moveset (physical melee's form B rows) and the menus follow; the read-back is
//    logged;
//  - the clip's own sounds (PlaySound at the hand) and effects (SfxSlotRequest on the weapon's
//    dummies) are replayed; the presenter adds a latch tick, a detent per mechanical sound and the
//    clack at the switch (core/vr/combat_channel.h transform events).
// No player animation id ever enters the TimeAct ring, so the arms stay on the controllers and the
// hunter keeps walking, rolling and quickstepping (a dodge, or the arm hold the body keeps after a
// weapon switch, neither holds up nor cancels it). A
// squeeze waits (transform_buffer_ms) while the game has the right arm for anything else (a hit,
// an item) or the transform before is still locking, then is refused; a transform before its
// switch is abandoned when the game takes the arm that way (no form change, as a vanilla cancel).
// A squeeze during a swing or with the hand moving is a flick (transform_flick): the clip starts at
// its flick start at the flick's pace, the form switches within the stroke, and physical melee
// makes the rest of that stroke the game's transform attack. Weapons without a native clip
// (core/vr/transform_clips.h Vanilla rows), transform=vanilla and any signature mismatch keep L1.
namespace VR::Bloodborne {

// OnEbootLoaded (VR enabled): checks the sites, installs the weapon-animation detour. Logs one
// line.
void InstallTransform(VAddr base, u64 size);

// The pose-copy wrapper (main player, every update; physical melee's tick calls it first with the
// same right-arm state): starts, runs and ends a transform, publishes the status.
void TransformPoseTick(u8* chr, const MeleeBody& body);

// The saw uses only the weapon-part animation hook. Cleared on every blocked/stale tick.
// Returns false if the verified native hook is unavailable (never falls back to a player clip).
bool SetSawWeaponPose(u8* chr, float time, bool active);

// The main player's update (site A): the form switch of a running transform.
void TransformSiteA(u8* player);

// Physical melee's gate: why a swing must not open now (transform_flick=0: a transform before its
// switch has settled); nullptr: it may (with the flick on, a swing during a transform joins it).
[[nodiscard]] const char* TransformSwingBlock();

// The newest transform run, as physical melee needs it to turn the stroke a flick happens in into
// the game's transform attack (core/vr/bloodborne_melee.cpp). Same thread as the pose tick.
struct TransformRunInfo {
    u32 id{0};     // +1 per run (0: none yet)
    u32 serial{0}; // its squeeze
    bool active{false};
    bool flick{false}; // a flick (a squeeze in motion, or a swing joined it before its switch)
    bool no_join{false}; // a hand transform's: no swing joins it (TransformPull::no_join)
    bool to_b{false};
    s32 from{-1};  // the arm style it switches from ...
    s32 style{-1}; // ... and to
    s32 weapon{-1};
    bool switch_asked{false}; // the switch was handed to site A (the old part goes soon)
    bool switched{false};     // site A switched the form
    bool aborted{false};      // it stopped before its switch (no form change)
    s64 start_ns{0};          // the squeeze it started on
    s64 switched_ns{0};
};
[[nodiscard]] const TransformRunInfo& TransformRun();

// A swing joined run `id` before its form switch (physical melee): from now on it plays at the
// flick's pace (core/vr/transform.h FlickFrom), so the weapon changes form within the stroke.
void TransformMakeFlick(u32 id, u32 window);

} // namespace VR::Bloodborne
