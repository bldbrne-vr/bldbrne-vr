// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <new>
#include <zlib.h>

#include "core/vr/sever_model.h"

namespace VR::Sever {
namespace {
constexpr std::uint64_t MaxInflated = 256ull << 20;
constexpr std::uint32_t MaxModelBones = 2048, MaxMeshes = 64, MaxVertices = 65536,
                        MaxTriangles = 262144, MaxMembers = 32, MaxSets = 16, MaxBuffers = 8;

// A bounds-checked little-endian view: every read is checked against the data it views.
struct View {
    std::span<const std::uint8_t> data;
    bool Has(std::uint64_t at, std::uint64_t n) const {
        return at <= data.size() && n <= data.size() - at;
    }
    template <class T>
    bool Get(std::uint64_t at, T& v) const {
        if (!Has(at, sizeof(T))) {
            return false;
        }
        std::memcpy(&v, data.data() + at, sizeof(T));
        return true;
    }
};

std::uint32_t BigEndian(std::span<const std::uint8_t> d, std::size_t at) {
    return (std::uint32_t{d[at]} << 24) | (std::uint32_t{d[at + 1]} << 16) |
           (std::uint32_t{d[at + 2]} << 8) | std::uint32_t{d[at + 3]};
}

// DCX: "DCX\0", "DCS\0" at 0x18 (big-endian sizes: inflated, then compressed), "DCP\0" "DFLT",
// "DCA\0" and its 8-byte block, then the zlib stream.
ModelError Inflate(std::span<const std::uint8_t> file, std::vector<std::uint8_t>& out) {
    if (file.size() < 0x50 || std::memcmp(file.data(), "DCX\0", 4) != 0 ||
        std::memcmp(file.data() + 0x18, "DCS\0", 4) != 0) {
        return ModelError::NotDcx;
    }
    const std::span<const std::uint8_t> head = file.first(0x80 < file.size() ? 0x80 : file.size());
    const auto find = [&](const char* tag) -> std::size_t {
        for (std::size_t i = 0; i + 4 <= head.size(); ++i) {
            if (std::memcmp(head.data() + i, tag, 4) == 0) {
                return i;
            }
        }
        return head.size();
    };
    const std::size_t dflt = find("DFLT"), dca = find("DCA\0");
    if (dflt == head.size() || dca == head.size() || dca + 8 >= file.size()) {
        return ModelError::NotDcx;
    }
    const std::uint32_t inflated = BigEndian(file, 0x1C);
    if (inflated == 0 || inflated > MaxInflated) {
        return ModelError::TooBig;
    }
    out.assign(inflated, 0);
    z_stream z{};
    const int init = inflateInit(&z);
    if (init == Z_MEM_ERROR) {
        throw std::bad_alloc{}; // transient, like an allocation by the decoder's vectors
    }
    if (init != Z_OK) {
        return ModelError::Inflate;
    }
    z.next_in = const_cast<Bytef*>(file.data() + dca + 8);
    z.avail_in = static_cast<uInt>(file.size() - (dca + 8));
    z.next_out = out.data();
    z.avail_out = inflated;
    const int result = inflate(&z, Z_FINISH);
    const auto produced = z.total_out;
    inflateEnd(&z);
    if (result == Z_MEM_ERROR) {
        throw std::bad_alloc{};
    }
    if (result != Z_STREAM_END || produced != inflated) {
        out.clear();
        return ModelError::Inflate;
    }
    return ModelError::None;
}

// A name: UTF-16 (non-ASCII kept as '?') or bytes, NUL-terminated, at most 128 characters.
bool Name(const View& f, std::uint32_t at, bool unicode, std::string& out) {
    out.clear();
    for (std::size_t i = 0; i < 128; ++i) {
        if (unicode) {
            std::uint16_t c{};
            if (!f.Get(at + 2ull * i, c)) {
                return false;
            }
            if (c == 0) {
                return true;
            }
            out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        } else {
            std::uint8_t c{};
            if (!f.Get(at + i, c)) {
                return false;
            }
            if (c == 0) {
                return true;
            }
            out.push_back(static_cast<char>(c));
        }
    }
    return true; // (a longer name is cut at 128)
}

using Matrix = std::array<float, 16>;
Matrix Multiply(const Matrix& a, const Matrix& b) {
    Matrix m{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float s = 0;
            for (int k = 0; k < 4; ++k) {
                s += a[r * 4 + k] * b[k * 4 + c];
            }
            m[r * 4 + c] = s;
        }
    }
    return m;
}
// The game's local bone transform, row vectors: S * Rx * Rz * Ry * T.
Matrix Local(Vec3 t, Vec3 r, Vec3 s) {
    const float cx = std::cos(r.x), sx = std::sin(r.x), cy = std::cos(r.y), sy = std::sin(r.y),
                cz = std::cos(r.z), sz = std::sin(r.z);
    const Matrix S{s.x, 0, 0, 0, 0, s.y, 0, 0, 0, 0, s.z, 0, 0, 0, 0, 1};
    const Matrix Rx{1, 0, 0, 0, 0, cx, sx, 0, 0, -sx, cx, 0, 0, 0, 0, 1};
    const Matrix Ry{cy, 0, -sy, 0, 0, 1, 0, 0, sy, 0, cy, 0, 0, 0, 0, 1};
    const Matrix Rz{cz, sz, 0, 0, -sz, cz, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const Matrix T{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, t.x, t.y, t.z, 1};
    return Multiply(Multiply(Multiply(Multiply(S, Rx), Rz), Ry), T);
}

bool Finite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Layout types (SoulsFormats FLVER.LayoutType) and their sizes; semantics.
enum : std::uint32_t {
    Float2 = 0x01, Float3 = 0x02, Float4 = 0x03, Byte4A = 0x10, Byte4B = 0x11,
    Short2toFloat2 = 0x12, Byte4C = 0x13, UV = 0x15, UVPair = 0x16, ShortBoneIndices = 0x18,
    Short4toFloat4A = 0x1A, Short4toFloat4B = 0x2E, Byte4E = 0x2F,
};
enum : std::uint32_t { Position = 0, Weights = 1, Indices = 2, Normal = 3, Uv = 5 };
std::uint32_t TypeSize(std::uint32_t type) {
    switch (type) {
    case Float2:
    case UVPair:
    case ShortBoneIndices:
    case Short4toFloat4A:
    case Short4toFloat4B:
        return 8;
    case Float3:
        return 12;
    case Float4:
        return 16;
    case Byte4A:
    case Byte4B:
    case Short2toFloat2:
    case Byte4C:
    case UV:
    case Byte4E:
        return 4;
    default:
        return 0;
    }
}
bool Byte4(std::uint32_t type) {
    return type == Byte4A || type == Byte4B || type == Byte4C || type == Byte4E;
}

struct Member {
    std::uint32_t offset, type, semantic;
};

ModelError Mesh_(const View& f, std::uint32_t version, std::uint32_t data, std::uint64_t mesh_at,
                 std::uint64_t fs_at, std::uint32_t fs_size, std::uint32_t facesets,
                 std::uint64_t vb_at, std::uint32_t buffers, std::uint64_t lay_at,
                 std::uint32_t layouts, std::uint8_t header_index_size, std::uint32_t bones,
                 ModelMesh& out, std::uint32_t& material, std::int32_t& group) {
    std::int32_t mat{}, def{}, nfs{}, fs_off{}, nvb{}, vb_off{};
    if (!f.Get(mesh_at + 4, mat) || !f.Get(mesh_at + 0x10, def) || !f.Get(mesh_at + 0x20, nfs) ||
        !f.Get(mesh_at + 0x24, fs_off) || !f.Get(mesh_at + 0x28, nvb) ||
        !f.Get(mesh_at + 0x2C, vb_off) || nfs < 0 || nfs > std::int32_t(MaxSets) || nvb <= 0 ||
        nvb > std::int32_t(MaxBuffers) || fs_off < 0 || vb_off < 0) {
        return ModelError::Layout;
    }
    material = static_cast<std::uint32_t>(mat);
    group = def;
    // Its own bone list, when it has one: a vertex's byte-sized bone indices then point into it,
    // not into the skeleton (2026-11-12: 52 of the game's models have over 256 bones, up to
    // 1,490; c2000's 111 need no list).
    std::int32_t list_count{}, list_off{};
    if (!f.Get(mesh_at + 0x14, list_count) || !f.Get(mesh_at + 0x1C, list_off) || list_count < 0 ||
        list_count > 256 || (list_count > 0 && list_off < 0)) {
        return ModelError::Layout;
    }
    std::vector<std::uint16_t> list(static_cast<std::size_t>(list_count));
    for (std::int32_t k = 0; k < list_count; ++k) {
        std::int32_t b{};
        if (!f.Get(static_cast<std::uint64_t>(list_off) + 4ull * k, b) || b < 0 ||
            static_cast<std::uint32_t>(b) >= bones) {
            return ModelError::Layout;
        }
        list[static_cast<std::size_t>(k)] = static_cast<std::uint16_t>(b);
    }
    // Its vertex buffers: one vertex count, each buffer's members by its layout.
    std::uint32_t count = 0;
    std::array<std::uint8_t, 4> have{}; // position, normal, skin, uv
    std::vector<std::array<std::uint8_t, 4>> indices;
    std::vector<std::array<float, 4>> weights;
    for (std::int32_t b = 0; b < nvb; ++b) {
        std::int32_t vb{};
        if (!f.Get(static_cast<std::uint64_t>(vb_off) + 4ull * b, vb) || vb < 0 ||
            static_cast<std::uint32_t>(vb) >= buffers) {
            return ModelError::Layout;
        }
        const std::uint64_t v = vb_at + 0x20ull * vb;
        std::int32_t layout{}, size{}, n{}, offset{};
        if (!f.Get(v + 4, layout) || !f.Get(v + 8, size) || !f.Get(v + 0xC, n) ||
            !f.Get(v + 0x1C, offset) || layout < 0 || static_cast<std::uint32_t>(layout) >= layouts ||
            size <= 0 || size > 256 || n < 0 || offset < 0) {
            return ModelError::Layout;
        }
        if (static_cast<std::uint32_t>(n) > MaxVertices) {
            return ModelError::TooBig;
        }
        if (b == 0) {
            count = static_cast<std::uint32_t>(n);
            out.vertices.assign(count, Vertex{});
            indices.assign(count, {});
            weights.assign(count, {});
        } else if (static_cast<std::uint32_t>(n) != count) {
            return ModelError::Layout;
        }
        const std::uint64_t base = std::uint64_t{data} + static_cast<std::uint64_t>(offset);
        if (!f.Has(base, std::uint64_t(size) * count)) {
            return ModelError::Layout;
        }
        std::int32_t members{}, members_off{};
        const std::uint64_t l = lay_at + 0x10ull * layout;
        if (!f.Get(l, members) || !f.Get(l + 0xC, members_off) || members < 0 ||
            members > std::int32_t(MaxMembers) || members_off < 0) {
            return ModelError::Layout;
        }
        std::vector<Member> list;
        for (std::int32_t k = 0; k < members; ++k) {
            const std::uint64_t m = static_cast<std::uint64_t>(members_off) + 0x14ull * k;
            std::int32_t struct_off{}, index{};
            std::uint32_t type{}, semantic{};
            if (!f.Get(m + 4, struct_off) || !f.Get(m + 8, type) || !f.Get(m + 0xC, semantic) ||
                !f.Get(m + 0x10, index) || struct_off < 0) {
                return ModelError::Layout;
            }
            if (index != 0 || TypeSize(type) == 0) {
                continue; // a second UV set, a colour, a type not used here
            }
            if (static_cast<std::uint64_t>(struct_off) + TypeSize(type) > std::uint64_t(size)) {
                return ModelError::Layout;
            }
            list.push_back({static_cast<std::uint32_t>(struct_off), type, semantic});
        }
        const float uv_scale = version >= 0x2000F ? 1.0f / 2048 : 1.0f / 1024;
        for (const Member& m : list) {
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::uint8_t* p = f.data.data() + base + std::uint64_t(size) * i + m.offset;
                Vertex& vx = out.vertices[i];
                if (m.semantic == Position && (m.type == Float3 || m.type == Float4)) {
                    std::memcpy(&vx.position, p, 12);
                    have[0] = 1;
                } else if (m.semantic == Normal && m.type == Float3) {
                    std::memcpy(&vx.normal, p, 12);
                    have[1] = 1;
                } else if (m.semantic == Normal && Byte4(m.type)) {
                    vx.normal = {(p[0] - 127.0f) / 127.0f, (p[1] - 127.0f) / 127.0f,
                                 (p[2] - 127.0f) / 127.0f};
                    have[1] = 1;
                } else if (m.semantic == Indices && Byte4(m.type)) {
                    indices[i] = {p[0], p[1], p[2], p[3]};
                    have[2] |= 1;
                } else if (m.semantic == Indices && m.type == ShortBoneIndices) {
                    std::int16_t s[4];
                    std::memcpy(s, p, 8);
                    for (int k = 0; k < 4; ++k) {
                        if (s[k] < 0 || s[k] > 255) {
                            return ModelError::Layout;
                        }
                        indices[i][k] = static_cast<std::uint8_t>(s[k]);
                    }
                    have[2] |= 1;
                } else if (m.semantic == Weights && Byte4(m.type)) {
                    weights[i] = {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
                    have[2] |= 2;
                } else if (m.semantic == Weights && m.type == Short4toFloat4A) {
                    std::int16_t s[4];
                    std::memcpy(s, p, 8);
                    for (int k = 0; k < 4; ++k) {
                        weights[i][k] = std::max<std::int16_t>(s[k], 0) / 32767.0f;
                    }
                    have[2] |= 2;
                } else if (m.semantic == Uv && (m.type == UV || m.type == UVPair)) {
                    std::int16_t s[2];
                    std::memcpy(s, p, 4);
                    vx.uv = {s[0] * uv_scale, s[1] * uv_scale};
                    have[3] = 1;
                } else if (m.semantic == Uv && m.type == Float2) {
                    std::memcpy(vx.uv.data(), p, 8);
                    have[3] = 1;
                }
            }
        }
    }
    out.simulated = !have[0];
    // The skin: up to four influences, zero ones dropped, normalized to sum one (the bytes'
    // rounding); none at all: wholly its group bone (else bone 0).
    for (std::uint32_t i = 0; i < count; ++i) {
        Vertex& vx = out.vertices[i];
        float sum = 0;
        int n = 0;
        vx.skin = {};
        for (int k = 0; k < 4 && have[2] == 3; ++k) {
            if (weights[i][k] > 0) {
                const std::uint32_t local = indices[i][k];
                if (list.empty() ? local >= bones : local >= list.size()) {
                    return ModelError::Layout;
                }
                vx.skin[n++] = {list.empty() ? static_cast<std::uint16_t>(local) : list[local],
                                weights[i][k]};
                sum += weights[i][k];
            }
        }
        if (n == 0 || !(sum > 0)) {
            const auto bone = def >= 0 && static_cast<std::uint32_t>(def) < bones ? def : 0;
            vx.skin[0] = {static_cast<std::uint16_t>(bone), 1.0f};
        } else {
            for (int k = 0; k < n; ++k) {
                vx.skin[k].weight /= sum;
            }
        }
        const float len = std::sqrt(vx.normal.x * vx.normal.x + vx.normal.y * vx.normal.y +
                                    vx.normal.z * vx.normal.z);
        vx.normal = have[1] && std::isfinite(len) && len > 1e-6f
                        ? Vec3{vx.normal.x / len, vx.normal.y / len, vx.normal.z / len}
                        : Vec3{0, 1, 0};
        if (!Finite(vx.position) || !std::isfinite(vx.uv[0]) || !std::isfinite(vx.uv[1])) {
            return ModelError::Layout;
        }
    }
    // LOD0: the first face set with no LOD or motion-blur flag; a strip unrolled (its winding
    // alternating), degenerate triangles and restarts dropped.
    for (std::int32_t s = 0; s < nfs; ++s) {
        std::int32_t set{};
        if (!f.Get(static_cast<std::uint64_t>(fs_off) + 4ull * s, set) || set < 0 ||
            static_cast<std::uint32_t>(set) >= facesets) {
            return ModelError::Layout;
        }
        const std::uint64_t at = fs_at + std::uint64_t(fs_size) * set;
        std::uint32_t flags{};
        std::uint8_t strip{};
        std::int32_t n{}, off{}, isize{};
        if (!f.Get(at, flags) || !f.Get(at + 4, strip) || !f.Get(at + 8, n) ||
            !f.Get(at + 0xC, off) || n < 0 || off < 0) {
            return ModelError::Layout;
        }
        if (version >= 0x20009 && !f.Get(at + 0x18, isize)) {
            return ModelError::Layout;
        }
        if ((flags & 0xFF000000u) != 0) {
            continue;
        }
        const std::uint32_t width = isize == 32 ? 4 : isize == 16 ? 2
                                    : header_index_size == 32  ? 4
                                                               : 2;
        const std::uint64_t ib = std::uint64_t{data} + static_cast<std::uint64_t>(off);
        if (!f.Has(ib, std::uint64_t(width) * n)) {
            return ModelError::Layout;
        }
        const auto index = [&](std::int32_t k) -> std::uint32_t {
            const std::uint8_t* p = f.data.data() + ib + std::uint64_t(width) * k;
            if (width == 2) {
                std::uint16_t v;
                std::memcpy(&v, p, 2);
                return v == 0xFFFF ? 0xFFFFFFFFu : v;
            }
            std::uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        };
        const auto add = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
            // (Past the ceiling: refused below; never more held than that.)
            if (a >= count || b >= count || c >= count || a == b || b == c || a == c ||
                out.triangles.size() > MaxTriangles) {
                return;
            }
            // A triangle of no area (a strip's seam: two vertices at one position, split by
            // their UVs or normals) draws nothing: dropped, as the cutter refuses them.
            const Vec3 p = out.vertices[a].position, q = out.vertices[b].position,
                       r = out.vertices[c].position;
            const double ux = double(q.x) - p.x, uy = double(q.y) - p.y, uz = double(q.z) - p.z;
            const double vx = double(r.x) - p.x, vy = double(r.y) - p.y, vz = double(r.z) - p.z;
            const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
            if (!out.simulated && nx * nx + ny * ny + nz * nz <= 1e-20) {
                return;
            }
            out.triangles.push_back({{a, b, c}, material});
        };
        if (strip) {
            // Its winding alternates from each strip's first index (a restart begins a new one).
            std::int32_t first = 0;
            for (std::int32_t k = 0; k + 2 < n; ++k) {
                const std::uint32_t a = index(k), b = index(k + 1), c = index(k + 2);
                if (a == 0xFFFFFFFFu) {
                    first = k + 1;
                    continue;
                }
                if ((k - first) % 2 == 0) {
                    add(a, b, c);
                } else {
                    add(b, a, c);
                }
            }
        } else {
            for (std::int32_t k = 0; k + 2 < n; k += 3) {
                add(index(k), index(k + 1), index(k + 2));
            }
        }
        if (out.triangles.size() > MaxTriangles) {
            return ModelError::TooBig;
        }
        break;
    }
    return ModelError::None;
}

ModelError Flver(const View& f, Model& out) {
    std::uint32_t version{}, data{}, data_len{}, dummies{}, materials{}, bones{}, meshes{},
        buffers{}, facesets{}, layouts{}, textures{};
    std::uint8_t index_size{}, unicode{};
    if (!f.Get(0x08, version) || !f.Get(0x0C, data) || !f.Get(0x10, data_len) ||
        !f.Get(0x14, dummies) || !f.Get(0x18, materials) || !f.Get(0x1C, bones) ||
        !f.Get(0x20, meshes) || !f.Get(0x24, buffers) || !f.Get(0x48, index_size) ||
        !f.Get(0x49, unicode) || !f.Get(0x50, facesets) || !f.Get(0x54, layouts) ||
        !f.Get(0x58, textures) || version < 0x20000 || version > 0x2FFFF || !f.Has(data, data_len)) {
        return ModelError::Layout;
    }
    if (bones > MaxModelBones || meshes > MaxMeshes || materials > 4096 || dummies > 4096 ||
        buffers > 4096 || facesets > 4096 || layouts > 4096 || textures > 65536) {
        return ModelError::TooBig;
    }
    out.version = version;
    const std::uint64_t mat_at = 0x80 + 0x40ull * dummies;
    const std::uint64_t bone_at = mat_at + 0x20ull * materials;
    const std::uint64_t mesh_at = bone_at + 0x80ull * bones;
    const std::uint32_t fs_size = version >= 0x20005 ? 0x20 : 0x1C;
    const std::uint64_t fs_at = mesh_at + 0x30ull * meshes;
    const std::uint64_t vb_at = fs_at + std::uint64_t(fs_size) * facesets;
    const std::uint64_t lay_at = vb_at + 0x20ull * buffers;
    const std::uint64_t tex_at = lay_at + 0x10ull * layouts;
    if (!f.Has(lay_at, 0x10ull * layouts) || !f.Has(tex_at, 0x20ull * textures)) {
        return ModelError::Layout;
    }
    // The skeleton: names, parents, the bind pose composed down from each root.
    out.bones.resize(bones);
    std::vector<Matrix> local(bones);
    for (std::uint32_t b = 0; b < bones; ++b) {
        const std::uint64_t o = bone_at + 0x80ull * b;
        Vec3 t{}, r{}, s{};
        std::uint32_t name{};
        std::int16_t parent{};
        if (!f.Get(o, t) || !f.Get(o + 0xC, name) || !f.Get(o + 0x10, r) ||
            !f.Get(o + 0x1C, parent) || !f.Get(o + 0x20, s) ||
            !Name(f, name, unicode != 0, out.bones[b].name) || !Finite(t) || !Finite(r) ||
            !Finite(s) || parent < -1 || parent >= std::int32_t(bones)) {
            return ModelError::Layout;
        }
        out.bones[b].parent = parent;
        local[b] = Local(t, r, s);
    }
    std::vector<std::uint8_t> done(bones, 0);
    std::vector<std::uint32_t> chain(bones);
    for (std::uint32_t b = 0; b < bones; ++b) {
        // Up the chain to the first composed bone (a loop is a damaged file), then back down.
        std::uint32_t depth = 0, at = b;
        while (!done[at]) {
            if (depth == bones) {
                return ModelError::Layout;
            }
            chain[depth++] = at;
            const std::int32_t p = out.bones[at].parent;
            if (p < 0) {
                break;
            }
            at = static_cast<std::uint32_t>(p);
        }
        while (depth > 0) {
            const std::uint32_t c = chain[--depth];
            const std::int32_t p = out.bones[c].parent;
            out.bones[c].bind = p < 0 ? local[c] : Multiply(local[c], out.bones[p].bind);
            out.bones[c].joint = {out.bones[c].bind[12], out.bones[c].bind[13],
                                  out.bones[c].bind[14]};
            done[c] = 1;
        }
    }
    // Every mesh.
    out.meshes.resize(meshes);
    for (std::uint32_t m = 0; m < meshes; ++m) {
        std::uint32_t material = 0;
        std::int32_t group = -1;
        const ModelError e =
            Mesh_(f, version, data, mesh_at + 0x30ull * m, fs_at, fs_size, facesets, vb_at,
                  buffers, lay_at, layouts, index_size, bones, out.meshes[m], material, group);
        if (e != ModelError::None) {
            return e;
        }
        std::uint32_t name{};
        if (material >= materials || !f.Get(mat_at + 0x20ull * material, name) ||
            !Name(f, name, unicode != 0, out.meshes[m].material)) {
            return ModelError::Layout;
        }
        out.meshes[m].group =
            group >= 0 && static_cast<std::uint32_t>(group) < bones ? out.bones[group].name : "";
        // Its material's diffuse texture (a type with "Diffuse": "g_Diffuse"), by base name.
        std::int32_t tex_count{}, tex_first{};
        if (f.Get(mat_at + 0x20ull * material + 0x08, tex_count) &&
            f.Get(mat_at + 0x20ull * material + 0x0C, tex_first)) {
            const std::int64_t tex_end = std::int64_t{tex_first} + tex_count;
            for (std::int32_t t = tex_first;
                 t >= 0 && t < tex_end && static_cast<std::uint32_t>(t) < textures; ++t) {
                std::uint32_t path{}, type{};
                std::string type_name, path_name;
                if (!f.Get(tex_at + 0x20ull * static_cast<std::uint32_t>(t), path) ||
                    !f.Get(tex_at + 0x20ull * static_cast<std::uint32_t>(t) + 4, type) ||
                    !Name(f, type, unicode != 0, type_name) ||
                    type_name.find("Diffuse") == std::string::npos ||
                    !Name(f, path, unicode != 0, path_name)) {
                    continue;
                }
                if (const auto slash = path_name.find_last_of("\\/"); slash != std::string::npos) {
                    path_name = path_name.substr(slash + 1);
                }
                if (const auto dot = path_name.find_last_of('.'); dot != std::string::npos) {
                    path_name.resize(dot);
                }
                for (char& c : path_name) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                out.meshes[m].diffuse = std::move(path_name);
                break;
            }
        }
    }
    return ModelError::None;
}
} // namespace

