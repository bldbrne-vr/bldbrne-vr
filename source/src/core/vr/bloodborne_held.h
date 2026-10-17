// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "core/vr/anim_classes.h"

// Bloodborne (v1.09 eboot): the armed Blood Vial in the hunter's own left hand (vr.ini
// vial_in_hand=game).
//
// Bloodborne's held items are effects: the Blood Bullets clip (a00 100141) holds a vial in the left
// palm with TAE event 96 (FFX 173 with the vial model s10002 on dummy poly 3, L_Hand) and hides
// the gun with event 322 (left part 0). While the presenter requests the vial (core/vr/
// held_item.h), every main-player update does the same through the game's own functions: it
// refreshes FFX 173 on dummy 3 in a private effect slot (SfxSlotRequest 0x1896DB0, the call event
// 96 makes) and writes the left weapon's hide byte (ChrAsm +0x268, which the weapon update reads
// and resets every frame). The game attaches, lights, fogs and depth-tests the vial like any held
// item, so it follows the hunter's real hand through the IK and every animation, and his fingers
// cover it. Stopping the refresh ends the effect within one update and the gun comes back at the
// next weapon update; nothing is consumed, no animation or SpEffect is started.
//
// It runs from the body's pose-copy wrapper (PlayerIns vt+0xE0, loop thread: after the IK, before
// the effect update vt+0xF8 and the weapon update vt+0x118 of the same frame), or, when the body
// hook is not installed, from the player update hook (site A, core/vr/bloodborne_heal.cpp). Every
// site is signature-checked at install; any mismatch or runtime failure publishes a status that
// makes the VR overlay draw its own vial at the controller instead.
namespace VR::Bloodborne {

// OnEbootLoaded (VR enabled): checks the sites. Logs one line either way.
void InstallHeld(VAddr base, u64 size);

// The arm gate of this update, from the body (core/vr/anim_classes.h).
struct HeldGate {
    bool valid{false};
    VR::Body::GateReason reason{VR::Body::GateReason::None};
    VR::Body::AnimClass owner{VR::Body::AnimClass::Unknown};
    bool held{false};
};

// The body's pose-copy wrapper, main player only (loop thread), every update.
void HeldPoseTick(u8* chr, const HeldGate& gate);

// The player update hook (site A), main player only: ticks when the pose hook is not installed.
void HeldSiteATick(u8* player);

// The render-camera hook (loop thread), after the frame's camera is placed: while the vial is in
// the hand, its capsule in the view of camera record `record_id` (`rows`: the render camera's
// right / up / forward rows, 4 floats each; `position`: its position) goes into that record for
// the DIBR depth proxy (vr.ini vial_dibr_proxy).
void HeldRenderTick(u64 record_id, const float* rows, const float* position);

// The left weapon was hidden by the last update (the body's gun check skips it then).
[[nodiscard]] bool HeldLeftHidden();

// vial_inject_drain (core/vr/vial_drain.h): the last update made the vial in the hunter's hand
// ours (the game's effect not refreshed): the in-frame renderer draws it with the level of
// `drain_word` (VR::Held::PackDrain's, as that update saw it; 0 = full). Stale after 150 ms
// (`now_ns`: steady clock).
[[nodiscard]] bool HeldVialOurs(s64 now_ns, u64& drain_word);

// The grip the hunter's left fingers close into for what the last update showed in his hand
// (VR::Body::Grip::Kind; 0 = nothing held). The body's pose copy writes it (vr.ini held_grip).
struct HeldGripState {
    u8 kind{};
    s32 goods{-1};
};
[[nodiscard]] HeldGripState HeldGrip();
[[nodiscard]] u8 HeldGripKind();

// One part (0-3) of the right weapon hidden for this update (the music box's byte, ChrAsm +0x26C:
// the weapon update reads it and the ChrAsm update resets it, so it is written every update before
// the weapon update: the pose copy). False when the hide sites did not match (core/vr/bow.h: the
// Bowblade's own arrow on the string hand while the real bow is out).
bool HideRightWeaponPart(u8* chr, u32 part);

} // namespace VR::Bloodborne
