// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "core/vr/sever_model.h"
#include "core/vr/sever_policy.h"
#include "core/vr/sever_pose.h"

// Beast Claw dismemberment, the live pose:
// what the game thread copies at the kill (the enemy's Havok skeleton pose, its FLVER-to-Havok
// bone map, its world matrix) turned into the model's FLVER bone matrices on the worker. Pure:
// copies in, matrices out; no guest memory.
//  - Havok: each bone's local hkQsTransform (translation, quaternion x y z w, scale), parents
//    before children; parent * local = {parent.t + parent.q (local.t), parent.q local.q,
//    parent.s local.s} (the parent's scale is not applied to the translation, as Havok does).
//  - The model's frame: Havok's model space is the FLVER's (the hunter's rig, c0000_rig.h). RigGap
//    checks it per model before any cut: the reference pose put through the same map must land
//    every bone's joint on its bind joint from the file.
//  - Matrices are row vectors (sever_pose.h): v' = v * M, scale, then rotation, then translation.
namespace VR::Sever {

struct QsPose {
    Vec3 t{};
    std::array<float, 4> q{0, 0, 0, 1}; // x y z w
    Vec3 s{1, 1, 1};
};

namespace Rig {
inline Vec3 Rotate(const std::array<float, 4>& q, Vec3 v) {
    // v + 2w (q x v) + 2 q x (q x v)
    const Vec3 u{q[0], q[1], q[2]};
    const Vec3 c{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    const Vec3 cc{u.y * c.z - u.z * c.y, u.z * c.x - u.x * c.z, u.x * c.y - u.y * c.x};
    return {v.x + 2 * (q[3] * c.x + cc.x), v.y + 2 * (q[3] * c.y + cc.y),
            v.z + 2 * (q[3] * c.z + cc.z)};
}
inline std::array<float, 4> Mul(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
inline bool Finite(const QsPose& x) {
    for (float f : {x.t.x, x.t.y, x.t.z, x.q[0], x.q[1], x.q[2], x.q[3], x.s.x, x.s.y, x.s.z}) {
        if (!std::isfinite(f)) {
            return false;
        }
    }
    return true;
}
// rows * cols, both row-vector 4x4.
inline Matrix4 Mul(const Matrix4& a, const Matrix4& b) {
    Matrix4 out{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float s = 0;
            for (int k = 0; k < 4; ++k) {
                s += a[r * 4 + k] * b[k * 4 + c];
            }
            out[r * 4 + c] = s;
        }
    }
    return out;
}
} // namespace Rig

// Havok parent * local (the parent's scale not applied to the translation).
inline QsPose Compose(const QsPose& a, const QsPose& b) {
    const Vec3 t = Rig::Rotate(a.q, b.t);
    return {{a.t.x + t.x, a.t.y + t.y, a.t.z + t.z},
            Rig::Mul(a.q, b.q),
            {a.s.x * b.s.x, a.s.y * b.s.y, a.s.z * b.s.z}};
}

// An affine transform, column vectors: 3 rows of {R_i0, R_i1, R_i2, t_i} (ChrCtrl's W layout).
using Affine = std::array<float, 12>;
inline Affine AffineOf(const QsPose& x) {
    Affine m{};
    const float s[3] = {x.s.x, x.s.y, x.s.z};
    const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int j = 0; j < 3; ++j) {
        const Vec3 c = Rig::Rotate(x.q, axes[j]); // column j: the turned axis j, scaled
        m[0 * 4 + j] = c.x * s[j];
        m[1 * 4 + j] = c.y * s[j];
        m[2 * 4 + j] = c.z * s[j];
    }
    m[3] = x.t.x;
    m[7] = x.t.y;
    m[11] = x.t.z;
    return m;
}
// The inverse of AffineOf(x): diag(1 / s) R^T, and its translation.
inline Affine InverseOf(const QsPose& x) {
    const Affine f = AffineOf(QsPose{{}, x.q, {1, 1, 1}});
    const float s[3] = {x.s.x, x.s.y, x.s.z};
    Affine m{};
    for (int i = 0; i < 3; ++i) {
        const float k = std::abs(s[i]) > 1e-12f ? 1.0f / s[i] : 0.0f;
        for (int j = 0; j < 3; ++j) {
            m[i * 4 + j] = f[j * 4 + i] * k;
        }
        m[i * 4 + 3] = -(m[i * 4 + 0] * x.t.x + m[i * 4 + 1] * x.t.y + m[i * 4 + 2] * x.t.z);
    }
    return m;
}
inline Affine Mul(const Affine& a, const Affine& b) {
    Affine m{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            m[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
                           a[i * 4 + 2] * b[2 * 4 + j] + (j == 3 ? a[i * 4 + 3] : 0.0f);
        }
    }
    return m;
}
inline Vec3 Apply(const Affine& a, Vec3 v) {
    return {a[0] * v.x + a[1] * v.y + a[2] * v.z + a[3], a[4] * v.x + a[5] * v.y + a[6] * v.z + a[7],
            a[8] * v.x + a[9] * v.y + a[10] * v.z + a[11]};
}

// Local to model over the parents (each before its child; -1 a root). False (out empty) for a
// parent that does not precede its bone, a size mismatch or a value not finite.
inline bool HavokModel(std::span<const QsPose> local, std::span<const std::int16_t> parents,
                       std::vector<QsPose>& model) {
    model.clear();
    if (local.size() != parents.size() || local.size() > MaxBones) {
        return false;
    }
    model.resize(local.size());
    for (std::size_t i = 0; i < local.size(); ++i) {
        const int p = parents[i];
        if (!Rig::Finite(local[i]) || p >= static_cast<int>(i)) {
            model.clear();
            return false;
        }
        if (p < 0) {
            model[i] = local[i];
            continue;
        }
        model[i] = Compose(model[static_cast<std::size_t>(p)], local[i]);
    }
    return true;
}

// A transform as a row-vector matrix: row i the scaled, rotated axis i; row 3 the translation.
inline Matrix4 RowMatrix(const QsPose& x) {
    Matrix4 m{};
    const float s[3] = {x.s.x, x.s.y, x.s.z};
    const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int i = 0; i < 3; ++i) {
        const Vec3 r = Rig::Rotate(x.q, axes[i]);
        m[i * 4 + 0] = r.x * s[i];
        m[i * 4 + 1] = r.y * s[i];
        m[i * 4 + 2] = r.z * s[i];
    }
    m[12] = x.t.x;
    m[13] = x.t.y;
    m[14] = x.t.z;
    m[15] = 1;
    return m;
}

