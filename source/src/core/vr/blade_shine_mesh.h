// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The split blades' shine model (core/vr/dual_blades.h; bloodborne_weapon_shape.cpp builds it once
// per blade form on its worker, from the blade's triangles as the game poses them in its grip's
// frame): the triangles whose corners are all steel (its edge bones'; all of them when it names
// none), each corner once, its normal the area-weighted sum of its triangles' turned outward (by
// the sign of the volume they enclose: the shader draws only the faces toward the eye), and the
// blade's line in that frame, the band's way: from the grip (the frame's origin) toward its
// farthest corner, from the nearest corner's reach along it (never behind the grip) to the
// farthest's. Bad input (an index out of range, a steel list of another length, a triangle with no
// area) is left out, never read past.
//
// Pure (tests/test_dual_blades.cpp).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace VR::DualBlades {

using P3 = std::array<float, 3>;

struct ShineMesh {
    std::vector<P3> position;
    std::vector<P3> normal; // unit, outward
    std::vector<std::uint32_t> indices;
    P3 base{}; // the line (the grip's frame): the guard end ...
    P3 tip{};  // ... and the tip (both zero: no line)
    std::size_t triangles_in{0}; // the part's
    bool steel_only{false};      // its edge bones named: only theirs
};

namespace ShineDetail {
inline P3 Sub(const P3& a, const P3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
inline P3 Cross(const P3& a, const P3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline float Dot(const P3& a, const P3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline bool Finite(const P3& a) {
    return std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]);
}
} // namespace ShineDetail

inline ShineMesh BuildShineMesh(const std::vector<P3>& verts,
                                const std::vector<std::uint32_t>& tris,
                                const std::vector<std::uint8_t>& steel) {
    using namespace ShineDetail;
    ShineMesh out{};
    out.triangles_in = tris.size() / 3;
    const bool steel_known = steel.size() == verts.size();
    out.steel_only =
        steel_known && std::find(steel.begin(), steel.end(), std::uint8_t{1}) != steel.end();
    std::vector<std::int32_t> remap(verts.size(), -1);
    for (std::size_t t = 0; t + 2 < tris.size(); t += 3) {
        const std::uint32_t c[3] = {tris[t], tris[t + 1], tris[t + 2]};
        bool use = true;
        for (const std::uint32_t v : c) {
            use = use && v < verts.size() && Finite(verts[v]) && (!out.steel_only || steel[v] != 0);
        }
        if (!use) {
            continue;
        }
        const P3 n = Cross(Sub(verts[c[1]], verts[c[0]]), Sub(verts[c[2]], verts[c[0]]));
        if (!(Dot(n, n) > 1e-14f)) {
            continue; // no area: no direction to give
        }
        for (const std::uint32_t v : c) {
            if (remap[v] < 0) {
                remap[v] = static_cast<std::int32_t>(out.position.size());
                out.position.push_back(verts[v]);
                out.normal.push_back({0.0f, 0.0f, 0.0f});
            }
            P3& sum = out.normal[static_cast<std::size_t>(remap[v])];
            sum = {sum[0] + n[0], sum[1] + n[1], sum[2] + n[2]};
            out.indices.push_back(static_cast<std::uint32_t>(remap[v]));
        }
    }
    if (out.indices.empty()) {
        out.position.clear();
        out.normal.clear();
        return out;
    }
    // Outward: the volume the triangles enclose (about their centroid) is positive when they wind
    // the way their normals point out; else every normal is turned.
    P3 centre{};
    for (const P3& p : out.position) {
        centre = {centre[0] + p[0], centre[1] + p[1], centre[2] + p[2]};
    }
    const float inv = 1.0f / static_cast<float>(out.position.size());
    centre = {centre[0] * inv, centre[1] * inv, centre[2] * inv};
    double volume = 0.0;
    for (std::size_t i = 0; i + 2 < out.indices.size(); i += 3) {
        const P3 a = Sub(out.position[out.indices[i]], centre);
        const P3 b = Sub(out.position[out.indices[i + 1]], centre);
        const P3 c = Sub(out.position[out.indices[i + 2]], centre);
        volume += static_cast<double>(Dot(a, Cross(b, c)));
    }
    const float sign = volume < 0.0 ? -1.0f : 1.0f;
    for (P3& n : out.normal) {
        const float l = std::sqrt(Dot(n, n));
        n = l > 1e-20f ? P3{sign * n[0] / l, sign * n[1] / l, sign * n[2] / l}
                       : P3{0.0f, 0.0f, 1.0f};
    }
    // The line: the grip toward the farthest corner, from the nearest corner's reach along it.
    float reach2 = 0.0f;
    P3 farthest{};
    for (const P3& p : out.position) {
        const float d2 = Dot(p, p);
        if (d2 > reach2) {
            reach2 = d2;
            farthest = p;
        }
    }
    if (reach2 > 1e-8f) {
        const float reach = std::sqrt(reach2);
        const P3 dir{farthest[0] / reach, farthest[1] / reach, farthest[2] / reach};
        float lo = reach;
        float hi = 0.0f;
        for (const P3& p : out.position) {
            const float s = Dot(p, dir);
            lo = std::min(lo, s);
            hi = std::max(hi, s);
        }
        lo = std::max(lo, 0.0f);
        out.base = {dir[0] * lo, dir[1] * lo, dir[2] * lo};
        out.tip = {dir[0] * hi, dir[1] * hi, dir[2] * hi};
    }
    return out;
}

} // namespace VR::DualBlades
