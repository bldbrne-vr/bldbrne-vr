// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <span>
#include <vector>

#include "common/types.h"

// A map's placed lights, read from its own light file (map/mAA_BB_00_00/mAA_BB_00_00_0000.btl.dcx,
// unpacked: "BTL" version 2), for the fog the lamps light (2026-11-16: the fog grid took its lamps
// from the frame's light draws alone, the air's list, which leaves out every light that reaches
// further than 5.75 m as a fill light: the lit wall lantern in Central Yharnam's alley, 6 m, 2 m
// from the fog, lit none of it; and a lamp the game is not drawing lit nothing).
//
// The file: six dwords (the version 2, 2, how many lights, the names' bytes, 0, a light's bytes:
// 200), the lights' names, then the lights at the file's end. A light: 0x10 its name's offset,
// 0x18 its kind (0 a point light, 1 a spot, 2 a directional one), 0x1C on (a byte), 0x1D its
// colour (three bytes), 0x20 the colour's power, 0x38 its place (x y z), 0x58 its radius (m).
namespace VR::MapLights {

struct Light {
    s32 kind;      // 0 point, 1 spot, 2 directional
    bool on;
    float x, y, z; // the world's
    float radius;  // m
    float r, g, b; // the colour times its power
};

inline constexpr u32 HeaderBytes = 24;
inline constexpr u32 LightBytes = 200;
inline constexpr u32 MaxLights = 4096;

// The lights of an unpacked file. False: not a file this understands (nothing added).
inline bool Read(std::span<const u8> btl, std::vector<Light>& out) {
    const auto dword = [&](std::size_t at) {
        s32 v = 0;
        std::memcpy(&v, btl.data() + at, 4);
        return v;
    };
    if (btl.size() < HeaderBytes || dword(0) != 2) {
        return false;
    }
    const s32 count = dword(8), size = dword(20);
    if (count < 0 || static_cast<u32>(count) > MaxLights || size != static_cast<s32>(LightBytes) ||
        btl.size() < HeaderBytes + static_cast<std::size_t>(count) * LightBytes) {
        return false;
    }
    const std::size_t first = btl.size() - static_cast<std::size_t>(count) * LightBytes;
    for (s32 i = 0; i < count; ++i) {
        const u8* const p = btl.data() + first + static_cast<std::size_t>(i) * LightBytes;
        float power, place[3], radius;
        s32 kind;
        std::memcpy(&kind, p + 0x18, 4);
        std::memcpy(&power, p + 0x20, 4);
        std::memcpy(place, p + 0x38, 12);
        std::memcpy(&radius, p + 0x58, 4);
        if (!std::isfinite(power) || !std::isfinite(place[0]) || !std::isfinite(place[1]) || !std::isfinite(place[2]) ||
            !std::isfinite(radius)) {
            continue;
        }
        out.push_back(Light{kind, p[0x1C] != 0, place[0], place[1], place[2], radius, float(p[0x1D]) / 255.0f * power,
                            float(p[0x1E]) / 255.0f * power, float(p[0x1F]) / 255.0f * power});
    }
    return true;
}

// A lamp, lantern, candle or fire (against the game's fill lights, placed to brighten whole areas:
// the moonlight's blues, the wide warm washes): a point light that is on, warm (its red over its
// blue by WarmShare, light_shadow.h's line), bright enough, and of a lamp's reach.
inline constexpr float WarmShare = 1.15f;
inline constexpr float MinLuminance = 0.6f;
inline constexpr float MinRadiusM = 1.0f;
inline constexpr float MaxRadiusM = 8.0f;
inline bool IsLamp(const Light& l) {
    return l.kind == 0 && l.on && l.radius >= MinRadiusM && l.radius <= MaxRadiusM && l.r > WarmShare * l.b &&
           0.3f * l.r + 0.6f * l.g + 0.1f * l.b >= MinLuminance;
}

// The lamps of every map read at start (core/vr/bloodborne_woods_shadows.cpp, before the first
// frame; read by the fog grid's side of the rasterizer).
struct Lamp {
    u16 map; // (area << 8) | block
    Light light;
};
inline std::vector<Lamp>& Lamps() {
    static std::vector<Lamp> lamps;
    return lamps;
}

// How far a lamp's light gets before a wall (2026-11-16, the user: "could it be, if a light
// actually reaches that fog?"; until then a lamp lit all the fog within its radius, the fog behind
// the wall it hangs on too). The game's own collision world is asked once a lamp, from the lamp
// outwards along Rays (the six axes of the world, then the eight corners between them): each
// ray's free length (m, at most the lamp's radius). From them one free length along each of the
// six axes (+x -x +y -y +z -z): the mean of the axis' own ray and the middle one of the five on its
// side (its own and the four corner rays): a wall the lamp hangs on is short on all five; along the
// wall's face two corner rays run into the wall and the middle one is still free; a thin post
// straight ahead is one short ray and the light goes on past it. A place's reach then lies between the
// axes' by the squares of its direction's parts (ReachAlong; the fog grid's shader does the same).
inline constexpr u32 Rays = 14;
inline constexpr std::array<std::array<float, 3>, Rays> RayDirections{{
    {1.0f, 0.0f, 0.0f},
    {-1.0f, 0.0f, 0.0f},
    {0.0f, 1.0f, 0.0f},
    {0.0f, -1.0f, 0.0f},
    {0.0f, 0.0f, 1.0f},
    {0.0f, 0.0f, -1.0f},
    {0.57735f, 0.57735f, 0.57735f},
    {0.57735f, 0.57735f, -0.57735f},
    {0.57735f, -0.57735f, 0.57735f},
    {0.57735f, -0.57735f, -0.57735f},
    {-0.57735f, 0.57735f, 0.57735f},
    {-0.57735f, 0.57735f, -0.57735f},
    {-0.57735f, -0.57735f, 0.57735f},
    {-0.57735f, -0.57735f, -0.57735f},
}};
// A ray starts this far out from the lamp's own place (its fixture and the wall it is bolted to
// are not what its light meets first on the open side).
inline constexpr float RayStartM = 0.3f;

inline std::array<float, 6> AxisReach(const std::array<float, Rays>& free, float radius) {
    std::array<float, 6> out{};
    for (u32 axis = 0; axis < 6; ++axis) {
        const u32 part = axis / 2;
        const float sign = axis % 2 == 0 ? 1.0f : -1.0f;
        std::array<float, 5> mine{};
        u32 n = 0;
        mine[n++] = free[axis];
        for (u32 r = 6; r < Rays; ++r) {
            if (RayDirections[r][part] * sign > 0.0f) {
                mine[n++] = free[r];
            }
        }
        // (Its own ray's, the wall it meets: no light past a wall straight ahead. Until the
        // evening of 2026-11-16 the mean of its own and the median of the five: a flat wall 0.50 m
        // off gave 0.68 m, light in the fog behind it. Only where every corner ray on its side
        // goes on at least twice as far along the axis, and the thing is in the nearer half of
        // the radius, is it a post or a bracket and not a wall: then the median of the corners'.)
        const float own = mine[0];
        bool post = own < 0.5f * radius && n > 1;
        for (u32 k = 1; k < n; ++k) {
            // (A corner ray that met nothing: the radius along the axis; one that did: its
            // length's part along the axis.)
            mine[k] = mine[k] < radius - 1e-3f ? mine[k] * 0.57735f : radius;
            post = post && mine[k] >= 2.0f * own;
        }
        float reach = own;
        if (post) {
            std::sort(mine.begin() + 1, mine.begin() + n);
            reach = mine[1 + (n - 1) / 2];
        }
        out[axis] = std::clamp(reach, 0.0f, radius);
    }
    return out;
}

// The reach along a direction (any length) from the six axes' free lengths.
inline float ReachAlong(const std::array<float, 6>& axes, float x, float y, float z) {
    const float l2 = x * x + y * y + z * z;
    if (!(l2 > 1e-12f)) {
        return std::min({axes[0], axes[1], axes[2], axes[3], axes[4], axes[5]});
    }
    // (The box of the six lengths: how far the direction goes before it leaves the box. A wall
    // across an axis stops every direction at the wall; mixed by the squares of the direction's
    // parts, as it was at first, a slanted direction took most of its length from the open axes
    // and went through.)
    const float l = std::sqrt(l2);
    float t = 1e9f;
    const float parts[3] = {x, y, z};
    for (u32 c = 0; c < 3; ++c) {
        const float along = std::abs(parts[c]);
        if (along > 1e-6f * l) {
            t = std::min(t, axes[2 * c + (parts[c] > 0.0f ? 0 : 1)] / along);
        }
    }
    return l * t;
}

// Each lamp's free lengths (as Lamps() lists them), measured by the game's loop thread when the
// hunter first comes near (core/vr/bloodborne_woods_shadows.cpp FogLampReachTick), read by the fog
// grid's side of the rasterizer: under ReachMutex.
struct Reach {
    std::array<float, 6> axes{};
    bool measured{};
};
inline std::mutex& ReachMutex() {
    static std::mutex mutex;
    return mutex;
}
inline std::vector<Reach>& Reaches() {
    static std::vector<Reach> reaches;
    return reaches;
}

} // namespace VR::MapLights
