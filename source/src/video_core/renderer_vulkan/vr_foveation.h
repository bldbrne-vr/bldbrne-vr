// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

// Fixed foveated rendering (vr.ini foveation, VR_FOVEATED for tests): the eye-sized multiview scene passes
// take a VK_KHR_fragment_shading_rate attachment, full rate around each eye's optical axis and
// coarser towards the lens edges, which the lenses blur anyway. Only cards with attachment
// shading rates (NVIDIA Turing, AMD RDNA 2, Intel Arc and newer) use it; others render as before.
// VR_FOVEATED=1 takes the defaults, VR_FOVEATED=<inner>[,<outer>[,<ring>]] sets the radii (1 =
// across, half the image from the axis; up and down, the edge it runs to: Map): full rate inside
// `inner`, then a `ring` band at 2x1
// (half rate across, which hides the step to coarse shading), 2x2 out to `outer`, 4x4 beyond
// (where the card has it, else 2x2). Default inner radius 0.47, outer 1.05 (2026-11-05: every
// level a tiny bit lighter, the user; was 0.44 / 1.0); the transition band stays 0.15. 4x4 still only outside the half extent, which the
// lenses blur (2026-10-22 images: 4x4 well inside the visible field shows blocks on stone). The
// extension guarantees 1x2, 2x1 and 2x2 on every card that has it. These helpers are pure; the
// image and the attachment live in the scheduler (vk_scheduler.cpp).
namespace Vulkan::Foveation {

struct Settings {
    bool on{};
    float inner{0.47f};
    float outer{1.05f};
    float ring{0.15f};
};

inline Settings Parse(const char* value) {
    Settings s;
    if (!value || !*value || (value[0] == '0' && value[1] == '\0'))
        return s;
    s.on = true;
    if (value[0] == '1' && value[1] == '\0')
        return s;
    char* end{};
    const float inner = std::strtof(value, &end);
    if (end != value && std::isfinite(inner) && inner > 0.0f)
        s.inner = inner;
    if (end && *end == ',') {
        const char* rest = end + 1;
        const float outer = std::strtof(rest, &end);
        if (end != rest && std::isfinite(outer) && outer > 0.0f)
            s.outer = outer;
        if (end && *end == ',') {
            const char* ring_text = end + 1;
            const float ring = std::strtof(ring_text, &end);
            if (end != ring_text && std::isfinite(ring) && ring >= 0.0f)
                s.ring = ring;
        }
    }
    if (s.outer < s.inner)
        s.outer = s.inner;
    return s;
}

// vr.ini foveation (core/vr/graphics_settings.h): 0 off, 1 on (the defaults above),
// 2 performance (full rate inside 0.43, 2x2 out to 0.85, 4x4 beyond), 3 light (the user,
// 2026-11-01: full rate inside 0.63, the 2x1 ring, then 2x2 to the edges, never 4x4). Each was
// 0.03 tighter before 2026-11-05 (outer 0.8).
inline Settings FromMode(int mode) {
    Settings s;
    s.on = mode >= 1 && mode <= 3;
    if (mode == 2) {
        s.inner = 0.43f;
        s.outer = 0.85f;
    } else if (mode == 3) {
        s.inner = 0.63f;
        s.outer = 4.0f; // past every corner (1.41 at most): no 4x4
    }
    return s;
}

// The settings in force: VR_FOVEATED (tests) overrides vr.ini for the whole run; vr.ini without
// the key (-1, Auto) means on. Live (2026-11-01): the VR page's Picture > Performance changes
// vr.ini foveation and the next eye-sized pass takes the new level (vk_scheduler.cpp); Off
// leaves the attachment out, which shades at full rate. It still needs a card whose driver
// supports attachment shading rates and the restrictions vk_instance.cpp checks: other cards
// render as before.
inline Settings Choose(const char* env, int mode) {
    if (env && *env)
        return Parse(env);
    return FromMode(mode < 0 ? 1 : mode);
}

// Whether VR_FOVEATED fixes the level for the run (tests; "0" keeps the extension out entirely).
inline bool Fixed(const char* env) {
    return env && *env;
}

// The attachment's texel value for a fragment size (VK_KHR_fragment_shading_rate):
// log2(width) << 2 | log2(height).
constexpr std::uint8_t Rate(std::uint32_t width, std::uint32_t height) {
    return std::uint8_t((std::bit_width(width) - 1) << 2 | (std::bit_width(height) - 1));
}

// The optical axis in the image, from the eye's OpenXR angles (radians, left and down
// negative): where the frustum's tangent is zero, as a fraction from the left / top edge.
inline float AxisU(float angle_left, float angle_right) {
    const float l = std::tan(angle_left), r = std::tan(angle_right);
    return r > l ? -l / (r - l) : 0.5f;
}
inline float AxisV(float angle_up, float angle_down) {
    const float u = std::tan(angle_up), d = std::tan(angle_down);
    return u > d ? u / (u - d) : 0.5f;
}

// One layer of the attachment: ceil(width / texel_w) x ceil(height / texel_h) texels, the rate
// chosen by each texel centre's distance from the axis (cx, cy): across in half widths, up and down
// each in the distance from the axis to that edge (cy above it, 1 - cy below), so the top and the
// bottom edges are both 1 away whatever the axis' height. Quest's eyes see 44 degrees up and 55
// down (the axis 40% from the top): in half heights the bottom edge was 1.19 away, a wide 2x2 band
// and a 4x4 strip where the hands and the weapon are, the top edge 0.81, barely any (the user,
// 2026-10-26: "very strong towards the bottom, weak towards the top"). A centred axis: unchanged.
inline std::vector<std::uint8_t> Map(std::uint32_t width, std::uint32_t height,
                                     std::uint32_t texel_w, std::uint32_t texel_h, float cx,
                                     float cy, const Settings& s, bool four_by_four) {
    const std::uint32_t nx = (width + texel_w - 1) / texel_w;
    const std::uint32_t ny = (height + texel_h - 1) / texel_h;
    const float above = std::max(cy, 1e-3f), below = std::max(1.0f - cy, 1e-3f);
    std::vector<std::uint8_t> map(std::size_t(nx) * ny);
    for (std::uint32_t y = 0; y < ny; ++y) {
        for (std::uint32_t x = 0; x < nx; ++x) {
            const float u = (float(x) + 0.5f) * float(texel_w) / float(width);
            const float v = (float(y) + 0.5f) * float(texel_h) / float(height);
            const float dx = (u - cx) / 0.5f, dy = (v - cy) / (v < cy ? above : below);
            const float d = std::sqrt(dx * dx + dy * dy);
            map[std::size_t(y) * nx + x] = d < s.inner                    ? Rate(1, 1)
                                           : d < s.inner + s.ring         ? Rate(2, 1)
                                           : d < s.outer || !four_by_four ? Rate(2, 2)
                                                                          : Rate(4, 4);
        }
    }
    return map;
}

} // namespace Vulkan::Foveation
