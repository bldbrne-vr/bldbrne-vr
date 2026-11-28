// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>

#include "common/types.h"

// Ambient occlusion (2026-11-09: "raytraced lighting games usually look really good like
// ambient lights also look SUPER ATMOSPHERIC AND REAL", then "Can we disable the game's SSAO so it
// doesn't take resources while we do our own?"). The game's screen-space AO pass (the
// gxposteffect ScreenSpaceAO2 family: 8 pairs of depth taps in a small fixed kernel) writes its
// occlusion to the red channel and the pixel's packed depth to the rest, for its blur and its
// ambient lighting pass. AmbientOcclusionPass (light_shadow_pass.cpp) puts VrAmbientOcclusion in
// that red channel instead: a search along Slices screen directions out to RadiusM (GTAO, Jimenez
// et al. 2016) whose samples mark the sectors of sky they hide (the visibility bitmask below),
// the cosine-weighted share of the sky each pixel still sees. The
// game's own loop then feeds nothing and the driver drops it: one AO, not two.
namespace Shader::Optimization::AmbientOcclusion {

// Startup policy (vr.ini ambient_occlusion, VR_AMBIENT_OCCLUSION=0|1 overrides it): with an
// upscaler or without, like the light shadows (light_shadow.h Enabled).
inline bool Enabled(bool vr, bool setting, const char* override_value) {
    return override_value ? override_value[0] != '0' : vr && setting;
}

// Our AO off but the lamp shadows' mask on (light_shadow.h SceneMaps::Mask, the light shadows on):
// the game's AO programs still carry ours, for the mask alone (RuntimeInfo::VrAoMaskOnly); the
// game's own AO stays.
inline bool MaskOnly(bool ours, bool light_shadows, bool mask) {
    return !ours && light_shadows && mask;
}

// The game's screen-space AO programs (the flag is set only for them; the pass checks the shape:
// a matrix-rebuilt view position and red = the occlusion, green = packed depth).
inline constexpr bool IsAoProgram(u64 hash) {
    return hash == 0xcbbca5dcu || hash == 0x83da8467u || hash == 0x7b1a6a02u ||
           hash == 0x63bf8617u;
}

// The game's upsample of a half-size AO (83da8467's: red = the occlusion, the rest packed depth)
// to the eye's size: 4 bilinear taps, each weighted by its depth's match and by 1 / its distance
// from the pixel, the distance taken from fract(uv * the half size). A pixel's centre never
// sits on a half-size texel's corner, but a 4x4 foveation block's centre (2 pixels in, on an
// even pixel edge) does: the distance is 0, the weight 1/0, and the block black (a 2x2 block's
// centre is half a texel in, fine). The only such program in every test run's shader cache
// (619 programs: the Chalice dungeons, the woods, Gascoigne, the lifts, Cainhurst...).
inline constexpr bool IsUpsampleProgram(u64 hash) {
    return hash == 0xc45c51cbu;
}

// The search: RadiusM around the pixel (metres), at least MinPixels and at most MaxPixels on
// screen, Steps per side along each of Slices directions (rotated per pixel and per frame),
// closer together near the pixel (fine contact, then the room's larger shapes).
inline constexpr float RadiusM = 1.6f;
// The visible share is raised to Power (the tester: "turn up the strength ... so I can clearly
// see them"): corners and crevices go darker, open ground stays as it was.
inline constexpr float Power = 1.8f;
inline constexpr float MinPixels = 6.0f;
inline constexpr float MaxPixels = 96.0f;
inline constexpr u32 Slices = 2;
inline constexpr u32 Steps = 5;

// The surface's normal (2026-11-15; the user in the headset: the shading "adds some artifacts
// that ... jump a bit" when the head moves, and "in some places they get added on the floor and
// jitter"). It was rebuilt from the view position's screen derivatives: at half size, across the
// foveation's coarse fragments, and on a floor seen at a shallow angle (where a pixel's depth step
// is large and its rounding shows) that normal wobbles from pixel to pixel and frame to frame, and
// the open half-circle about it with it: shading on a flat floor. The game's AO pass binds the
// scene's own normal buffer (world space, x y z in 0..1) and the view's rotation (its matrix
// buffer's dwords 20..22, 24..26, 28..30): that normal instead, where the pass finds both (the
// program samples a second image whose x, y and z it reads, and reads those dwords). What
// LanternEngine's and BBFSS's AO do too. VR_AO_BUFFER_NORMALS=0 (tests): the derivatives'.
inline bool BufferNormals() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_AO_BUFFER_NORMALS");
        return env == nullptr || env[0] != '0';
    }();
    return on;
}
inline constexpr u32 RotationDwords[3] = {20, 24, 28};
// The search's noise stays the same from frame to frame (the same day): turned every frame it
// leaned on the upscaler's history, which a turning head empties: the grain showed, and moved.
// Per pixel only now, as the game's own AO's and LanternEngine's; the game's blur takes it.
// VR_AO_FRAME_NOISE=1 (tests): turned every frame, as before.
inline bool FrameNoise() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_AO_FRAME_NOISE");
        return env != nullptr && env[0] == '1';
    }();
    return on;
}

// Visibility bitmask (2026-11-10, the user: "let's adopt their AO", LanternEngine's GTAO, after
// Therrien et al. 2023, "Screen Space Indirect Lighting with Visibility Bitmask"): each sample
// hides only the sectors of the slice's half-circle between its front and its back, as if every
// surface were ThicknessM (+ ThicknessPerM per metre of its distance) thick, thinner towards the
// radius' edge; Sectors sectors, each weighted by the cosine to the normal. A railing or a blade
// of grass hides a sliver of sky, not all of it above it: no dark halos round thin things, and a
// wall still closes its corner. Samples nearer than BiasShare of the radius are the pixel's own.
inline constexpr u32 Sectors = 32;
inline constexpr float ThicknessM = 0.10f;
inline constexpr float ThicknessPerM = 0.04f;
inline constexpr float BiasShare = 0.02f;

// Shine stays out of cracks (2026-11-11, the polish list): the game's ambient composite
// (`0x3fe9e964`, reflections.h IsCompositeProgram) darkens the env and screen-space shine (its
// second target) by the AO as it darkens the ambient light: by the occlusion alone. With the AO
// on, SpecularOcclusionPass (reflections_pass.cpp, RuntimeInfo::VrSpecularOcclusion) darkens the
// shine by Lagarde's specular occlusion instead (Frostbite, "Moving Frostbite to PBR", 2014):
// saturate((N.V + ao)^exp2(-16 r^2 - 1) - 1 + ao), r the roughness from the reflection's lod. A
// glossy crack seen at a grazing angle (a VR street) loses its shine; open, rough or facing
// surfaces keep the game's. Never brighter than the game's (SpecDarkenOnly); SpecOcclusion is
// its share (0: the game's). Cost: three transcendentals a pixel, no fetch.
inline constexpr float SpecOcclusion = 1.0f;
inline constexpr bool SpecDarkenOnly = true;
// Its startup policy: with our AO (Enabled); VR_SPEC_OCCLUSION=0 turns it off (tests).
inline bool SpecEnabled(bool ao_enabled, const char* override_value) {
    return ao_enabled && !(override_value && override_value[0] == '0');
}

} // namespace Shader::Optimization::AmbientOcclusion
