// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

// Bloodborne (v1.09 eboot) VR difficulty tuning, both off at 0 (nothing patched):
//
// - player_hp_bonus_pct (default 20): the hunter's max HP is +N%. SpEffectMgr::GetMaxHpRateProduct
//   (0x17E46E0) is detoured and its result multiplied for the main player's SpEffect manager only,
//   so every max-HP derivation (the per-update recompute, menus, the vial's 40%, level-up) uses the
//   game's own base x product math. Nothing is stored: the save keeps its vanilla base max HP, and
//   load, death, area change and NG+ need nothing re-applied.
// - offhand_damage_pct (default 10): hits the main player deals with the left-hand weapon (guns,
//   cannon, flamesprayer, torch) do +N% of the vanilla final damage. Detour at 0x1A2BC9F inside
//   the chr damage function, right before the final float damage is truncated: attacker == the
//   main player, hand == left (-2), target != the player.
//
// Logs: VR_HP_LOG=1 (max HP checks and hits taken), VR_HP_AUTOTEST=1 (in-run A/B of the bonus),
// VR_OFFHAND_LOG=1|2 (each player hit; 2 = hits taken too), VR_OFFHAND_DRYRUN=1 (computes, never
// writes).
namespace VR::Bloodborne {

// OnEbootLoaded (VR enabled; before guest threads exist): checks every site and installs.
void InstallTuning(VAddr base, u64 size);

// Main player update (Site A, the player's update thread): logs and the autotest only.
void TuningUpdate(u8* player);

[[nodiscard]] bool IsHpBonusInstalled();
[[nodiscard]] bool IsOffhandDamageInstalled();

// The character the main player last hit and when (steady clock ns; the off-hand damage hook sees
// every hit he deals while it is installed). Test logs only (core/vr/bloodborne_gun.cpp).
[[nodiscard]] u8* LastPlayerHitTarget(s64& steady_ns);

// How many damage calculations have hit the main player (with damage above 0: an enemy's attack,
// a bullet, an explosion) and when the last one did (steady clock ns), while the damage hook is
// installed (IsOffhandDamageInstalled). The hit vignette tells hits from drains with it.
[[nodiscard]] u32 PlayerDamageEvents(s64& last_steady_ns);

// How many of the main player's bullets have hit another character with damage so far: his
// left-hand gun's and his thrown items' (a knife, a Molotov, a pebble), while the damage hook is
// installed. The presenter's hit ticks (the gun hand's after a shot, the throw's).
struct RangedHits {
    u32 gun{0};
    u32 thrown{0};
};
[[nodiscard]] RangedHits PlayerRangedHits();

} // namespace VR::Bloodborne
