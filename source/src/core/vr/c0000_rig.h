// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/vr/c0000_skeleton.h"

// Builds the hunter's Skeleton (core/vr/c0000_skeleton.h) from the user's own game data: the
// Havok reference pose (the hkaSkeleton "Master", read from game memory) and the
// player model's bind pose and dummy polys. Pure functions, no emulator
// dependencies (core/vr/bloodborne_skeleton.cpp at runtime, the unit tests).
//
// The math is the reference extraction's, in double precision and in the same order (no FMA
// contraction), rounded to float at the end:
//  - model = syncModelSpace FK of the reference pose (c0000_skeleton.h's conventions);
//  - FLVER bind: a bone's local rotation R = Ry(r.y) Rz(r.z) Rx(r.x), its model frame
//    (P.R R, P.t + P.R t) (the FLVER's scales are not applied);
//  - a dummy: its position, forward and up through its FLVER parent bone's bind frame into model
//    space, then into the local frame (Havok reference model) of the Havok bone its FLVER attach
//    bone is, by name; forward and up normalised;
//  - Havok -> FLVER: the FLVER bone with the same name.
// The digests (FNV-1a 64 of the raw inputs) tell the retail v1.09 data from anything else: a
// modded or different skeleton is not used (the features that need it stay off).
namespace VR::Body::C0000::Rig {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

#if defined(__clang__)
#define C0000_RIG_EXACT_MATH _Pragma("clang fp contract(off)")
#else
#define C0000_RIG_EXACT_MATH
#endif

// FNV-1a 64 of the retail v1.09 inputs (HavokDigest of the hkaSkeleton's parents and reference
// pose; FlverDigest of the player model's bones and body dummies). Hashes only: no game data.
constexpr u64 RetailHavokDigest = 0x10880e822f3bbfc8ull;
constexpr u64 RetailFlverDigest = 0x0432cc484d7ab5daull;

// The dummies the body uses (c0000_skeleton.h DmyId*).
inline constexpr std::array<s32, 11> BodyDummyIds = {
    DmyIdHead,         DmyIdEye,          DmyIdHead59,         DmyIdLockOn,
    DmyIdRightWeaponBase, DmyIdRightTwoHand, DmyIdLeftWeaponBase, DmyIdLeftHand,
    DmyIdLeftGun,      DmyIdLeftGun2,     DmyIdLeftGun3};

// ---- Bounds-checked little-endian reads -------------------------------------------------------

template <typename T>
inline bool ReadLe(std::span<const u8> d, std::size_t offset, T& out) {
    if (offset > d.size() || d.size() - offset < sizeof(T)) {
        return false;
    }
    std::memcpy(&out, d.data() + offset, sizeof(T));
    return true;
}

inline bool ReadFloats(std::span<const u8> d, std::size_t offset, float* out, int n) {
    for (int i = 0; i < n; ++i) {
        if (!ReadLe(d, offset + 4 * static_cast<std::size_t>(i), out[i])) {
            return false;
        }
    }
    return true;
}

// A NUL-terminated name at `offset`: UTF-16LE (ASCII kept, others as '?') or single-byte.
inline bool ReadName(std::span<const u8> d, std::size_t offset, bool utf16, std::string& out) {
    out.clear();
    for (std::size_t o = offset; o < d.size() && out.size() < 256;) {
        u32 c = d[o];
        if (utf16) {
            if (o + 1 >= d.size()) {
                return false;
            }
            c |= u32{d[o + 1]} << 8;
            o += 2;
        } else {
            o += 1;
        }
        if (c == 0) {
            return true;
        }
        out += static_cast<char>(c < 0x80 ? c : '?');
    }
    return false;
}

// ---- FNV-1a 64 ---------------------------------------------------------------------------------

struct Fnv {
    u64 h{0xcbf29ce484222325ull};
    void Bytes(const void* p, std::size_t n) {
        const auto* b = static_cast<const u8*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            h = (h ^ b[i]) * 0x100000001b3ull;
        }
    }
    template <typename T>
    void Value(T v) {
        Bytes(&v, sizeof(v));
    }
};

// ---- BND4 --------------------------------------------------------------------------------------

