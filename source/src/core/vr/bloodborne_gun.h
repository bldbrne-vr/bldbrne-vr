// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "core/vr/anim_classes.h"
#include "core/vr/body_frame.h"
#include "core/vr/body_target_facts.h"

// Bloodborne (v1.09 eboot): the left-hand gun as a VR gun (core/vr/gun_aim.h has the math).
//
// Aim (vr.ini gun_aim): every bullet the game launches passes the emitter builder 0x19F2440,
// which asks 0x18C7800 for the emitter matrix (the hunter's facing, or the lock-on point). A
// 21-byte detour right after that call (0x19F263A) rewrites the matrix of the hunter's own gun
// shots (BehaviorParam category 2 of a known gun) to a look-at of the gun's visible barrel: the
// left grip of the trigger pull (the presenter's pull, core/vr/combat_channel.h) times the
// barrel's angle in the hand, mapped to the world with the camera's base yaw. Locked on, the ray
// turns at most about 1 degree toward the target; the start moves sideways onto the barrel's line.
// No barrel (third person, the virtual screen, the game owning the arm, an unknown gun): the
// game's own direction, logged with the reason.
//
// Fire (vr.ini gun_fire=direct): the presenter no longer presses L2 for the gun. Each pull goes
// to the body's pose-copy wrapper (PlayerIns vt+0xE0, the loop thread, where the game's own gun
// clip fires: TAE event 318) and is fired there through the game's own functions, exactly as the
// event does: CanFireGun (bullets), GetBehaviorId (judge 800, 810 under Bone Marrow Ash),
// InvokeGun 0x18C7530 (the BehaviorParam -> bullet path: damage, parry, AI sound, bloodtinge
// scaling are the game's) and ConsumeBullets. The muzzle flash (FFX on the part's muzzle dummy,
// a private effect slot), the bang (PlaySound at the muzzle), the regen pause and the gun's
// cadence follow the gun clip's TAE. The bullet launches in the same game frame, along the barrel
// ray of the pull. No gun clip plays: the arm stays yours; the recoil is a kick of the IK target
// (GunKick) and a thump on the controller (the presenter).
//
// Every site is signature-checked at install; a mismatch keeps that part vanilla and is logged.
namespace VR::Bloodborne {

// OnEbootLoaded (VR enabled): checks the sites, installs the aim detour. Logs one line each.
void InstallGun(VAddr base, u64 size);

// What the body knows about the left arm this update (core/vr/bloodborne_body.cpp).
struct GunBody {
    bool body{false};    // the VR body runs (not disabled)
    bool driven{false};  // the first-person view: our arms are in the pose
    bool left_ik{false}; // the gate gives the left arm to you
    float left_weight{0.0f};
    VR::Body::GateReason reason{VR::Body::GateReason::None};
    // A kept dodge with vr.ini dodge_attack: left_ik is true before the dodge's attack window; a
    // shot fired now ends the dodge's i-frames (dodge_attack_iframes=0).
    bool dodge_attack{false};
};

// The body's pose-copy wrapper, main player only (loop thread), every update, after the held
// item's tick: the gun status, a pending pull's shot, the muzzle flash and the regen pause.
void GunPoseTick(u8* chr, const GunBody& body);

// What the body knows about the right arm this update (core/vr/bloodborne_body.cpp).
struct BladeBody {
    bool driven{false};   // the first-person view: our arms are in the pose
    bool arm_free{false}; // the gate gives the right arm to you and the game takes its attack
    VR::Body::GateReason reason{VR::Body::GateReason::None};
};

// The right hand's shots (core/vr/trick_weapons.h Shot: gun_blade, moon_wave, stake_blast), the
// body's pose-copy wrapper after the melee tick (its status names the weapon and form): the
// presenter's newest BladeShot fired as the weapon's own shot clip would (InvokeGun on the right
// hand's behaviour row; a gun-blade's bullets, flash and bang), with no animation; the emitter hook
// starts it on the weapon's own line and flies it there, or along the presenter's (the wave out
// through the blade, the punch). Its fate: Combat::MeleeEventKind::Shot.
void BladeShotTick(u8* chr, const BladeBody& body);

// A BehaviorParam_PC row's refType (0 an attack, 1 a bullet), -1 without one (read only; the
// melee's Stake Driver blast, core/vr/bloodborne_melee.cpp).
[[nodiscard]] s32 GunBehaviorRefType(s32 bid);

// bow_direct (core/vr/bow.h): the fire hook is ready and arrow rows were not missing.
// An unknown twang uses the release whoosh; it does not delay direct shots.
[[nodiscard]] bool GunBowDirectOk();

// The projectile launch tail (core/vr/bloodborne_quickitems.cpp, 0x19F7835, same thread as the
// emitter): VR_GUN_LOG's readback of our ray against the launched velocity, and the child
// bullets along it.
void GunLaunchTail(u8* projectile);

// Copied native shot observations for the body-owner target controller.
VR::Body::RecoilFacts GunTargetFacts();
void LogGunTargetKick(const VR::Body::RecoilLog& kick);

// Successful Stake Driver melee discharge: native blast effect and weapon-hand recoil.
void StakeDischarge(u8* chr, s64 now_ns);

// The EquipParamWeapon row of `weapon` (WeaponParamRow 0x1F29AA0, signature-checked at install), or
// nullptr. Read only (the torch's discovery log, core/vr/torch_lift.h).
[[nodiscard]] const u8* GunWeaponParamRow(s32 weapon);

// weapon_wall (core/vr/solid_weapon.h): the left gun's line along its barrel (L_Weapon's
// gun_aim.h BarrelInWeaponBone(`attach`)), from `back` to `front` along it from the left hand's
// grip (world units); false with no gun in the left hand (or not measured yet). Game:Main.
bool GunBarrelReach(s32& attach, float& back, float& front);

} // namespace VR::Bloodborne
