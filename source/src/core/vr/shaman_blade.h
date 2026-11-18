// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include "core/vr/body_frame.h"
#include "core/vr/evasion_hitgeom.h"
#include "core/vr/visceral_clearance.h"

namespace VR::ShamanBlade {
inline constexpr int Goods = 2090;
inline constexpr std::uint32_t NoTarget = 0xffffffffu;
inline constexpr bool CharacterHandle(std::uint32_t h) { return (h >> 28) == 1; }
inline bool ValidScale(float s) { return std::isfinite(s) && s >= .01f && s <= 100.0f; }
// The native contact sphere is 0.04 game units. Never accept a geometric gap
// larger than that sphere can bridge, even at an extreme VR world scale. 3 cm (1.2 cm until
// 2026-11-04: the hit capsules are slimmer than the body drawn, and real stabs missed).
inline float ContactRadius(float scale) { return std::min(.03f*scale,.035f); }
inline constexpr std::int64_t MaxAgeNs = 80'000'000;
using Body::Vec3;
inline bool Finite(Vec3 p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
struct Contact {
    std::int64_t ns{}, session{};
    std::uint32_t stroke{}, space{};
    Vec3 point{};
    std::uint32_t target{NoTarget}; // native handle, including its generation; never a pointer
    Vec3 blade_base{};
};
inline bool Fresh(const Contact& c, std::int64_t now, std::int64_t session,
                  std::uint32_t stroke, std::uint32_t space) {
    return c.ns > 0 && now >= c.ns && now - c.ns <= MaxAgeNs && session > 0 &&
           c.session == session && stroke != 0 && c.stroke == stroke && c.space == space &&
           CharacterHandle(c.target) && Finite(c.point) && Finite(c.blade_base);
}

// Controller-local motion only. Walking with a stationary hand cannot arm a blade.
// The first 6 cm of a continuous deliberate stroke are its wind-up, not an attack.
class Stroke {
public:
    bool Step(std::int64_t session, double t, Vec3 relative, bool valid) {
        if (session != session_) { *this = {}; session_ = session; }
        const double dt = t - last_;
        last_ = t;
        if (!valid || session <= 0 || !std::isfinite(t) || !Finite(relative) ||
            dt <= 0 || dt > .05) { travel_ = 0; active_ = false; return false; }
        const float speed = Body::Length(relative);
        if (speed < .7f || speed > 12.0f) {
            travel_ = 0; active_ = false; spent_ = false; return false;
        }
        if (spent_) return false;
        if (!active_ && speed >= 1.2f) travel_ += speed * static_cast<float>(dt);
        if (!active_ && travel_ >= .06f) {
            active_ = true;
            if (++serial_ == 0) ++serial_;
        }
        return active_;
    }
    void Consume() { spent_ = true; active_ = false; }
    std::uint32_t Serial() const { return serial_; }
private:
    std::int64_t session_{};
    double last_{};
    float travel_{};
    std::uint32_t serial_{};
    bool active_{}, spent_{};
};

// A short blade, including rotation between samples. Fixed work, no heap storage.
// Reject discontinuities rather than sweeping a blade through a recenter/teleport.
inline bool Touch(Vec3 a0, Vec3 b0, Vec3 a1, Vec3 b1, float scale,
                  const GrabCamera::CollisionCapsule& c, Vec3& point) {
    if (!Finite(a0) || !Finite(b0) || !Finite(a1) || !Finite(b1) ||
        !Finite(c.a) || !Finite(c.b) || !ValidScale(scale) ||
        !std::isfinite(c.radius) || c.radius <= 0 || c.radius > 100.0f * scale ||
        Body::Length(a1-a0) > .35f*scale || Body::Length(b1-b0) > .35f*scale) return false;
    // Almost every body part is outside the blade's swept bounds. Reject those before
    // doing any subdivided capsule work; interpolation stays within these bounds.
    const float r = c.radius + ContactRadius(scale);
    const auto separated = [r](float a, float b, float d, float e, float lo, float hi) {
        return std::max({a,b,d,e}) < std::min(lo,hi)-r ||
               std::min({a,b,d,e}) > std::max(lo,hi)+r;
    };
    if (separated(a0.x,b0.x,a1.x,b1.x,c.a.x,c.b.x) ||
        separated(a0.y,b0.y,a1.y,b1.y,c.a.y,c.b.y) ||
        separated(a0.z,b0.z,a1.z,b1.z,c.a.z,c.b.z)) return false;
    const float travel = std::max(Body::Length(a1-a0), Body::Length(b1-b0));
    const int steps = std::clamp(static_cast<int>(std::ceil(travel/(.01f*scale))), 1, 35);
    const auto e = [](Vec3 p) { return Evade::Vec3{p.x,p.y,p.z}; };
    for (int i=0; i<=steps; ++i) {
        const float t = static_cast<float>(i)/steps;
        Evade::Vec3 p{}, q{};
        if (Evade::SegSegClosest(e(a0+(a1-a0)*t), e(b0+(b1-b0)*t), e(c.a), e(c.b), p,q) <= r*r) {
            point = {p.x,p.y,p.z};
            return true;
        }
    }
    return false;
}
} // namespace VR::ShamanBlade
