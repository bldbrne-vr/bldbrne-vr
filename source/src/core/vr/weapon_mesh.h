// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/vr/arm_ik.h"
#include "core/vr/c0000_rig.h"

// A weapon part's model as the hand holds it, for the solid weapons' fitted shapes
// (core/vr/weapon_fit.h; bloodborne_weapon_shape.cpp reads it). Read from the user's own game
// files (parts/wp_a_XXXX.partsbnd.dcx -> WP_A_XXXX.flver, part k > 0: WP_A_XXXX_k.flver), never
// shipped: the corners of its full-detail triangles in the part's model space, posed by the part's
// live skeleton (the game's hkaPose of that part, matched by bone name; a FLVER bone the skeleton
// lacks follows its parent as in the bind pose).
//
// FLVER2 as Bloodborne writes it (version 0x20014, little endian): header counts (dummies +0x14,
// materials +0x18, bones +0x1C, meshes +0x20, vertex buffers +0x24, face sets +0x50, layouts
// +0x54), the data offset +0x0C; meshes of 0x30 bytes (default bone +0x10, bone map +0x14 / +0x1C,
// face sets +0x20 / +0x24, vertex buffers +0x28 / +0x2C), face sets of 0x20 (flags, strip +4,
// count +8, offset +0x0C, index size +0x18), vertex buffers of 0x20 (layout +4, stride +8, count
// +0x0C, offset +0x1C), layout members of 0x14 (offset +4, type +8, semantic +0x0C, index +0x10).
// A vertex is skinned when its bone weights are given (its bone: the heaviest), else it sits on
// one bone (the normal's fourth byte, through the mesh's bone map, else the mesh's default bone)
// and is given in that bone's frame.
//
// Pure (tests/test_weapon_fit.cpp).
namespace VR::WeaponMesh {

using Body::Quat;
using Body::Vec3;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using s16 = std::int16_t;
using s32 = std::int32_t;

// 3 x 4 row-major [R | t].
using Mat34 = std::array<float, 12>;

inline Mat34 Identity() {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
}

inline Mat34 Mul(const Mat34& a, const Mat34& b) {
    Mat34 m{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            float v = j == 3 ? a[i * 4 + 3] : 0.0f;
            for (int k = 0; k < 3; ++k) {
                v += a[i * 4 + k] * b[k * 4 + j];
            }
            m[i * 4 + j] = v;
        }
    }
    return m;
}

inline Vec3 Apply(const Mat34& m, Vec3 v) {
    return {m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3],
            m[4] * v.x + m[5] * v.y + m[6] * v.z + m[7],
            m[8] * v.x + m[9] * v.y + m[10] * v.z + m[11]};
}

// The inverse of an affine 3 x 4 (identity when singular).
inline Mat34 Inverse(const Mat34& m) {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9],
                i = m[10];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    if (!(std::abs(det) > 1e-12f)) {
        return Identity();
    }
    const float k = 1.0f / det;
    Mat34 r{A * k, -(b * i - c * h) * k, (b * f - c * e) * k,  0.0f,
            B * k, (a * i - c * g) * k,  -(a * f - c * d) * k, 0.0f,
            C * k, -(a * h - b * g) * k, (a * e - b * d) * k,  0.0f};
    const Vec3 t = Apply(r, {m[3], m[7], m[11]});
    r[3] = -t.x;
    r[7] = -t.y;
    r[11] = -t.z;
    return r;
}

// A FLVER bone's local transform: scale, then rotations about X, Z and Y, then the translation.
inline Mat34 BoneLocal(const std::array<float, 3>& t, const std::array<float, 3>& r,
                       const std::array<float, 3>& s) {
    const float cx = std::cos(r[0]), sx = std::sin(r[0]);
    const float cy = std::cos(r[1]), sy = std::sin(r[1]);
    const float cz = std::cos(r[2]), sz = std::sin(r[2]);
    const Mat34 rx{1, 0, 0, 0, 0, cx, -sx, 0, 0, sx, cx, 0};
    const Mat34 rz{cz, -sz, 0, 0, sz, cz, 0, 0, 0, 0, 1, 0};
    const Mat34 ry{cy, 0, sy, 0, 0, 1, 0, 0, -sy, 0, cy, 0};
    Mat34 m = Mul(ry, Mul(rz, rx));
    for (int row = 0; row < 3; ++row) {
        m[row * 4 + 0] *= s[0];
        m[row * 4 + 1] *= s[1];
        m[row * 4 + 2] *= s[2];
    }
    m[3] = t[0];
    m[7] = t[1];
    m[11] = t[2];
    return m;
}

