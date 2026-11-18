// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cmath>
#include <span>
#include <vector>

#include "core/vr/sever_mesh.h"

// Beast Claw dismemberment, the pose (the snapshot plan):
// at the kill the game thread captures only the dead enemy's bone matrices; the worker puts the
// model's bind-pose meshes (sever_model.h, read from its file) into that frozen pose here, then
// cuts them. Linear blend skinning, as the game skins: each vertex moved by its bones' skin
// transforms (the bind pose undone, the pose done), weighted. Matrices are row vectors, as the
// model's (v' = v * M, M's last row the translation). Pure: no guest memory.
namespace VR::Sever {

using Matrix4 = std::array<float, 16>;

// A bone's skin transform: rows 0..2 the linear part, row 3 the translation.
struct SkinTransform {
    std::array<float, 12> m{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
};

// The inverse of an affine row-vector matrix ([A 0; t 1] -> [A^-1 0; -t A^-1 1]); false when
// it is singular or not finite.
inline bool InvertAffine(const Matrix4& m, Matrix4& out) {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9],
                i = m[10];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    if (!std::isfinite(det) || std::abs(det) < 1e-12f) {
        return false;
    }
    const float k = 1.0f / det;
    out = {A * k, (c * h - b * i) * k, (b * f - c * e) * k, 0,
           B * k, (a * i - c * g) * k, (c * d - a * f) * k, 0,
           C * k, (b * g - a * h) * k, (a * e - b * d) * k, 0,
           0,     0,                   0,                   1};
    const float tx = m[12], ty = m[13], tz = m[14];
    out[12] = -(tx * out[0] + ty * out[4] + tz * out[8]);
    out[13] = -(tx * out[1] + ty * out[5] + tz * out[9]);
    out[14] = -(tx * out[2] + ty * out[6] + tz * out[10]);
    for (float x : out) {
        if (!std::isfinite(x)) {
            return false;
        }
    }
    return true;
}

// A bone's skin transform from its bind pose and its pose (both world, row vectors):
// inverse(bind) * pose. False for a degenerate bind.
inline bool SkinFrom(const Matrix4& bind, const Matrix4& pose, SkinTransform& out) {
    Matrix4 inv{};
    if (!InvertAffine(bind, inv)) {
        return false;
    }
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 3; ++c) {
            float s = 0;
            for (int k = 0; k < 4; ++k) {
                s += inv[r * 4 + k] * pose[k * 4 + c];
            }
            out.m[r * 3 + c] = s;
        }
    }
    return true;
}

// The bind-pose vertices put into the pose: positions and normals blended by their bones' skin
// transforms (normals renormalized), UVs and skin kept. False (out empty) for a bone past the
// transforms or a result not finite.
inline bool Pose(std::span<const Vertex> bind, std::span<const SkinTransform> skins,
                 std::vector<Vertex>& out) {
    out.assign(bind.begin(), bind.end());
    for (Vertex& v : out) {
        Vec3 p{}, n{};
        const Vec3 p0 = v.position, n0 = v.normal;
        for (const Influence& in : v.skin) {
            if (!(in.weight > 0.0f)) {
                continue;
            }
            if (in.bone >= skins.size()) {
                out.clear();
                return false;
            }
            const auto& m = skins[in.bone].m;
            const float w = in.weight;
            p.x += w * (p0.x * m[0] + p0.y * m[3] + p0.z * m[6] + m[9]);
            p.y += w * (p0.x * m[1] + p0.y * m[4] + p0.z * m[7] + m[10]);
            p.z += w * (p0.x * m[2] + p0.y * m[5] + p0.z * m[8] + m[11]);
            n.x += w * (n0.x * m[0] + n0.y * m[3] + n0.z * m[6]);
            n.y += w * (n0.x * m[1] + n0.y * m[4] + n0.z * m[7]);
            n.z += w * (n0.x * m[2] + n0.y * m[5] + n0.z * m[8]);
        }
        const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
            !std::isfinite(len) || len < 1e-12f) {
            out.clear();
            return false;
        }
        v.position = p;
        v.normal = {n.x / len, n.y / len, n.z / len};
    }
    return true;
}

// A posed joint (its pose matrix's translation).
inline Vec3 Joint(const Matrix4& pose) {
    return {pose[12], pose[13], pose[14]};
}

} // namespace VR::Sever
