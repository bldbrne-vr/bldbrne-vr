// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vr_overlay_meshes.h"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "video_core/renderer_vulkan/vr_stereo_warp_math.h"

namespace Vulkan::VrOverlay {

// ---- math ---------------------------------------------------------------------------------

Quat QuatMul(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Quat QuatFromAxisAngle(Vec3 axis, float radians) {
    const Vec3 n = Normalize(axis);
    const float s = std::sin(radians * 0.5f);
    return {n.x * s, n.y * s, n.z * s, std::cos(radians * 0.5f)};
}

Vec3 Rotate(const Quat& q, Vec3 v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = Cross(u, v) * 2.0f;
    return v + t * q.w + Cross(u, t);
}

float HeadYaw(const Quat& q) {
    const Vec3 f = Rotate(q, Vec3{0.0f, 0.0f, -1.0f});
    return std::atan2(-f.x, -f.z);
}

Mat4 Mat4::Translation(Vec3 t) {
    Mat4 r;
    r.m[12] = t.x;
    r.m[13] = t.y;
    r.m[14] = t.z;
    return r;
}

Mat4 Mat4::Scale(Vec3 s) {
    Mat4 r;
    r.m[0] = s.x;
    r.m[5] = s.y;
    r.m[10] = s.z;
    return r;
}

Mat4 Mat4::Rotation(const Quat& q) {
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    Mat4 r;
    r.m = {1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz),        2.0f * (xz - wy),        0.0f,
           2.0f * (xy - wz),        1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx),        0.0f,
           2.0f * (xz + wy),        2.0f * (yz - wx),        1.0f - 2.0f * (xx + yy), 0.0f,
           0.0f,                    0.0f,                    0.0f,                    1.0f};
    return r;
}

Mat4 Mat4::RotationAxis(Vec3 axis, float radians) {
    return Rotation(QuatFromAxisAngle(axis, radians));
}

Mat4 Mat4::RotationX(float a) {
    const float c = std::cos(a), s = std::sin(a);
    Mat4 r;
    r.m[5] = c;
    r.m[6] = s;
    r.m[9] = -s;
    r.m[10] = c;
    return r;
}

Mat4 Mat4::RotationY(float a) {
    const float c = std::cos(a), s = std::sin(a);
    Mat4 r;
    r.m[0] = c;
    r.m[2] = -s;
    r.m[8] = s;
    r.m[10] = c;
    return r;
}

Mat4 Mat4::RotationZ(float a) {
    const float c = std::cos(a), s = std::sin(a);
    Mat4 r;
    r.m[0] = c;
    r.m[1] = s;
    r.m[4] = -s;
    r.m[5] = c;
    return r;
}

Mat4 Mat4::FromBasis(Vec3 x, Vec3 y, Vec3 z, Vec3 origin) {
    Mat4 r;
    r.m = {x.x, x.y, x.z, 0.0f, y.x, y.y, y.z, 0.0f, z.x, z.y, z.z, 0.0f,
           origin.x, origin.y, origin.z, 1.0f};
    return r;
}

Mat4 Mat4::FromPose(const Pose& pose) {
    Mat4 r = Rotation(pose.orientation);
    r.m[12] = pose.position.x;
    r.m[13] = pose.position.y;
    r.m[14] = pose.position.z;
    return r;
}

Vec3 Mat4::TransformPoint(Vec3 p) const {
    return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
            m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
            m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
}

Vec3 Mat4::TransformVector(Vec3 v) const {
    return {m[0] * v.x + m[4] * v.y + m[8] * v.z, m[1] * v.x + m[5] * v.y + m[9] * v.z,
            m[2] * v.x + m[6] * v.y + m[10] * v.z};
}

Vec4 Mat4::operator*(const Vec4& v) const {
    return {m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12] * v.w,
            m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13] * v.w,
            m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] * v.w,
            m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15] * v.w};
}

Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (u32 col = 0; col < 4; ++col) {
        for (u32 row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (u32 k = 0; k < 4; ++k) {
                sum += a.m[k * 4 + row] * b.m[col * 4 + k];
            }
            r.m[col * 4 + row] = sum;
        }
    }
    return r;
}

Mat4 RigidInverse(const Mat4& a) {
    Mat4 r;
    for (u32 i = 0; i < 3; ++i) {
        for (u32 j = 0; j < 3; ++j) {
            r.m[j * 4 + i] = a.m[i * 4 + j];
        }
    }
    const Vec3 t{a.m[12], a.m[13], a.m[14]};
    const Vec3 inv_t = r.TransformVector(t);
    r.m[12] = -inv_t.x;
    r.m[13] = -inv_t.y;
    r.m[14] = -inv_t.z;
    return r;
}

Mat4 Inverse(const Mat4& a) {
    const auto& m = a.m;
    std::array<float, 16> inv{};
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
             m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
             m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
             m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
              m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
             m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
             m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
             m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
              m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
             m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
             m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
              m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
              m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
             m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
             m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
              m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
              m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::abs(det) < 1e-30f) {
        return Mat4{};
    }
    Mat4 r;
    for (u32 i = 0; i < 16; ++i) {
        r.m[i] = inv[i] / det;
    }
    return r;
}

Mat4 ProjectionReversedZ(float angle_left, float angle_right, float angle_up, float angle_down,
                         float near_m) {
    const float tl = std::tan(angle_left), tr = std::tan(angle_right);
    const float tu = std::tan(angle_up), td = std::tan(angle_down);
    const float w = tr - tl, h = tu - td;
    Mat4 p;
    p.m = {};
    p.At(0, 0) = 2.0f / w;
    p.At(0, 2) = (tr + tl) / w;
    p.At(1, 1) = -2.0f / h;
    p.At(1, 2) = -(tu + td) / h;
    p.At(2, 3) = near_m;
    p.At(3, 2) = -1.0f;
    return p;
}

// ---- generators ---------------------------------------------------------------------------

namespace {

constexpr float PoleRadius = 1e-7f;

void AddTriangle(MeshData& mesh, u32 a, u32 b, u32 c) {
    mesh.indices.push_back(a);
    mesh.indices.push_back(b);
    mesh.indices.push_back(c);
}

// Inverse transpose of the upper 3x3 (for normals).
std::array<Vec3, 3> NormalMatrix(const Mat4& t) {
    const Vec3 c0 = t.Column(0), c1 = t.Column(1), c2 = t.Column(2);
    // Rows of the inverse are cross products of the columns divided by the determinant; the
    // inverse transpose therefore has these as its columns.
    const Vec3 r0 = Cross(c1, c2), r1 = Cross(c2, c0), r2 = Cross(c0, c1);
    const float det = Dot(c0, r0);
    const float inv = std::abs(det) > 1e-30f ? 1.0f / det : 0.0f;
    return {r0 * inv, r1 * inv, r2 * inv};
}

Vec3 SuperellipsePoint(const LoftRing& ring, float t) {
    const float c = std::cos(t), s = std::sin(t);
    const float k = 2.0f / ring.exponent;
    const float cu = std::copysign(std::pow(std::abs(c), k), c);
    const float cv = std::copysign(std::pow(std::abs(s), k), s);
    return ring.center + ring.axis_u * (ring.a * cu) + ring.axis_v * (ring.b * cv);
}

void FlipWinding(MeshData& mesh) {
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        std::swap(mesh.indices[i + 1], mesh.indices[i + 2]);
    }
}

// Area-weighted smooth normals over shared vertices.
void ComputeSmoothNormals(MeshData& mesh) {
    for (auto& v : mesh.vertices) {
        v.normal = {};
    }
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        Vertex& a = mesh.vertices[mesh.indices[i]];
        Vertex& b = mesh.vertices[mesh.indices[i + 1]];
        Vertex& c = mesh.vertices[mesh.indices[i + 2]];
        const Vec3 n = Cross(b.position - a.position, c.position - a.position);
        a.normal = a.normal + n;
        b.normal = b.normal + n;
        c.normal = c.normal + n;
    }
    for (auto& v : mesh.vertices) {
        v.normal = Normalize(v.normal);
    }
}

