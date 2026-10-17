// SPDX-License-Identifier: GPL-2.0-or-later

// Render pitch bias (vr.ini render_pitch_bias_deg, R4 of documents/vr-resolution-hypothesis.md).
//
// A headset eye sees further down than up (Quest 3: 44 deg up, 55 down), but the game renders one
// symmetric view: its half-height covers the larger of the two plus the margin, so the render
// spends its rows on room above the eyes that the late warp rarely needs (your log: 17 deg there,
// up to 12 used) while the bottom runs out (6 deg there, up to 14 used). With the bias the game's
// RENDER camera looks `bias` lower than your head: the same rows cover the eyes with the room
// shared between the top and the bottom, so a smaller vertical FOV (more pixels per degree) still
// leaves both edges what your head actually uses.
//
// Nothing you see moves: every frame's camera record carries the render camera's own orientation
// (the head pitch minus the bias), and everything that re-sees a frame from your head (the late
// warp, the eye TAA's history, the UI's rest quad, the compositor) starts from that record, so the
// bias is undone exactly; everything that follows your head (the body, the arms, the hands, the
// gun and throw aim, the near clip, the HUD, the vignettes) keeps the head's own pitch. Only the
// late warp can undo it, so the camera applies it only while the late warp is showing the DIBR
// eyes (and never on the menu panel's frozen frames).
//
// Header-only and dependency-free (unit tests: tests/test_render_tilt.cpp).

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace VR::Tilt {

constexpr float Pi = 3.14159265358979f;
constexpr float Deg = Pi / 180.0f;
// The render camera's pitch never reaches straight up or down (the game's yaw / pitch frame).
constexpr float MaxRenderPitch = 87.0f * Deg;
// ComputeVfov's limits of the vertical half-angle (20 .. 70 deg).
constexpr float MinHalfVfov = 20.0f * Deg;
constexpr float MaxHalfVfov = 70.0f * Deg;
// The largest bias the setting takes (degrees).
constexpr float MaxBiasDeg = 15.0f;

// The margin (degrees) the render's FOV is widened by around the eyes: fov_margin_deg, and while
// the bias applies (`bias` != 0) at least `min_margin_deg` (render_pitch_min_margin_deg, default 8
// with the default bias of 5, approved in the TILT 2560 review, 2026-10-07). The bias moves room
// from the top to the bottom, so a margin sized for the unbiased render leaves too little on both
// edges (Quest 3, bias 5 with margin 6: 7 deg up / 6 down, against 17 / 6 without the bias); with
// 8: 9 up / 8 down, sized from your head motion. A larger fov_margin_deg is kept as it is; no bias
// = fov_margin_deg alone.
inline float MarginDeg(float margin_deg, float min_margin_deg, float bias) {
    return bias != 0.0f ? std::max(margin_deg, min_margin_deg) : margin_deg;
}

// The render camera's pitch (radians, + = up, the camera record's convention) for a head pitched
// `head_pitch` (after the pitch limit) with the render looking `bias` lower.
inline float RenderPitch(float head_pitch, float bias) {
    return std::clamp(head_pitch - bias, -MaxRenderPitch, MaxRenderPitch);
}

// The widest horizontal tangent of an eye edge at tangent `tan_x` (head frame, its rows from
// tan_down .. tan_up, tan_down < 0) in a render pitched `bias` below the head: a pitch keeps every
// row a row (a row at tangent ty sits at tan(atan(ty) + bias)) but scales its x by
// 1 / (cos bias - ty sin bias), widest at the top row when the render looks down (keystone).
inline float KeystoneTanX(float tan_x, float tan_up, float tan_down, float bias) {
    const float c = std::cos(bias), s = std::sin(bias);
    const float denom = std::min(c - tan_up * s, c - tan_down * s);
    return tan_x / std::max(denom, 1e-3f);
}

// The vertical half-angle of the symmetric render (radians) that covers both eyes (`eyes`: two
// FOVs with angle_left / angle_right / angle_up / angle_down, radians, OpenXR convention) widened
// by `margin` (radians), for a render `aspect` wide and pitched `bias` below the head:
//  - vertically: the higher of the eyes' top + bias and bottom - bias, plus the margin, and never
//    less than the eyes themselves (the late warp's view at your head uses the render's
//    intrinsics, so its crop must fit them unpitched);
//  - horizontally: the outer edges at their widest row (KeystoneTanX) plus the margin.
// bias = 0 is ComputeVfov's formula before the bias, bit for bit. Clamped to 20 .. 70 deg.
template <typename EyeFovs>
float CoverHalfVfov(const EyeFovs& eyes, float aspect, float bias, float margin) {
    float up = 0.0f, down = 0.0f, half_h = 0.0f;
    for (const auto& fov : eyes) {
        up = std::max(up, fov.angle_up);
        down = std::max(down, -fov.angle_down);
        half_h = std::max({half_h, fov.angle_right, -fov.angle_left});
    }
    float half_v = std::max(up, down);
    if (bias != 0.0f) {
        const float pitched = std::max(up + bias, down - bias);
        half_h = std::atan(KeystoneTanX(std::tan(half_h), std::tan(up), -std::tan(down), bias));
        const float from_h =
            std::atan(std::tan(std::min(half_h + margin, 1.5f)) / std::max(aspect, 0.1f));
        return std::clamp(std::max({pitched + margin, half_v, from_h}), MinHalfVfov, MaxHalfVfov);
    }
    const float from_h =
        std::atan(std::tan(std::min(half_h + margin, 1.5f)) / std::max(aspect, 0.1f));
    return std::clamp(std::max(half_v + margin, from_h), MinHalfVfov, MaxHalfVfov);
}

// The late warp's room at rest (degrees: how far your head can turn past an eye's edge before the
// edge leaves the render) at the binding point of each edge: 0 the outer-left (left eye), 1 the
// outer-right (right eye), 2 the top, 3 the bottom (both eyes). Head roll 0; each edge is sampled
// at its two ends and its middle, as the presenter's room log does.
struct Rooms {
    std::array<float, 4> edge{};      // the smallest of the edge's three points
    std::array<float, 2> side_mid{};  // outer-left / outer-right at the eye's middle row
};

// A direction (x, y, -1) of the head's frame, seen from a render pitched `bias` below it: its
// tangents (x', y') on the render's image plane.
inline std::array<float, 2> ToRender(float x, float y, float bias) {
    const float c = std::cos(bias), s = std::sin(bias);
    const float z = c - y * s; // -z of the direction in the render's frame
    return {x / z, (y * c + s) / z};
}

template <typename EyeFovs>
Rooms RestRooms(const EyeFovs& eyes, float vfov, float aspect, float bias) {
    const float ty = std::tan(0.5f * vfov);
    const float border[2] = {ty * aspect, ty};
    Rooms r{};
    // One edge: axis 0 = a vertical edge at x tangent `at` (its rows lo .. hi), 1 = a horizontal
    // edge at y tangent `at` (its columns lo .. hi); `sign`: outwards.
    const auto room = [&](unsigned axis, float sign, float at, float lo, float hi) {
        float least = 1e9f;
        for (const float u : {lo, 0.5f * (lo + hi), hi}) {
            const auto t = axis == 0 ? ToRender(at, u, bias) : ToRender(u, at, bias);
            least = std::min(least, (std::atan(border[axis]) - std::atan(sign * t[axis])) / Deg);
        }
        return least;
    };
    const auto& l = eyes[0];
    const auto& rt = eyes[1];
    r.edge[0] =
        room(0, -1.0f, std::tan(l.angle_left), std::tan(l.angle_down), std::tan(l.angle_up));
    r.edge[1] =
        room(0, 1.0f, std::tan(rt.angle_right), std::tan(rt.angle_down), std::tan(rt.angle_up));
    r.edge[2] = std::min(
        room(1, 1.0f, std::tan(l.angle_up), std::tan(l.angle_left), std::tan(l.angle_right)),
        room(1, 1.0f, std::tan(rt.angle_up), std::tan(rt.angle_left), std::tan(rt.angle_right)));
    r.edge[3] = std::min(
        room(1, -1.0f, std::tan(l.angle_down), std::tan(l.angle_left), std::tan(l.angle_right)),
        room(1, -1.0f, std::tan(rt.angle_down), std::tan(rt.angle_left),
             std::tan(rt.angle_right)));
    r.side_mid[0] =
        (std::atan(border[0]) - std::atan(-ToRender(std::tan(l.angle_left), 0.0f, bias)[0])) / Deg;
    r.side_mid[1] =
        (std::atan(border[0]) - std::atan(ToRender(std::tan(rt.angle_right), 0.0f, bias)[0])) /
        Deg;
    return r;
}

// Whether this camera update renders pitched: the bias is set, the eyes are DIBR's (one centre
// camera, warped into both eyes), it is not a menu panel's frozen frame, and the late warp is
// showing the eyes (it is what undoes the bias; a frame shown without it would miss the bias's
// share of its top edge).
inline bool BiasWanted(float bias_deg, bool dibr, bool menu, bool late_warp_showing) {
    return bias_deg != 0.0f && dibr && !menu && late_warp_showing;
}

} // namespace VR::Tilt
