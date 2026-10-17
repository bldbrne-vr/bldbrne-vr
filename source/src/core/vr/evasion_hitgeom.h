// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/vr/evasion.h"

// Physical evasion's hit log (core/vr/bloodborne_evasion.cpp, VR_EVADE_HITLOG): the geometry of
// one enemy attack cast against the hunter's hurtboxes. The game sweeps the attack's capsule (or
// sphere) in a straight line from where it was to where it is this frame; each hurtbox is a
// static capsule. The log tests every attack near the player twice: against the hurtboxes where
// they are (shifted by the lean: should agree with the game's own hit) and where they would be
// without the lean (vanilla). A vanilla hit that neither the leaned bodies nor the game had is a
// blow you leaned out of.
//
// Pure (no emulator dependencies) so unit tests can drive it (tests/test_evasion_hitgeom.cpp).
namespace VR::Evade {

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

// Closest points of segments p1q1 and p2q2 (Ericson, Real-Time Collision Detection 5.1.9;
// degenerate segments are points). Returns the squared distance; c1 / c2 get the points.
inline float SegSegClosest(const Vec3& p1, const Vec3& q1, const Vec3& p2, const Vec3& q2, Vec3& c1,
                           Vec3& c2) {
    constexpr float Eps = 1e-9f;
    const Vec3 d1 = q1 - p1;
    const Vec3 d2 = q2 - p2;
    const Vec3 r = p1 - p2;
    const float a = Dot(d1, d1);
    const float e = Dot(d2, d2);
    const float f = Dot(d2, r);
    float s = 0.0f;
    float t = 0.0f;
    if (a <= Eps && e <= Eps) {
        c1 = p1;
        c2 = p2;
        return Dot(c1 - c2, c1 - c2);
    }
    if (a <= Eps) {
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = Dot(d1, r);
        if (e <= Eps) {
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = Dot(d1, d2);
            const float denom = a * e - b * b;
            s = denom > Eps ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = std::clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = std::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    c1 = p1 + d1 * s;
    c2 = p2 + d2 * t;
    return Dot(c1 - c2, c1 - c2);
}

inline float SegSegDist2(const Vec3& p1, const Vec3& q1, const Vec3& p2, const Vec3& q2) {
    Vec3 c1, c2;
    return SegSegClosest(p1, q1, p2, q2, c1, c2);
}

// A capsule (a, b, r) moved by `sweep` during the frame against a static capsule (c, d, rh):
// sampled every <= a quarter of the smaller radius of travel (1..64 steps, plus both ends).
// clearance = the smallest distance between the two surfaces (< 0: they touch); `t` is where
// along the sweep (0..1) and `point` the attack's closest point there.
struct Sweep {
    bool hit{false};
    float clearance{1e9f};
    float t{0.0f};
    Vec3 point{};
};

inline Sweep SweptCapsule(const Vec3& a, const Vec3& b, float r, const Vec3& sweep, const Vec3& c,
                          const Vec3& d, float rh) {
    const float travel = std::sqrt(Dot(sweep, sweep));
    const float step = std::max(0.25f * std::min(r, rh), 0.005f);
    const int steps = std::clamp(static_cast<int>(std::ceil(travel / step)), 1, 64);
    Sweep out{};
    for (int i = 0; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const Vec3 o = sweep * t;
        Vec3 p, q;
        const float dist = std::sqrt(SegSegClosest(a + o, b + o, c, d, p, q)) - r - rh;
        if (dist < out.clearance) {
            out.clearance = dist;
            out.t = t;
            out.point = p;
        }
    }
    out.hit = out.clearance < 0.0f;
    return out;
}

// One attack against the hurtboxes: hit where they would be without the lean (vanilla), where
// they are (leaned), and the game's own verdict (a cast hit on the player).
enum class HitClass : std::uint8_t {
    None,    // neither: a near miss either way
    Evaded,  // the vanilla body was hit, the leaned one not, nor did the game: you leaned out of it
    Traded,  // the game hit you, the vanilla body would not have been: the lean moved you into it
    Both,    // hit with or without the lean
    Ignored, // the leaned body was hit but the game did not count it (i-frames, a grab, its lists)
};

constexpr HitClass Classify(bool vanilla, bool leaned, bool game) {
    if (game) {
        return vanilla ? HitClass::Both : HitClass::Traded;
    }
    if (leaned) {
        return HitClass::Ignored;
    }
    return vanilla ? HitClass::Evaded : HitClass::None;
}

constexpr const char* HitClassName(HitClass c) {
    switch (c) {
    case HitClass::Evaded:
        return "EVADED";
    case HitClass::Traded:
        return "TRADED";
    case HitClass::Both:
        return "BOTH";
    case HitClass::Ignored:
        return "IGNORED";
    case HitClass::None:
        break;
    }
    return "NONE";
}

} // namespace VR::Evade