inline Mat34 FromQuat(const Quat& q, Vec3 t) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    return {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),     t.x,
            2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),     t.y,
            2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y), t.z};
}

struct Mesh {
    std::vector<Vec3> verts; // bind pose, model space (m)
    std::vector<s16> vbone;  // each vertex's bone (FLVER index)
    std::vector<u32> tris;   // triangle list (indices into verts)
    std::vector<std::string> bone_names;
    std::vector<s16> bone_parent;
    std::vector<Mat34> bone_bind; // model-space bind transforms
    std::vector<Body::C0000::Rig::FlverDummy> dummies;
};

namespace Detail {

using Body::C0000::Rig::ReadLe;

inline bool ReadComponents(std::span<const u8> d, std::size_t at, u32 type, float* f, bool& bytes) {
    bytes = false;
    switch (type) {
    case 0x01:
    case 0x02:
    case 0x03: {
        const int n = type == 0x01 ? 2 : type == 0x02 ? 3 : 4;
        for (int c = 0; c < n; ++c) {
            if (!ReadLe(d, at + 4 * static_cast<std::size_t>(c), f[c])) {
                return false;
            }
        }
        return true;
    }
    case 0x10:
    case 0x11:
    case 0x13:
    case 0x2F:
        for (int c = 0; c < 4; ++c) {
            u8 b{};
            if (!ReadLe(d, at + static_cast<std::size_t>(c), b)) {
                return false;
            }
            f[c] = b;
        }
        bytes = true;
        return true;
    case 0x12:
    case 0x15:
    case 0x16:
    case 0x1A:
        for (int c = 0; c < 4; ++c) {
            s16 v{};
            if (!ReadLe(d, at + 2 * static_cast<std::size_t>(c), v)) {
                return false;
            }
            f[c] = v;
        }
        return true;
    default:
        return false;
    }
}

} // namespace Detail

