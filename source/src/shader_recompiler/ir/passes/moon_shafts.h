// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstdlib>

#include "common/types.h"

// Moon shafts (2026-11-09: "AAA 2026 lighting"; volumetric moonlight): the moon's (or
// the evening sun's) light scattered by the air along each pixel's view ray, shadowed by the
// game's own moon shadow atlas: beams through windows and between trees, moonlit mist in the
// streets with dark pockets where the buildings shadow it.
//
// The rasterizer takes the moon's shadow from the game's screen-space shadow pass (PCF9,
// IsShadowProgram): its cascades336 buffer (inverse P at dwords 0..15, four screen-to-shadow
// matrices at 16..79, the cascades' split depths at 80..82, NativeProjection::RecognizeLighting)
// and its shadow atlas (a 4096x4096 depth image of four cascades), and the moon's colour from the
// directional light pass (IsDirLightProgram: its buffer 0 is a table of light sets of
// SetDwords dwords, light 0's direction in vec4 0 and its colour in vec4 4; light 0 is the one the
// shadow mask darkens; the GPU writes the table). Rasterizer::VrMoonUpload hands the light
// composite (BounceLight::IsCompositeProgram) the cascades as view-to-shadow rows of the flat
// camera's view and the moon's direction in a buffer of its own (Shader::BufferType::
// VrMoonShafts), the atlas as a host image (Shader::VrHostImageBase + VrHostImage::MoonAtlas)
// and the table as a host buffer (VrHostBuffer::MoonSets): the composite reads the light sets
// (255 * w of the image its own per-set table is indexed by) of the surfaces at ColourPoints for
// one colour of the air. BouncePass (light_shadow_pass.cpp) adds VrMoonShafts to the composite's
// colour, in the scene's own HDR light before the tone map, and dims that colour by the air's
// transmittance: Steps points along the ray (denser near the eye, jittered per pixel and per
// frame: the upscaler's accumulation smooths the noise), each one lit or in the atlas' shadow,
// times the air's transmittance, a forward-scattering phase (Henyey-Greenstein PhaseG, mixed
// with an even share) and the density. (Light 0's direction in the table is not in the view's
// space: the moon's direction comes from the cascades.)
namespace Shader::Optimization::MoonShafts {

// Startup policy (vr.ini moon_shafts, VR_MOON_SHAFTS=0|1 overrides it): with an upscaler or
// without (light_shadow.h Enabled).
inline bool Enabled(bool vr, bool setting, const char* override_value) {
    return override_value ? override_value[0] != '0' : vr && setting;
}

// The game's screen-space cascaded shadow pass (GXScreenSpaceCascadedShadow_PCF9 and its unused
// _Soft variant): the cascades and the atlas are taken at its draws.
inline constexpr bool IsShadowProgram(u64 hash) {
    return hash == 0xf55643deu || hash == 0xb89e316bu;
}
// The directional light pass (DDirLightShadowed1): its light-set table.
inline constexpr bool IsDirLightProgram(u64 hash) {
    return hash == 0xd92266b9u;
}
inline constexpr u32 SetDwords = 72;
inline constexpr u32 SetColour = 4;    // vec4 index of light 0's colour
inline constexpr u32 TableSets = 256;  // the table's sets at most (the id is a byte)

// Quality (vr.ini moon_shafts_quality, VR_MOON_SHAFTS_QUALITY=0|1|2): points per ray. Read at
// run time from the buffer: no recompile.
// (Low: 6 as in 0.9.6; 8 for a day, 2026-11-18. The user that evening: "for 12gb cards lets do 6
// rays... like we had it on 0.9.6".)
inline constexpr u32 QualitySteps[3] = {6, 10, 16};

// The air: Density per metre (scattering and extinction; VR_MOON_SHAFTS_DENSITY), lit up to FarM
// from the eye (thinning out over its second half: a haze round you, not a wall over the distant
// city, whose painted sky gets none), Henyey-Greenstein PhaseG for ForwardShare of the light (the
// rest even), the whole times Strength. A shadow test needs the point BiasM behind the atlas'
// depth.
// (2026-11-11, "moon beams": was 0.006, 30 m, g 0.3 with half the light even, 0.30 - a flat
// grey veil everywhere. All of it forward now, so the air glows towards the moon and where its
// light comes through gaps (moonlit mist through the Dream's trees), only 12 m of it (the distant
// city against the painted sky stays clean); g 0.6, not 0.75, which put a glow blob over the
// Tomb's buildings.)
// (2026-11-14 midday, the user after the moon glow tour (base / ours / LanternEngine's numbers in
// six places): "LanternEngine's way". Ours lit what stood near far more than the sky beside it (a
// glowing tree against a dark sky), sat in a lobe that showed indoors, and aimed at a "moon" that
// was a cloud. LanternEngine's volumetric light: along the shadow light, g 0.4, 40 m of thicker
// but far weaker air, the sky lit as much as anything, light only added, never taken. So: 0.025
// per metre over 40 m, g 0.4, strength 0.05 (was 0.009, 12 m, 0.6, 0.40).)
// (2026-11-14 evening, the user after pictures of Cathedral Ward at four strengths (the full one
// a grey wash over a dark place, hiding the game's own ground fog): "moon glow down to a quarter
// ... But have the moon glow impact trees a tiny bit like how we had it before, just a tiny bit."
// A quarter of it on the sky; the lit air 16 m deep again instead of 40, so what stands 16 m off
// and more has all of it in front and the sky behind keeps FarKeep 0.75 of that: trees and rocks
// about a third more than the sky beside them. Strength 0.033 for that quarter (the shorter air
// gathers less).)
inline constexpr float Density = 0.025f;
inline constexpr float FarM = 16.0f;
inline constexpr float PhaseG = 0.4f;
// The phase's direction is the visible moon (core/vr/moon_screen.h: the game's light shafts'
// centre, kept in the moon's light space while they are not drawn, GlowHoldMs), not its shadow
// light's (BBFSS: about 25 degrees above the drawn sun in Central Yharnam; ours, 2026-11-12, a
// sharp test phase at the Central Yharnam start: 33 to 34 degrees from the drawn sun, above and to
// its left). A visible moon more than GlowMaxDeg from the shadow light is taken for something
// else (the shadow light's then). TESTS ONLY (VR_MOON_GLOW_SCREEN=1): the game drew its light
// shafts in none of the test views, so the screen point's convention is unverified.
inline bool GlowFromScreen() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_MOON_GLOW_SCREEN");
        return env != nullptr && env[0] == '1';
    }();
    return on;
}
inline constexpr long long GlowFreshMs = 250;
inline constexpr long long GlowHoldMs = 120000;
inline constexpr double GlowMaxDeg = 50.0;
inline constexpr float ForwardShare = 1.0f;
// (0.045 the same evening, the user: "Do a bit more than a quarter everywhere else"; was 0.033.)
inline constexpr float Strength = 0.045f;
// ... and by the game's area (core/vr/game_area.h; the user the same evening: "maybe we do it
// full strength in the woods, and quarter every where else?"): in an area of AreaFull the air as
// it was played there, WoodsStrength over WoodsFarM (the day's first numbers, LanternEngine's);
// elsewhere Strength over FarM. Eased over AreaEaseS at a border. Only while VR_MOON_SHAFTS_STRENGTH
// and _FAR are not given (tests keep their own numbers).
inline constexpr bool AreaFull(u32 area) {
    return area == 27; // the Forbidden Woods
}
inline constexpr float WoodsStrength = 0.05f;
inline constexpr float WoodsFarM = 40.0f;
inline constexpr float AreaEaseS = 4.0f;
// A surface beyond FarM keeps less of the lit air in front of it, down to FarKeep from FarFade
// times FarM on (the distant city stays close to the painted sky beside it, which gets none).
inline constexpr float FarFade = 4.0f;
// (0.8 since 2026-11-14 midday, was 0.35; LanternEngine's is 1. The user: "it's ok if there's a
// tiny bit of more glow on trees or rocks, but a tiny bit ... our thing that added more, we can
// keep it but gently".)
// (0.75 since the evening of 2026-11-14, with the 16 m air: see Density.)
inline constexpr float FarKeep = 0.75f;
// The air thickens from NearStartM to NearFullM (what is at your hands stays clear), and dims
// the surface behind it by ExtinctionShare of the density over its path (shadowed air reads as
// dark pockets, lit air as beams).
inline constexpr float NearStartM = 1.5f;
inline constexpr float NearFullM = 6.0f;
// (0 since 2026-11-14 midday, LanternEngine's way: the air only adds light; was 1.5.)
inline constexpr float ExtinctionShare = 0.0f;
// Tests (read when the shaders are made): VR_MOON_SHAFTS_FAR_KEEP (FarKeep; 1: far surfaces keep
// all the lit air in front of them) and VR_MOON_SHAFTS_EXTINCTION (ExtinctionShare; 0: the air
// only adds light, as LanternEngine's volumetric light does).
inline float FarKeepKnob() {
    static const float v = [] {
        const char* const env = std::getenv("VR_MOON_SHAFTS_FAR_KEEP");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.0f, 1.0f) : FarKeep;
    }();
    return v;
}
inline float ExtinctionKnob() {
    static const float v = [] {
        const char* const env = std::getenv("VR_MOON_SHAFTS_EXTINCTION");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.0f, 8.0f) : ExtinctionShare;
    }();
    return v;
}
// The moon's colour for the whole air: the light sets of the surfaces at these screen points
// (ndc), weighted by 1 / distance^2, those within NearIgnoreM (the hunter's own hands) left out.
inline constexpr float NearIgnoreM = 1.5f;
inline constexpr float ColourPoints[6][2] = {{-0.6f, -0.5f}, {0.0f, -0.5f}, {0.6f, -0.5f},
                                             {-0.6f, 0.5f},  {0.0f, 0.5f},  {0.6f, 0.5f}};