const char* ModelErrorName(ModelError e) {
    switch (e) {
    case ModelError::None:
        return "read";
    case ModelError::Read:
        return "the file could not be read";
    case ModelError::NotDcx:
        return "not a DCX (DFLT) file";
    case ModelError::Inflate:
        return "its zlib stream is damaged";
    case ModelError::NoFlver:
        return "no FLVER2 model inside";
    case ModelError::Layout:
        return "a table, offset or vertex layout not understood";
    case ModelError::TooBig:
        return "over the reader's ceilings";
    }
    return "?";
}

// The bnd's texture file (BND4 entries {.., +0x08 u64 size, .., +0x18 u32 offset}, from 0x40, of
// the header's +0x20 size each; the first that is a TPF), copied; empty when it has none.
std::vector<std::uint8_t> TexturesOf(std::span<const std::uint8_t> bnd) {
    std::int32_t count{};
    std::uint64_t entry{};
    if (bnd.size() < 0x40 || std::memcmp(bnd.data(), "BND4", 4) != 0) {
        return {};
    }
    std::memcpy(&count, bnd.data() + 0x0C, sizeof(count));
    std::memcpy(&entry, bnd.data() + 0x20, sizeof(entry));
    if (count <= 0 || count > 4096 || entry < 0x1C || entry > 0x100) {
        return {};
    }
    for (std::int32_t k = 0; k < count; ++k) {
        const std::uint64_t e = 0x40 + entry * static_cast<std::uint64_t>(k);
        std::uint64_t size{};
        std::uint32_t offset{};
        if (e + 0x1C > bnd.size()) {
            return {};
        }
        std::memcpy(&size, bnd.data() + e + 0x08, sizeof(size));
        std::memcpy(&offset, bnd.data() + e + 0x18, sizeof(offset));
        if (size < 16 || offset > bnd.size() || size > bnd.size() - offset ||
            std::memcmp(bnd.data() + offset, "TPF\0", 4) != 0) {
            continue;
        }
        return {bnd.begin() + offset, bnd.begin() + offset + static_cast<std::ptrdiff_t>(size)};
    }
    return {};
}

