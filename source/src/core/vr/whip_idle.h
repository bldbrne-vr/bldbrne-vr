// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

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

#include "core/vr/c0000_rig.h"

// The whips' idle line (core/vr/bloodborne_whip.cpp), read from the user's own game files: the
// chain's rest shape before the part was ever seen in its idle clip (a flick into the whip as the
// first transform of a session). Header only, no emulator dependencies (the unit tests).
//
// From the form-B part's anibnd (game asset -> game asset):
//  - game asset: the hkaSkeleton (names, parents, reference pose);
//  - game asset: the idle clip, an hkaSplineCompressedAnimation, sampled at its first
//    frame (block 0, local frame 0: each track's B-spline at 0, or its static value), bound to the
//    skeleton's bones by its hkaAnimationBinding; a bone without a track keeps its reference pose.
// Then the model-space FK (rotation, translation; the scales are not applied) and the chain as
// ResolveChain finds it (B_blade_root's child that has children, and on down). The math is the
// reference extraction's (scratchpad whipfin/gen_rest.py with whip/whipanim.py), in double
// precision and the same order, rounded to float at the end.
namespace VR::Whip {

// The idle line of one whip (positions in m and rotations (x, y, z, w) in the part's model space).
struct WhipRest {
    std::int32_t variation{-1};
    std::int32_t bones{0}; // the skeleton's bone count
    std::int32_t n{0};     // chain bones
    float links{0.0f};     // m along the links
    std::array<std::array<float, 3>, 32> t{};
    std::array<std::array<float, 4>, 32> q{};
};

// The whips and their form-B part files (ids and file names only).
struct WhipPart {
    std::int32_t variation;
    const char* model; // Runtime model identifier
};
inline constexpr std::array<WhipPart, 2> WhipParts = {{
    {2400, "wp_a_1702"}, // Beast Cutter
    {2200, "wp_a_1070"}, // Threaded Cane
}};

namespace Idle {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
namespace R = VR::Body::C0000::Rig;

#if defined(__clang__)
#define WHIP_IDLE_EXACT_MATH _Pragma("clang fp contract(off)")
#else
#define WHIP_IDLE_EXACT_MATH
#endif

// ---- HKX packfile (hk_2014) -------------------------------------------------------------------

struct Packfile {
    std::span<const u8> d;
    std::size_t data{0}; // "__data__"'s start
    std::unordered_map<s32, s32> local; // local fixups (pointer at -> target), data-relative
    std::vector<std::pair<s32, std::string>> objects; // (offset, class name), data-relative

