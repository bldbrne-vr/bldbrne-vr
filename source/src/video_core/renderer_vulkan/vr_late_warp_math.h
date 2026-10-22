// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

// Late warp (vr.ini late_warp, renderer_vulkan/vr_late_warp.h) math shared by VrLateWarp, its
// reprojection shader (host_shaders/vr_late_reproject.comp) and the CPU reference / tests.
// Header-only and Vulkan-free on purpose so a test can include it without the renderer.
//
// Model. A DIBR capture is the game's centre frame S (colour + depth) rendered with one camera
// record: orientation Q_s (head yaw, pitch, roll) at the camera point C_s (tracking space). Every XR
// frame the late warp renders the same scene again from V: the head at that frame's predicted
// display time (orientation Q_v, the camera point C_v the game would render from with that head:
// CameraPoint). The world is taken as rigid between the two (only the head moved). V has S's
// intrinsics, so each eye's crop of V is the crop CopyFrame submits, and both eyes are made from V
// by the existing DIBR warp (vr_dibr_warp.comp, every source column looked up through V's map into
// S): stereo along V's own right axis, head roll included.
//
// V pixel p is the ray d = R_s^T R_v (x_t, y_t, -1) in S's camera frame (x right, y up, looking
// down -z), V's centre c = R_s^T (C_v - C_s) * units_per_metre (game units). The ray's point whose
// source inverse depth (1/z in S, game units) is r projects into S at exactly
//     q(r) = q0 + r * g,   q0 = (cx + fx dn.x, cy - fy dn.y),   dn = d / -d.z,
//     g = (fx e.x, -fy e.y),   e = c + c.z * dn   (e.z = 0),
// a straight line in S linear in r (the epipolar line; q0 is the rotation-only point at infinity),
// and its inverse depth in V is w(r) = r * (-d.z) / (1 + r * c.z). The surface V sees is the
// nearest root of W(q(r)) = r, found by scanning r from the near end down (March), as the 1D DIBR
// scan does (it is the case R = I, c = (+-b, 0, 0): g = (+-K, 0)).

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <type_traits>

#include "core/vr/head_room.h"
#include "video_core/renderer_vulkan/vr_stereo_warp_math.h"

namespace Vulkan::VrLate {

struct Vec3 {
    float x{}, y{}, z{};
};

struct Quat {
    float x{}, y{}, z{}, w{1.0f};
};

/// Row-major 3x3 matrix.
struct Mat3 {
    float m[3][3]{};
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec3 operator-(const Vec3& a, const Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vec3 operator*(const Vec3& a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}
inline float Dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline float Length(const Vec3& a) {
    return std::sqrt(Dot(a, a));
}

/// The rotation of unit quaternion q (camera axes -> tracking axes for a camera's orientation).
inline Mat3 FromQuat(const Quat& q) {
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    const float s = n > 0.0f ? 1.0f / n : 0.0f;
    const float x = q.x * s, y = q.y * s, z = q.z * s, w = q.w * s;
    Mat3 r{};
    r.m[0][0] = 1.0f - 2.0f * (y * y + z * z);
    r.m[0][1] = 2.0f * (x * y - z * w);
    r.m[0][2] = 2.0f * (x * z + y * w);
    r.m[1][0] = 2.0f * (x * y + z * w);
    r.m[1][1] = 1.0f - 2.0f * (x * x + z * z);
    r.m[1][2] = 2.0f * (y * z - x * w);
    r.m[2][0] = 2.0f * (x * z - y * w);
    r.m[2][1] = 2.0f * (y * z + x * w);
    r.m[2][2] = 1.0f - 2.0f * (x * x + y * y);
    return r;
}

inline Mat3 Transpose(const Mat3& a) {
    Mat3 t{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            t.m[i][j] = a.m[j][i];
        }
    }
    return t;
}

inline Mat3 operator*(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
        }
    }
    return r;
}

inline Vec3 operator*(const Mat3& a, const Vec3& v) {
    return {a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z,
            a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z,
            a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z};
}

inline Quat operator*(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

/// The angle of the rotation between two orientations, radians.
inline float AngleBetween(const Quat& a, const Quat& b) {
    const float na = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w);
    const float nb = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z + b.w * b.w);
    if (!(na > 0.0f) || !(nb > 0.0f)) {
        return 0.0f;
    }
    const float dot = std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) / (na * nb);
    return 2.0f * std::acos(std::min(dot, 1.0f));
}

/// The yaw of an orientation's view direction (radians, + = turned left: the horizontal forward is
/// (-sin yaw, 0, -cos yaw)); VR::YawPitchFromQuat's.
inline float YawOf(const Quat& q) {
    const Vec3 f = FromQuat(q) * Vec3{0.0f, 0.0f, -1.0f};
    return std::atan2(-f.x, -f.z);
}