ModelError InflateDcx(std::span<const std::uint8_t> file, std::vector<std::uint8_t>& out) {
    return Inflate(file, out);
}

ModelError LoadNpcDispMasks(std::span<const std::uint8_t> file,
                            std::unordered_map<std::int32_t, std::uint32_t>& out) {
    out.clear();
    std::vector<std::uint8_t> bnd;
    if (const ModelError e = Inflate(file, bnd); e != ModelError::None) {
        return e;
    }
    const auto get = [](std::span<const std::uint8_t> d, std::size_t at, auto& v) {
        if (at > d.size() || d.size() - at < sizeof(v)) {
            return false;
        }
        std::memcpy(&v, d.data() + at, sizeof(v));
        return true;
    };
    std::int32_t count{};
    std::int64_t header{};
    if (bnd.size() < 0x40 || std::memcmp(bnd.data(), "BND4", 4) != 0 || !get(bnd, 0x0C, count) ||
        !get(bnd, 0x20, header) || header < 0x24 || header > 0x100 || count <= 0 || count > 4096) {
        return ModelError::Layout;
    }
    const bool unicode = bnd[0x30] != 0;
    constexpr std::string_view Want = "NpcParam.param";
    for (std::int32_t k = 0; k < count; ++k) {
        const std::size_t at = 0x40 + static_cast<std::size_t>(header) * static_cast<std::size_t>(k);
        std::int64_t size{};
        std::uint32_t offset{}, name_at{};
        if (!get(bnd, at + 8, size) || !get(bnd, at + 24, offset) || !get(bnd, at + 32, name_at)) {
            return ModelError::Layout;
        }
        std::string name;
        for (std::size_t i = 0; i < 260; ++i) {
            std::uint16_t c{};
            if (unicode ? !get(bnd, name_at + 2 * i, c) : !get(bnd, name_at + i, *reinterpret_cast<std::uint8_t*>(&c))) {
                break;
            }
            if (c == 0) {
                break;
            }
            name.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        }
        if (name.size() < Want.size() || name.compare(name.size() - Want.size(), Want.size(), Want) != 0) {
            continue;
        }
        if (size <= 64 || offset > bnd.size() || bnd.size() - offset < static_cast<std::uint64_t>(size)) {
            return ModelError::Layout;
        }
        const std::span<const std::uint8_t> p(bnd.data() + offset, static_cast<std::size_t>(size));
        std::uint16_t rows{};
        if (!get(p, 10, rows) || rows == 0 || 64 + std::size_t{rows} * 24 > p.size()) {
            return ModelError::Layout;
        }
        std::vector<std::pair<std::int32_t, std::int64_t>> table(rows);
        std::vector<std::int64_t> starts(rows);
        for (std::size_t i = 0; i < rows; ++i) {
            get(p, 64 + i * 24, table[i].first);
            get(p, 64 + i * 24 + 8, table[i].second);
            starts[i] = table[i].second;
        }
        std::sort(starts.begin(), starts.end());
        std::int64_t step = 0;
        for (std::size_t i = 1; i < starts.size(); ++i) {
            const std::int64_t d = starts[i] - starts[i - 1];
            if (d > 0 && (step == 0 || d < step)) {
                step = d;
            }
        }
        if (rows > 1 && step < 332) {
            return ModelError::Layout; // not the rows this reads
        }
        for (const auto& [id, data] : table) {
            if (data < 0 || static_cast<std::uint64_t>(data) + 332 > p.size()) {
                continue;
            }
            const std::uint8_t* const row = p.data() + data;
            out[id] = std::uint32_t{row[326]} | std::uint32_t{row[327]} << 8 |
                      std::uint32_t{row[330]} << 16 | std::uint32_t{row[331]} << 24;
        }
        return out.empty() ? ModelError::Layout : ModelError::None;
    }
    return ModelError::Layout;
}