    std::optional<s32> Find(std::string_view cls) const {
        for (const auto& [o, c] : objects) {
            if (c == cls) {
                return o;
            }
        }
        return std::nullopt;
    }
    std::optional<std::size_t> Target(s32 field) const {
        const auto it = local.find(field);
        if (it == local.end()) {
            return std::nullopt;
        }
        return data + static_cast<std::size_t>(it->second);
    }
    template <typename T>
    bool At(s32 field, T& out) const {
        return field >= 0 && R::ReadLe(d, data + static_cast<std::size_t>(field), out);
    }
};

inline std::optional<Packfile> ParsePackfile(std::span<const u8> d, std::string& why) {
    s32 sections{}, version{};
    s16 padding{};
    if (d.size() < 0x40 || !R::ReadLe(d, 0x14, sections) || !R::ReadLe(d, 0x0C, version) ||
        !R::ReadLe(d, 0x3E, padding) || sections <= 0 || sections > 16 || padding < 0) {
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
        const std::size_t o =
            0x40 + static_cast<std::size_t>(padding) + stride * static_cast<std::size_t>(i);
        Section s;
        if (o + 40 > d.size()) {
            why = "HKX section header";
            return std::nullopt;
        }
        for (std::size_t c = 0; c < 19 && d[o + c] != 0; ++c) {
            s.name += static_cast<char>(d[o + c]);
        }
        if (!R::ReadLe(d, o + 20, s.start) || !R::ReadLe(d, o + 24, s.local) ||
            !R::ReadLe(d, o + 28, s.global) || !R::ReadLe(d, o + 32, s.virt) ||
            !R::ReadLe(d, o + 36, s.exports)) {
            why = "HKX section header";
            return std::nullopt;
        }
        secs.push_back(s);
    }
    const Section* data = nullptr;
    const Section* classes = nullptr;
    for (const Section& s : secs) {
        data = s.name == "__data__" ? &s : data;
        classes = s.name == "__classnames__" ? &s : classes;
    }
    if (data == nullptr || classes == nullptr || data->start > d.size()) {
        why = "HKX without __data__ / __classnames__";
        return std::nullopt;
    }
    Packfile p;
    p.d = d;
    p.data = data->start;
    for (std::size_t o = p.data + data->local; o + 8 <= p.data + data->global; o += 8) {
        s32 src{}, dst{};
        if (!R::ReadLe(d, o, src) || !R::ReadLe(d, o + 4, dst)) {
            why = "HKX local fixups";
            return std::nullopt;
        }
        if (src != -1) {
            p.local[src] = dst;
        }
    }
    for (std::size_t o = p.data + data->virt; o + 12 <= p.data + data->exports; o += 12) {
        s32 src{}, sec{}, name{};
        if (!R::ReadLe(d, o, src) || !R::ReadLe(d, o + 4, sec) || !R::ReadLe(d, o + 8, name)) {
            why = "HKX virtual fixups";
            return std::nullopt;
        }
        std::string cls;
        if (src != -1 && name >= 0 &&
            R::ReadName(d, classes->start + static_cast<std::size_t>(name), false, cls)) {
            p.objects.emplace_back(src, cls);
        }
    }
    return p;
}

// ---- The part's skeleton -----------------------------------------------------------------------

struct Bone {
    std::string name;
    s16 parent{-1};
    std::array<double, 3> t{};
    std::array<double, 4> q{}; // normalised
};

inline std::optional<std::vector<Bone>> ParseSkeleton(std::span<const u8> hkx, std::string& why) {
    WHIP_IDLE_EXACT_MATH
    const auto p = ParsePackfile(hkx, why);
    if (!p) {
        return std::nullopt;
    }
    const auto o = p->Find("hkaSkeleton");
    if (!o) {
        why = "no hkaSkeleton";
        return std::nullopt;
    }
    s32 n_parents{}, n_bones{}, n_pose{};
    const auto parents = p->Target(*o + 0x18);
    const auto bones = p->Target(*o + 0x28);
    const auto pose = p->Target(*o + 0x38);
    if (!p->At(*o + 0x20, n_parents) || !p->At(*o + 0x30, n_bones) || !p->At(*o + 0x40, n_pose) ||
        !parents || !bones || !pose || n_bones <= 0 || n_bones > 256 || n_parents != n_bones ||
        n_pose != n_bones) {
        why = "hkaSkeleton arrays";
        return std::nullopt;
    }
    std::vector<Bone> out(static_cast<std::size_t>(n_bones));
    for (s32 i = 0; i < n_bones; ++i) {
        const auto k = static_cast<std::size_t>(i);
        Bone& b = out[k];
        const auto name = p->local.find(static_cast<s32>(*bones - p->data) + 16 * i);
        std::array<float, 3> t{};
        std::array<float, 4> q{};
        if (!R::ReadLe(hkx, *parents + 2 * k, b.parent) || name == p->local.end() ||
            !R::ReadName(hkx, p->data + static_cast<std::size_t>(name->second), false, b.name) ||
            !R::ReadFloats(hkx, *pose + 0x30 * k, t.data(), 3) ||
            !R::ReadFloats(hkx, *pose + 0x30 * k + 0x10, q.data(), 4) || b.parent >= i) {
            why = "hkaSkeleton bone out of bounds";
            return std::nullopt;
        }
        double norm = 0.0;
        for (int c = 0; c < 4; ++c) {
            norm += static_cast<double>(q[c]) * q[c];
        }
        norm = std::sqrt(norm);
        if (!(norm > 0.0)) {
            why = "hkaSkeleton reference rotation";
            return std::nullopt;
        }
        for (int c = 0; c < 3; ++c) {
            b.t[c] = t[c];
        }
        for (int c = 0; c < 4; ++c) {
            b.q[c] = q[c] / norm;
        }
    }
    return out;
}

// ---- The idle clip's first frame (hkaSplineCompressedAnimation) --------------------------------

struct TrackPose {
    std::array<double, 3> t{};
    std::array<double, 4> q{}; // normalised
};

struct Frame0 {
    std::vector<TrackPose> tracks;
    std::vector<s16> track_to_bone; // empty: track i is bone i
};

namespace Detail {

struct Reader {
    std::span<const u8> d;
    std::size_t start{0};
    std::size_t o{0};
    bool ok{true};
    template <typename T>
    T Get() {
        T v{};
        ok = ok && R::ReadLe(d, o, v);
        o += sizeof(T);
        return v;
    }
    void Pad(std::size_t a) {
        const std::size_t r = (o - start) % a;
        if (r != 0) {
            o += a - r;
        }
    }
};

// The B-spline at u (de Boor), its knots and control points as the block stores them.
template <int Dim>
std::array<double, Dim> Evaluate(int n, int p, const std::vector<double>& knots,
                                 const std::vector<std::array<double, Dim>>& cps, double u) {
    WHIP_IDLE_EXACT_MATH
    int k = p;
    if (u >= knots[static_cast<std::size_t>(n + 1)]) {
        k = n;
    } else if (u <= knots[static_cast<std::size_t>(p)]) {
        k = p;
    } else {
        while (!(knots[static_cast<std::size_t>(k)] <= u && u < knots[static_cast<std::size_t>(k + 1)])) {
            ++k;
        }
    }
    std::vector<std::array<double, Dim>> d(static_cast<std::size_t>(p + 1));
    for (int j = 0; j <= p; ++j) {
        d[static_cast<std::size_t>(j)] = cps[static_cast<std::size_t>(j + k - p)];
    }
    for (int r = 1; r <= p; ++r) {
        for (int j = p; j >= r; --j) {
            const double lo = knots[static_cast<std::size_t>(j + k - p)];
            const double den = knots[static_cast<std::size_t>(j + 1 + k - r)] - lo;
            const double a = den == 0.0 ? 0.0 : (u - lo) / den;
            for (int c = 0; c < Dim; ++c) {
                d[static_cast<std::size_t>(j)][c] =
                    (1 - a) * d[static_cast<std::size_t>(j - 1)][c] + a * d[static_cast<std::size_t>(j)][c];
            }
        }
    }
    return d[static_cast<std::size_t>(p)];
}

inline double Quantized(Reader& r, int qt, double lo, double hi) {
    WHIP_IDLE_EXACT_MATH
    if (qt == 0) {
        return lo + (hi - lo) * (r.Get<u8>() / 255.0);
    }
    return lo + (hi - lo) * (r.Get<u16>() / 65535.0);
}

inline int QuatAlign(int qt) {
    static constexpr std::array<int, 6> Align = {4, 1, 2, 1, 2, 4};
    return qt >= 0 && qt < 6 ? Align[static_cast<std::size_t>(qt)] : 4;
}

inline std::optional<std::array<double, 4>> ReadQuat(Reader& r, int qt) {
    WHIP_IDLE_EXACT_MATH
    std::array<double, 3> t{};
    int shift = 0;
    bool invert = false;
    if (qt == 1) { // THREECOMP40
        std::uint64_t v = 0;
        for (int i = 0; i < 5; ++i) {
            v |= static_cast<std::uint64_t>(r.Get<u8>()) << (8 * i);
        }
        constexpr std::uint64_t Mask = (1u << 12) - 1;
        constexpr double Half = static_cast<double>(Mask >> 1);
        constexpr double Fraction = 0.000345436;
        for (int c = 0; c < 3; ++c) {
            t[c] = (static_cast<double>((v >> (12 * c)) & Mask) - Half) * Fraction;
        }
        shift = static_cast<int>((v >> 36) & 3);
        invert = ((v >> 38) & 1) != 0;
    } else if (qt == 2) { // THREECOMP48
        const u16 x = r.Get<u16>(), y = r.Get<u16>(), z = r.Get<u16>();
        constexpr u32 Mask = 0x7FFF;
        constexpr double Half = static_cast<double>(Mask >> 1);
        constexpr double Fraction = 0.000043161;
        t = {(static_cast<double>(x & Mask) - Half) * Fraction,
             (static_cast<double>(y & Mask) - Half) * Fraction,
             (static_cast<double>(z & Mask) - Half) * Fraction};
        shift = ((y >> 14) & 2) | ((x >> 15) & 1);
        invert = ((z >> 15) & 1) != 0;
    } else if (qt == 5) {
        std::array<double, 4> q{};
        for (int c = 0; c < 4; ++c) {
            q[c] = r.Get<float>();
        }
        return q;
    } else {
        return std::nullopt; // a quantisation the whips' clips do not use
    }
    double w = std::sqrt(std::max(0.0, 1.0 - (t[0] * t[0] + t[1] * t[1] + t[2] * t[2])));
    if (invert) {
        w = -w;
    }
    std::array<double, 4> q{};
    int src = 0;
    for (int c = 0; c < 4; ++c) {
        q[c] = c == shift ? w : t[static_cast<std::size_t>(src++)];
    }
    return q;
}

// A position / scale track at u (block-local frames).
inline std::array<double, 3> VecTrack(Reader& r, u8 types, int qt, std::array<double, 3> def,
                                      double u) {
    if ((types & 0x70) != 0) {
        const int n = r.Get<u16>();
        const int p = r.Get<u8>();
        std::vector<double> knots(static_cast<std::size_t>(n + p + 2));
        for (double& k : knots) {
            k = r.Get<u8>();
        }
        r.Pad(4);
        std::array<int, 3> kind{};    // 0 default, 1 static, 2 spline
        std::array<double, 3> lo = def, hi{};
        for (int c = 0; c < 3; ++c) {
            if ((types & (0x10 << c)) != 0) {
                kind[c] = 2;
                lo[c] = r.Get<float>();
                hi[c] = r.Get<float>();
            } else if ((types & (1 << c)) != 0) {
                kind[c] = 1;
                lo[c] = r.Get<float>();
            }
        }
        std::vector<std::array<double, 3>> cps(static_cast<std::size_t>(n + 1));
        for (auto& cp : cps) {
            for (int c = 0; c < 3; ++c) {
                cp[c] = kind[c] == 2 ? Quantized(r, qt, lo[c], hi[c]) : lo[c];
            }
        }
        r.Pad(4);
        if (!r.ok || n < p || p < 0) {
            r.ok = false;
            return def;
        }
        return Evaluate<3>(n, p, knots, cps, u);
    }
    std::array<double, 3> v = def;
    for (int c = 0; c < 3; ++c) {
        if ((types & (1 << c)) != 0) {
            v[c] = r.Get<float>();
        }
    }
    r.Pad(4);
    return v;
}

inline std::array<double, 4> RotTrack(Reader& r, u8 types, int qt, double u) {
    if ((types & 0xF0) != 0) {
        const int n = r.Get<u16>();
        const int p = r.Get<u8>();
        std::vector<double> knots(static_cast<std::size_t>(n + p + 2));
        for (double& k : knots) {
            k = r.Get<u8>();
        }
        r.Pad(static_cast<std::size_t>(QuatAlign(qt)));
        std::vector<std::array<double, 4>> cps(static_cast<std::size_t>(n + 1));
        for (auto& cp : cps) {
            const auto q = ReadQuat(r, qt);
            if (!q) {
                r.ok = false;
                return {0, 0, 0, 1};
            }
            cp = *q;
        }
        r.Pad(4);
        if (!r.ok || n < p || p < 0) {
            r.ok = false;
            return {0, 0, 0, 1};
        }
        return Evaluate<4>(n, p, knots, cps, u);
    }
    if ((types & 0x0F) != 0) {
        r.Pad(static_cast<std::size_t>(QuatAlign(qt)));
        const auto q = ReadQuat(r, qt);
        r.Pad(4);
        if (!q) {
            r.ok = false;
            return {0, 0, 0, 1};
        }
        return *q;
    }
    r.Pad(4);
    return {0, 0, 0, 1};
}

} // namespace Detail

inline std::optional<Frame0> ParseFrame0(std::span<const u8> hkx, std::string& why) {
    WHIP_IDLE_EXACT_MATH
    const auto p = ParsePackfile(hkx, why);
    if (!p) {
        return std::nullopt;
    }
    const auto o = p->Find("hkaSplineCompressedAnimation");
    if (!o) {
        why = "not a spline-compressed animation";
        return std::nullopt;
    }
    s32 tracks{}, blocks{}, n_offsets{}, n_data{};
    const auto offsets = p->Target(*o + 0x58);
    const auto data = p->Target(*o + 0x98);
    if (!p->At(*o + 0x14, tracks) || !p->At(*o + 0x3C, blocks) || !p->At(*o + 0x60, n_offsets) ||
        !p->At(*o + 0xA0, n_data) || !offsets || !data || tracks <= 0 || tracks > 256 ||
        blocks <= 0 || n_offsets < 1 || n_data <= 0 || *data + static_cast<std::size_t>(n_data) > hkx.size()) {
        why = "spline animation header";
        return std::nullopt;
    }
    s32 block0{};
    if (!R::ReadLe(hkx, *offsets, block0) || block0 < 0 || block0 >= n_data) {
        why = "spline animation block 0";
        return std::nullopt;
    }
    const auto bytes = hkx.subspan(*data, static_cast<std::size_t>(n_data));
    Detail::Reader r{bytes, static_cast<std::size_t>(block0), static_cast<std::size_t>(block0)};
    std::vector<std::array<u8, 4>> masks(static_cast<std::size_t>(tracks));
    for (auto& m : masks) {
        for (u8& b : m) {
            b = r.Get<u8>();
        }
    }
    r.Pad(4);
    Frame0 f;
    f.tracks.resize(static_cast<std::size_t>(tracks));
    constexpr double U = 0.0; // the first frame
    for (s32 i = 0; i < tracks && r.ok; ++i) {
        const auto& m = masks[static_cast<std::size_t>(i)];
        const int pq = m[0] & 3, rq = (m[0] >> 2) & 0xF, sq = (m[0] >> 6) & 3;
        TrackPose& t = f.tracks[static_cast<std::size_t>(i)];
        t.t = Detail::VecTrack(r, m[1], pq, {0.0, 0.0, 0.0}, U);
        auto q = Detail::RotTrack(r, m[2], rq, U);
        (void)Detail::VecTrack(r, m[3], sq, {1.0, 1.0, 1.0}, U);
        const double norm = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (!(norm > 0.0)) {
            r.ok = false;
            break;
        }
        for (double& c : q) {
            c /= norm;
        }
        t.q = q;
    }
    if (!r.ok) {
        why = "spline animation data out of bounds or of an unknown quantisation";
        return std::nullopt;
    }
    if (const auto b = p->Find("hkaAnimationBinding")) {
        s32 n{};
        const auto map = p->Target(*b + 0x20);
        if (p->At(*b + 0x28, n) && n > 0 && map) {
            f.track_to_bone.resize(static_cast<std::size_t>(n));
            for (s32 i = 0; i < n; ++i) {
                if (!R::ReadLe(hkx, *map + 2 * static_cast<std::size_t>(i),
                               f.track_to_bone[static_cast<std::size_t>(i)])) {
                    why = "animation binding out of bounds";
                    return std::nullopt;
                }
            }
        }
    }
    return f;
}

// ---- The idle line ----------------------------------------------------------------------------

namespace Detail {

inline std::array<double, 4> Mul(const std::array<double, 4>& a, const std::array<double, 4>& b) {
    WHIP_IDLE_EXACT_MATH
    const double ax = a[0], ay = a[1], az = a[2], aw = a[3];
    const double bx = b[0], by = b[1], bz = b[2], bw = b[3];
    return {aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz};
}

inline std::array<double, 3> Cross(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    WHIP_IDLE_EXACT_MATH
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

// v + 2 u x (u x v + w v)
inline std::array<double, 3> Rotate(const std::array<double, 4>& q, const std::array<double, 3>& v) {
    WHIP_IDLE_EXACT_MATH
    const std::array<double, 3> u = {q[0], q[1], q[2]};
    const auto c1 = Cross(u, v);
    const std::array<double, 3> inner = {c1[0] + q[3] * v[0], c1[1] + q[3] * v[1],
                                         c1[2] + q[3] * v[2]};
    const auto c2 = Cross(u, inner);
    return {v[0] + 2 * c2[0], v[1] + 2 * c2[1], v[2] + 2 * c2[2]};
}

} // namespace Detail

inline std::optional<WhipRest> BuildIdle(std::int32_t variation, const std::vector<Bone>& skeleton,
                                         const Frame0& frame, std::string& why) {
    WHIP_IDLE_EXACT_MATH
    const auto n = static_cast<s32>(skeleton.size());
    std::vector<TrackPose> local(skeleton.size());
    for (std::size_t i = 0; i < skeleton.size(); ++i) {
        local[i] = {skeleton[i].t, skeleton[i].q};
    }
    for (std::size_t ti = 0; ti < frame.tracks.size(); ++ti) {
        const s32 bi = ti < frame.track_to_bone.size() ? frame.track_to_bone[ti] : static_cast<s32>(ti);
        if (bi >= 0 && bi < n) {
            local[static_cast<std::size_t>(bi)] = frame.tracks[ti];
        }
    }
    std::vector<TrackPose> model(skeleton.size());
    for (s32 i = 0; i < n; ++i) {
        const auto k = static_cast<std::size_t>(i);
        const s16 p = skeleton[k].parent;
        if (p < 0) {
            model[k] = local[k];
            continue;
        }
        const TrackPose& pm = model[static_cast<std::size_t>(p)];
        const auto rt = Detail::Rotate(pm.q, local[k].t);
        model[k].t = {pm.t[0] + rt[0], pm.t[1] + rt[1], pm.t[2] + rt[2]};
        auto q = Detail::Mul(pm.q, local[k].q);
        const double norm = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        for (double& c : q) {
            c /= norm;
        }
        model[k].q = q;
    }
    s32 root = -1;
    for (s32 i = 0; i < n; ++i) {
        if (skeleton[static_cast<std::size_t>(i)].name == "B_blade_root") {
            root = i;
            break;
        }
    }
    if (root < 0) {
        why = "no B_blade_root";
        return std::nullopt;
    }
    const auto has_children = [&](s32 b) {
        for (s32 i = b + 1; i < n; ++i) {
            if (skeleton[static_cast<std::size_t>(i)].parent == b) {
                return true;
            }
        }
        return false;
    };
    WhipRest w;
    w.variation = variation;
    w.bones = n;
    std::array<s32, 32> chain{};
    s32 cur = root;
    for (;;) {
        s32 next = -1;
        for (s32 i = cur + 1; i < n; ++i) {
            if (skeleton[static_cast<std::size_t>(i)].parent == cur && has_children(i)) {
                next = i;
                break;
            }
        }
        if (next < 0) {
            break;
        }
        if (w.n == static_cast<s32>(chain.size())) {
            why = "a chain of more than 32 bones";
            return std::nullopt;
        }
        chain[static_cast<std::size_t>(w.n++)] = next;
        cur = next;
    }
    if (w.n < 8) {
        why = "a chain of " + std::to_string(w.n) + " bones only";
        return std::nullopt;
    }
    double links = 0.0;
    for (s32 k = 0; k < w.n; ++k) {
        const TrackPose& m = model[static_cast<std::size_t>(chain[static_cast<std::size_t>(k)])];
        for (int c = 0; c < 3; ++c) {
            w.t[static_cast<std::size_t>(k)][c] = static_cast<float>(m.t[c]);
        }
        for (int c = 0; c < 4; ++c) {
            w.q[static_cast<std::size_t>(k)][c] = static_cast<float>(m.q[c]);
        }
        if (k > 0) {
            const TrackPose& b =
                model[static_cast<std::size_t>(chain[static_cast<std::size_t>(k - 1)])];
            links += std::sqrt((m.t[0] - b.t[0]) * (m.t[0] - b.t[0]) +
                               (m.t[1] - b.t[1]) * (m.t[1] - b.t[1]) +
                               (m.t[2] - b.t[2]) * (m.t[2] - b.t[2]));
        }
    }
    w.links = static_cast<float>(links);
    for (s32 k = 0; k < w.n; ++k) {
        for (float v : w.t[static_cast<std::size_t>(k)]) {
            if (!std::isfinite(v)) {
                why = "not finite";
                return std::nullopt;
            }
        }
    }
    return w;
}

// The whole of it from the part's anibnd (a BND4): its game asset and game asset.
inline std::optional<WhipRest> FromAnibnd(std::int32_t variation, std::span<const u8> anibnd,
                                          std::string& why) {
    const auto skel = R::Bnd4Find(anibnd, "skeleton_2.hkx");
    const auto idle = R::Bnd4Find(anibnd, "a999_000000_2.hkx");
    if (!skel || !idle) {
        why = !skel ? "no skeleton_2.hkx" : "no idle clip a999_000000_2.hkx";
        return std::nullopt;
    }
    auto bones = ParseSkeleton(*skel, why);
    if (!bones) {
        why = "skeleton_2.hkx: " + why;
        return std::nullopt;
    }
    auto frame = ParseFrame0(*idle, why);
    if (!frame) {
        why = "a999_000000_2.hkx: " + why;
        return std::nullopt;
    }
    return BuildIdle(variation, *bones, *frame, why);
}

#undef WHIP_IDLE_EXACT_MATH

} // namespace Idle

} // namespace VR::Whip
