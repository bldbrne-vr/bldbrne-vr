// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "core/vr/arm_ik.h"

// The solid weapons' fitted shapes (vr.ini weapon_wall_shape=1, 2026-10-25): the capsules of the
// weapon in the hand, fitted to its own model as the hand holds it (core/vr/weapon_mesh.h, read
// from the user's game files by bloodborne_weapon_shape.cpp), so the other weapon meets the weapon
// where the weapon is: the folded Saw Cleaver's plate, the Hunter Axe's head, the Burial Blade's
// curve (bloodborne_body.cpp SolidWeapons; the walls meet its line).
//
// The shape is the hand's sphere at the origin and up to MaxCapsules capsules, in the order they
// are swept (core/vr/solid_weapon.h): the first from the hand out, each next one from a point
// already proven clear (on the hand or an earlier capsule: a sweep must start outside), never
// thicker than what it starts on, or carried (a thick head: its start swept along its own way
// from where it was clear, one sweep more). Each is laid greedily: it ends at one of the weapon's
// outermost points (a vertex extreme in some direction) or at a point spread over its surface (a
// chain following a curved staff), and starts at the nearest point of the shape so far or at an
// earlier capsule's end (a chain along the outline), whichever lowers most
//   J = the weapon's worst reach past the shape toward a wall (any direction)
//     + half its mean reach past it
//     + half the shape's worst reach past the weapon (where it would stop early; no capsule may
//       make it reach more than fat_cap past the weapon where it did not already)
//     + 0.15 m x the share of its surface outside the shape
//     + 2 x the capsules' length out in the air (more than `air_m` from its surface; no capsule
//       more than air_max: a chord across a curved handle's bow would stop it where nothing is)
//     + 5 mm per carried capsule (its extra sweep).
// It stops when the weapon reaches at most poke_ok past the shape with cover_ok of its surface in
// it, when no capsule (at least min_len long) lowers J by min_gain, or at max_caps. A flat wall
// meets a weapon at its outermost points, so the reach terms are what keeps it out of walls; the
// surface term and the air term keep the capsules on the weapon (a railing, a pole).
//
// Pure, deterministic (tests/test_weapon_fit.cpp). Metres, the grip's frame (R_Weapon).
namespace VR::WeaponFit {

using Body::Vec3;

constexpr int MaxCapsules = 7;

struct Capsule {
    Vec3 a{};
    Vec3 b{};
    float r{0.0f};
    bool carried{false}; // thicker than what it starts on: its start swept along its own way
    bool detached{false}; // authored start: no earlier capsule proves it clear
};

struct Params {
    int max_caps{MaxCapsules};
    bool extend_grip{false}; // simple proxies: include the handle behind the hand in capsule one
    float r{0.02f};         // each capsule's radius (the solid line's before 2026-10-25)
    float hand_r{0.05f};    // the hand's sphere at the origin
    float poke_ok{0.02f};   // done when the weapon reaches at most this past the shape ...
    float cover_ok{0.85f};  // ... and this share of its surface is in it
    float slack{0.015f};    // a surface point this near the shape counts as in it
    float air_m{0.035f};    // a capsule's axis further than this from the surface is in the air
    float air_weight{2.0f}; // J per metre of capsule in the air
    float air_max{0.02f};   // no capsule runs further through the air (a chord across a bow)
    float min_len{0.03f};   // no shorter capsule ...
    float min_gain{0.004f}; // ... and none that lowers J by less (a corner's few mm)
    int max_samples{4096};  // surface samples (about; spread over the triangles, by area)
    int spread{128};        // candidate ends spread over the surface (besides its corners)
    // Thick heads (a hammer's, a mace's): with `fat` a second fit tries capsules this thick too
    // while the weapon reaches more than twice poke_ok past the shape; one thicker than what it
    // starts on is carried (one sweep more each update: carried_cost), the shape reaching past the
    // weapon (where it stops early) counts fat_weight per metre, its worst direction, and a thick
    // one must lie in the weapon (Solid: fat_share of its surface within fat_fill of the weapon's
    // cells, so never on a blade). FitCapsules keeps it only where it beats the thin outline
    // (2026-10-25 survey of 46 weapon forms: the Bloodletter's ball 47 -> 23 cm, the Beast Cutter
    // 5.1 -> 2.9, Tonitrus' mace 2.9 -> 1.8, the Boom Hammer's head 4.1 -> 3.6; no outline worse;
    // a box-shaped head, the Kirkhammer's, keeps thin capsules zig-zagging to its corners).
    bool fat{true};
    std::array<float, 3> fat_r{0.04f, 0.06f, 0.09f};
    float carried_cost{0.005f};
    float fat_weight{0.5f};
    float fat_cap{0.025f};  // no capsule makes the shape reach further past the weapon than this
    float fat_fill{0.01f};  // a thick capsule's surface this near the weapon's cells ...
    float fat_share{0.95f}; // ... at this share of it at least
};

struct Fit {
    std::array<Capsule, MaxCapsules> caps{};
    int count{0};
    float poke{0.0f};     // how far the weapon reaches past the shape at most (m)
    float coverage{0.0f}; // the share of its surface in the shape
    float air{0.0f};      // capsule length in the air (m)
    float fat{0.0f};      // how far the shape reaches past the weapon at most (m)
    float score{0.0f};    // J with every term (the thin outline's and the thick fit's compared)
};

namespace Detail {

constexpr int Dirs = 256;

inline const std::array<Vec3, Dirs>& Directions() {
    static const std::array<Vec3, Dirs> dirs = [] {
        std::array<Vec3, Dirs> d{};
        const double golden = 3.14159265358979323846 * (1.0 + std::sqrt(5.0));
        for (int i = 0; i < Dirs; ++i) {
            const double k = i + 0.5;
            const double phi = std::acos(1.0 - 2.0 * k / Dirs);
            const double th = golden * k;
            d[static_cast<std::size_t>(i)] = {static_cast<float>(std::cos(th) * std::sin(phi)),
                                              static_cast<float>(std::sin(th) * std::sin(phi)),
                                              static_cast<float>(std::cos(phi))};
        }
        return d;
    }();
    return dirs;
}

inline float SegDist(Vec3 p, Vec3 a, Vec3 ab, float inv_l2) {
    const float t = inv_l2 > 0.0f ? std::clamp(Body::Dot(p - a, ab) * inv_l2, 0.0f, 1.0f) : 0.0f;
    return Body::Length(p - (a + ab * t));
}

// Cells (of `cell` m) within `reach` of the weapon's surface samples.
class NearGrid {
public:
    void Build(std::span<const Vec3> pts, float reach) {
        if (pts.empty()) {
            return;
        }
        lo_ = hi_ = pts[0];
        for (const Vec3& p : pts) {
            lo_ = {std::min(lo_.x, p.x), std::min(lo_.y, p.y), std::min(lo_.z, p.z)};
            hi_ = {std::max(hi_.x, p.x), std::max(hi_.y, p.y), std::max(hi_.z, p.z)};
        }
        const Vec3 pad{reach + 0.02f, reach + 0.02f, reach + 0.02f};
        lo_ = lo_ - pad;
        hi_ = hi_ + pad;
        const Vec3 size = hi_ - lo_;
        cell_ = 0.01f;
        while ((size.x / cell_ + 1) * (size.y / cell_ + 1) * (size.z / cell_ + 1) > 4.0e6f) {
            cell_ *= 1.25f;
        }
        nx_ = static_cast<int>(size.x / cell_) + 1;
        ny_ = static_cast<int>(size.y / cell_) + 1;
        nz_ = static_cast<int>(size.z / cell_) + 1;
        bits_.assign((static_cast<std::size_t>(nx_) * ny_ * nz_ + 63) / 64, 0);
        const int k = static_cast<int>(std::ceil(reach / cell_));
        std::vector<std::array<int, 3>> ball;
        for (int i = -k; i <= k; ++i) {
            for (int j = -k; j <= k; ++j) {
                for (int l = -k; l <= k; ++l) {
                    if (static_cast<float>(i * i + j * j + l * l) * cell_ * cell_ <=
                        reach * reach) {
                        ball.push_back({i, j, l});
                    }
                }
            }
        }
        for (const Vec3& p : pts) {
            const int ci = static_cast<int>((p.x - lo_.x) / cell_);
            const int cj = static_cast<int>((p.y - lo_.y) / cell_);
            const int cl = static_cast<int>((p.z - lo_.z) / cell_);
            for (const auto& o : ball) {
                Set(ci + o[0], cj + o[1], cl + o[2]);
            }
        }
    }
    // Length of the segment a -> b (sampled every cell) outside the marked cells.
    float AirLength(Vec3 a, Vec3 b) const {
        const float len = Body::Length(b - a);
        if (bits_.empty()) {
            return len;
        }
        const int n = std::max(static_cast<int>(len / cell_), 1);
        int miss = 0;
        for (int s = 0; s <= n; ++s) {
            const Vec3 p = a + (b - a) * (static_cast<float>(s) / n);
            if (!Get(static_cast<int>(std::floor((p.x - lo_.x) / cell_)),
                     static_cast<int>(std::floor((p.y - lo_.y) / cell_)),
                     static_cast<int>(std::floor((p.z - lo_.z) / cell_)))) {
                ++miss;
            }
        }
        return len * static_cast<float>(miss) / static_cast<float>(n + 1);
    }

private:
    void Set(int i, int j, int l) {
        if (i < 0 || j < 0 || l < 0 || i >= nx_ || j >= ny_ || l >= nz_) {
            return;
        }
        const std::size_t at = (static_cast<std::size_t>(l) * ny_ + j) * nx_ + i;
        bits_[at / 64] |= std::uint64_t{1} << (at % 64);
    }
    bool Get(int i, int j, int l) const {
        if (i < 0 || j < 0 || l < 0 || i >= nx_ || j >= ny_ || l >= nz_) {
            return false;
        }
        const std::size_t at = (static_cast<std::size_t>(l) * ny_ + j) * nx_ + i;
        return (bits_[at / 64] >> (at % 64)) & 1u;
    }
    Vec3 lo_{}, hi_{};
    float cell_{0.01f};
    int nx_{0}, ny_{0}, nz_{0};
    std::vector<std::uint64_t> bits_;
};

// The weapon as cells of about a centimetre: its surface (each triangle laid down in points half
// a cell apart) and what it encloses (every cell the outside, flooded in from the box's border,
// never reaches: a hammer's head is solid, a blade only its surface; a hole in the mesh lets the
// flood in and leaves that part hollow, never fuller), and those grown by a reach (Grow).
class Solid {
public:
    using Bits = std::vector<std::uint64_t>;

