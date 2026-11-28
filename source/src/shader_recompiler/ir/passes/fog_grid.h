// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "common/types.h"
#include "shader_recompiler/ir/passes/air_light.h"

// The fog grid (2026-11-14: "still getting some blinking of the fog"; "start building
// the fog grid").
//
// Why: the air pass (air_light.h) marches the mist for every half-size cell of each eye's screen
// on its own, eight points along the ray with one of four fixed jitters. What a cell holds changes
// with every sway of the head (the eight points land somewhere else in the banks, a far thing is
// in a cell or it is not), nothing is shared between the eyes, and each eye pays for its own march.
//
// Now one box of air cells for both eyes: the head's view (the flat camera's: x right, y up, z
// ahead, the light lists' and the moon cascades' own space) cut into GridX x GridY columns and
// GridZ slices, thin near you and thick far off. One dispatch of our own a frame
// (host_shaders/vr_fog_grid.comp; renderer_vulkan/vr_fog_grid.h), just before the scene's copy
// draws, one thread a column, front to back:
//  - every cell works out the mist at its middle (air_light.h Mist: the same layer over the
//    ground, the same drifting banks, the same thin haze) and what it sends towards the eye (the
//    moon's colour, the mist's own phase);
//  - and keeps the sums so far, so slice k holds all the light between the eye and its far face
//    and how much of what is behind gets through. Written to a buffer the scene's copy reads
//    (Shader::BufferType::VrFogGrid).
// The scene's copy (air_light.h IsApplyProgram) looks each pixel up: its view position moved to
// the head's (the eye's shift along x), the eight cells round it blended by hand. A cell is a
// place in the view, its mist a smooth function of the world: nothing is random, nothing depends
// on which pixel asks, both eyes read the same air.
//
// Steps 2 and 3 of the build order (this file's state):
//  - The moon's shadow. Every cell asks the game's cascaded moon shadow atlas (moon_shafts.h: the
//    same buffer of cascades, the same test) whether the moon reaches it: Taps places a frame
//    inside the cell, spread along its depth. The lit share cuts the mist's light (Contrast of
//    it; what is lit counts LitGain times, so a beam stands out of the dark air round it) and all
//    of the moonlight in the thin air (moon_shafts.h: its density, strength and reach by the
//    area, its near and far windows, its phase), which now lives here: the half-size cells are
//    not marched any more and the scene's copy reads none of them (no 3x3 tent).
//  - Jitter and history. The places move inside the cell every frame (each cell its own
//    sequence), and a cell's lit share is blended with what the same place in the world held in
//    the last frames (a second picture of the cells, looked up through the camera's move: its
//    turn from the moon's own light space, which the cascades give exactly for the very frame
//    drawn and which does not turn with the head; its step from the camera record). A cell whose
//    place was outside the last grid starts anew with FreshTaps places. Only the lit share has a
//    history: the mist, the colours and the phases are smooth and worked out anew every frame, so
//    nothing of them can trail.
// Fog patches (fog_patches.h): fog that stands at a place in the world, added to the cells' mist;
// the grid is built wherever one is near, the blanket or not.
// The lamps light the mist (Lamps below): the point lights of the frame, each its own colour.
// Still as they were: the lamps' glow in the clear air, the distance haze past the grid, the knees.
//
// On only with VR_FOG_GRID=1 (or =small: the small grid) and the mist itself on (air_light.h
// Mist::Enabled); the air pass as it was otherwise. The switch is a bit of the three air programs'
// runtime info (RuntimeInfo::VrFogGrid), so a permutation made with it is never taken for one made
// without. (The moon's rays, VR_AIR_RAYS, lived in the cells' mist: none with the grid.)
namespace Shader::Optimization::FogGrid {

inline bool Enabled() {
    static const bool on = [] {
        // (On by default since 2026-11-15, the user: "i want the fog grid and the tree shadows in
        // fastv2-premigration". VR_FOG_GRID=0: the old cells' mist and tent.)
        const char* const env = std::getenv("VR_FOG_GRID");
        return AirLight::Mist::Enabled() && (env == nullptr || env[0] == '1' || env[0] == 's' || env[0] == 'l');
    }();
    return on;
}
inline bool Small() {
    static const bool small = [] {
        const char* const env = std::getenv("VR_FOG_GRID");
        return env != nullptr && env[0] == 's';
    }();
    return small;
}

// Tests: VR_FOG_GRID=large, 240 x 136 x 96 cells (3.4 times the cells): what finer cells buy.
inline bool Large() {
    static const bool large = [] {
        const char* const env = std::getenv("VR_FOG_GRID");
        return env != nullptr && env[0] == 'l';
    }();
    return large;
}
inline constexpr u32 LargeX = 240;
inline constexpr u32 LargeY = 136;
inline constexpr u32 LargeZ = 96;

// The size (cells). 160 x 90 x 64 = 921 600. Over a headset's view (both eyes together about 108
// degrees across and 96 up and down) a cell is about one degree: some 12 x 17 pixels of a
// 1336 x 1440 eye. The mist is soft; the banks are tens of metres across.
// The small one (128 x 72 x 48, under half the cells) is there to measure what the size costs.
inline constexpr u32 GridX = 160;
inline constexpr u32 GridY = 90;
inline constexpr u32 GridZ = 64;
inline constexpr u32 SmallX = 128;
inline constexpr u32 SmallY = 72;
inline constexpr u32 SmallZ = 48;

// The slices: equal steps of log2(view z) from NearM to FarM (the plane's depth, not the ray's
// length: a pixel's slice comes straight from its view z). 0.5 m to 64 m is seven octaves: each of
// 64 slices is 7.9% deeper than the one before (8 cm thick at 1 m, 0.8 m at 10 m, 4.7 m at 60 m).
// The mist is clear within Mist::NearStartM (1 m) of the eye and marched to Mist::FarM (60 m of
// the ray, which is never more than 60 m of depth): the grid holds all of it. Past the mist's
// reach the scene's copy goes on with the distance haze's closed form, as before.
// (Live key lighting.mist_far past 64 m: the grid still ends at 64 m of depth.)
inline constexpr float NearM = 0.5f;
inline constexpr float FarM = 64.0f;
// The far plane in use (2026-11-16, the game's own lingering fog taken over, core/vr/fog_emitters.h:
// its "distant-view" banks are meant to be seen from further than 64 m). VR_FOG_GRID_FAR=<m>
// (64..256) puts the last slice there: the same 64 slices, each a little deeper (160 m: 9.4% a
// slice, not 7.9%). The fog patches' far fade and reach go with it (fog_patches.h).
// With the game's fog taken over: TakenFarM.
inline constexpr float TakenFarM = 128.0f;
// The game's fog taken over in the hunter's map: set by core/vr/bloodborne_woods_shadows.cpp, at
// the start (before the first frame) as the menu's Fog row is, then once a frame as his map's file
// was loaded (since 2026-11-21 the row switches the fog at the next load of an area). Everything
// that follows it reads it each time (a change comes with a load of an area).
inline std::atomic<bool>& FogTakenNow() {
    static std::atomic<bool> taken{false};
    return taken;
}
inline bool FogTaken() {
    return FogTakenNow().load(std::memory_order_relaxed);
}
// The moonlit thin air's far fade (moon_shafts.h FarFade: a surface beyond the air's reach keeps
// less of the glow, down to FarKeep at `fade` times the reach) ends no further than the grid: the
// last slice stands for everything beyond it (far hills, the sky) and holds the fade's end. In the
// Forbidden Woods the air reaches 40 m and its fade went on to 160 m in a grid of 64 m: a thing
// 60 m off kept 0.98 of the glow and one past 64 m 0.75, a quarter of the glow over one slice as
// you walk up to a far hillside (a review's finding, 2026-11-17). There it now fades from 40 to
// 64 m. Where the reach is 16 m the fade ended at the grid's 64 m already: unchanged.
inline constexpr float GlowFarFade(float fade, float reach_m, float grid_far_m) {
    return reach_m > 0.0f ? std::clamp(grid_far_m / reach_m, 1.0f + 1e-3f, std::max(fade, 1.0f + 1e-3f)) : fade;
}
// The shader's rule for a slice whose far end is `z_m` off along the view's axis (vr_fog_grid.comp
// `beyond`): 0 inside the reach, 1 from `fade` times the reach on. With GlowFarFade the grid's last
// slice gets 1 by this rule (it is not forced to it), and where the reach is longer than the grid
// (VR_MOON_SHAFTS_FAR, tests) it gets 0 like the slices before it: no step at the grid's end.
inline float GlowBeyond(float z_m, float reach_m, float fade) {
    const float t = std::clamp((z_m - reach_m) / (reach_m * std::max(fade - 1.0f, 1e-3f)), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
inline float FarNow() {
    static const float asked = [] {
        const char* const env = std::getenv("VR_FOG_GRID_FAR");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), FarM, 256.0f) : 0.0f;
    }();
    return asked > 0.0f ? asked : FogTaken() ? TakenFarM : FarM;
}

// The frustum: the head's, widened to hold both eyes. An eye sits `shift` to the side of the head
// (eye 0 at minus half the eye distance times the world's scale: the fog lights' rule) and sees a
// point at depth z at its own tangent t; from the head that point is at tangent t + shift / z, at
// most |shift| / NearM more (0.032 / 0.5 = 0.064, three or four cells). So per axis:
//   lo = min over the eyes of (tanLo_eye + min(shift_eye, 0) / NearM), less GuardCells cells
//   hi = max over the eyes of (tanHi_eye + max(shift_eye, 0) / NearM), plus GuardCells cells
// (the eyes' tangents from their own ndc-to-view matrices). The guard keeps the blend of eight
// cells off the grid's edge.
inline constexpr float GuardCells = 2.0f;

// The parameters of the dispatch (a read-only storage buffer of ParamVec4s vec4, std430:
// every member a vec4 or an ivec4, so the C++ side is an array of dwords and no padding rule can
// bite). The vec4's index:
//  0 (i) cells x, y, z, the frame's tag (never 0)
//  1 frustum: tan x lo, tan y lo, tan x width, tan y width (the widened one, above)
//  2 slices: near, far, log2(far / near), 1 / that
//  3..5 this frame's view to world (three rows: the rotation, then the camera's place)
//  6 the air's colour (the moon's: the probe's, as the scene's copy gets it), the mist's reach (m
//    of the ray)
//  7 mist: time, density, 1 / height, the eye's height over the ground
//  8 mist: ambient, forward, the noise's scale, extinction
//  9 mist: wind x y z, patchy
//  10 mist: near start, near full, strength, high (the thin haze's share)
//  11 mist: 1 / the haze's height, the mist's phase g, 0, 0
//  12 towards the moon (the view's x y z), half the eye distance (the eyes' shift)
//  13 moon glow: density (per metre), reach (m), phase g, forward share
//  14 moon glow: strength, near start, near full, the share of its density that dims
//  15 moon glow: far keep, far fade, 0, 0
//  16 shadow: the mist's contrast, the lit gain, places a cell, places of a cell without a past
//  17 history: this frame's share (1: no past), jitter on (0 or 1), the frame's number, the past
//     can be read (0 or 1)
//  18..20 this view to the last build's view (three rows of four)
//  21 the last build's frustum (as vec4 1)
//  22 the atlas: the compare's sign, width, height, shadows on (0: every cell lit)
//  23 the cascades' far ends in view z
//  24..39 the four cascades, four vec4 each: rows 0..2 of its view-to-shadow (uv x, uv y, depth;
//     over (view, 1)), then its tile's corner (atlas uv), the tile's size, the depth bias
//     (moon_shafts.h's buffer, as Rasterizer::VrMoonUpload fills it)
//  40 fog patches (fog_patches.h): how many, near start, near full, the noise's scale
//  41 fog patches: far start, far end, the noise's drift (a share of the mist's wind), 0
//  42.. the patches, PatchVec4s vec4 each (fog_patches.h Pick), MaxPatches at most
inline constexpr u32 CascadeVec4 = 24;
inline constexpr u32 PatchVec4 = 40;
inline constexpr u32 PatchHeadVec4s = 3; // (the third: the patches' own colour, fog_patches.h TintNow)
inline constexpr u32 PatchVec4s = 4;
inline constexpr u32 MaxPatches = 32; // (one bit each in the shader's column mask; 8 until 2026-11-16: the game's fog taken over lists more than that within reach)
// The lamps (Lamps below), from LampVec4 = the patches' end:
//  +0 how many lights (0: none, the cells never look), their strength, every cell asks every
//     light (tests: 1), 0
//  +1.. the lights, LightVec4s vec4 each: the view position and the reach (1 / radius^2); the
//     colour and the core (m^2)
//  then the tiles: one dword a tile (TilesX x TilesY x TilesZ, by row, column, band), four bytes:
//     the indices of the lights that reach it (255: no more)
namespace Lamps {
inline constexpr u32 MaxLights = 25; // fog_light.h MaxLights and the muzzle flash
inline constexpr u32 LightVec4s = 2;
inline constexpr u32 TilesX = 20;
inline constexpr u32 TilesY = 12;
inline constexpr u32 TilesZ = 8;
inline constexpr u32 Tiles = TilesX * TilesY * TilesZ;
inline constexpr u32 TileLights = 4;
} // namespace Lamps
inline constexpr u32 LampVec4 = PatchVec4 + PatchHeadVec4s + PatchVec4s * MaxPatches;
inline constexpr u32 LampTileVec4 = LampVec4 + 1 + Lamps::LightVec4s * Lamps::MaxLights;
// The hunter's wake in the fog (fog_patches.h Wake), after the lamps' tiles: how many points, the
// reach along the ray (m), 0, 0 | their strengths | a point each: its place (the ground's y), 1 / r^2.
inline constexpr u32 MaxWake = 8; // (the hunter's four, the others' four: fog_patches.h Wake)
inline constexpr u32 WakeVec4 = LampTileVec4 + Lamps::Tiles / 4;
// The columns each patch can be seen in (first x, first y, last x, last y; the rasterizer's, from
// the patch's bounding sphere in the grid's view): a column outside them leaves the patch alone.
inline constexpr u32 WakeVec4s = 1 + MaxWake / 4 + MaxWake; // the head, the strengths, the points
inline constexpr u32 RectVec4 = WakeVec4 + WakeVec4s;
// A lamp's light in the taken-over fog (Lamps::Taken* below), after the patches' columns: the
// forward share, the phase's g, what the fog between takes (per density and metre), the cap.
inline constexpr u32 LampShapeVec4 = RectVec4 + MaxPatches;
// Each lamp's free lengths before a wall along the world's axes (core/vr/map_lights.h Reach), two
// vec4 a lamp: +x +y +z, then -x -y -z (m; a lamp not measured, and the frame's own lights: its
// radius every way).
inline constexpr u32 LampFreeVec4 = LampShapeVec4 + 1;
// The taken-over banks' shaping by the noise's wide wave and their merging (fog_patches.h
// Organic): the billow (how much a bank's height follows the wave), the merge's limit (per metre;
// 0: the banks' plain sum), the outline's break-up, 0.
inline constexpr u32 OrganicVec4 = LampFreeVec4 + 2 * Lamps::MaxLights;
// Each patch's ground slope (fog_patches.h Slope): the rise per metre along the world's x and z, 0, 0.
inline constexpr u32 SlopeVec4 = OrganicVec4 + 1;
// The clearings through crowded banks (fog_patches.h Organic::ClearNow): the share a lane takes,
// 2 pi / its width (per metre), 0, 0.
inline constexpr u32 ClearVec4 = SlopeVec4 + MaxPatches;
// The wisps' strength by the height (fog_patches.h Organic::LayersNow): at the ground, 1 / the
// height's scale, high up; the share of the low fog that stays at the hunter's feet (Feet). (A patch's slope vec4 holds its self-shade's gain in z.)
inline constexpr u32 LayerVec4 = ClearVec4 + 1;
// The world-fixed volume (the user's pick in the fog lab, 2026-11-17: "I think I like the
// world-fixed fog volume", "0.5 m, soft"): the taken-over banks' fog worked out once a frame at
// fixed places of the world, cubes of CubeM x TallM x CubeM in a box round the hunter snapped to
// the cubes' lattice, read by the camera grid (three reads along a cell) instead of worked out at
// the grid's own cells' middles, which move with the camera: a wisp is where it is in the world.
// One volume for both eyes. Its vec4: the box's low corner in the world, 1 when it is on.
// VR_FOG_VOLUME=0: the grid's own way everywhere.
namespace World {
// (VR_FOG_VOLUME=3, a check: the grid works the cubes' rule out itself where it would read the
// picture.)
inline int Mode() {
    static const int mode = [] {
        const char* const env = std::getenv("VR_FOG_VOLUME");
        return env != nullptr ? std::atoi(env) : 1;
    }();
    return mode;
}
inline constexpr u32 SizeX = 128, SizeZ = 128, SizeY = 48; // host_shaders/vr_fog_grid.comp WORLD_SIZE
inline constexpr float CubeM = 0.5f, TallM = 0.25f;
inline constexpr float BelowM = 4.0f; // of the box under the hunter's feet (the rest, 8 m, over them)
inline bool On() {
    static const bool off = [] {
        const char* const env = std::getenv("VR_FOG_VOLUME");
        return env != nullptr && env[0] == '0';
    }();
    return FogTaken() && !off;
}
} // namespace World
inline constexpr u32 WorldVec4 = LayerVec4 + 1;
// A lamp's light dies in the fog (Lamps::Fall* below): per metre in the taken-over banks, in the
// air's mist, 0, what the mist's lamp light counts times.
inline constexpr u32 LampFallVec4 = WorldVec4 + 1;
// The mist's ground from the map (core/vr/mist_ground.h): the lattice's low corner (x, z), 1 / a
// cell's width, 1 when it is on; then its cells' ground heights, four a vec4, row by row.
inline constexpr u32 GroundVec4 = LampFallVec4 + 1;
inline constexpr u32 GroundCells = 32; // core/vr/mist_ground.h Cells
// The fog stirred by what moves through it (2026-11-17: "make the [fog] react to enemies
// and the hunter walking through it, or swinging our weapon.. basically make it feel like a fluid
// simulation, except all fake shader work"). A field over the world volume's columns, kept from
// frame to frame and fixed to the world: how far the fog there has been pushed aside (x, z; m),
// how much of it is cleared, how churned it is. A pass of its own a frame (the grid's shader built
// with STIR_FIELD): what was there flows back (AsideS), closes (ClearS), calms (ChurnS), spreads a
// little, is carried round where it is churned; then each mover pushes the fog aside round itself,
// drags it along its way and clears it (by the point wake's own rule round a body: all but gone
// at its heart, thicker in a rim about its radius; a standing hunter's fog is what it was). The
// volume's build reads it: the cleared share is gone, the wisps are read where the fog came from
// and swirl where it is churned. The clearing behind the hunter is what the
// field still holds of him: it closes smoothly, with no points dropped along his way (the user,
// the same night: "while moving backwards the [fog] appeared almost... like in steps, not
// fluidly"). The movers: the hunter's body, the enemies and NPCs near him (the lamp shadows'
// casters), his hands when they move fast and low (the weapon's swing). VR_FOG_STIR=0: the point
// wake, as it was.
// Its vec4s: the frame's seconds, 1 when on, how many movers, 0 | the last frame's volume corner
// (x, z), 1 when the last field is there, 0 | what a frame keeps of the push, the clearing, the
// churn; the share that spreads | a mover, two vec4: its place (x, its low y, z), 1 / r^2; its
// speed (x, z; m/s), its strength, its churn.
namespace Stir {
inline constexpr u32 MaxMovers = 12;
inline constexpr float AsideS = 3.0f;
inline constexpr float ClearS = 1.6f;
inline constexpr float ChurnS = 2.5f;
inline constexpr float SpreadPerS = 2.0f;
// The others' bodies: a radius and strength a little under the hunter's own (fog_patches.h Wake).
inline constexpr float OtherRadiusM = 1.4f;
inline constexpr float OtherStrength = 0.8f;
// A hand: nothing under HandSlowMPerS, all of it from HandFastMPerS (a swing; a walking arm is
// under 1.5 m/s); all of it up to HandLowM over his feet, none from HandHighM (the banks are
// knee-high: a swing over the head does not reach them).
inline constexpr float HandRadiusM = 1.1f;
inline constexpr float HandSlowMPerS = 1.5f;
inline constexpr float HandFastMPerS = 5.0f;
inline constexpr float HandLowM = 0.7f;
inline constexpr float HandHighM = 1.9f;
inline constexpr float MaxSpeedMPerS = 12.0f; // (a faster step between two frames is a warp: no speed)
inline constexpr float BodyChurn = 0.7f;      // at 4 m/s
inline float HandStrength(float speed, float over_feet) {
    const float fast = std::clamp((speed - HandSlowMPerS) / (HandFastMPerS - HandSlowMPerS), 0.0f, 1.0f);
    const float low = std::clamp((HandHighM - over_feet) / (HandHighM - HandLowM), 0.0f, 1.0f);
    return fast * low;
}
// (Tests: VR_FOG_STIR_TEST, bits: 1 no clearing, 4 the wisps not moved, 8 no others, 16 no
// hands.)
inline int Test() {
    static const int bits = [] {
        const char* const env = std::getenv("VR_FOG_STIR_TEST");
        return env != nullptr ? std::atoi(env) : 0;
    }();
    return bits;
}
inline bool On() {
    static const bool off = [] {
        const char* const env = std::getenv("VR_FOG_STIR");
        return env != nullptr && env[0] == '0';
    }();
    return World::On() && !off;
}
} // namespace Stir
inline constexpr u32 StirVec4 = GroundVec4 + 1 + GroundCells * GroundCells / 4;
inline constexpr u32 StirVec4s = 3 + 2 * Stir::MaxMovers;
// A lamp's light gets further in fog much darker than it (Lamps::Far* below): what its light
// dies less by a metre past the even distance in the taken-over banks, in the mist; the contrast
// it begins at, 1 / the contrast's span to all of it.
inline constexpr u32 LampFarVec4 = StirVec4 + StirVec4s;
// The banks the world-fixed volume holds, a bit a bank in its first word: vr_fog_grid.comp's
// InVolume asked once a build here (HeldMask), where every column of both passes and every bank of
// every slice asked the same three numbers again (a code review, 2026-11-19).
inline constexpr u32 HeldVec4 = LampFarVec4 + 1;
// The lamps' light the fog gathers along a ray, softly limited (Lamps::Soft* below): the limit of
// the bare lamps' light, of the lamps' with a halo (in the grid's light, MAX_LIGHT = 64 white; 0:
// none), 0, 0. (A lamp's kind: LampFreeVec4's second vec4's w, 1 with a halo.)
inline constexpr u32 LampSoftVec4 = HeldVec4 + 1;
// The blasts of the frame (core/vr/fog_impulse.h Blasts; how many: the stir block's second vec4,
// w): two vec4 a blast: its middle (x, z), 1 / r^2, the hole's depth | the wisps' carry (x, z; m),
// their shove outward (a metre from its middle), their churn.
inline constexpr u32 MaxBlasts = 6;
inline constexpr u32 BlastVec4 = LampSoftVec4 + 1;
// (A third vec4 a blast: its ring of piled fog (where in x = d^2 / r^2, 1 / its width, how much
// thicker) and the wisps' turn round its middle, a metre from it.)
// (A fourth: its long way (x, z; none: round), the height the fog has come back up to, 0.)
inline constexpr u32 BlastVec4s = 4;
inline constexpr u32 ParamVec4s = BlastVec4 + BlastVec4s * MaxBlasts;
inline constexpr u32 ParamDwords = ParamVec4s * 4;

// The moon's shadow in the mist (live keys lighting.grid_contrast, lighting.grid_lit): the mist's
// light counts 1 + Contrast * (lit * LitGain - 1). 0: the mist as without shadows.
// (0.75 and 2: under the Forbidden Woods' canopy about a third of the air is lit; the mist's mean
// brightness stays near what it was, the dark air keeps a quarter of its light.)
inline constexpr float Contrast = 0.75f;
inline constexpr float LitGain = 2.0f;
// The game's fog taken over, by the same rule but gentler (2026-11-17, of the Forbidden
// Woods: "I could see almost no fog ... fog is supposed to be a blanket, it has its own color
// (based on ambiance and moon light/atmosphere), and if there's another light source ... it gets
// affected by that"): with 0.75 and 2 a bank the moon does not reach kept a quarter of its light
// and one it reaches had one and three quarters, seven to one, and under the trees a quarter is
// next to nothing. 0.4 and 2.5: six tenths in the moon's shadow (the sky's own light), 1.6 in the
// moon's light. VR_FOG_SHADE="contrast,gain" (tests; "0.75,2": as it was).
inline constexpr float TakenContrast = 0.4f;
inline constexpr float TakenLitGain = 2.5f;
inline std::array<float, 2> TakenShadeNow() {
    static const std::array<float, 2> v = [] {
        std::array<float, 2> s{TakenContrast, TakenLitGain};
        if (const char* const env = std::getenv("VR_FOG_SHADE")) {
            std::sscanf(env, "%f,%f", &s[0], &s[1]);
        }
        return std::array<float, 2>{std::clamp(s[0], 0.0f, 1.0f), std::clamp(s[1], 0.0f, 16.0f)};
    }();
    return v;
}
// In the Forbidden Woods (moon_shafts.h AreaFull, eased): the tester, 2026-11-15, of the lit and
// dark air under the canopy: "in forbidden woods I need it a bit stronger". Pictures of three
// strengths: contrast and gain alone hardly showed; with three times the moonlit thin air beams
// stand between the trees.
inline constexpr float WoodsContrast = 1.0f;
inline constexpr float WoodsLitGain = 7.0f; // (3.5 until 2026-11-15: the beams between the trees were barely there; the user picked the stronger)
// Towards the moon the mist is brighter and its shadows fainter (2026-11-15, of a
// picture whose mist had lost its shadows: "i kinda like the bright moon fog, as long as its only
// when we look at the source of it directly"): by cos^MoonwardFocus of the angle between the
// pixel's ray and the moon (0 from a right angle on), the mist's light up to 1 + WoodsMoonward
// times, the shadow's contrast down to 1 - MoonwardSmooth of it. Woods only (as the lit gain).
// Live keys lighting.grid_moonward (the brightness; 0: none) and lighting.grid_moonward_focus.
inline constexpr float WoodsMoonward = 1.3f; // (1.5 at first; the tester: "lower ... by a tiny tiny amount")
inline constexpr float MoonwardFocus = 2.2f; // (2026-11-15: "tighter than wide, but wider than tight": 1.5 the wide one, 3 the tight)
inline constexpr float MoonwardSmooth = 0.7f;
// The moonlit thin air while the game's own moon rays draw (its light shafts and glow god rays:
// the blood moon after Rom, the Hunter's Dream's moon): this share of itself, eased over
// GameRaysEaseS, back to all of it GameRaysFreshMs after the game's rays were last drawn. Shot at
// the Tomb of Oedon under the blood moon, 2026-11-17: at full strength it lay over the whole
// picture as an even grey veil and softened the game's own rays; the moon is the game's to draw
// there. VR_GLOW_UNDER_GAME_RAYS=share (1: as before).
// (Tried at 0.3 the same night: it steps back as meant, and the picture is all but the same; the
// veil is the haze and the mist, not this glow. 1: off, until the veil itself is dealt with.)
inline constexpr float GameRaysShare = 1.0f;
inline constexpr float GameRaysEaseS = 1.5f;
inline constexpr s64 GameRaysFreshMs = 1000;
inline float GameRaysShareNow() {
    static const float v = [] {
        const char* const env = std::getenv("VR_GLOW_UNDER_GAME_RAYS");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.0f, 1.0f) : GameRaysShare;
    }();
    return v;
}
inline constexpr float WoodsGlow = 4.0f; // times moon_shafts.h WoodsStrength, in the grid only
// Places a cell and frame asked of the atlas (lighting.grid_taps, 1..8), and for a cell without a
// past (just turned into view, after a warp).
inline constexpr u32 Taps = 1; // (2 until 2026-11-15: one place a frame with the history measured as steady, still head and slow pan, and the dispatch 0.137 -> 0.112 ms)
inline constexpr u32 FreshTaps = 8;
// The history: a cell's lit share follows what it sees now over HistoryS (the share of a frame
// 1 - exp(-dt / HistoryS), MinBlend at least). Live key lighting.grid_history (times HistoryS; 0:
// no history, the raw places of the frame), lighting.grid_jitter (0: the places hold still).
// No past is read after a step of more than WarpM or a turn of more than MaxTurnDeg in a frame (a
// warp, a cut: a head or a snap turn is less), more than GapS without a build, another grid size,
// or a frame without cascades of its own.
inline constexpr float HistoryS = 0.12f;
inline constexpr float MinBlend = 0.05f;
inline constexpr float WarpM = 4.0f;
inline constexpr double MaxTurnDeg = 60.0;
inline constexpr float GapS = 0.25f;

