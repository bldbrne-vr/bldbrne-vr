// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "core/vr/bullet_deflect.h"

// Bloodborne (v1.09 eboot) bullet deflection (vr.ini melee_deflect; the geometry is
// core/vr/bullet_deflect.h). Every projectile an enemy fires is noted at its launch (the quick
// items' launch hook, bloodborne_quickitems.cpp LaunchHook). Each render tick (Game:Main, where
// the projectiles update), after the body has placed the weapons, the step each one will take next
// is tested against the right weapon's capsules as they swept since the last update; one that
// would meet them is put where it touches, sent off along the bounce (Bounce; a swing's return by
// ReturnChance, any other kept clear of the shooter) and made the hunter's (P+0x2FC): the game's
// attack casts resolve a bullet's attacker through its owner every update (0x185DE29 /
// 0x186012F), so it now hurts his enemies and no longer him. One sent back at its shooter is
// steered at him on its way, and the first damage its blast does him is felt in the hand
// (MeleeEventKind::Ricochet).
namespace VR::Bloodborne {

void InstallDeflect(VAddr base, u64 size);
// Whether the launch hook should be installed for it (melee_deflect and the signatures matched).
bool DeflectWanted();

// The launch hook (P = the projectile, its velocity and orientation set): an enemy's bullet.
void DeflectLaunch(u8* p);

// The body (SolidWeapons): the right weapon's capsules this update (world, game units; `scale`
// game units per metre). `fresh`: a new update of the hands (not a stereo pair's second eye or a
// snap turn); `snap`: the weapon was placed again (no sweep from where it was).
void DeflectBlade(const Deflect::Blade& blade, float scale, bool fresh, bool snap, s64 now);
// No right weapon this update (unarmed, a lever, the hand not ours).
void DeflectNoBlade();

// The body: the newest deflection not yet felt (the weapon wall's strike: sparks and a thud).
struct DeflectStrike {
    Deflect::Vec3 point{};  // world
    Deflect::Vec3 normal{}; // world, out of the blade
    float speed{0.0f};      // m/s, the strike's weight
};
bool TakeDeflectStrike(DeflectStrike& out);

// The render tick (bloodborne_camera.cpp, after BodyRenderTick).
void DeflectTick(s64 now);

// The game's damage on a character from a bullet of the hunter's (bloodborne_tuning.cpp's damage
// hook, the final damage): a blast sent back hitting its shooter. One atomic load when none flies.
void DeflectOnDamage(const u8* target, s32 bullet, float damage);

// Main-player pose update, at the same point as melee's regen pause. Only an atomic expiry
// check when no paid block is pending; never drains stamina here. now=0 samples the clock lazily.
void DeflectRegenTick(u8* player, s64 now = 0);

} // namespace VR::Bloodborne
