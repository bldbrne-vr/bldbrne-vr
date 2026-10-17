// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <span>
#include "video_core/renderer_vulkan/vr_overlay_meshes.h"

namespace Vulkan::VrOverlay {
// Held quick items the game has no model of, after their menu art. Metres, +Y up, -Z the
// front, the grip near the origin. One mesh per item: aux.x is the material, aux.y a baked
// cavity (0 open .. 1 deep) the shader darkens by.
namespace GoodsMat {
constexpr float Glass = 0, Blood = 1, Linen = 2, Velvet = 3, Cork = 4, Paper = 5, Iron = 6,
                Bronze = 7, Bone = 8, Leather = 9, Chalk = 10, Glow = 11, Tonic = 12,
                String = 13, Label = 14, Lace = 15,
                // The blade's rubs: their aux.y is how near the torn edge (1 at it), not a cavity.
                FirePaper = 16, BoltPaper = 17, Phantasm = 18;
}

inline void SetMaterial(MeshData& m, float material, float cavity = 0.0f) {
    for (auto& v : m.vertices) v.aux = {material, cavity};
}

// A cheap smooth noise for crumples and lumps (-1 .. 1).
inline float GoodsNoise(Vec3 p) {
    return (std::sin(p.x * 1.7f + std::sin(p.y * 2.3f)) * std::sin(p.y * 1.9f + p.z * 1.3f) +
            std::sin(p.z * 2.1f + std::sin(p.x * 1.1f) * 2.0f) * .6f) / 1.6f;
}

// A round tube along `path` (radius r0 -> r1), its ends closed by poles.
inline MeshData Sweep(std::span<const Vec3> path, float r0, float r1, u32 segments = 10) {
    std::vector<LoftRing> rings;
    Vec3 u{};
    for (size_t i = 0; i < path.size(); ++i) {
        const Vec3 t = Normalize(path[std::min(i + 1, path.size() - 1)] - path[i ? i - 1 : 0]);
        if (i == 0) u = Normalize(Cross(t, std::abs(t.y) < .9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}));
        u = Normalize(u - t * Dot(u, t)); // parallel transport: no twist
        const float r = r0 + (r1 - r0) * float(i) / float(path.size() - 1);
        rings.push_back({path[i], u, Cross(t, u), r, r});
    }
    return Loft(rings, segments, path.front(), path.back());
}

// Signed distance helpers for the organic items (skulls, clots, cords). < 0 inside.
inline float SdEllipsoid(Vec3 p, Vec3 c, Vec3 r) {
    const Vec3 q{(p.x - c.x) / r.x, (p.y - c.y) / r.y, (p.z - c.z) / r.z};
    const float k = Length(q);
    return (k - 1.0f) * std::min({r.x, r.y, r.z}); // a bound, exact on spheres
}
inline float SdCapsule(Vec3 p, Vec3 a, Vec3 b, float r) {
    const Vec3 pa = p - a, ba = b - a;
    const float h = std::clamp(Dot(pa, ba) / Dot(ba, ba), 0.0f, 1.0f);
    return Length(pa - ba * h) - r;
}
inline float SmoothUnion(float a, float b, float k) {
    const float h = std::clamp(.5f + .5f * (b - a) / k, 0.0f, 1.0f);
    return b + (a - b) * h - k * h * (1.0f - h);
}
inline float SmoothCut(float a, float b, float k) { return -SmoothUnion(-a, b, k); } // a minus b

