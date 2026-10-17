// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

// Bloodborne (v1.09 eboot) interactive visceral (vr.ini visceral_pull; the rule in
// core/vr/visceral_pull.h): the hunter's throws (viscerals, ripostes, backstabs) stay in first
// person, and just before the rip the hunter and his target slow to x visceral_pull_speed until the
// presenter sees you yank your right hand back out (visceral_channel.h VisceralPulled).
//
// How: the frame-time detour of the shorter staggers (bloodborne_stagger.cpp, ChrIns::Update
// 0x18BD716) scales the frame time the hunter's update runs on (his behaviour, TAE, root motion)
// and, while a hold runs, also his target's: its entry lets the ChrIns at VisceralPullPartnerSlot()
// through besides the main player. With visceral_freehand the catch holds the world still: while
// the qword at VisceralPullWorldSlot() is set every ChrIns goes through, and all but the pair run
// at the hold's world rate (characters only: projectiles, effects and sounds go on). The hunter's
// throw: his throw module's ThrowState (odd = he
// throws, bloodborne_layout.h); its clip and time: his TimeAct ring's newest entry. The target: the
// character he hit last (bloodborne_tuning.cpp LastPlayerHitTarget), which must be in the throw
// too (its ThrowState even), else no hold. The rip's clip time is learned at the throw's first
// damage (VisceralPullOnDamage, from the final-damage detour) and saved in vr.ini.
//
// Logs: `VR visceral pull: ...` per hold and per learned clip; VR_VISCERAL_PULL_LOG=1 adds every
// throw's start and end.
namespace VR::Bloodborne {

// OnEbootLoaded, before InstallStaggerSpeed: reads the settings and the learned rip times.
void InitVisceralPull(VAddr base, u64 size);
// The frame-time detour must be installed for it (even with stagger_shorten_ms=0).
[[nodiscard]] bool VisceralPullWanted();
// The address of the pointer the detour's entry compares each ChrIns with (the held throw's
// target; null otherwise). Read by generated code as a plain qword.
[[nodiscard]] u8* const* VisceralPullPartnerSlot();

// The frame-time detour (the game's update thread). The main player's update: `clip` / `clip_time`
// his newest TimeAct entry (clip -1: not read). Returns the factor for his frame time.
[[nodiscard]] float VisceralPullMain(u8* player, s32 clip, float clip_time, float dt);
// Only the visual arm blend returns early; combat stays gated by the game's throw.
[[nodiscard]] bool VisceralPullRecoverArms(const u8* player);
// Only the outgoing throw gate uses this; enemy grabs keep their existing policy.
[[nodiscard]] bool VisceralPullFreehand();
// The character the hunter's throw holds (its victim), while it runs; null otherwise.
[[nodiscard]] u8* VisceralPullVictim(const u8* player);
// The qword the detour's entry tests: non-zero while freehand's catch holds the world still
// (every character then goes through the hook).
[[nodiscard]] const u64* VisceralPullWorldSlot();
// Another character's update (the held throw's target, an open visceral's staggered target, or
// any while the world holds): its factor.
[[nodiscard]] float VisceralPullPartnerRate(const u8* chr);

// The final-damage detour (bloodborne_tuning.cpp): the main player dealt damage to `target`
// (`bullet`: the damage result's bullet id, -1 for a melee hit). Only a melee hit on the throw's
// own victim teaches the rip. Returns true to soften HP damage for a mistimed pull.
[[nodiscard]] bool VisceralPullOnDamage(u8* player, u8* target, s32 bullet);

// OnEbootLoaded, after InstallStaggerSpeed: on only with the frame-time detour in (and the damage
// detour to soften mistimed pulls); then the hunter's throws play in first
// person (VR::SetVisceralFirstPerson).
void ConfirmVisceralPull(bool frame_time_hook, bool damage_hook);

} // namespace VR::Bloodborne