// The model's full-detail triangles, their corners in its bind pose, and its bones. Nullopt (and
// `why`) for a file that is no FLVER2 or holds no mesh.
inline std::optional<Mesh> ParseFlverMesh(std::span<const u8> d, std::string& why) {
    using Detail::ReadLe;
    std::string rig_why;
    const auto rig = Body::C0000::Rig::ParseFlver(d, rig_why);
    if (!rig) {
        why = rig_why;
        return std::nullopt;
    }
    u32 data{};
    s32 dummies{}, materials{}, bones{}, meshes{}, buffers{}, face_sets{}, layouts{};
    if (!ReadLe(d, 0x0C, data) || !ReadLe(d, 0x14, dummies) || !ReadLe(d, 0x18, materials) ||
        !ReadLe(d, 0x1C, bones) || !ReadLe(d, 0x20, meshes) || !ReadLe(d, 0x24, buffers) ||
        !ReadLe(d, 0x50, face_sets) || !ReadLe(d, 0x54, layouts) || meshes < 1 || meshes > 64 ||
        buffers < 1 || buffers > 256 || face_sets < 1 || face_sets > 1024 || layouts < 1 ||
        layouts > 256 || d.size() < 0x80) {
        why = "FLVER without meshes";
        return std::nullopt;
    }
    const u8 header_index_size = d[0x48];
    const std::size_t mesh_at = 0x80 + 0x40 * static_cast<std::size_t>(dummies) +
                                0x20 * static_cast<std::size_t>(materials) +
                                0x80 * static_cast<std::size_t>(bones);
    const std::size_t face_at = mesh_at + 0x30 * static_cast<std::size_t>(meshes);
    const std::size_t buffer_at = face_at + 0x20 * static_cast<std::size_t>(face_sets);
    const std::size_t layout_at = buffer_at + 0x20 * static_cast<std::size_t>(buffers);
    Mesh out;
    out.dummies = rig->dummies;
    const std::size_t nb = rig->bones.size();
    out.bone_names.resize(nb);
    out.bone_parent.resize(nb);
    out.bone_bind.resize(nb);
    std::vector<Mat34> local(nb);
    for (std::size_t b = 0; b < nb; ++b) {
        out.bone_names[b] = rig->bones[b].name;
        out.bone_parent[b] = rig->bones[b].parent;
        local[b] = BoneLocal(rig->bones[b].t, rig->bones[b].r, rig->bones[b].s);
    }
    for (std::size_t b = 0; b < nb; ++b) {
        Mat34 m = Identity();
        s32 at = static_cast<s32>(b);
        for (int hops = 0; at >= 0 && at < static_cast<s32>(nb) && hops < 256; ++hops) {
            m = Mul(local[static_cast<std::size_t>(at)], m);
            at = out.bone_parent[static_cast<std::size_t>(at)];
        }
        out.bone_bind[b] = m;
    }
    for (s32 mi = 0; mi < meshes; ++mi) {
        const std::size_t at = mesh_at + 0x30 * static_cast<std::size_t>(mi);
        s32 default_bone{}, map_count{}, fs_count{}, vb_count{};
        u32 map_at{}, fs_offset{}, vb_offset{};
        if (!ReadLe(d, at + 0x10, default_bone) || !ReadLe(d, at + 0x14, map_count) ||
            !ReadLe(d, at + 0x1C, map_at) || !ReadLe(d, at + 0x20, fs_count) ||
            !ReadLe(d, at + 0x24, fs_offset) || !ReadLe(d, at + 0x28, vb_count) ||
            !ReadLe(d, at + 0x2C, vb_offset) || fs_count < 1 || vb_count < 1 || vb_count > 8 ||
            map_count < 0 || map_count > 1024) {
            continue;
        }
        const auto mapped = [&](s32 j) -> s16 {
            if (map_count > 0) {
                s32 b{};
                if (j < 0 || j >= map_count ||
                    !ReadLe(d, map_at + 4 * static_cast<std::size_t>(j), b)) {
                    return static_cast<s16>(std::clamp(default_bone, 0, static_cast<s32>(nb) - 1));
                }
                j = b;
            }
            return static_cast<s16>(j >= 0 && j < static_cast<s32>(nb)
                                        ? j
                                        : std::clamp(default_bone, 0, static_cast<s32>(nb) - 1));
        };
        // The mesh's vertices from its buffers (merged by vertex index).
        const std::size_t first = out.verts.size();
        u32 count = 0;
        std::vector<Vec3> pos;
        std::vector<std::array<float, 4>> weights, indices, normal;
        bool have_w = false, have_i = false, have_n = false, normal_bytes = false,
             index_bytes = false, weight_bytes = false;
        for (s32 v = 0; v < vb_count; ++v) {
            s32 vb{};
            if (!ReadLe(d, vb_offset + 4 * static_cast<std::size_t>(v), vb) || vb < 0 ||
                vb >= buffers) {
                continue;
            }
            const std::size_t vat = buffer_at + 0x20 * static_cast<std::size_t>(vb);
            s32 layout{}, stride{}, n{};
            u32 offset{};
            if (!ReadLe(d, vat + 0x04, layout) || !ReadLe(d, vat + 0x08, stride) ||
                !ReadLe(d, vat + 0x0C, n) || !ReadLe(d, vat + 0x1C, offset) || layout < 0 ||
                layout >= layouts || stride <= 0 || n <= 0 || n > 200000 ||
                static_cast<std::uint64_t>(data) + offset +
                        static_cast<std::uint64_t>(stride) * static_cast<std::uint64_t>(n) >
                    d.size()) {
                continue;
            }
            if (count == 0) {
                count = static_cast<u32>(n);
                pos.assign(count, {});
                weights.assign(count, {});
                indices.assign(count, {});
                normal.assign(count, {});
            }
            const std::size_t lat = layout_at + 0x10 * static_cast<std::size_t>(layout);
            s32 members{};
            u32 members_at{};
            if (!ReadLe(d, lat, members) || !ReadLe(d, lat + 0x0C, members_at)) {
                continue;
            }
            for (s32 k = 0; k < std::min(members, 32); ++k) {
                const std::size_t mat = members_at + 0x14 * static_cast<std::size_t>(k);
                s32 member_offset{}, index{};
                u32 type{}, semantic{};
                if (!ReadLe(d, mat + 0x04, member_offset) || !ReadLe(d, mat + 0x08, type) ||
                    !ReadLe(d, mat + 0x0C, semantic) || !ReadLe(d, mat + 0x10, index) ||
                    index != 0 || semantic > 3) {
                    continue;
                }
                for (u32 i = 0; i < std::min<u32>(count, static_cast<u32>(n)); ++i) {
                    const std::size_t src = data + offset + static_cast<std::size_t>(stride) * i +
                                            static_cast<std::size_t>(member_offset);
                    float f[4]{};
                    bool bytes = false;
                    if (!Detail::ReadComponents(d, src, type, f, bytes)) {
                        break;
                    }
                    switch (semantic) {
                    case 0:
                        pos[i] = {f[0], f[1], f[2]};
                        break;
                    case 1:
                        weights[i] = {f[0], f[1], f[2], f[3]};
                        have_w = true;
                        weight_bytes = bytes;
                        break;
                    case 2:
                        indices[i] = {f[0], f[1], f[2], f[3]};
                        have_i = true;
                        index_bytes = bytes;
                        break;
                    case 3:
                        normal[i] = {f[0], f[1], f[2], f[3]};
                        have_n = true;
                        normal_bytes = bytes;
                        break;
                    }
                }
            }
        }
        if (count == 0) {
            continue;
        }
        (void)index_bytes;
        (void)weight_bytes;
        bool skinned = false;
        if (have_w && have_i) {
            for (u32 i = 0; i < count && !skinned; ++i) {
                skinned = weights[i][0] > 0.0f;
            }
        }
        out.verts.resize(first + count);
        out.vbone.resize(first + count);
        for (u32 i = 0; i < count; ++i) {
            s16 bone;
            if (skinned) {
                int heaviest = 0;
                for (int c = 1; c < 4; ++c) {
                    if (weights[i][c] > weights[i][heaviest]) {
                        heaviest = c;
                    }
                }
                bone = mapped(static_cast<s32>(indices[i][heaviest]));
                out.verts[first + i] = pos[i];
            } else {
                bone = have_n && normal_bytes ? mapped(static_cast<s32>(normal[i][3]))
                                              : mapped(default_bone);
                out.verts[first + i] = Apply(out.bone_bind[static_cast<std::size_t>(bone)], pos[i]);
            }
            out.vbone[first + i] = bone;
        }
        // The first full-detail face set (no LOD or motion-blur variant).
        for (s32 f = 0; f < fs_count; ++f) {
            s32 fs{};
            if (!ReadLe(d, fs_offset + 4 * static_cast<std::size_t>(f), fs) || fs < 0 ||
                fs >= face_sets) {
                continue;
            }
            const std::size_t fat = face_at + 0x20 * static_cast<std::size_t>(fs);
            u32 flags{}, index_count{}, index_offset{};
            s32 index_size{};
            u8 strip{};
            if (!ReadLe(d, fat, flags) || !ReadLe(d, fat + 4, strip) ||
                !ReadLe(d, fat + 8, index_count) || !ReadLe(d, fat + 0x0C, index_offset) ||
                !ReadLe(d, fat + 0x18, index_size)) {
                break;
            }
            if (flags & 0xC3000000u) {
                continue;
            }
            const u32 size = index_size != 0 ? static_cast<u32>(index_size) : header_index_size;
            if ((size != 16 && size != 32) || index_count > 3'000'000) {
                break;
            }
            const std::size_t width = size / 8;
            if (static_cast<std::uint64_t>(data) + index_offset +
                    static_cast<std::uint64_t>(index_count) * width >
                d.size()) {
                break;
            }
            const u32 restart = size == 16 ? 0xFFFFu : 0xFFFFFFFFu;
            const auto index_at = [&](u32 k) {
                const std::size_t o = data + index_offset + static_cast<std::size_t>(k) * width;
                if (size == 16) {
                    std::uint16_t v{};
                    ReadLe(d, o, v);
                    return u32{v};
                }
                u32 v{};
                ReadLe(d, o, v);
                return v;
            };
            const auto push = [&](u32 a, u32 b, u32 c) {
                if (a < count && b < count && c < count && a != b && b != c && a != c) {
                    out.tris.push_back(static_cast<u32>(first) + a);
                    out.tris.push_back(static_cast<u32>(first) + b);
                    out.tris.push_back(static_cast<u32>(first) + c);
                }
            };
            if (strip != 0) {
                u32 w[3]{};
                u32 have = 0;
                bool flip = false;
                for (u32 k = 0; k < index_count; ++k) {
                    const u32 x = index_at(k);
                    if (x == restart) {
                        have = 0;
                        flip = false;
                        continue;
                    }
                    w[0] = w[1];
                    w[1] = w[2];
                    w[2] = x;
                    if (++have < 3) {
                        continue;
                    }
                    if (flip) {
                        push(w[1], w[0], w[2]);
                    } else {
                        push(w[0], w[1], w[2]);
                    }
                    flip = !flip;
                }
            } else {
                for (u32 k = 0; k + 2 < index_count; k += 3) {
                    push(index_at(k), index_at(k + 1), index_at(k + 2));
                }
            }
            break;
        }
    }
    if (out.verts.empty() || out.tris.empty()) {
        why = "FLVER with no triangles";
        return std::nullopt;
    }
    return out;
}