/// Where the game's render camera sits for a head at `head` (tracking space, metres) looking along
/// `head_yaw`, relative to the 6DoF origin `origin`: what OnCameraTick and the render-camera hook
/// put into it (the camera's game position minus the body's, rotated into tracking space): the
/// head's offset from the origin in the 6DoF room (core/vr/head_room.h: at most `limit_m` up,
/// sideways and forward, `down_m` below the origin, 0 = the sphere of limit_m; limit_m 0: the head
/// never moves the camera, positional=0) plus, in first person, the eye `forward_m` in front of
/// the head centre along the view's yaw (fp_forward_m).
inline Vec3 CameraPoint(const Vec3& head, float head_yaw, const Vec3& origin, float limit_m,
                        float forward_m, float down_m = 0.0f, bool roomscale = false) {
    Vec3 d = head - origin;
    ::VR::HeadRoom::Clamp(d.x, d.y, d.z, limit_m, down_m, roomscale);
    return d + Vec3{-std::sin(head_yaw), 0.0f, -std::cos(head_yaw)} * forward_m;
}

// ---- late_warp_motion: the game camera's own motion (walking, turning) between its updates ----
//
// A camera update r embeds the game world in tracking space (origin-relative, metres) as
//     T_r(W) = A(b_r) (W - B_r) / s,   A(b) = [[cb, 0, -sb], [0, 1, 0], [-sb, 0, -cb]]
// (OnCameraTick's tracking -> game axes for the heading b; a reflection, A = A^T = A^-1), with B_r
// the update's BODY POINT: its render position minus the head's part A(b) c s (c = CameraPoint,
// s = units per metre). The head's own motion between two updates cancels in B; walking moves B,
// a turn (smooth, lock-on) changes b. A point x of update a's embedding sits in update n's at
//     F(x) = T_n(T_a^-1(x)) = R_y(b_n - b_a) x - A(b_n) (B_n - B_a) / s
// (A(b_n) A(b_a) = R_y(b_n - b_a), R_y: the rotation about tracking +Y). A fraction alpha of it
// (the update's heading and body point interpolated) is the world's motion `alpha` of the way
// from a to n: MotionBetween. The late warp moves the capture's camera S with it (MoveCamera), so
// a frame shown for 2-3 refreshes moves on with the walk; the hunter's own body (arms, weapon,
// legs: BodyShapes) stays where it is in tracking space.

inline float WrapPi(float a) {
    constexpr float Pi = 3.14159265358979f;
    while (a > Pi) {
        a -= 2.0f * Pi;
    }
    while (a < -Pi) {
        a += 2.0f * Pi;
    }
    return a;
}

/// A(b) v: tracking axes -> game axes for the heading b (and back: A is its own inverse).
inline Vec3 GameFromTracking(float b, const Vec3& v) {
    const float cb = std::cos(b), sb = std::sin(b);
    return {cb * v.x - sb * v.z, v.y, -sb * v.x - cb * v.z};
}

/// The body point of a camera update (game units): its render position minus the head's part.
/// `camera_point`: CameraPoint of the update (origin-relative, metres).
inline Vec3 BodyPoint(const Vec3& render_position, float base_yaw, const Vec3& camera_point,
                      float units_per_metre) {
    return render_position - GameFromTracking(base_yaw, camera_point) * units_per_metre;
}

/// R_y(angle) v: the rotation about tracking +Y (+ = counter-clockwise seen from above, like
/// QuatFromYawPitchRoll's yaw).
inline Vec3 RotateY(float angle, const Vec3& v) {
    const float c = std::cos(angle), s = std::sin(angle);
    return {c * v.x + s * v.z, v.y, -s * v.x + c * v.z};
}

inline Quat YawQuat(float angle) {
    return {0.0f, std::sin(0.5f * angle), 0.0f, std::cos(0.5f * angle)};
}

/// The world's motion in tracking space (origin-relative): x -> R_y(yaw) x + t.
struct Motion {
    float yaw{};
    Vec3 t{};
};

inline Vec3 Apply(const Motion& m, const Vec3& x) {
    return RotateY(m.yaw, x) + m.t;
}

/// `alpha` of the world's motion from update a (heading b_a, body point body_a) to update n
/// (b_n, body_n): the heading and the body point interpolated (alpha 1 = F exactly; any alpha
/// extrapolates the same way).
inline Motion MotionBetween(float b_a, const Vec3& body_a, float b_n, const Vec3& body_n,
                            float alpha, float units_per_metre) {
    Motion m{};
    m.yaw = alpha * WrapPi(b_n - b_a);
    const float k = units_per_metre > 0.0f ? -alpha / units_per_metre : 0.0f;
    m.t = GameFromTracking(b_a + m.yaw, (body_n - body_a) * k);
    return m;
}

/// One camera update on the path the shown camera follows: its time (ms from the capture's), body
/// point (game units) and heading (radians, unwrapped along the path).
struct PathPoint {
    float t{};
    Vec3 body{};
    float base{};
};