inline constexpr float BiasM = 0.15f;
// Frames without a shadow pass after which the composite lets go of the atlas (about 2 s).
inline constexpr u32 AtlasStaleFrames = 180;
inline constexpr float StartM = 0.25f;

// The buffer (dwords): a header of HeaderDwords, then the four cascades of CascadeDwords.
//  0 steps (u32; 0: no moon this frame, nothing marched)   1 frame (u32, the noise's)
//  2 the compare's sign (+1: lit while the point's depth <= the atlas', -1 the other way)
//  3 the eyes' half shift along view x (the fog lights' rule)   4 density   5 far
//  6 phase g   7 forward share   8..10 towards the moon (view, unit)   11 strength
//  12..14 a test's colour (VR_MOON_SHAFTS_COLOUR; else 0: the light sets' only)   15 0
//  16..19 the cascades' far ends in view z (split 0..2, then the last's end)
//  20..21 the atlas' size in texels (float)   22 the light-set table's sets (u32)   23 0
//  per cascade k at 24 + 16 k: rows 0..2 of its view-to-shadow (12: uv x, uv y, depth; each
//  row x y z w over (view, 1)), its tile's corner (2, atlas uv), the tile's size (1), the bias in
//  the atlas' depth units (1).
inline constexpr u32 HeaderDwords = 24;
inline constexpr u32 CascadeDwords = 16;
inline constexpr u32 Cascades = 4;
inline constexpr u32 BufferDwords = HeaderDwords + Cascades * CascadeDwords;

} // namespace Shader::Optimization::MoonShafts