// The surface sdf(p) = 0 of a shape star-shaped round `centre`: a sphere's vertices moved in
// along their rays onto it (so the count is the sphere's: low poly), normals from the gradient.
// aux = (material, occlusion): what the sdf has close over the normal darkens it, so sockets
// and folds come out dark without a texture. Built once per mesh: a cold path.
template <class Sdf>
MeshData SdfWrap(const Sdf& sdf, float material, Vec3 centre, float reach, u32 segments, u32 rings) {
    MeshData mesh = Sphere(1.0f, segments, rings);
    const float e = reach * .002f;
    const auto normal = [&](Vec3 p) {
        return Normalize({sdf(p + Vec3{e, 0, 0}) - sdf(p - Vec3{e, 0, 0}),
                          sdf(p + Vec3{0, e, 0}) - sdf(p - Vec3{0, e, 0}),
                          sdf(p + Vec3{0, 0, e}) - sdf(p - Vec3{0, 0, e})});
    };
    const auto project = [&](Vec3 p) {
        for (int i = 0; i < 4; ++i) p = p - normal(p) * sdf(p);
        return p;
    };
    for (auto& v : mesh.vertices) {
        // In from the reach to the first hit (the outermost surface), then bisect it.
        const Vec3 dir = Normalize(v.position);
        float out = reach, in = reach;
        while (in > 0.0f && sdf(centre + dir * in) >= 0.0f) out = in, in -= reach * .01f;
        in = std::max(in, 0.0f);
        for (int i = 0; i < 16; ++i) {
            const float t = .5f * (in + out);
            (sdf(centre + dir * t) < 0.0f ? in : out) = t;
        }
        v.position = centre + dir * in;
    }
    // Relax onto the surface: the rays leave steps round sockets and spread thin on walls.
    // The poles' copies share a position, so neighbours are keyed by position.
    std::vector<Vec3> sum(mesh.vertices.size());
    std::vector<float> count(mesh.vertices.size());
    for (int pass = 0; pass < 6; ++pass) {
        std::fill(sum.begin(), sum.end(), Vec3{});
        std::fill(count.begin(), count.end(), 0.0f);
        for (size_t i = 0; i < mesh.indices.size(); i += 3) {
            for (int k = 0; k < 3; ++k) {
                const u32 a = mesh.indices[i + k], b = mesh.indices[i + (k + 1) % 3];
                sum[a] = sum[a] + mesh.vertices[b].position; count[a] += 1.0f;
                sum[b] = sum[b] + mesh.vertices[a].position; count[b] += 1.0f;
            }
        }
        for (size_t i = 0; i < sum.size(); ++i) {
            if (count[i] > 0.0f) {
                const Vec3 p = mesh.vertices[i].position;
                mesh.vertices[i].position = project(p + (sum[i] * (1.0f / count[i]) - p) * .5f);
            }
        }
    }
    for (auto& v : mesh.vertices) {
        const Vec3 p = v.position, n = normal(p);
        float occlusion = 0.0f;
        for (float k : {.03f, .06f, .12f}) occlusion += std::max(k * reach - sdf(p + n * (k * reach)), 0.0f) / (k * reach);
        v = {p, n, {material, std::min(occlusion * .4f, 1.0f)}};
    }
    return mesh;
}

// Blood of Arianna / Adella / Adeline: our Blood Vial (vr_syringe_mesh.h) made a sealed
// tube. Its barrel, collars and tie rods are kept (same chamber: Shade::Syringe draws the
// blood, uv = {.0352, .1408, .0146, fill}); the needle's nozzle becomes a rounded foot and the
// plunger a lipped neck, sealed by BloodTubeClothMesh or, with `ring`, a steel pull stopper.
inline MeshData BloodTubeMesh(bool ring, u32 segments = 48) {
    const std::array<ProfilePoint, 25> p{{{0, .0120f},
                                          {.0026f, .0124f},
                                          {.0032f, .0150f, true},
                                          {.0070f, .0168f, true},
                                          {.0118f, .0192f},
                                          {.0146f, .0232f},
                                          {.0155f, .0270f, true},
                                          {.0170f, .0290f},
                                          {.0170f, .0310f, true},
                                          {.0159f, .0320f, true},
                                          {.0159f, .0352f, true},
                                          {.0155f, .0362f},
                                          {.0155f, .1398f},
                                          {.0159f, .1408f, true},
                                          {.0170f, .1420f},
                                          {.0170f, .1450f, true},
                                          {.0155f, .1460f, true},
                                          {.0155f, .1480f, true},
                                          {.0170f, .1490f, true},
                                          {.0170f, .1520f},
                                          {.0150f, .1540f, true},
                                          {.0138f, .1540f, true},
                                          {.0138f, .1590f, true},
                                          {.0152f, .1605f},
                                          {0, .1625f}}};
    MeshData mesh = Lathe(p, segments);
    const std::array<ProfilePoint, 6> rod{{{0, .032f},
                                           {.00085f, .032f, true},
                                           {.00115f, .034f},
                                           {.00115f, .143f},
                                           {.00085f, .146f, true},
                                           {0, .146f}}};
    for (float side : {-1.0f, 1.0f}) {
        auto rail = Lathe(rod, 12);
        TransformMesh(rail, Mat4::Translation({side * .0164f, 0, 0}));
        AppendMesh(mesh, rail);
    }
    if (ring) {
        // A domed steel stopper on the neck, a stem, and a ring to pull it by.
        const std::array<ProfilePoint, 7> plug{{{0, .1590f}, {.0148f, .1590f},
            {.0160f, .1600f, true}, {.0160f, .1640f, true}, {.0110f, .1680f}, {.0030f, .1700f},
            {0, .1702f}}};
        AppendMesh(mesh, Lathe(plug, 24));
        AppendMesh(mesh, Lathe(std::array<ProfilePoint, 4>{{{0, .169f}, {.0018f, .169f, true},
            {.0018f, .179f, true}, {0, .179f}}}, 10));
        std::vector<ProfilePoint> wire;
        for (int i = 0; i < 8; ++i) {
            const float a = i * Pi / 4.0f;
            wire.push_back({.0105f + .0016f * std::cos(a), .0016f * std::sin(a)});
        }
        auto loop = Lathe(wire, 32, true);
        TransformMesh(loop, Mat4::Translation({0, .1905f, 0}) * Mat4::RotationX(Pi * .5f));
        AppendMesh(mesh, loop);
    }
    return mesh;
}

