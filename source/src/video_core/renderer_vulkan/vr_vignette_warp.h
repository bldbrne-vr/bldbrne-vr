// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "common/types.h"
#include "video_core/renderer_vulkan/vr_overlay_meshes.h"

// The vignettes drawn by the late warp (vr.ini vignette_in_warp): the hit flash and its wedge, the
// hit-stun hold, the stagger / knockdown black, the heal glow, the low-HP edge and the rally rim
// are blended into each eye in the late warp's last write (vr_eye_upscale.comp, VR_VIGNETTE)
// instead of being drawn into the VR overlay's own projection layer, which the compositor then
// blended over the whole field every refresh. Each one is the overlay's own draw item
// (VrOverlay::AppendVignette: its material and its ring's plane), shaded per eye pixel exactly as
// vr_overlay.frag shades the ring mesh: the eye pixel's ray meets the ring's plane (so the stereo
// depth is the overlay's, hud_depth_m), the profile there, then "over" in linear light (the
// overlay's premultiplied layer over the eyes). The ring mesh's rasterised profile is reproduced
// by interpolating its rings linearly in the plane's radius (exact along the mesh's spokes).
// Pure functions here, mirrored line by line in the shader (tests/test_vignette_warp.cpp).
namespace Vulkan::VrVignetteWarp {

namespace O = VrOverlay;

constexpr u32 MaxItems = 6;

// The ring mesh's rings (BuildMesh(MeshId::Vignette)): every 3 degrees from VignetteInnerDeg while
// below VignetteFullDeg + 4, then 66, 75 and VignetteOuterDeg (vr_eye_upscale.comp's
// VgSampleRing uses the same).
constexpr u32 RingCount = 15;
constexpr std::array<float, RingCount> RingDeg{26.0f, 29.0f, 32.0f, 35.0f, 38.0f,
                                               41.0f, 44.0f, 47.0f, 50.0f, 53.0f,
                                               56.0f, 59.0f, 66.0f, 75.0f, 85.0f};

// The mesh's per-vertex profile (aux.y) at a ring angle: a squared smoothstep from
// VignetteInnerDeg to VignetteFullDeg.
inline float RingProfile(float angle_deg) {
    const float t = std::clamp((angle_deg - O::VignetteInnerDeg) /
                                   (O::VignetteFullDeg - O::VignetteInnerDeg),
                               0.0f, 1.0f);
    const float s = t * t * (3.0f - 2.0f * t);
    return s * s;
}

struct RingSample {
    bool inside{false}; // on the mesh (between its first and last ring)
    float angle_deg{};  // aux.x * 90 as rasterised
    float profile{};    // aux.y as rasterised
};
// At `r` (the plane's radius: the tangent of the angle off the ring's axis at distance 1). The
// ring pair around r found from its angle (no table lookups: the shader would keep a table per
// invocation), then linear in r between their radii as the rasteriser interpolates the mesh.
inline RingSample SampleRing(float r) {
    // (A relative 1e-5 at both ends: the mesh's own float rounding of its first and last ring.)
    if (!(r >= std::tan(RingDeg[0] * O::DegToRad) * 0.99999f) ||
        r > std::tan(RingDeg[RingCount - 1] * O::DegToRad) * 1.00001f) {
        return {};
    }
    constexpr float Deg = 57.2957795f;
    const float a = std::atan(r) * Deg;
    float lo = 0.0f;
    float hi = 0.0f;
    if (a < 59.0f) {
        lo = 26.0f + 3.0f * std::floor(std::max(a - 26.0f, 0.0f) / 3.0f);
        hi = lo + 3.0f;
    } else if (a < 66.0f) {
        lo = 59.0f;
        hi = 66.0f;
    } else if (a < 75.0f) {
        lo = 66.0f;
        hi = 75.0f;
    } else {
        lo = 75.0f;
        hi = 85.0f;
    }
    const float ra = std::tan(lo * O::DegToRad);
    const float rb = std::tan(hi * O::DegToRad);
    const float f = std::clamp((r - ra) / (rb - ra), 0.0f, 1.0f);
    const float pa = RingProfile(lo);
    return {true, lo + (hi - lo) * f, pa + (RingProfile(hi) - pa) * f};
}

inline float SmoothStep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// One vignette (std140, 7 vec4): vr_eye_upscale.comp's vg_item[7 * i + k].
struct Item {
    std::array<float, 4> axis_x{}; // the ring plane's frame in local space (unit)
    std::array<float, 4> axis_y{};
    std::array<float, 4> axis_z{}; // its normal, toward the head
    std::array<float, 4> origin{};  // its centre; w = the distance (the mesh's scale)
    std::array<float, 4> color{};   // linear rgb; a = opacity (material alpha x fade)
    std::array<float, 4> params{};  // the material's params (x, y; z the power; w the mode)
    std::array<float, 4> wedge{};   // the material's emissive (x, y, power, focus; 0 = none)
};
static_assert(sizeof(Item) == 112);

// What the late warp's uniform block holds (std140): vr_eye_upscale.comp's VignetteData.
struct Block {
    std::array<u32, 4> head{};  // x the count, y 1 = the eyes hold B8G8R8A8 bytes
    std::array<float, 4> field{}; // degrees left, right, up, down (FrameConstants::field)
    std::array<std::array<float, 4>, 6> eye_rot{}; // eye e: columns 3e .. 3e + 2 (eye -> local)
    std::array<std::array<float, 4>, 2> eye_pos{}; // in local space
    std::array<std::array<float, 4>, 2> tan{}; // tan of angleLeft, angleRight, angleUp, angleDown
    std::array<Item, MaxItems> items{};
};
static_assert(sizeof(Block) == 864);

// The vignettes of one XR frame, as DrawOverlay made them, and the field the overlay would have
// fitted them to.
struct Frame {
    float health{}, desaturation{};
    u32 count{0};
    std::array<Item, MaxItems> items{};
    std::array<float, 4> field{};
};

// A vignette's draw item (VrOverlay::AppendVignette) as the warp draws it: false if it is not one.
inline bool FromDrawItem(const O::DrawItem& d, Item& out) {
    if (d.mesh != O::MeshId::Vignette || d.shade != O::Shade::Vignette) {
        return false;
    }
    const O::Vec3 c0 = d.model.Column(0);
    const O::Vec3 c1 = d.model.Column(1);
    const O::Vec3 c2 = d.model.Column(2);
    const O::Vec3 o = d.model.Column(3);
    const float s = std::sqrt(c0.x * c0.x + c0.y * c0.y + c0.z * c0.z);
    if (!(s > 1e-6f)) {
        return false;
    }
    out.axis_x = {c0.x / s, c0.y / s, c0.z / s, 0.0f};
    out.axis_y = {c1.x / s, c1.y / s, c1.z / s, 0.0f};
    out.axis_z = {c2.x / s, c2.y / s, c2.z / s, 0.0f};
    out.origin = {o.x, o.y, o.z, s};
    const auto& m = d.material;
    out.color = {m.base.x, m.base.y, m.base.z, m.base.w * std::clamp(m.fade, 0.0f, 1.0f)};
    out.params = {m.params.x, m.params.y, m.params.z, m.params.w};
    out.wedge = {m.emissive.x, m.emissive.y, m.emissive.z, m.emissive.w};
    return true;
}

inline float Dot3(const std::array<float, 4>& a, const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// Where the ray p + s d (s > 0) meets the item's plane, in the mesh's plane coordinates (x, y at
// distance 1: the tangents off the ring's axis). False: it does not (parallel, or behind).
inline bool PlaneHit(const Item& it, const float p[3], const float d[3], float& tx, float& ty) {
    const float denom = Dot3(it.axis_z, d);
    const float rel_p[3] = {p[0] - it.origin[0], p[1] - it.origin[1], p[2] - it.origin[2]};
    const float num = -Dot3(it.axis_z, rel_p);
    if (std::abs(denom) < 1e-8f) {
        return false;
    }
    const float s = num / denom;
    if (!(s > 0.0f)) {
        return false;
    }
    const float q[3] = {rel_p[0] + s * d[0], rel_p[1] + s * d[1], rel_p[2] + s * d[2]};
    tx = Dot3(it.axis_x, q) / it.origin[3];
    ty = Dot3(it.axis_y, q) / it.origin[3];
    return true;
}

// The item's opacity at plane coordinates (tx, ty): vr_overlay.frag's ShadeVignette over the
// rasterised ring mesh (no fragment outside it). `field`: FrameConstants::field.
inline float Alpha(const Item& it, float tx, float ty, const std::array<float, 4>& field) {
    const float r = std::sqrt(tx * tx + ty * ty);
    const RingSample ring = SampleRing(r);
    if (!ring.inside) {
        return 0.0f;
    }
    float profile = std::clamp(ring.profile, 0.0f, 1.0f);
    const int vmode = static_cast<int>(it.params[3] + 0.5f);
    if (vmode != 0) {
        const float angle = ring.angle_deg;
        if (vmode == 3) {
            const std::array<float, 4> f =
                field[0] > 1.0f ? field : std::array<float, 4>{54.0f, 54.0f, 44.0f, 55.0f};
            constexpr float Deg = 57.2957795f;
            const float ax = std::atan(tx) * Deg;
            const float ay = std::atan(ty) * Deg;
            const float qx = ax / std::max(ax < 0.0f ? f[0] : f[1], 1.0f);
            const float qy = ay / std::max(ay > 0.0f ? f[2] : f[3], 1.0f);
            profile = SmoothStep(it.params[0], it.params[1], std::sqrt(qx * qx + qy * qy));
        } else if (vmode == 1) {
            const float t = SmoothStep(it.params[0], it.params[1], angle);
            profile = t * t;
        } else {
            profile = SmoothStep(it.params[0] - 3.0f, it.params[0] + 1.0f, angle) *
                      (1.0f - SmoothStep(it.params[1] - 1.0f, it.params[1] + 3.0f, angle));
        }
    }
    if (it.params[2] != 1.0f) {
        profile = std::pow(profile, std::max(it.params[2], 0.01f));
    }
    if (it.wedge[3] > 0.0f) {
        const float c = r > 1e-6f ? (tx * it.wedge[0] + ty * it.wedge[1]) / r : 0.0f;
        const float wedge = std::pow(std::max(0.5f + 0.5f * c, 0.0f), std::max(it.wedge[2], 0.01f));
        profile *= (1.0f - it.wedge[3]) + it.wedge[3] * wedge;
    }
    return it.color[3] * profile;
}

inline float SrgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
inline float LinearToSrgb(float c) {
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// One eye pixel: `rgb` the stored (sRGB-encoded, the eyes' byte order) colour, `p` / `d` the
// pixel's ray in local space. Each vignette in order over it, in linear light (what the
// compositor did with the overlay's premultiplied layer over the eyes).
inline std::array<float, 3> Apply(const Frame& f, bool bgra, std::array<float, 3> rgb,
                                  const float p[3], const float d[3]) {
    if (f.count == 0) {
        return rgb;
    }
    // Decoded at the first vignette that reaches the pixel: one none reaches (the clear middle)
    // is left exactly as it was.
    std::array<float, 3> c = rgb;
    bool decoded = false;
    for (u32 i = 0; i < f.count && i < MaxItems; ++i) {
        const Item& it = f.items[i];
        float tx = 0.0f;
        float ty = 0.0f;
        if (!PlaneHit(it, p, d, tx, ty)) {
            continue;
        }
        const float a = std::clamp(Alpha(it, tx, ty, f.field), 0.0f, 1.0f);
        if (!(a > 0.0f)) {
            continue;
        }
        if (!decoded) {
            c = {SrgbToLinear(rgb[0]), SrgbToLinear(rgb[1]), SrgbToLinear(rgb[2])};
            decoded = true;
        }
        const std::array<float, 3> col =
            bgra ? std::array<float, 3>{it.color[2], it.color[1], it.color[0]}
                 : std::array<float, 3>{it.color[0], it.color[1], it.color[2]};
        for (u32 k = 0; k < 3; ++k) {
            c[k] = c[k] + (col[k] - c[k]) * a;
        }
    }
    if (!decoded) {
        return rgb;
    }
    return {LinearToSrgb(c[0]), LinearToSrgb(c[1]), LinearToSrgb(c[2])};
}

} // namespace Vulkan::VrVignetteWarp