    void Build(std::span<const Vec3> verts, std::span<const std::uint32_t> tris, float pad) {
        const std::size_t ntri = tris.size() / 3;
        const auto corners = [&](std::size_t t, Vec3& a, Vec3& b, Vec3& d) {
            const std::uint32_t i0 = tris[3 * t], i1 = tris[3 * t + 1], i2 = tris[3 * t + 2];
            if (i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size()) {
                return false;
            }
            a = verts[i0];
            b = verts[i1];
            d = verts[i2];
            return Body::IsFinite(a) && Body::IsFinite(b) && Body::IsFinite(d);
        };
        bool any = false;
        Vec3 lo{}, hi{};
        for (std::size_t t = 0; t < ntri; ++t) {
            std::array<Vec3, 3> v{};
            if (!corners(t, v[0], v[1], v[2])) {
                continue;
            }
            for (const Vec3& q : v) {
                lo = any ? Vec3{std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)} : q;
                hi = any ? Vec3{std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)} : q;
                any = true;
            }
        }
        if (!any) {
            return;
        }
        // At least one free cell round it all (the flood's way round), no more than 4M cells.
        const auto cells = [&](float cell) {
            const Vec3 size = hi - lo;
            const float more = 2.0f * (pad + cell);
            return ((size.x + more) / cell + 1) * ((size.y + more) / cell + 1) *
                   ((size.z + more) / cell + 1);
        };
        cell_ = 0.01f;
        while (cells(cell_) > 4.0e6f) {
            cell_ *= 1.25f;
        }
        const float edge = pad + cell_;
        lo_ = lo - Vec3{edge, edge, edge};
        const Vec3 size = hi - lo + Vec3{2.0f * edge, 2.0f * edge, 2.0f * edge};
        nx_ = static_cast<int>(size.x / cell_) + 1;
        ny_ = static_cast<int>(size.y / cell_) + 1;
        nz_ = static_cast<int>(size.z / cell_) + 1;
        const std::size_t n = static_cast<std::size_t>(nx_) * ny_ * nz_;
        Bits surface((n + 63) / 64, 0);
        for (std::size_t t = 0; t < ntri; ++t) {
            Vec3 a, b, d;
            if (!corners(t, a, b, d)) {
                continue;
            }
            const float e =
                std::max({Body::Length(b - a), Body::Length(d - b), Body::Length(a - d)});
            const int k = std::clamp(static_cast<int>(std::ceil(e / (0.5f * cell_))), 1, 1024);
            const Vec3 u = (b - a) * (1.0f / static_cast<float>(k));
            const Vec3 v = (d - a) * (1.0f / static_cast<float>(k));
            for (int i = 0; i <= k; ++i) {
                for (int j = 0; i + j <= k; ++j) {
                    std::size_t at = 0;
                    if (Index(a + u * static_cast<float>(i) + v * static_cast<float>(j), at)) {
                        Set(surface, at);
                    }
                }
            }
        }
        // The outside: from every border cell, six ways, through no surface cell.
        Bits outside((n + 63) / 64, 0);
        std::vector<std::uint32_t> todo;
        const auto visit = [&](int i, int j, int l) {
            const std::size_t at = At(i, j, l);
            if (!Get(surface, at) && !Get(outside, at)) {
                Set(outside, at);
                todo.push_back(static_cast<std::uint32_t>(at));
            }
        };
        for (int j = 0; j < ny_; ++j) {
            for (int l = 0; l < nz_; ++l) {
                visit(0, j, l);
                visit(nx_ - 1, j, l);
            }
        }
        for (int i = 0; i < nx_; ++i) {
            for (int l = 0; l < nz_; ++l) {
                visit(i, 0, l);
                visit(i, ny_ - 1, l);
            }
            for (int j = 0; j < ny_; ++j) {
                visit(i, j, 0);
                visit(i, j, nz_ - 1);
            }
        }
        while (!todo.empty()) {
            const std::size_t at = todo.back();
            todo.pop_back();
            const int i = static_cast<int>(at % static_cast<std::size_t>(nx_));
            const int j = static_cast<int>((at / static_cast<std::size_t>(nx_)) %
                                           static_cast<std::size_t>(ny_));
            const int l = static_cast<int>(at / (static_cast<std::size_t>(nx_) * ny_));
            if (i > 0) {
                visit(i - 1, j, l);
            }
            if (i + 1 < nx_) {
                visit(i + 1, j, l);
            }
            if (j > 0) {
                visit(i, j - 1, l);
            }
            if (j + 1 < ny_) {
                visit(i, j + 1, l);
            }
            if (l > 0) {
                visit(i, j, l - 1);
            }
            if (l + 1 < nz_) {
                visit(i, j, l + 1);
            }
        }
        in_.assign(outside.size(), 0);
        for (std::size_t w = 0; w < in_.size(); ++w) {
            in_[w] = ~outside[w];
        }
        if (n % 64 != 0) {
            in_.back() &= (std::uint64_t{1} << (n % 64)) - 1;
        }
    }
    // The weapon's cells and every cell within `reach` of one.
    Bits Grow(float reach) const {
        Bits out = in_;
        const int k = static_cast<int>(std::floor(reach / cell_));
        std::vector<std::array<int, 3>> ball;
        for (int i = -k; i <= k; ++i) {
            for (int j = -k; j <= k; ++j) {
                for (int l = -k; l <= k; ++l) {
                    if (static_cast<float>(i * i + j * j + l * l) * cell_ * cell_ <=
                        reach * reach) {
                        ball.push_back({i, j, l});
                    }
                }
            }
        }
        for (std::size_t w = 0; w < in_.size(); ++w) {
            for (std::uint64_t bits = in_[w]; bits != 0; bits &= bits - 1) {
                const std::size_t at = w * 64 + static_cast<std::size_t>(std::countr_zero(bits));
                const int i = static_cast<int>(at % static_cast<std::size_t>(nx_));
                const int j = static_cast<int>((at / static_cast<std::size_t>(nx_)) %
                                               static_cast<std::size_t>(ny_));
                const int l = static_cast<int>(at / (static_cast<std::size_t>(nx_) * ny_));
                // Only a cell on the weapon's edge (one of its six neighbours outside) grows it.
                const bool edge = i == 0 || i + 1 == nx_ || j == 0 || j + 1 == ny_ || l == 0 ||
                                  l + 1 == nz_ || !Get(in_, At(i - 1, j, l)) ||
                                  !Get(in_, At(i + 1, j, l)) || !Get(in_, At(i, j - 1, l)) ||
                                  !Get(in_, At(i, j + 1, l)) || !Get(in_, At(i, j, l - 1)) ||
                                  !Get(in_, At(i, j, l + 1));
                if (!edge) {
                    continue;
                }
                for (const auto& o : ball) {
                    const int x = i + o[0], y = j + o[1], z = l + o[2];
                    if (x >= 0 && y >= 0 && z >= 0 && x < nx_ && y < ny_ && z < nz_) {
                        Set(out, At(x, y, z));
                    }
                }
            }
        }
        return out;
    }
    bool In(const Bits& bits, Vec3 p) const {
        std::size_t at = 0;
        return Index(p, at) && Get(bits, at);
    }
    // Length of the segment a -> b (sampled every cell) outside `nearby`.
    float AirLength(const Bits& nearby, Vec3 a, Vec3 b) const {
        const float len = Body::Length(b - a);
        if (nearby.empty()) {
            return len;
        }
        const int n = std::max(static_cast<int>(len / cell_), 1);
        int miss = 0;
        for (int s = 0; s <= n; ++s) {
            miss += In(nearby, a + (b - a) * (static_cast<float>(s) / n)) ? 0 : 1;
        }
        return len * static_cast<float>(miss) / static_cast<float>(n + 1);
    }
    // The share of a capsule's surface (points about a cell apart all over it) in `bits`.
    float Within(const Bits& bits, Vec3 s, Vec3 e, float r) const {
        if (bits.empty()) {
            return 0.0f;
        }
        const Vec3 d = e - s;
        const float len = Body::Length(d);
        const Vec3 t = len > 1e-6f ? d * (1.0f / len) : Vec3{0.0f, 1.0f, 0.0f};
        const Vec3 u1 = Body::Normalize(
            Body::Cross(t, std::abs(t.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f}));
        const Vec3 u2 = Body::Cross(t, u1);
        const int around = std::clamp(static_cast<int>(std::ceil(6.2832f * r / cell_)), 8, 64);
        const int steps = std::max(static_cast<int>(std::ceil(len / cell_)), 1);
        const int rings = std::max(static_cast<int>(std::ceil(1.5708f * r / cell_)), 1);
        int in = 0, all = 0;
        for (int k = 0; k < around; ++k) {
            const float ang = 6.2832f * static_cast<float>(k) / static_cast<float>(around);
            const Vec3 out = u1 * std::cos(ang) + u2 * std::sin(ang);
            for (int i = 0; i <= steps; ++i) {
                in += In(bits, s + d * (static_cast<float>(i) / steps) + out * r) ? 1 : 0;
                ++all;
            }
            // Its two round ends, from the rim to the pole.
            for (int q = 1; q <= rings; ++q) {
                const float phi = 1.5708f * static_cast<float>(q) / static_cast<float>(rings);
                const Vec3 o = out * (r * std::cos(phi));
                const Vec3 h = t * (r * std::sin(phi));
                in += (In(bits, e + o + h) ? 1 : 0) + (In(bits, s + o - h) ? 1 : 0);
                all += 2;
            }
        }
        return static_cast<float>(in) / static_cast<float>(all);
    }

private:
    std::size_t At(int i, int j, int l) const {
        return (static_cast<std::size_t>(l) * ny_ + j) * nx_ + i;
    }
    bool Index(Vec3 p, std::size_t& at) const {
        const float fi = std::floor((p.x - lo_.x) / cell_);
        const float fj = std::floor((p.y - lo_.y) / cell_);
        const float fl = std::floor((p.z - lo_.z) / cell_);
        if (!(fi >= 0.0f && fj >= 0.0f && fl >= 0.0f && fi < static_cast<float>(nx_) &&
              fj < static_cast<float>(ny_) && fl < static_cast<float>(nz_))) {
            return false;
        }
        at = At(static_cast<int>(fi), static_cast<int>(fj), static_cast<int>(fl));
        return true;
    }
    static void Set(Bits& bits, std::size_t at) {
        bits[at / 64] |= std::uint64_t{1} << (at % 64);
    }
    static bool Get(const Bits& bits, std::size_t at) {
        return (bits[at / 64] >> (at % 64)) & 1u;
    }
    Vec3 lo_{};
    float cell_{0.01f};
    int nx_{0}, ny_{0}, nz_{0};
    Bits in_;
};