// Quarter-circle arc samples (explicit normals) for lathe profiles: center (cr, cy), radius,
// angles in degrees measured from +r toward +y.
void Arc(std::vector<ProfilePoint>& out, float cr, float cy, float radius, float from_deg,
         float to_deg, u32 steps, bool skip_first = false) {
    for (u32 i = skip_first ? 1 : 0; i <= steps; ++i) {
        const float a = (from_deg + (to_deg - from_deg) * static_cast<float>(i) / steps) * DegToRad;
        const float c = std::cos(a), s = std::sin(a);
        float r = cr + radius * c;
        if (std::abs(r) < 1e-7f) {
            r = 0.0f;
        }
        out.push_back({r, cy + radius * s, false, c, s});
    }
}

} // namespace

MeshData Lathe(std::span<const ProfilePoint> profile, u32 segments, bool closed_loop) {
    MeshData mesh;
    const size_t n = profile.size();
    if (n < 2 || segments < 3) {
        return mesh;
    }
    const size_t seg_count = closed_loop ? n : n - 1;
    std::vector<std::array<float, 2>> seg_normal(seg_count);
    std::vector<float> arc(n + 1, 0.0f);
    for (size_t i = 0; i < seg_count; ++i) {
        const ProfilePoint& a = profile[i];
        const ProfilePoint& b = profile[(i + 1) % n];
        const float dr = b.r - a.r, dy = b.y - a.y;
        const float len = std::max(std::sqrt(dr * dr + dy * dy), 1e-12f);
        seg_normal[i] = {dy / len, -dr / len};
        arc[i + 1] = arc[i] + len;
    }
    const float total = std::max(arc[seg_count], 1e-12f);

    struct Ring {
        u32 first{};
        bool pole{false};
    };
    std::vector<Ring> ring_in(n), ring_out(n);
    const auto add_ring = [&](size_t i, float nr, float ny) {
        const ProfilePoint& p = profile[i];
        const float l = std::max(std::sqrt(nr * nr + ny * ny), 1e-12f);
        nr /= l;
        ny /= l;
        Ring ring{static_cast<u32>(mesh.vertices.size()), !closed_loop && p.r <= PoleRadius};
        const float v = arc[i] / total;
        for (u32 j = 0; j < segments; ++j) {
            // A pole's vertex j serves the triangle between angles j and j + 1.
            const float u = (static_cast<float>(j) + (ring.pole ? 0.5f : 0.0f)) / segments;
            const float phi = u * 2.0f * Pi;
            const float s = std::sin(phi), c = std::cos(phi);
            const float r = ring.pole ? 0.0f : p.r;
            mesh.vertices.push_back({{r * s, p.y, r * c}, {nr * s, ny, nr * c}, {u, v}});
        }
        return ring;
    };

    for (size_t i = 0; i < n; ++i) {
        const ProfilePoint& p = profile[i];
        const bool has_prev = closed_loop || i > 0;
        const bool has_next = closed_loop || i + 1 < n;
        const auto& prev_n = has_prev ? seg_normal[(i + seg_count - 1) % seg_count]
                                      : seg_normal[i % seg_count];
        const auto& next_n = has_next ? seg_normal[i % seg_count] : prev_n;
        if (p.nr != 0.0f || p.ny != 0.0f) {
            ring_in[i] = ring_out[i] = add_ring(i, p.nr, p.ny);
        } else if (p.crease && has_prev && has_next) {
            ring_in[i] = add_ring(i, prev_n[0], prev_n[1]);
            ring_out[i] = add_ring(i, next_n[0], next_n[1]);
        } else {
            ring_in[i] = ring_out[i] =
                add_ring(i, prev_n[0] + next_n[0], prev_n[1] + next_n[1]);
        }
    }

    for (size_t s = 0; s < seg_count; ++s) {
        const Ring a = ring_out[s];
        const Ring b = ring_in[(s + 1) % n];
        for (u32 j = 0; j < segments; ++j) {
            const u32 j1 = (j + 1) % segments;
            if (a.pole && b.pole) {
                continue;
            }
            if (a.pole) {
                AddTriangle(mesh, a.first + j, b.first + j1, b.first + j);
            } else if (b.pole) {
                AddTriangle(mesh, a.first + j, a.first + j1, b.first + j);
            } else {
                AddTriangle(mesh, a.first + j, a.first + j1, b.first + j1);
                AddTriangle(mesh, a.first + j, b.first + j1, b.first + j);
            }
        }
    }
    mesh.closed = closed_loop || (profile.front().r <= PoleRadius && profile.back().r <= PoleRadius);
    return mesh;
}

MeshData Loft(std::span<const LoftRing> rings, u32 segments, Vec3 start_pole, Vec3 end_pole) {
    MeshData mesh;
    const u32 count = static_cast<u32>(rings.size());
    if (count < 1 || segments < 3) {
        return mesh;
    }
    for (u32 r = 0; r < count; ++r) {
        const float v = static_cast<float>(r + 1) / static_cast<float>(count + 1);
        for (u32 k = 0; k < segments; ++k) {
            const float t = 2.0f * Pi * static_cast<float>(k) / segments;
            mesh.vertices.push_back({SuperellipsePoint(rings[r], t), {}, {0.0f, v}});
        }
    }
    const u32 pole0 = static_cast<u32>(mesh.vertices.size());
    mesh.vertices.push_back({start_pole, {}, {0.0f, 0.0f}});
    const u32 pole1 = pole0 + 1;
    mesh.vertices.push_back({end_pole, {}, {0.0f, 1.0f}});
    for (u32 r = 0; r + 1 < count; ++r) {
        for (u32 k = 0; k < segments; ++k) {
            const u32 k1 = (k + 1) % segments;
            const u32 a = r * segments + k, b = r * segments + k1;
            const u32 c = (r + 1) * segments + k1, d = (r + 1) * segments + k;
            AddTriangle(mesh, a, b, c);
            AddTriangle(mesh, a, c, d);
        }
    }
    const u32 last = (count - 1) * segments;
    for (u32 k = 0; k < segments; ++k) {
        const u32 k1 = (k + 1) % segments;
        AddTriangle(mesh, pole0, k1, k);
        AddTriangle(mesh, last + k, last + k1, pole1);
    }
    if (SignedVolume(mesh) < 0.0) {
        FlipWinding(mesh);
    }
    ComputeSmoothNormals(mesh);
    mesh.closed = true;
    return mesh;
}

MeshData TaperedCapsule(float length, float r0, float r1, u32 segments, u32 arc_steps) {
    const float sin_a = std::clamp((r0 - r1) / std::max(length, 1e-6f), -0.95f, 0.95f);
    const float alpha = std::asin(sin_a) / DegToRad;
    const u32 n0 = std::max<u32>(2, static_cast<u32>(std::lround(arc_steps * (alpha + 90.0f) / 90.0f)));
    const u32 n1 = std::max<u32>(2, static_cast<u32>(std::lround(arc_steps * (90.0f - alpha) / 90.0f)));
    std::vector<ProfilePoint> profile;
    Arc(profile, 0.0f, 0.0f, r0, -90.0f, alpha, n0);
    Arc(profile, 0.0f, length, r1, alpha, 90.0f, n1);
    MeshData mesh = Lathe(profile, segments);
    TransformMesh(mesh, Mat4::RotationX(-90.0f * DegToRad)); // +Y -> -Z
    return mesh;
}

MeshData Sphere(float radius, u32 segments, u32 rings) {
    std::vector<ProfilePoint> profile;
    Arc(profile, 0.0f, 0.0f, radius, -90.0f, 90.0f, std::max<u32>(rings, 2));
    return Lathe(profile, segments);
}

