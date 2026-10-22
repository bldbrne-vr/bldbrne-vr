// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include "common/types.h"
#include "core/vr/vr_session.h"

// Bloodborne (v1.09 eboot) dodge comfort camera (core/vr/motion_camera.h). Once per main-player
// update (hook site A, shared with the instant injection and the cinematic detection) it reads the
// signals that tune the camera's time constants and publishes them under a mutex: site A usually
// runs on a job worker thread, the render-camera hook on the loop thread. It only reads memory.
//
// Signals, from the module bag = [PlayerIns+0x3B0]:
//  - SpEffect 392 / 393 ("dodge active", quickstep / roll / backstep) in the SpEffect list
//    [PlayerIns+0x1C8] (first node +0x08, id +0x40, next +0x58), as HKS env 56 walks it;
//  - the newest motion clip among the pending TimeAct entries ([bag+0x18]);
//  - the physics module's "landed" byte [[bag+0x68]+0x212] (HKS env 20; the follow camera's pivot
//    is the same module's +0x1E0);
//  - the HKS ladder action state [[[PlayerIns+0x58]+0x38]+0x60] (-1 off the ladder);
//  - logging only: the invincibility bit [[bag+0x08]+0x1BC] & 2 and the HKS action state's
//    "ignore gravity" / "no fall animation" bits (+0x3C & 0x10 / 0x20).
// The engine code behind each signal is signature-checked at install, the module vtables (and the
// action state's owner) on the first update and on rising edges; a failed check turns that signal
// off for the session. Without any signal the camera still works (Ground everywhere).
namespace VR::Bloodborne {

// Install time (VR first person, dodge camera, coat boost or VR_DODGE_LOG): checks the engine
// code behind each signal and logs which signals are on. `site_ok` / `main_ok`: the player update
// hook's own checks. Returns true when the hook should read the signals.
bool InitMotion(VAddr base, u64 size, bool site_ok, bool main_ok);

// The main player's update (hook site A, r13 = PlayerIns*; usually a job worker thread, one
// update at a time).
void MotionUpdate(u8* player);
// Fresh unmodified Master bone height above the reference pose, in game units.
void PublishJumpLift(float lift, s64 now);

// The sprint leap (core/vr/left_locomotion.h, left_scheme=2), any thread: SpEffect 390 (the
// game's jump window, 0.2 s after a dash: a roll request then jumps) and 391 (dashing) at the main
// player's last update. `valid` false: never read (dodge_camera=0, third person, a failed check).
struct DashEffects {
    bool valid{false};
    bool window{false}; // SpEffect 390
    bool dash{false};   // SpEffect 391
    bool dodge{false};  // SpEffect 392 or 393: a quickstep, roll or backstep (the A+B roll)
    s64 steady_ns{0};   // steady_clock nanoseconds of that update
    // The jump (the latch's hold after a leap): its clip (a000_007800; or the fall and landing it
    // turns into off a ledge, 8050 / 81xx / 82xx) among the TimeAct entries the behaviour pushed
    // (the game pushes the playing clip again and again while it runs).
    // `jump_valid` false: the clips are not read. `jump_read_ns`: the last update that read them,
    // `jump_ns`: the last one with the jump (0: never), `jump_updates`: updates with it so far.
    bool jump_valid{false};
    s64 jump_read_ns{0};
    s64 jump_ns{0};
    u32 jump_updates{0};
};
DashEffects LatestDashEffects();

// The plunge (core/vr/plunge.h), any thread: the physics module's landed byte and height at the
// main player's last update. `valid` false: not read (dodge_camera=0, third person, a failed
// check): no plunges.
struct AirSample {
    bool valid{false};
    bool grounded{true};
    float y{0.0f};    // physics position height (game units, metres)
    s64 steady_ns{0}; // steady_clock nanoseconds of that update
};
AirSample LatestAir();

// Any thread: a fresh ladder state or climbing clip, using the motion gate's death/respawn
// latch. Updated at the player hook even while a ladder is shown on the cinematic flat screen.
// False with stale/missing motion signals. The sprint toggle must not judge ladders as dashes.
bool LadderActive();

// Any thread: steady-clock time of the current climb's observed mount. Zero without a mount,
// off the ladder, after death, or with stale/missing signals. Used for the brief coat fade.
s64 LatestLadderStartNs();

// ---- Render-camera hook (loop thread), own first person in the follow camera -------------------
struct EyeTick {
    VR::Vec3 target{}; // physics pivot - chrOrgOffset.y + eye height (game units)
    VR::Vec3 pivot{};  // logging: the follow camera's pivot
    VR::Vec3 org{};    // logging: chrOrgOffset
    bool pair_first{true};
    s32 eye{-1}; // logging: this tick's stereo eye (-1 = mono)
    s64 now_ns{0};
};

// dodge_camera=1: one comfort camera tick. Returns the eye base: before the forward offset, the
// stereo eye and 6DoF (head movement is never filtered). `reset`: the camera jumps to the target
// ("inactive": the render hook did not drive it since the last tick; "path": the last tick used the
// old camera; nullptr = none).
VR::Vec3 ComfortEye(const EyeTick& tick, const char* reset);

// dodge_camera=0 with VR_DODGE_LOG set: the old camera's eye base, for the A/B statistics.
void LegacyEye(const EyeTick& tick, const VR::Vec3& base);

} // namespace VR::Bloodborne