ModelError LoadModel(std::span<const std::uint8_t> file, Model& out) {
    out = {};
    std::vector<std::uint8_t> bnd;
    if (const ModelError e = Inflate(file, bnd); e != ModelError::None) {
        return e;
    }
    // The first FLVER2 inside (a character's bnd: its model first), viewed to the bnd's end.
    for (std::size_t at = 0; at + 8 <= bnd.size(); ++at) {
        if (std::memcmp(bnd.data() + at, "FLVER\0L\0", 8) != 0) {
            continue;
        }
        const View f{std::span<const std::uint8_t>(bnd).subspan(at)};
        Model model;
        const ModelError e = Flver(f, model);
        if (e == ModelError::None) {
            out = std::move(model);
            out.textures = TexturesOf(bnd);
        }
        return e;
    }
    return ModelError::NoFlver;
}

ModelError ReadModel(const std::filesystem::path& path, Model& out) {
    out = {};
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return ModelError::Read;
    }
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size <= 0 || static_cast<std::uint64_t>(size) > MaxInflated) {
        return size <= 0 ? ModelError::Read : ModelError::TooBig;
    }
    std::vector<std::uint8_t> file(static_cast<std::size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(file.data()), size)) {
        return ModelError::Read;
    }
    return LoadModel(file, out);
}

} // namespace VR::Sever