MeshData RoundedBox(Vec3 half_extents, float radius, u32 steps) {
    MeshData mesh;
    const float h[3] = {half_extents.x, half_extents.y, half_extents.z};
    radius = std::min({radius, h[0], h[1], h[2]});
    radius = std::max(radius, 1e-5f);
    steps = std::max<u32>(steps, 1);
    // Per-axis sample coordinates: the rounded band at each end and one flat span.
    const auto coords = [&](u32 axis) {
        std::vector<float> c;
        const float e = h[axis], inner = e - radius;
        for (u32 i = 0; i <= steps; ++i) {
            c.push_back(-e + radius * static_cast<float>(i) / steps);
        }
        if (inner > 1e-7f) {
            for (u32 i = 0; i <= steps; ++i) {
                c.push_back(inner + radius * static_cast<float>(i) / steps);
            }
        } else {
            for (u32 i = 1; i <= steps; ++i) {
                c.push_back(-e + radius * static_cast<float>(steps + i) / steps);
            }
        }
        return c;
    };
    const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    struct Face {
        u32 n, u, v;
        float sign;
    };
    // u x v = outward normal.
    const Face faces[6] = {{0, 1, 2, 1.0f}, {0, 2, 1, -1.0f}, {1, 2, 0, 1.0f},
                           {1, 0, 2, -1.0f}, {2, 0, 1, 1.0f}, {2, 1, 0, -1.0f}};
    const Vec3 inner{h[0] - radius, h[1] - radius, h[2] - radius};
    for (const Face& f : faces) {
        const std::vector<float> cu = coords(f.u), cv = coords(f.v);
        const u32 base = static_cast<u32>(mesh.vertices.size());
        for (size_t j = 0; j < cv.size(); ++j) {
            for (size_t i = 0; i < cu.size(); ++i) {
                const Vec3 p = axes[f.n] * (f.sign * h[f.n]) + axes[f.u] * cu[i] + axes[f.v] * cv[j];
                const Vec3 c{std::clamp(p.x, -inner.x, inner.x), std::clamp(p.y, -inner.y, inner.y),
                             std::clamp(p.z, -inner.z, inner.z)};
                const Vec3 dir = Normalize(p - c);
                mesh.vertices.push_back({c + dir * radius, dir, {0.0f, 0.0f}});
            }
        }
        const u32 nu = static_cast<u32>(cu.size());
        for (u32 j = 0; j + 1 < cv.size(); ++j) {
            for (u32 i = 0; i + 1 < nu; ++i) {
                const u32 a = base + j * nu + i;
                AddTriangle(mesh, a, a + 1, a + nu + 1);
                AddTriangle(mesh, a, a + nu + 1, a + nu);
            }
        }
    }
    mesh.closed = true;
    return mesh;
}

MeshData Puck(float radius, float height, float bevel, u32 segments) {
    bevel = std::clamp(bevel, 1e-5f, std::min(radius, height) * 0.999f);
    std::vector<ProfilePoint> profile;
    profile.push_back({0.0f, 0.0f});
    profile.push_back({radius, 0.0f, true});
    Arc(profile, radius - bevel, height - bevel, bevel, 0.0f, 90.0f, 3);
    profile.push_back({0.0f, height});
    return Lathe(profile, segments);
}

void TransformMesh(MeshData& mesh, const Mat4& transform) {
    const auto nm = NormalMatrix(transform);
    for (auto& v : mesh.vertices) {
        v.position = transform.TransformPoint(v.position);
        v.normal = Normalize(nm[0] * v.normal.x + nm[1] * v.normal.y + nm[2] * v.normal.z);
    }
}

void AppendMesh(MeshData& dst, const MeshData& src) {
    const u32 base = static_cast<u32>(dst.vertices.size());
    dst.vertices.insert(dst.vertices.end(), src.vertices.begin(), src.vertices.end());
    for (const u32 i : src.indices) {
        dst.indices.push_back(base + i);
    }
    dst.closed = dst.closed && src.closed;
}

double SignedVolume(const MeshData& mesh) {
    double volume = 0.0;
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const Vec3 a = mesh.vertices[mesh.indices[i]].position;
        const Vec3 b = mesh.vertices[mesh.indices[i + 1]].position;
        const Vec3 c = mesh.vertices[mesh.indices[i + 2]].position;
        volume += static_cast<double>(Dot(a, Cross(b, c)));
    }
    return volume / 6.0;
}

// ---- part definitions ---------------------------------------------------------------------