// Every FLVER bone's posed model matrix: its Havok bone's (the map), or, with none (a helper the
// skeleton leaves out), its FLVER parent's posed matrix times its bind offset from that parent
// (it rides along as in the bind pose). False for a map, a bone or a parent out of range.
inline bool FlverPose(std::span<const ModelBone> bones, std::span<const std::int16_t> map,
                      std::span<const QsPose> havok_model, std::vector<Matrix4>& out) {
    out.clear();
    if (map.size() != bones.size() || bones.size() > MaxBones) {
        return false;
    }
    out.resize(bones.size());
    for (std::size_t b = 0; b < bones.size(); ++b) {
        const int h = map[b];
        if (h >= 0 && static_cast<std::size_t>(h) < havok_model.size()) {
            out[b] = RowMatrix(havok_model[static_cast<std::size_t>(h)]);
            continue;
        }
        const int p = bones[b].parent;
        if (p < 0) {
            out[b] = bones[b].bind; // a root with no Havok bone: where the file puts it
            continue;
        }
        if (p >= static_cast<int>(b)) {
            out.clear();
            return false; // (the reader's bones list parents first)
        }
        Matrix4 parent_inverse{};
        if (!InvertAffine(bones[static_cast<std::size_t>(p)].bind, parent_inverse)) {
            out.clear();
            return false;
        }
        out[b] = Rig::Mul(Rig::Mul(bones[b].bind, parent_inverse),
                          out[static_cast<std::size_t>(p)]);
    }
    return true;
}

// The rig check: the Havok reference pose (model space) through the same map; the largest gap (m)
// between a mapped bone's joint and its bind joint from the file. Infinity when nothing maps.
inline float RigGap(std::span<const ModelBone> bones, std::span<const std::int16_t> map,
                    std::span<const QsPose> reference_model) {
    float gap = 0;
    bool any = false;
    for (std::size_t b = 0; b < bones.size() && b < map.size(); ++b) {
        const int h = map[b];
        if (h < 0 || static_cast<std::size_t>(h) >= reference_model.size()) {
            continue;
        }
        const Vec3 a = reference_model[static_cast<std::size_t>(h)].t, j = bones[b].joint;
        const float d = std::sqrt((a.x - j.x) * (a.x - j.x) + (a.y - j.y) * (a.y - j.y) +
                                  (a.z - j.z) * (a.z - j.z));
        if (!std::isfinite(d)) {
            return INFINITY;
        }
        gap = std::max(gap, d);
        any = true;
    }
    return any ? gap : INFINITY;
}

// The skin transforms (sever_pose.h Pose) of every bone. False for a degenerate bind.
inline bool SkinsOf(std::span<const ModelBone> bones, std::span<const Matrix4> posed,
                    std::vector<SkinTransform>& out) {
    out.assign(bones.size(), {});
    for (std::size_t b = 0; b < bones.size() && b < posed.size(); ++b) {
        if (!SkinFrom(bones[b].bind, posed[b], out[b])) {
            out.clear();
            return false;
        }
    }
    return posed.size() == bones.size();
}

// The character's world matrix (ChrCtrl's W: three rows {R_i, t_i}: world_i = R_i . model + t_i)
// undone: a world point, or (no translation) a world direction, in model space. False for a
// singular W.
inline bool ToModel(const std::array<float, 12>& w, Vec3 world, bool point, Vec3& out) {
    const float a = w[0], b = w[1], c = w[2], d = w[4], e = w[5], f = w[6], g = w[8], h = w[9],
                i = w[10];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    if (!std::isfinite(det) || std::abs(det) < 1e-12f) {
        return false;
    }
    const Vec3 v = point ? Vec3{world.x - w[3], world.y - w[7], world.z - w[11]} : world;
    // inverse(R) v, by the adjugate.
    const float k = 1.0f / det;
    out = {(A * v.x + (c * h - b * i) * v.y + (b * f - c * e) * v.z) * k,
           (B * v.x + (a * i - c * g) * v.y + (c * d - a * f) * v.z) * k,
           (C * v.x + (b * g - a * h) * v.y + (a * e - b * d) * v.z) * k};
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

} // namespace VR::Sever