// One bone of the part's live skeleton: model-space position (m) and rotation.
struct LiveBone {
    std::string name;
    Vec3 t{};
    Quat q{};
};

// Each FLVER bone's transform as the live skeleton poses it (model space of the part): the live
// bone of its name, else its parent's with the bind offset between them, else the bind pose.
inline std::vector<Mat34> BonePoses(const Mesh& m, std::span<const LiveBone> live) {
    const std::size_t nb = m.bone_names.size();
    std::vector<Mat34> pose(nb);
    std::vector<int> state(nb, 0); // 0 to do, 1 waiting for its parent, 2 done
    const auto find = [&](const std::string& name) -> const LiveBone* {
        for (const LiveBone& b : live) {
            if (b.name == name) {
                return &b;
            }
        }
        return nullptr;
    };
    std::vector<s32> stack;
    for (std::size_t root = 0; root < nb; ++root) {
        if (state[root] == 2) {
            continue;
        }
        stack.assign(1, static_cast<s32>(root));
        while (!stack.empty()) {
            const std::size_t ub = static_cast<std::size_t>(stack.back());
            if (state[ub] == 2) {
                stack.pop_back();
                continue;
            }
            const s32 p = m.bone_parent[ub];
            const bool parent = p >= 0 && p < static_cast<s32>(nb);
            if (const LiveBone* l = find(m.bone_names[ub])) {
                pose[ub] = FromQuat(Body::Normalize(l->q), l->t);
            } else if (parent && state[static_cast<std::size_t>(p)] == 0) {
                state[ub] = 1;
                stack.push_back(p);
                continue;
            } else if (parent && state[static_cast<std::size_t>(p)] == 2) {
                const std::size_t up = static_cast<std::size_t>(p);
                pose[ub] = Mul(pose[up], Mul(Inverse(m.bone_bind[up]), m.bone_bind[ub]));
            } else {
                pose[ub] = m.bone_bind[ub]; // a root (or a cycle)
            }
            state[ub] = 2;
            stack.pop_back();
        }
    }
    return pose;
}