namespace {

// Ghost hand dimensions (right hand, grip frame).
struct FingerDef {
    MeshId mesh[3];
    float length[3];
    float radius[3];
    float z;
};
constexpr std::array<FingerDef, 4> Fingers{{
    {{MeshId::HandIndex0, MeshId::HandIndex1, MeshId::HandIndex2},
     {0.044f, 0.026f, 0.021f},
     {0.0095f, 0.0085f, 0.0078f},
     -0.031f},
    {{MeshId::HandMiddle0, MeshId::HandMiddle1, MeshId::HandMiddle2},
     {0.048f, 0.029f, 0.023f},
     {0.0098f, 0.0088f, 0.0080f},
     -0.010f},
    {{MeshId::HandRing0, MeshId::HandRing1, MeshId::HandRing2},
     {0.045f, 0.028f, 0.022f},
     {0.0092f, 0.0083f, 0.0075f},
     0.010f},
    {{MeshId::HandPinky0, MeshId::HandPinky1, MeshId::HandPinky2},
     {0.036f, 0.021f, 0.019f},
     {0.0082f, 0.0074f, 0.0068f},
     0.028f},
}};
constexpr std::array<MeshId, 3> ThumbMeshes{MeshId::HandThumb0, MeshId::HandThumb1,
                                            MeshId::HandThumb2};
constexpr std::array<float, 3> ThumbLength{0.040f, 0.031f, 0.026f};
constexpr std::array<float, 3> ThumbRadius{0.0125f, 0.0100f, 0.0092f};
// Thumb bones (right hand; x mirrors for the left): resting over the faceplate.
constexpr Vec3 ThumbBase{0.026f, 0.012f, -0.030f};
constexpr std::array<Vec3, 3> ThumbDir{{
    {-0.35f, -0.55f, -0.76f},
    {-0.40f, -0.85f, -0.34f},
    {-0.45f, -0.88f, 0.15f},
}};
constexpr float ForearmLength = 0.065f;
constexpr Vec3 ForearmStart{0.032f, 0.056f, 0.0f};
constexpr Vec3 ForearmEnd{0.034f, 0.120f, 0.004f};

// Controller head (right hand, grip frame).
constexpr Vec3 HeadCenter{0.0f, -0.012f, -0.050f};
constexpr float HeadHalfF = 0.031f;
constexpr float HeadHalfX = 0.027f;
constexpr float HeadBottom = -0.014f;
constexpr float HeadTop = 0.006f;
constexpr float HeadEdge = 0.006f;
constexpr float HeadDome = 0.0015f;
inline Vec3 HeadNormal() {
    return Normalize(Vec3{0.0f, 0.34f, -0.94f});
}
inline Vec3 HeadForward() {
    return Normalize(Vec3{0.0f, -0.94f, -0.34f});
}

MeshData VialMesh(MeshId id) {
    switch (id) {
    case MeshId::VialNeedle: {
        const ProfilePoint p[] = {{0.0f, -0.0640f},
                                  {0.00045f, -0.0600f, true},
                                  {0.00045f, -0.0405f, true},
                                  {0.0f, -0.0405f}};
        return Lathe(p, 8);
    }
    case MeshId::VialHub: {
        const ProfilePoint p[] = {{0.0f, -0.0410f},          {0.0016f, -0.0410f, true},
                                  {0.0022f, -0.0395f},       {0.0030f, -0.0350f},
                                  {0.0042f, -0.0330f, true}, {0.0042f, -0.0315f, true},
                                  {0.0f, -0.0315f}};
        return Lathe(p, 16);
    }
    case MeshId::VialFerrule: {
        const ProfilePoint p[] = {{0.0f, -0.0320f},          {0.0070f, -0.0320f, true},
                                  {0.0088f, -0.0300f},       {0.0100f, -0.0275f, true},
                                  {0.0100f, -0.0215f, true}, {0.0094f, -0.0208f, true},
                                  {0.0f, -0.0208f}};
        return Lathe(p, 32);
    }
    case MeshId::VialGlass: {
        const ProfilePoint p[] = {
            {0.0f, -0.0245f},    {0.0080f, -0.0245f}, {0.0090f, -0.0238f}, {0.0095f, -0.0224f},
            {0.0096f, -0.0205f}, {0.0096f, 0.0240f},  {0.0094f, 0.0266f},  {0.0089f, 0.0290f},
            {0.0081f, 0.0310f},  {0.0075f, 0.0324f},  {0.0072f, 0.0336f},  {0.0072f, 0.0350f},
            {0.0080f, 0.0358f},  {0.0076f, 0.0366f},  {0.0f, 0.0370f}};
        return Lathe(p, 32);
    }
    case MeshId::VialBlood: {
        const ProfilePoint p[] = {
            {0.0f, -0.0228f},    {0.0076f, -0.0228f}, {0.0081f, -0.0222f}, {0.0083f, -0.0212f},
            {0.0083f, 0.0242f},  {0.0081f, 0.0266f},  {0.0076f, 0.0288f},  {0.0069f, 0.0306f},
            {0.0064f, 0.0320f},  {0.0062f, 0.0330f},  {0.0f, 0.0330f}};
        return Lathe(p, 24);
    }
    case MeshId::VialStopper: {
        const ProfilePoint p[] = {{0.0f, 0.0340f},     {0.0068f, 0.0340f, true},
                                  {0.0074f, 0.0346f},  {0.0078f, 0.0420f},
                                  {0.0072f, 0.0455f},  {0.0050f, 0.0470f},
                                  {0.0f, 0.0472f}};
        return Lathe(p, 24);
    }
    case MeshId::VialBand: {
        const ProfilePoint p[] = {{0.0074f, 0.0333f, true},
                                  {0.0089f, 0.0336f, true},
                                  {0.0089f, 0.0374f, true},
                                  {0.0074f, 0.0377f, true}};
        return Lathe(p, 32, true);
    }
    case MeshId::VialGlow: {
        std::vector<ProfilePoint> p;
        constexpr float R = 0.0136f;
        Arc(p, 0.0f, -0.0180f, R, -90.0f, 0.0f, 5);
        Arc(p, 0.0f, 0.0250f, R, 0.0f, 90.0f, 5);
        return Lathe(p, 24);
    }
    default:
        return {};
    }
}

MeshData ControllerMesh(MeshId id) {
    switch (id) {
    case MeshId::ControllerHandle: {
        // (z, half width x, half depth y, y center), superellipse cross-sections.
        constexpr float rings[][4] = {
            {-0.046f, 0.0150f, 0.0175f, -0.002f}, {-0.035f, 0.0172f, 0.0198f, -0.002f},
            {-0.010f, 0.0175f, 0.0205f, 0.000f},  {0.020f, 0.0168f, 0.0195f, 0.002f},
            {0.045f, 0.0155f, 0.0178f, 0.004f},   {0.053f, 0.0140f, 0.0162f, 0.005f},
            {0.058f, 0.0112f, 0.0130f, 0.005f},   {0.0612f, 0.0070f, 0.0080f, 0.005f},
        };
        std::vector<LoftRing> loft;
        for (const auto& r : rings) {
            loft.push_back({{0.0f, r[3], r[0]}, {1, 0, 0}, {0, 1, 0}, r[1], r[2], 2.6f});
        }
        return Loft(loft, 32, {0.0f, -0.002f, -0.050f}, {0.0f, 0.005f, 0.0630f});
    }
    case MeshId::ControllerHead: {
        const Vec3 n = HeadNormal(), f = HeadForward();
        std::vector<LoftRing> loft;
        const auto ring = [&](float t, float inset) {
            loft.push_back({HeadCenter + n * t, f, {1, 0, 0}, HeadHalfF - inset,
                            HeadHalfX - inset, 2.4f});
        };
        constexpr u32 Steps = 4;
        for (u32 i = 0; i <= Steps; ++i) { // bottom edge: -90 .. 0 degrees
            const float a = (-90.0f + 90.0f * i / Steps) * DegToRad;
            ring(HeadBottom + HeadEdge + HeadEdge * std::sin(a), HeadEdge * (1.0f - std::cos(a)));
        }
        for (u32 i = 0; i <= Steps; ++i) { // top edge: 0 .. 90 degrees
            const float a = (90.0f * i / Steps) * DegToRad;
            ring(HeadTop - HeadEdge + HeadEdge * std::sin(a), HeadEdge * (1.0f - std::cos(a)));
        }
        // A gentle dome over the faceplate.
        ring(HeadTop + HeadDome * 0.75f, HeadEdge + (HeadHalfX - HeadEdge) * 0.5f);
        return Loft(loft, 40, HeadCenter + n * HeadBottom, HeadCenter + n * (HeadTop + HeadDome));
    }
    case MeshId::ControllerStickWell:
        return Puck(0.0115f, 0.0012f, 0.0005f, 32);
    case MeshId::ControllerStickShaft: {
        const ProfilePoint p[] = {{0.0f, 0.0f}, {0.0040f, 0.0f, true}, {0.0045f, 0.0060f, true},
                                  {0.0f, 0.0060f}};
        return Lathe(p, 16);
    }
    case MeshId::ControllerStickCap: {
        std::vector<ProfilePoint> p;
        p.push_back({0.0f, 0.0f});
        p.push_back({0.0080f, 0.0f, true});
        Arc(p, 0.0075f, 0.0025f, 0.0015f, -30.0f, 90.0f, 4);
        p.push_back({0.0060f, 0.0039f});
        p.push_back({0.0035f, 0.0033f});
        p.push_back({0.0f, 0.0031f});
        return Lathe(p, 32);
    }
    case MeshId::ControllerButton:
        return Puck(0.0055f, 0.0030f, 0.0010f, 24);
    case MeshId::ControllerSystemButton:
        return Puck(0.0035f, 0.0014f, 0.0006f, 20);
    case MeshId::ControllerTrigger:
        return RoundedBox({0.0085f, 0.0045f, 0.0130f}, 0.0030f, 3);
    case MeshId::ControllerGripButton:
        return RoundedBox({0.0030f, 0.0070f, 0.0140f}, 0.0020f, 3);
    default:
        return {};
    }
}

MeshData HandMesh(MeshId id) {
    if (id == MeshId::HandPalm) {
        return RoundedBox({0.012f, 0.040f, 0.040f}, 0.011f, 4);
    }
    if (id == MeshId::HandForearm) {
        return TaperedCapsule(ForearmLength, 0.026f, 0.030f, 20, 5);
    }
    for (u32 k = 0; k < 3; ++k) {
        if (id == ThumbMeshes[k]) {
            const float r1 = k < 2 ? ThumbRadius[k + 1] : ThumbRadius[k] * 0.85f;
            return TaperedCapsule(ThumbLength[k], ThumbRadius[k], r1, 14, 4);
        }
    }
    for (const FingerDef& f : Fingers) {
        for (u32 k = 0; k < 3; ++k) {
            if (id == f.mesh[k]) {
                const float r1 = k < 2 ? f.radius[k + 1] : f.radius[k] * 0.85f;
                return TaperedCapsule(f.length[k], f.radius[k], r1, 14, 4);
            }
        }
    }
    return {};
}

} // namespace

const char* MeshName(MeshId id) {
    static constexpr std::array<const char*, MeshCount> names{
        "VialNeedle",       "VialHub",           "VialFerrule",
        "VialGlass",        "VialBlood",         "VialStopper",
        "VialBand",         "VialGlow",          "ControllerHandle",
        "ControllerHead",   "ControllerStickWell", "ControllerStickShaft",
        "ControllerStickCap", "ControllerButton", "ControllerSystemButton",
        "ControllerTrigger", "ControllerGripButton", "HandPalm",
        "HandForearm",      "HandThumb0",        "HandThumb1",
        "HandThumb2",       "HandIndex0",        "HandIndex1",
        "HandIndex2",       "HandMiddle0",       "HandMiddle1",
        "HandMiddle2",      "HandRing0",         "HandRing1",
        "HandRing2",        "HandPinky0",        "HandPinky1",
        "HandPinky2",       "LaserBeam",         "LaserDot",
        "AxisArrow",        "Arrow",             "Vignette",
    };
    const u32 i = static_cast<u32>(id);
    return i < MeshCount ? names[i] : "?";
}

