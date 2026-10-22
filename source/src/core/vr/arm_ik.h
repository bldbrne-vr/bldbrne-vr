// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include "core/vr/c0000_skeleton.h"

// VR body: arm IK core for the tracked hunter arms. Pure math on Havok-style poses (no OpenXR,
// Vulkan or emulator dependencies), so synthetic and recorded poses can drive it in unit tests.
//
// Conventions
//  - A pose is an hkaPose local-space array: one QsTransform (translation, rotation, scale) per
//    bone plus a parent-index array in which every parent precedes its children (Havok's rule).
//  - Composition follows hkQsTransform::setMul (checked against syncModelSpace 0x6C4F20):
//        model.t = P.t + rotate(P.q, L.t)     (the parent's scale is NOT applied)
//        model.q = P.q * L.q                  model.s = P.s * L.s
//  - Quaternions are (x, y, z, w) like hkQuaternionf, Hamilton product, v' = q v q^-1.
//  - Hunter c0000 model space: +Y up, -Z forward, -X to the character's right. Arm bones run
//    along local +X and the elbow hinge (upperDir x foreDir) is local +Y of both arm bones.
//
// SolveArm, per arm and frame:
//  1. FK of the clavicle chain from the animated locals.
//  2. Effector target (the weapon bone, placed on the controller grip) -> wrist target.
//  3. Clavicle reach (FRIK): far targets turn the clavicle part of the way toward them.
//  4. Elbow direction: the pole (a body-space hint) is turned by the wrist roll and the wrist
//     swing, so the arm follows the hand like a real one, clamped, rate-limited and low-passed
//     (the roll's share fades out as the elbow folds toward its limit, where the roll is ill
//     defined). Near the pole singularity the previous elbow side is kept, so the elbow never
//     flips. Natural arms (ArmParams::naturalRoll): only the roll beyond the forearm's own range
//     turns the elbow, the roll followed continuously through 180 degrees; an elbow inside the
//     trunk (ArmParams::keepOut) turns out of it.
//  5. Two-bone solve (law of cosines) with elbow flexion limits and a small stretch for targets
//     just out of reach. Past that the arm points straight at the target and the result says
//     how far the hand is detached (ghost hand / haptic tick in the caller).
//  6. Bone rotations from (bone axis, hinge axis) pairs, so there is no twist ambiguity. The hand
//     takes the target orientation exactly (1:1 weapon); its roll is spread over the forearm
//     twist helpers (natural arms: followed continuously, ArmParams::trackTwist) and the
//     elbow/shoulder helpers take their share of the bend. The forearm bone itself keeps the
//     elbow's hinge (the ulna does not turn); the twist helpers are the radius.
//  7. Blend with the animated locals by `weight`. If any guard fails nothing is written.
namespace VR::Body {

constexpr float Pi = 3.14159265358979f;
constexpr float Deg = Pi / 180.0f;

// ---- Vectors --------------------------------------------------------------------------------

struct Vec3 {
    float x{}, y{}, z{};
};

inline Vec3 operator+(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec3 operator-(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vec3 operator-(Vec3 a) {
    return {-a.x, -a.y, -a.z};
}
inline Vec3 operator*(Vec3 a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}
inline Vec3 operator*(float s, Vec3 a) {
    return a * s;
}
inline Vec3& operator+=(Vec3& a, Vec3 b) {
    a = a + b;
    return a;
}
inline float Dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
// Component-wise product (scales).
inline Vec3 Mul(Vec3 a, Vec3 b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}
inline float LengthSq(Vec3 a) {
    return Dot(a, a);
}
inline float Length(Vec3 a) {
    return std::sqrt(Dot(a, a));
}
inline bool IsFinite(Vec3 a) {
    return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}
// Unit vector, or `fallback` for zero, tiny or non-finite input.
inline Vec3 Normalize(Vec3 a, Vec3 fallback = {}) {
    const float length = Length(a);
    return (length > 1e-12f && std::isfinite(length)) ? a * (1.0f / length) : fallback;
}
inline Vec3 Lerp(Vec3 a, Vec3 b, float t) {
    return a + (b - a) * t;
}
// Component of v perpendicular to the unit vector n.
inline Vec3 Perp(Vec3 v, Vec3 n) {
    return v - n * Dot(v, n);
}
// Some unit vector perpendicular to the unit vector n (deterministic; degenerate cases only).
inline Vec3 AnyPerpendicular(Vec3 n) {
    const Vec3 helper = std::abs(n.x) < 0.6f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
    return Normalize(Cross(n, helper), {0.0f, 0.0f, 1.0f});
}
// Angle between two directions, accurate for small and large angles.
inline float Angle(Vec3 a, Vec3 b) {
    return std::atan2(Length(Cross(a, b)), Dot(a, b));
}
// Signed angle from `from` to `to` about the unit `axis` (both taken in the plane normal to it).
inline float SignedAngle(Vec3 from, Vec3 to, Vec3 axis) {
    return std::atan2(Dot(axis, Cross(from, to)), Dot(from, to));
}
inline float WrapPi(float a) {
    return std::remainder(a, 2.0f * Pi);
}
inline float Smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// ---- Quaternions (x, y, z, w) ---------------------------------------------------------------

struct Quat {
    float x{}, y{}, z{}, w{1.0f};
};

inline Quat operator*(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Quat Conjugate(const Quat& q) {
    return {-q.x, -q.y, -q.z, q.w};
}
inline float Dot(const Quat& a, const Quat& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}
inline bool IsFinite(const Quat& q) {
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}
// Unit quaternion, or identity for zero or non-finite input.
inline Quat Normalize(const Quat& q) {
    const float length = std::sqrt(Dot(q, q));
    if (!(length > 1e-12f) || !std::isfinite(length)) {
        return {};
    }
    const float inv = 1.0f / length;
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}
// v' = q v q^-1 for a unit q.
inline Vec3 Rotate(const Quat& q, Vec3 v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = Cross(u, v) * 2.0f;
    return v + t * q.w + Cross(u, t);
}
// Rotation by `angle` radians about `axis` (need not be unit; zero axis = identity).
inline Quat AxisAngle(Vec3 axis, float angle) {
    const Vec3 n = Normalize(axis);
    if (LengthSq(n) == 0.0f) {
        return {};
    }
    const float s = std::sin(0.5f * angle);
    return {n.x * s, n.y * s, n.z * s, std::cos(0.5f * angle)};
}
inline Vec3 RotateAbout(Vec3 v, Vec3 axis, float angle) {
    return Rotate(AxisAngle(axis, angle), v);
}
// Rotation angle of q in [0, pi].
inline float AngleOf(const Quat& q) {
    return 2.0f * std::atan2(Length(Vec3{q.x, q.y, q.z}), std::abs(q.w));
}
// Angle of the rotation that takes a to b, in [0, pi].
inline float AngleBetween(const Quat& a, const Quat& b) {
    return AngleOf(Conjugate(a) * b);
}
// Shortest-arc rotation taking direction `from` to direction `to`.
inline Quat FromTo(Vec3 from, Vec3 to) {
    const Vec3 a = Normalize(from), b = Normalize(to);
    if (LengthSq(a) == 0.0f || LengthSq(b) == 0.0f) {
        return {};
    }
    const float d = Dot(a, b);
    if (d < -0.999999f) {
        const Vec3 axis = AnyPerpendicular(a);
        return {axis.x, axis.y, axis.z, 0.0f};
    }
    const Vec3 c = Cross(a, b);
    return Normalize(Quat{c.x, c.y, c.z, 1.0f + d});
}
// Rotation whose columns (images of +X, +Y, +Z) are the orthonormal right-handed basis X, Y, Z.
inline Quat FromBasis(Vec3 X, Vec3 Y, Vec3 Z) {
    const float m00 = X.x, m01 = Y.x, m02 = Z.x;
    const float m10 = X.y, m11 = Y.y, m12 = Z.y;
    const float m20 = X.z, m21 = Y.z, m22 = Z.z;
    const float trace = m00 + m11 + m22;
    Quat q;
    if (trace > 0.0f) {
        const float s = 0.5f / std::sqrt(trace + 1.0f);
        q = {(m21 - m12) * s, (m02 - m20) * s, (m10 - m01) * s, 0.25f / s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = 2.0f * std::sqrt(std::max(1.0f + m00 - m11 - m22, 1e-12f));
        q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        const float s = 2.0f * std::sqrt(std::max(1.0f + m11 - m00 - m22, 1e-12f));
        q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        const float s = 2.0f * std::sqrt(std::max(1.0f + m22 - m00 - m11, 1e-12f));
        q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
    return Normalize(q);
}
// Shortest-path spherical interpolation, t in [0, 1].
inline Quat Slerp(const Quat& a, Quat b, float t) {
    float cosom = Dot(a, b);
    if (cosom < 0.0f) {
        b = {-b.x, -b.y, -b.z, -b.w};
        cosom = -cosom;
    }
    float s0 = 1.0f - t, s1 = t;
    if (cosom < 0.9995f) {
        const float omega = std::acos(std::min(cosom, 1.0f));
        const float so = std::sin(omega);
        s0 = std::sin((1.0f - t) * omega) / so;
        s1 = std::sin(t * omega) / so;
    }
    return Normalize(
        Quat{a.x * s0 + b.x * s1, a.y * s0 + b.y * s1, a.z * s0 + b.z * s1, a.w * s0 + b.w * s1});
}
// `t` times the rotation q (shortest path): Slerp(identity, q, t).
inline Quat ScaleRotation(const Quat& q, float t) {
    const Quat p = q.w < 0.0f ? Quat{-q.x, -q.y, -q.z, -q.w} : q;
    const Vec3 v{p.x, p.y, p.z};
    const float s = Length(v);
    if (!(s > 1e-9f)) {
        return {};
    }
    return AxisAngle(v, 2.0f * std::atan2(s, p.w) * t);
}
// Signed twist of q about the unit `axis` in (-pi, pi] (swing-twist split q = swing * twist).
inline float TwistAngle(const Quat& q, Vec3 axis) {
    return WrapPi(2.0f * std::atan2(Dot(Vec3{q.x, q.y, q.z}, axis), q.w));
}
// The twist about the unit `axis` (parent frame) of the change from local rotation `from` to `to`
// (the twist of to * conj(from)), clamped to +-`max`, for a twist helper bone. Continuous
// everywhere: it fades to 0 where the split is undefined (a change of the bend near 180 degrees)
// and from halfway between `max` and 180 degrees toward +-180 (where +180 and -180 are the same
// turn), so a helper never snaps from one clamp to the other.
inline float HelperTwist(const Quat& from, const Quat& to, Vec3 axis, float max) {
    const Quat rel = Normalize(Normalize(to) * Conjugate(Normalize(from)));
    const float along = Dot(Vec3{rel.x, rel.y, rel.z}, axis);
    const float twist = WrapPi(2.0f * std::atan2(along, rel.w));
    const float defined = Smoothstep(0.2f, 0.5f, std::sqrt(along * along + rel.w * rel.w));
    const float limit = std::clamp(max, 0.0f, Pi);
    const float fade =
        1.0f - Smoothstep(std::min(0.5f * (limit + Pi), 0.99f * Pi), Pi, std::abs(twist));
    return std::clamp(twist, -limit, limit) * fade * defined;
}
// `raw` (an angle in (-pi, pi]) followed on from `tracked` (last frame's, unwrapped): the nearest
// turn of raw to it. Past +-`span` (at least pi) it goes round to the other side (a hysteresis:
// a turn on past half a turn keeps its way until `span`, which a forearm and shoulder cannot
// reach, so a hand that came the impossible way round finds the possible one). The first time
// (`valid` false) it is raw itself.
inline float TrackAngle(float raw, float& tracked, bool valid, float span) {
    float t = valid ? tracked + WrapPi(raw - tracked) : raw;
    const float s = std::clamp(span, Pi, 2.9f * Pi);
    if (t > s) {
        t -= 2.0f * Pi;
    } else if (t < -s) {
        t += 2.0f * Pi;
    }
    tracked = t;
    return t;
}
// HelperTwist followed from frame to frame (`tracked`, `valid`: the caller's memory): the twist
// keeps turning the same way through +-180 degrees instead of fading to 0 there, so the helper
// never has to unwind; clamped to +-`max` and faded where the split is undefined, as HelperTwist.
// With `branch_hint` (the wrist roll followed the same way, which differs from this twist by much less
// than half a turn) the twist takes the turn nearest to it, so the helpers go round with the
// elbow, never on their own.
inline float TrackedHelperTwist(const Quat& from, const Quat& to, Vec3 axis, float max,
                                float& tracked, bool valid,
                                std::optional<float> branch_hint = {}) {
    const Quat rel = Normalize(Normalize(to) * Conjugate(Normalize(from)));
    const float along = Dot(Vec3{rel.x, rel.y, rel.z}, axis);
    const float twist = WrapPi(2.0f * std::atan2(along, rel.w));
    const float defined = Smoothstep(0.2f, 0.5f, std::sqrt(along * along + rel.w * rel.w));
    const float limit = std::clamp(max, 0.0f, Pi);
    float t;
    if (branch_hint && std::isfinite(*branch_hint)) {
        t = twist + 2.0f * Pi * std::round((*branch_hint - twist) / (2.0f * Pi));
        tracked = t;
    } else {
        // Followed on to a quarter turn past the clamp's side of 180 degrees, then round.
        t = TrackAngle(twist, tracked, valid, std::max(limit, 0.5f * Pi) + 0.5f * Pi);
    }
    return std::clamp(t, -limit, limit) * defined;
}
// How far x is past `limit` (0 below it), eased in quadratically over +-soft about it (C1).
inline float SoftExcess(float x, float limit, float soft) {
    if (!(soft > 0.0f)) {
        return std::max(x - limit, 0.0f);
    }
    if (x <= limit - soft) {
        return 0.0f;
    }
    if (x >= limit + soft) {
        return x - limit;
    }
    const float u = x - limit + soft;
    return u * u / (4.0f * soft);
}
// The rotation taking local `axisLocal` to `dir` and local `hingeLocal` to `hingeDir` (each hinge
// made orthogonal to its axis). Empty when an axis is zero or a hinge is parallel to its axis.
inline std::optional<Quat> FrameFromAxes(Vec3 axisLocal, Vec3 hingeLocal, Vec3 dir, Vec3 hingeDir) {
    const Vec3 a = Normalize(axisLocal);
    const Vec3 A = Normalize(dir);
    const Vec3 hp = Perp(Normalize(hingeLocal), a);
    const Vec3 Hp = Perp(Normalize(hingeDir), A);
    // Hinges within ~0.06 degrees of their axis define no frame.
    if (LengthSq(a) == 0.0f || LengthSq(A) == 0.0f || !(Length(hp) > 1e-3f) ||
        !(Length(Hp) > 1e-3f)) {
        return std::nullopt;
    }
    const Vec3 h = Normalize(hp);
    const Vec3 H = Normalize(Hp);
    const Quat local = FromBasis(a, h, Cross(a, h));
    const Quat world = FromBasis(A, H, Cross(A, H));
    return Normalize(world * Conjugate(local));
}

// ---- Transforms and poses -------------------------------------------------------------------

struct QsTransform {
    Vec3 t{};
    Quat q{};
    Vec3 s{1.0f, 1.0f, 1.0f};
};

inline bool IsFinite(const QsTransform& x) {
    return IsFinite(x.t) && IsFinite(x.q) && IsFinite(x.s);
}
// Havok parent * local (parent scale not applied to the translation).
inline QsTransform Compose(const QsTransform& parent, const QsTransform& local) {
    return {parent.t + Rotate(parent.q, local.t), parent.q * local.q, Mul(parent.s, local.s)};
}
// Inverse of Compose: the local transform of `model` under `parent`.
inline QsTransform LocalFromModel(const QsTransform& parent, const QsTransform& model) {
    const Quat inv = Conjugate(parent.q);
    auto div = [](float a, float b) { return std::abs(b) > 1e-12f ? a / b : a; };
    return {Rotate(inv, model.t - parent.t),
            inv * model.q,
            {div(model.s.x, parent.s.x), div(model.s.y, parent.s.y), div(model.s.z, parent.s.z)}};
}
// The parent model transform that puts a child with `childLocal` at `childModel` (rotation and
// translation; e.g. the hand transform for a weapon-bone target).
inline QsTransform ParentFromChild(const QsTransform& childModel, const QsTransform& childLocal) {
    const Quat q = Normalize(childModel.q * Conjugate(childLocal.q));
    return {childModel.t - Rotate(q, childLocal.t), q, {1.0f, 1.0f, 1.0f}};
}
// Translation and scale lerp, rotation slerp; w = 0 gives a, w = 1 gives b.
inline QsTransform Blend(const QsTransform& a, const QsTransform& b, float w) {
    if (w <= 0.0f) {
        return a;
    }
    if (w >= 1.0f) {
        return b;
    }
    return {Lerp(a.t, b.t, w), Slerp(a.q, b.q, w), Lerp(a.s, b.s, w)};
}

// Memory layout of hkQsTransformf (three hkVector4f: translation, rotation xyzw, scale), as in
// the hkaPose local array at [pose + 0x08].
struct alignas(16) HkQsTransform {
    float t[4];
    float q[4];
    float s[4];
};
static_assert(sizeof(HkQsTransform) == 0x30, "hkQsTransformf is 48 bytes");

inline QsTransform FromHavok(const HkQsTransform& h) {
    return {{h.t[0], h.t[1], h.t[2]}, {h.q[0], h.q[1], h.q[2], h.q[3]}, {h.s[0], h.s[1], h.s[2]}};
}
// Writes x into h, leaving the padding lanes (t.w, s.w) as they are.
inline void ToHavok(const QsTransform& x, HkQsTransform& h) {
    h.t[0] = x.t.x;
    h.t[1] = x.t.y;
    h.t[2] = x.t.z;
    h.q[0] = x.q.x;
    h.q[1] = x.q.y;
    h.q[2] = x.q.z;
    h.q[3] = x.q.w;
    h.s[0] = x.s.x;
    h.s[1] = x.s.y;
    h.s[2] = x.s.z;
}

constexpr int MaxChainDepth = 256;
using Size = std::array<int, 1>::size_type;

// Local -> model over a parent array (parent < 0 = root). Bones whose parent does not precede
// them are copied as roots and make the result false.
template <typename Index>
inline bool LocalToModel(const QsTransform* local, const Index* parents, int count,
                         QsTransform* model) {
    bool ok = true;
    for (int i = 0; i < count; ++i) {
        const int p = static_cast<int>(parents[i]);
        if (p < 0) {
            model[i] = local[i];
        } else if (p >= i) {
            model[i] = local[i];
            ok = false;
        } else {
            model[i] = Compose(model[p], local[i]);
        }
    }
    return ok;
}
// Model -> local (inverse of LocalToModel).
template <typename Index>
inline bool ModelToLocal(const QsTransform* model, const Index* parents, int count,
                         QsTransform* local) {
    bool ok = true;
    for (int i = 0; i < count; ++i) {
        const int p = static_cast<int>(parents[i]);
        if (p < 0) {
            local[i] = model[i];
        } else if (p >= i) {
            local[i] = model[i];
            ok = false;
        } else {
            local[i] = LocalFromModel(model[p], model[i]);
        }
    }
    return ok;
}
// Model transform of one bone, walking only its parent chain.
template <typename Index>
inline std::optional<QsTransform> ModelOf(const QsTransform* local, const Index* parents, int count,
                                          int bone) {
    if (bone < 0 || bone >= count) {
        return std::nullopt;
    }
    std::array<int, MaxChainDepth> chain{};
    Size n = 0;
    for (int b = bone; b >= 0;) {
        if (n == chain.size()) {
            return std::nullopt;
        }
        chain[n++] = b;
        const int p = static_cast<int>(parents[b]);
        if (p >= b) {
            return std::nullopt;
        }
        b = p;
    }
    QsTransform m = local[chain[n - 1]];
    for (Size k = n - 1; k-- > 0;) {
        m = Compose(m, local[chain[k]]);
    }
    return m;
}

// ---- Blend weights --------------------------------------------------------------------------

// 1 - cos(pi x / 2): starts gently, arrives firmly (the QWOP arm blend).
inline float EaseInSine(float x) {
    return 1.0f - std::cos(0.5f * Pi * std::clamp(x, 0.0f, 1.0f));
}

// Per-arm IK weight: eases in over `inSeconds` when active, out over `outSeconds` otherwise.
struct BlendWeight {
    float progress = 0.0f; // linear 0..1

    float Update(bool active, float dt, float inSeconds = 0.30f, float outSeconds = 0.08f) {
        if (!std::isfinite(dt) || dt < 0.0f) {
            dt = 0.0f;
        }
        const float span = active ? inSeconds : outSeconds;
        const float step = span > 0.0f ? dt / span : 1.0f;
        progress = active ? std::min(1.0f, progress + step) : std::max(0.0f, progress - step);
        return Value();
    }
    void Set(bool active) {
        progress = active ? 1.0f : 0.0f;
    }
    float Value() const {
        return EaseInSine(progress);
    }
};

// ---- Two-bone IK ----------------------------------------------------------------------------

struct TwoBoneLimits {
    float minFlex = 10.0f * Deg;   // the elbow never locks straight (stable hinge, natural look)
    float maxFlex = 150.0f * Deg;  // elbow fold limit: the hand cannot come closer than this
    float maxStretch = 0.10f;      // bones may lengthen this much toward targets out of reach
    float maxTargetFactor = 2.25f; // targets beyond this times the arm length: bad tracking, bail
};

// Shoulder-to-wrist distance for an elbow flexion angle (0 = straight).
inline float ChordLength(float l1, float l2, float flex) {
    return std::sqrt(std::max(0.0f, l1 * l1 + l2 * l2 + 2.0f * l1 * l2 * std::cos(flex)));
}

struct TwoBoneSolution {
    Vec3 shoulder, elbow, wrist;       // wrist = where the hand actually goes
    Vec3 axis;                         // unit shoulder -> wrist
    Vec3 bendDir;                      // unit, perpendicular to axis: the side the elbow points to
    Vec3 upperDir, foreDir;            // unit bone directions
    Vec3 hinge;                        // unit elbow hinge, upperDir x foreDir direction
    float upperLength{}, foreLength{}; // after stretch
    float stretch = 1.0f;
    float flex{};         // elbow flexion, radians
    float reachDeficit{}; // target distance minus achieved (> 0 out of reach, < 0 too close)
};

// Places the elbow on the side of `bendDir` (its component perpendicular to the shoulder ->
// target line is used; resolve near-parallel hints first, see SolveArm). `fallbackAxis` is used
// when the target sits on the shoulder. Empty for non-finite input, bones under 1 mm, or targets
// further than maxTargetFactor times the arm length.
inline std::optional<TwoBoneSolution> SolveTwoBone(Vec3 shoulder, Vec3 target, float l1, float l2,
                                                   Vec3 bendDir, const TwoBoneLimits& limits = {},
                                                   Vec3 fallbackAxis = {}) {
    if (!IsFinite(shoulder) || !IsFinite(target) || !IsFinite(bendDir) || !IsFinite(fallbackAxis) ||
        !(l1 > 1e-3f) || !(l2 > 1e-3f) || !std::isfinite(l1) || !std::isfinite(l2)) {
        return std::nullopt;
    }
    const float minFlex =
        std::clamp(std::isfinite(limits.minFlex) ? limits.minFlex : 0.0f, 0.0f, 170.0f * Deg);
    const float maxFlex =
        std::clamp(std::isfinite(limits.maxFlex) ? limits.maxFlex : 0.0f, minFlex, 175.0f * Deg);
    const float maxStretch =
        std::clamp(std::isfinite(limits.maxStretch) ? limits.maxStretch : 0.0f, 0.0f, 1.0f);
    const Vec3 d = target - shoulder;
    const float c = Length(d);
    if (!std::isfinite(c) || c > limits.maxTargetFactor * (l1 + l2)) {
        return std::nullopt;
    }
    Vec3 axis = c > 1e-6f ? d * (1.0f / c) : Normalize(fallbackAxis);
    if (LengthSq(axis) == 0.0f) {
        axis = AnyPerpendicular(Normalize(bendDir, {0.0f, 1.0f, 0.0f}));
    }
    Vec3 bend = Normalize(Perp(bendDir, axis));
    if (LengthSq(bend) == 0.0f) {
        bend = AnyPerpendicular(axis);
    }

    const float reachMax0 = ChordLength(l1, l2, minFlex);
    const float reachMin = std::max(ChordLength(l1, l2, maxFlex), 1e-4f);
    const float stretch =
        reachMax0 > 0.0f ? std::clamp(c / reachMax0, 1.0f, 1.0f + maxStretch) : 1.0f;
    const float L1 = l1 * stretch, L2 = l2 * stretch;
    const float reach = std::clamp(c, reachMin, reachMax0 * stretch);

    const float cosA =
        std::clamp((L1 * L1 + reach * reach - L2 * L2) / (2.0f * L1 * reach), -1.0f, 1.0f);
    const float sinA = std::sqrt(std::max(0.0f, 1.0f - cosA * cosA));

    TwoBoneSolution s;
    s.shoulder = shoulder;
    s.axis = axis;
    s.bendDir = bend;
    s.elbow = shoulder + axis * (L1 * cosA) + bend * (L1 * sinA);
    s.wrist = shoulder + axis * reach;
    s.upperDir = Normalize(s.elbow - shoulder, axis);
    s.foreDir = Normalize(s.wrist - s.elbow, axis);
    s.hinge = Normalize(Cross(bend, axis));
    s.upperLength = L1;
    s.foreLength = L2;
    s.stretch = stretch;
    s.flex = Angle(s.upperDir, s.foreDir);
    s.reachDeficit = c - reach;
    return s;
}

// Elbow side perpendicular to `axis` from a hint. Where the hint runs (nearly) along the axis it
// hands over smoothly to `fallback` (normally last frame's elbow side), so the elbow never
// flips through the singularity. `strength` = sine of the hint-to-axis angle.
inline Vec3 ResolveBendDir(Vec3 hint, Vec3 axis, Vec3 fallback, float* strength = nullptr) {
    Vec3 fb = Normalize(Perp(fallback, axis));
    if (LengthSq(fb) == 0.0f) {
        fb = AnyPerpendicular(axis);
    }
    const Vec3 h = Normalize(hint);
    const Vec3 pp = Perp(h, axis);
    const float m = Length(pp);
    if (strength != nullptr) {
        *strength = m;
    }
    const float w = Smoothstep(0.05f, 0.35f, m);
    if (w <= 0.0f) {
        return fb;
    }
    const Vec3 pn = pp * (1.0f / m);
    if (w >= 1.0f) {
        return pn;
    }
    return Normalize(RotateAbout(fb, axis, SignedAngle(fb, pn, axis) * w), pn);
}

// ---- Arm rig on a Havok pose ----------------------------------------------------------------

// Model-space torso axes (unit). Defaults: c0000 model space with the torso facing forward.
struct BodyFrame {
    Vec3 right{-1.0f, 0.0f, 0.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    Vec3 forward{0.0f, 0.0f, -1.0f};
};

// Elbow pole in body space (BetterVR): out to the arm's side, down and back. side = +1 for the
// right arm, -1 for the left.
inline Vec3 BodyPole(const BodyFrame& f, float side, float out = 1.0f, float down = 1.0f,
                     float back = 0.5f) {
    return f.right * (side * out) - f.up * down - f.forward * back;
}

enum class HelperMode : unsigned char {
    ElbowShare,    // child of the upper arm: `weight` of the forearm's local rotation change
    ShoulderShare, // child of the clavicle: `weight` of the upper arm's local rotation change
    WristTwist,    // child of the forearm: `weight` of the hand's twist change about the forearm
    UpperArmTwist, // child of the upper arm: adds `weight` x its twist change (negative = counter)
};

struct HelperBone {
    int bone = -1; // -1 = unused slot
    HelperMode mode = HelperMode::WristTwist;
    float weight = 0.0f;
};

constexpr int MaxHelpers = 6;

struct ArmRig {
    int clavicle = -1, upperArm = -1, forearm = -1, hand = -1;
    int effector = -1; // bone placed on the controller grip (weapon bone, child of the hand)
    float side = 1.0f; // +1 right arm, -1 left arm
    Vec3 hingeUpper{0.0f, 1.0f, 0.0f}; // elbow hinge (upperDir x foreDir) in upper-arm local space
    Vec3 hingeFore{0.0f, 1.0f, 0.0f};  // the same hinge in forearm local space
    Quat handNeutral{};                // hand local rotation with a relaxed wrist
    std::array<HelperBone, MaxHelpers> helpers{};
};

// c0000 (hunter) arms. Neutral wrist = bind pose (blade forward, about 77 deg off the forearm):
// the hand's reference local rotation of the skeleton read at runtime (core/vr/c0000_skeleton.h;
// the caller has checked C0000::Active()).
// Helper weights: elbow helpers are exactly half the forearm bend in the bind pose; twist and
// shoulder weights are first guesses for Bloodborne's skinning [U].
inline Quat HandNeutralOf(const C0000::Skeleton& sk, int hand) {
    const auto& q = sk.local[static_cast<std::size_t>(hand)].q;
    return {q[0], q[1], q[2], q[3]};
}
inline ArmRig HunterRightArm(const C0000::Skeleton& sk = C0000::Ref()) {
    ArmRig r;
    r.clavicle = 79;
    r.upperArm = 80;
    r.forearm = 81;
    r.hand = 82;
    r.effector = 98; // R_Weapon (dmy 300)
    r.side = 1.0f;
    r.handNeutral = HandNeutralOf(sk, r.hand);
    r.helpers = {{{99, HelperMode::WristTwist, 1.0f},
                  {100, HelperMode::WristTwist, 0.5f},
                  {101, HelperMode::ElbowShare, 0.5f},
                  {105, HelperMode::ShoulderShare, 0.5f},
                  {102, HelperMode::UpperArmTwist, 0.0f},
                  {103, HelperMode::UpperArmTwist, 0.0f}}};
    return r;
}
inline ArmRig HunterLeftArm(const C0000::Skeleton& sk = C0000::Ref()) {
    ArmRig r;
    r.clavicle = 50;
    r.upperArm = 51;
    r.forearm = 52;
    r.hand = 53;
    r.effector = 69; // L_Weapon (dmy 340)
    r.side = -1.0f;
    r.handNeutral = HandNeutralOf(sk, r.hand);
    r.helpers = {{{70, HelperMode::WristTwist, 1.0f},
                  {71, HelperMode::WristTwist, 0.5f},
                  {72, HelperMode::ElbowShare, 0.5f},
                  {76, HelperMode::ShoulderShare, 0.5f},
                  {73, HelperMode::UpperArmTwist, 0.0f},
                  {74, HelperMode::UpperArmTwist, 0.0f}}};
    return r;
}

// A trunk the elbow stays out of (natural arms): an elliptic cylinder along `up` from `base` (the
// waist), its axis `forwardOffset` in front of that line; the elbow's centre is kept outside
// halfWidth (along `right`) x halfDepth (along `forward`) about it, between `bottom` and `top`
// along `up` from `base` (faded in over `fade` inside each end). Model space; unit axes.
struct ElbowKeepOut {
    bool enabled = false;
    Vec3 base{};
    Vec3 up{0.0f, 1.0f, 0.0f};
    Vec3 right{-1.0f, 0.0f, 0.0f};
    Vec3 forward{0.0f, 0.0f, -1.0f};
    float halfWidth = 0.20f;
    float halfDepth = 0.145f;
    float forwardOffset = 0.025f;
    float bottom = -0.15f;
    float top = 0.44f;
    float fade = 0.08f;
};

// How deep `p` is inside the keep-out, as a fraction of its radius there (> 0 inside, 0 on the
// surface, < 0 outside), times the fade at its ends.
inline float KeepOutDepth(const ElbowKeepOut& k, Vec3 p) {
    const Vec3 d = p - k.base;
    const float h = Dot(d, k.up);
    const float f = std::max(k.fade, 1e-3f);
    const float w = Smoothstep(k.bottom, k.bottom + f, h) * (1.0f - Smoothstep(k.top - f, k.top, h));
    if (!(w > 0.0f)) {
        return -1.0f;
    }
    const float x = Dot(d, k.right) / std::max(k.halfWidth, 1e-3f);
    const float z = (Dot(d, k.forward) - k.forwardOffset) / std::max(k.halfDepth, 1e-3f);
    const float r = std::sqrt(x * x + z * z);
    return r < 1.0f ? (1.0f - r) * w : 1.0f - r;
}

struct ArmTarget {
    Vec3 position;                // model-space effector position (the controller grip)
    std::optional<Quat> rotation; // model-space effector rotation; empty keeps the animated wrist
    float rotationWeight = 1.0f;  // 1 = exact hand orientation (1:1 weapon)
};

struct ArmParams {
    TwoBoneLimits limits{};
    Vec3 pole{};               // elbow hint (model space, from the shoulder); zero = keep the
                               // animation's elbow side. See BodyPole.
    Vec3 up{0.0f, 1.0f, 0.0f}; // model up
    float weight = 1.0f;       // 0 = animated pose, 1 = full IK
    // Optional frozen source for a return to tracking. Indexed like pose, with
    // the same bone count. Only blending uses it; IK still solves the live target.
    const QsTransform* blendFrom = nullptr;
    // Clavicle reach (FRIK): from clavicleStart x arm length, ramping over clavicleRange x arm
    // length, the clavicle turns up to clavicleReach of the way toward the target. Downward
    // turns are scaled by clavicleDownScale; never more than clavicleMaxAngle.
    float clavicleReach = 0.15f;
    float clavicleStart = 0.5f;
    float clavicleRange = 0.85f;
    float clavicleDownScale = 0.4f;
    float clavicleMaxAngle = 20.0f * Deg;
    // Reach catch-up (the shoulder girdle protracting): a wrist goal still beyond the arm's
    // reach with its full stretch after the clavicle reach turns the clavicle further toward it
    // (downward turns scaled as above) by just what closes the gap, at most this much more. Only
    // targets the arm could not reach move; 0 = off (as before, bit for bit).
    float clavicleCatchUp = 0.0f;
    // The elbow follows the hand: rollToPole x wrist roll (about the forearm, from the neutral
    // wrist) turns the elbow about the shoulder-wrist line in the same sense; swingToPole pulls
    // it toward where a relaxed wrist would put it. Total turn at most maxPoleTurn.
    float rollToPole = 0.35f;
    float swingToPole = 0.30f;
    float maxPoleTurn = 75.0f * Deg;
    float rollFadeStart = 120.0f * Deg; // past this roll the roll term fades out (0 at 180)
    // The roll term fades out as the elbow folds: fully on below maxFlex - foldFadeStart, off from
    // maxFlex - foldFadeEnd (the forearm's roll axis is ill defined there).
    float foldFadeStart = 40.0f * Deg;
    float foldFadeEnd = 5.0f * Deg;
    float maxPoleTurnRate = 360.0f * Deg; // rad/s (6 degrees a frame at 60 Hz; 0 = no limit)
    float poleTau = 0.05f; // s; elbow side low-pass (0 = off). Slower near singularities.
    float maxBendRate = 360.0f * Deg;   // rad/s: the elbow side's turn about the shoulder-wrist
                                        // line, after the low-pass (0 = no limit)
    float maxWristTwist = 120.0f * Deg; // clamp on the twist handed to the twist helpers
    // Natural forearm roll (body_natural_arms, 2026-10-08; false = the proportional term above,
    // bit for bit). The forearm turns the hand by itself, like a real radius round the ulna, up to
    // rollFreePron palm down (pronation) and rollFreeSup palm up (supination) from the neutral
    // wrist: inside that range the roll never moves the elbow. Only the roll beyond it does,
    // rollExcessGain x the excess (eased in over +-rollSoft about each end), the way the shoulder
    // takes over where the forearm runs out: too much pronation turns the elbow out and up, too
    // much supination in and down. The roll is followed from frame to frame (ArmState::roll, up to
    // +-rollTrackMax), so it neither fades nor changes sign at 180 degrees: a hand rolled on past
    // it keeps its elbow where it was going. Here the turn's limit is maxPoleTurnNatural.
    bool naturalRoll = false;
    float rollFreePron = 75.0f * Deg;
    float rollFreeSup = 90.0f * Deg;
    float rollExcessGain = 0.8f;
    float rollSoft = 15.0f * Deg;
    float rollTrackMax = 240.0f * Deg;
    float maxPoleTurnNatural = 100.0f * Deg;
    // The swing term's share where it tucks the elbow in (turns it the way supination would):
    // hands held together in front (praying, a two-handed hold) bend the wrists back rather than
    // pull the elbows in under them. 1 = as the turn out.
    float swingTuckScale = 1.0f;
    // Twist helpers followed from frame to frame (ArmState::twist): continuous through 180
    // degrees (no fade toward 0 there, which made the wrist skin snap), clamped at maxWristTwist.
    // false: HelperTwist (fades out toward 180).
    bool trackTwist = false;
    // trackTwist: the helpers' twist changes at most this fast (rad/s; 0 = no limit), so even a
    // hand rolled on round past what the tracking follows unwinds them in a moment, not a frame.
    float maxTwistRate = 0.0f;
    // The trunk the elbow keeps out of (enabled: natural arms). An elbow the pole, roll and swing
    // would put inside it turns out about the shoulder-wrist line (the way pronation turns it)
    // just far enough to clear it (at most keepOutMaxTurn further), so a forearm turned palm up
    // or hands brought together in front fold the upper arms against the ribs, never into them.
    ElbowKeepOut keepOut{};
    float keepOutMaxTurn = 120.0f * Deg;
    // The upper arm and forearm this many times their animated length (your height,
    // core/vr/player_body.h: a tall player's relaxed hands land in a longer arm's). 1 = as before,
    // bit for bit. The stretch toward far targets comes on top.
    float lengthScale = 1.0f;
};

// Per-arm memory between frames. Reset when the arm stops being driven.
struct ArmState {
    Vec3 bendDir{};
    bool valid = false;
    float poleTurn = 0.0f; // the last elbow turn by the wrist (rate limit)
    // naturalRoll: the wrist roll followed continuously (radians, beyond +-180 up to rollTrackMax).
    float roll = 0.0f;
    bool rollValid = false;
    // trackTwist: the forearm and upper-arm twist handed to the helpers, followed continuously.
    std::array<float, 2> twist{};
    std::array<float, 2> twistOut{}; // what the helpers got (maxTwistRate)
    bool twistValid = false;

    void Reset() {
        *this = {};
    }
};

enum class ArmStatus : unsigned char {
    Solved,
    Skipped,        // weight 0: pose untouched
    BadRig,         // indices out of range, wrong parents, or a hinge along its bone
    BadPose,        // non-finite or degenerate animated pose
    BadTarget,      // non-finite target
    TooFar,         // target beyond maxTargetFactor x arm length (tracking or mapping error)
    NumericFailure, // non-finite result (never written)
};

constexpr int MaxWrittenBones = 5 + MaxHelpers;

struct ArmResult {
    ArmStatus status = ArmStatus::Skipped;
    bool reached = false; // effector within 2 mm of the target (at full weight)
    float error = 0.0f;   // effector distance to the target, m (full weight)
    float reachDeficit = 0.0f;
    float stretch = 1.0f;
    float flex = 0.0f;       // elbow flexion, radians
    float wristRoll = 0.0f;  // hand roll from the neutral wrist, radians (rotation targets)
    float wristSwing = 0.0f; // forearm vs relaxed-wrist forearm direction, radians
    float poleTurn = 0.0f;   // elbow turn from the pole by the wrist heuristics, radians
    float keptOut = 0.0f;    // of which the turn out of the trunk (ArmParams::keepOut), radians
    Vec3 shoulder, elbow, wrist, effector, bendDir; // model space, full weight
    std::array<int, MaxWrittenBones> written{};
    int writtenCount = 0;
};

// Solves one arm on the local pose in place. `pose` holds the animated locals on entry and the
// blended IK locals on exit (only the bones listed in ArmResult::written change). dt in seconds
// (0 = no temporal smoothing).
template <typename Index>
inline ArmResult SolveArm(QsTransform* pose, const Index* parents, int count, const ArmRig& rig,
                          const ArmTarget& target, const ArmParams& params, ArmState& state,
                          float dt = 0.0f) {
    ArmResult r;
    const auto fail = [&](ArmStatus status) {
        r.status = status;
        state.Reset();
        return r;
    };
    const int C = rig.clavicle, U = rig.upperArm, F = rig.forearm, H = rig.hand;
    const int E = rig.effector < 0 ? H : rig.effector;
    const auto in_range = [count](int b) { return b >= 0 && b < count; };
    if (pose == nullptr || parents == nullptr || !in_range(C) || !in_range(U) || !in_range(F) ||
        !in_range(H) || !in_range(E)) {
        return fail(ArmStatus::BadRig);
    }
    const auto parent_of = [parents](int b) { return static_cast<int>(parents[b]); };
    if (parent_of(U) != C || parent_of(F) != U || parent_of(H) != F ||
        (E != H && parent_of(E) != H) || parent_of(C) >= C) {
        return fail(ArmStatus::BadRig);
    }
    for (const HelperBone& hb : rig.helpers) {
        if (hb.bone < 0) {
            continue;
        }
        if (!in_range(hb.bone) || hb.bone == C || hb.bone == U || hb.bone == F || hb.bone == H ||
            hb.bone == E) {
            return fail(ArmStatus::BadRig);
        }
        const int expected = hb.mode == HelperMode::ElbowShare      ? U
                             : hb.mode == HelperMode::ShoulderShare ? C
                             : hb.mode == HelperMode::WristTwist    ? F
                                                                    : U;
        if (parent_of(hb.bone) != expected) {
            return fail(ArmStatus::BadRig);
        }
        if (std::count_if(rig.helpers.begin(), rig.helpers.end(),
                          [&hb](const HelperBone& other) { return other.bone == hb.bone; }) > 1) {
            return fail(ArmStatus::BadRig); // listed twice
        }
    }

    float weight = std::isfinite(params.weight) ? std::clamp(params.weight, 0.0f, 1.0f) : 0.0f;
    if (weight <= 0.0f) {
        return fail(ArmStatus::Skipped);
    }
    if (!std::isfinite(dt) || dt < 0.0f) {
        dt = 0.0f;
    }

    // 1. Animated chain.
    const auto valid_local = [](const QsTransform& x) {
        const float n = Dot(x.q, x.q);
        return IsFinite(x) && n > 0.5f && n < 2.0f;
    };
    QsTransform aC = pose[C], aU = pose[U], aF = pose[F], aH = pose[H];
    QsTransform aE = E == H ? QsTransform{} : pose[E];
    if (!valid_local(aC) || !valid_local(aU) || !valid_local(aF) || !valid_local(aH) ||
        !valid_local(aE)) {
        return fail(ArmStatus::BadPose);
    }
    aC.q = Normalize(aC.q);
    aU.q = Normalize(aU.q);
    aF.q = Normalize(aF.q);
    aH.q = Normalize(aH.q);
    aE.q = Normalize(aE.q);
    std::optional<QsTransform> parent_model = parent_of(C) < 0
                                                  ? std::optional<QsTransform>{QsTransform{}}
                                                  : ModelOf(pose, parents, count, parent_of(C));
    if (!parent_model || !IsFinite(*parent_model) ||
        !(Dot(parent_model->q, parent_model->q) > 0.5f)) {
        return fail(ArmStatus::BadPose);
    }
    parent_model->q = Normalize(parent_model->q);
    const float l1_anim = Length(aF.t), l2_anim = Length(aH.t);
    if (!(l1_anim > 1e-3f) || !(l2_anim > 1e-3f)) {
        return fail(ArmStatus::BadPose);
    }
    const Vec3 axis_u = aF.t * (1.0f / l1_anim);
    const Vec3 axis_f = aH.t * (1.0f / l2_anim);
    // The arm's length for this player (lengthScale 1: exactly the animated lengths).
    const float length_scale = std::isfinite(params.lengthScale)
                                   ? std::clamp(params.lengthScale, 0.5f, 2.0f)
                                   : 1.0f;
    const float l1 = l1_anim * length_scale, l2 = l2_anim * length_scale;
    if (!FrameFromAxes(axis_u, rig.hingeUpper, axis_u, rig.hingeUpper) ||
        !FrameFromAxes(axis_f, rig.hingeFore, axis_f, rig.hingeFore)) {
        return fail(ArmStatus::BadRig);
    }

    QsTransform Cm = Compose(*parent_model, aC);
    const QsTransform Um0 = Compose(Cm, aU);
    const QsTransform Fm0 = Compose(Um0, aF);
    const QsTransform Hm0 = Compose(Fm0, aH);

    // 2. Target.
    if (!IsFinite(target.position) || (target.rotation && !IsFinite(*target.rotation))) {
        return fail(ArmStatus::BadTarget);
    }
    const float rot_weight =
        std::isfinite(target.rotationWeight) ? std::clamp(target.rotationWeight, 0.0f, 1.0f) : 0.0f;
    const bool has_rot =
        target.rotation && rot_weight > 0.0f && Dot(*target.rotation, *target.rotation) > 1e-6f;
    const Quat hand_goal =
        has_rot ? Normalize(Normalize(*target.rotation) * Conjugate(aE.q)) : Hm0.q;
    Quat hand_est = has_rot ? Slerp(Hm0.q, hand_goal, rot_weight) : Hm0.q;
    Vec3 wrist_goal = target.position - Rotate(hand_est, aE.t);
    if (!(Length(wrist_goal - Um0.t) <= params.limits.maxTargetFactor * (l1 + l2))) {
        return fail(ArmStatus::TooFar);
    }
    const Vec3 up = Normalize(params.up, {0.0f, 1.0f, 0.0f});

    // 3. Clavicle reach.
    bool clavicle_turned = false;
    if (params.clavicleReach > 0.0f) {
        const float arm = l1 + l2;
        const float dist = Length(wrist_goal - Um0.t);
        const float range = std::max(params.clavicleRange, 1e-3f) * arm;
        const float f = std::clamp((dist - params.clavicleStart * arm) / range, 0.0f, 1.0f) *
                        std::min(params.clavicleReach, 1.0f);
        const Vec3 c0 = Normalize(Um0.t - Cm.t);
        Vec3 toward = Normalize(wrist_goal - Cm.t);
        if (f > 0.0f && LengthSq(c0) > 0.0f && LengthSq(toward) > 0.0f) {
            const float dy = Dot(toward, up) - Dot(c0, up);
            if (dy < 0.0f) {
                toward = Normalize(toward - up * (dy * (1.0f - params.clavicleDownScale)), toward);
            }
            const Quat full = FromTo(c0, toward);
            const float angle =
                std::min(AngleOf(full) * f, std::max(params.clavicleMaxAngle, 0.0f));
            Cm.q = Normalize(AxisAngle(Vec3{full.x, full.y, full.z}, angle) * Cm.q);
            clavicle_turned = angle > 0.0f;
        }
    }
    // 3b. Reach catch-up: turning the shoulder about the clavicle toward the wrist goal brings it
    // monotonically nearer (up to pointing at it), so the smallest turn that closes the gap is
    // found by bisection.
    if (params.clavicleCatchUp > 0.0f) {
        const float min_flex = std::clamp(
            std::isfinite(params.limits.minFlex) ? params.limits.minFlex : 0.0f, 0.0f, 170.0f * Deg);
        const float max_stretch = std::clamp(
            std::isfinite(params.limits.maxStretch) ? params.limits.maxStretch : 0.0f, 0.0f, 1.0f);
        const float reach_max = ChordLength(l1, l2, min_flex) * (1.0f + max_stretch);
        const Vec3 s0 = Cm.t + Rotate(Cm.q, aU.t);
        const Vec3 c0 = Normalize(s0 - Cm.t);
        Vec3 toward = Normalize(wrist_goal - Cm.t);
        if (Length(wrist_goal - s0) > reach_max && LengthSq(c0) > 0.0f &&
            LengthSq(toward) > 0.0f) {
            const float dy = Dot(toward, up) - Dot(c0, up);
            if (dy < 0.0f) {
                toward = Normalize(toward - up * (dy * (1.0f - params.clavicleDownScale)), toward);
            }
            const Quat full = FromTo(c0, toward);
            const Vec3 turn_axis{full.x, full.y, full.z};
            // The shoulder drops much less far than it moves forward or up: a turn that takes it
            // down gets clavicleDownScale of the limit (straight down) .. all of it (level, up).
            const float down =
                std::clamp(-Dot(Normalize(toward - c0 * Dot(toward, c0)), up), 0.0f, 1.0f);
            const float limit =
                std::min(params.clavicleCatchUp, 60.0f * Deg) *
                (1.0f - (1.0f - std::clamp(params.clavicleDownScale, 0.0f, 1.0f)) * down);
            const float max_angle = std::min(AngleOf(full), limit);
            if (LengthSq(turn_axis) > 1e-12f && max_angle > 0.0f) {
                const auto gap = [&](float a) {
                    const Quat q = Normalize(AxisAngle(turn_axis, a) * Cm.q);
                    return Length(wrist_goal - (Cm.t + Rotate(q, aU.t))) - reach_max;
                };
                float angle = max_angle;
                if (gap(max_angle) < 0.0f) {
                    float lo = 0.0f, hi = max_angle;
                    for (int i = 0; i < 12; ++i) {
                        const float mid = 0.5f * (lo + hi);
                        (gap(mid) > 0.0f ? lo : hi) = mid;
                    }
                    angle = hi;
                }
                Cm.q = Normalize(AxisAngle(turn_axis, angle) * Cm.q);
                clavicle_turned = true;
            }
        }
    }
    const Quat Uq_parent = Cm.q;
    const Vec3 S = Cm.t + Rotate(Cm.q, aU.t);

    // 4. Elbow side.
    Vec3 axis = Normalize(wrist_goal - S, Normalize(Hm0.t - S, -up));
    const Vec3 anim_side = Perp(Fm0.t - Um0.t, Normalize(Hm0.t - Um0.t, axis));
    const Vec3 anim_fallback = LengthSq(anim_side) > 1e-8f ? anim_side : -up;
    const Vec3 hint = LengthSq(params.pole) > 1e-12f ? params.pole : anim_fallback;
    float strength = 1.0f;
    Vec3 bend = ResolveBendDir(hint, axis, state.valid ? state.bendDir : anim_fallback, &strength);
    const Vec3 base = bend;
    const float max_turn = std::clamp(
        params.naturalRoll ? params.maxPoleTurnNatural : params.maxPoleTurn, 0.0f, Pi);
    float turn = 0.0f;
    bool roll_followed = false;
    float roll_branch = 0.0f; // the followed roll (naturalRoll), for the twist helpers' turn
    if (has_rot &&
        (params.rollToPole != 0.0f || params.swingToPole != 0.0f || params.naturalRoll)) {
        const auto sol0 = SolveTwoBone(S, wrist_goal, l1, l2, bend, params.limits, axis);
        const auto Rf0 =
            sol0 ? FrameFromAxes(axis_f, rig.hingeFore, sol0->foreDir, sol0->hinge) : std::nullopt;
        if (Rf0) {
            const Quat neutral = Normalize(rig.handNeutral);
            // Wrist roll about the pre-turn forearm (the pole's elbow side, before this frame's
            // turn), from the neutral wrist. The old term is stateless and periodic: full effect
            // up to rollFadeStart, none at 180 degrees (where +180 and -180 are the same hand, so
            // the term never changes sign there). Both: none where a near-180 wrist swing leaves
            // the roll undefined, and none as the elbow folds to its limit (the forearm turns fast
            // about the short shoulder-wrist line there).
            const Quat dev = Normalize(Conjugate(*Rf0) * hand_goal * Conjugate(neutral));
            const float along = Dot(Vec3{dev.x, dev.y, dev.z}, axis_f);
            const float roll = WrapPi(2.0f * std::atan2(along, dev.w));
            const float defined = Smoothstep(0.2f, 0.5f, std::sqrt(along * along + dev.w * dev.w));
            const float fade =
                1.0f - Smoothstep(std::min(params.rollFadeStart, 0.99f * Pi), Pi, std::abs(roll));
            const float max_flex = std::clamp(params.limits.maxFlex, 0.0f, 175.0f * Deg);
            const float fold_start = std::max(params.foldFadeStart, 1e-3f);
            const float fold_end = std::clamp(params.foldFadeEnd, 0.0f, fold_start - 1e-3f);
            const float fold =
                1.0f - Smoothstep(max_flex - fold_start, max_flex - fold_end, sol0->flex);
            if (params.naturalRoll) {
                // The roll followed on from last frame's (no wrap, no fade at 180), and only its
                // part beyond the forearm's own range turns the elbow (+ = pronation).
                const float soft = std::max(params.rollSoft, 0.0f);
                const auto excess_of = [&](float x) { // + = pronation beyond its range
                    const float p = x * rig.side;
                    return SoftExcess(p, std::max(params.rollFreePron, 0.0f), soft) -
                           SoftExcess(-p, std::max(params.rollFreeSup, 0.0f), soft);
                };
                if (!state.rollValid) {
                    // A first roll near 180 degrees is either way round: take the way the
                    // forearm and shoulder strain least for.
                    const float alt = roll - (roll > 0.0f ? 2.0f * Pi : -2.0f * Pi);
                    state.roll =
                        std::abs(excess_of(alt)) < std::abs(excess_of(roll)) ? alt : roll;
                }
                const float followed = TrackAngle(roll, state.roll, true, params.rollTrackMax);
                roll_followed = true;
                roll_branch = followed;
                r.wristRoll = followed;
                turn = std::clamp(params.rollExcessGain * excess_of(followed) * rig.side *
                                      defined * fold,
                                  -max_turn, max_turn);
            } else {
                r.wristRoll = roll;
                turn = std::clamp(params.rollToPole * roll * fade * defined * fold, -max_turn,
                                  max_turn);
            }
            // Wrist swing: a relaxed wrist would put the forearm along f_star, so the elbow wants
            // to be on the opposite side of the shoulder-wrist line. Fades out before it would
            // have to choose a way around (no flips).
            const Vec3 f_star = Rotate(Normalize(hand_goal * Conjugate(neutral)), axis_f);
            const Vec3 ideal = -Perp(f_star, axis);
            const float ideal_len = Length(ideal);
            if (ideal_len > 1e-4f) {
                const Vec3 rolled = RotateAbout(base, axis, turn);
                const float phi = SignedAngle(rolled, ideal * (1.0f / ideal_len), axis);
                if (params.swingTuckScale != 1.0f && phi * params.swingToPole * rig.side < 0.0f) {
                    // Tucking the elbow in.
                    turn += phi * params.swingToPole * Smoothstep(0.15f, 0.5f, ideal_len) *
                            (1.0f - Smoothstep(0.5f * Pi, 0.85f * Pi, std::abs(phi))) *
                            params.swingTuckScale;
                } else {
                    // (The expression as it was, so the old arms stay bit for bit.)
                    turn += phi * params.swingToPole * Smoothstep(0.15f, 0.5f, ideal_len) *
                            (1.0f - Smoothstep(0.5f * Pi, 0.85f * Pi, std::abs(phi)));
                }
            }
            turn = std::clamp(turn, -max_turn, max_turn);
        }
    }
    state.rollValid = roll_followed;
    // The trunk: an elbow inside it turns out (the pronation sense, rig.side) just far enough to
    // clear it (5-degree steps, then bisection), the hand where it is.
    if (params.keepOut.enabled) {
        const auto solk = SolveTwoBone(S, wrist_goal, l1, l2, base, params.limits, axis);
        if (solk) {
            const Vec3 off = solk->elbow - S;
            const Vec3 along = axis * Dot(off, axis);
            const Vec3 radial = off - along;
            const auto depth = [&](float a) {
                return KeepOutDepth(params.keepOut, S + along + RotateAbout(radial, axis, a));
            };
            if (depth(turn) > 0.0f) {
                const float limit = std::clamp(params.keepOutMaxTurn, 0.0f, Pi);
                const float step = 5.0f * Deg;
                float lo = 0.0f, hi = -1.0f;
                for (float a = step; a <= limit + 1e-4f; a += step) {
                    if (depth(turn + rig.side * a) <= 0.0f) {
                        hi = a;
                        break;
                    }
                    lo = a;
                }
                if (hi > 0.0f) {
                    for (int i = 0; i < 8; ++i) {
                        const float mid = 0.5f * (lo + hi);
                        (depth(turn + rig.side * mid) > 0.0f ? lo : hi) = mid;
                    }
                    turn += rig.side * hi;
                    r.keptOut = hi;
                }
            }
        }
    }
    // The turn changes at most maxPoleTurnRate (the roll and swing terms can change fast where
    // the arm folds or the hand passes the shoulder: the elbow would whip round).
    if (state.valid && dt > 0.0f && params.maxPoleTurnRate > 0.0f) {
        const float step = params.maxPoleTurnRate * dt;
        turn = std::clamp(turn, state.poleTurn - step, state.poleTurn + step);
    }
    state.poleTurn = turn;
    if (turn != 0.0f) {
        bend = RotateAbout(base, axis, turn);
    }
    r.poleTurn = turn;
    if (state.valid && dt > 0.0f && (params.poleTau > 0.0f || params.maxBendRate > 0.0f)) {
        const Vec3 prev = Normalize(Perp(state.bendDir, axis));
        if (LengthSq(prev) > 0.0f) {
            float angle = SignedAngle(prev, bend, axis);
            if (params.poleTau > 0.0f) {
                // Weak hints (pole almost along the arm) are trusted less: ~3x slower follow.
                const float tau =
                    params.poleTau * (1.0f + 2.0f * (1.0f - Smoothstep(0.1f, 0.35f, strength)));
                angle *= 1.0f - std::exp(-dt / tau);
            }
            // A pole that regains strength far from where the elbow is (the hand passing the
            // pole's direction, the torso turned far round) swings the elbow round the
            // shoulder-wrist line at most this fast; the line itself moves with the hand freely.
            if (params.maxBendRate > 0.0f) {
                angle = std::clamp(angle, -params.maxBendRate * dt, params.maxBendRate * dt);
            }
            bend = Normalize(RotateAbout(prev, axis, angle), bend);
        }
    }
    state.bendDir = bend;
    state.valid = true;

    // 5./6. Solve; re-solve when the wrist estimate used a different hand rotation.
    std::optional<TwoBoneSolution> sol;
    std::optional<Quat> Ru, Rf;
    Quat Rh{};
    const bool exact_hand = has_rot && rot_weight >= 1.0f;
    for (int pass = 0; pass < 4; ++pass) {
        sol = SolveTwoBone(S, wrist_goal, l1, l2, bend, params.limits, axis);
        Ru = sol ? FrameFromAxes(axis_u, rig.hingeUpper, sol->upperDir, sol->hinge) : std::nullopt;
        Rf = sol ? FrameFromAxes(axis_f, rig.hingeFore, sol->foreDir, sol->hinge) : std::nullopt;
        if (!sol || !Ru || !Rf) {
            return fail(ArmStatus::NumericFailure);
        }
        const Quat follow = Normalize(*Rf * aH.q);
        Rh = exact_hand ? hand_goal : has_rot ? Slerp(follow, hand_goal, rot_weight) : follow;
        if (exact_hand) {
            break;
        }
        // The hand follows the new forearm, which moves the wrist goal a little: iterate.
        const Vec3 next = target.position - Rotate(Rh, aE.t);
        if (Length(next - wrist_goal) < 1e-5f) {
            break;
        }
        wrist_goal = next;
    }

    // New locals.
    QsTransform nC = aC, nU = aU, nF = aF, nH = aH;
    nC.q = Normalize(Conjugate(parent_model->q) * Uq_parent);
    nU.q = Normalize(Conjugate(Uq_parent) * *Ru);
    nF.q = Normalize(Conjugate(*Ru) * *Rf);
    const float grow = length_scale * sol->stretch; // == stretch when lengthScale is 1
    nF.t = aF.t * grow;
    nH.q = Normalize(Conjugate(*Rf) * Rh);
    nH.t = aH.t * grow;

    std::array<QsTransform, MaxHelpers> helper_new{};
    const float max_twist = std::clamp(params.maxWristTwist, 0.0f, Pi);
    // The forearm's and the upper arm's twist (for their helpers), once each.
    std::array<std::optional<float>, 2> twist{};
    const auto twist_of = [&](int k) {
        if (!twist[static_cast<Size>(k)]) {
            const Quat& from = k == 0 ? aH.q : aU.q;
            const Quat& to = k == 0 ? nH.q : nU.q;
            const Vec3 ax = k == 0 ? axis_f : axis_u;
            float d = params.trackTwist
                          ? TrackedHelperTwist(from, to, ax, max_twist,
                                               state.twist[static_cast<Size>(k)], state.twistValid,
                                               k == 0 && roll_followed
                                                   ? std::optional<float>{roll_branch}
                                                   : std::nullopt)
                          : HelperTwist(from, to, ax, max_twist);
            if (params.trackTwist) {
                float& last = state.twistOut[static_cast<Size>(k)];
                if (state.twistValid && dt > 0.0f && params.maxTwistRate > 0.0f) {
                    d = std::clamp(d, last - params.maxTwistRate * dt,
                                   last + params.maxTwistRate * dt);
                }
                last = d;
            }
            twist[static_cast<Size>(k)] = d;
        }
        return *twist[static_cast<Size>(k)];
    };
    for (Size i = 0; i < rig.helpers.size(); ++i) {
        const HelperBone& hb = rig.helpers[i];
        if (hb.bone < 0) {
            continue;
        }
        const QsTransform anim = pose[hb.bone];
        if (!valid_local(anim)) {
            return fail(ArmStatus::BadPose);
        }
        QsTransform out = anim;
        const Quat q0 = Normalize(anim.q);
        switch (hb.mode) {
        case HelperMode::ElbowShare:
            out.q = Normalize(ScaleRotation(nF.q * Conjugate(aF.q), hb.weight) * q0);
            break;
        case HelperMode::ShoulderShare:
            out.q = Normalize(ScaleRotation(nU.q * Conjugate(aU.q), hb.weight) * q0);
            break;
        case HelperMode::WristTwist: {
            const float d = twist_of(0);
            out.q = Normalize(AxisAngle(axis_f, d * hb.weight) * q0);
            break;
        }
        case HelperMode::UpperArmTwist: {
            const float d = twist_of(1);
            out.q = Normalize(AxisAngle(axis_u, d * hb.weight) * q0);
            break;
        }
        }
        helper_new[i] = out;
    }
    if (params.trackTwist) {
        // Both followed every frame (a rig without one of them keeps its 0).
        twist_of(0);
        twist_of(1);
    }
    state.twistValid = params.trackTwist;

    // Result at full weight.
    r.shoulder = S;
    r.elbow = sol->elbow;
    r.wrist = sol->wrist;
    r.effector = E == H ? sol->wrist : sol->wrist + Rotate(Rh, aE.t);
    r.bendDir = sol->bendDir;
    r.error = Length(r.effector - target.position);
    r.reached = r.error <= 0.002f;
    r.reachDeficit = sol->reachDeficit;
    r.stretch = sol->stretch;
    r.flex = sol->flex;
    if (has_rot) {
        const Vec3 f_star =
            Rotate(Normalize(hand_goal * Conjugate(Normalize(rig.handNeutral))), axis_f);
        r.wristSwing = Angle(f_star, sol->foreDir);
    }

    // 7. Guard, blend, write.
    bool finite = IsFinite(nC) && IsFinite(nU) && IsFinite(nF) && IsFinite(nH) &&
                  IsFinite(r.effector) && IsFinite(r.elbow);
    for (Size i = 0; i < rig.helpers.size(); ++i) {
        finite = finite && (rig.helpers[i].bone < 0 || IsFinite(helper_new[i]));
    }
    if (!finite) {
        return fail(ArmStatus::NumericFailure);
    }
    const auto write = [&](int bone, const QsTransform& anim, const QsTransform& ik) {
        pose[bone] = Blend(params.blendFrom ? params.blendFrom[bone] : anim, ik, weight);
        r.written[static_cast<Size>(r.writtenCount++)] = bone;
    };
    if (clavicle_turned || params.blendFrom) {
        write(C, aC, nC);
    }
    write(U, aU, nU);
    write(F, aF, nF);
    write(H, aH, nH);
    for (Size i = 0; i < rig.helpers.size(); ++i) {
        const HelperBone& hb = rig.helpers[i];
        if (hb.bone >= 0) {
            write(hb.bone, pose[hb.bone], helper_new[i]);
        }
    }
    r.status = ArmStatus::Solved;
    return r;
}

} // namespace VR::Body