// The cloth tied over BloodTubeMesh's neck: a dome over the mouth, gathered at the tie, then
// falling in folds to a ragged hem, with a cord and its two loose ends. Velvet (Arianna's,
// a lace hem) or linen (Adella's). Two-sided; aux.y is the folds' shadow.
inline MeshData BloodTubeClothMesh(bool velvet) {
    constexpr float R = .0160f, Top = .1625f, Tie = .1555f, Drop = .036f;
    const auto drape = [&](MeshData m, float material) {
        for (auto& v : m.vertices) {
            const float a = std::atan2(v.position.x, v.position.z);
            const float fall = std::clamp((Tie - v.position.y) / Drop, 0.0f, 1.0f);
            const float fold = std::sin(a * 7.0f + 1.3f) * .55f + std::sin(a * 13.0f) * .22f;
            const float k = 1.0f + fold * .16f * fall;
            v.position.x *= k;
            v.position.z *= k;
            v.position.y += (std::sin(a * 3.0f + .4f) * .005f + fold * .002f) * fall * fall;
            v.aux = {material, (1.0f - fold) * .4f * fall};
        }
        return m;
    };
    const std::array<ProfilePoint, 8> cloth{{{0, Top + .0070f}, {.0080f, Top + .0055f},
        {R + .0015f, Top + .0008f}, {R + .0020f, Top - .004f}, {R + .0006f, Tie},
        {R + .0035f, Tie - .010f}, {R + .0070f, Tie - .024f}, {R + .0095f, Tie - Drop}}};
    MeshData mesh = drape(Lathe(cloth, 56), velvet ? GoodsMat::Velvet : GoodsMat::Linen);
    if (velvet) { // Arianna's: a band of pale lace along the hem
        const std::array<ProfilePoint, 2> hem{{{R + .0095f, Tie - Drop}, {R + .0105f, Tie - Drop - .006f}}};
        AppendMesh(mesh, drape(Lathe(hem, 56), GoodsMat::Lace));
    }
    std::vector<ProfilePoint> cord;
    for (int i = 0; i < 6; ++i) {
        const float a = i * Pi / 3.0f;
        cord.push_back({R + .0016f + .0010f * std::cos(a), Tie + .0010f * std::sin(a)});
    }
    auto tie = Lathe(cord, 28, true);
    SetMaterial(tie, GoodsMat::String);
    AppendMesh(mesh, tie);
    for (float side : {-1.0f, 1.0f}) {
        const std::array<Vec3, 4> end{{{side * .003f, Tie, -R - .0020f},
            {side * .006f, Tie - .008f, -R - .0045f}, {side * .007f, Tie - .017f, -R - .0060f},
            {side * .0065f, Tie - .025f, -R - .0065f}}};
        auto e = Sweep(end, .0009f, .0007f, 6);
        SetMaterial(e, GoodsMat::String);
        AppendMesh(mesh, e);
    }
    mesh.closed = false;
    return mesh;
}

// Iosefka's Blood Vial: an open apothecary bottle of pale gold blood to drink from, 13 cm. A
// thick glass shell (out, over the rolled lip, back down the inside); the liquid is the shader's,
// like the Blood Vial's: a sloshing column in the body (y -0.056 .. 0.016, radius 0.0222; the
// held item's GoodsLook passes it).
inline MeshData IosefkaBottleMesh() {
    const std::array<ProfilePoint, 20> glass{{{0, -.062f}, {.020f, -.062f},
        {.0235f, -.059f}, {.0245f, -.054f}, {.0245f, .018f}, {.0225f, .029f}, {.016f, .038f},
        {.0105f, .044f}, {.0095f, .050f}, {.0095f, .060f}, {.0114f, .0625f},
        {.0112f, .0660f, true}, {.0080f, .0668f, true}, {.0068f, .0640f}, {.0068f, .0500f},
        {.0080f, .0440f}, {.0140f, .0370f}, {.0222f, .0180f}, {.0222f, -.0540f},
        {0, -.0580f}}};
    MeshData mesh = Lathe(glass, 32);
    for (auto& v : mesh.vertices) v.aux = {2.0f, 0.0f}; // Shade::Syringe: a potion, all glass
    return mesh;
}

// Its paper label, wrapped part way round the front, the corners lifting (Shade::Goods; aux.y
// is the across-label coordinate, not a cavity).
inline MeshData IosefkaLabelMesh() {
    MeshData mesh{};
    constexpr u32 Nu = 16, Nv = 6;
    for (u32 j = 0; j <= Nv; ++j) {
        for (u32 i = 0; i <= Nu; ++i) {
            const float s = float(i) / Nu * 2.0f - 1.0f, t = float(j) / Nv;
            const float a = Pi + s * 1.25f;
            const float lift = .0012f * std::pow(std::abs(s), 6.0f) * (t > .5f ? t : 1.0f - t);
            const float rr = .0249f + lift;
            mesh.vertices.push_back({{rr * std::sin(a), -.040f + .048f * t, rr * std::cos(a)},
                                     {std::sin(a), 0, std::cos(a)}, {GoodsMat::Label, s * .5f + .5f}});
        }
    }
    for (u32 j = 0; j < Nv; ++j) {
        for (u32 i = 0; i < Nu; ++i) {
            const u32 a = j * (Nu + 1) + i, b = a + Nu + 1;
            mesh.indices.insert(mesh.indices.end(), {a, a + 1, b + 1, a, b + 1, b});
        }
    }
    mesh.closed = false;
    return mesh;
}

