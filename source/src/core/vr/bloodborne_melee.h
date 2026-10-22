// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include "common/types.h"
#include "core/vr/anim_classes.h"
#include "core/vr/body_frame.h"

namespace VR::Bow { struct NativeGeometry; }

// Bloodborne (v1.09 eboot): physical melee, the swing IS the attack (vr.ini melee=physical;
// core/vr/melee_window.h has the pure logic).
//
// The presenter publishes a "window" for every real swing (core/vr/combat_channel.h): open while
// the blade moves fast, no R1 / R2. The body's pose-copy wrapper (PlayerIns vt+0xE0, the loop
// thread, where the game's own TAE events call InvokeAttack) then does exactly what the attack
// clip's InvokeAttackBehavior event does, without the clip: GetBehaviorId (the weapon's row for
// light 0 / heavy 150 / charged 170, form B 400 / 550-910-500 / 570-930) and InvokeAttack
// 0x1A293B0 on a slot of our own (6 or 7), every game frame while the window is open. The game
// creates the attack once (stamina once, one hit list per swing) and sweeps its capsule between
// the weapon's dummy polys, which follow the IK blade, every frame; everything else is the game's.
// The hunter never leaves his idle or locomotion state: no attack animation, no lunge, the arm
// stays on the controller.
//
// Around it:
//  - the visceral, riposte or backstab: each new window first calls the game's own R1 throw check
//    (TryThrowAll 0x1A60620, pad type 3) in the main player's update, where vanilla R1 does;
//  - damage: the final-damage detour (core/vr/bloodborne_tuning.cpp) hands every hit of the
//    hunter's to MeleeDamage, which scales a physical swing's hit by the blade's speed and the
//    swing cadence (melee_speed_damage) and reports it (haptic thump, the blade's bite);
//  - walls: a 16-byte detour on the player's map-hit function 0x1A332B0 (attacker module
//    vt+0xB0) reports the clank and suppresses the game's canned wall bounce for our attacks (the
//    sparks and sound stay the game's);
//  - the capsule radius x melee_radius_scale, the weapon's own swing sound (PlaySound) at the
//    commit, its blade trail (the TAE's SpawnFFX_Blade 0x1898E60) while the window is open (both
//    from the weapon's attack clip, core/vr/melee_moves.h), and the regen pause;
//  - the telegraph (melee_telegraph): a swing the presenter judged readable (a wind-up, a heavy,
//    charged, long or wide swing) gets the two notices a vanilla attack's start sends the AIs, in
//    the main player's update: "the target attacks" (PlayerIns vt+0x440, the AIs' FindAttack
//    interrupt) and the parry timing (vt+0x448). No animation, no state of the hunter's changes.
// Weapons and forms without physical moves (core/vr/melee_moves.h) keep R1 / R2: the status
// (core/vr/combat_channel.h MeleeStatus) tells the presenter.
//
// Every site is signature-checked at install; a mismatch keeps melee vanilla and is logged.
namespace VR::Bloodborne {

// OnEbootLoaded (VR enabled): checks the sites, installs the map-hit detour. Logs one line.
void InstallMelee(VAddr base, u64 size);

// True when physical melee is installed and on (the final-damage detour must be in for it).
[[nodiscard]] bool MeleePhysicalWanted();

// What the body knows about the right arm this update (core/vr/bloodborne_body.cpp).
struct MeleeBody {
    bool body{false};     // the VR body runs (not disabled)
    bool driven{false};   // the first-person view: our arms are in the pose
    bool right_ik{false}; // the gate gives the right arm to you
    float right_weight{0.0f};
    VR::Body::GateReason reason{VR::Body::GateReason::None};
    bool held{false}; // an animation holds the arms (owner_id below)
    s32 owner_id{-1};
    // The right arm is yours through a kept hit or dodge (arm_keep_on_hit / arm_keep_on_dodge),
    // but the game takes no attack yet (its cancel window has not opened): right_ik is false.
    bool attack_wait{false};
    // A kept dodge with vr.ini dodge_attack: a swing goes now anyway (the dodge plays on; its
    // attack ends the dodge's i-frames, dodge_attack_iframes=0). The transform still waits.
    bool dodge_attack{false};
    // The left arm (melee_left): the gate gives it to you and the game takes its attack.
    bool left_ik{false};
    float left_weight{0.0f};
    // The hands this update (core/vr/body_frame.h): the last sample again (arm pair sync, a stereo
    // pair's second eye), and the play space's heading (radians; a snap turn jumps it).
    bool hands_reused{false};
    float base_yaw{0.0f};
    // Borrowed only during this synchronous pose tick; consumers copy what they need.
    // The part-animation hook otherwise sees last frame's already-synchronized dummy matrix.
    const VR::Body::QsTransform* pose_locals{nullptr};
    const VR::Body::ChrFrame* pose_frame{nullptr};
    s64 pose_ns{0};
};

// The body's pose-copy wrapper, main player only (loop thread), every update, after the gun:
// opens, keeps alive and ends the swing's attack, publishes the status. It first runs the
// weapon-only transform's tick (core/vr/bloodborne_transform.h) with the same right-arm state.
void MeleePoseTick(u8* chr, const MeleeBody& body);

// The main player's update (core/vr/bloodborne_heal.cpp site A): a new telegraph's AI notices,
// and the visceral try for a new window (VR_MELEE_AI_LOG: the AIs' side, also with
// melee=vanilla).
void MeleeSiteA(u8* player);

// The final-damage detour (DamageStep), for every hit the main player deals (`damage` = the final
// float damage, writable): true when it was a physical swing's hit (scaled and reported).
bool MeleeDamage(u8* res, u8* target, float* damage);

// A right-hand hit (the final-damage detour's damage result), for the kill ragdoll's push
// (core/vr/bloodborne_ragdoll.h): the physical swing it came from (its window, the move, the
// presenter's tip speed) and the right weapon's blade in the world at the last pose tick, swing or
// not (with melee_kill_ragdoll; R1 / R2's canned swings too): its hit capsule's two ends
// (MeleeBladeSegment, the grip side first) and their velocities from that tick and the one before.
struct MeleeHitInfo {
    s32 weapon_variation{-1}; // captured when the killing swing opened
    bool form_b{false};
    bool two_handed{false};
    bool swing{false};      // a physical swing's hit (else swing's fields below are not set)
    u32 serial{0};          // its window
    const char* kind{""};   // "light", "heavy", "charged" ...
    bool transform{false};  // a flick's transform attack
    float tip_speed{0.0f};  // the presenter's tip speed for that window (m/s; 0 unknown)
    bool blade{false};      // the blade below is known (two pose ticks within 0.1 s)
    VR::Body::Vec3 base{}, tip{};
    VR::Body::Vec3 base_vel{}, tip_vel{};
    s64 blade_ns{0}; // steady clock of the pose tick
};

// The final-damage detour (DamageStep): what MeleeDamage matched for this hit; false when the hit
// is not a physical swing's (the blade may still be filled in for any right-hand hit).
bool MeleeHitInfoOf(u8* res, MeleeHitInfo& out);

// SolveBody (loop thread): the blade's bite on the right arm's target, model space (melee_bite;
// none without a recent hit).
void MeleeBite(VR::Body::ModelTarget& right_target, s64 now_ns);

// quick_use_wipe: the right weapon's blade (its hit capsule's axis, base -> tip, world) for the
// paper wiped along it; false (`how`: why) when not known. `grip`: the weapon's grip (dummy 300)
// it was measured from. `strike_near`: how far from the grip the striking part begins (the hit
// capsule's near end before any haft extension, a hit sphere's head), game units. Game:Main.
bool MeleeBladeSegment(u8* chr, VR::Body::Vec3& base, VR::Body::Vec3& tip, const char*& how,
                       VR::Body::Vec3* grip = nullptr, float* strike_near = nullptr);

// weapon_wall (core/vr/solid_weapon.h): the right weapon's blade as far from its grip as its base
// and its tip (world units), at the last pose tick; false when not known (no blade: a fist, the
// bow). Game:Main (the body's pose copy, the same thread).
bool MeleeBladeReach(float& base, float& tip);

s32 MeleePartLine(int hand, VR::Body::Vec3& base, VR::Body::Vec3& tip, bool& seen);

// Current native bow/arrow animation roots, for weapon-only nock placement.
// Requires the bow skeleton; the arrow skeleton only while an arrow is present.
// Never fabricates geometry.
bool MeleeBowGeometry(const u8* chr, VR::Bow::NativeGeometry& out, bool arrow_present);
// Pose-copy publishes a string centre in the bow part's frame; ns=0 cancels.
void MeleeBowString(VR::Body::Vec3 target, s64 ns);
// Shared native weapon-animation tail, immediately before model-space sync.
void MeleeBowPartPose(u8* node);


// quick_use_wipe's measurement: the light move's blade trail dummies (`id_a`, `id_b`: the game's
// swing trail is drawn between them, along the blade you see), world; false when not known.
bool MeleeBladeTrail(u8* chr, VR::Body::Vec3& a, VR::Body::Vec3& b, s32& id_a, s32& id_b);

} // namespace VR::Bloodborne