MeshData BuildMesh(MeshId id) {
    const u32 i = static_cast<u32>(id);
    if (i <= static_cast<u32>(MeshId::VialGlow)) {
        return VialMesh(id);
    }
    if (i <= static_cast<u32>(MeshId::ControllerGripButton)) {
        return ControllerMesh(id);
    }
    if (i <= static_cast<u32>(MeshId::HandPinky2)) {
        return HandMesh(id);
    }
    switch (id) {
    case MeshId::LaserBeam: {
        const ProfilePoint p[] = {{1.0f, 0.0f}, {2.0f, 1.0f}};
        MeshData mesh = Lathe(p, 8);
        TransformMesh(mesh, Mat4::RotationX(-90.0f * DegToRad)); // +Y -> -Z
        mesh.closed = false;
        return mesh;
    }
    case MeshId::LaserDot:
        return Sphere(1.0f, 16, 8);
    case MeshId::AxisArrow: {
        const ProfilePoint p[] = {{0.0f, 0.0f},        {0.04f, 0.0f, true}, {0.04f, 0.8f, true},
                                  {0.10f, 0.8f, true}, {0.0f, 1.0f}};
        return Lathe(p, 12);
    }
    case MeshId::Arrow: {
        // At 78 cm: a 4 mm shaft, a 6 cm head 9 mm across at its base.
        const ProfilePoint p[] = {{0.0f, 0.0f},          {0.0051f, 0.0f, true},
                                  {0.0051f, 0.925f, true}, {0.0115f, 0.925f, true},
                                  {0.0f, 1.0f}};
        MeshData mesh = Lathe(p, 10);
        TransformMesh(mesh, Mat4::RotationX(-90.0f * DegToRad)); // +Y -> -Z
        return mesh;
    }
    case MeshId::Vignette: {
        // Rings every 3 degrees through the ramp, wider beyond it; the profile is a squared
        // smoothstep, so the glow stays at the edges.
        std::vector<float> angles;
        for (float a = VignetteInnerDeg; a < VignetteFullDeg + 4.0f; a += 3.0f) {
            angles.push_back(a);
        }
        for (const float a : {66.0f, 75.0f, VignetteOuterDeg}) {
            angles.push_back(a);
        }
        constexpr u32 Segments = 64;
        MeshData mesh;
        mesh.closed = false;
        for (const float a : angles) {
            const float t = std::clamp(
                (a - VignetteInnerDeg) / (VignetteFullDeg - VignetteInnerDeg), 0.0f, 1.0f);
            const float smooth = t * t * (3.0f - 2.0f * t);
            const float r = std::tan(a * DegToRad);
            for (u32 j = 0; j <= Segments; ++j) {
                const float phi = 2.0f * Pi * static_cast<float>(j) / Segments;
                mesh.vertices.push_back({{r * std::cos(phi), r * std::sin(phi), 0.0f},
                                         {0.0f, 0.0f, 1.0f},
                                         {a / 90.0f, smooth * smooth}});
            }
        }
        const u32 row = Segments + 1;
        for (u32 i = 0; i + 1 < angles.size(); ++i) {
            for (u32 j = 0; j < Segments; ++j) {
                const u32 a = i * row + j; // inner
                const u32 b = a + row;     // outer
                // Counter-clockwise seen from +Z.
                mesh.indices.insert(mesh.indices.end(), {a, b, b + 1, a, b + 1, a + 1});
            }
        }
        return mesh;
    }
    default:
        return {};
    }
}

MeshLibrary BuildLibrary() {
    MeshLibrary library;
    for (u32 i = 0; i < MeshCount; ++i) {
        const MeshData mesh = BuildMesh(static_cast<MeshId>(i));
        MeshRange& range = library.ranges[i];
        range.first_index = static_cast<u32>(library.indices.size());
        range.index_count = static_cast<u32>(mesh.indices.size());
        range.vertex_offset = static_cast<s32>(library.vertices.size());
        range.vertex_count = static_cast<u32>(mesh.vertices.size());
        library.closed[i] = mesh.closed;
        library.vertices.insert(library.vertices.end(), mesh.vertices.begin(),
                                mesh.vertices.end());
        library.indices.insert(library.indices.end(), mesh.indices.begin(), mesh.indices.end());
    }
    return library;
}

// ---- materials and draw items -------------------------------------------------------------

DrawConstants MakeDrawConstants(const DrawItem& item, u32 eye) {
    DrawConstants c;
    c.model = item.model.m;
    c.base = item.material.base;
    c.emissive = item.material.emissive;
    c.params = item.material.params;
    c.extra = {static_cast<float>(eye), static_cast<float>(static_cast<u32>(item.shade)),
               item.material.fade, item.material.aux_fade};
    return c;
}

Lighting DefaultLighting(float head_yaw, float exposure) {
    const Mat4 yaw = Mat4::RotationY(head_yaw);
    Lighting l;
    l.key_dir = Normalize(yaw.TransformVector({0.30f, 0.85f, 0.45f}));
    l.key_color = Vec3{0.80f, 0.84f, 1.00f} * 2.2f;
    l.fill_dir = Normalize(yaw.TransformVector({-0.50f, -0.20f, -0.85f}));
    l.fill_color = Vec3{1.00f, 0.62f, 0.35f} * 0.6f;
    l.sky = {0.045f, 0.050f, 0.065f};
    l.ground = {0.020f, 0.016f, 0.014f};
    l.rim = {0.35f, 0.38f, 0.48f};
    l.exposure = exposure;
    return l;
}

FrameConstants MakeFrameConstants(const std::array<Mat4, 2>& view_proj,
                                  const std::array<Vec3, 2>& eye_pos, const Lighting& l) {
    FrameConstants f{};
    for (u32 i = 0; i < 2; ++i) {
        f.view_proj[i] = view_proj[i].m;
        f.eye_pos[i] = ToVec4(eye_pos[i], 1.0f);
    }
    f.key_dir = ToVec4(l.key_dir, 0.0f);
    f.key_color = ToVec4(l.key_color, 0.0f);
    f.fill_dir = ToVec4(l.fill_dir, 0.0f);
    f.fill_color = ToVec4(l.fill_color, 0.0f);
    f.sky = ToVec4(l.sky, l.exposure);
    f.ground = ToVec4(l.ground, 0.0f);
    f.rim = ToVec4(l.rim, 0.0f);
    return f;
}

void SortDrawItems(std::vector<DrawItem>& items) {
    std::stable_sort(items.begin(), items.end(), [](const DrawItem& a, const DrawItem& b) {
        return static_cast<u32>(a.pass) < static_cast<u32>(b.pass);
    });
}

// ---- scene assembly -----------------------------------------------------------------------

