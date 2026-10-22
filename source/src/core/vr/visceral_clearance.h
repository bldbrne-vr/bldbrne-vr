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
#include <limits>
#include <optional>
#include <span>
#include "core/vr/body_frame.h"

namespace VR::GrabCamera {

struct CollisionCapsule {
    Body::Vec3 a{}, b{};
    float radius{};
};

struct SurfaceRay {
    bool hit{false};
    bool projected{false}; // eye-level miss: conservative horizontal silhouette
    float distance{};      // signed: negative means the eye starts inside the body
    Body::Vec3 direction{}; // horizontal unit vector toward the victim
};

// Intersect the whole line with the capsule, then keep intersections touching the
// forward ray. A signed entry lets a camera already inside move out through the
// near surface, rather than incorrectly measuring distance to the far surface.
inline std::optional<float> RayCapsule(Body::Vec3 eye, Body::Vec3 dir,
                                       const CollisionCapsule& c) {
    if (!Body::IsFinite(eye) || !Body::IsFinite(dir) || !Body::IsFinite(c.a) ||
        !Body::IsFinite(c.b) || !std::isfinite(c.radius) || c.radius <= 0.0f ||
        std::abs(Body::LengthSq(dir) - 1.0f) > 0.001f)
        return std::nullopt;
    const auto axis = c.b - c.a;
    const float length = Body::Length(axis);
    const auto u = length > 1e-6f ? axis * (1.0f / length) : Body::Vec3{};
    float entry = std::numeric_limits<float>::infinity();
    float exit = -entry;
    auto add = [&](float t) {
        entry = std::min(entry, t);
        exit = std::max(exit, t);
    };
    auto sphere = [&](Body::Vec3 centre, int cap) {
        const auto v = eye - centre;
        const float b = Body::Dot(v, dir);
        const float disc = b * b - Body::LengthSq(v) + c.radius * c.radius;
        if (disc < 0.0f) return;
        const float h = std::sqrt(disc);
        for (float t : {-b - h, -b + h}) {
            const float axial = Body::Dot(eye + dir * t - c.a, u);
            if (cap == 0 || (cap < 0 && axial <= 0.0f) ||
                (cap > 0 && axial >= length))
                add(t);
        }
    };
    if (length <= 1e-6f) {
        sphere(c.a, 0);
    } else {
        const auto v = eye - c.a;
        const float du = Body::Dot(dir, u), vu = Body::Dot(v, u);
        const auto dp = dir - u * du, vp = v - u * vu;
        const float a = Body::LengthSq(dp), b = Body::Dot(dp, vp);
        const float disc = b * b - a * (Body::LengthSq(vp) - c.radius * c.radius);
        if (a > 1e-8f && disc >= 0.0f) {
            const float h = std::sqrt(disc);
            for (float t : {(-b - h) / a, (-b + h) / a}) {
                const float axial = vu + t * du;
                if (axial >= 0.0f && axial <= length) add(t);
            }
        }
        sphere(c.a, -1);
        sphere(c.b, 1);
    }
    if (!std::isfinite(entry) || exit < 0.0f) return std::nullopt;
    return entry;
}

inline SurfaceRay CastVictimSurface(Body::Vec3 eye, Body::Vec3 victim,
                                    Body::Vec3 facing, float scale,
                                    std::span<const CollisionCapsule> capsules) {
    SurfaceRay out{};
    if (!Body::IsFinite(eye) || !Body::IsFinite(victim) ||
        !std::isfinite(scale) || scale <= 0.0f)
        return out;
    const Body::Vec3 flat{victim.x - eye.x, 0.0f, victim.z - eye.z};
    const Body::Vec3 forward{facing.x, 0.0f, facing.z};
    if (Body::LengthSq(flat) < 1e-8f &&
        (!Body::IsFinite(forward) || Body::LengthSq(forward) < 1e-8f))
        return out;
    out.direction = Body::Normalize(flat, Body::Normalize(forward));
    // First use the actual body at eye height. Only a miss uses its horizontal
    // silhouette, so crouches and gaps cannot silently remove all protection.
    for (bool projected : {false, true}) {
        float nearest = std::numeric_limits<float>::infinity();
        for (auto c : capsules) {
            if (!Body::IsFinite(c.a) || !Body::IsFinite(c.b)) continue;
            if (projected) c.a.y = c.b.y = eye.y;
            if (const auto t = RayCapsule(eye, out.direction, c);
                t && *t >= -3.0f * scale && *t <= 6.0f * scale)
                nearest = std::min(nearest, *t);
        }
        if (std::isfinite(nearest)) {
            out.hit = true;
            out.projected = projected;
            out.distance = nearest;
            break;
        }
    }
    return out;
}

} // namespace VR::GrabCamera