// Small Resonant Bell: a small bronze hand bell under a scrolled iron hanger, 11 cm.
inline MeshData ResonantBellMesh() {
    // Inside apex -> down the inner wall -> the lip -> up the outside -> the crown.
    const std::array<ProfilePoint, 11> bell{{{0, .036f}, {.009f, .034f}, {.0135f, .022f},
        {.0185f, .004f}, {.0205f, 0, true}, {.0232f, -.001f, true}, {.0225f, .006f},
        {.0165f, .020f}, {.0130f, .034f}, {.0085f, .045f}, {0, .047f}}};
    MeshData mesh = Lathe(bell, 32);
    SetMaterial(mesh, GoodsMat::Bronze); // its hollow: BakeOcclusion
    auto clapper = Sphere(.0042f, 12, 6);
    TransformMesh(clapper, Mat4::Translation({0, .006f, 0}));
    SetMaterial(clapper, GoodsMat::Bronze, .6f);
    AppendMesh(mesh, clapper);
    // The hanger: up from the crown, over in a hook, and into a small curl.
    std::vector<Vec3> path;
    for (int i = 0; i <= 40; ++i) {
        const float t = i / 40.0f;
        if (t < .25f) {
            path.push_back({0, .045f + t / .25f * .020f, 0});
        } else {
            const float s = (t - .25f) / .75f;
            const float a = s * 1.85f * Pi;          // the hook turns over...
            const float rad = .016f * (1.0f - .62f * s); // ...and winds into the curl
            path.push_back({.016f - rad * std::cos(a), .065f + rad * std::sin(a), 0});
        }
    }
    auto hanger = Sweep(path, .0028f, .0016f, 8);
    SetMaterial(hanger, GoodsMat::Iron);
    AppendMesh(mesh, hanger);
    std::vector<ProfilePoint> collar;
    for (int i = 0; i < 6; ++i) {
        const float a = i * Pi / 3.0f;
        collar.push_back({.0036f + .0015f * std::cos(a), .049f + .0015f * std::sin(a)});
    }
    auto c = Lathe(collar, 14, true);
    SetMaterial(c, GoodsMat::Iron);
    AppendMesh(mesh, c);
    return mesh;
}

// Madman's Knowledge / Great One's Wisdom: an aged human skull without its jaw, as in the
// menu art; life size (19 cm), the face to -Z, the cranium's centre at the origin.
inline MeshData SkullMesh() {
    const auto sdf = [](Vec3 p) {
        const Vec3 m{std::abs(p.x), p.y, p.z}; // symmetric
        float d = SdEllipsoid(p, {0, .020f, .014f}, {.068f, .066f, .090f});             // cranium
        d = SmoothUnion(d, SdEllipsoid(p, {0, -.006f, -.060f}, {.060f, .038f, .036f}), .016f); // brow
        d = SmoothUnion(d, SdEllipsoid(p, {0, -.054f, -.066f}, {.034f, .036f, .030f}), .016f); // maxilla
        d = SmoothUnion(d, SdEllipsoid(m, {.044f, -.036f, -.062f}, {.018f, .015f, .020f}), .010f); // cheek
        d = SmoothUnion(d, SdCapsule(m, {.050f, -.036f, -.052f}, {.064f, -.032f, -.004f}, .0065f), .008f);
        d = SmoothCut(d, SdEllipsoid(m, {.073f, -.022f, -.030f}, {.012f, .030f, .030f}), .010f); // temple
        // The base behind the face is flat; the face itself reaches lower.
        d = SmoothCut(d, (p.y + .052f) + std::max(-(p.z + .030f), 0.0f) * 3.0f, .008f);
        d = std::max(d, -(p.y + .088f));
        // Orbits, the nasal opening, and the hollow under the cheekbones.
        d = SmoothCut(d, SdEllipsoid(m, {.025f, -.014f, -.088f}, {.0205f, .0185f, .034f}), .004f);
        d = SmoothCut(d, SdEllipsoid(p, {0, -.048f, -.094f}, {.0100f, .0160f, .020f}), .003f);
        d = SmoothCut(d, SdEllipsoid(m, {.040f, -.068f, -.060f}, {.012f, .012f, .018f}), .008f);
        // Weathering: shallow pits and swellings.
        return d + .0005f * GoodsNoise(p * 90.0f);
    };
    MeshData mesh = SdfWrap(sdf, GoodsMat::Bone, {0, -.012f, -.006f}, .14f, 88, 56);
    // Upper teeth along the front of the maxilla.
    for (int i = 0; i < 10; ++i) {
        const float a = (i - 4.5f) * .19f;
        auto tooth = RoundedBox({.0034f, .0062f, .0032f}, .0016f, 1);
        TransformMesh(tooth, Mat4::Translation({.026f * std::sin(a), -.0880f + .0012f * std::abs(i - 4.5f) / 4.5f,
                                                -.066f - .024f * std::cos(a)}) * Mat4::RotationY(a));
        SetMaterial(tooth, GoodsMat::Bone, .2f);
        AppendMesh(mesh, tooth);
    }
    mesh.closed = false;
    return mesh;
}