// The file whose name's base name (after the last '\' or '/', ASCII, case-insensitive) is `base`.
inline std::optional<std::span<const u8>> Bnd4Find(std::span<const u8> bnd, std::string_view base) {
    s32 count{};
    s64 header{};
    if (bnd.size() < 0x40 || std::memcmp(bnd.data(), "BND4", 4) != 0 || !ReadLe(bnd, 0x0C, count) ||
        !ReadLe(bnd, 0x20, header) || header < 0x24 || header > 0x100 || count <= 0 ||
        count > 65536) {
        return std::nullopt;
    }
    const bool utf16 = bnd[0x30] != 0;
    for (s32 k = 0; k < count; ++k) {
        const std::size_t entry = 0x40 + static_cast<std::size_t>(header) * static_cast<std::size_t>(k);
        s64 size{};
        u32 offset{}, name_offset{};
        if (!ReadLe(bnd, entry + 8, size) || !ReadLe(bnd, entry + 24, offset) ||
            !ReadLe(bnd, entry + 32, name_offset)) {
            return std::nullopt;
        }
        std::string name;
        if (size <= 0 || offset > bnd.size() || static_cast<u64>(size) > bnd.size() - offset ||
            !ReadName(bnd, name_offset, utf16, name)) {
            continue;
        }
        const std::size_t slash = name.find_last_of("\\/");
        std::string tail = slash == std::string::npos ? name : name.substr(slash + 1);
        std::transform(tail.begin(), tail.end(), tail.begin(),
                       [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; });
        if (tail == base) {
            return bnd.subspan(offset, static_cast<std::size_t>(size));
        }
    }
    return std::nullopt;
}

// ---- FLVER2: bones and dummy polys ------------------------------------------------------------

struct FlverBone {
    std::string name;
    s16 parent{-1};
    std::array<float, 3> t{};
    std::array<float, 3> r{}; // Euler radians (applied X, then Z, then Y)
    std::array<float, 3> s{};
};

struct FlverDummy {
    s16 ref{0};    // ReferenceID (the dummy id the game looks up)
    s16 parent{0}; // the bone whose bind frame the dummy is given in
    s16 attach{0}; // the bone it moves with
    std::array<float, 3> pos{};
    std::array<float, 3> fwd{};
    std::array<float, 3> up{};
};

struct FlverRig {
    std::vector<FlverBone> bones;
    std::vector<FlverDummy> dummies;
};

// FLVER2 ("FLVER\0", little endian): the header's counts (dummies +0x14, materials +0x18, bones
// +0x1C), the name encoding (+0x49), dummies of 0x40 bytes at 0x80, materials of 0x20 bytes, then
// bones of 0x80 bytes.
inline std::optional<FlverRig> ParseFlver(std::span<const u8> d, std::string& why) {
    s32 dummies{}, materials{}, bones{};
    if (d.size() < 0x80 || std::memcmp(d.data(), "FLVER\0", 6) != 0 || d[6] != 'L') {
        why = "not a little-endian FLVER";
        return std::nullopt;
    }
    if (!ReadLe(d, 0x14, dummies) || !ReadLe(d, 0x18, materials) || !ReadLe(d, 0x1C, bones) ||
        dummies < 0 || dummies > 4096 || materials < 0 || materials > 4096 || bones <= 0 ||
        bones > 4096) {
        why = "FLVER header counts";
        return std::nullopt;
    }
    const bool utf16 = d[0x49] != 0;
    FlverRig rig;
    rig.dummies.resize(static_cast<std::size_t>(dummies));
    for (s32 i = 0; i < dummies; ++i) {
        const std::size_t o = 0x80 + 0x40 * static_cast<std::size_t>(i);
        FlverDummy& x = rig.dummies[static_cast<std::size_t>(i)];
        if (!ReadFloats(d, o, x.pos.data(), 3) || !ReadFloats(d, o + 0x10, x.fwd.data(), 3) ||
            !ReadLe(d, o + 0x1C, x.ref) || !ReadLe(d, o + 0x1E, x.parent) ||
            !ReadFloats(d, o + 0x20, x.up.data(), 3) || !ReadLe(d, o + 0x2C, x.attach)) {
            why = "FLVER dummy out of bounds";
            return std::nullopt;
        }
    }
    const std::size_t bone_base = 0x80 + 0x40 * static_cast<std::size_t>(dummies) +
                                  0x20 * static_cast<std::size_t>(materials);
    rig.bones.resize(static_cast<std::size_t>(bones));
    for (s32 i = 0; i < bones; ++i) {
        const std::size_t o = bone_base + 0x80 * static_cast<std::size_t>(i);
        FlverBone& b = rig.bones[static_cast<std::size_t>(i)];
        u32 name{};
        if (!ReadFloats(d, o, b.t.data(), 3) || !ReadLe(d, o + 0x0C, name) ||
            !ReadFloats(d, o + 0x10, b.r.data(), 3) || !ReadLe(d, o + 0x1C, b.parent) ||
            !ReadFloats(d, o + 0x20, b.s.data(), 3) || !ReadName(d, name, utf16, b.name)) {
            why = "FLVER bone out of bounds";
            return std::nullopt;
        }
    }
    return rig;
}

