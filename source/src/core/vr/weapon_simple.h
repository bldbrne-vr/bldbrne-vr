// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "core/vr/weapon_fit.h"
#include <string_view>

// Small collision proxies: preserve the large pieces, not every tooth or
// corner. The detailed fitter remains available for comparison; gameplay uses
// three capsules normally; selected broad heads and curved blades use at most four.
namespace VR::WeaponFit {
inline Params SimpleParams() {
    Params p;
    p.max_caps = 3;
    p.extend_grip = true;
    p.r = 0.03f;
    p.poke_ok = 0.04f;
    p.cover_ok = 0.75f;
    p.air_max = 0.10f; // permit a short bridge over a bent handle, not a
                       // blade-sized shortcut
    p.air_weight = 0.3f;
    p.fat_cap = 0.035f;
    return p;
}

// Principal centre line of a structural piece, in the already-posed grip frame.
inline bool PieceAxis(std::span<const Vec3> points, Vec3 &a, Vec3 &b) {
    if (points.size() < 3)
        return false;
    Vec3 center{};
    for (const Vec3 v : points) {
        if (!Body::IsFinite(v))
            return false;
        center = center + v;
    }
    center = center * (1.0f / static_cast<float>(points.size()));
    std::array<Vec3, 3> columns{};
    for (const Vec3 p : points) {
        const Vec3 d = p - center;
        columns[0] = columns[0] + d * d.x;
        columns[1] = columns[1] + d * d.y;
        columns[2] = columns[2] + d * d.z;
    }
    Vec3 axis = columns[0];
    for (const Vec3 c : columns)
        if (Body::LengthSq(c) > Body::LengthSq(axis))
            axis = c;
    if (Body::LengthSq(axis) < 1e-14f)
        return false;
    axis = Body::Normalize(axis);
    for (int i = 0; i < 24; ++i)
        axis =
            Body::Normalize(columns[0] * axis.x + columns[1] * axis.y + columns[2] * axis.z, axis);
    float lo = 1e30f, hi = -1e30f;
    for (const Vec3 p : points) {
        const float t = Body::Dot(p - center, axis);
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
    a = center + axis * lo;
    b = center + axis * hi;
    return hi - lo > 0.03f;
}

// Evaluate the same support directions used by the detailed fitter. Coverage is
// area-weighted over triangle centroids; air is measured against its surface
// grid. These are diagnostics.
inline void MeasureSimple(Fit &fit, std::span<const Vec3> verts,
                          std::span<const std::uint32_t> tris) {
    const Params p = SimpleParams();
    fit.poke = fit.fat = fit.air = fit.coverage = fit.score = 0;
    for (const Vec3 n : Detail::Directions()) {
        float mesh = -1e30f, shape = p.hand_r;
        for (const Vec3 v : verts)
            mesh = std::max(mesh, Body::Dot(v, n));
        for (int k = 0; k < fit.count; ++k) {
            const Capsule &c = fit.caps[k];
            shape = std::max(shape, std::max(Body::Dot(c.a, n), Body::Dot(c.b, n)) + c.r);
        }
        fit.poke = std::max(fit.poke, mesh - shape);
        fit.fat = std::max(fit.fat, shape - mesh);
    }
    float total = 0, covered = 0;
    for (std::size_t i = 0; i + 2 < tris.size(); i += 3) {
        if (tris[i] >= verts.size() || tris[i + 1] >= verts.size() || tris[i + 2] >= verts.size())
            continue;
        const Vec3 a = verts[tris[i]], b = verts[tris[i + 1]], c = verts[tris[i + 2]];
        const float area = Body::Length(Body::Cross(b - a, c - a));
        const Vec3 v = (a + b + c) * (1.0f / 3.0f);
        float distance = Body::Length(v) - p.hand_r;
        for (int k = 0; k < fit.count; ++k) {
            const Capsule &cap = fit.caps[k];
            const Vec3 ab = cap.b - cap.a;
            const float l2 = Body::LengthSq(ab);
            distance =
                std::min(distance, Detail::SegDist(v, cap.a, ab, l2 > 0 ? 1 / l2 : 0) - cap.r);
        }
        total += area;
        if (distance <= p.slack)
            covered += area;
    }
    fit.coverage = total > 0 ? covered / total : 0;
    Detail::NearGrid grid;
    grid.Build(verts, p.air_m);
    for (int k = 0; k < fit.count; ++k)
        fit.air += grid.AirLength(fit.caps[k].a, fit.caps[k].b);
}

// Nudge generic blade/head proxies toward the part of the surface they under-cover.
// Symmetric edges cancel, so a straight double-edged sword stays centred. The grip stays
// anchored, and every child start follows its original attachment on the adjusted parent.
inline Fit BiasSimpleEdges(Fit fit, std::span<const Vec3> verts,
                           std::span<const std::uint32_t> tris) {
    if (fit.count == 0)
        return fit;
    std::array<Vec3, MaxCapsules> pull{};
    std::array<float, MaxCapsules> magnitude{};
    for (std::size_t i = 0; i + 2 < tris.size(); i += 3) {
        if (tris[i] >= verts.size() || tris[i + 1] >= verts.size() || tris[i + 2] >= verts.size())
            continue;
        const Vec3 a = verts[tris[i]], b = verts[tris[i + 1]], c = verts[tris[i + 2]];
        const Vec3 p = (a + b + c) * (1.0f / 3.0f);
        const float area = Body::Length(Body::Cross(b - a, c - a));
        float distance = Body::Length(p) - SimpleParams().hand_r;
        int nearest = -1;
        for (int k = 0; k < fit.count; ++k) {
            const Capsule &cap = fit.caps[k];
            const Vec3 ab = cap.b - cap.a;
            const float l2 = Body::LengthSq(ab);
            const float d = Detail::SegDist(p, cap.a, ab, l2 > 1e-12f ? 1 / l2 : 0) - cap.r;
            if (d < distance) {
                distance = d;
                nearest = k;
            }
        }
        if (nearest < 0 || distance <= 0.005f || !(area > 0))
            continue;
        const Capsule &cap = fit.caps[nearest];
        const Vec3 side = Body::Perp(p - cap.a, Body::Normalize(cap.b - cap.a));
        const float len = Body::Length(side);
        if (len <= cap.r + 0.005f)
            continue;
        const float weight = area * (len - cap.r);
        pull[nearest] = pull[nearest] + side * (weight / len);
        magnitude[nearest] += weight;
    }
    const auto measured = [&](Fit f) {
        f.poke = f.fat = f.coverage = f.air = f.score = 0;
        MeasureSimple(f, verts, tris);
        return f;
    };
    const Fit before = measured(fit);
    for (const float amount : {0.025f, 0.0125f}) {
        Fit moved = fit;
        for (int k = 0; k < fit.count; ++k) {
            const Capsule &old = fit.caps[k];
            Capsule &cap = moved.caps[k];
            if (k > 0 && Body::LengthSq(old.a) > 1e-10f) {
                int parent = -1;
                float at = 0, best = 1e-8f;
                for (int j = 0; j < k; ++j) {
                    const Vec3 ab = fit.caps[j].b - fit.caps[j].a;
                    const float l2 = Body::LengthSq(ab);
                    const float u =
                        l2 > 1e-12f
                            ? std::clamp(Body::Dot(old.a - fit.caps[j].a, ab) / l2, 0.0f, 1.0f)
                            : 0;
                    const float d =
                        Body::LengthSq(old.a - Body::Lerp(fit.caps[j].a, fit.caps[j].b, u));
                    if (d < best) {
                        best = d;
                        parent = j;
                        at = u;
                    }
                }
                if (parent < 0)
                    return fit; // never detach a capsule from its proven start
                cap.a = Body::Lerp(moved.caps[parent].a, moved.caps[parent].b, at);
            }
            const float force = Body::Length(pull[k]);
            if (magnitude[k] > 1e-8f && force > 0.35f * magnitude[k])
                cap.b = old.b + pull[k] * (amount / force);
        }
        moved = measured(moved);
        // Do not trade away a whole corner or introduce a new bridge through empty space.
        if (moved.poke <= before.poke + 0.005f && moved.fat <= before.fat + 0.01f &&
            moved.air <= before.air + 0.025f && moved.coverage >= before.coverage - 0.02f)
            return moved;
    }
    return fit;
}

// Run one simple capsule along the Beast Cutter's broad, toothed edge. Its
// shoulder is the widest point beside the upper shaft; the start stays connected
// to the already-covered lower corner. All measurements use the posed mesh.
inline Fit AddCutterEdge(Fit fit, std::span<const Vec3> verts,
                         std::span<const std::uint32_t> tris) {
    if (fit.count != 3)
        return fit;
    const Vec3 axis = fit.caps[0].b;
    const float l2 = Body::LengthSq(axis);
    if (l2 < 1e-8f)
        return fit;
    Vec3 shoulder{};
    float width = 0;
    for (const Vec3 v : verts) {
        const float u = Body::Dot(v, axis) / l2;
        const float side = Body::LengthSq(v - axis * u);
        if (u >= 0.15f && u <= 0.50f && side > width) {
            width = side;
            shoulder = v;
        }
    }
    if (width < 4 * fit.caps[0].r * fit.caps[0].r)
        return fit;
    const Vec3 along = Body::Normalize(axis);
    const Vec3 outward = Body::Normalize(Body::Perp(shoulder, along));
    int corner = -1;
    float side = 0;
    for (int i = 0; i < fit.count; ++i) {
        const Vec3 tip = fit.caps[i].b;
        const float reach = Body::Dot(Body::Perp(tip, along), outward);
        if (Body::Dot(tip, axis) / l2 > 0.65f && reach > side) {
            side = reach;
            corner = i;
        }
    }
    if (corner < 0)
        return fit;
    const Vec3 start = fit.caps[corner].b;
    const Vec3 run = shoulder - start;
    if (Body::Length(run) < 0.45f * Body::Length(axis))
        return fit;
    const float radius = std::min(0.03f, fit.caps[corner].r);
    fit.caps[fit.count++] = {start, shoulder - Body::Normalize(run) * radius, radius, false};
    MeasureSimple(fit, verts, tris);
    return fit;
}

// Select the large blade/head in the posed mesh, without relying on world axes.
inline bool RefinedBladeBone(int model, std::string_view name) {
    switch (model) {
    case 1011:
        return name == "B_Blade";
    case 1020:
        return name == "B_Axe";
    case 1021:
        return name == "B_Sword";
    case 1031:
    case 1706:
        return name == "B_Head";
    case 1700:
        return name == "B_blade";
    case 1707:
        return name == "B_Blade01" || name == "B_Blade02";
    default:
        return false;
    }
}

inline float SimpleDistance(Vec3 v, const Fit &fit) {
    float distance = Body::Length(v) - SimpleParams().hand_r;
    for (int i = 0; i < fit.count; ++i) {
        const Capsule &c = fit.caps[i];
        const Vec3 ab = c.b - c.a;
        const float l2 = Body::LengthSq(ab);
        distance = std::min(distance, Detail::SegDist(v, c.a, ab, l2 > 0 ? 1 / l2 : 0) - c.r);
    }
    return distance;
}

// Spend at most one extra piece on the head's largest remaining gap. The earlier
// pieces stay fixed; every candidate starts on a capsule already swept.
inline Fit AddHeadCoverage(Fit fit, std::span<const Vec3> head, std::span<const Vec3> verts,
                           std::span<const std::uint32_t> tris, float radius) {
    if (fit.count != 3 || head.empty())
        return fit;
    Vec3 target{};
    float gap = 0.04f;
    for (const Vec3 v : head) {
        const float d = SimpleDistance(v, fit);
        if (d > gap) {
            gap = d;
            target = v;
        }
    }
    if (gap <= 0.04f)
        return fit;
    // Inset a corner in all directions, not just along the capsule. Otherwise a
    // thick piece on a hammer's top edge would stop above the visible head.
    Vec3 center{};
    for (const Vec3 v : head)
        center = center + v;
    center = center * (1.0f / static_cast<float>(head.size()));
    target = target + Body::Normalize(center - target) * (2.0f * radius);
    Detail::NearGrid grid;
    grid.Build(verts, SimpleParams().air_m);
    Detail::Solid solid;
    Detail::Solid::Bits nearby;
    if (radius > 0.04f) {
        solid.Build(verts, tris, SimpleParams().air_m + 0.02f);
        nearby = solid.Grow(SimpleParams().air_m);
    }
    Fit best = fit;
    float bestScore = 0;
    for (const Vec3 v : head)
        bestScore += std::max(0.0f, SimpleDistance(v, fit));
    bestScore = bestScore / static_cast<float>(head.size()) - 0.002f;
    for (int i = 0; i < fit.count; ++i) {
        const Capsule &parent = fit.caps[i];
        const Vec3 ab = parent.b - parent.a;
        const float l2 = Body::LengthSq(ab);
        const float u = l2 > 0 ? std::clamp(Body::Dot(target - parent.a, ab) / l2, 0.0f, 1.0f) : 0;
        for (const Vec3 start : {parent.a, parent.b, Body::Lerp(parent.a, parent.b, u)}) {
            const Vec3 run = target - start;
            if (Body::Length(run) < radius + 0.03f)
                continue;
            const Vec3 end = target;
            const float air = radius > 0.04f ? solid.AirLength(nearby, start, end) : grid.AirLength(start, end);
            if (air > 0.15f)
                continue;
            Fit candidate = fit;
            candidate.caps[candidate.count++] = {start, end, radius, radius > parent.r};
            float score = 0;
            for (const Vec3 v : head)
                score += std::max(0.0f, SimpleDistance(v, candidate));
            score = score / static_cast<float>(head.size()) + 0.1f * air;
            if (score < bestScore) {
                bestScore = score;
                best = candidate;
            }
        }
    }
    MeasureSimple(best, verts, tris);
    return best;
}

inline Fit RefineBlade(Fit fit, std::span<const Vec3> blade, std::span<const Vec3> verts,
                       std::span<const std::uint32_t> tris, int model) {
    if (fit.count != 3 || blade.empty())
        return fit;
    Capsule &spine = fit.caps[2];
    const Vec3 axis = Body::Normalize(spine.b - spine.a);
    // Moving this joint is safe only when it is the end of the preceding connector.
    if ((model == 1011 || model == 1021) && Body::LengthSq(spine.a - fit.caps[1].b) > 1e-8f)
        return fit;
    if (model == 1011) {
        // Keep the Saw Spear at three pieces: extend both ends of its straight blade.
        float lo = 0, hi = Body::Length(spine.b - spine.a);
        for (const Vec3 v : blade) {
            const float at = Body::Dot(v - spine.a, axis);
            lo = std::min(lo, at + spine.r);
            hi = std::max(hi, at - spine.r);
        }
        const Vec3 old = spine.a;
        spine.b = old + axis * hi;
        spine.a = old + axis * lo;
        fit.caps[1].b = spine.a;
    } else {
        // Move the scythe's main blade out toward its cutting edge; its hinge follows.
        if (model == 1021) {
            const Vec3 shift =
                Body::Normalize(Body::Perp((spine.a + spine.b) * 0.5f, axis)) * 0.035f;
            spine.a = spine.a + shift;
            spine.b = spine.b + shift;
            fit.caps[1].b = spine.a;
        }
        Vec3 tip = spine.b;
        float reach = Body::Dot(tip, axis);
        for (const Vec3 v : blade)
            if (Body::Dot(v, axis) > reach) {
                reach = Body::Dot(v, axis);
                tip = v;
            }
        const Vec3 run = tip - spine.b;
        if (Body::Length(run) > 0.06f)
            fit.caps[fit.count++] = {spine.b, tip - Body::Normalize(run) * spine.r, spine.r, false};
    }
    MeasureSimple(fit, verts, tris);
    return fit;
}

// A folding blade: the handle to its hinge, the short hinge bridge, then one
// blade spine. Bone membership is supplied by the model reader; all points are
// posed before fitting so folded/extended forms use the same construction
// without fixed game-space coordinates.
inline Fit FitSimpleCapsules(std::span<const Vec3> verts, std::span<const std::uint32_t> tris,
                             std::span<const Vec3> handle = {}, std::span<const Vec3> blade = {},
                             int model = -1) {
    switch (model) {
    case 1011:
    case 1021:
    case 1700: {
        Fit fit = BiasSimpleEdges(FitCapsules(verts, tris, SimpleParams()), verts, tris);
        return RefineBlade(fit, blade, verts, tris, model);
    }
    case 1020:
    case 1031:
    case 1707: {
        Fit fit = BiasSimpleEdges(FitCapsules(verts, tris, SimpleParams()), verts, tris);
        return AddHeadCoverage(fit, blade, verts, tris, model == 1031 ? 0.09f : 0.03f);
    }
    case 1706: {
        Params p = SimpleParams();
        p.max_caps = 4;
        p.r = 0.04f;
        p.poke_ok = 0.02f;
        p.fat_r = {0.08f, 0.12f, 0.18f};
        return BiasSimpleEdges(FitCapsules(verts, tris, p), verts, tris);
    }
    case 1705: {
        Params p = SimpleParams();
        p.max_caps = 4;
        p.r = 0.04f;
        p.poke_ok = 0.02f;
        p.cover_ok = 0.85f;
        return BiasSimpleEdges(FitCapsules(verts, tris, p), verts, tris);
    }
    }
    Vec3 ha{}, hb{}, ba{}, bb{};
    if (!PieceAxis(handle, ha, hb) || !PieceAxis(blade, ba, bb)) {
        Fit fit = BiasSimpleEdges(FitCapsules(verts, tris, SimpleParams()), verts, tris);
        if (model == 1702)
            fit = AddCutterEdge(fit, verts, tris);
        return fit;
    }
    const auto distance = [](Vec3 p, Vec3 a, Vec3 b) {
        return std::min(Body::LengthSq(p - a), Body::LengthSq(p - b));
    };
    if (distance(ha, ba, bb) < distance(hb, ba, bb))
        std::swap(ha, hb);
    const Vec3 handleAxis = Body::Normalize(hb - ha);
    ha = ha + handleAxis * 0.03f;
    hb = hb - handleAxis * 0.03f;
    const Vec3 hinge = hb;
    if (Body::LengthSq(bb - hinge) < Body::LengthSq(ba - hinge))
        std::swap(ba, bb);
    constexpr float r = 0.05f;
    // Pull the blade's extreme ends in by its radius; the rounded ends reach its
    // tips.
    const Vec3 axis = Body::Normalize(bb - ba);
    if (Body::Length(bb - ba) > 2 * r + 0.03f) {
        ba = ba + axis * r;
        bb = bb - axis * r;
    }
    // Bias the blade spine gently toward the cutting edge (away from the handle).
    // Keep the connector attached to that shifted spine.
    const Vec3 outward = Body::Perp((ba + bb - ha - hb) * 0.5f, axis);
    if (Body::LengthSq(outward) > 1e-6f) {
        const Vec3 shift = Body::Normalize(outward) * 0.025f;
        ba = ba + shift;
        bb = bb + shift;
    }
    Fit out{};
    out.caps[out.count++] = {ha, hinge, 0.03f, false};
    if (Body::Length(ba - hinge) > 0.03f)
        out.caps[out.count++] = {hinge, ba, 0.03f, false};
    else
        ba = hinge;
    out.caps[out.count++] = {ba, bb, r, true};
    MeasureSimple(out, verts, tris);
    return out;
}
} // namespace VR::WeaponFit
