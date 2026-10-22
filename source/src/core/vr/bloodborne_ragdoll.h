// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include "common/types.h"

// Bloodborne (v1.09 eboot): instant ragdoll on a right-hand melee kill (vr.ini melee_kill_ragdoll,
// melee_ragdoll_impulse; the pure rules are core/vr/ragdoll_kill.h).
//
// What the game does (offline reverse-engineering notes): the death animations of the ordinary
// enemies (a22xx, a16x0) carry TAE event 228 ("RagdollReviveTime", args 0, 0) over their last part;
// its handler (0x1A2227B in the TAE event switch 0x1A216B0) writes the chr's ChrCtrl ragdoll request
// +0x144 = 1 and weight +0x14C = 1.0, and the ChrCtrl ragdoll update (0x1515CC0, every chr every
// frame on its own CSChrThread) turns that into RagdollIns state 2 (the bodies dynamic, the ragdoll
// drives the pose). Until then the corpse plays its death animation (the Scourge Beast rears up for
// 1.7 s).
//
// Here: the final-damage detour (core/vr/bloodborne_tuning.cpp) classifies every hit of the
// hunter's; a lethal right-hand weapon attack (BehaviorParam category 1, no bullet, the right hand,
// the target not in a throw) arms its target. A 16-byte entry detour on 0x1515CC0 compares the
// ChrCtrl with the (at most 4) armed ones and only then calls in: on the target's own update, once
// its HP is 0, the game names the hunter its killer and its death animation is in the TimeAct ring,
// a death that ends in the ragdoll (core/vr/ragdoll_deaths.h) gets the same two writes event 228
// makes, and the switch happens in that very call; on its next update the dynamic bodies are pushed
// with hknpWorld::applyBodyLinearImpulse (0xB68DB0, what the game's own ragdoll blast calls on the
// same thread) along the blade's velocity at the hit (core/vr/bloodborne_melee.h MeleeHitInfoOf).
// Everything else of the death is the game's: the death animation still runs underneath (its
// sounds, its effects), the blood echoes, drops, flags, the lock-on release, the enemy staying dead.
// Deaths by bullets, fire, bolt paper, thrown items, the left hand, falls, viscerals and backstabs,
// deaths that end animated, bosses, dissolves and NPC hunters stay as they are. A kill that would go
// limp rolls once (melee_ragdoll_vanilla_pct, 22% by default): on it, it keeps the game's own death
// too (nothing written, no push).
//
// Every site is signature-checked at install; a mismatch leaves every death vanilla and is logged.
namespace VR::Bloodborne {

// OnEbootLoaded (VR enabled), after InstallTuning (the damage detour must be in). Logs one line.
// Installed whatever melee_kill_ragdoll is at the start: the VR page (Assists > Instant ragdoll,
// Ragdoll push) changes both live, read at the kill and at the push.
void InstallRagdoll(VAddr base, u64 size);

// True when melee_kill_ragdoll asks for it now (the final-damage detour must be installed for it).
[[nodiscard]] bool RagdollWanted();

// The final-damage detour (DamageStep, Game:Main), for every hit whose attacker is the main player,
// after the physical swing's scaling: a lethal right-hand weapon attack arms its target. Returns the
// damage to apply: unchanged (test aids: VR_RAGDOLL_ONESHOT=1 makes such a hit lethal, =any every
// hit of the hunter's; VR_RAGDOLL_LOG=1 logs the armed blow and the ragdoll's state after it).
float RagdollOnPlayerHit(u8* res, u8* player, float damage);

} // namespace VR::Bloodborne