// ---- HKX packfile (hk_2014): the hkaSkeleton --------------------------------------------------

struct HavokRig {
    std::array<std::string, BoneCount> names{};
    std::array<s16, BoneCount> parents{};
    std::array<Transform, BoneCount> local{};
};

// A packfile's one hkaSkeleton (the emulator reads the loaded copy from the game's memory
// instead; the unit tests' game-data tier reads the file): sections at 0x40 + the padding
// (+0x3E), "__data__"'s local fixups (pointers within the section) and virtual fixups (objects
// with their class names from "__classnames__").
inline std::optional<HavokRig> ParseHkxSkeleton(std::span<const u8> d, std::string& why) {
    s32 sections{}, version{};
    s16 padding{};
    if (d.size() < 0x40 || !ReadLe(d, 0x14, sections) || !ReadLe(d, 0x0C, version) ||
        !ReadLe(d, 0x3E, padding) || sections <= 0 || sections > 16 || padding < 0) {
        why = "not an HKX packfile";
        return std::nullopt;
    }
    struct Section {
        std::string name;
        u32 start{}, local{}, global{}, virt{}, exports{};
    };
    std::vector<Section> secs;
    const std::size_t stride = version >= 11 ? 0x40 : 0x30;
    for (s32 i = 0; i < sections; ++i) {
        const std::size_t o = 0x40 + static_cast<std::size_t>(padding) + stride * static_cast<std::size_t>(i);
        Section s;
        if (o + 20 + 28 > d.size()) {
            why = "HKX section header";
            return std::nullopt;
        }
        for (std::size_t c = 0; c < 19 && d[o + c] != 0; ++c) {
            s.name += static_cast<char>(d[o + c]);
        }
        if (!ReadLe(d, o + 20, s.start) || !ReadLe(d, o + 24, s.local) ||
            !ReadLe(d, o + 28, s.global) || !ReadLe(d, o + 32, s.virt) ||
            !ReadLe(d, o + 36, s.exports)) {
            why = "HKX section header";
            return std::nullopt;
        }
        secs.push_back(s);
    }
    const auto find = [&](std::string_view name) -> const Section* {
        for (const Section& s : secs) {
            if (s.name == name) {
                return &s;
            }
        }
        return nullptr;
    };
    const Section* data = find("__data__");
    const Section* classes = find("__classnames__");
    if (data == nullptr || classes == nullptr) {
        why = "HKX without __data__ / __classnames__";
        return std::nullopt;
    }
    const std::size_t ds = data->start;
    std::unordered_map<s32, s32> fix;
    for (std::size_t p = ds + data->local; p + 8 <= ds + data->global; p += 8) {
        s32 src{}, dst{};
        if (!ReadLe(d, p, src) || !ReadLe(d, p + 4, dst)) {
            why = "HKX local fixups";
            return std::nullopt;
        }
        if (src != -1) {
            fix[src] = dst;
        }
    }
    std::optional<s32> skeleton;
    for (std::size_t p = ds + data->virt; p + 12 <= ds + data->exports; p += 12) {
        s32 src{}, sec{}, name{};
        if (!ReadLe(d, p, src) || !ReadLe(d, p + 4, sec) || !ReadLe(d, p + 8, name)) {
            why = "HKX virtual fixups";
            return std::nullopt;
        }
        std::string cls;
        if (src != -1 && ReadName(d, classes->start + static_cast<std::size_t>(name), false, cls) &&
            cls == "hkaSkeleton") {
            if (skeleton) {
                why = "more than one hkaSkeleton";
                return std::nullopt;
            }
            skeleton = src;
        }
    }
    if (!skeleton) {
        why = "no hkaSkeleton";
        return std::nullopt;
    }
    const s32 o = *skeleton;
    const auto target = [&](s32 field) -> std::optional<std::size_t> {
        const auto it = fix.find(o + field);
        if (it == fix.end()) {
            return std::nullopt;
        }
        return ds + static_cast<std::size_t>(it->second);
    };
    s32 n_parents{}, n_bones{}, n_pose{};
    const auto parents = target(0x18);
    const auto bones = target(0x28);
    const auto pose = target(0x38);
    if (!ReadLe(d, ds + o + 0x20, n_parents) || !ReadLe(d, ds + o + 0x30, n_bones) ||
        !ReadLe(d, ds + o + 0x40, n_pose) || !parents || !bones || !pose ||
        n_parents != BoneCount || n_bones != BoneCount || n_pose != BoneCount) {
        why = "hkaSkeleton arrays (not 108 bones)";
        return std::nullopt;
    }
    HavokRig rig;
    for (int i = 0; i < BoneCount; ++i) {
        const auto k = static_cast<std::size_t>(i);
        const auto name = fix.find(static_cast<s32>(*bones - ds) + 16 * i);
        Transform& t = rig.local[k];
        if (!ReadLe(d, *parents + 2 * k, rig.parents[k]) || name == fix.end() ||
            !ReadName(d, ds + static_cast<std::size_t>(name->second), false, rig.names[k]) ||
            !ReadFloats(d, *pose + 0x30 * k, t.t.data(), 3) ||
            !ReadFloats(d, *pose + 0x30 * k + 0x10, t.q.data(), 4) ||
            !ReadFloats(d, *pose + 0x30 * k + 0x20, t.s.data(), 3)) {
            why = "hkaSkeleton bone out of bounds";
            return std::nullopt;
        }
    }
    return rig;
}