// The mesh's corners as the live skeleton poses them (model space of the part).
inline std::vector<Vec3> Pose(const Mesh& m, std::span<const LiveBone> live) {
    const std::vector<Mat34> pose = BonePoses(m, live);
    std::vector<Vec3> out(m.verts.size());
    for (std::size_t i = 0; i < m.verts.size(); ++i) {
        const s16 b = m.vbone[i];
        out[i] = b >= 0 && static_cast<std::size_t>(b) < pose.size()
                     ? Apply(Mul(pose[static_cast<std::size_t>(b)],
                                 Inverse(m.bone_bind[static_cast<std::size_t>(b)])),
                             m.verts[i])
                     : m.verts[i];
    }
    return out;
}

// The rigid transform laying `from` on `to` (least squares, Horn's unit quaternion: the largest
// eigenvector of his 4 x 4 matrix, by Jacobi rotations).
inline Mat34 RigidFit(std::span<const Vec3> from, std::span<const Vec3> to) {
    const std::size_t n = std::min(from.size(), to.size());
    if (n == 0) {
        return Identity();
    }
    double cf[3]{}, ct[3]{};
    for (std::size_t i = 0; i < n; ++i) {
        cf[0] += from[i].x;
        cf[1] += from[i].y;
        cf[2] += from[i].z;
        ct[0] += to[i].x;
        ct[1] += to[i].y;
        ct[2] += to[i].z;
    }
    for (int k = 0; k < 3; ++k) {
        cf[k] /= static_cast<double>(n);
        ct[k] /= static_cast<double>(n);
    }
    double S[3][3]{};
    for (std::size_t i = 0; i < n; ++i) {
        const double a[3] = {from[i].x - cf[0], from[i].y - cf[1], from[i].z - cf[2]};
        const double b[3] = {to[i].x - ct[0], to[i].y - ct[1], to[i].z - ct[2]};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                S[r][c] += a[r] * b[c];
            }
        }
    }
    double N[4][4] = {
        {S[0][0] + S[1][1] + S[2][2], S[1][2] - S[2][1], S[2][0] - S[0][2], S[0][1] - S[1][0]},
        {S[1][2] - S[2][1], S[0][0] - S[1][1] - S[2][2], S[0][1] + S[1][0], S[2][0] + S[0][2]},
        {S[2][0] - S[0][2], S[0][1] + S[1][0], -S[0][0] + S[1][1] - S[2][2], S[1][2] + S[2][1]},
        {S[0][1] - S[1][0], S[2][0] + S[0][2], S[1][2] + S[2][1], -S[0][0] - S[1][1] + S[2][2]}};
    double V[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    for (int sweep = 0; sweep < 32; ++sweep) {
        double off = 0.0;
        for (int p = 0; p < 4; ++p) {
            for (int q = p + 1; q < 4; ++q) {
                off += N[p][q] * N[p][q];
            }
        }
        if (off < 1e-24) {
            break;
        }
        for (int p = 0; p < 4; ++p) {
            for (int q = p + 1; q < 4; ++q) {
                if (std::abs(N[p][q]) < 1e-30) {
                    continue;
                }
                const double theta = (N[q][q] - N[p][p]) / (2.0 * N[p][q]);
                const double t =
                    (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double sn = t * c;
                for (int k = 0; k < 4; ++k) {
                    const double nkp = N[k][p], nkq = N[k][q];
                    N[k][p] = c * nkp - sn * nkq;
                    N[k][q] = sn * nkp + c * nkq;
                }
                for (int k = 0; k < 4; ++k) {
                    const double npk = N[p][k], nqk = N[q][k];
                    N[p][k] = c * npk - sn * nqk;
                    N[q][k] = sn * npk + c * nqk;
                }
                for (int k = 0; k < 4; ++k) {
                    const double vkp = V[k][p], vkq = V[k][q];
                    V[k][p] = c * vkp - sn * vkq;
                    V[k][q] = sn * vkp + c * vkq;
                }
            }
        }
    }
    int best = 0;
    for (int k = 1; k < 4; ++k) {
        if (N[k][k] > N[best][best]) {
            best = k;
        }
    }
    const Quat q{static_cast<float>(V[1][best]), static_cast<float>(V[2][best]),
                 static_cast<float>(V[3][best]), static_cast<float>(V[0][best])};
    Mat34 m = FromQuat(Body::Normalize(q), {});
    const Vec3 rc =
        Apply(m, {static_cast<float>(cf[0]), static_cast<float>(cf[1]), static_cast<float>(cf[2])});
    m[3] = static_cast<float>(ct[0]) - rc.x;
    m[7] = static_cast<float>(ct[1]) - rc.y;
    m[11] = static_cast<float>(ct[2]) - rc.z;
    return m;
}

// A dummy of the part where the game has it now (the grip's frame): its FLVER ReferenceID (the
// part's group taken off) and position.
struct Marker {
    s32 ref{0};
    Vec3 p{};
};

struct Posed {
    std::vector<Vec3> verts; // the grip's frame
    std::vector<Mat34> bone_to_grip; // corrected bone-local -> grip, for authored capsules
    float rms{-1.0f};        // the matched dummies' distance from the game's (m); -1: none
    int matched{0};
    int corrected{0}; // bones laid on their dummies instead of the skeleton's pose
};

// The mesh as the hand holds it: posed by the live skeleton, put on the grip (`to_grip`), then
// checked against the game's own dummies of the part: each marker is matched to the nearest of the
// model's dummies of its id where the pose puts them (within 0.5 m; a model may have more copies of
// an id than the game reported). A bone whose dummies match three or more markers (not on one line)
// and that a rigid fit lays clearly better on them (fitted, matched again, a few times; at least
// 2 mm better, within 5 mm) is moved by that fit, and so are the bones below it not corrected
// themselves: a part the skeleton does not pose as the game shows it (one with no readable pose:
// its bind pose). `rms` is the markers' distance from their dummies after; the caller refuses the
// fit when it is large.
inline Posed PoseChecked(const Mesh& m, std::span<const Body::C0000::Rig::FlverDummy> dummies,
                         std::span<const LiveBone> live, const Mat34& to_grip,
                         std::span<const Marker> markers) {
    Posed out{};
    const std::size_t nb = m.bone_names.size();
    const std::vector<Mat34> pose = BonePoses(m, live);
    std::vector<Mat34> skin(nb);
    for (std::size_t b = 0; b < nb; ++b) {
        skin[b] = Mul(to_grip, Mul(pose[b], Inverse(m.bone_bind[b])));
    }
    // The dummies in the bind pose (model space: given in their parent bone's bind frame).
    std::vector<Vec3> bind_at(dummies.size());
    for (std::size_t i = 0; i < dummies.size(); ++i) {
        const auto& d = dummies[i];
        const Vec3 local{d.pos[0], d.pos[1], d.pos[2]};
        bind_at[i] = d.parent >= 0 && static_cast<std::size_t>(d.parent) < nb
                         ? Apply(m.bone_bind[static_cast<std::size_t>(d.parent)], local)
                         : local;
    }
    const auto skin_of = [&](std::size_t i, const std::vector<Mat34>& sk) -> const Mat34& {
        const s16 a = dummies[i].attach;
        return a >= 0 && static_cast<std::size_t>(a) < nb ? sk[static_cast<std::size_t>(a)]
                                                          : to_grip;
    };
    // The model dummy of `ref` nearest `p` with the dummies placed by `place(i)` (-1: none
    // within 0.5 m).
    const auto nearest = [&](s32 ref, Vec3 p, const auto& place, float& dist) {
        int best = -1;
        dist = 0.5f;
        for (std::size_t i = 0; i < dummies.size(); ++i) {
            if (dummies[i].ref != ref) {
                continue;
            }
            const float d = Body::Length(place(i) - p);
            if (d < dist) {
                dist = d;
                best = static_cast<int>(i);
            }
        }
        return best;
    };
    const auto skinned = [&](std::size_t i) { return Apply(skin_of(i, skin), bind_at[i]); };
    // Per bone (the one each dummy moves with): the markers its dummies match, and where the
    // skeleton puts those dummies.
    std::vector<std::vector<std::size_t>> mine(nb);  // markers
    std::vector<std::vector<std::size_t>> which(nb); // their dummies
    for (std::size_t k = 0; k < markers.size(); ++k) {
        float d = 0.0f;
        const int i = nearest(markers[k].ref, markers[k].p, skinned, d);
        if (i < 0) {
            continue;
        }
        const s16 a = dummies[static_cast<std::size_t>(i)].attach;
        if (a >= 0 && static_cast<std::size_t>(a) < nb) {
            mine[static_cast<std::size_t>(a)].push_back(k);
            which[static_cast<std::size_t>(a)].push_back(static_cast<std::size_t>(i));
        }
    }
    std::vector<Mat34> fix(nb, Identity());
    std::vector<bool> fixed(nb, false);
    for (std::size_t b = 0; b < nb; ++b) {
        if (mine[b].size() < 3) {
            continue;
        }
        std::vector<Vec3> from, to;
        for (std::size_t j = 0; j < mine[b].size(); ++j) {
            from.push_back(skinned(which[b][j]));
            to.push_back(markers[mine[b][j]].p);
        }
        // Not on one line: some dummy off the line through the first and the last.
        float spread = 0.0f;
        const Vec3 axis = Body::Normalize(from.back() - from.front());
        for (const Vec3& v : from) {
            spread = std::max(spread, Body::Length(Body::Perp(v - from.front(), axis)));
        }
        if (spread < 0.01f) {
            continue;
        }
        float before = 0.0f;
        for (std::size_t j = 0; j < from.size(); ++j) {
            before += Body::LengthSq(from[j] - to[j]);
        }
        before = std::sqrt(before / static_cast<float>(from.size()));
        // Fitted, then each marker matched again to this bone's dummies where the fit puts them
        // (a repeated id paired with the wrong copy at first), a few times.
        Mat34 r = RigidFit(from, to);
        for (int pass = 0; pass < 4; ++pass) {
            bool changed = false;
            const auto moved = [&](std::size_t i) { return Apply(r, skinned(i)); };
            for (std::size_t j = 0; j < mine[b].size(); ++j) {
                float d = 0.0f;
                const int i = nearest(markers[mine[b][j]].ref, markers[mine[b][j]].p, moved, d);
                if (i >= 0 && static_cast<std::size_t>(i) != which[b][j] &&
                    dummies[static_cast<std::size_t>(i)].attach == static_cast<s16>(b)) {
                    which[b][j] = static_cast<std::size_t>(i);
                    from[j] = skinned(which[b][j]);
                    changed = true;
                }
            }
            if (!changed) {
                break;
            }
            r = RigidFit(from, to);
        }
        float after = 0.0f;
        for (std::size_t j = 0; j < from.size(); ++j) {
            after += Body::LengthSq(Apply(r, from[j]) - to[j]);
        }
        after = std::sqrt(after / static_cast<float>(from.size()));
        if (after + 0.002f < before && after < 0.005f) {
            fix[b] = r;
            fixed[b] = true;
            ++out.corrected;
        }
    }
    // A corrected bone moves the bones below it that are not corrected themselves.
    std::vector<Mat34> final_skin = skin;
    for (std::size_t b = 0; b < nb; ++b) {
        s32 at = static_cast<s32>(b);
        for (int hops = 0; at >= 0 && static_cast<std::size_t>(at) < nb && hops < 256; ++hops) {
            if (fixed[static_cast<std::size_t>(at)]) {
                final_skin[b] = Mul(fix[static_cast<std::size_t>(at)], skin[b]);
                break;
            }
            at = m.bone_parent[static_cast<std::size_t>(at)];
        }
    }
    const auto final_at = [&](std::size_t i) { return Apply(skin_of(i, final_skin), bind_at[i]); };
    float sq = 0.0f;
    for (const Marker& k : markers) {
        float d = 0.0f;
        if (nearest(k.ref, k.p, final_at, d) >= 0) {
            sq += d * d;
            ++out.matched;
        }
    }
    out.rms = out.matched > 0 ? std::sqrt(sq / static_cast<float>(out.matched)) : -1.0f;
    out.bone_to_grip.resize(nb);
    for (std::size_t b = 0; b < nb; ++b)
        out.bone_to_grip[b] = Mul(final_skin[b], m.bone_bind[b]);
    out.verts.resize(m.verts.size());
    for (std::size_t i = 0; i < m.verts.size(); ++i) {
        const s16 b = m.vbone[i];
        out.verts[i] = b >= 0 && static_cast<std::size_t>(b) < nb
                           ? Apply(final_skin[static_cast<std::size_t>(b)], m.verts[i])
                           : Apply(to_grip, m.verts[i]);
    }
    return out;
}

} // namespace VR::WeaponMesh
