// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include "common/types.h"

// The ground under the air's mist, from the map itself (2026-11-17: "when I go down the
// stairs the fog seems to come/jump up momentarily", "why not make them be where [the ground] is
// placed and be done").
//
// The mist is a layer over the ground, and its ground was one level plane at the hunter's feet,
// eased over two seconds: down a flight of stairs his feet dropped under the plane (the mist came
// up round him), then the plane followed and every bit of mist in view sank with it. The ground is
// where it is: a lattice of the world (Cells x Cells cells of CellM, round the hunter, fixed to the
// world), each cell's ground height found by the game's own sphere cast against its static map.
// It grows outward from the cell he stands in, each cell looked for from a neighbour's height
// (from UpM over it to DownM under it): stairs and slopes are followed, the mist lies on each
// street at that street's height. Over a drop deeper than DownM, and where the cast starts inside
// the scenery (a house), the neighbour's height goes on: the layer runs level over a void and
// through a wall, as the one plane did everywhere.
//
// One level a place: under a bridge or in a cellar below a street the lattice may hold the level
// above. When he stands on a floor that is LevelM from the lattice's ground at his place it is
// begun anew from his own cell (the renderer eases what it shows, vr_fog_grid: no jump).
//
// The loop thread grows it (Grow, a few casts a tick) and publishes it; the renderer reads the
// published copy. VR_MIST_GROUND=0: the level plane, as it was.
namespace VR::MistGround {

inline constexpr int Cells = 32;
inline constexpr float CellM = 4.0f;
inline constexpr float UpM = 1.8f;
inline constexpr float DownM = 3.0f;
inline constexpr float StepUpM = 3.5f; // a start inside the scenery: looked for again from this much higher
inline constexpr float LevelM = 2.0f;
inline constexpr float LevelS = 0.4f;
inline constexpr u32 CastsATick = 10;

inline bool On() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_MIST_GROUND");
        return !(env != nullptr && env[0] == '0');
    }();
    return on;
}

enum class Cast { Unavailable, Clear, Hit, Inside };

inline int CellOf(float v) {
    return static_cast<int>(std::floor(v / CellM));
}
inline int Wrap(int v) {
    return ((v % Cells) + Cells) % Cells;
}

// The lattice: a cell of the world in the slot its coordinates wrap to; a slot holds one cell.
struct Lattice {
    std::array<float, Cells * Cells> y{};
    std::array<int, Cells * Cells> cx{}, cz{};
    std::array<bool, Cells * Cells> known{};
    float off_level_s{};

    static int Slot(int x, int z) {
        return Wrap(x) + Cells * Wrap(z);
    }
    bool Known(int x, int z) const {
        const int s = Slot(x, z);
        return known[s] && cx[s] == x && cz[s] == z;
    }
    float At(int x, int z) const {
        return y[Slot(x, z)];
    }
    void Set(int x, int z, float ground) {
        const int s = Slot(x, z);
        y[s] = ground;
        cx[s] = x;
        cz[s] = z;
        known[s] = true;
    }
    void Forget() {
        known.fill(false);
        off_level_s = 0.0f;
    }
    // The ground at a place: between the four cells round it that are known (false: none is).
    bool Ground(float wx, float wz, float& out) const {
        const float fx = wx / CellM - 0.5f, fz = wz / CellM - 0.5f;
        const int x0 = static_cast<int>(std::floor(fx)), z0 = static_cast<int>(std::floor(fz));
        const float tx = fx - static_cast<float>(x0), tz = fz - static_cast<float>(z0);
        float sum = 0.0f, weight = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const int x = x0 + (k & 1), z = z0 + (k >> 1);
            const float w = ((k & 1) ? tx : 1.0f - tx) * ((k >> 1) ? tz : 1.0f - tz);
            if (Known(x, z) && w > 1e-4f) {
                sum += At(x, z) * w;
                weight += w;
            }
        }
        if (weight <= 0.0f) {
            return false;
        }
        out = sum / weight;
        return true;
    }
};

// The window's cells by their distance from its middle, nearest first.
inline const std::array<std::array<s8, 2>, Cells * Cells>& NearestFirst() {
    static const std::array<std::array<s8, 2>, Cells * Cells> order = [] {
        std::array<std::array<s8, 2>, Cells * Cells> o{};
        for (int i = 0; i < Cells * Cells; ++i) {
            o[i] = {static_cast<s8>(i % Cells - Cells / 2), static_cast<s8>(i / Cells - Cells / 2)};
        }
        std::ranges::stable_sort(o, [](const auto& a, const auto& b) {
            return a[0] * a[0] + a[1] * a[1] < b[0] * b[0] + b[1] * b[1];
        });
        return o;
    }();
    return order;
}