// ---- Checks and digests ------------------------------------------------------------------------

// The bone names and parents the code relies on (c0000_skeleton.h): "" when they all match.
inline std::string CheckInterop(const std::array<std::string, BoneCount>& names,
                                const std::array<s16, BoneCount>& parents) {
    for (int i = 0; i < BoneCount; ++i) {
        const auto k = static_cast<std::size_t>(i);
        if (parents[k] != Parents[k]) {
            return "bone " + std::to_string(i) + " parent " + std::to_string(parents[k]) +
                   " (expected " + std::to_string(Parents[k]) + ")";
        }
        if (names[k] != BoneNames[k]) {
            return "bone " + std::to_string(i) + " name " + names[k] + " (expected " +
                   std::string{BoneNames[k]} + ")";
        }
    }
    return {};
}

inline u64 HavokDigest(const std::array<s16, BoneCount>& parents,
                       const std::array<Transform, BoneCount>& local) {
    Fnv f;
    for (const s16 p : parents) {
        f.Value(p);
    }
    for (const Transform& t : local) {
        f.Bytes(t.t.data(), 12);
        f.Bytes(t.q.data(), 16);
        f.Bytes(t.s.data(), 12);
    }
    return f.h;
}

inline bool IsBodyDummy(s32 id) {
    return std::find(BodyDummyIds.begin(), BodyDummyIds.end(), id) != BodyDummyIds.end();
}

inline u64 FlverDigest(const FlverRig& rig) {
    Fnv f;
    f.Value(static_cast<u32>(rig.bones.size()));
    for (const FlverBone& b : rig.bones) {
        f.Bytes(b.name.data(), b.name.size());
        f.Value(u8{0});
        f.Value(b.parent);
        f.Bytes(b.t.data(), 12);
        f.Bytes(b.r.data(), 12);
        f.Bytes(b.s.data(), 12);
    }
    for (const FlverDummy& x : rig.dummies) {
        if (!IsBodyDummy(x.ref)) {
            continue;
        }
        f.Value(x.ref);
        f.Value(x.parent);
        f.Value(x.attach);
        f.Bytes(x.pos.data(), 12);
        f.Bytes(x.fwd.data(), 12);
        f.Bytes(x.up.data(), 12);
    }
    return f.h;
}