namespace {

Material MakeLit(Vec3 base, float roughness, float metallic, float rim = 0.0f,
                 float rim_power = 3.0f, Vec3 emissive = {}) {
    Material m;
    m.base = ToVec4(base, 1.0f);
    m.emissive = ToVec4(emissive, rim);
    m.params = {roughness, metallic, rim_power, 0.0f};
    return m;
}

// An opaque part, or (alpha < 1) a depth prepass plus a blend of its front-most surface.
void PushSolid(std::vector<DrawItem>& out, MeshId mesh, const Mat4& model, Material material,
               float alpha) {
    if (alpha >= 0.999f) {
        out.push_back({mesh, Pass::Opaque, Shade::Lit, model, material});
        return;
    }
    material.fade = std::clamp(alpha, 0.0f, 1.0f);
    out.push_back({mesh, Pass::DepthOnly, Shade::Lit, model, material});
    out.push_back({mesh, Pass::TransEqual, Shade::Lit, model, material});
}

// Rotation taking -Z to `dir` (any roll), placed at `origin`.
Mat4 AlongDirection(Vec3 dir, Vec3 origin, Vec3 hint) {
    const Vec3 z = -Normalize(dir);
    Vec3 x = Cross(hint, z);
    if (Length(x) < 1e-4f) {
        x = Cross(Vec3{0.0f, 1.0f, 0.0f}, z);
        if (Length(x) < 1e-4f) {
            x = Cross(Vec3{1.0f, 0.0f, 0.0f}, z);
        }
    }
    x = Normalize(x);
    const Vec3 y = Cross(z, x);
    return Mat4::FromBasis(x, y, z, origin);
}

float EaseOutBack(float t) {
    constexpr float c1 = 1.70158f * 0.6f; // a gentle (about 6 %) overshoot
    constexpr float c3 = c1 + 1.0f;
    const float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

float Smooth(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

Mat4 VialWorldFromGrip(const Mat4& world_from_grip, const VialAttach& attach) {
    const Mat4 rotation = Mat4::RotationZ(attach.rotation_deg.z * DegToRad) *
                          Mat4::RotationY(attach.rotation_deg.y * DegToRad) *
                          Mat4::RotationX(attach.rotation_deg.x * DegToRad);
    return world_from_grip * Mat4::Translation(attach.offset_m) * rotation *
           Mat4::RotationX(-90.0f * DegToRad);
}

void AppendVial(std::vector<DrawItem>& out, const Mat4& world_from_vial, const VialVisual& vial) {
    const float alpha = std::clamp(vial.alpha, 0.0f, 1.0f);
    if (alpha <= 0.0f || vial.scale <= 0.0f) {
        return;
    }
    const Mat4 model = world_from_vial * Mat4::Translation(vial.shake) *
                       Mat4::Translation({0.0f, -vial.plunge, 0.0f}) * Mat4::Scale(vial.scale);
    const float glow = std::max(vial.glow, 0.0f);

    const Material steel = MakeLit({0.55f, 0.55f, 0.57f}, 0.25f, 1.0f);
    const Material brass = MakeLit({0.20f, 0.14f, 0.07f}, 0.40f, 1.0f, 0.10f);
    const Material iron = MakeLit({0.12f, 0.11f, 0.10f}, 0.45f, 1.0f, 0.15f);
    const Material leather = MakeLit({0.085f, 0.050f, 0.035f}, 0.70f, 0.0f, 0.20f);
    PushSolid(out, MeshId::VialNeedle, model, steel, alpha);
    PushSolid(out, MeshId::VialHub, model, brass, alpha);
    PushSolid(out, MeshId::VialFerrule, model, iron, alpha);
    PushSolid(out, MeshId::VialBand, model, iron, alpha);
    PushSolid(out, MeshId::VialStopper, model, leather, alpha);

    const float plane = VialLiquidPlane(model, vial.level);
    if (vial.level > VialLevelEmpty + 0.0005f) {
        Material blood;
        blood.base = {0.13f, 0.0035f, 0.007f, 0.96f};
        blood.emissive = {0.040f * glow, 0.0015f * glow, 0.0025f * glow, 0.0f};
        blood.params = {0.18f, 0.0f, 3.0f, plane};
        blood.fade = alpha;
        out.push_back({MeshId::VialBlood, Pass::Liquid, Shade::Liquid, model, blood});
    }

    Material glass;
    glass.base = {0.80f, 0.85f, 0.82f, 0.08f};
    glass.emissive = {0.0f, 0.0f, 0.0f, 0.6f};
    glass.params = {0.05f, 0.0f, 3.0f, 0.0f};
    glass.fade = alpha;
    out.push_back({MeshId::VialGlass, Pass::GlassBack, Shade::Glass, model, glass});
    out.push_back({MeshId::VialGlass, Pass::GlassFront, Shade::Glass, model, glass});

    if (glow > 0.0f && vial.level > VialLevelEmpty + 0.0005f) {
        Material halo;
        halo.base = {0.0f, 0.0f, 0.0f, 0.0f};
        halo.emissive = {0.12f * glow, 0.006f * glow, 0.009f * glow, 0.0f};
        halo.params = {0.0f, 0.0f, 1.5f, plane};
        halo.fade = alpha;
        out.push_back({MeshId::VialGlow, Pass::Additive, Shade::Glow, model, halo});
    }
}

float VialLiquidPlane(const Mat4& world_from_vial, float level) {
    constexpr float Bottom = VialLevelEmpty; // blood volume, vial-space y
    constexpr float Top = 0.0330f;
    const float fill = std::clamp(level, Bottom, Top) - Bottom;
    const float up = Normalize(world_from_vial.Column(1)).y; // vial +Y against gravity
    const float t = Smooth((up + 0.25f) / 0.5f);           // 0 inverted .. 1 upright
    return (Top - fill) + ((Bottom + fill) - (Top - fill)) * t;
}

void AppendVialHoldout(std::vector<DrawItem>& out, Hand hand, const Mat4& world_from_grip,
                       float scale) {
    std::vector<DrawItem> fist;
    AppendGhostHand(fist, hand, world_from_grip, {1.0f, 1.0f, 1.0f});
    for (DrawItem& item : fist) {
        // Fingers and thumb only (the palm and forearm sit behind the vial anyway).
        if (item.pass != Pass::DepthOnly || item.mesh == MeshId::HandPalm ||
            item.mesh == MeshId::HandForearm) {
            continue;
        }
        item.pass = Pass::Holdout;
        item.model = item.model * Mat4::Scale(scale);
        out.push_back(item);
    }
}

void AppendController(std::vector<DrawItem>& out, Hand hand, const Mat4& world_from_grip,
                      const ControllerVisual& c, bool buttons_inboard) {
    const float alpha = std::clamp(c.alpha, 0.0f, 1.0f);
    if (alpha <= 0.0f) {
        return;
    }
    const float s = hand == Hand::Right ? 1.0f : -1.0f;
    const float side = buttons_inboard ? -s : s; // +x of the face buttons
    const Vec3 body_color{0.040f, 0.041f, 0.046f};
    const Material body = MakeLit(body_color, 0.55f, 0.0f, 0.25f, 3.0f);
    const Material face = MakeLit(body_color, 0.35f, 0.0f, 0.25f, 3.0f);
    const Material well = MakeLit({0.020f, 0.020f, 0.022f}, 0.70f, 0.0f);
    const Material stick = MakeLit({0.024f, 0.024f, 0.027f}, 0.85f, 0.0f, 0.15f);
    const Material control = MakeLit({0.060f, 0.062f, 0.068f}, 0.45f, 0.0f, 0.15f);
    const Material button = MakeLit({0.075f, 0.078f, 0.085f}, 0.40f, 0.0f, 0.10f);

    PushSolid(out, MeshId::ControllerHandle, world_from_grip, body, alpha);
    PushSolid(out, MeshId::ControllerHead, world_from_grip, face, alpha);

    // Face controls in the head frame: +X right, +Y up the faceplate normal, origin on top.
    const Vec3 n = HeadNormal(), f = HeadForward(), x{1.0f, 0.0f, 0.0f};
    const Mat4 head_basis = Mat4::FromBasis(x, n, Cross(x, n), {});
    const auto on_face = [&](float df, float dx, float t) {
        return world_from_grip * Mat4::Translation(HeadCenter + f * df + x * dx + n * t) *
               head_basis;
    };
    constexpr float Surface = HeadTop - 0.0004f;

    // Thumbstick: well, shaft and cap tilted toward the stick vector (up to 18 degrees).
    const float stick_df = 0.007f, stick_dx = -side * 0.009f;
    PushSolid(out, MeshId::ControllerStickWell, on_face(stick_df, stick_dx, Surface + 0.0008f),
              well, alpha);
    const float sx = std::clamp(c.stick.x, -1.0f, 1.0f), sy = std::clamp(c.stick.y, -1.0f, 1.0f);
    const float tilt = std::min(std::sqrt(sx * sx + sy * sy), 1.0f) * 18.0f * DegToRad;
    Mat4 tilt_rot{};
    if (tilt > 1e-4f) {
        // In head-local coordinates (X right, Y = normal, Z = -forward): lean toward
        // (sx, 0, -sy).
        tilt_rot = Mat4::RotationAxis(Cross(Vec3{0.0f, 1.0f, 0.0f}, Normalize({sx, 0.0f, -sy})),
                                      tilt);
    }
    const float press = c.stick_click ? -0.0010f : 0.0f;
    const Mat4 stick_base = on_face(stick_df, stick_dx, Surface + 0.0012f + press) * tilt_rot;
    PushSolid(out, MeshId::ControllerStickShaft, stick_base, stick, alpha);
    PushSolid(out, MeshId::ControllerStickCap, stick_base * Mat4::Translation({0, 0.0052f, 0}),
              stick, alpha);

    // Face buttons (A / X lower, B / Y upper) and the system button.
    const Vec3 pressed_glow{0.06f, 0.08f, 0.12f};
    const auto face_button = [&](MeshId mesh, float df, float dx, bool pressed) {
        Material m = button;
        if (pressed) {
            m.emissive = ToVec4(pressed_glow, m.emissive.w);
        }
        PushSolid(out, mesh, on_face(df, dx, Surface + 0.0002f - (pressed ? 0.0012f : 0.0f)), m,
                  alpha);
    };
    face_button(MeshId::ControllerButton, -0.006f, side * 0.010f, c.button_lower);
    face_button(MeshId::ControllerButton, 0.009f, side * 0.012f, c.button_upper);
    face_button(MeshId::ControllerSystemButton, -0.016f, side * 0.005f, c.system);

    // Trigger: hinged under the front of the head, pulled back up to 15 degrees.
    const float trigger = std::clamp(c.trigger, 0.0f, 1.0f);
    const Mat4 trigger_model = world_from_grip * Mat4::Translation({0.0f, -0.034f, -0.040f}) *
                               Mat4::RotationX(-15.0f * DegToRad * trigger) *
                               Mat4::Translation({0.0f, -0.002f, 0.013f});
    PushSolid(out, MeshId::ControllerTrigger, trigger_model, control, alpha);

    // Grip button on the finger side of the handle, pressed in by the squeeze.
    const float squeeze = std::clamp(c.squeeze, 0.0f, 1.0f);
    const Mat4 grip_model =
        world_from_grip * Mat4::Translation({-s * (0.0170f - 0.0020f * squeeze), -0.004f, -0.010f});
    PushSolid(out, MeshId::ControllerGripButton, grip_model, control, alpha);
}

void AppendGhostHand(std::vector<DrawItem>& out, Hand hand, const Mat4& world_from_grip,
                     const GhostHandVisual& ghost) {
    const float alpha = std::clamp(ghost.alpha, 0.0f, 1.0f);
    if (alpha <= 0.0f) {
        return;
    }
    const float s = hand == Hand::Right ? 1.0f : -1.0f;
    Material m;
    m.base = {0.55f, 0.70f, 0.85f, alpha};
    m.emissive = {0.10f, 0.14f, 0.18f, 0.0f};
    m.params = {0.5f, 0.0f, 2.0f, 0.0f};
    std::vector<std::pair<MeshId, Mat4>> parts;
    parts.reserve(17);

    parts.emplace_back(MeshId::HandPalm,
                       world_from_grip * Mat4::Translation({s * 0.030f, 0.016f, -0.002f}));
    {
        const Vec3 a{s * ForearmStart.x, ForearmStart.y, ForearmStart.z};
        const Vec3 b{s * ForearmEnd.x, ForearmEnd.y, ForearmEnd.z};
        parts.emplace_back(MeshId::HandForearm,
                           world_from_grip * AlongDirection(b - a, a, {1.0f, 0.0f, 0.0f}));
    }
    // Fingers: each joint flexes about (0, 0, -s), starting from straight ahead (-Y).
    const Vec3 axis{0.0f, 0.0f, -s};
    for (u32 i = 0; i < Fingers.size(); ++i) {
        const FingerDef& f = Fingers[i];
        const float curl =
            i == 0 ? 0.30f + 0.70f * std::clamp(ghost.trigger, 0.0f, 1.0f)
                   : 0.45f + 0.55f * std::clamp(ghost.squeeze, 0.0f, 1.0f);
        const float flex[3] = {(25.0f + 55.0f * curl) * DegToRad, (35.0f + 65.0f * curl) * DegToRad,
                               (15.0f + 45.0f * curl) * DegToRad};
        Vec3 joint{s * 0.024f, -0.022f, f.z};
        float angle = 0.0f;
        for (u32 k = 0; k < 3; ++k) {
            angle += flex[k];
            const Vec3 dir = Rotate(QuatFromAxisAngle(axis, angle), Vec3{0.0f, -1.0f, 0.0f});
            parts.emplace_back(f.mesh[k], world_from_grip * AlongDirection(dir, joint, axis));
            joint = joint + dir * f.length[k];
        }
    }
    {
        Vec3 joint{s * ThumbBase.x, ThumbBase.y, ThumbBase.z};
        for (u32 k = 0; k < 3; ++k) {
            const Vec3 dir = Normalize({s * ThumbDir[k].x, ThumbDir[k].y, ThumbDir[k].z});
            parts.emplace_back(ThumbMeshes[k],
                               world_from_grip * AlongDirection(dir, joint, {s, 0.0f, 0.0f}));
            joint = joint + dir * ThumbLength[k];
        }
    }
    for (const auto& [mesh, model] : parts) {
        Material pm = m;
        pm.aux_fade = mesh == MeshId::HandForearm ? 1.0f : 0.0f;
        out.push_back({mesh, Pass::DepthOnly, Shade::Ghost, model, pm});
        out.push_back({mesh, Pass::TransEqual, Shade::Ghost, model, pm});
    }
}

void AppendLaser(std::vector<DrawItem>& out, const Mat4& world_from_aim, const LaserVisual& l) {
    const float alpha = std::clamp(l.alpha, 0.0f, 1.0f);
    if (alpha <= 0.0f || l.length_m <= 0.0f) {
        return;
    }
    Material beam;
    beam.base = {1.0f, 0.82f, 0.55f, 0.55f};
    beam.emissive = {1.2f, 0.95f, 0.6f, 0.0f};
    beam.params = {0.0f, 0.0f, 2.0f, 0.0f};
    beam.fade = alpha;
    out.push_back({MeshId::LaserBeam, Pass::Additive, Shade::Beam,
                   world_from_aim * Mat4::Scale({l.radius_m, l.radius_m, l.length_m}), beam});
    if (l.hit_dot) {
        Material dot;
        dot.base = {0.0f, 0.0f, 0.0f, 0.0f};
        dot.emissive = {1.0f, 0.85f, 0.6f, 0.0f};
        dot.params = {0.0f, 0.0f, 1.0f, 0.0f};
        dot.fade = alpha;
        out.push_back({MeshId::LaserDot, Pass::Additive, Shade::Glow,
                       world_from_aim * Mat4::Translation({0.0f, 0.0f, -l.length_m}) *
                           Mat4::Scale(l.dot_radius_m),
                       dot});
    }
}

void AppendArrow(std::vector<DrawItem>& out, const Mat4& world_from_nock, float length_m,
                 float alpha) {
    if (!(alpha > 0.0f) || !(length_m > 0.0f)) {
        return;
    }
    PushSolid(out, MeshId::Arrow, world_from_nock * Mat4::Scale(length_m),
              MakeLit({0.30f, 0.24f, 0.18f}, 0.75f, 0.0f), std::min(alpha, 1.0f));
}

void AppendGripGuide(std::vector<DrawItem>& out, Vec3 centre, Vec3 axis, float alpha,
                     float half_length_m) {
    if (!(alpha > 0.0f) || !std::isfinite(alpha) || !std::isfinite(Length(centre)) ||
        !std::isfinite(Length(axis)) || !(Length(axis) > 1e-4f) ||
        !std::isfinite(half_length_m) || !(half_length_m > 0.0f)) {
        return;
    }
    const Vec3 z = Normalize(axis);
    const Vec3 up = std::abs(z.y) > 0.95f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
    const Vec3 x = Normalize(Cross(up, z));
    Material m;
    m.emissive = {0.35f, 0.65f, 0.85f, 0.0f};
    m.params = {0.0f, 0.0f, 2.0f, -1000.0f};
    m.fade = std::clamp(alpha, 0.0f, 1.0f);
    out.push_back({MeshId::LaserDot, Pass::Additive, Shade::Glow,
                   Mat4::FromBasis(x, Cross(z, x), z, centre) *
                       Mat4::Scale({0.035f, 0.035f, half_length_m}), m});
}

void AppendSpark(std::vector<DrawItem>& out, Vec3 head, Vec3 tail, float radius_m, Vec3 color,
                 float alpha) {
    const Vec3 back = tail - head;
    const float length = Length(back);
    if (!(alpha > 0.0f) || !(length > 1e-5f) || !(radius_m > 0.0f)) {
        return;
    }
    const Vec3 z = back * (-1.0f / length); // the beam's -Z: back along the streak
    const Vec3 up = std::abs(z.y) > 0.95f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
    const Vec3 x = Normalize(Cross(up, z));
    Material m;
    m.base = ToVec4(color, 0.6f);
    m.emissive = ToVec4(color * 1.5f, 0.0f);
    m.params = {0.0f, 0.0f, 2.0f, 0.0f};
    m.fade = std::min(alpha, 1.0f);
    out.push_back({MeshId::LaserBeam, Pass::Additive, Shade::Beam,
                   Mat4::FromBasis(x, Cross(z, x), z, head) *
                       Mat4::Scale({radius_m, radius_m, length}),
                   m});
}

void AppendVignette(std::vector<DrawItem>& out, const Pose& head, const VignetteVisual& v) {
    if (!(v.alpha > 0.0f)) {
        return;
    }
    Material m;
    m.base = ToVec4(v.color, std::min(v.alpha, 1.0f));
    m.params = {0.0f, 0.0f, std::clamp(v.profile_power, 0.25f, 8.0f), 0.0f}; // z: profile power
    if (v.inner_deg > 0.0f) {
        // x, y: the angles (degrees) of the shader's own profile; w: 1 a ramp, 2 a band.
        const float inner = std::max(v.inner_deg, VignetteInnerDeg);
        m.params = {inner, std::max(v.full_deg, inner + 1.0f), m.params.z, v.band ? 2.0f : 1.0f};
    }
    if (v.fit_inner > 0.0f) {
        // x, y: the fitted profile's fractions of the displayed field; w: 3 (FrameData field).
        m.params = {v.fit_inner, std::max(v.fit_full, v.fit_inner + 0.01f), m.params.z, 3.0f};
    }
    if (v.wedge_focus > 0.0f) {
        // The directional hit flash: the shader weights the ring toward (x, y) (emissive is
        // otherwise unused by the vignette).
        m.emissive = {v.wedge_x, v.wedge_y, std::clamp(v.wedge_power, 0.25f, 16.0f),
                      std::min(v.wedge_focus, 1.0f)};
    }
    const float d = std::max(v.distance_m, 0.1f);
    out.push_back({MeshId::Vignette, Pass::Screen, Shade::Vignette,
                   Mat4::FromPose(head) * Mat4::Translation({0.0f, 0.0f, -d}) * Mat4::Scale(d), m});
}

void AppendAxes(std::vector<DrawItem>& out, const Mat4& frame, float length_m) {
    const Mat4 scale = Mat4::Scale(length_m);
    const auto arrow = [&](const Mat4& rotation, Vec3 color) {
        Material m;
        m.base = ToVec4(color, 1.0f);
        out.push_back({MeshId::AxisArrow, Pass::Opaque, Shade::Unlit, frame * rotation * scale, m});
    };
    arrow(Mat4::RotationZ(-90.0f * DegToRad), {0.9f, 0.05f, 0.05f}); // +Y -> +X
    arrow(Mat4{}, {0.05f, 0.9f, 0.05f});
    arrow(Mat4::RotationX(90.0f * DegToRad), {0.1f, 0.2f, 1.0f}); // +Y -> +Z
}

// ---- stereo match with the game layer -------------------------------------------------------

float NearDisparityScale(float depth_m, float knee_m, float min_m) {
    if (!(depth_m > 0.0f) || !(knee_m > 0.0f) || !(min_m > 0.0f) || min_m >= knee_m) {
        return 1.0f;
    }
    // Disparity is proportional to 1 / depth: the warp's pixel units with K = 1 (per metre).
    VrStereo::DisparityParams p{};
    p.knee_px = 1.0f / knee_m;
    p.floor_px = 1.0f / min_m;
    return VrStereo::CompressNear(1.0f / depth_m, p) * depth_m;
}

// ---- vial animation -------------------------------------------------------------------------

bool VialAnimator::Update(float dt, bool armed, float remaining_s, u32 events, bool tracked,
                          VialVisual& out) {
    dt = std::clamp(dt, 0.0f, 0.1f);
    untracked = tracked ? 0.0f : untracked + dt;
    const float track_target = untracked > 0.25f ? 0.0f : 1.0f;
    track_fade += std::clamp(track_target - track_fade, -dt / 0.15f, dt / 0.15f);

    if (events & VialEventHandedToGame) {
        // The game shows its own vial (Triangle): hide at once.
        phase = Phase::Hidden;
        time = 0.0f;
        return false;
    }
    time += dt;
    const bool warning = armed && remaining_s >= 0.0f && remaining_s < 1.0f;
    pulse += dt * (warning ? 2.4f : 1.2f);
    pulse -= std::floor(pulse);

    const auto current_alpha = [&] {
        switch (phase) {
        case Phase::Appear:
            return std::clamp(time / AppearSeconds, 0.0f, 1.0f);
        case Phase::Leave: {
            const float u = std::clamp(time / LeaveSeconds, 0.0f, 1.0f);
            return leave_from * (1.0f - u * u);
        }
        case Phase::Hidden:
            return 0.0f;
        default:
            return 1.0f;
        }
    };
    const auto start_leave = [&] {
        leave_from = current_alpha();
        phase = Phase::Leave;
        time = dt;
    };

    switch (phase) {
    case Phase::Hidden:
        if (armed) {
            phase = Phase::Appear;
            time = dt;
            level = VialLevelFull;
            plunge = 0.0f;
        }
        break;
    case Phase::Appear:
    case Phase::Armed:
        if (!armed) {
            if (events & VialEventInjectSent) {
                phase = Phase::Inject;
                time = dt;
                level_from = level;
                healed = denied = false;
            } else {
                start_leave();
            }
        } else if (phase == Phase::Appear && time >= AppearSeconds) {
            phase = Phase::Armed;
            time = time - AppearSeconds;
        }
        break;
    case Phase::Leave:
        if (armed) {
            // Re-armed while leaving: grow back from the current opacity.
            const float a = current_alpha();
            phase = Phase::Appear;
            time = a * AppearSeconds;
            level = VialLevelFull;
            plunge = 0.0f;
        } else if (events & VialEventInjectSent) {
            // A vial armed and stabbed while the last one was still going: its injection.
            phase = Phase::Inject;
            time = dt;
            level = level_from = VialLevelFull;
            plunge = 0.0f;
            healed = denied = false;
        } else if (time >= LeaveSeconds) {
            phase = Phase::Hidden;
            time = 0.0f;
        }
        break;
    case Phase::Inject:
    case Phase::Denied:
        if (armed) {
            // The next vial armed (two in a row) before this one finished: it pops in at once.
            phase = Phase::Appear;
            time = dt;
            level = VialLevelFull;
            plunge = 0.0f;
        }
        break;
    }
    if (phase == Phase::Inject) {
        if (events & VialEventInjectHealed) {
            healed = true;
        }
        if (events & VialEventInjectDenied) {
            phase = Phase::Denied;
            time = dt;
            level_from = level;
        } else if ((healed && time >= 0.55f) || time >= 2.5f) {
            start_leave();
        }
    } else if (phase == Phase::Denied && time >= 0.25f) {
        start_leave();
    }

    VialVisual v{};
    const float wave = std::sin(2.0f * Pi * pulse);
    float glow = 1.0f + 0.5f * wave;
    if (warning) {
        glow *= 0.5f + 0.5f * std::max(remaining_s, 0.0f);
    }
    switch (phase) {
    case Phase::Hidden:
        return false;
    case Phase::Appear: {
        const float u = std::clamp(time / AppearSeconds, 0.0f, 1.0f);
        v.scale = 0.6f + 0.4f * EaseOutBack(u);
        v.alpha = u;
        break;
    }
    case Phase::Armed:
        break;
    case Phase::Leave: {
        const float u = std::clamp(time / LeaveSeconds, 0.0f, 1.0f);
        v.scale = 1.0f - 0.3f * u * u;
        v.alpha = leave_from * (1.0f - u * u);
        break;
    }
    case Phase::Inject:
        plunge = 0.012f * std::clamp(time / 0.06f, 0.0f, 1.0f);
        level = level_from + (VialLevelEmpty - level_from) * Smooth(time / 0.25f);
        glow = 1.0f + 2.5f * std::exp(-time / 0.15f);
        break;
    case Phase::Denied: {
        const float u = std::clamp(time / 0.10f, 0.0f, 1.0f);
        plunge = 0.012f * (1.0f - u);
        level = level_from + (VialLevelFull - level_from) * Smooth(u);
        if (time < 0.15f) {
            const float shake = 0.0015f * std::sin(2.0f * Pi * 30.0f * time);
            v.shake = {shake, 0.0f, 0.0f};
        }
        break;
    }
    }
    v.level = level;
    v.plunge = plunge;
    v.glow = glow;
    v.alpha *= track_fade;
    out = v;
    return v.alpha > 0.002f;
}

} // namespace Vulkan::VrOverlay