// Antidote: a square of waxed paper, cupped, with white pills on it. 8 cm.
inline MeshData AntidoteMesh() {
    constexpr u32 N = 14;
    MeshData mesh{};
    const auto height = [](float x, float z) {
        const float r2 = (x * x + z * z) / (.04f * .04f);
        return .006f * r2 + .0012f * GoodsNoise({x * 260.0f, 0, z * 260.0f}) +
               .002f * std::max(std::abs(x), std::abs(z)) / .04f * GoodsNoise({x * 90.0f, 3, z * 90.0f});
    };
    for (u32 j = 0; j <= N; ++j) {
        for (u32 i = 0; i <= N; ++i) {
            const float x = (float(i) / N - .5f) * .08f, z = (float(j) / N - .5f) * .08f;
            const float e = .0004f;
            const Vec3 n = Normalize({height(x - e, z) - height(x + e, z), 2 * e,
                                      height(x, z - e) - height(x, z + e)});
            mesh.vertices.push_back({{x, height(x, z), z}, n, {GoodsMat::Paper, 0}});
        }
    }
    for (u32 j = 0; j < N; ++j) {
        for (u32 i = 0; i < N; ++i) {
            const u32 a = j * (N + 1) + i, b = a + N + 1;
            mesh.indices.insert(mesh.indices.end(), {a, b, b + 1, a, b + 1, a + 1});
        }
    }
    constexpr std::array<std::array<float, 2>, 9> at{{{0, 0}, {.012f, .003f}, {-.011f, .005f},
        {.004f, .012f}, {-.004f, -.011f}, {.010f, -.009f}, {-.014f, -.006f}, {.016f, .014f},
        {-.007f, .017f}}};
    for (const auto& [x, z] : at) {
        auto pill = Sphere(.0058f, 14, 8);
        TransformMesh(pill, Mat4::Translation({x, height(x, z) + .0019f, z}) *
                                Mat4::Scale({1.0f, .38f, 1.0f}));
        SetMaterial(pill, GoodsMat::Chalk);
        AppendMesh(mesh, pill);
    }
    mesh.closed = false;
    return mesh;
}

// Notebook: a worn leather-bound pocket book closed by a wrapped strap, 12 x 9 cm.
inline MeshData NotebookMesh() {
    MeshData mesh{};
    const auto add = [&](MeshData part, Mat4 t, float material, float cavity = 0.0f) {
        SetMaterial(part, material, cavity);
        TransformMesh(part, t);
        AppendMesh(mesh, part);
    };
    constexpr float W = .045f, H = .060f, T = .010f;
    add(RoundedBox({W, H, .0012f}, .0010f, 2), Mat4::Translation({0, 0, -T + .0012f}), GoodsMat::Leather);
    add(RoundedBox({W, H, .0012f}, .0010f, 2), Mat4::Translation({0, 0, T - .0012f}), GoodsMat::Leather);
    add(RoundedBox({.0030f, H, T}, .0028f, 3), Mat4::Translation({-W + .0006f, 0, 0}), GoodsMat::Leather);
    add(RoundedBox({W - .0030f, H - .0025f, T - .0020f}, .0008f, 1), Mat4::Translation({-.0012f, 0, 0}),
        GoodsMat::Paper, .3f);
    // The strap: twice round the middle, then a loose tail hanging from the front.
    for (float y : {.004f, -.004f}) {
        std::vector<LoftRing> rings;
        constexpr int N = 48;
        for (int i = 0; i <= N; ++i) {
            const float a = i * 2.0f * Pi / N;
            const float c = std::cos(a), s = std::sin(a);
            const float sx = std::copysign(std::pow(std::abs(c), .18f), c);
            const float sz = std::copysign(std::pow(std::abs(s), .18f), s);
            const Vec3 at{(W + .0012f) * sx, y + .0012f * std::sin(a * 3.0f), (T + .0010f) * sz};
            rings.push_back({at, {0, 1, 0}, Normalize({sz * (T / W), 0, sx}), .0032f, .0006f, 2.0f});
        }
        rings.pop_back();
        auto band = Loft(rings, 6, rings.front().center, rings.back().center);
        add(band, Mat4::Translation({0, 0, 0}), GoodsMat::Leather, .1f);
    }
    const std::array<Vec3, 5> tail{{{.010f, -.002f, -T - .0016f}, {.020f, -.010f, -T - .0030f},
        {.026f, -.024f, -T - .0038f}, {.028f, -.040f, -T - .0030f}, {.030f, -.052f, -T - .0040f}}};
    std::vector<LoftRing> rings;
    for (size_t i = 0; i < tail.size(); ++i) {
        const Vec3 t = Normalize(tail[std::min(i + 1, tail.size() - 1)] - tail[i ? i - 1 : 0]);
        rings.push_back({tail[i], Normalize(Cross({0, 0, 1}, t)), {0, 0, 1}, .0030f, .0006f, 2.0f});
    }
    add(Loft(rings, 6, tail.front(), tail.back()), Mat4::Translation({0, 0, 0}), GoodsMat::Leather);
    return mesh;
}

