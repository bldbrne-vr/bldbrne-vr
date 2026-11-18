// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>

#include "common/types.h"

// Less shimmer on shiny details (2026-11-11, the plan after the 0.9.5 numbers:
// "specular anti-aliasing (less shimmer)"): fine metal edges, wet stone and ornaments sparkle
// and crawl in the headset, because a highlight narrower than a pixel lands on one pixel this
// frame and on its neighbour the next. The game lights its G-buffer in screen-space passes that
// all read the surface's gloss g the same way: the lights' specular power is exp2(13 g) (the
// light pre-pass, GXLightAcc_*), the ambient composite's reflection mip 6.5 - 6.5 g and its
// Fresnel g^2 (0x3fe9e964). SpecularAaPass (specular_aa_pass.cpp) gives each of those programs
// VrSpecularAa's gloss instead: the lobe widened by how much the G-buffer normal varies across
// the pixel (Tokuyoshi and Kaplanyan, "Improved geometric specular antialiasing", 2019), so the
// hundreds of material programs stay as they are. A flat puddle keeps its sharp highlight; a
// bumpy wet street far away turns from sparkles into a sheen, as a filtered normal map would.
namespace Shader::Optimization::SpecularAa {

// Startup policy (VR_SPEC_AA=0|1 overrides it): in VR with the New graphics page's Shading on
// (vr.ini ambient_occlusion: "Game's" turns every one of these looks off), with or without an
// upscaler (it is cheap, and an upscaler's smaller input makes the sparkle worse).
inline bool Enabled(bool vr, bool setting, const char* override_value) {
    return override_value ? override_value[0] != '0' : vr && setting;
}

// The game's light programs (every GXLightAcc_*.ppo of the shader archive; the deferred ones
// run) and its ambient composite 0x3fe9e964. Sorted: binary search, BuildRuntimeInfo runs twice
// a draw.
inline constexpr std::array<u32, 87> LightingPrograms{
    0x02aee188u, 0x02cd8f9bu, 0x07aa7c3eu, 0x07d07cf3u, 0x093a99d8u, 0x0a8a518bu, 0x0b2ace4cu,
    0x0ef50047u, 0x149432ffu, 0x186d69d5u, 0x1ac8be70u, 0x201cb43du, 0x205c82b3u, 0x22b357f3u,
    0x294503bau, 0x2994a496u, 0x2aaa5cb1u, 0x3105494cu, 0x31b90cc5u, 0x370174f7u, 0x376e0a37u,
    0x3aafbe57u, 0x3b397883u, 0x3cd2ac34u, 0x3fe9e964u, 0x40727870u, 0x428a80bfu, 0x42bd4804u,
    0x43d26ee3u, 0x46323550u, 0x505ebdbeu, 0x57a66dedu, 0x5d7150dau, 0x5de04266u, 0x5e210809u,
    0x5f76eb3du, 0x600f86b0u, 0x65ff4c81u, 0x6ab6cecbu, 0x6b42fc89u, 0x761d2013u, 0x799bfa32u,
    0x7b33e949u, 0x7cc2c044u, 0x82951fa5u, 0x896e7242u, 0x89f9ee3eu, 0x8aa6908eu, 0x8d5387d9u,
    0x8e4d8f54u, 0x8ea5be99u, 0x98506debu, 0x9f9ef267u, 0xa22c7f71u, 0xa422d554u, 0xb3c63c01u,
    0xb464a1f2u, 0xb47e0972u, 0xb59b4c4eu, 0xb5b52d63u, 0xba81fc0au, 0xbae5454eu, 0xbb2ac1cbu,
    0xbcf67c52u, 0xc023d70fu, 0xc12ac9e5u, 0xc37e6323u, 0xc3eb1d3au, 0xc44db5d0u, 0xc6edb19au,
    0xd22cbfd1u, 0xd3143e39u, 0xd3a7b5a0u, 0xd75fe2ffu, 0xd776fb27u, 0xd92266b9u, 0xdb92e75cu,
    0xdea6db05u, 0xe1610d5bu, 0xe4daeaaeu, 0xe72d12aeu, 0xe96447c3u, 0xea36a1b0u, 0xecda90f6u,
    0xf2531727u, 0xf40da260u, 0xf81596cdu,
};
static_assert(std::ranges::is_sorted(LightingPrograms));

inline bool IsLightingProgram(u64 hash) {
    return hash <= 0xffffffffull &&
           std::ranges::binary_search(LightingPrograms, static_cast<u32>(hash));
}

// The widening: n = exp2(13 g), alpha^2 = 2 / (n + 2) (Blinn-Phong's power as a Beckmann
// roughness), plus k = Strength * clamp(2 Sigma2 (|dn/dx|^2 + |dn/dy|^2) - Floor, 0, Kappa),
// back to a gloss g' = log2(2 / (alpha^2 + k) - 2) / 13, never above g. Sigma2 and Kappa are the
// paper's; Floor is two steps of the G-buffer's 8-bit normal (its rounding alone would cut a
// mirror's power along contour lines); EdgeRel: an axis whose view depth changes by more than
// that share of the pixel's across the pixel crosses a silhouette and adds nothing (a floor at
// eye height stays under it out to ~140 m, a body in front of a wall is far above it).
inline constexpr float Sigma2 = 0.25f;
inline constexpr float Kappa = 0.18f;
inline constexpr float Floor = 6e-5f;
inline constexpr float EdgeRel = 0.10f;
inline constexpr float Strength = 1.0f;

// Tests: VR_SPEC_AA_STRENGTH and VR_SPEC_AA_KAPPA replace Strength and Kappa (read once, when a
// shader is compiled: a mock run's fresh cache); VR_SPEC_AA_DEBUG=1 adds DebugScale * (g - g')
// to each light's specular red (red where the gloss was widened, under the lights).
inline constexpr float DebugScale = 4.0f;

} // namespace Shader::Optimization::SpecularAa