/// The shown camera at time t on a path of updates (ascending t, n >= 1): a local linear fit with a
/// triangular kernel `width` ms wide on each side. Exact on steady motion (walking, a smooth turn at
/// a steady rate) and through a single point; it smooths the game camera's per-update jitter (a
/// smooth turn's input is taken at XR frames, so each update's heading is off by up to half an XR
/// frame of turning) instead of replaying it. Points outside the kernel do not count; with fewer
/// than two inside, the two nearest are interpolated (or extrapolated) linearly.
inline PathPoint EvaluatePath(const PathPoint* pts, uint32_t n, float t, float width) {
    PathPoint out{t, pts[0].body, pts[0].base};
    if (n == 0) {
        return out;
    }
    if (n == 1) {
        out.body = pts[0].body;
        out.base = pts[0].base;
        return out;
    }
    double s0 = 0, s1 = 0, s2 = 0;
    uint32_t inside = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const double w = std::max(0.0, 1.0 - std::abs(pts[i].t - t) / std::max(width, 1e-3f));
        if (w <= 0.0) {
            continue;
        }
        const double d = pts[i].t - t;
        s0 += w;
        s1 += w * d;
        s2 += w * d * d;
        ++inside;
    }
    const double det = s0 * s2 - s1 * s1;
    if (inside < 2 || !(det > 1e-9 * std::max(s0 * s2, 1e-12))) {
        // The two nearest points (by time), linearly.
        uint32_t k = 1;
        while (k + 1 < n && pts[k].t < t) {
            ++k;
        }
        const PathPoint& a = pts[k - 1];
        const PathPoint& b = pts[k];
        const float f = b.t > a.t ? (t - a.t) / (b.t - a.t) : 0.0f;
        out.body = a.body + (b.body - a.body) * f;
        out.base = a.base + (b.base - a.base) * f;
        return out;
    }
    double bx = 0, by = 0, bz = 0, bb = 0, tx = 0, ty = 0, tz = 0, tb = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const double w = std::max(0.0, 1.0 - std::abs(pts[i].t - t) / std::max(width, 1e-3f));
        if (w <= 0.0) {
            continue;
        }
        const double d = pts[i].t - t;
        bx += w * pts[i].body.x;
        by += w * pts[i].body.y;
        bz += w * pts[i].body.z;
        bb += w * pts[i].base;
        tx += w * d * pts[i].body.x;
        ty += w * d * pts[i].body.y;
        tz += w * d * pts[i].body.z;
        tb += w * d * pts[i].base;
    }
    const auto at = [&](double sy, double sty) {
        return static_cast<float>((s2 * sy - s1 * sty) / det);
    };
    out.body = {at(bx, tx), at(by, ty), at(bz, tz)};
    out.base = at(bb, tb);
    return out;
}

/// A camera (orientation q, origin-relative point c) moved with the world.
inline void MoveCamera(const Motion& m, Quat& q, Vec3& c) {
    q = YawQuat(m.yaw) * q;
    c = Apply(m, c);
}

// ---- the hunter's own body: what the motion leaves where it is ----
// Capsules (segments with a radius) in the capture's camera frame (game units, x right, y up,
// looking down -z): a texel whose surface point lies in one, above the floor plane (the floor under
// his feet stays the world), is the hunter (his arms follow the controllers in tracking space, his
// body the 6DoF origin); the rest is the world.

constexpr uint32_t kMaxBodyCapsules = 8;

/// A body capsule in the capture's camera frame (the camera at the origin). `behind`: a point
/// counts only when it is at most this much farther from the camera than the capsule's axis point
/// nearest to it. The visible (front-facing) surface of a limb or a blade round the axis is never
/// farther than its axis point, so a wall or an enemy just behind an arm or the weapon stays the
/// world (2026-10-05: the walls round the hands were taken as his body); a hull capsule (eye,
/// torso, legs, whose inside shows through the cutaway) keeps its whole radius.
struct Capsule {
    Vec3 a{}, b{};
    float radius{};
    float behind{};
};

struct BodyShapes {
    std::array<Capsule, kMaxBodyCapsules> capsules{};
    uint32_t count{0};
    float reach{0.0f}; ///< no capsule point is farther from the camera than this (game units)
    /// Body points lie above this plane: Dot(floor_n, p) + floor_d > 0 (zero normal: no floor).
    Vec3 floor_n{};
    float floor_d{0.0f};
};

