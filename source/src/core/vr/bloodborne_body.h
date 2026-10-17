// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "core/vr/grab_camera.h"
#include "core/vr/vr_session.h"

namespace VR::Interactions { class NativeCameraRuntime; }

// Bloodborne (v1.09 eboot) VR body: the hunter's arms on your controllers, his torso turned and
// moved under your eyes, his head hidden (first person; VR_BODY_DRYRUN solves everything and
// writes nothing).
//
// The hook wraps the main player's pose copy, PlayerIns vtable +0xE0 (0x18BE020, both PlayerIns
// vtables): it copies the behaviour graph's local pose into the character's hkaPose on the loop
// thread, before this frame's camera and before anything reads the pose (syncModelSpace in
// vt+0xF0, the weapon parts, dummy polys, skinning). The slots hold their value only after
// relocation, so they are swapped from Linker::Execute right after RelocateAllImports
// (OnEbootRelocated), or lazily from the first render-camera tick if that call never came.
// VR_BODY_HOOK=code patches the function's tail instead (installed from OnEbootLoaded), =off
// installs nothing. Every site, slot and the skeleton (108 bones, parents, names) are checked
// first; a mismatch turns the body off for the session with one log line.
//
// Each rendered frame the render-camera hook publishes the view anchor (core/vr/body_frame.h); the
// next pose copy maps the newest hand sample from the OpenXR thread (core/vr/body_channel.h) into
// the character's model space with it. Right after a fresh copy of the main player's pose (the
// loop thread; everything it shares with other threads is a seqlock, an atomic or the snapshot
// mutex) the body is solved on a host copy of the 108 locals (core/vr/body_solver.h: torso yaw,
// upper body under the eye, both arms, hiding) and only the bones it changed are written back.
// Who owns each arm comes from the animation the game is playing and the throw, ladder, event and
// HP state, read in the same place (core/vr/anim_classes.h): the weights ease over
// arm_blend_in_ms / arm_blend_out_ms. Nothing is written while the body is not driven (no
// first-person follow camera, cinematic flat mode, the virtual screen) or a guard fails.
namespace VR::Bloodborne {

// OnEbootLoaded (VR enabled; pre-relocation, code patches only): remembers the eboot; with
// VR_BODY_HOOK=code installs the tail patch.
void BodyEbootLoaded(VAddr base, u64 size);

// Linker::Execute, right after RelocateAllImports (emulator thread, before the game runs): swaps
// the pose-copy vtable slots.
void OnEbootRelocated(VAddr base, u64 size);

// The pose-copy wrapper is installed (it also ticks the held vial, core/vr/bloodborne_held.h).
[[nodiscard]] bool PoseHookInstalled();

// Validated, unanimated character root at the render tick. Includes terrain height. `yaw`
// (optional): his heading (radians, Body::ChrYaw).
[[nodiscard]] bool BodyRootHeight(float& height, float* yaw = nullptr);

// Fresh, untouched animated eye during an enemy grab; or (`own_throw`) during a freehand throw
// the hunter performs, his facing and his victim's root. False outside those animations.
// Called only by the render hook with its existing camera runtime/sequence.
[[nodiscard]] bool BodyGrabEye(s64 now, VR::GrabCamera::Sample& sample,
                               VR::Interactions::NativeCameraRuntime& camera);

// Render-camera hook (loop thread), once per rendered frame after the camera is placed: `result`
// and `position` (the final render camera position, game units) of a driven frame, `fp_follow` =
// the first-person follow camera. Inactive frames (cutscenes, the aim camera, the virtual screen,
// cinematic flat mode) pass nullptr. Publishes the anchor; W-drift and FK checks in log mode.
void BodyRenderTick(const VR::CameraOverride* result, const float* position, bool fp_follow);

} // namespace VR::Bloodborne