// One fit, thin capsules only or (p.fat) thick ones too.
inline Fit FitOnce(std::span<const Vec3> verts, std::span<const std::uint32_t> tris,
                   const Params& p) {
    Fit fit{};
    if (verts.empty() || tris.size() < 3) {
        return fit;
    }
    // Surface samples, about evenly spread: each triangle cut into k x k smaller ones (k by its
    // longest edge and a spacing that keeps them to about max_samples), their centroids weighted by
    // their areas. A long thin triangle (a blade's face in one or two) is sampled all along.
    const std::size_t ntri = tris.size() / 3;
    const auto corners = [&](std::size_t t, Vec3& a, Vec3& b, Vec3& d) {
        const std::uint32_t i0 = tris[3 * t], i1 = tris[3 * t + 1], i2 = tris[3 * t + 2];
        if (i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size()) {
            return false;
        }
        a = verts[i0];
        b = verts[i1];
        d = verts[i2];
        return Body::IsFinite(a) && Body::IsFinite(b) && Body::IsFinite(d);
    };
    float total = 0.0f;
    for (std::size_t t = 0; t < ntri; ++t) {
        Vec3 a, b, d;
        if (corners(t, a, b, d)) {
            total += 0.5f * Body::Length(Body::Cross(b - a, d - a));
        }
    }
    const float spacing =
        std::max(0.02f, std::sqrt(2.0f * total / static_cast<float>(std::max(p.max_samples, 1))));
    std::vector<Vec3> c;
    std::vector<float> w;
    for (std::size_t t = 0; t < ntri; ++t) {
        Vec3 a, b, d;
        if (!corners(t, a, b, d)) {
            continue;
        }
        const float area = 0.5f * Body::Length(Body::Cross(b - a, d - a));
        if (!(area > 1e-9f)) {
            continue;
        }
        const float edge =
            std::max({Body::Length(b - a), Body::Length(d - b), Body::Length(a - d)});
        const int k = std::clamp(static_cast<int>(std::ceil(edge / spacing)), 1, 32);
        const float sub = area / static_cast<float>(k * k);
        const Vec3 u = (b - a) * (1.0f / static_cast<float>(k));
        const Vec3 v = (d - a) * (1.0f / static_cast<float>(k));
        for (int i = 0; i < k; ++i) {
            for (int j = 0; i + j < k; ++j) {
                c.push_back(a + u * (static_cast<float>(i) + 1.0f / 3.0f) +
                            v * (static_cast<float>(j) + 1.0f / 3.0f));
                w.push_back(sub);
                if (i + j + 1 < k) {
                    c.push_back(a + u * (static_cast<float>(i) + 2.0f / 3.0f) +
                                v * (static_cast<float>(j) + 2.0f / 3.0f));
                    w.push_back(sub);
                }
            }
        }
    }
    if (c.empty()) {
        return fit;
    }
    float wsum = 0.0f;
    for (float x : w) {
        wsum += x;
    }
    const auto& dirs = Directions();
    // The weapon's reach in each direction, and its outermost corners (the candidate ends).
    std::array<float, Dirs> reach{};
    std::vector<std::uint32_t> hull;
    for (int d = 0; d < Dirs; ++d) {
        float best = -1e30f;
        std::uint32_t at = 0;
        for (std::uint32_t i = 0; i < verts.size(); ++i) {
            const float s = Body::Dot(verts[i], dirs[static_cast<std::size_t>(d)]);
            if (s > best) {
                best = s;
                at = i;
            }
        }
        reach[static_cast<std::size_t>(d)] = best;
        if (std::find(hull.begin(), hull.end(), at) == hull.end()) {
            hull.push_back(at);
        }
    }
    // Candidate ends: the outermost corners, and points spread over the surface (farthest-point
    // sampling from the one farthest from the hand), so a chain can follow a curved staff.
    std::vector<Vec3> ends;
    ends.reserve(hull.size() + static_cast<std::size_t>(p.spread));
    for (const std::uint32_t h : hull) {
        ends.push_back(verts[h]);
    }
    {
        std::vector<float> apart(c.size());
        std::size_t at = 0;
        for (std::size_t i = 0; i < c.size(); ++i) {
            if (Body::LengthSq(c[i]) > Body::LengthSq(c[at])) {
                at = i;
            }
        }
        for (std::size_t i = 0; i < c.size(); ++i) {
            apart[i] = Body::Length(c[i] - c[at]);
        }
        for (int k = 0; k < p.spread && k < static_cast<int>(c.size()); ++k) {
            ends.push_back(c[at]);
            std::size_t next = 0;
            for (std::size_t i = 0; i < c.size(); ++i) {
                apart[i] = std::min(apart[i], Body::Length(c[i] - c[at]));
                if (apart[i] > apart[next]) {
                    next = i;
                }
            }
            at = next;
        }
    }
    // In the air: further than air_m from the weapon's surface (its samples); with thick capsules,
    // further than air_m from its cells (a capsule may cross a solid head), and a thick one must
    // lie in it (fat_share of its surface within fat_fill of its cells).
    NearGrid grid;
    Solid solid;
    Solid::Bits nearby, fill;
    if (!p.fat) {
        grid.Build(c, p.air_m);
    } else {
        solid.Build(verts, tris, std::max(p.air_m, p.fat_fill) + 0.02f);
        nearby = solid.Grow(p.air_m);
        fill = solid.Grow(p.fat_fill);
    }
    // The shape so far: each sample's distance outside it, its reach per direction.
    std::vector<float> dist(c.size());
    for (std::size_t i = 0; i < c.size(); ++i) {
        dist[i] = Body::Length(c[i]) - p.hand_r;
    }
    std::array<float, Dirs> shape{};
    shape.fill(p.hand_r);
    float covered = 0.0f;
    for (std::size_t i = 0; i < c.size(); ++i) {
        if (dist[i] <= p.slack) {
            covered += w[i];
        }
    }
    const auto gaps = [&](const std::array<float, Dirs>& s, float& worst, float& mean, float& fat) {
        worst = 0.0f;
        mean = 0.0f;
        fat = 0.0f;
        for (int d = 0; d < Dirs; ++d) {
            const float g = reach[static_cast<std::size_t>(d)] - s[static_cast<std::size_t>(d)];
            worst = std::max(worst, g);
            mean += std::max(g, 0.0f);
            fat = std::max(fat, -g);
        }
        mean /= Dirs;
    };
    float worst = 0.0f, mean = 0.0f, fat = 0.0f;
    gaps(shape, worst, mean, fat);
    float air = 0.0f;
    int carried = 0;
    const float fat_w = p.fat ? p.fat_weight : 0.0f;
    float j_now = worst + 0.5f * mean + fat_w * fat + 0.15f * (1.0f - covered / wsum);
    std::array<float, Dirs> trial{};
    while (fit.count < std::min(p.max_caps, MaxCapsules)) {
        if (worst <= p.poke_ok && covered / wsum >= p.cover_ok) {
            break;
        }
        float best_j = j_now - p.min_gain;
        Capsule best{};
        float best_air = 0.0f;
        bool found = false;
        for (const Vec3& q : ends) {
            // Starts: the nearest point of the shape so far, and the earlier capsules' ends.
            std::array<std::pair<Vec3, float>, MaxCapsules + 1> starts{};
            int ns = 0;
            {
                Vec3 s{};
                float rs = p.hand_r;
                float ds = Body::Length(q);
                for (int k = 0; k < fit.count; ++k) {
                    const Capsule& cap = fit.caps[static_cast<std::size_t>(k)];
                    const Vec3 ab = cap.b - cap.a;
                    const float l2 = Body::LengthSq(ab);
                    const float t =
                        l2 > 1e-12f ? std::clamp(Body::Dot(q - cap.a, ab) / l2, 0.0f, 1.0f) : 0.0f;
                    const Vec3 x = cap.a + ab * t;
                    const float dx = Body::Length(q - x);
                    if (dx < ds) {
                        ds = dx;
                        s = x;
                        rs = cap.r;
                    }
                }
                starts[static_cast<std::size_t>(ns++)] = {s, rs};
                for (int k = 0; k < fit.count; ++k) {
                    const Capsule& cap = fit.caps[static_cast<std::size_t>(k)];
                    starts[static_cast<std::size_t>(ns++)] = {cap.b, cap.r};
                }
            }
            for (int si = 0; si < ns; ++si) {
                const Vec3 s = starts[static_cast<std::size_t>(si)].first;
                const float under = starts[static_cast<std::size_t>(si)].second;
                // The radii tried: the line's (never thicker than what it starts on), and while the
                // weapon reaches far past the shape, the thick ones (carried when thicker).
                const int options =
                    p.fat && worst > 2.0f * p.poke_ok ? 1 + static_cast<int>(p.fat_r.size()) : 1;
                for (int o = 0; o < options; ++o) {
                    const float r =
                        o == 0 ? std::min(p.r, under) : p.fat_r[static_cast<std::size_t>(o - 1)];
                    const bool carry = o > 0 && r > under;
                    Vec3 start = s;
                    if (p.extend_grip && fit.count == 0 && r <= p.hand_r) {
                        const Vec3 axis = Body::Normalize(q);
                        float back = 0.0f;
                        for (const Vec3 v : verts) {
                            const float t = Body::Dot(v, axis);
                            if (t < back && Body::Length(v - axis * t) <= r + p.slack) back = t;
                        }
                        start = axis * std::min(0.0f, back + r);
                    }
                    const Vec3 sq = q - start;
                    const float len = Body::Length(sq);
                    if (len - r < p.min_len) {
                        continue;
                    }
                    const Vec3 e = q - sq * (r / len);
                    const float cand_air =
                        p.fat ? solid.AirLength(nearby, start, e) : grid.AirLength(start, e);
                    if (cand_air > p.air_max) {
                        continue;
                    }
                    // Its reach: the shape's support with this capsule, which must not reach past
                    // the weapon by more than fat_cap where the shape did not already (a thick
                    // capsule on a thin blade would stop it short of every wall).
                    bool fits = true;
                    for (int d = 0; d < Dirs && fits; ++d) {
                        const Vec3 n = dirs[static_cast<std::size_t>(d)];
                        const float here = std::max(Body::Dot(start, n), Body::Dot(e, n)) + r;
                        const float was = shape[static_cast<std::size_t>(d)];
                        fits =
                            here <= reach[static_cast<std::size_t>(d)] + p.fat_cap || here <= was;
                        trial[static_cast<std::size_t>(d)] = std::max(was, here);
                    }
                    if (!fits) {
                        continue;
                    }
                    float cw = 0.0f, cm = 0.0f, cf = 0.0f;
                    gaps(trial, cw, cm, cf);
                    const float j_reach = cw + 0.5f * cm + fat_w * cf +
                                          p.air_weight * (air + cand_air) +
                                          p.carried_cost * static_cast<float>(carried + carry);
                    // Cannot beat the best even with the whole surface covered: skip the samples.
                    if (j_reach >= best_j) {
                        continue;
                    }
                    if (o > 0 && solid.Within(fill, start, e, r) < p.fat_share) {
                        continue;
                    }
                    const Vec3 ab = e - start;
                    const float l2 = Body::LengthSq(ab);
                    const float inv = l2 > 1e-12f ? 1.0f / l2 : 0.0f;
                    float gain = 0.0f;
                    for (std::size_t i = 0; i < c.size(); ++i) {
                        if (dist[i] > p.slack && SegDist(c[i], start, ab, inv) - r <= p.slack) {
                            gain += w[i];
                        }
                    }
                    const float j = j_reach + 0.15f * (1.0f - (covered + gain) / wsum);
                    if (j < best_j) {
                        best_j = j;
                        best = {start, e, r, carry};
                        best_air = cand_air;
                        found = true;
                    }
                }
            }
        }
        if (!found) {
            break;
        }
        fit.caps[static_cast<std::size_t>(fit.count++)] = best;
        air += best_air;
        carried += best.carried ? 1 : 0;
        const Vec3 ab = best.b - best.a;
        const float l2 = Body::LengthSq(ab);
        const float inv = l2 > 1e-12f ? 1.0f / l2 : 0.0f;
        covered = 0.0f;
        for (std::size_t i = 0; i < c.size(); ++i) {
            dist[i] = std::min(dist[i], SegDist(c[i], best.a, ab, inv) - best.r);
            if (dist[i] <= p.slack) {
                covered += w[i];
            }
        }
        for (int d = 0; d < Dirs; ++d) {
            const Vec3 n = dirs[static_cast<std::size_t>(d)];
            shape[static_cast<std::size_t>(d)] =
                std::max(shape[static_cast<std::size_t>(d)],
                         std::max(Body::Dot(best.a, n), Body::Dot(best.b, n)) + best.r);
        }
        gaps(shape, worst, mean, fat);
        j_now = best_j;
    }
    fit.poke = std::max(worst, 0.0f);
    fit.coverage = covered / wsum;
    fit.air = air;
    fit.fat = fat;
    fit.score = worst + 0.5f * mean + p.fat_weight * fat + 0.15f * (1.0f - fit.coverage) +
                p.air_weight * air + p.carried_cost * static_cast<float>(carried);
    return fit;
}

} // namespace Detail

// The thin outline, and with `fat` the fit with thick capsules too when it scores better without
// the weapon reaching further past it (2 mm): a solid head gets thick capsules, every other weapon
// keeps its outline.
inline Fit FitCapsules(std::span<const Vec3> verts, std::span<const std::uint32_t> tris,
                       const Params& p = {}) {
    Params thin = p;
    thin.fat = false;
    const Fit outline = Detail::FitOnce(verts, tris, thin);
    if (!p.fat) {
        return outline;
    }
    const Fit thick = Detail::FitOnce(verts, tris, p);
    return thick.score < outline.score && thick.poke <= outline.poke + 0.002f ? thick : outline;
}

} // namespace VR::WeaponFit