// The lamps light the mist (2026-11-15: fog pockets and the woods' mist "take the colour
// of nearby point lights": the held torch and lantern, the map's lamps and fires, enemy torches,
// "a shot flashing the bank in front of you"). The lights are the composite's own list
// (fog_light.h: the lamp memory's strongest with a glow of their own, the rasterizer's
// VrFogUpload), the torch in his hand (Lamps::Held*) and the muzzle flash while it shines, in the
// head's view, one list for both eyes.
// A cell with mist or patch in it adds, per light that reaches it, the light's air colour times
// its falloff (1 / (d^2 + core + (half the cell's length)^2) - reach: the game's, softened by the
// cell's own size) times the cell's density and Strength; summed front to back with the rest.
// Each light's own colour; where two reach a cell they add; the moon's light stays the base.
// No shadows (nothing bound to the dispatch knows what stands between a lamp and a cell), no
// history: worked out anew every frame, so a flash is there for its frames only and trails nothing.
// Cheap by:
//  - tiles: the grid in TilesX x TilesY x TilesZ boxes (a work group of columns, eight slices);
//    the rasterizer puts each light in the boxes its reach touches and keeps the TileLights
//    strongest a box, as four bytes. A column reads one word every eight slices; a cell in a box no
//    light reaches does nothing more;
//  - a cell without mist or patch never looks at the lamps at all;
//  - no light this frame (and no shot): the count is 0 and the cells never enter the path;
//  - squared distances, one reciprocal a light and cell, no square root.
// How it shares the work with the lamps' glow in the scene's copy (fog_light.h's closed form, the
// halo): the halo is the glow in the CLEAR air (its own thin density) and stays; what the mist and
// the patches scatter is the grid's. So with the grid the halo's extras for a lamp standing in
// the mist go (air_light.h Mist::LampGlow, NearGlow, LampWide: a guess of what the mist adds, by
// the lamp's height over the ground): the grid has the mist's real density at every place. What
// a lamp seen THROUGH the mist gets back (Mist::FarGlow) stays: the grid dims a far fire like
// everything behind the mist, and its own cells out there are metres deep.
// Live keys lighting.grid_lamps (the strength; 0: none), lighting.grid_flash (the muzzle flash's
// share). Tests: VR_FOG_GRID_LAMPS=0 none (and the halo's extras back), =all no tiles (every misty
// cell asks every light).
namespace Lamps {
// (0.2, was 0.05 in the first pictures: the Central Yharnam lamps gave the street patch 1 to 3
// levels of 255, the woods lamp 5 to 12 round itself. The flash a share of it: at 0.05 a shot gave
// the woods bank 8 levels over the flash's own light on the ground, hard to tell.)
inline constexpr float Strength = 0.2f;
// (0.35, was 0.5 for one build: a shot at the woods lamp washed the whole view milky, the blanket
// is everywhere within the flash's 30 m.)
inline constexpr float FlashShare = 0.35f;
// A lamp's core in the mist (m^2; the halo's is 0.15 m squared): with the halo's tight core the
// mist within a metre of the Forbidden Woods lamp was a white ball at this strength. 1: the mist
// at the lamp counts as 1 m off; from 2 m on it is the game's falloff within a fifth.
inline constexpr float CoreM2 = 1.0f;
// The torch in the hunter's hand: its light reaches 8.5 m, so the air's list leaves it out as a
// fill light (fog_light.h IsFillLight) and it never had a glow in the air. The rasterizer's
// capture keeps it for the grid alone: a light the game spawned within HeldNearM of the eye, warm
// (light_shadow.h WarmShare) and at least HeldLuminance bright (the grey light the game keeps on
// the camera is neither). HeldShare of its air colour, a soft core (it is in your hand: no hot
// ball of mist at the fist). Live key lighting.grid_held.
inline constexpr float HeldNearM = 2.0f;
inline constexpr float HeldLuminance = 0.6f;
inline constexpr float HeldShare = 2.0f; // (1 with a 1 m2 core until 2026-11-15: a pale veil at the eye that washed over the torch's flame)
inline constexpr float HeldCoreM2 = 4.0f; // (flatter near the hand: the warm light shows on the mist a few metres out)
// The map's own lamps (core/vr/map_lights.h: its light file's lamp-like lights, read at start) in
// the grid's list too, with the game's fog taken over (2026-11-16, the user: "I am down to try
// that"): the frame's list leaves out a lamp that reaches further than 5.75 m (Central Yharnam's
// lit wall lanterns reach 6) and has none the game is not drawing. The nearest to the eye, a lamp
// within MapLampSameM of one the frame lists left out; MapLampShare of its colour.
// VR_FOG_MAP_LAMPS=0: none.
inline constexpr float MapLampShare = 1.0f;
inline constexpr float MapLampSameM = 1.0f;
inline bool MapLamps() {
    static const bool off = [] {
        const char* const env = std::getenv("VR_FOG_MAP_LAMPS");
        return env != nullptr && env[0] == '0';
    }();
    return FogTaken() && !off;
}
// On the game's fog taken over a lamp's light counts this many times (the bank is knee-high and
// thin: at the mist's own strength a lantern 2 m off barely showed in it; the game's own fog by a
// lamp was drawn bright). VR_FOG_LAMP_GAIN (tests).
// (1 and no cap since 2026-11-17: times 4 lifted the dim light far from the flame and the cap
// cut the bright light by it: an even warm tint over the whole bank, brown on grey fog, "even in
// forbidden woods when an enemy holding a torch goes near fog it all becomes brown. I thought we
// were having only some cells being affected like it would happen in real life". The light a
// place of fog gets is the lamp's own by the distance, less by the fog between: a glow by the
// flame, next to nothing three metres off.)
// (1.5, the user the same night: "we could make lamplight be 1.5 instead of 4": with no cap a
// factor only makes the whole glow stronger, its fall with the distance is kept.)
inline constexpr float TakenGain = 1.5f; // (6 at first, unshaped: the alley's lantern made its bank a cream blanket; 2 with the shape and the cap: through the clinic's gate the bank by the lamp stayed cold grey where the game's is warm)
// ... and it is light that travels through the fog (2026-11-16: "as if light travels
// through it and not a blanket unified color"): TakenForward of it goes by a forward phase
// (Henyey-Greenstein, TakenPhaseG: several times brighter from fog that lies between the eye and
// the lamp, a halo round it; dimmer lit from the side or from behind the viewer), the fog between
// the lamp and a place takes TakenDim of it per unit of density and metre (the place's own density:
// no march), and what a place's lamps give it together is at most TakenCap (no white-out by a lamp,
// no warm wash over a street: "not ... a sand storm"). VR_FOG_LAMP_SHAPE="forward,g,dim,cap" (tests).
inline constexpr float TakenForward = 0.7f;
inline constexpr float TakenPhaseG = 0.55f;
inline constexpr float TakenDim = 0.5f;
inline constexpr float TakenCap = 0.0f; // (0: none)
inline std::array<float, 4> TakenShapeNow() {
    static const std::array<float, 4> v = [] {
        std::array<float, 4> s{TakenForward, TakenPhaseG, TakenDim, TakenCap};
        if (const char* const env = std::getenv("VR_FOG_LAMP_SHAPE")) {
            std::sscanf(env, "%f,%f,%f,%f", &s[0], &s[1], &s[2], &s[3]);
        }
        return std::array<float, 4>{std::clamp(s[0], 0.0f, 1.0f), std::clamp(s[1], 0.0f, 0.9f),
                                    std::clamp(s[2], 0.0f, 16.0f), std::clamp(s[3], 0.0f, 64.0f)};
    }();
    return v;
}
// A lamp's light dies on its way through the fog (2026-11-17: "it still felt as if it
// spread too much.. is there some smart fall off ... some part ... becomes much brighter but then
// it falls off so not all of it gets fully lit (unless the light is truly large and strong)?").
// By the game's falloff alone (1 / d^2 to the lamp's radius) every metre from a lamp holds as much
// of its light in the fog as the metre before, the shell growing as the light thins: a lamp of
// 10 m tinted a whole bank evenly, an enemy's torch a clearing of the Forbidden Woods' mist. In
// fog light is scattered away on its way: times exp(-fall * distance), FallTakenPerM in the
// taken-over banks and FallMistPerM in the air's thinner mist, and as much more at the lamp as
// keeps the fog FallEvenM from it as bright as it was. With 0.5 a bank's light is 2.1 times at the
// flame, the same at 1.5 m, 0.47 at 3 m, 0.10 at 6 m: a bright heart, soon dark; a light ten times
// as strong gets some 4.6 m further before it is as dim. VR_FOG_LAMP_FALL="banks,mist" (per metre;
// "0,0": as it was).
inline constexpr float FallTakenPerM = 0.5f;
inline constexpr float FallMistPerM = 0.3f;
inline constexpr float FallEvenM = 1.5f;
inline std::array<float, 2> FallNow() {
    static const std::array<float, 2> v = [] {
        std::array<float, 2> f{FallTakenPerM, FallMistPerM};
        if (const char* const env = std::getenv("VR_FOG_LAMP_FALL")) {
            std::sscanf(env, "%f,%f", &f[0], &f[1]);
        }
        return std::array<float, 2>{std::clamp(f[0], 0.0f, 4.0f), std::clamp(f[1], 0.0f, 4.0f)};
    }();
    return v;
}
// ... and it gets further where the fog is much darker than the lamp (2026-11-17: "can
// you increase ... how far the light goes a tiny bit", "longer, only if it's very dark", "if the
// diff of the light of the fog or ambient vs lamp light brightness is very high"). The contrast
// at a place of fog: the lamp's own light FallEvenM from it (LightAtEven: its colour by the game's
// falloff, times the lamps' strength) over the fog's own light there (its colour times what the
// moon and the air give it, for the same amount of fog). Under FarLo nothing changes; from FarHi
// on the light dies past FallEvenM at FarKeep of the rate only (FarOf between them). Within
// FallEvenM of the lamp nothing changes in any fog: the bright heart is the same.
// (With 0.5 a metre and 0.6 of it kept: 0.64 of the light left 3 m off for 0.47, 0.26 at 6 m for
// 0.10. In the Forbidden Woods the mist under the trees has a seventh of the moonlit mist's own
// light: a lantern's contrast there is some 30, in the moonlight 4; the Hunter's Dream's lamps
// stand at 3 to 8 over their banks. 8 and 32 by those.)
// VR_FOG_LAMP_FAR="keep,lo,hi" ("1": as it was).
inline constexpr float FarKeep = 0.6f; // (0.8 could not be told from none in the Woods' pictures)
inline constexpr float FarLo = 8.0f;
inline constexpr float FarHi = 32.0f;
inline std::array<float, 3> FarNow() {
    static const std::array<float, 3> v = [] {
        std::array<float, 3> f{FarKeep, FarLo, FarHi};
        if (const char* const env = std::getenv("VR_FOG_LAMP_FAR")) {
            std::sscanf(env, "%f,%f,%f", &f[0], &f[1], &f[2]);
        }
        f[0] = std::clamp(f[0], 0.0f, 1.0f);
        f[1] = std::clamp(f[1], 0.0f, 1e6f);
        f[2] = std::clamp(f[2], f[1] + 1e-3f, 2e6f);
        return f;
    }();
    return v;
}
// The lamps' light in the fog levels off as the fog thickens (2026-11-20, of a lamp by
// the Oedon Chapel urns whose glow grew to a bright ball each time its bank swelled, Breath: "I do
// want lights to light up fog though... just to not pile up"). A place of fog gives the lamp light
// it scatters times its density, so a bank swelled to three times its thin self made the glow three
// times as bright, a ball with no flame in it. The light a ray gathers from the lamps is SoftLimit
// * (1 - exp(-light / SoftLimit)): thin fog by a lamp as before (the curve starts at the light
// itself), a thick bank's glow towards SoftLimit, never past it; its hue kept.
// Two limits (the user, the same day: "if there is an actual lamp (like if there is a halo there)
// it's ok if the limit goes higher up. But if there is no halo and it's just atmospheric, then it
// needs to have a cap"): SoftLimit for a bare lamp's light (the map's lamps the frame's list does
// not hold: no halo of ours), SoftHaloLimit for the lamps the frame's list gives the grid (each
// with its halo: air_light.h Sources), the light in your hand and a shot's flash. Set by the
// user at the Oedon Chapel urn lamp: "HALOED should be uncapped, bare should be max 1.6".
// VR_FOG_LAMP_SOFT="bare,halo" (0: no limit; "0,0": as before).
inline constexpr float SoftLimit = 1.6f;
inline constexpr float SoftHaloLimit = 0.0f;
// The Forbidden Woods' bare lamps (the user, the same day: "make forbidden woods always be at ...
// max, and everywhere else at 1.6", then "capped to 4"): its mist under the trees is dark and its
// lamps and lanterns light it as a glow the woods are known by. Area 0x1B (core/vr/game_area.h).
inline constexpr float SoftWoodsLimit = 4.0f;
inline constexpr u32 SoftWoodsArea = 0x1B;
// The two limits where the hunter is (`map`: (area << 8) | block, the fog patches' map).
// VR_FOG_LAMP_SOFT="bare,halo" (tests) for every area.
inline std::array<float, 2> SoftFor(u32 map) {
    static const std::array<float, 2> pinned = [] {
        std::array<float, 2> s{-1.0f, -1.0f};
        if (const char* const env = std::getenv("VR_FOG_LAMP_SOFT")) {
            std::sscanf(env, "%f,%f", &s[0], &s[1]);
            s = {std::clamp(s[0], 0.0f, 64.0f), std::clamp(s[1], 0.0f, 64.0f)};
        }
        return s;
    }();
    if (pinned[0] >= 0.0f) {
        return pinned;
    }
    return {(map >> 8) == SoftWoodsArea ? SoftWoodsLimit : SoftLimit, SoftHaloLimit};
}
// How much of the further reach a place of fog gets by the contrast (the shader's LampFurther).
inline float FarOf(float contrast, float lo, float hi) {
    const float t = std::clamp((contrast - lo) / std::max(hi - lo, 1e-6f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
// A lamp's own light FallEvenM from it: its colour's luminance by the game's falloff (`reach`:
// 1 / its radius squared).
inline float LightAtEven(float r, float g, float b, float reach, float core2) {
    return (0.3f * r + 0.6f * g + 0.1f * b) * std::max(1.0f / (FallEvenM * FallEvenM + core2) - reach, 0.0f);
}
// What the fog's dying leaves of a lamp's light `away` from it, against what it leaves FallEvenM
// from it (`share`: FarOf at the place): tests, the header's numbers.
inline float FallLeft(float away, float per_m, float keep, float share) {
    return std::exp(-per_m * (away - FallEvenM) + per_m * (1.0f - keep) * share * std::max(away - FallEvenM, 0.0f));
}
inline float TakenGainNow() {
    static const float v = [] {
        const char* const env = std::getenv("VR_FOG_LAMP_GAIN");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.0f, 64.0f) : TakenGain;
    }();
    return v;
}
inline int Mode() {
    static const int mode = [] {
        const char* const env = std::getenv("VR_FOG_GRID_LAMPS");
        return env == nullptr ? 1 : env[0] == '0' ? 0 : env[0] == 'a' ? 2 : 1;
    }();
    return mode;
}
} // namespace Lamps

// The buffer the scene's copy reads (BufferType::VrFogGrid; written by our dispatch, never by a
// game shader: declared not written, so native stereo makes no right-eye twin of it and both
// eyes read the one buffer): a header of HeaderDwords, then CellDwords a cell, by row, column,
// slice (a column's cells lie together: its thread writes one stretch of memory, and the two
// slices a pixel blends are neighbours).
//  header: 0 cells x  1 y  2 z  3 the frame's tag (0: nothing built: the copy adds no mist)
//          4..7 the frustum (vec4 1 above)  8..11 the slices (vec4 2)  12 the half shift  13..15 0
//  cell:   word 0: red (bits 0..15), green (16..31)   word 1: blue (0..15), the air's
//          transmittance (16..31). The colours as sqrt(light / MaxLight) * 65535 (the cells' own
//          code, air_light.h: fine steps in the dark, where this game lives), the transmittance
//          * 65535. All "from the eye to this slice's far face".
// Looking a depth z up: slice coordinate s = cells z * log2(z / near) / log2(far / near); the
// value AT s = k + 1 is cell k's, so the blend along the depth is between cells floor(s - 1) and
// the next, and below s = 1 between "no air" (light 0, transmittance 1) and cell 0.
inline constexpr u32 HeaderDwords = 16;
inline constexpr u32 CellDwords = 2;
inline constexpr float MaxLight = 64.0f;
inline constexpr u64 BufferDwords(u32 x, u32 y, u32 z) {
    return HeaderDwords + u64(CellDwords) * x * y * z;
}
// The stand-in until the first build (a buffer of its own): a header with tag 0, two cells a side.
inline constexpr u32 NoneDwords = HeaderDwords + 8 * CellDwords;
// Memory at 160 x 90 x 64: 7.0 MiB, and two pictures of the lit share (16-bit float): 1.8 MiB each.

} // namespace Shader::Optimization::FogGrid
