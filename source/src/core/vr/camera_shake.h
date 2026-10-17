// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The game's camera shake in VR (vr_camera_shake, bloodborne_camera.cpp): pure helpers to measure
// how far a camera matrix the game built is off the orientation its own pitch and yaw give (the
// shake the game adds), and how far a camera matrix moved from the one written before it.
// Header-only, no engine dependencies (tests/test_camera_shake.cpp).

#include <algorithm>
#include <cmath>

namespace VR::Shake {

// An orientation as the game stores it: rows right, up, forward (world axes, y up).
struct Rows {
    float r[3][3]{};
};

// The game's convention (bloodborne_camera.cpp WriteOrientation): yaw + = right, pitch + = down,
// no roll.
inline Rows FromYawPitch(float yaw, float pitch) {
    const float sy = std::sin(yaw), cy = std::cos(yaw);
    const float sp = std::sin(pitch), cp = std::cos(pitch);
    Rows o;
    o.r[0][0] = cy;
    o.r[0][1] = 0.0f;
    o.r[0][2] = -sy;
    o.r[1][0] = sy * sp;
    o.r[1][1] = cp;
    o.r[1][2] = cy * sp;
    o.r[2][0] = sy * cp;
    o.r[2][1] = -sp;
    o.r[2][2] = cy * cp;
    return o;
}

// A camera's matrix (+0x10: three rows of four floats, the fourth unused).
inline Rows FromMatrix(const float* m) {
    Rows o;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            o.r[i][j] = m[i * 4 + j];
        }
    }
    return o;
}

// The angle of the rotation from one orientation to the other, degrees (0 = the same). Rows that
// are not orthonormal (a scaled or broken matrix) still give a finite angle.
inline float AngleDeg(const Rows& a, const Rows& b) {
    float trace = 0.0f;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            trace += a.r[i][j] * b.r[i][j];
        }
    }
    const float c = std::clamp((trace - 1.0f) * 0.5f, -1.0f, 1.0f);
    return std::acos(c) * 57.2957795f;
}

// Roll of an orientation about its forward axis, degrees (+ = the right side up; 0 = level).
inline float RollDeg(const Rows& a) {
    // The right row's height against the up row's: a level camera's right row has no height.
    return std::atan2(a.r[0][1], a.r[1][1]) * 57.2957795f;
}

inline float Distance(const float* a, const float* b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// How far the camera position sits off the line through its orbit pivot along the view's forward
// row (metres): the follow camera orbits on that line, so a shake that moves the camera sideways or
// up shows here (a change of the orbit distance does not).
inline float OffAxis(const float* pos, const float* pivot, const Rows& rows) {
    const float d[3] = {pos[0] - pivot[0], pos[1] - pivot[1], pos[2] - pivot[2]};
    const float* f = rows.r[2];
    const float along = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
    const float p[3] = {d[0] - along * f[0], d[1] - along * f[1], d[2] - along * f[2]};
    return std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
}

} // namespace VR::Shake
