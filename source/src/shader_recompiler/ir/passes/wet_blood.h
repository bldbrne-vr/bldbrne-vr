// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>

#include "common/types.h"

// Wet, glistening blood (2026-11-11, the plan after the 0.9.5 numbers: "wet
// glistening blood"): blood on enemies, on the hunter's sleeves and gloves should catch the
// lamps and the moon like a wet surface. The game blends it into its G-buffer itself: every
// character material has a _Bloody_ twin (GXFlver_*_Bloody_*_Gbuf) that lerps the surface's
// albedo, specular colour F0 and gloss towards a blood layer painted per character, by
// FC_BloodAmount (a material constant): f = B + A (X - B), X = W B + Y, where W is the painted
// blood texture's alpha (1 where there is none) and Y the blood's own share. The blood's
// coverage of the pixel is therefore A (1 - W), exactly. WetBloodPass (wet_blood_pass.cpp)
// raises the blended F0 and gloss towards wet values by that coverage, where they are computed,
// so the G-buffer, the material's own reflection and every light pass that reads them follow.
// Nothing else is touched: red cloth, carpets and wood keep their look.
namespace Shader::Optimization::WetBlood {

// Startup policy (VR_WET_BLOOD=0|1 overrides it): in VR with the New graphics page's Shading on
// (vr.ini ambient_occlusion), with or without an upscaler.
inline bool Enabled(bool vr, bool setting, const char* override_value) {
    return override_value ? override_value[0] != '0' : vr && setting;
}

// The 40 _Bloody_ G-buffer pixel programs of the shader archive. Sorted (binary search).
inline constexpr std::array<u32, 40> BloodPrograms{
    0x0826a9b9u, 0x17a56942u, 0x1c4c9a9fu, 0x2140da2bu, 0x2382289bu, 0x2be973acu, 0x2ceb60eeu,
    0x3d4aa26du, 0x3f91507eu, 0x4b6a2ff4u, 0x4e4ec61bu, 0x50287a64u, 0x52738b8au, 0x552c87f4u,
    0x578969a9u, 0x6613daddu, 0x7090112du, 0x70bb42afu, 0x7532b462u, 0x761f73e3u, 0x8adc5eeeu,
    0x998b035eu, 0x99bcb69eu, 0x9abe2f09u, 0xa810c61bu, 0xa89e01e9u, 0xa94afc01u, 0xaa2a9925u,
    0xad824e7eu, 0xb2802f73u, 0xc1986f33u, 0xc2ea2369u, 0xc8e69132u, 0xd3ea6e1bu, 0xd655c9f1u,
    0xe21e283au, 0xe43bb329u, 0xed6dfb99u, 0xf4792c11u, 0xff0d534au,
};
static_assert(std::ranges::is_sorted(BloodPrograms));

inline bool IsBloodProgram(u64 hash) {
    return hash <= 0xffffffffull &&
           std::ranges::binary_search(BloodPrograms, static_cast<u32>(hash));
}

// Where blood covers the pixel fully, the gloss rises to at least GlossWet (the lights' power
// exp2(13 * 0.75) = 861, before each light's own size limit) and F0 to at least F0Wet (water is
// 0.02: "a bit more specular"), by Strength; values the game already made higher stay. The gloss
// is the material's before its own shininess scale, which still applies. Wet things look a
// little darker too: the albedo times 1 - WetDarken there.
inline constexpr float GlossWet = 0.75f;
inline constexpr float F0Wet = 0.035f;
inline constexpr float Strength = 0.85f;
inline constexpr float WetDarken = 0.1f;

// Tests: VR_WET_BLOOD_DEBUG=1 paints the coverage green in the albedo (read once, when a shader
// is compiled: a mock run's fresh cache).

} // namespace Shader::Optimization::WetBlood
