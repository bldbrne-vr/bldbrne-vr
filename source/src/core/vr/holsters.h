// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include "core/vr/belt_vial.h"

namespace VR::Holsters {
using P3 = Belt::P3;
inline float Distance(P3 a, P3 b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
                     (a.z - b.z) * (a.z - b.z));
}
// Right: vials. Left pocket/chest: equipped quick slots 1/2, in game inventory order.
inline constexpr std::array<int, 3> InventoryAssignment{1, 2, 3};
struct Config {
    std::array<int, 3> assignment{InventoryAssignment}; // 0 disables a target
    float scale{1}, reach{0.12f};
};
// Tracking-space positions shared by hit testing and the world renderer. No independent
// renderer placement: the visible case and the target can never drift apart.
struct Layout {
    std::array<P3, 3> centre{};
    std::array<int, 3> assignment{};
    int hover{-1};
    bool valid{false}, entered{false};
    // Unit inward axis of the rendered vial pouch (toward the body), in tracking space.
    P3 pouch_back{0, 0, 1};
    // Unit right/up/back axes of the rendered chest mount, in tracking space.
    std::array<P3, 3> chest_axes{P3{1, 0, 0}, P3{0, 1, 0}, P3{0, 0, 1}};
};
// The rendered positions, returned to the left-hand pickup test. Published by the
// render-camera hook, so grabbing uses the actual displayed mount, not an HMD estimate.
struct Targets {
    Layout layout{};
    std::int64_t ns{};
    std::uint32_t space{};
};
inline bool Fresh(const Targets& t, std::int64_t now, std::uint32_t space) {
    if (!t.layout.valid || t.space != space || t.ns <= 0 || now < t.ns || now - t.ns > 100'000'000)
        return false;
    for (const auto& p : t.layout.centre)
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            return false;
    return true;
}
// Additional vertical half-height for the chest grab ellipsoid (metres).
inline constexpr float ChestVerticalExtraM = .02f;
inline constexpr float ChestDepthReductionM = .09f;
inline constexpr float PouchVerticalExtraM = .01f;
// The vials' pouch zone's reach toward the left mount (metres, Tracker::Follow).
inline constexpr float PouchLeftM = .06f;
inline constexpr float PouchBackM = .03f;

// Distance to a bounded parallelogram [0,left] + [0,back]. Sweeping the existing
// pouch volume inward preserves all its old points and its outward boundary.
// Inputs already use the pouch's vertical distance weighting. Edges also handle
// zero or parallel spans (missing left mount or inward axis).
inline float PouchPatchDistance(P3 p, P3 left, P3 back) {
    const auto dot = [](P3 a, P3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; };
    const auto sub = [](P3 a, P3 b) { return P3{a.x-b.x, a.y-b.y, a.z-b.z}; };
    const auto edge = [&](P3 origin, P3 span) {
        const auto d = sub(p, origin);
        const float length2 = dot(span, span);
        const float t = length2 > 1e-12f ? std::clamp(dot(d, span)/length2, 0.f, 1.f) : 0.f;
        return Distance(d, {span.x*t, span.y*t, span.z*t});
    };
    float best = std::min({edge({}, left), edge(back, left), edge({}, back), edge(left, back)});
    const float aa = dot(left,left), bb = dot(back,back), ab = dot(left,back);
    const float det = aa*bb-ab*ab;
    if (det > 1e-12f) {
        const float ap = dot(left,p), bp = dot(back,p);
        const float u = (ap*bb-bp*ab)/det, v = (bp*aa-ap*ab)/det;
        if (u >= 0 && u <= 1 && v >= 0 && v <= 1)
            best = std::min(best, Distance(p, {left.x*u+back.x*v, left.y*u+back.y*v,
                                               left.z*u+back.z*v}));
    }
    return best;
}

class Tracker {
public:
    void Reset() {
        *this = {};
    }
    Layout Follow(Layout layout, P3 hand, bool enabled, float reach, float scale) {
        if (!enabled || !layout.valid || !Finite(hand)) {
            Reset();
            return {};
        }
        scale = std::isfinite(scale) ? std::clamp(scale, .6f, 1.5f) : 1.f;
        reach = std::isfinite(reach) ? std::clamp(reach, .06f, .18f) : .12f;
        // Beyond `reach`: pouch +4 cm, left pocket +5.5 cm, chest +2.5 cm.
        // Relative to 60af5bfb: pouch +3 cm, pocket +2 cm; chest grows vertically below.
        const auto radius = [&](int i) {
            const int a = layout.assignment[i];
            const float slot_extra = i == 1 ? .04f : i == 2 ? .01f : 0.f;
            return reach * scale + (a == 1 ? .04f : a > 1 ? .015f + slot_extra : 0.f);
        };
        // The vials' pouch on the right hip reaches PouchLeftM further toward the left mount
        // (2026-10-15: the tester, "extend it 6cm towards the left", easier to grab): the distance to
        // that segment swept 3 cm inward along the rendered pouch's back axis.
        // Missing directions fall back to the remaining segment or the centre.
        const auto distance = [&](int i) {
            if (i == 2 && layout.assignment[i] > 1) {
                // Keep the shallow axis perpendicular to the chest through turns and lean.
                const auto c = layout.centre[i];
                const P3 delta{hand.x - c.x, hand.y - c.y, hand.z - c.z};
                const auto along = [&](P3 axis) {
                    return delta.x * axis.x + delta.y * axis.y + delta.z * axis.z;
                };
                const float r = radius(i);
                const float dx = along(layout.chest_axes[0]);
                const float dy = along(layout.chest_axes[1]) * r / (r + ChestVerticalExtraM);
                // Keep a usable positive depth at the smallest reach/body-scale settings.
                const float depth = std::max(r - ChestDepthReductionM, .02f);
                const float dz = along(layout.chest_axes[2]) * r / depth;
                return std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            if (i != 0 || layout.assignment[0] != 1) {
                return Distance(hand, layout.centre[i]);
            }
            const P3 c = layout.centre[0];
            const float lx = layout.centre[1].x - c.x, lz = layout.centre[1].z - c.z;
            const float len = std::hypot(lx, lz);
            P3 left{}, back{};
            if (len > .05f && std::isfinite(len)) {
                const float k = std::min(PouchLeftM * scale, len) / len;
                left = {lx*k, 0, lz*k};
            }
            const float back_len = Distance(layout.pouch_back, {});
            if (Finite(layout.pouch_back) && back_len > 1e-4f) {
                const float k = PouchBackM * scale / back_len;
                back = {layout.pouch_back.x*k, layout.pouch_back.y*k, layout.pouch_back.z*k};
            }
            const float r = radius(0), vertical = r / (r + PouchVerticalExtraM);
            back.y *= vertical;
            return PouchPatchDistance({hand.x-c.x, (hand.y-c.y)*vertical, hand.z-c.z}, left, back);
        };
        int next = -1;
        if (hover_ >= 0 && layout.assignment[hover_] > 0 &&
            distance(hover_) <= radius(hover_) + .03f * scale)
            next = hover_;
        if (next < 0) {
            float best = std::max({radius(0), radius(1), radius(2)});
            for (int i = 0; i < 3; ++i) {
                const float d = distance(i);
                if (layout.assignment[i] > 0 && d < radius(i) && d < best) {
                    next = i;
                    best = d;
                }
            }
        }
        layout.entered = next >= 0 && next != hover_;
        layout.hover = hover_ = next;
        return layout;
    }

private:
    static bool Finite(P3 p) {
        return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
    }
    int hover_{-1};
};
} // namespace VR::Holsters
