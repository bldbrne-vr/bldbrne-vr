// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <cmath>

// The 6DoF room (vr.ini positional_limit_m, positional_limit_down_m): how far your real head may
// move the game camera away from the recentered head (the 6DoF origin; with a height, your
// standing eye height over the floor). Beyond it the camera stops and the world moves with your
// head, so leaning can never put the eye through a wall.
//
// Until 2026-10-08 the room was a sphere of positional_limit_m = 0.5 m. A real duck also leans
// forward (the tester's playtest: the neck at (-0.10, -0.56, -0.37) m, 0.68 m from the origin), so
// the sphere scaled it back to 0.41 m of drop, and a straight duck stopped at 0.50 m; the tester:
// "I want to be able to duck a little lower ... like 15cm more". Now the room is the sphere above
// the origin's horizontal plane and a cylinder of the same radius below it, `down_m` deep: up,
// sideways and forward are unchanged (the sphere's 0.5 m, the wall guard), and a duck keeps the
// camera on your head down to down_m whatever the lean: that playtest duck is followed all the
// way (0.56 m of drop, 15 cm more). down_m was 0.65 m (a straight duck 15 cm further than the
// sphere); the same evening the request was "can we make this possible to go 15cm more? (so up to
// 30 in total)": 0.80 m by default, 30 cm more than the sphere.
//
// The clamp is the nearest point of that room (convex: the hemisphere and the cylinder share
// their rim), so it is continuous: on and above the plane it is the old sphere's radial scale
// bit for bit, below it the horizontal part stays within limit_m and the drop within down_m
// separately. down_m = 0 is the old sphere all round (bit for bit); limit_m <= 0: the head never
// moves the camera (positional=0).
//
// The floor: the camera's drop is the hunter's eye going down from fp_eye_height_m over his feet,
// so DownLimit keeps it FloorMarginM over them (0.80 m leaves 0.78 m at the tester's 1.58 m eye; the
// guard only binds with a far larger positional_limit_down_m, 1.23 m there, or a far lower eye,
// under 1.15 m). Your real head
// never goes under your real floor, so the real floor needs no clamp of its own.
//
// Pure (no emulator or OpenXR dependencies): the camera tick (core/vr/vr_session.cpp), the late
// warp (renderer_vulkan/vr_late_warp_math.h CameraPoint) and tests/test_head_room.cpp share it.
namespace VR::HeadRoom {

// The lowest the hunter's eye may go over his feet (metres): his chin and the near plane (0.1 m)
// stay clear of the floor.
inline constexpr float FloorMarginM = 0.35f;

/// The downward room the camera uses (metres below the origin; the camera record's
/// head_offset_down_m): `down_m` (positional_limit_down_m) kept FloorMarginM over the hunter's
/// feet (`eye_height_m`: fp_eye_height_m; <= FloorMarginM: no guard). 0 = the old sphere (down_m
/// <= 0, or the head never moves the camera: limit_m <= 0).
inline float DownLimit(float limit_m, float down_m, float eye_height_m) {
    if (!(limit_m > 0.0f) || !(down_m > 0.0f)) {
        return 0.0f;
    }
    const float floor_room = eye_height_m - FloorMarginM;
    return floor_room > 0.0f ? std::min(down_m, floor_room) : down_m;
}

/// The deepest drop the camera follows (metres): the down room, or the sphere's radius without
/// one (physical evasion's duck stays within it, as its lean stays within limit_m).
inline float MaxDrop(float limit_m, float down_limit_m) {
    return down_limit_m > 0.0f ? down_limit_m : std::max(limit_m, 0.0f);
}

/// Physical evasion's counted duck cap (core/vr/bloodborne_evasion.cpp DuckCap): `duck_m`
/// (evasion_duck_m, 0.75 by default) within the camera's deepest drop (DownLimit's room for
/// positional_limit_m / positional_limit_down_m / fp_eye_height_m, MaxDrop).
inline float DuckCap(float duck_m, float limit_m, float down_m, float eye_height_m) {
    return std::min(duck_m, MaxDrop(limit_m, DownLimit(limit_m, down_m, eye_height_m)));
}

/// Clamps the head's offset from the origin (x right, y up, z back; metres) into the room:
/// `limit_m` up, sideways and forward, `down_m` (DownLimit; 0 = the sphere) below the origin.
inline void Clamp(float& x, float& y, float& z, float limit_m, float down_m, bool roomscale = false) {
    const float limit = std::max(limit_m, 0.0f);
    if (!(limit > 0.0f)) {
        x = y = z = 0.0f;
        return;
    }
    // Room-scale walking needs the full horizontal displacement, including past the old
    // lean radius. Keep the vertical/floor limits, independently of horizontal travel.
    if (roomscale) {
        y = std::clamp(y, -(down_m > 0.0f ? down_m : limit), limit);
        return;
    }
    if (!(down_m > 0.0f) || y >= 0.0f) {
        // The sphere (the code before 2026-10-08, bit for bit).
        const float length = std::sqrt(x * x + y * y + z * z);
        if (length > limit && length > 0.0f) {
            const float k = limit / length;
            x *= k;
            y *= k;
            z *= k;
        }
        return;
    }
    // Below the origin: the cylinder (the horizontal part and the drop each within its own limit).
    const float horizontal = std::sqrt(x * x + z * z);
    if (horizontal > limit) {
        const float k = limit / horizontal;
        x *= k;
        z *= k;
    }
    y = std::max(y, -down_m);
}

} // namespace VR::HeadRoom
