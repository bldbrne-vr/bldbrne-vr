// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

// vr.ini eye_taa: where an eye pixel of this refresh was
// in the previous refresh's eye image, for the eyes' temporal anti-aliasing
// (host_shaders/vr_eye_taa.comp does the same per pixel; this is its CPU reference and the push
// constants' maths). Header-only (tests/test_eye_taa.cpp).
//
// An eye image is a pinhole view (OpenXR axes: -Z forward, +Y up, +X right) with intrinsics
// fx, fy, cx, cy in its own pixels (texel i's centre at i + 0.5): pixel (px, py) looks along
// ((px + 0.5 - cx) / fx, -(py + 0.5 - cy) / fy, -1).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "video_core/renderer_vulkan/vr_late_warp_math.h"

namespace Vulkan::VrEyeTaa {

using Mat3 = std::array<std::array<float, 3>, 3>; // row-major
using Vec3 = std::array<float, 3>;

struct Intrinsics {
    float fx{}, fy{}, cx{}, cy{};
};

/// Row-major rotation matrix of a unit quaternion (x, y, z, w): its columns are the rotated axes.
inline Mat3 Rotation(float x, float y, float z, float w) {
    return {{{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
             {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
             {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}}};
}

/// This eye's frame into the previous eye's: p_prev = m * p_cur + t (metres).
struct Rigid {
    Mat3 m{};
    Vec3 t{};
};

struct View {
    Mat3 axes{};
    // Preserve sub-mm head offsets even far from the game's world origin.
    std::array<double, 3> position{}; // game-world metres, not the compositor's tracking pose
};

// Embed the displayed eye in the game world, including both head motion and the late warp's
// walking/smooth-turn motion. The game's axes have opposite handedness; Between only needs
// orthonormal axes, not a quaternion, so the reflection cancels between consecutive views.
inline View WorldEye(const VrLate::Quat& capture_head, const VrLate::Vec3& render_position,
                     float base_yaw, const VrLate::Reprojection& view_to_capture,
                     float units_per_metre, float eye_offset_m) {
    const auto rs = VrLate::FromQuat(capture_head);
    const auto rv = rs * view_to_capture.rsv;
    View view{};
    for (int j = 0; j < 3; ++j) {
        const auto axis = VrLate::GameFromTracking(base_yaw, {rv.m[0][j], rv.m[1][j], rv.m[2][j]});
        view.axes[0][j] = axis.x;
        view.axes[1][j] = axis.y;
        view.axes[2][j] = axis.z;
    }
    const auto offset = VrLate::GameFromTracking(base_yaw, rs * view_to_capture.c);
    const double scale = std::max(units_per_metre, 1e-3f);
    view.position = {(static_cast<double>(render_position.x) + offset.x) / scale +
                         view.axes[0][0] * eye_offset_m,
                     (static_cast<double>(render_position.y) + offset.y) / scale +
                         view.axes[1][0] * eye_offset_m,
                     (static_cast<double>(render_position.z) + offset.z) / scale +
                         view.axes[2][0] * eye_offset_m};
    return view;
}

// One warp, for the history's continuity (VrLateWarp: the last actually rendered warp's against
// this one's).
struct Frame {
    // eye_taa_follow: the displayed eye is known in the game world (WorldEye): driven, DIBR, first
    // person, not a menu, the render position known, the current recenter space, no arm the game
    // animates. Else only today's head-moved history while the world stands still.
    bool valid{};
    bool motion{}; // late_warp_motion moved the world on with the game camera on this warp
    uint32_t turn{}, space{};
    int64_t capture_ns{}, display_ns{};
    float base_yaw{}, units_per_metre{1.0f};
    VrLate::Vec3 body{};
};

// the original eye TAA rule (f66605cf): the world stood still since that warp (the game camera neither
// walked nor turned: no late motion now or then, the same turn, recenter and heading, the
// hunter's body point within 1 mm). Only the head moved.
inline bool WorldStill(const Frame& prev, const Frame& cur) {
    return !prev.motion && !cur.motion && prev.turn == cur.turn && prev.space == cur.space &&
           std::abs(VrLate::WrapPi(cur.base_yaw - prev.base_yaw)) < 3e-4f &&
           VrLate::Length(cur.body - prev.body) < 1e-3f * std::max(cur.units_per_metre, 1e-3f);
}

// Same guards as late camera motion: never blend across a recenter, snap, camera takeover,
// teleport, time reversal or a long frame gap. Ordinary walking and smooth turns ARE continuous.
inline bool CanAccumulate(const Frame& prev, const Frame& cur) {
    const auto display_gap = cur.display_ns - prev.display_ns;
    const auto capture_gap = cur.capture_ns - prev.capture_ns;
    if (!prev.valid || !cur.valid || prev.turn != cur.turn || prev.space != cur.space ||
        cur.units_per_metre != prev.units_per_metre || !(cur.units_per_metre > 0.0f) ||
        display_gap <= 0 || display_gap > 70'000'000 || capture_gap < 0 ||
        capture_gap > 70'000'000) {
        return false;
    }
    const float moved = VrLate::Length(cur.body - prev.body) / cur.units_per_metre;
    return std::abs(VrLate::WrapPi(cur.base_yaw - prev.base_yaw)) <= 0.35f && moved <= 1.0f &&
           moved <= 25.0f * static_cast<float>(capture_gap) * 1e-9f;
}

// Which history a warp blends, and how (VrLateWarp::Record, per warp).
enum class History : uint8_t {
    None,  // this refresh only
    Head,  // moved by the tracked eye poses only (the original eye TAA): the world stood still
    World, // moved by the displayed eyes in the game world (WorldEye): head AND game camera
};
struct Choice {
    History history{History::None};
    // The world moved since the history (World only): vr_eye_taa.comp's depth-checked path, the
    // body, held items and the near field without history. Else today's filter, unchanged.
    bool moving{false};
};

// vr.ini eye_taa_follow: 1 = the history follows the game camera too (World when the pair is
// continuous, Head as today otherwise), 0 = exactly the original eye TAA (Head while the world stands
// still, else none). `have_prev`: a warp with the TAA rendered before (prev is its frame).
inline Choice Choose(bool follow, bool have_prev, const Frame& prev, const Frame& cur) {
    if (!have_prev) {
        return {};
    }
    const bool still = WorldStill(prev, cur);
    if (!follow) {
        return {still ? History::Head : History::None, false};
    }
    if (CanAccumulate(prev, cur)) {
        return {History::World, !still};
    }
    // Not a pair the game world can place (third person, a menu, an arm the game animates, a
    // game hitch): today's head-moved history while the world stands still, but never across a
    // display gap (a cinematic or a menu that stopped the warps: stale history).
    const auto display_gap = cur.display_ns - prev.display_ns;
    return {still && display_gap > 0 && display_gap <= 70'000'000 ? History::Head
                                                                  : History::None,
            false};
}

// ---- The eyes' 1/z for the TAA (eye_w, written by vr_dibr_warp.comp StoreEyeW) ----
// >= 0: a world surface's displayed-view 1/z (game units; 0 = sky).
// <= -2: -(w + 2), a surface whose history is only colour-clamped, never depth-checked: invented
//        fills (late-warp holes, DIBR fills, the plain fallback behind the body while the world
//        moves) always; the body, held items and the near field while the world stands still.
// else (-1): no history (the HUD, no surface; the body, held items and the near field while the
//        world moves).
struct EyeW {
    float w{-1.0f}; // < 0: no history
    bool clamp_only{false};
};
inline float EncodeClampOnly(float w) {
    return -(std::max(w, 0.0f) + 2.0f);
}
inline EyeW DecodeEyeW(float v) {
    if (v >= 0.0f) {
        return {v, false};
    }
    if (v <= -1.5f) {
        return {-v - 2.0f, true};
    }
    return {};
}

// The history's alpha (RGBA16F): >= 0 the surface's 1/z in metres (0: sky); HistUnknown a fill's
// (usable, colour-clamped only); HistNone never usable (body, HUD, no surface). Written by
// vr_eye_taa.comp: on a still-world warp every surface with a depth keeps it (the body too: the
// next moving warp then sees it as nearer and rejects it).
constexpr float HistUnknown = -1.0f;
constexpr float HistNone = -2.0f;

// ---- The moving path (vr_eye_taa.comp, FLAG_MOVING) ----
// Pixels a history sample moves per 1/m of depth error (the surface's parallax under this
// refresh's eye translation): q = proj(M dir + t w), dq/dw = f (t.xy + u t.z) / s with u the
// reprojected point's tangent and s ~ 1. Zero under pure rotation.
inline float ParallaxPx(const Vec3& t, float fx_prev, float ux, float uy) {
    return fx_prev * std::hypot(t[0] + ux * t[2], t[1] + uy * t[2]);
}

// A history tap is usable unless it is never usable (body, HUD) or a clear disocclusion: the
// history there is NEARER than this pixel's surface by more than `gate_px` of parallax (it hid
// this surface a refresh ago). A farther or unknown tap stays (the 3x3 colour clamp handles it,
// as the original eye TAA filter does everywhere); slopes and half-float rounding never reject.
inline bool TapUsable(float stored, float expected, float k_px, float gate_px) {
    if (stored < -1.5f) {
        return false;
    }
    if (stored < 0.0f || expected < 0.0f) {
        return true;
    }
    const float nearer = stored - expected;
    return nearer * k_px <= gate_px || nearer <= 2e-3f * std::max(stored, expected);
}

// The current 3x3 holds surfaces this refresh moves apart by more than `gate_px`: the edge cap
// (eye_taa_edge_cap) applies there.
inline bool ParallaxEdge(float w_min_m, float w_max_m, float k_px, float gate_px) {
    return (w_max_m - w_min_m) * k_px > gate_px;
}

// The 2x2 footprint's bilinear weights over the usable taps (sum 1). False: under 25% of the
// footprint usable (no history). `all_usable`: the original eye TAA's Catmull-Rom sample applies as is.
inline bool MovingWeights(const std::array<float, 4>& stored, float expected, float k_px,
                          float gate_px, float x, float y, std::array<float, 4>& weights,
                          bool& all_usable) {
    weights = {(1 - x) * (1 - y), x * (1 - y), (1 - x) * y, x * y};
    float sum = 0.0f;
    all_usable = true;
    for (int i = 0; i < 4; ++i) {
        if (!TapUsable(stored[i], expected, k_px, gate_px)) {
            all_usable = all_usable && weights[i] == 0.0f;
            weights[i] = 0.0f;
        }
        sum += weights[i];
    }
    if (sum < 0.25f) {
        return false;
    }
    for (auto& w : weights) {
        w /= sum;
    }
    return true;
}

/// From the two eye poses (rotation rc / rp, position pc / pp, tracking space, metres).
inline Rigid Between(const Mat3& rp, const Vec3& pp, const Mat3& rc, const Vec3& pc) {
    Rigid r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            float acc = 0.0f;
            for (int k = 0; k < 3; ++k) {
                acc += rp[k][i] * rc[k][j];
            }
            r.m[i][j] = acc;
        }
    }
    const Vec3 d{pc[0] - pp[0], pc[1] - pp[1], pc[2] - pp[2]};
    for (int i = 0; i < 3; ++i) {
        r.t[i] = rp[0][i] * d[0] + rp[1][i] * d[1] + rp[2][i] * d[2];
    }
    return r;
}

inline Rigid Between(const View& prev, const View& cur) {
    const Vec3 delta{static_cast<float>(cur.position[0] - prev.position[0]),
                     static_cast<float>(cur.position[1] - prev.position[1]),
                     static_cast<float>(cur.position[2] - prev.position[2])};
    return Between(prev.axes, {}, cur.axes, delta);
}

/// Where this eye's pixel (px, py) with the surface at depth `z_m` metres (0: at infinity) is in
/// the previous eye's image (texel coordinates: centres at integers, the history's CR sample
/// position). False: behind the previous eye.
inline bool Previous(const Rigid& r, const Intrinsics& cur, const Intrinsics& prev, float px,
                     float py, float z_m, float& qx, float& qy) {
    const Vec3 dir{(px + 0.5f - cur.cx) / cur.fx, -(py + 0.5f - cur.cy) / cur.fy, -1.0f};
    Vec3 p{};
    for (int i = 0; i < 3; ++i) {
        const float rot = r.m[i][0] * dir[0] + r.m[i][1] * dir[1] + r.m[i][2] * dir[2];
        p[i] = z_m > 0.0f ? rot * z_m + r.t[i] : rot;
    }
    if (!(p[2] < -1e-5f)) {
        return false;
    }
    qx = prev.cx + prev.fx * (p[0] / -p[2]) - 0.5f;
    qy = prev.cy - prev.fy * (p[1] / -p[2]) - 0.5f;
    return true;
}

} // namespace Vulkan::VrEyeTaa
