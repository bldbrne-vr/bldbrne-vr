// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>

#include "common/types.h"

// Bounce light (2026-11-09: "brilliant raytraced lighting ... to make the environment
// look real", then "bounce light"): one bounce of the scene's own lit diffuse light. The game's
// light composite (GXLightComposite) adds its light buffers (the lit diffuse at its inputs 3 and
// 4, the specular at 5) and fogs them; its input 2 is the surface colour, input 1 the depth.
// BouncePass (light_shadow_pass.cpp) adds VrBounceLight to its colour: Samples points of the
// screen around the pixel out to RadiusM, each one's lit diffuse light weighted by the cosine to
// this pixel's normal and by distance, times this pixel's own surface colour: a fire's light
// spills warm onto the walls round it, a red carpet tints the stones beside it.
namespace Shader::Optimization::BounceLight {

// Startup policy (vr.ini bounce_light, VR_BOUNCE_LIGHT=0|1 overrides it): with an upscaler or
// without (light_shadow.h Enabled).
inline bool Enabled(bool vr, bool setting, const char* override_value) {
    return override_value ? override_value[0] != '0' : vr && setting;
}

// The game's light composite program (the flag is set only for it; the pass checks its shape).
inline constexpr bool IsCompositeProgram(u64 hash) {
    return hash == 0x32e47788u;
}

// Its inputs (image handles, sampler 0): the depth, the surface colour, the lit diffuse light.
inline constexpr u32 DepthImage = 1;
inline constexpr u32 AlbedoImage = 2;
inline constexpr u32 DiffuseImages[2] = {3, 4};

// The gather: RadiusM around the pixel (at least MinPixels, at most MaxPixels on screen),
// Samples points on a spiral (rotated per pixel and per frame), the sum times Strength.
inline constexpr float RadiusM = 1.5f;
inline constexpr float MinPixels = 8.0f;
inline constexpr float MaxPixels = 128.0f;
inline constexpr u32 Samples = 8;
// No light through walls (2026-11-11, the idea of Lantern Engine's indirect light, our code; the
// user: "let's do 1-3"): the samples walk out from the pixel along Directions screen lines (turned
// by the jitter), StepsPerSide on each side, and a sample counts only for the part of the view it
// adds above the horizon its nearer ones made (its elevation over the pixel's surface, the cosine
// to the normal, minus the highest so far on that side): light from beyond a pillar, a railing or
// a step's edge no longer reaches the floor in front of it. Samples = Directions x 2 x
// StepsPerSide (the same work as the spiral). VR_BOUNCE_HORIZON=0: the old spiral (tests).
inline constexpr u32 Directions = 2;
inline constexpr u32 StepsPerSide = 2;
static_assert(Samples == Directions * 2 * StepsPerSide);
// (1.5 since 2026-11-11, the user on the further bounces: "Make it stronger"; was 1.2.)
inline constexpr float Strength = 1.5f;
// A gathered texel's light at most (and not below 0, and a number: one bad texel of the game's
// buffers would otherwise spread over every pixel that gathers it), and the bounce's.
inline constexpr float MaxLight = 256.0f;
// ... and a gathered sample's light at most SampleMaxLuminance (luminance, its colour kept; the
// user, 2026-11-12 night: "When a new light appears, the screen glows. It lights up geometry ...
// all geometry the light would affect, flashes. it's as if the light is figuring itself out"):
// next to a lamp the lit surfaces reach 7 and more, so whether a pixel's eight jittered samples
// fell on them swung its bounce frame to frame, and a lamp coming into view (nothing in the
// upscaler's history) lit everything round it for some 200 ms before the average settled. A lamp's
// own glow is not light its surroundings bounce. VR_BOUNCE_SAMPLE_MAX=<luminance> (tests).
inline constexpr float SampleMaxLuminance = 2.0f;
// More than one bounce (2026-11-11: "light bouncing more than once"): in surroundings of
// albedo rho that fill a share F of the pixel's view (the gather's mean weight: about 0 on open
// ground, 0.3-0.5 in an alley or a room), each further bounce is rho * 2 Strength * F of the one
// before, so all of them together are the first over (1 - that) per colour (a uniform enclosure's
// radiosity, the receiver's albedo standing in for its neighbours'). Multi scales it (0: one
// bounce; VR_BOUNCE_MULTI overrides it, tests); MaxGain caps it (at most 1 / (1 - 0.6) = 2.5x).
// Warmer, fuller wood and brick interiors for about 15 ALU a pixel, no fetch, no history.
// 4 (2026-11-11: "Make it stronger"): the physical 1 was a few percent, invisible even
// in the library; 4 roughly doubles an interior's fill, still capped by MaxGain.
inline constexpr float Multi = 4.0f;
inline constexpr float MaxGain = 0.6f;
// Crevices stay dark (2026-11-15; the user in the headset: the bounce is "making things that
// shouldn't be bounced (eg tiny crevices that should be dark and are dark by default) bright").
// The gather's samples stand half a metre apart: a groove between two stones is nothing to its
// horizons, its walls face the lit stones beside it, and the further bounces' enclosure gain
// (Multi) takes a groove for a room. What keeps a crevice dark in the game is its own light: the
// ambient light there is already shaded (the AO's work), the lamps do not reach in. So the
// bounce a pixel gets is at most GuardTimes its own lit diffuse light (both times its surface
// colour): open ground and walls, lit or in a lamp's shadow, keep theirs (their ambient light is
// whole), a crevice gets the share its shading leaves. LanternEngine multiplies its bounce by the
// AO; the light composite has no AO to read, its own diffuse light carries it.
// VR_BOUNCE_GUARD=<times> (tests; 0: off, as before).
inline constexpr float GuardTimes = 4.0f;

// The 2x2 split (2026-11-16, the user, after the GPU study put the bounce at 0.23-0.28 ms: "try
// it", then on the pictures of the chapel, Central Yharnam and the Dream's workshop: "Edge fixes,
// then push"): each pixel of a 2x2 block walks one of the four rays and the block shares their
// light (EmitVrBounceLight), a quarter of the fetches with no image, buffer or history of its own:
// the same light in the same places for about a quarter of the cost (the Dream: 0.29 -> 0.07 ms).
// A neighbour's rays count by how surely it lies on this pixel's surface (soft weights, after
// BBFSS's denoiser, our code): e^-(its distance from the plane over QuadPlaneShare of the view
// depth), the larger of the two pixels' planes, times the cosine between their normals to the
// QuadNormalPower; the rays left out are made up by the ones counted. The normals are each pixel's
// own (SurfaceNormal: per axis the step to the neighbour on its side of an edge). A same-plane
// floor shares fully however grazing; a railing before a wall, a crate's corner, do not.
// VR_BOUNCE_QUAD=0 (tests; the shader cache does not key it, so a fresh cache): every pixel walks
// all four rays, as before.
inline constexpr float QuadPlaneShare = 0.01f;
inline constexpr u32 QuadNormalPower = 8; // (a power of two: squarings)
static_assert((QuadNormalPower & (QuadNormalPower - 1)) == 0);
// The block's four pixels walk the four rays of two directions (its rows pick the direction, its
// columns the side): more directions would leave some unwalked and still be scaled as walked.
static_assert(Directions == 2, "the 2x2 split covers two directions, two sides");
inline bool QuadGather() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_BOUNCE_QUAD");
        return env == nullptr || env[0] != '0';
    }();
    return on;
}

} // namespace Shader::Optimization::BounceLight