// One tick of the lattice's growth round the hunter (his feet's place; `on_floor`: he stands on a
// floor, not on a ladder or in the air). `cast(x, y_from, y_to, z, hit_y)`: the map's ground on
// the way straight down from y_from to y_to at (x, z). Returns whether a cell changed.
template <typename CastFn>
bool Grow(Lattice& l, float hx, float hy, float hz, bool on_floor, float dt, CastFn&& cast) {
    const int mx = CellOf(hx), mz = CellOf(hz);
    const auto inside = [&](int x, int z) {
        return x >= mx - Cells / 2 && x < mx + Cells / 2 && z >= mz - Cells / 2 && z < mz + Cells / 2;
    };
    bool changed = false;
    // Another level than the lattice holds at his place: begun anew.
    if (float ground = 0.0f; on_floor && l.Ground(hx, hz, ground) && std::abs(ground - hy) > LevelM) {
        l.off_level_s += dt;
        if (l.off_level_s > LevelS) {
            l.Forget();
            changed = true;
        }
    } else {
        l.off_level_s = 0.0f;
    }
    u32 casts = 0;
    const auto centre = [](int c) { return (static_cast<float>(c) + 0.5f) * CellM; };
    // His own cell, from his feet.
    if (!l.Known(mx, mz)) {
        float hit = hy;
        const Cast c = cast(centre(mx), hy + UpM, hy - DownM, centre(mz), hit);
        if (c == Cast::Unavailable) {
            return changed;
        }
        ++casts;
        l.Set(mx, mz, c == Cast::Hit ? hit : hy);
        changed = true;
    }
    // Outward: each unknown cell next to a known one, the nearest to him first.
    for (const auto& o : NearestFirst()) {
        if (casts >= CastsATick) {
            break;
        }
        const int x = mx + o[0], z = mz + o[1];
        if (l.Known(x, z)) {
            continue;
        }
        // (From the known neighbour nearest to him.)
        static constexpr int Next[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        int best = -1, best_d = 1 << 30;
        for (int k = 0; k < 4; ++k) {
            const int nx = x + Next[k][0], nz = z + Next[k][1];
            const int d = (nx - mx) * (nx - mx) + (nz - mz) * (nz - mz);
            if (inside(nx, nz) && l.Known(nx, nz) && d < best_d) {
                best = k;
                best_d = d;
            }
        }
        if (best < 0) {
            continue;
        }
        const float from = l.At(x + Next[best][0], z + Next[best][1]);
        float hit = from;
        Cast c = cast(centre(x), from + UpM, from - DownM, centre(z), hit);
        ++casts;
        if (c == Cast::Inside) {
            c = cast(centre(x), from + UpM + StepUpM, from + UpM, centre(z), hit);
            ++casts;
        }
        if (c == Cast::Unavailable) {
            return changed;
        }
        l.Set(x, z, c == Cast::Hit ? hit : from);
        changed = true;
    }
    return changed;
}

// What the renderer reads: the window round the hunter (its low cell, row by row), a cell not
// known yet NaN.
struct Published {
    int x0{}, z0{};
    std::array<float, Cells * Cells> y{};
    u32 serial{}; // (0: nothing yet)
};
inline std::mutex& Mutex() {
    static std::mutex m;
    return m;
}
inline Published& Shared() {
    static Published p;
    return p;
}
inline void Publish(const Lattice& l, float hx, float hz) {
    const int x0 = CellOf(hx) - Cells / 2, z0 = CellOf(hz) - Cells / 2;
    std::scoped_lock lock{Mutex()};
    Published& p = Shared();
    p.x0 = x0;
    p.z0 = z0;
    for (int z = 0; z < Cells; ++z) {
        for (int x = 0; x < Cells; ++x) {
            p.y[z * Cells + x] = l.Known(x0 + x, z0 + z) ? l.At(x0 + x, z0 + z) : std::nanf("");
        }
    }
    ++p.serial;
    if (p.serial == 0) {
        p.serial = 1;
    }
}

// The renderer's own copy, eased: a cell's height goes to what is published at EaseMPerS at most
// (a lattice begun anew glides, it does not jump); a cell not known yet is at `fallback` (the
// level plane's height). Out: the window row by row, and its low corner.
inline constexpr float EaseMPerS = 3.0f;
struct Shown {
    Lattice l;
    u32 serial{};
    int x0{}, z0{};
    bool any{};
};
inline bool Show(Shown& s, float fallback, float dt, std::array<float, Cells * Cells>& out, float& corner_x,
                 float& corner_z) {
    Published p;
    {
        std::scoped_lock lock{Mutex()};
        p = Shared();
    }
    if (p.serial == 0) {
        return false;
    }
    const float step = EaseMPerS * std::clamp(dt, 0.0f, 0.25f);
    for (int z = 0; z < Cells; ++z) {
        for (int x = 0; x < Cells; ++x) {
            const int wx = p.x0 + x, wz = p.z0 + z;
            const float want = p.y[z * Cells + x];
            const float target = std::isnan(want) ? fallback : want;
            float shown = target;
            if (s.any && s.l.Known(wx, wz)) {
                const float had = s.l.At(wx, wz);
                shown = had + std::clamp(target - had, -step, step);
            }
            s.l.Set(wx, wz, shown);
            out[z * Cells + x] = shown;
        }
    }
    s.any = true;
    s.x0 = p.x0;
    s.z0 = p.z0;
    corner_x = static_cast<float>(p.x0) * CellM;
    corner_z = static_cast<float>(p.z0) * CellM;
    return true;
}

} // namespace VR::MistGround
