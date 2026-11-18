// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>

#include "common/types.h"

// Lit smoke (2026-11-11: the visible next-gen items, "volumetric fog" part 1): the
// game's soft smoke, mist and dust sprites (its GXFfx soft programs) take no light from the
// lamps, fires and torches round them, so a pyre's smoke column is the same flat grey beside the
// fire as far from it. ParticleAirPass (particle_air_pass.cpp) finds, in such a program's
// fragment stage (RuntimeInfo::VrParticleAir), the vector from the fragment to the camera in
// world axes the program already interpolates for its distance fog (sqrt(a.x^2 + a.z^2) of one
// attribute, its chans 0 and 2), and adds VrParticleAir to its colour: per light of the frame's
// strongest (the lights in the air's own, Rasterizer::VrFogUpload: its lamp memory, merging and
// flashes), the game's own point-light falloff 1 / (d^2 + CoreM^2) - reach at the fragment, a
// mild forward phase, times the light's air colour; then times the smoke's own hue as an albedo.
// The alpha is untouched (the blend gives the density), so both eyes get the same light and
// nothing is noisy. Only alpha-blended draws (an additive fire or ember is its own light).
namespace Shader::Optimization::ParticleAir {

// Startup policy: with the lights in the air (vr.ini fog_light, the New graphics page's "Light
// in the air"; its lights are the ones this reads) and an upscaler; VR_PARTICLE_AIR=0|1
// overrides it.
inline bool Enabled(bool vr, bool setting, bool upscaler, const char* override_value) {
    return override_value ? override_value[0] != '0' : vr && setting && upscaler;
}

// The soft particle programs of the shader archive (GXFfx*Soft*, PS_VFX_SoftTexture*). The pass
// leaves any whose fog vector it does not find as it is (with a log line). Sorted.
inline constexpr std::array<u32, 12> Programs{
    0x1587ddc9u, // GXFfxtessellateSoftMultiTexture
    0x20cee1b5u, // PS_VFX_SoftBumpTexture
    0x2e64f5c6u, // GXFfxtessSoftBumpTexture
    0x35b8197fu, // PS_VFX_SoftCheapBumpTexture
    0x3a69b784u, // GXFfxtessellateSoftMultiTexture2Sampler
    0x498c09f3u, // GXFfxtessellateSoftBumpTexture
    0x528b383cu, // PS_VFX_SoftTexture
    0x5e224cb6u, // GXFfxsoftTexture
    0x5ed27cb4u, // GXFfxtessSoftMultiTexture
    0x71c96598u, // GXFfxtessellateSoftTexture
    0xbf18ff46u, // PS_VFX_SoftCheapTexture
    0xfa0033dau, // GXFfxsoftMultiTexture2Sampler
};
static_assert(std::ranges::is_sorted(Programs));

inline bool IsParticleProgram(u64 hash) {
    return hash <= 0xffffffffull && std::ranges::binary_search(Programs, static_cast<u32>(hash));
}

// The buffer (Shader::BufferType::VrParticleLights, Rasterizer::VrParticleUpload): a header of
// HeaderDwords (the light count as a u32, the strength, 0, 0, then the flat camera's world-to-view
// rotation as three rows of four), then up to MaxLights lights of LightDwords each, as the lights
// in the air's (the flat camera's view position xyz, the reach, the air colour rgb times its
// share, 0): the frame's strongest, the first of that buffer.
inline constexpr u32 MaxLights = 8;
inline constexpr u32 HeaderDwords = 16;
inline constexpr u32 RowsDword = 4;
inline constexpr u32 LightDwords = 8;
inline constexpr u32 BufferDwords = HeaderDwords + MaxLights * LightDwords;

// How much of the light the smoke gives back (VR_PARTICLE_AIR_STRENGTH overrides it: a buffer
// value, no shader change).
// (0.4 since 2026-11-11: 0.6 with the small core and the high cap blew the smoke round a pyre out
// to a white blob in the pictures.)
// (0.7 since 2026-11-14, the user: "I don't see fires like really light up their smoke". The cap
// below still keeps a pyre's smoke from going white.)
// (1.0 since the morning of 2026-11-14, the user in the headset: lamps should light the fog more,
// "the smoke too".)
inline constexpr float Strength = 1.0f;
// The light's core: no infinite brightness at a flame (0.5 m: smoke right at a fire takes at most
// four times its light at a metre, not forty).
inline constexpr float CoreM = 0.5f;
// A light's own core (its record's dword 7, squared metres; at least CoreM's): a light with no
// flame at its place (air_light.h Sources) lights the smoke over SoftCoreM, a soft tint of the
// mist round it and no bright ball at an empty spot (which the source check would take for a
// flame).
inline constexpr float SoftCoreM = 1.0f;
// A mild forward glow (Henyey-Greenstein g), half of it constant and capped at PhaseMax: a
// strong forward lobe made blobs of the moon's shafts.
inline constexpr float PhaseG = 0.3f;
inline constexpr float PhaseMax = 2.0f;
// The smoke's albedo: Albedo times its hue ((rgb + Lift) / (max(rgb) + Lift): grey mist stays
// grey, soot stays darker) times its brightness (luma times Gain) within Floor..1.
inline constexpr float Albedo = 0.6f;
inline constexpr float Lift = 0.02f;
inline constexpr float Gain = 8.0f;
inline constexpr float Floor = 0.25f;
// Smoke farther from the eye than MaxEyeM takes none (beyond the lamps' reach anyway).
inline constexpr float MaxEyeM = 40.0f;
// The light added at most (and a number): a soft glow, never white.
inline constexpr float MaxAir = 2.0f;

} // namespace Shader::Optimization::ParticleAir
