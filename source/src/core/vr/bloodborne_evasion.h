// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "core/vr/vr_session.h"

// Bloodborne (v1.09 eboot) physical evasion (vr.ini physical_evasion=1): your real lean, step and
// duck move the hunter's hurtboxes (the 16 ragdoll capsules the attack casts test), times
// evasion_gain, so a strike aimed at where you were misses. No dodge animation, no i-frames, no
// buttons. The model is core/vr/evasion.h.
//
// The hurtboxes are hard-keyframed to the animation pose every frame by RagdollIns::
// KeyframeToAnimPose (0x180F450); a detour at 0x180F578, inside its per-body loop, adds each
// body's offset to the keyframe target position right before 0xB68A40 applies it, for the main
// player's ragdoll only (one compare per body for every other character). The render-camera hook
// computes the offsets once per rendered frame from the camera's neck offset and the VR body's
// status (animation gates, the UpperPos shift already in the pose, arm IK weights) and publishes
// them (seqlock) for the keyframe call, which swaps in the shift of the pose it keyframes. Fixed-
// target attacks and grabs (throws) cannot be leaned out of, as in vanilla.
//
// Walls (evasion_wall=1): while you lean, the render-camera hook calls the game's own collision
// queries (the follow camera's sphere cast, on the ragdolls' map-only layer; the ragdoll ray as the
// fallback) from the root and from the chest and head capsules where they are, and the model keeps
// the lean evasion_wall_margin_m short of the first wall. The hit log (VR_EVADE_HITLOG, and the
// near-miss haptic of evasion_feedback) reads every enemy attack cast near the player through two
// read-only hooks in AttackHitManager's cast and tests it against the hurtboxes with and without
// the lean (core/vr/evasion_hitgeom.h).
//
// VR_EVADE_HOOK=off installs nothing. VR_EVADE_LOG=1 logs once a second (lean, gate, offsets,
// the wall, keyframe targets vs the bodies' positions), 2 adds every frame for 2 s after the MARK
// combo, 3 every frame. VR_EVADE_WALL=sphere|ray|off picks the wall casts. VR_EVADE_HITLOG=1 logs
// each attack near the player and a summary every second, 2 also every cast.
namespace VR::Bloodborne {

// OnEbootLoaded (VR enabled; before guest threads exist): checks the site and installs the patch.
void EvasionEbootLoaded(VAddr base, u64 size);

// Render-camera hook (loop thread), once per rendered frame: `result` of a driven first-person
// frame, nullptr otherwise (the hurtboxes then fade back).
void EvasionRenderTick(const VR::CameraOverride* result, bool fp_follow);

[[nodiscard]] bool IsEvasionInstalled();

// The game's own sphere cast (its follow camera's) against the static map, the layers the ragdolls
// collide with (characters never): a sphere of `radius` (m) from `from` to `to`, its first
// contact's centre, the surface normal there and the fraction of the way. False: nothing in the
// way, a start inside the map, no world, or the cast's bytes do not match. Read-only (the main
// player's update: core/vr/weapon_wall.h). Installed whatever physical_evasion says.
bool CastMapSphere(const float from[3], const float to[3], float radius, float hit[3],
                   float normal[3], float& fraction);

} // namespace VR::Bloodborne
