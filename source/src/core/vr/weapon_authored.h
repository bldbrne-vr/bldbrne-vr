// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "core/vr/weapon_fit.h"
#include "core/vr/weapon_mesh.h"
#include <string_view>

namespace VR::WeaponAuthored {
using Body::Vec3;
struct Endpoint {
    std::string_view bone;
    Vec3 local;
};
struct Capsule {
    Endpoint a, b;
    float r;
};
struct Shape {
    int model;
    bool form_b;
    int part;
    std::span<const Capsule> caps;
    bool left{false}; // paired blade carried by the left hand
};

// No geometry edits: choose the capsule nearest the grip as the primary line and
// prove all remaining starts independently. User drawing order is not sweep order.
inline void PrepareSweeps(WeaponFit::Fit &fit) {
    if (fit.count < 1)
        return;
    int root = 0;
    float best = 1e30f;
    for (int i = 0; i < fit.count; ++i) {
        const auto &c = fit.caps[i];
        const Vec3 ab = c.b - c.a;
        const float l2 = Body::LengthSq(ab);
        const float d = WeaponFit::Detail::SegDist({}, c.a, ab, l2 > 1e-12f ? 1 / l2 : 0);
        if (d < best) {
            best = d;
            root = i;
        }
    }
    std::swap(fit.caps[0], fit.caps[root]);
    fit.caps[0].carried = false;
    fit.caps[0].detached = false;
    for (int i = 1; i < fit.count; ++i) {
        const auto gap = [&](Vec3 p, float radius) {
            float distance = 1e30f;
            for (int j = 0; j < i; ++j) {
                const auto &c = fit.caps[j];
                const Vec3 ab = c.b - c.a;
                const float l2 = Body::LengthSq(ab);
                distance = std::min(
                    distance, WeaponFit::Detail::SegDist(p, c.a, ab, l2 > 1e-12f ? 1 / l2 : 0) +
                                  radius - c.r);
            }
            return distance;
        };
        int next = i;
        bool reverse = false;
        float nearest = 1e30f;
        for (int j = i; j < fit.count; ++j) {
            const auto &c = fit.caps[j];
            for (bool end : {false, true}) {
                const float d = gap(end ? c.b : c.a, c.r);
                if (d < nearest) {
                    nearest = d;
                    next = j;
                    reverse = end;
                }
            }
        }
        std::swap(fit.caps[i], fit.caps[next]);
        auto &c = fit.caps[i];
        if (reverse)
            std::swap(c.a, c.b);
        c.carried = gap(c.a, c.r) > 1e-5f;
        c.detached = gap(c.a, std::min(fit.caps[0].r, c.r)) > 1e-5f;
    }
}

// The caller has already checked the live pose against its markers. Resolve by
// name because normal and transformed FLVERs can order their bones differently.
// Missing bones fail explicitly; do not silently replace an authored form by a fit.
inline bool Apply(const Shape &shape, const WeaponMesh::Mesh &mesh, const WeaponMesh::Posed &posed,
                  WeaponFit::Fit &out) {
    if (shape.caps.empty() || shape.caps.size() > WeaponFit::MaxCapsules ||
        posed.bone_to_grip.size() != mesh.bone_names.size())
        return false;
    WeaponFit::Fit result{};
    const auto point = [&](const Endpoint &endpoint, Vec3 &p) {
        const auto it = std::find(mesh.bone_names.begin(), mesh.bone_names.end(), endpoint.bone);
        if (it == mesh.bone_names.end())
            return false;
        p = WeaponMesh::Apply(posed.bone_to_grip[it - mesh.bone_names.begin()], endpoint.local);
        return Body::IsFinite(p) && Body::LengthSq(p) <= 100.0f;
    };
    for (const auto &c : shape.caps) {
        auto &target = result.caps[result.count++];
        if (!std::isfinite(c.r) || c.r < .005f || c.r > .2f || !point(c.a, target.a) ||
            !point(c.b, target.b))
            return false;
        target.r = c.r;
    }
    PrepareSweeps(result);
    out = result;
    return true;
}
} // namespace VR::WeaponAuthored