// Coldblood Dew: a clot of dark blood with a drop welling from it, in rings like a splash, 6 cm.
inline MeshData ColdbloodMesh() {
    const auto sdf = [](Vec3 p) {
        const float r = Length(Vec3{p.x, 0, p.z});
        float d = SdEllipsoid(p, {0, -.002f, 0}, {.028f, .009f, .028f});
        d -= .0010f * std::sin(r * 900.0f) * std::clamp(r / .012f, 0.0f, 1.0f); // ripples
        d = SmoothUnion(d, SdEllipsoid(p, {0, .010f, 0}, {.010f, .011f, .010f}), .008f);
        d = SmoothUnion(d, SdEllipsoid(p, {0, .023f, 0}, {.0058f, .0068f, .0058f}), .004f);
        return d + .0006f * GoodsNoise(p * 160.0f);
    };
    MeshData mesh = SdfWrap(sdf, GoodsMat::Glow, {0, .002f, 0}, .05f, 64, 40);
    mesh.closed = false;
    return mesh;
}
// Fire Paper / Bolt Paper (by `material`): one ragged scrap of charred paper, after the menu
// art: a curled, crumpled wing tapering to a point. Held by its left end (the origin), the rest
// reaching right (+x, 11 cm) toward the blade; y up, +z its face. Two-sided; aux.y is how near
// the torn edge a point is (0 inside .. 1 at it) for the shader's embers / sparks.
inline MeshData RubPaperMesh(float material) {
    constexpr u32 Nu = 28, Nv = 9;
    constexpr float Left = -.015f, Length = .110f;
    MeshData mesh{};
    const auto at = [&](float u, float v) {
        // A wing: narrow where it is held, broadest a third along, then torn to a point, its
        // spine arching up; the edges ragged.
        const float w = .030f * (.45f + .55f * std::sin(Pi * std::min(u / .7f, 1.0f) * .5f + .2f)) *
                            std::pow(1.0f - u, .75f) +
                        .0022f * GoodsNoise({u * 31.0f, v > 0 ? 3.0f : 7.0f, 1.0f}) + .0006f;
        const float x = Left + Length * u;
        const float spine = .024f * std::sin(Pi * u * .85f) * (.4f + u);
        // Twisted along its length and bent back, then crumpled.
        const float twist = .75f * u, across = v * w;
        float y = spine + across * std::cos(twist) + .003f * GoodsNoise({x * 120.0f, 4.0f, 9.0f});
        float z = across * std::sin(twist) - .016f * std::sin(u * Pi * .9f) + .0035f * v * v +
                  .0022f * GoodsNoise({x * 260.0f, y * 260.0f, 2.0f}) + .0010f * GoodsNoise({x * 700.0f, y * 700.0f, 5.0f});
        return Vec3{x, y, z};
    };
    for (u32 i = 0; i <= Nu; ++i) {
        for (u32 j = 0; j <= Nv; ++j) {
            const float u = float(i) / Nu, v = float(j) / Nv * 2.0f - 1.0f, e = 1e-3f;
            const Vec3 p = at(u, v);
            const Vec3 n = Normalize(Cross(at(std::min(u + e, 1.0f), v) - at(std::max(u - e, 0.0f), v),
                                           at(u, std::min(v + e, 1.0f)) - at(u, std::max(v - e, -1.0f))));
            const float edge = std::max({std::abs(v), std::pow(u, 4.0f), 1.0f - std::min(u * 12.0f, 1.0f)});
            mesh.vertices.push_back({p, n, {material, edge}});
        }
    }
    for (u32 i = 0; i < Nu; ++i) {
        for (u32 j = 0; j < Nv; ++j) {
            const u32 a = i * (Nv + 1) + j, b = a + Nv + 1;
            mesh.indices.insert(mesh.indices.end(), {a, b, b + 1, a, b + 1, a + 1});
        }
    }
    mesh.closed = false;
    return mesh;
}