inline float SegmentDistance(const Vec3& p, const Vec3& a, const Vec3& b) {
    const Vec3 ab = b - a;
    const float len2 = Dot(ab, ab);
    const float t = len2 > 0.0f ? std::clamp(Dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    return Length(p - (a + ab * t));
}

inline Vec3 ClosestOnSegment(const Vec3& p, const Vec3& a, const Vec3& b) {
    const Vec3 ab = b - a;
    const float len2 = Dot(ab, ab);
    const float t = len2 > 0.0f ? std::clamp(Dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    return a + ab * t;
}

/// `behind` < 0: the capsule's radius (no limit; Capsule::behind).
inline void AddCapsule(BodyShapes& shapes, const Vec3& a, const Vec3& b, float radius,
                       float behind = -1.0f) {
    if (shapes.count >= kMaxBodyCapsules || !(radius > 0.0f)) {
        return;
    }
    shapes.capsules[shapes.count++] = {a, b, radius, behind < 0.0f ? radius : behind};
    shapes.reach = std::max(shapes.reach, std::max(Length(a), Length(b)) + radius);
}

/// late_warp_motion: the hunter's soles below the 6DoF origin (tracking metres, negative): the
/// nominal eye height plus the camera's own dip under it (CameraRecord::floor_dip_m, negative =
/// the camera low: on stairs, slopes and broken floors the floor is that much nearer), never
/// nearer than 0.5 m. Without the dip the floor round his feet rose above the floor plane and was
/// taken as his body (2026-10-05: dips of -5 to -17 cm while walking).
inline float SolesY(float eye_height_m, bool has_dip, float dip_m) {
    const float dip = has_dip && std::isfinite(dip_m) ? std::clamp(dip_m, -0.8f, 0.5f) : 0.0f;
    return -std::max(eye_height_m + dip, 0.5f);
}

inline bool InBody(const BodyShapes& shapes, const Vec3& p) {
    if (Dot(shapes.floor_n, p) + shapes.floor_d <= 0.0f && Dot(shapes.floor_n, shapes.floor_n) > 0.0f) {
        return false;
    }
    for (uint32_t i = 0; i < shapes.count; ++i) {
        const Capsule& c = shapes.capsules[i];
        const Vec3 axis = ClosestOnSegment(p, c.a, c.b);
        if (Length(p - axis) <= c.radius && Length(p) <= Length(axis) + c.behind) {
            return true;
        }
    }
    return false;
}

// The hunter's body capsules (late_warp_motion), tight round him (2026-10-05: 0.45 / 0.35 /
// 0.55 m took walls and door frames beside him as his body; the cutaway clears him within 22 cm
// of the eye, the torso's capsule reaches the collar and the shoulders).
inline constexpr float kBodyEyeM = 0.25f;   // body capsule round the eye
inline constexpr float kBodyTorsoM = 0.28f; // ... the torso's radius
inline constexpr float kBodyLegsM = 0.40f;  // ... the legs' (a stride)
inline constexpr float kBodyFloorM = 0.08f; // the floor up to this over his soles: world
// An arm's points count up to this much farther from the camera than its capsule's axis (the
// elbow off the shoulder-grip line).
inline constexpr float kBodyArmBehindM = 0.12f;
inline constexpr float kBodyArmM = 0.18f;     // the shoulder-to-grip arm's radius
inline constexpr float kBodyWeaponM = 1.8f;   // the right hand's weapon length
inline constexpr float kBodyForearmM = 0.30f; // arms_only: grip -> elbow at most
inline constexpr float kBodyForearmR = 0.12f; // ... its radius

/// What the renderer draws of the hunter in first-person colour passes (VR::Body::DrawnArms):
/// only what is drawn may stay where it is when the world moves on. Whole (no_head, full): eye,
/// torso, legs and the shoulder-to-grip arms; Forearms (arms_only): a forearm from each grip
/// toward its shoulder; Hands (hands_only): neither (the hand lies within ~10 cm of the grip, on
/// the weapon's and the gun's capsules, which every mode keeps). Before the 2026-10-08 review
/// every mode kept the whole hull, so in hands_only the world inside it (the next stair tread, a
/// crate at his feet, a railing by his chest) stayed head-locked between game frames and lost its
/// eye-TAA history.
enum class BodyDrawn : uint8_t { Whole, Forearms, Hands };

/// The hunter for one capture, tracking metres relative to the 6DoF origin.
struct HunterPose {
    Vec3 head{};                 // the camera point (his eye)
    float yaw{0.0f};             // the head's yaw (his shoulders' heading)
    float soles_y{0.0f};         // SolesY
    std::array<bool, 2> own{};   // left, right: the hand is yours (tracked, not the game's arm)
    std::array<Vec3, 2> grip{};  // the grips
    std::array<Vec3, 2> blade{}; // unit: along the blade / barrel (grip -Z pitched by blade_pitch)
};

/// His body capsules in a capture's camera frame (rs_t: tracking -> camera rotation, c_s the
/// camera point, upm game units per metre).
inline BodyShapes HunterBodyShapes(const HunterPose& h, BodyDrawn drawn, const Mat3& rs_t,
                                   const Vec3& c_s, float upm) {
    const auto to_s = [&](const Vec3& x) { return (rs_t * (x - c_s)) * upm; };
    BodyShapes shapes{};
    const Vec3 right{std::cos(h.yaw), 0.0f, -std::sin(h.yaw)};
    const Vec3 hip{0.0f, h.soles_y + 0.95f, 0.0f};
    if (drawn == BodyDrawn::Whole) {
        // Around the eye: the hat brim, the collar, the shoulders, the chest below.
        AddCapsule(shapes, to_s(c_s), to_s(c_s), kBodyEyeM * upm);
        // Torso (leaning with the head), then the legs down to the floor.
        AddCapsule(shapes, to_s(h.head + Vec3{0.0f, -0.25f, 0.0f}), to_s(hip), kBodyTorsoM * upm);
        AddCapsule(shapes, to_s(hip), to_s(Vec3{0.0f, h.soles_y, 0.0f}), kBodyLegsM * upm);
    }
    // The floor: tracking up, kBodyFloorM above his soles, in the capture's frame.
    shapes.floor_n = rs_t * Vec3{0.0f, 1.0f, 0.0f};
    shapes.floor_d = -Dot(shapes.floor_n, to_s(Vec3{0.0f, h.soles_y + kBodyFloorM, 0.0f}));
    for (uint32_t side = 0; side < 2; ++side) {
        if (!h.own[side]) {
            continue;
        }
        const Vec3 shoulder =
            h.head + Vec3{0.0f, -0.22f, 0.0f} + right * (side == 1 ? 0.18f : -0.18f);
        const Vec3& grip = h.grip[side];
        const Vec3& blade = h.blade[side];
        // The arm: only its side toward the camera (Capsule::behind): a wall a little behind it
        // stays the world. The weapon and the gun keep their whole capsules: the game holds them
        // off the grip's blade axis (a limit there cut off part of a Saw Cleaver's blade).
        if (drawn == BodyDrawn::Whole) {
            AddCapsule(shapes, to_s(shoulder), to_s(grip), kBodyArmM * upm, kBodyArmBehindM * upm);
        } else if (drawn == BodyDrawn::Forearms) {
            const Vec3 to_shoulder = shoulder - grip;
            const float len = Length(to_shoulder);
            const Vec3 elbow =
                len > 1e-4f ? grip + to_shoulder * (std::min(kBodyForearmM, len) / len) : grip;
            AddCapsule(shapes, to_s(grip), to_s(elbow), kBodyForearmR * upm,
                       kBodyArmBehindM * upm);
        }
        if (side == 1) {
            AddCapsule(shapes, to_s(grip - blade * 0.3f), to_s(grip + blade * kBodyWeaponM),
                       0.24f * upm);
        } else {
            AddCapsule(shapes, to_s(grip - blade * 0.2f), to_s(grip + blade * 0.6f), 0.26f * upm);
        }
    }
    return shapes;
}

/// A render's pinhole in continuous pixel coordinates (texel i's centre at i + 0.5): pixel x =
/// cx + fx * X / -Z, y = cy - fy * Y / -Z (the game's projection, vr_stereo_warp_math.h).
struct Intrinsics {
    float fx{}, fy{}, cx{}, cy{};
};

inline Intrinsics MakeIntrinsics(float vfov, float aspect, float width, float height) {
    const float t = std::tan(0.5f * vfov);
    return {0.5f * width / (t * aspect), 0.5f * height / t, 0.5f * width, 0.5f * height};
}

/// The surface point of a capture texel (continuous pixel (px, py), inverse depth w > 0) in the
/// capture's camera frame (game units).
inline Vec3 Unproject(const Intrinsics& in, float px, float py, float w) {
    const float z = 1.0f / w;
    return {(px - in.cx) / in.fx * z, -(py - in.cy) / in.fy * z, -z};
}

/// One XR frame's V -> S mapping.
struct Reprojection {
    Mat3 rsv{}; ///< V's camera axes -> S's camera axes
    Vec3 c{};   ///< V's centre in S's camera frame, game units
};

/// q_s / c_s: the source camera's orientation and camera point, q_v / c_v: V's (tracking space,
/// metres; the points from CameraPoint, the same origin).
inline Reprojection MakeReprojection(const Quat& q_s, const Vec3& c_s, const Quat& q_v,
                                     const Vec3& c_v, float units_per_metre) {
    const Mat3 rs_t = Transpose(FromQuat(q_s));
    return {rs_t * FromQuat(q_v), (rs_t * (c_v - c_s)) * units_per_metre};
}

/// One V pixel's epipolar line in S.
struct Line {
    float q0x{}, q0y{}; ///< the rotation-only point (r = 0), continuous source pixels
    float gx{}, gy{};   ///< dq / dr, pixels per unit of inverse depth
    float dz{1.0f};     ///< -d.z of the unnormalised ray (V's inverse depth: w = r dz / (1 + r cz))
    bool valid{false};  ///< false: the ray points behind S's image plane
};

/// The line of V's continuous pixel (px, py).
inline Line LineFor(const Reprojection& rp, const Intrinsics& in, float px, float py) {
    const Vec3 dv{(px - in.cx) / in.fx, -(py - in.cy) / in.fy, -1.0f};
    const Vec3 d = rp.rsv * dv;
    Line line{};
    line.dz = -d.z;
    line.valid = line.dz > 1e-3f;
    const float inv = 1.0f / std::max(line.dz, 1e-3f);
    const Vec3 dn{d.x * inv, d.y * inv, -1.0f};
    const Vec3 e = rp.c + dn * rp.c.z;
    line.q0x = in.cx + in.fx * dn.x;
    line.q0y = in.cy - in.fy * dn.y;
    line.gx = in.fx * e.x;
    line.gy = -in.fy * e.y;
    return line;
}

/// V's inverse depth of the ray point with source inverse depth r (cz = Reprojection::c.z).
inline float ViewInverseDepth(const Line& line, float cz, float r) {
    return r * line.dz / std::max(1.0f + r * cz, 1e-3f);
}

/// What vr_dibr_prep.comp stores for a surface of inverse depth w (game units): one eye's
/// disparity K * w through the near-field knee, the cap and the snap (VrStereo::DepthToDisparity
/// without the depth decode; w = 0 is the sky).
inline float DisparityOfInverseDepth(float w, const VrStereo::DisparityParams& p) {
    if (!(w > 0.0f)) {
        return 0.0f;
    }
    float D = VrStereo::CompressNear(p.k_px * w, p);
    D = std::min(D, p.max_px);
    return D < p.snap_px ? 0.0f : D;
}

// ---- the scan (CPU reference of vr_late_reproject.comp) ----

constexpr float kStepPx = 0.5f;   ///< source pixels between two samples of the scan
constexpr uint32_t kMaxSteps = 768; ///< longer lines are scanned in larger steps
constexpr int kTileSize = VrStereo::kTileSize; ///< the prep pass's tiles (min/max 1/z)

enum class Kind : int {
    None = 0,    ///< nothing crossed (only past the render's border)
    Surface = 1, ///< a root on one continuous surface (or just past an edge onto it)
    Edge = 2,    ///< the nearer surface at its depth edge (its footprint)
    Hole = 3,    ///< the ray hit something S never saw (behind a nearer surface): its background
};

struct Hit {
    float qx{}, qy{}; ///< continuous source pixels
    float r{};        ///< source inverse depth of the surface
    Kind kind{Kind::None};
    uint32_t steps{};
    /// Layered scans only: the ray stayed behind occluder texels (kOccluderW) from its first sample
    /// to its last (the world behind the hunter's body S never saw, nothing beside it either).
    bool occluded{false};
};

/// Layered scans (late_warp_motion): the body layer's fetch gives the world's texels 0 (nothing:
/// farther than any ray it scans); the world layer's (WorldLayerW) gives a hunter's body texel 0
/// while the ray is in front of the body there (S saw through that space to the body: empty) and
/// this occluder behind it (what is behind the body S never saw: the scan goes on behind it).
constexpr float kOccluderW = 1.0e20f;

/// The world layer's 1/z of a texel whose signed 1/z is `v` (vr_late_classify.comp: body texels
/// negative) for a ray at source inverse depth r.
inline float WorldLayerW(float v, float r) {
    return v >= 0.0f ? v : (r > -v ? 0.0f : kOccluderW);
}

/// The source inverse depth at continuous (qx, qy) of a width x height image, borders clamped:
/// bilinear where the 2x2 texels around it are one surface (spread <= tau), else the nearest texel
/// (every texel a flat footprint up to its edges, as the 1D scan's half-pixel footprints).
template <typename Fetch>
float SampleInverseDepth(const Fetch& fetch, int width, int height, float qx, float qy,
                         float tau) {
    const float tx = qx - 0.5f;
    const float ty = qy - 0.5f;
    const float bx = std::floor(tx);
    const float by = std::floor(ty);
    const float fx = tx - bx;
    const float fy = ty - by;
    const int x0 = std::clamp(static_cast<int>(bx), 0, width - 1);
    const int x1 = std::clamp(static_cast<int>(bx) + 1, 0, width - 1);
    const int y0 = std::clamp(static_cast<int>(by), 0, height - 1);
    const int y1 = std::clamp(static_cast<int>(by) + 1, 0, height - 1);
    const float w00 = fetch(x0, y0);
    const float w10 = fetch(x1, y0);
    const float w01 = fetch(x0, y1);
    const float w11 = fetch(x1, y1);
    const float lo = std::min(std::min(w00, w10), std::min(w01, w11));
    const float hi = std::max(std::max(w00, w10), std::max(w01, w11));
    if (hi - lo <= tau) {
        const float top = w00 + (w10 - w00) * fx;
        const float bottom = w01 + (w11 - w01) * fx;
        return top + (bottom - top) * fy;
    }
    return fx < 0.5f ? (fy < 0.5f ? w00 : w01) : (fy < 0.5f ? w10 : w11);
}

/// The nearest surface along `line` with source inverse depth in [r_lo, r_hi] (the range must
/// bound every surface the line crosses, so the scan starts in front of all of them). Scanned from
/// the near end in kStepPx steps of the line (W(q) against r):
/// - a crossing between two samples of one surface: the root, linear between them (exact on planes);
/// - a crossing onto a nearer surface (a jump of more than tau): the ray hits it at its edge if it
///   was in front of it there (Edge), else the ray is behind it, hidden from S: the scan goes on
///   until it leaves the nearer surface again, in front of what follows (the scan continues: the ray
///   passed behind a thin object) or behind it (a Hole: the ray hit what S never saw; its colour is
///   that surface's next to the edge);
/// - a crossing onto a farther surface it is already behind at the edge: a Hole too.
/// `layered` (a body or world layer of late_warp_motion, whose fetch masks the other layer): no
/// rotation-only shortcut, and a first sample on an occluder (kOccluderW) starts behind it.
/// `fetch(x, y)` or, for a layer whose texels depend on the ray's depth (the world layer),
/// `fetch(x, y, r)`.
template <typename Fetch>
Hit March(const Line& line, const Fetch& fetch, int width, int height, float r_lo, float r_hi,
          float tau, bool layered = false) {
    const auto sample = [&](float r, float& qx, float& qy) {
        qx = line.q0x + r * line.gx;
        qy = line.q0y + r * line.gy;
        if constexpr (std::is_invocable_v<const Fetch&, int, int, float>) {
            return SampleInverseDepth([&](int x, int y) { return fetch(x, y, r); }, width, height,
                                      qx, qy, tau);
        } else {
            return SampleInverseDepth(fetch, width, height, qx, qy, tau);
        }
    };
    Hit hit{};
    r_hi = std::max(r_hi, r_lo);
    const float glen = std::sqrt(line.gx * line.gx + line.gy * line.gy);
    if (!layered && glen * (r_hi - r_lo) <= 0.25f) {
        // No measurable parallax over the range (the head only turned): the rotation-only point,
        // refined once at its surface's depth.
        float qx{}, qy{};
        const float r0 = sample(0.5f * (r_lo + r_hi), qx, qy);
        hit.r = sample(r0, hit.qx, hit.qy);
        hit.kind = Kind::Surface;
        hit.steps = 2;
        return hit;
    }
    const float dr = std::max(kStepPx / glen, (r_hi - r_lo) / static_cast<float>(kMaxSteps));
    bool hidden = false;
    bool has_bg = true;             // false: hidden from the first sample (layered)
    float bg_qx{}, bg_qy{}, bg_r{}; // hidden: the surface the ray went behind from
    float pr{}, pqx{}, pqy{}, ps{}; // the previous sample
    float r = r_hi;
    for (uint32_t n = 0;; ++n) {
        float qx{}, qy{};
        const float s = sample(r, qx, qy);
        const float delta = r - s;
        hit.steps = n + 1;
        if (n == 0 && layered && s >= 0.5f * kOccluderW) {
            hidden = true; // behind the hunter's body from the start
            has_bg = false;
        }
        if (n > 0) {
            const bool jump = std::abs(s - ps) > tau;
            const float rb = 0.5f * (r + pr); // an edge lies between the two samples
            const float qbx = 0.5f * (qx + pqx);
            const float qby = 0.5f * (qy + pqy);
            // The ray's crossing of a surface of depth s beyond the edge: from (qb, rb) to (q, r).
            const auto beyond_edge = [&](Kind kind) {
                const float t = rb > r ? std::clamp((rb - s) / (rb - r), 0.0f, 1.0f) : 1.0f;
                hit = {qbx + (qx - qbx) * t, qby + (qy - qby) * t, s, kind, n + 1};
            };
            if (!hidden) {
                const float pd = pr - ps;
                if (pd <= 0.0f) {
                    // Behind a surface from the start (the range did not bound it): that surface.
                    hit = {pqx, pqy, ps, Kind::Surface, n + 1};
                    return hit;
                }
                if (delta <= 0.0f) {
                    if (!jump) {
                        const float t = pd / (pd - delta);
                        hit = {pqx + (qx - pqx) * t, pqy + (qy - pqy) * t, ps + (s - ps) * t,
                               Kind::Surface, n + 1};
                        return hit;
                    }
                    if (s > ps) {
                        if (rb >= s) {
                            beyond_edge(Kind::Edge);
                            return hit;
                        }
                        hidden = true;
                        has_bg = ps > 0.0f || !layered; // not from a body texel's empty space
                        bg_qx = pqx;
                        bg_qy = pqy;
                        bg_r = ps;
                    } else {
                        if (rb >= s) {
                            beyond_edge(Kind::Surface);
                        } else {
                            hit = {qx, qy, s, Kind::Hole, n + 1};
                        }
                        return hit;
                    }
                }
            } else if (jump && s < ps) {
                if (rb > s) {
                    hidden = false; // out from behind it, in front of what follows
                    if (delta <= 0.0f) {
                        beyond_edge(Kind::Surface);
                        return hit;
                    }
                } else {
                    hit = {qx, qy, s, Kind::Hole, n + 1};
                    return hit;
                }
            } else if (!jump && delta > 0.0f) {
                hidden = false; // through the surface from behind it (a back face): not seen
            }
        }
        pr = r;
        pqx = qx;
        pqy = qy;
        ps = s;
        if (r <= r_lo) {
            break;
        }
        r = std::max(r - dr, r_lo);
    }
    if (hidden && has_bg) {
        hit = {bg_qx, bg_qy, bg_r, Kind::Hole, hit.steps};
    } else if (hidden) {
        hit = {pqx, pqy, 0.0f, Kind::None, hit.steps, true};
    } else {
        hit = {pqx, pqy, ps, Kind::None, hit.steps};
    }
    return hit;
}

// ---- late_warp_edge_blend (vr_dibr_warp.comp LATE_WARP: EdgeColour) ----
// Pass 2 reads the capture's colour at V's sub-pixel position q: Catmull-Rom / bilinear over one
// surface, and at a depth edge (the bilinear footprint is not one surface) the nearest texel, so
// the edge cannot smear across a silhouette. But V slides over the capture by a new sub-pixel
// phase every refresh, and the nearest texel flips there: the capture's own anti-aliased edge is
// shown in whole-texel jumps (the late warp's crawl on fine detail: Dream trees
// 13.0 -> 17.8). Where the two sides of the edge move apart by little between the capture and V
// (q(r) = q0 + r g: |g| (w_hi - w_lo) px), the capture's edge is still where V sees it to within
// that, and the bilinear colour slides it smoothly instead; where they move apart by more
// (a near silhouette, a large head motion) the nearest texel stays.

/// The largest |g| over V's image (width x height continuous pixels): px of capture -> V parallax
/// per unit of inverse depth. |g|^2 is a sum of squares of affine functions of dn, and V's image is
/// a convex quadrilateral in dn, so the maximum is at one of its corners. A corner ray behind the
/// capture's image plane: infinite (no blend).
inline float ParallaxPerInverseDepth(const Reprojection& rp, const Intrinsics& in, float width,
                                     float height) {
    float g = 0.0f;
    for (int k = 0; k < 4; ++k) {
        const Line line = LineFor(rp, in, (k & 1) ? width : 0.0f, (k & 2) ? height : 0.0f);
        if (!line.valid) {
            return 3.0e38f;
        }
        g = std::max(g, std::hypot(line.gx, line.gy));
    }
    return g;
}

/// Pass 2's WarpPush::edge_k for one warp: < 0 off (the nearest texel at every depth edge, as
/// before); else the parallax per unit of inverse depth in units of `threshold_px`
/// (vr.ini late_warp_edge_blend, 0 = off). Off while the world moves (late_warp_motion): the
/// hunter's body and the world then move apart by the game camera's own motion, which no 1/z
/// difference measures.
inline float EdgeBlendScale(float threshold_px, bool motion, const Reprojection& rp,
                            const Intrinsics& in, float width, float height) {
    if (!(threshold_px > 0.0f) || motion) {
        return -1.0f;
    }
    return std::min(ParallaxPerInverseDepth(rp, in, width, height) / threshold_px, 1.0e30f);
}

/// The share of the bilinear colour at a depth edge whose footprint spans inverse depths
/// [w_lo, w_hi] (EdgeBlendScale `scale`): 1 while its two sides move apart by at most half the
/// threshold between the capture and V, falling to 0 (the nearest texel, as before) at the
/// threshold.
inline float EdgeBlendWeight(float scale, float w_lo, float w_hi) {
    if (!(scale >= 0.0f)) {
        return 0.0f;
    }
    return std::clamp(2.0f - 2.0f * scale * std::max(w_hi - w_lo, 0.0f), 0.0f, 1.0f);
}

/// vr_dibr_warp.comp EdgeColour (one channel): the colour at a depth edge of the 2x2 footprint
/// `c` / its inverse depths `w` (texels i, i + x, i + y, i + x + y) at fractions (fx, fy): the
/// nearest texel, mixed toward the bilinear colour by EdgeBlendWeight over the texels with
/// weight.
inline float EdgeColour(const std::array<float, 4>& c, const std::array<float, 4>& w, float fx,
                        float fy, float scale) {
    const int nearest = (fx >= 0.5f ? 1 : 0) + (fy >= 0.5f ? 2 : 0);
    const bool ux = fx > 1e-4f;
    const bool uy = fy > 1e-4f;
    float lo = w[0], hi = w[0];
    for (int k = 1; k < 4; ++k) {
        if (((k & 1) && !ux) || ((k & 2) && !uy)) {
            continue; // no weight
        }
        lo = std::min(lo, w[k]);
        hi = std::max(hi, w[k]);
    }
    const float a = EdgeBlendWeight(scale, lo, hi);
    const float bx = ux ? fx : 0.0f; // a texel without weight takes none
    const float by = uy ? fy : 0.0f;
    const float top = c[0] + (c[1] - c[0]) * bx;
    const float bottom = c[2] + (c[3] - c[2]) * bx;
    const float bilinear = top + (bottom - top) * by;
    return c[nearest] + (bilinear - c[nearest]) * a;
}

} // namespace Vulkan::VrLate