// ---- The build ---------------------------------------------------------------------------------

namespace Detail {

struct D3 {
    double x{}, y{}, z{};
};
struct D4 {
    double x{}, y{}, z{}, w{};
};
using M3 = std::array<std::array<double, 3>, 3>;

// Havok's rotate: 2 ((w^2 - 1/2) v + (q.v) q + w (q x v)).
inline D3 QRot(const D4& q, const D3& v) {
    C0000_RIG_EXACT_MATH
    const double d = q.x * v.x + q.y * v.y + q.z * v.z;
    const double cx = q.y * v.z - q.z * v.y;
    const double cy = q.z * v.x - q.x * v.z;
    const double cz = q.x * v.y - q.y * v.x;
    const double k = q.w * q.w - 0.5;
    return {2 * (k * v.x + d * q.x + q.w * cx), 2 * (k * v.y + d * q.y + q.w * cy),
            2 * (k * v.z + d * q.z + q.w * cz)};
}

inline D4 QMul(const D4& a, const D4& b) {
    C0000_RIG_EXACT_MATH
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

inline M3 MMul(const M3& a, const M3& b) {
    C0000_RIG_EXACT_MATH
    M3 m{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double s = 0.0;
            for (int k = 0; k < 3; ++k) {
                s += a[i][k] * b[k][j];
            }
            m[i][j] = s;
        }
    }
    return m;
}

inline D3 MVec(const M3& a, const D3& v) {
    C0000_RIG_EXACT_MATH
    const double in[3] = {v.x, v.y, v.z};
    double out[3];
    for (int i = 0; i < 3; ++i) {
        double s = 0.0;
        for (int k = 0; k < 3; ++k) {
            s += a[i][k] * in[k];
        }
        out[i] = s;
    }
    return {out[0], out[1], out[2]};
}

inline M3 Rx(double t) {
    const double c = std::cos(t), s = std::sin(t);
    return {{{1, 0, 0}, {0, c, -s}, {0, s, c}}};
}
inline M3 Ry(double t) {
    const double c = std::cos(t), s = std::sin(t);
    return {{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}};
}
inline M3 Rz(double t) {
    const double c = std::cos(t), s = std::sin(t);
    return {{{c, -s, 0}, {s, c, 0}, {0, 0, 1}}};
}

inline D3 Of(const std::array<float, 3>& a) {
    return {a[0], a[1], a[2]};
}

inline double Length(const D3& v) {
    C0000_RIG_EXACT_MATH
    double s = 0.0;
    s += v.x * v.x;
    s += v.y * v.y;
    s += v.z * v.z;
    return std::sqrt(s);
}

inline std::array<float, 3> F3(const D3& v) {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

inline std::array<float, 3> Unit(const D3& v) {
    C0000_RIG_EXACT_MATH
    const double n = Length(v);
    return F3({v.x / n, v.y / n, v.z / n});
}

struct Frame {
    M3 r{};
    D3 t{};
};

struct ModelD {
    D3 t;
    D4 q;
    D3 s;
};

// Havok FK (syncModelSpace) of the reference pose, in double.
inline std::array<ModelD, BoneCount> Fk(const std::array<Transform, BoneCount>& local) {
    C0000_RIG_EXACT_MATH
    std::array<ModelD, BoneCount> model{};
    for (int i = 0; i < BoneCount; ++i) {
        const auto k = static_cast<std::size_t>(i);
        const Transform& l = local[k];
        const D3 lt = Of(l.t);
        const D4 lq{l.q[0], l.q[1], l.q[2], l.q[3]};
        const D3 ls = Of(l.s);
        const int parent = Parents[k];
        if (parent < 0) {
            model[k] = {lt, lq, ls};
        } else {
            const ModelD& p = model[static_cast<std::size_t>(parent)];
            const D3 r = QRot(p.q, lt);
            model[k] = {{p.t.x + r.x, p.t.y + r.y, p.t.z + r.z},
                        QMul(p.q, lq),
                        {p.s.x * ls.x, p.s.y * ls.y, p.s.z * ls.z}};
        }
    }
    return model;
}

// `out.model` and the reference lengths and reach from `out.local` and its FK.
inline void FillModel(Skeleton& out, const std::array<ModelD, BoneCount>& model) {
    C0000_RIG_EXACT_MATH
    for (int i = 0; i < BoneCount; ++i) {
        const auto k = static_cast<std::size_t>(i);
        out.model[k] = {F3(model[k].t),
                        {static_cast<float>(model[k].q.x), static_cast<float>(model[k].q.y),
                         static_cast<float>(model[k].q.z), static_cast<float>(model[k].q.w)},
                        F3(model[k].s)};
    }
    const auto bone_length = [&](Bone b) {
        return static_cast<float>(Length(Of(out.local[static_cast<std::size_t>(b)].t)));
    };
    out.clavicle_offset = bone_length(R_Clavicle);
    out.clavicle_length = bone_length(R_UpperArm);
    out.upper_arm_length = bone_length(R_Forearm);
    out.forearm_length = bone_length(R_Hand);
    out.hand_to_weapon = bone_length(R_Weapon);
    const auto distance = [&](Bone a, Bone b) {
        const D3& p = model[static_cast<std::size_t>(a)].t;
        const D3& q = model[static_cast<std::size_t>(b)].t;
        return static_cast<float>(Length({p.x - q.x, p.y - q.y, p.z - q.z}));
    };
    out.reference_shoulder_to_hand = distance(R_UpperArm, R_Hand);
    out.reference_shoulder_to_weapon = distance(R_UpperArm, R_Weapon);
}

} // namespace Detail

// A skeleton whose `local` (and dummies) were set some other way (the tests' mannequin): its
// model pose, lengths and reach, exactly as Build computes them.
inline void FinishModel(Skeleton& s) {
    Detail::FillModel(s, Detail::Fk(s.local));
}

// The Skeleton from the reference pose (`local`, parents as c0000_skeleton.h's, checked by the
// caller with CheckInterop) and the player model's bones and dummies. False (and why) when the FLVER
// lacks a bone name or one of the body's dummies.
inline bool Build(const std::array<Transform, BoneCount>& local, const FlverRig& flver,
                  Skeleton& out, std::string& why) {
    C0000_RIG_EXACT_MATH
    using namespace Detail;
    out = {};
    out.local = local;
    const std::array<ModelD, BoneCount> model = Fk(local);
    FillModel(out, model);
    // Havok -> FLVER by name (a later FLVER bone of the same name wins, as in a name map).
    const std::size_t nf = flver.bones.size();
    std::unordered_map<std::string, int> flver_index;
    for (std::size_t j = 0; j < nf; ++j) {
        flver_index[flver.bones[j].name] = static_cast<int>(j);
    }
    std::unordered_map<int, int> flver_to_havok;
    for (int i = 0; i < BoneCount; ++i) {
        const auto it = flver_index.find(std::string{BoneNames[static_cast<std::size_t>(i)]});
        if (it == flver_index.end()) {
            why = "c0000.flver has no bone " + std::string{BoneNames[static_cast<std::size_t>(i)]};
            return false;
        }
        out.havok_to_flver[static_cast<std::size_t>(i)] = static_cast<s16>(it->second);
        flver_to_havok[it->second] = i;
    }
    // FLVER bind frames (memoised; parents may follow their children in the FLVER).
    std::vector<std::optional<Frame>> frames(nf);
    std::vector<u8> visiting(nf, 0);
    const auto frame_of = [&](auto&& self, int j) -> std::optional<Frame> {
        if (j < 0 || static_cast<std::size_t>(j) >= nf || visiting[static_cast<std::size_t>(j)]) {
            return std::nullopt;
        }
        auto& slot = frames[static_cast<std::size_t>(j)];
        if (slot) {
            return slot;
        }
        const FlverBone& b = flver.bones[static_cast<std::size_t>(j)];
        const M3 r = MMul(Ry(b.r[1]), MMul(Rz(b.r[2]), Rx(b.r[0])));
        const D3 t = Of(b.t);
        if (b.parent < 0) {
            slot = Frame{r, t};
            return slot;
        }
        visiting[static_cast<std::size_t>(j)] = 1;
        const auto p = self(self, b.parent);
        visiting[static_cast<std::size_t>(j)] = 0;
        if (!p) {
            return std::nullopt;
        }
        const D3 pt = MVec(p->r, t);
        slot = Frame{MMul(p->r, r), {p->t.x + pt.x, p->t.y + pt.y, p->t.z + pt.z}};
        return slot;
    };
    // The body's dummies in their Havok bone's frame, in FLVER order.
    struct Found {
        s32 id;
        int bone;
        D3 pos, fwd, up;
    };
    std::vector<Found> found;
    for (const FlverDummy& x : flver.dummies) {
        const bool optional = x.ref == DmyIdLeftGun4; // never fails the build
        if (!IsBodyDummy(x.ref) && !optional) {
            continue;
        }
        const auto hk = flver_to_havok.find(x.attach);
        if (hk == flver_to_havok.end()) {
            continue;
        }
        const auto f = frame_of(frame_of, x.parent);
        if (!f && optional) {
            continue;
        }
        if (!f) {
            why = "c0000.flver dummy " + std::to_string(x.ref) + ": no parent bone frame";
            return false;
        }
        const D3 pr = MVec(f->r, Of(x.pos));
        const D3 pos_m{f->t.x + pr.x, f->t.y + pr.y, f->t.z + pr.z};
        const D3 fwd_m = MVec(f->r, Of(x.fwd));
        const D3 up_m = MVec(f->r, Of(x.up));
        const ModelD& b = model[static_cast<std::size_t>(hk->second)];
        const D4 inv{-b.q.x, -b.q.y, -b.q.z, b.q.w};
        const D3 d{pos_m.x - b.t.x, pos_m.y - b.t.y, pos_m.z - b.t.z};
        found.push_back({x.ref, hk->second, QRot(inv, d), QRot(inv, fwd_m), QRot(inv, up_m)});
    }
    const auto make = [](const Found& f) {
        return DummyPoly{f.id, static_cast<Bone>(f.bone), F3(f.pos), Unit(f.fwd), Unit(f.up)};
    };
    const struct {
        s32 id;
        DummyPoly* slot;
    } wanted[] = {
        {DmyIdHead, &out.head7},
        {DmyIdLockOn, &out.lock_on},
        {DmyIdRightWeaponBase, &out.right_weapon_base},
        {DmyIdRightTwoHand, &out.right_two_hand},
        {DmyIdLeftWeaponBase, &out.left_weapon_base},
        {DmyIdLeftGun, &out.left_gun},
        {DmyIdLeftGun2, &out.left_gun2},
        {DmyIdLeftGun3, &out.left_gun3},
        {DmyIdLeftHand, &out.left_hand},
        {DmyIdHead59, &out.head59},
    };
    for (const auto& w : wanted) {
        const auto it = std::find_if(found.begin(), found.end(),
                                     [&](const Found& f) { return f.id == w.id; });
        if (it == found.end()) {
            why = "c0000.flver has no dummy " + std::to_string(w.id);
            return false;
        }
        *w.slot = make(*it);
    }
    const auto cannon = std::find_if(found.begin(), found.end(),
                                     [](const Found& f) { return f.id == DmyIdLeftGun4; });
    out.left_gun4 = cannon != found.end() ? make(*cannon) : DummyPoly{};
    std::vector<Found> eyes;
    for (const Found& f : found) {
        if (f.id == DmyIdEye) {
            eyes.push_back(f);
        }
    }
    if (eyes.size() != 2) {
        why = "c0000.flver has " + std::to_string(eyes.size()) + " eye dummies (expected 2)";
        return false;
    }
    // Left first: the larger Head local +Y (stable for equal values).
    std::stable_sort(eyes.begin(), eyes.end(),
                     [](const Found& a, const Found& b) { return -a.pos.y < -b.pos.y; });
    out.eyes = {make(eyes[0]), make(eyes[1])};
    out.eye_center_on_head = F3({(eyes[0].pos.x + eyes[1].pos.x) / 2,
                                 (eyes[0].pos.y + eyes[1].pos.y) / 2,
                                 (eyes[0].pos.z + eyes[1].pos.z) / 2});
    return true;
}

} // namespace VR::Body::C0000::Rig