// The Empty Phantasm Shell: the husk a phantasm shed, after the menu art: a ribbed, slug-like
// hook curving down and up again, thick and blunt at the held end (the origin), thinning to a
// curled tip; drips of its slime hanging from the belly. x right, y up, +z toward the eye.
inline MeshData PhantasmShellMesh() {
    constexpr int N = 56;
    std::vector<LoftRing> rings;
    const auto path = [](float t) {
        // A U: down from the blunt, held arm, round the belly, up the thinner arm to a curl.
        const float a = Pi * (.80f + 1.40f * t);
        const float r = .036f - .008f * t;
        return Vec3{.031f + r * std::cos(a) + .006f * t * t,
                    -.021f + r * std::sin(a) + .028f * std::pow(t, 3.0f), .006f * std::sin(t * Pi)};
    };
    Vec3 u_prev{0, 0, 1};
    for (int i = 0; i <= N; ++i) {
        const float t = float(i) / N;
        const Vec3 p = path(t);
        const Vec3 tan = Normalize(path(std::min(t + .01f, 1.0f)) - path(std::max(t - .01f, 0.0f)));
        Vec3 u = Normalize(u_prev - tan * Dot(u_prev, tan));
        u_prev = u;
        const float rib = 1.0f + .07f * std::sin(t * 70.0f);
        const float r = (.0125f * (1.0f - .78f * std::pow(t, 1.4f)) + .0012f) * rib;
        rings.push_back({p, u, Cross(tan, u), r, r * .92f, 2.0f});
    }
    MeshData mesh = Loft(rings, 16, path(0) - Normalize(path(.02f) - path(0)) * .010f,
                         path(1) + Normalize(path(1) - path(.98f)) * .002f);
    // Slime dripping from the belly.
    for (int k = 0; k < 5; ++k) {
        const float t = .32f + .07f * k;
        const Vec3 p = path(t) - Vec3{0, .009f * (1.0f - .78f * std::pow(t, 1.4f)) + .002f, 0};
        const float len = .006f + .005f * ((k * 7) % 3);
        const std::array<Vec3, 3> drip{{p, p - Vec3{0, len * .6f, 0}, p - Vec3{0, len, 0}}};
        AppendMesh(mesh, Sweep(drip, .0018f, .0005f, 6));
    }
    SetMaterial(mesh, GoodsMat::Phantasm);
    return mesh;
}

// Ambient occlusion baked into aux.y (max with what the mesh has): 14 cosine-weighted rays from
// each vertex, `reach` long, against the item's own triangles. Shade::Goods darkens and dirties
// the hollows by it (it casts no shadows: a bell's mouth needs a reach across it). Once per mesh
// at the library build (a cold path); triangles beyond the reach are skipped by their bounds.
inline void BakeOcclusion(MeshData& m, float reach = .012f) {
    constexpr int Rays = 14;
    constexpr float Lift = .0002f;
    const float Reach = reach;
    struct Tri { Vec3 a, e1, e2, lo, hi; };
    std::vector<Tri> tris;
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        const Vec3 a = m.vertices[m.indices[i]].position, b = m.vertices[m.indices[i + 1]].position,
                   c = m.vertices[m.indices[i + 2]].position;
        tris.push_back({a, b - a, c - a,
                        {std::min({a.x, b.x, c.x}), std::min({a.y, b.y, c.y}), std::min({a.z, b.z, c.z})},
                        {std::max({a.x, b.x, c.x}), std::max({a.y, b.y, c.y}), std::max({a.z, b.z, c.z})}});
    }
    std::vector<const Tri*> near;
    for (auto& v : m.vertices) {
        const Vec3 o = v.position + v.normal * Lift;
        near.clear();
        for (const Tri& t : tris) {
            if (o.x < t.lo.x - Reach || o.x > t.hi.x + Reach || o.y < t.lo.y - Reach ||
                o.y > t.hi.y + Reach || o.z < t.lo.z - Reach || o.z > t.hi.z + Reach) continue;
            near.push_back(&t);
        }
        // A frame round the normal; golden-spiral directions over the cosine-weighted hemisphere.
        const Vec3 n = v.normal;
        const Vec3 u = Normalize(Cross(n, std::abs(n.y) < .9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}));
        const Vec3 w = Cross(n, u);
        int hits = 0;
        for (int r = 0; r < Rays; ++r) {
            const float k = (r + .5f) / Rays, phi = r * 2.39996323f;
            const float sr = std::sqrt(k), cz = std::sqrt(1.0f - k);
            const Vec3 d = u * (sr * std::cos(phi)) + w * (sr * std::sin(phi)) + n * cz;
            for (const Tri* t : near) { // Moller-Trumbore
                const Vec3 pv = Cross(d, t->e2);
                const float det = Dot(t->e1, pv);
                if (std::abs(det) < 1e-12f) continue;
                const Vec3 tv = o - t->a;
                const float bu = Dot(tv, pv) / det;
                if (bu < 0.0f || bu > 1.0f) continue;
                const Vec3 qv = Cross(tv, t->e1);
                const float bv = Dot(d, qv) / det;
                if (bv < 0.0f || bu + bv > 1.0f) continue;
                const float dist = Dot(t->e2, qv) / det;
                if (dist > 0.0f && dist < Reach) { ++hits; break; }
            }
        }
        v.aux.y = std::max(v.aux.y, std::min(1.1f * hits / Rays, 1.0f));
    }
}

