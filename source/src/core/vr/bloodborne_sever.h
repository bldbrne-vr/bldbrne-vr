// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "common/types.h"
#include "core/vr/sever_policy.h"

// Dismemberment in the game.
// On by default; VR_SEVER=0 turns it off. An eligible lethal Beast Claw or transformed Burial
// Blade hit that wins its roll (sever_policy.h Decide) copies the enemy's pose at the kill (its
// Havok skeleton's local and reference poses, its bones' names, its world matrix: about 20 us on
// the game thread) and queues it. One worker thread reads the enemy's model from its file
// (cached), checks the posed mesh, cuts it (sever_enemy.h CutEnemy, through the jobs' slots and
// memory budget) and puts the piece and the stump as runtime models; the corpse's part is hidden
// in its pose (SeverPose) and the pieces are drawn in-frame (SeverRenderTick). With
// VR_SEVER_DUMP=<folder>, each cut is also written there as OBJ (the game's own geometry, on the
// user's own machine only).
namespace VR::Bloodborne {

struct MeleeHitInfo;

// At start-up: the eboot's base (the test's character lists; VR_SEVER_TEST).
void InstallSever(VAddr base, u64 size);

// Dismemberment is on: unless VR_SEVER=0 (read once).
bool SeverEnabled();

// Awake while a dismembering weapon is held in either form (SeverWeaponEquipped, sever_policy.h
// CanWake), a cut corpse's part is hidden, or pieces exist; asleep, nothing of dismemberment
// runs: each hook tests this word inline
// and stops (2026-11-13: "No impact at all if beast claw is not equipped and there's no
// dismembered enemies around").
inline constexpr std::uint32_t SeverAwakeWeapon = 1;
inline constexpr std::uint32_t SeverAwakePieces = 2;
inline constexpr std::uint32_t SeverAwakeHidden = 4;
extern std::atomic<std::uint32_t> g_sever_awake;
inline std::uint32_t SeverAwake() {
    return g_sever_awake.load(std::memory_order_relaxed);
}

// The melee update, as the right weapon changes: a dismembering weapon (either form), or not (the
// worker thread starts with it; put away with nothing cut left, the thread ends).
void SeverWeaponEquipped(bool on);

void SeverPoseHidden(u8* chr);
void SeverAfterUpdateHidden(u8* chr);

// The NPC pose hook (bloodborne_body.cpp), right after any character's pose copy: a cut corpse's
// piece shrunk in its pose. While none is hidden: one inline test, no call.
inline void SeverPose(u8* chr) {
    if ((SeverAwake() & SeverAwakeHidden) != 0) {
        SeverPoseHidden(chr);
    }
}

// The NPC update hook (bloodborne_body.cpp, vt+0xF8), right after a character's update: a ragdoll
// has written its model-space pose (the one drawn); a cut corpse's piece is shrunk there too and
// its stump follows it. In the game's tables only while a cut corpse is hidden (SeverNpcUpdateHook).
inline void SeverAfterUpdate(u8* chr) {
    if ((SeverAwake() & SeverAwakeHidden) != 0) {
        SeverAfterUpdateHidden(chr);
    }
}

// bloodborne_body.cpp: its NPC update wrapper swapped into the game's two tables (true: a cut
// corpse is hidden) or out again (false: the game calls its own update straight).
void SeverNpcUpdateHook(bool on);

// The render-camera hook, every camera update while awake (the caller tests SeverAwake(): asleep,
// not even called): the severed pieces and stumps placed in that camera's view (their in-frame
// list), their flights stepped, cut corpses that left the world forgotten.
void SeverRenderTick(u64 record_id, const float* rows, const float* position);

// This corpse's cut is in flight or its part is hidden (a past life's memory is dropped here).
bool SeverKnown(const u8* target);

// Its part can be hidden: one of the two NPC classes whose pose copy and update are hooked
// (bloodborne_body.cpp SeverHookedCharacter), and not a c0000 human (its look is its armour).
bool SeverCutsCharacter(const u8* target);
// The final-damage detour, any hit of a dismembering weapon's: its enemy's model read ahead of its
// kill on the worker, once (asked once per enemy type lately; at most MaxPrefetch waiting, more
// dropped). One guest read and a few relaxed loads for a type already asked for.
void SeverPrefetch(const u8* target);
// bloodborne_body.cpp: its class is one of the two NPC tables, both hooks installed.
bool SeverHookedCharacter(const u8* chr);

// The kill's roll against its swing's chance (sever_policy.h SeverChance): uniform in [0, 1);
// 0 under the test switches (VR_SEVER_TEST, VR_SEVER_TEST_WEAKEN), so test runs always cut.
float SeverRoll();

// The final-damage detour, a lethal hit Decide said severs: the enemy's pose copied and
// queued. `heavy`: too heavy to cut in half (sever_policy.h MaxHalfKg): a torso blow takes the
// head or the nearest limb instead. False (logged) when it cannot be read or the queue is full.
bool SeverKill(const u8* target, const VR::Sever::Site& site, const std::array<float, 3>& contact,
               const MeleeHitInfo& info, bool heavy);

} // namespace VR::Bloodborne
