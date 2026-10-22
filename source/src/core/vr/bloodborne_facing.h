// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include "common/types.h"

// Bloodborne (v1.09 eboot) VR facing (vr.ini body_facing, core/vr/body_facing.h): the hunter's
// heading changes only with the turn stick and your own real heading, never with locomotion.
//
// Without a lock-on target the game turns the hunter toward the left stick and runs him forward,
// so walking, sprinting or rolling back turned him round while the VR view kept looking ahead
// (his back then filled the view and he faced away from doors). Locked on, it keeps him facing the
// target and moves him with its strafe / backpedal walks and runs, its 8-direction quicksteps and
// rolls. This module gives him that locomotion with the VR heading as the "target":
//
// LockTgtMan::Update (0x188E900, once per game frame, from 0x1917026 and 0x1918C78) writes the
// lock-on state every frame: locked, PlayerIns+0xF8 = 1, the lock point PlayerIns+0x100 (float4,
// world) and ChrCtrl+0x245 = 0; unlocked, +0xF8 = 0 and ChrCtrl+0x245 = 1 (the HKS env 18 "locked
// on" the behaviour script asks is !ChrCtrl+0x245; the pad manipulator turns him toward +0x100
// while +0xF8 is set, 0x1526B39). Right after each call (a patch of the two call sites), when the
// game left him unlocked, the same three fields are written as if he were locked on to a point
// 50 m ahead along the VR heading. The game's lock-on (LockTgtMan's target, the camera's lock
// flag, the reticle) is not touched: R3 still locks on to an enemy, and then the game's own lock
// turns him to it (and the heading follows him back to the view once it ends).
//
// Off (the game's own facing) while the first-person VR view is off (cinematic flat, the virtual
// screen, third person), in event animations, on ladders, in viscerals / grabs and when dead (the
// body's gate: those animations place him themselves), and with no fresh view. Every site is
// signature-checked; a mismatch leaves the game's facing and is logged.
namespace VR::Bloodborne {

// OnEbootLoaded (VR enabled): checks and patches the two call sites. Logs one line.
void InstallFacing(VAddr base, u64 size);

// The facing writes are on this frame (the last LockTgtMan update): the hunter strafes (logs,
// other modules).
[[nodiscard]] bool FacingActive();
// The hunter's heading as his physics module has it now (what W shows once the game applies it;
// the facing's idle turn writes it): radians in W's yaw convention. False when not readable.
bool PhysicsYawNow(float& yaw);

// The hunter's facing against the game's left-stick reference at the last LockTgtMan update (any
// thread; core/vr/left_locomotion.h: a stick pushed sideways or back of him is the game's strafe
// run, which never dashes). `hunter_yaw`: the VR heading while it drives him (the strafe's), else
// his own (a lock-on, body_facing=0, an animation that places him); `stick_yaw`: the view's yaw,
// which the first-person camera is given (the stick's reference). Game yaw, radians, + = right.
// `valid` false: no first-person VR view (the game's own camera) or no update yet.
struct FacingReading {
    bool valid{false};
    bool heading{false}; // the VR heading drives him
    float hunter_yaw{0.0f};
    float stick_yaw{0.0f};
    s64 steady_ns{0}; // steady_clock nanoseconds of that update
};
[[nodiscard]] FacingReading LatestFacing();

// The A+B roll (core/vr/roll_chord.h, roll_chord=1), any thread: the game keeps his facing until
// `until_ns` (steady clock nanoseconds; 0 ends it now). Not locked on, the game then turns him
// toward the stick and does its own roll instead of the quickstep the VR heading's lock point
// gives. The heading is kept through it (no restart at his yaw), so the game turns him back to it
// afterwards.
void HoldGameFacing(s64 until_ns);

} // namespace VR::Bloodborne