// The Sangiran 17 cranium (Madman's Knowledge, Great One's Wisdom) from its embedded data
// (src/resources/vr/sangiran17.bin): "S17M", u32 vertices, u32 indices; per vertex int16 xyz
// (0.1 mm), int8 normal xyz, u8 baked cavity; u16 indices. Life size, its frame the Frankfurt
// plane (y = 0), the face to -z. Built from photographs of the cast and its published lateral
// view: the face a thin-plate-spline warp of a CC0 skull (CDmir, OpenGameArt), the vault from
// the measured profile. Empty on bad data.
inline MeshData SkullMeshFrom(std::span<const u8> data) {
    MeshData mesh{};
    const auto u32_at = [&](size_t at) {
        return u32(data[at]) | u32(data[at + 1]) << 8 | u32(data[at + 2]) << 16 | u32(data[at + 3]) << 24;
    };
    if (data.size() < 12 || data[0] != 'S' || data[1] != '1' || data[2] != '7' || data[3] != 'M') {
        return mesh;
    }
    const u32 nv = u32_at(4), ni = u32_at(8);
    if (data.size() != 12 + size_t(nv) * 10 + size_t(ni) * 2) return mesh;
    const auto i16_at = [&](size_t at) { return float(static_cast<s16>(data[at] | data[at + 1] << 8)); };
    const auto i8_at = [&](size_t at) { return float(static_cast<s8>(data[at])); };
    for (u32 i = 0; i < nv; ++i) {
        const size_t at = 12 + size_t(i) * 10;
        mesh.vertices.push_back({{i16_at(at) * 1e-4f, i16_at(at + 2) * 1e-4f, i16_at(at + 4) * 1e-4f},
                                 Normalize({i8_at(at + 6), i8_at(at + 7), i8_at(at + 8)}),
                                 {GoodsMat::Bone, data[at + 9] / 255.0f}});
    }
    for (u32 i = 0; i < ni; ++i) {
        const size_t at = 12 + size_t(nv) * 10 + size_t(i) * 2;
        const u32 index = u32(data[at] | data[at + 1] << 8);
        if (index >= nv) return {};
        mesh.indices.push_back(index);
    }
    mesh.closed = false; // the teeth are separate parts
    return mesh;
}
// Reuse established geometry and procedural materials for otherwise icon-only goods.
// Variant order matches the contiguous MeshId entries beginning with HunterMark.
inline MeshData ReusedGoodsMesh(int variant) {
    MeshData mesh;
    if (variant == 0) {
        mesh = RubPaperMesh(GoodsMat::Paper);
        // A raised, dark Hunter rune on the folded parchment.
        for (const auto& path : std::array<std::array<Vec3, 3>, 3>{{
            {{{.035f,-.020f,.002f},{.035f,.004f,.006f},{.035f,.024f,.005f}}},
            {{{.015f,.020f,.004f},{.035f,.004f,.006f},{.055f,.020f,.004f}}},
            {{{.024f,-.012f,.004f},{.035f,-.020f,.002f},{.046f,-.012f,.004f}}}}}) {
            auto mark = Sweep(path, .0012f, .0012f, 6);
            SetMaterial(mark, GoodsMat::Leather);
            AppendMesh(mesh, mark);
        }
    } else if (variant == 1 || variant == 2) {
        mesh = NotebookMesh();
        for (auto& v : mesh.vertices) {
            if (v.aux.x == GoodsMat::Leather)
                v.aux.x = variant == 1 ? GoodsMat::Velvet : GoodsMat::Linen;
        }
    } else if (variant == 3 || variant == 4) {
        mesh = ResonantBellMesh();
        for (auto& v : mesh.vertices) {
            if (v.aux.x == GoodsMat::Bronze || v.aux.x == GoodsMat::Iron)
                v.aux.x = variant == 3 ? GoodsMat::Bronze : GoodsMat::Iron;
        }
    } else if (variant == 5) {
        mesh = Sphere(.013f, 24, 16);
        for (auto& v : mesh.vertices) v.position = v.position * (1.0f + .10f * GoodsNoise(v.position * 750.0f));
        SetMaterial(mesh, GoodsMat::Blood);
    } else if (variant == 6) {
        mesh = RoundedBox({.022f,.027f,.017f}, .009f, 4);
        SetMaterial(mesh, GoodsMat::Linen);
        auto cap = RoundedBox({.012f,.005f,.010f}, .003f, 3);
        SetMaterial(cap, GoodsMat::Cork);
        TransformMesh(cap, Mat4::Translation({0,.029f,0}));
        AppendMesh(mesh, cap);
    } else {
        mesh = PhantasmShellMesh();
        SetMaterial(mesh, GoodsMat::Bone);
    }
    BakeOcclusion(mesh);
    return mesh;
}

} // namespace Vulkan::VrOverlay
