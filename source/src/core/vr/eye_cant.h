// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>

// Canted displays (Pimax; a user's report, 2026-11-01: "slightly cross-eyed"): each eye's display
// is turned outward, so the runtime locates each view rotated from the head, its FOV measured
// around that rotated axis. The game is rendered with parallel eyes (native stereo shares the game
// camera's rotation; DIBR builds both eyes from one image), so:
//   - the render, its crops, the culling frustum and the foveation centre use each eye's view seen
//     from the head (Cover, AxisTangent), so the parallel images hold all of what each eye shows;
//   - the late warp turns each finished parallel eye into its own rotated view (Matrix, in
//     vr_late_cant.comp) and submits it with the eye's real pose and located FOV, what the runtime
//     expects whatever it does with a parallel submission.
// A pure rotation about the eye's own centre: exact for every depth (no parallax).
// Generic like eye_fov_cache.h (any Fov with angle_left/right/up/down, any Quat with x, y, z, w)
// so it is tested without an OpenXR or Vulkan device.
namespace VR::Cant {

// Below this an eye counts as parallel (0.5 degrees: float noise or a calibration's hair of turn
// in a straight headset's located pose never switches anything on; canted displays turn several
// degrees).
constexpr float MinAngle = 0.5f * 3.14159265f / 180.0f;

struct V3 {
    float x, y, z;
};

// The rotation's angle (radians, 0 .. pi).
template <class Q>
float Angle(const Q& q) {
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(n > 0.0f)) {
        return 0.0f;
    }
    return 2.0f * std::acos(std::clamp(std::abs(q.w) / n, 0.0f, 1.0f));
}

// Turned by more than MinAngle: the vector part against sin(MinAngle / 2) of the norm (no
// transcendental call; this runs per camera update and per XR frame on every headset).
template <class Q>
bool Canted(const Q& q) {
    constexpr float s = MinAngle * 0.5f; // sin(x) ~ x this small
    const float v = q.x * q.x + q.y * q.y + q.z * q.z;
    return v > s * s * (v + q.w * q.w);
}

// The 3x3 rotation matrix of `q` (row major: out = M * v).
template <class Q>
std::array<float, 9> Matrix(const Q& q) {
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    const float s = n > 0.0f ? 1.0f / n : 0.0f;
    const float x = q.x * s, y = q.y * s, z = q.z * s, w = n > 0.0f ? q.w * s : 1.0f;
    return {1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y - z * w),        2.0f * (x * z + y * w),
            2.0f * (x * y + z * w),        1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z - x * w),
            2.0f * (x * z - y * w),        2.0f * (y * z + x * w),        1.0f - 2.0f * (x * x + y * y)};
}

inline V3 Apply(const std::array<float, 9>& m, V3 v) {
    return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z,
            m[6] * v.x + m[7] * v.y + m[8] * v.z};
}

// The eye's view seen from the head: the FOV (head frame) that holds the eye's located FOV
// (`eye`, its own frame) for an eye turned `eye_to_head` from the head. A rotated frustum lands on
// the head's image plane as the quadrilateral of its four corner rays, so their bounding box is
// the cover. `eye` itself when the eye is parallel, or when a corner would turn behind the head's
// image plane (no cover exists).
template <class Fov, class Q>
Fov Cover(const Fov& eye, const Q& eye_to_head) {
    if (!Canted(eye_to_head)) {
        return eye;
    }
    const auto m = Matrix(eye_to_head);
    const float tl = std::tan(eye.angle_left), tr = std::tan(eye.angle_right);
    const float tu = std::tan(eye.angle_up), td = std::tan(eye.angle_down);
    float x0 = 0.0f, x1 = 0.0f, y0 = 0.0f, y1 = 0.0f;
    bool first = true;
    for (const float tx : {tl, tr}) {
        for (const float ty : {tu, td}) {
            const V3 d = Apply(m, {tx, ty, -1.0f});
            if (!(d.z < -1e-3f)) {
                return eye;
            }
            const float px = d.x / -d.z;
            const float py = d.y / -d.z;
            x0 = first ? px : std::min(x0, px);
            x1 = first ? px : std::max(x1, px);
            y0 = first ? py : std::min(y0, py);
            y1 = first ? py : std::max(y1, py);
            first = false;
        }
    }
    Fov out = eye;
    out.angle_left = std::atan(x0);
    out.angle_right = std::atan(x1);
    out.angle_up = std::atan(y1);
    out.angle_down = std::atan(y0);
    return out;
}

// Where the eye's own forward axis lands on the head's image plane (tangents: x right, y up). The
// lens centre of a canted eye, for the foveation map; {0, 0} for a parallel eye.
template <class Q>
std::array<float, 2> AxisTangent(const Q& eye_to_head) {
    if (!Canted(eye_to_head)) {
        return {0.0f, 0.0f};
    }
    const V3 d = Apply(Matrix(eye_to_head), {0.0f, 0.0f, -1.0f});
    if (!(d.z < -1e-3f)) {
        return {0.0f, 0.0f};
    }
    return {d.x / -d.z, d.y / -d.z};
}

// The pixel size of a canted eye's own image: the located FOV `eye` at the density the parallel
// image of its cover has (`cover_px` pixels over `cover`), at least 16 a side.
template <class Fov>
std::array<unsigned, 2> Extent(const Fov& eye, const Fov& cover, std::array<unsigned, 2> cover_px) {
    const auto span = [](float a, float b) { return std::tan(b) - std::tan(a); };
    const float cw = span(cover.angle_left, cover.angle_right);
    const float ch = span(cover.angle_down, cover.angle_up);
    if (!(cw > 0.0f) || !(ch > 0.0f)) {
        return cover_px;
    }
    const float w = static_cast<float>(cover_px[0]) * span(eye.angle_left, eye.angle_right) / cw;
    const float h = static_cast<float>(cover_px[1]) * span(eye.angle_down, eye.angle_up) / ch;
    return {std::max(16u, static_cast<unsigned>(std::lround(w))),
            std::max(16u, static_cast<unsigned>(std::lround(h)))};
}

} // namespace VR::Cant
