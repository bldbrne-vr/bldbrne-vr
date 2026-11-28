// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>

#include "common/types.h"

// Light shadows (2026-11-09: "Imagine our hand lantern and torch cast real shadows",
// then "Do them well" and "up a bit the range, so that we see more shadows eg of approaching
// enemies"). Bloodborne adds its point lights in a light pre-pass: one full-screen rectangle per
// group of up to 12 lights, each light 16 dwords of a constant buffer (its view-space position at
// 0..2, its reach at 3, its colour at 4..6, its specular colour at 8..10), the pixel's view
// position rebuilt from the depth with another buffer's ndc-to-view matrix (rows at 0..15). Its
// lanterns, torches and gun flashes cast no shadows. LightShadowPass finds that shape in a light
// rectangle's fragment stage (RuntimeInfo::vr_light_shadows) and multiplies each light's colours
// by VrLightVisibility: a ray from the pixel towards the light, stepped through the eye's depth.
// Only lights near the eye (the one in your hand, a torch beside you) march: the room's lamps keep
// the game's look and cost nothing, which also keeps the many lights of a lit hall cheap.
namespace Shader::Optimization::LightShadow {

// Startup policy shared by compilation and cache preloading (vr.ini light_shadows,
// VR_LIGHT_SHADOWS=0|1 overrides it): only light rectangles' fragment stages take it. With an
// upscaler or without (until 0.9.68 with one only, the tester, 2026-11-09: "too expensive at
// max/high res"; then 2026-11-19, the user: "we need to make these work with upscaling off as
// well (no need not to)").
inline bool Enabled(bool vr, bool setting, const char* override_value) {
    return override_value ? override_value[0] != '0' : vr && setting;
}

// A light marches when it is this close to the eye (view-space metres): the lantern or torch in
// your hand, a gun's flash, a torch-bearer at your side, and (2026-11-09: "turn up
// the strength ... so I can clearly see them") the lamps, fires and candles around you.
// Its shadows fade in over the last NearFadeM before that (walking up to a fire, its light never
// switches: the tester, 2026-11-09, "when I approach the fire ... its glow goes away").
// 22 m with a 4 m fade (2026-11-10: "extend their range so they don't have crazy pop
// in, and just vary their fidelity so they get more accurate the closer we are", 20 m; then
// "up them to 22m"; was 7 and 1):
// past DetailFarM the far mode below makes them cheap.
inline constexpr float NearLightM = 22.0f;
// A light casts shadows unless it is a fill: dimmer than FogLight::FillLuminance (the grey light
// in front of the camera, the moonlight's blues) or wider than ShadowFillReachM (the moonlight's
// fills reach 21 to 25 m). Not the fog's FillReachM (5.75 m): the carried torch reaches 8.4 m and
// the street lamps 6 to 10 m (light list audit, Central Yharnam, 2026-11-12); with it they cast
// no shadows at all, the hunter's own included.
inline constexpr float ShadowFillReachM = 12.0f;
// ... which is the rule for grey and blue lights only (2026-11-12, the user: "I see lights in game
// that light up areas, and should cast shadows, not cast anything"; the Grand Cathedral's room is
// lit by dim warm lights, lum 0.34, r 5 m). A warm light (its red over its blue by WarmShare: a
// flame's, a lamp's) casts at any brightness from DimMinLuminance and any reach to WarmReachM; a
// dim one (under FillLuminance) only near itself (the tester: "a faint short light ... shadows only
// near it"): where its falloff is above 1 - DimNearShare^2 (within that share of its reach), with
// half the steps. The fakes stay out: the game's grey light on the camera (within HeldInM of the
// eye) and the blue and grey fills (dim or wide). VR_LIGHT_RULE_OLD=1 (tests): bright and narrow
// only, as before.
inline constexpr float WarmShare = 1.15f;
// (0.06 and 1.0 since 2026-11-12 evening, the user in the headset: "the flickering shadows ... can
// make shadows disappear and re-appear ... I am ok if the light itself flickers but the shadows
// should only ever so slightly change", and small lights' shadows "terminate a few cm": a light's
// shadows no longer depend on its brightness at all above a floor no flame flickers down to, and
// cover all of its reach, fading out with its own falloff (DimBand) where its light ends. The
// flicker then only changes how dark they are, with the light. Were 0.15 and 0.5.)
inline constexpr float DimMinLuminance = 0.06f;
inline constexpr float DimNearShare = 1.0f;
inline constexpr float WarmReachM = 20.0f;
// None of it switches (2026-11-12: the game flickers every flame's brightness smoothly, the Grand
// Cathedral's candles of 1.5 m between 0.54 and 0.77 by the light list audit: at a threshold of
// 0.6 their shadows came and went with the flicker). A light's shadows have a strength:
//  - a warm light's comes in over DimFadeLuminance above DimMinLuminance; the share of its reach
//    they cover grows from DimNearShare (at DimShareLuminance and dimmer) to all of it (at
//    FogLight::FillLuminance), and they fade out over the last DimBand of the falloff before
//    that share's end (and before the light's own rim, where they used to end in a line);
//  - a grey or blue light's comes in over BrightFadeLuminance below FillLuminance;
//  - a dim light takes half the steps below DimStepsLuminance (the count is not seen).
// The game's grey light on the camera (within HeldInM of the eye) is a grey light's case only:
// a torch or a lantern held closer to the eye than that is warm, and casts (until 2026-11-12
// every light within HeldInM was left out: raising the torch to your face put its shadows out).
inline constexpr float DimFadeLuminance = 0.06f;
inline constexpr float DimShareLuminance = 0.30f;
inline constexpr float DimBand = 0.15f;
inline constexpr float BrightFadeLuminance = 0.10f;
// ... and a grey or blue light the game spawns (a carried lantern): in full from
// FogLight::BrightKeepLuminance, in over SpawnedFadeLuminance below it, so its flicker across
// FillLuminance (the cane men's violet lanterns, 0.57..0.63) no longer swings its shadows.
inline constexpr float SpawnedFadeLuminance = 0.05f;
inline constexpr float DimStepsLuminance = 0.45f; // (unused since the evening)
// (Since 2026-11-12 night, the user in the headset: small lights' shadows "terminate a few cm and
// they seem like they are flying, not falling on the floor": a thin thing (a candle stand, a
// messenger, a railing) was found only where it met the surface, where the steps are close; a few
// cm out the steps were too far apart to land on it, so its shadow stopped there, and the two
// eyes found different pieces of it. Now:
//  - a small light takes all the steps (it took half: SmallReachM 3 m, this evening's);
//  - the steps follow the square root of the light's falloff (they fell to MinSteps at its rim,
//    exactly where a shadow cast from near the light lands);
//  - the fixture's clearance (below, ClearFixtureM) is ClearPerReachM a metre of the light's
//    reach, from ClearSmallM to ClearCapM (the tester: "cap it at 32cm, and have it go as low as
//    6cm for small lights"): a candle of 1.5 m clears 6 cm, the Grand Cathedral's room lights
//    and a fire of 5 m 20 cm, a street lamp of 8 m and up 32 cm. Was ClearFixtureM's 0.35 m at
//    least and ClearPerRadius of the reach: nothing within a forearm of a candle shadowed, nor
//    within 0.9 to 1.5 m of a street lamp's flame (its bracket, a railing or an enemy under it).
//    Then 12 to 72 cm, 9 cm a metre (the same night, in the headset at 6 to 32 cm: whole-view
//    flashes as a lamp came into and out of view, its own cage and housing throwing a shadow
//    over the street only while on screen; and lights deep in chandeliers went dark; the user:
//    "Maybe we do 12cm - 72cm"): a candle clears 13.5 cm, a fire of 5 m 45 cm, a street lamp of
//    8 m and up 72 cm.
// VR_LIGHT_REACH_OLD=1 (tests): as this evening. VR_LIGHT_CLEAR_MIN, _MAX and _PER=<m> (tests):
// ClearSmallM, ClearCapM and ClearPerReachM.)
inline constexpr float SmallReachM = 3.0f;
// Small lights grouped for shadows (2026-11-12 night, the user: "for shadows many small nearby
// lights should be grouped"; the chalice dungeon's rows of candles): in one lamp shader a light
// of at most SmallReachM within GroupM of an earlier such light that marched takes that march's
// visibility (its own strength and fade still apply): one test where a candle cluster made many.
// OFF by default since the same night's measurements: keeping every earlier light's place and
// result alive for the later ones costs registers, so fewer pixels run at once, and the pass was
// SLOWER with it (the dungeon 1.84 -> 2.36 ms, Central Yharnam's first lamp 2.41 -> 2.97).
// VR_LIGHT_GROUP_M=<m> (tests; 0, the default: none).
inline float GroupM() {
    static const float m = [] {
        const char* const env = std::getenv("VR_LIGHT_GROUP_M");
        return env != nullptr ? float(std::atof(env)) : 0.0f;
    }();
    return m;
}
inline constexpr float ClearSmallM = 0.12f;
// The light in the air keeps the old clearance (ClearFixtureM, ClearPerRadius of the reach): at
// 6 to 32 cm the hand, arm and stick of a carried torch (8.4 m: 1.26 m before, 0.32 m then) cut
// a thick dark shape into its glow round the flame (the tester: "my torch had a thick shadow
// around it, which felt floating up in the air"). Documentation only: FixtureClear's `air`.
inline constexpr bool ClearAirOld = true;
inline constexpr float ClearCapM = 0.72f;
inline constexpr float ClearPerReachM = 0.09f;
inline bool ReachOld() {
    static const bool old = [] {
        const char* const env = std::getenv("VR_LIGHT_REACH_OLD");
        return env != nullptr && env[0] == '1';
    }();
    return old;
}
inline float ClearEnv(const char* name, float fallback) {
    const char* const env = std::getenv(name);
    return env != nullptr ? float(std::atof(env)) : fallback;
}
inline float ClearSmall() {
    static const float m = ClearEnv("VR_LIGHT_CLEAR_MIN", ClearSmallM);
    return m;
}
inline float ClearCap() {
    static const float m = ClearEnv("VR_LIGHT_CLEAR_MAX", ClearCapM);
    return m;
}
inline float ClearPerReach() {
    static const float m = ClearEnv("VR_LIGHT_CLEAR_PER", ClearPerReachM);
    return m;
}
// A fire's pile (2026-11-16, the user, Old Yharnam before the Blood-starved Beast: the flames'
// "shadows ... appear and disappear as I go near them"): a pyre's light sits low inside its own
// pile of sticks, wider than the 45 cm a fire of 5 m cleared. From afar the march's few steps miss
// the pile and the ground round the fire is lit; walking up, more and more of them land on it, and
// the fire's own sticks shadow the ground in patches. The fire's light left on the ground at 4 m,
// 3 m and 1.5 m from it, of the picture without lamp shadows (both eyes, +-4 by the flicker): 88,
// 85, 83% at 45 cm; the same at 52; 89, 89, 89% at 58; 88, 94, 92% at 66; 87, 91, 93% at 72. The
// code cannot tell a fire from a lamp of the same reach, so the user, not to change other lights:
// "I would make it 46cm and leave it there" (a step with nothing to see, by the numbers above; a
// larger one is this one constant, or VR_LIGHT_CLEAR_FIRE=<m> in tests). The clearance rises from
// a small light's (at SmallReachM, by ClearPerReachM: 27 cm) through ClearFireM at ClearFireReachM
// and on to the cap: a fire of 5 m clears 46 cm (was 45), a light of 4 m 36.5 cm (36), of 6 m
// 55.5 cm (54), of 7 m 65 cm (63), the cap from 7.7 m (the most any light gains: 2.4 cm, there).
// Candles and small lamps (to 3 m) and the street lamps (8 m and up) keep what they had. (0.45:
// the rule of before, by the reach alone.)
inline constexpr float ClearFireReachM = 5.0f;
inline constexpr float ClearFireM = 0.46f;
inline float ClearFire() {
    static const float m = ClearEnv("VR_LIGHT_CLEAR_FIRE", ClearFireM);
    return m;
}
// (The rise's slope and its value at reach 0; never under ClearPerReach, so below SmallReachM the
// line by the reach is the larger and a fire's clearance set at or under that line changes nothing.)
inline float ClearRisePerM() {
    return std::max((ClearFire() - ClearPerReach() * SmallReachM) / (ClearFireReachM - SmallReachM),
                    ClearPerReach());
}
inline float ClearRiseAt0() {
    return (ClearPerReach() - ClearRisePerM()) * SmallReachM;
}
// A fixture's clearance by its light's reach (metres): the lamp shaders' FixtureClear and the
// shadow slots' (Rasterizer) are this.
inline float FixtureClearM(float reach) {
    return std::clamp(std::max(ClearPerReach() * reach, ClearRiseAt0() + ClearRisePerM() * reach),
                      ClearSmall(), ClearCap());
}
// Both eyes (2026-11-12 night, the user: "we can end up with 1 eye seeing the light and the other
// not. Can we make sure that if 1 eye does, the other one also always acts as if it sees it?"):
// a ray's step off this eye's picture is looked up in the other eye's (native stereo with both
// eyes in one draw binds both eyes' depths and matrices), so a lamp or a blocker that either eye
// sees shadows in both, and the edge fade below is where neither eye sees. The other eye's view
// is this eye's moved by the difference of the light's two positions (each eye's light list is
// its own view's). VR_LIGHT_CROSS_EYE=0 (tests): each eye its own picture, as before.
inline bool CrossEye() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_LIGHT_CROSS_EYE");
        return env == nullptr || env[0] != '0';
    }();
    return on;
}
// Lights the game keeps on the camera (2026-11-12 night, the user at a gate in Central Yharnam:
// "the shadow is POINTING AT US ... when I move to the right, it moves to the left"): the game
// fixes some point lights to its camera, one 0.17 m right, 2.0 m up and 4.1 m ahead of the eye,
// bluish white, 2 m of reach, bright (a light on what you look at). It cast the shadows of the
// things before it towards the eye, and they swung with every step. Rasterizer::VrFogCapture
// finds them: a light more than AttachedFromM from the eye whose place in the camera's view is
// the same (within AttachedSameM) after the camera has moved AttachedMoveM or turned
// AttachedTurn (a world-fixed light would have moved); up to AttachedMax of them, in the body
// buffer (body_shadow.h AttachedAt). They light as ever and shadow nothing. Within
// AttachedFromM: the torch, the Hand Lantern, a gun's flash (held, they cast; the tester: "torch
// or hand lantern or gun muzzle flashes being an exception").
inline constexpr float AttachedFromM = 1.2f;
inline constexpr float AttachedSameM = 0.02f;
inline constexpr float AttachedMoveM = 0.10f;
inline constexpr float AttachedTurn = 0.05f; // of a rotation entry (about 3 degrees)
// (2 cm after the camera moved 10 cm or turned 3 degrees: a light the game sets on its camera
// each frame may wobble by millimetres; a world-fixed one beyond AttachedFromM moves 6 cm and more.)
inline constexpr u32 AttachedMax = 4;
// (Since the same night's third look: a cell per place (AttachedSameM), seen AttachedSightings
// frames; up to AttachedCandidates cells, forgotten AttachedForgetFrames after last seen. The
// world's lamps leave a cell at every place they pass while the camera moves: a short table
// lost the light on the camera, listed only a few frames in many, before it was seen thrice.)
inline constexpr u32 AttachedSightings = 3;
inline constexpr u32 AttachedCandidates = 16384;
inline constexpr u64 AttachedForgetFrames = 240;
// The body buffer's slots (body_shadow.h AttachedAt): AttachedCameraSlots for the lights on the
// camera (removable), the rest for NoCastLights seen this frame (they light, they cast nothing).
inline constexpr u32 AttachedCameraSlots = 2;
// Lights that cast no shadow, one by one (the user's picks, 2026-11-12: "just this one, because
// there might be other lights like this that would be cool if they did"): where each is in the
// world (game units), its reach dword and colour, matched on the CPU (each listed light against
// these, within NoCastSameM and NoCastShare) into a slot the shaders already test: no cost there.
struct NoCastLight {
    float x, y, z;
    float k;
    float r, g, b;
};
inline constexpr NoCastLight NoCastLights[] = {
    // Central Yharnam, the first lamp's gate: a moon fill (a map light: reach 5 m, blue, no
    // source), 2.5 m ahead of the camera there; its shadow of the gate slid as you strafed.
    {-197.816f, -26.460f, 59.449f, 0.04f, 0.605f, 1.027f, 1.5f},
};
inline constexpr float NoCastSameM = 0.5f;
inline constexpr float NoCastShare = 0.03f;
// Tests (VR_LIGHT_PROBE=1, when the shaders are made): the live keys lighting.mute_cast and
// lighting.nudge_* (body_shadow.h ProbeAt) reach the shaders; without it they emit nothing.
inline bool Probe() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_LIGHT_PROBE");
        return env != nullptr && env[0] == '1';
    }();
    return on;
}
// The blocker grid (2026-11-12 night, the user, after shadows went as he turned his head: "do
// screen only but keep a short 3d grid ... around us"): a box of SideX x SideY x SideZ voxels of
// VoxelM, y up, in the world, wrapping (a voxel's cell is its coordinates modulo the sides, with
// 4 bits a side of the rest as its tag), so it follows him with nothing moved. Each voxel is a
// word: its tag (12 bits) and the stamp of the frame it was last seen solid (20 bits; 0: empty).
//  - Fill: the lamp programs themselves, once a pixel (a quarter of the pixels a frame, by
//    FillEvery): the voxel of the surface the pixel shows is stamped, and CarvePoints points of
//    the empty air between the eye and it are cleared (only a voxel no pixel stamped this frame).
//    Not within NearSkipM of the eye (his arms, his weapons), nor inside his body's sphere, nor
//    beyond ReachM (the box's half side).
//  - Use: a ray's step that neither eye sees is looked up in the grid (and the midpoint since
//    the last step); a voxel stamped within LifeS blocks it. A wall or post seen a moment ago
//    keeps its shadow after it leaves the view; a thing never seen casts none.
// 128 x 64 x 128 words: 4 MiB. VR_LIGHT_GRID=0 (tests): none.
namespace Grid {
inline constexpr u32 SideX = 128, SideY = 64, SideZ = 128; // powers of two
inline constexpr u32 ShiftX = 7, ShiftY = 6, ShiftZ = 7;
inline constexpr u32 Voxels = SideX * SideY * SideZ;
// Block stamps (2026-11-13, VR_MARCH_OPT 128; the lookups cost 0.09 of the mask pass's 0.36 ms at
// Oedon): after the cells, one word a 4 x 4 x 4 block, the stamp of its last fill. A lookup reads
// its block first (64 KB: the caches keep it) and a cell only where the block is live: a live
// cell's block was stamped when it was or later.
inline constexpr u32 CoarseShift = 2;
inline constexpr u32 CoarseSideX = SideX >> CoarseShift, CoarseSideY = SideY >> CoarseShift,
                     CoarseSideZ = SideZ >> CoarseShift;
inline constexpr u32 CoarseWords = CoarseSideX * CoarseSideY * CoarseSideZ;
inline constexpr u32 BufferWords = Voxels + CoarseWords;
inline constexpr float VoxelM = 0.10f;
inline constexpr float ReachM = 6.0f;
inline constexpr float NearSkipM = 1.2f;
inline constexpr float LifeS = 20.0f;
inline constexpr float FramesPerS = 90.0f;
inline constexpr u32 StampBits = 20;
inline constexpr u32 StampMask = (1u << StampBits) - 1u;
inline constexpr u32 FillEvery = 4;
inline constexpr u32 CarvePoints = 3;
// A voxel stamped within this many frames is not cleared by the air a pixel looks through (a
// thin thing beside that ray, stamped by its own pixels one frame in FillEvery, stays).
inline constexpr u32 CarveAfter = 30;
inline bool Enabled() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_LIGHT_GRID");
        return env == nullptr || env[0] != '0';
    }();
    return on;
}
// Tests (the cost's split): VR_LIGHT_GRID_FILL=0 makes no fill, VR_LIGHT_GRID_LOOK=0 no lookups.
inline bool FillOn() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_LIGHT_GRID_FILL");
        return env == nullptr || env[0] != '0';
    }();
    return on;
}
inline bool LookOn() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_LIGHT_GRID_LOOK");
        return env == nullptr || env[0] != '0';
    }();
    return on;
}
// The grid's fallback made careful (2026-11-13 evening, the user: shadows "at the edges of the
// screen" while moving, gone with the grid's lookups off; a code review): a step's grid cells
// count only off the receiver's own surface (SurfaceVoxels voxels above its plane, or SurfaceVoxels
// + 1 along the ray where its normal is unsure: a floor no longer shades itself through its own
// 10 cm cells); the grid also within the picture's edge band, weighted as the screen's hits fade
// there (no jump from soft to full at the edge); and a step's cell and the one halfway back both
// solid for a full hit, one of them half. VR_LIGHT_GRID_SOFT=0: the fallback as before.
inline bool Soft() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_LIGHT_GRID_SOFT");
        return env == nullptr || env[0] != '0';
    }();
    return on;
}
inline constexpr float SurfaceVoxels = 1.5f;
} // namespace Grid
// Shadow maps from the view's depth (2026-11-12 night, the user: "We take the depth map we render
// for the player's view ... from light's perspective shadow it as a 2d image ... without ray
// casting and steps", then "insanely and mindblowingly fast"): Slots lamps a frame (the CPU's
// pick from the last frame's lamps: the share of the view each lights), a cube of Side texels a
// face round each, its axes the flat view's, holding the nearest surface's distance (float bits:
// atomicMin). The game's AO pass (half the resolution, both eyes, before the lamp pass) writes
// every pixel it sees into the cubes whose lamp reaches it; a lamp draw's light that is a slot's
// looks its cube up (a 3x3 test) instead of marching. Real surfaces: no thickness guessed.
// On by default since 2026-11-17, with the mask (Mask below): the lamp shadows' Low setting (the
// user, asking for a High one: LightShadow::High). Until then only with VR_SCENE_MAPS=1 and
// VR_SHADOW_MASK=1, every lamp's own march without them. VR_SCENE_MAPS=0 (tests): that march.
namespace SceneMaps {
inline constexpr u32 Slots = 16; // (24 at 128 texels until 2026-11-13 04:00; 16 at 192: sharper, the same memory)
inline constexpr u32 Side = 192;
inline constexpr u32 FaceTexels = Side * Side;
inline constexpr u32 SlotTexels = 6 * FaceTexels;
inline constexpr u32 Texels = Slots * SlotTexels;
inline constexpr u32 Empty = 0x7F7FFFFFu; // the largest float: no surface there
// A receiver finds its own surface in its lamp's cube: blocked only when the cube's surface is
// nearer the lamp than its own by BiasM plus BiasTexels of a texel's size at its distance.
inline constexpr float BiasM = 0.02f;
inline constexpr float BiasTexels = 1.5f;
// ... and SlopeTexels more of a texel's size per unit of the surface's slope to the light (up to
// SlopeMax): a floor seen low from its lamp would shade itself in stripes (its own neighbouring
// texels lie nearer the lamp). Uncapped since 2026-11-13 dawn (20: the cosine's clamp at 0.05):
// at 4 a floor lit edge-on shaded itself, and the edge-on fade that hid it took the real shadows
// of a fire's logs on the ground with it (Old Yharnam). A grazing ray's bias grows with its slope;
// what blocks a light that low lies near the light, well short of it.
inline constexpr float SlopeTexelsDefault = 2.0f;
inline float SlopeTexels() {
    static const float v = [] {
        const char* const env = std::getenv("VR_SCENE_SLOPE");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.0f, 8.0f) : SlopeTexelsDefault;
    }();
    return v;
}
// Tests (2026-11-13 evening, the user: "render every shadow of every light in a different color.
// then we will see which light triggers it"): VR_SHADOW_TINT=1 (with the mask): where a slot's lamp
// is blocked its light keeps only its slot's colour (TintColours) instead of going dark, so each
// lamp's shadows show in its own colour. The lamp pass asks a light's visibility once a channel.
inline bool Tint() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_SHADOW_TINT");
        return env != nullptr && env[0] == '1';
    }();
    return on;
}
inline constexpr float TintColours[16][3] = {
    {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.3f, 1.0f}, {1.0f, 1.0f, 0.0f},
    {1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 1.0f}, {1.0f, 0.5f, 0.0f}, {0.5f, 0.0f, 1.0f},
    {0.5f, 1.0f, 0.0f}, {1.0f, 0.3f, 0.6f}, {0.0f, 0.6f, 0.5f}, {0.3f, 0.7f, 1.0f},
    {0.6f, 0.4f, 0.0f}, {0.8f, 0.0f, 0.3f}, {0.5f, 1.0f, 0.7f}, {0.7f, 0.5f, 1.0f}};
// (A VrLightVisibility's light immediate carries the channel asked, 1..3, from TintChannelShift.)
inline constexpr u32 TintChannelShift = 16;
// Tests: VR_SHADOW_MASK_SHOW=1 draws the mask (its darkest slot) as the AO's darkness.
inline bool MaskShow() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_SHADOW_MASK_SHOW");
        return env != nullptr && env[0] == '1';
    }();
    return on;
}
inline constexpr float SlopeMaxDefault = 20.0f;
inline float SlopeMax() {
    static const float v = [] {
        const char* const env = std::getenv("VR_SCENE_SLOPE_MAX");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 1.0f, 20.0f) : SlopeMaxDefault;
    }();
    return v;
}
// The looked-up point lifted off its own surface (along its normal, towards the light's side) by
// NormalTexels of a texel's size there, at least NormalMinM: a surface never finds itself in its
// lamp's cube (the speckles on curved stone).
inline constexpr float NormalTexels = 1.5f;
// A shadow fades out as its light comes in edge-on (cos to the surface's normal from GrazeIn down
// to GrazeOut: only the last few degrees since the slope bias went uncapped; 0.03..0.2 took a
// ground fire's shadows); and over its light's last DimBand of falloff (the march's rule).
inline constexpr float GrazeOutDefault = 0.0f, GrazeInDefault = 0.03f;
// (Tests: VR_SCENE_GRAZE=out,in.)
inline std::pair<float, float> Graze() {
    static const std::pair<float, float> v = [] {
        const char* const env = std::getenv("VR_SCENE_GRAZE");
        float out = GrazeOutDefault, in = GrazeInDefault;
        if (env != nullptr) {
            out = std::clamp(static_cast<float>(std::atof(env)), 0.0f, 0.5f);
            const char* const comma = std::strchr(env, ',');
            in = comma != nullptr ? std::clamp(static_cast<float>(std::atof(comma + 1)), out + 0.005f, 1.0f) : out + 0.1f;
        }
        return std::pair{out, in};
    }();
    return v;
}
inline constexpr float NormalMinM = 0.01f;
// The mask fades a lamp over the last fifth of its range from the eye. A 2 m candle
// casts to 8 m on every tier. Large lights reach 22 m on Low and Medium, 23.5 m on High.
inline constexpr float RangePerReach = 4.0f;
inline constexpr float RangeMinM = 6.0f;
inline constexpr float RangeFadeShare = 0.2f;
constexpr float MaskedLightRangeM(float reach_m, u32 tier) {
    return std::clamp(reach_m * RangePerReach, RangeMinM, tier == 2 ? 23.5f : NearLightM);
}
inline constexpr float MatchM = 0.02f; // a lamp is a slot's within this (and 2% of its reach)
// Contact shadows (2026-11-13 dawn, Old Yharnam against the old march: grass and fern fronds near
// the eye shading themselves and the ground; a cube's texel, 8 cm at a fire's distance, never holds
// a frond): the mask pass marches ContactSteps depth taps over ContactM towards the one lamp that
// lights the pixel the most (its strength, falloff and facing, over ContactMinWeight); a surface
// just in front of the ray (by more than the pixel's own footprint, less than ContactThickM +
// ContactThickPerM a metre of its distance) shades it for that lamp. One short march a pixel.
inline constexpr unsigned ContactSteps = 6;
inline constexpr float ContactM = 0.3f;
inline constexpr float ContactThickM = 0.08f, ContactThickPerM = 0.01f;
inline constexpr float ContactMinWeight = 0.03f;
// The trace (2026-11-13, the user: "the tracing approach was better... IF WE CAN MAKE IT FAST... so
// maybe we combine the two"): the same march, longer, for the top lamp, this frame's depth (no lag
// on a snap turn); tests VR_SCENE_TRACE_STEPS, VR_SCENE_TRACE_M. Past half a metre its steps crowd
// near the surface (the square of their share) and a blocker counts TraceThickPerM thicker for each
// metre along the ray (the old march's thick blockers).
inline constexpr float TraceThickPerM = 0.15f;
// The full trace's blockers at least TraceThickM thick (the old march's 40 cm: the sewers' drain block,
// half a metre deep, seen from the front: rays near the wall passed "behind" its face and its shadow
// on the wall began a hand's width away from it, lit between).
inline constexpr float TraceThickM = 0.3f;
// The trace runs in the foveated pass's coarse blocks too (one trace a block: the outer view
// same-frame, fewer rays there; on since 2026-11-13 evening with trace-first, the user: "full
// tracing but fire less rays in the edges"). VR_SCENE_TRACE_COARSE=0: where it shades every pixel only.
inline bool TraceCoarse() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_SCENE_TRACE_COARSE");
        return env == nullptr || env[0] != '0';
    }();
    return on;
}
// The smart trace (2026-11-13 evening, the user: "fire a ray if it doesn't hit anything the next ray
// is further away. if it hits something the next ray is closer. Like a smart search"): TraceSteps
// steps that grow geometrically from the pixel out to SmartM (or short of the lamp), so a ray
// reaches far with the same steps; a step that goes from in front of the screen's surface to behind
// it is a crossing, and SmartRefine halvings between the two find where; it is a hit, the light all
// gone, only where that crossing is within TraceThickM (plus the last interval) of the surface: a
// ray passing behind something far in front of it (a railing near the eye) is not. No half-light
// hits: a patch is in shadow or not. VR_SCENE_SMART=0: the even steps of before.
inline bool Smart() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_SCENE_SMART");
        return env == nullptr || env[0] != '0';
    }();
    return on;
}
inline constexpr u32 SmartRefine = 3;
inline float SmartM() {
    static const float v = [] {
        const char* const env = std::getenv("VR_SCENE_SMART_M");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.5f, 30.0f) : 10.0f;
    }();
    return v;
}
// The steps' offset along the ray (2026-11-13 evening, the user: "YES - let's do it"): tied to the
// world, not the screen: a hash of the pixel's world cell (JitterCellM, its true world place, the
// camera's position in it), so turning or moving the head leaves the pattern on the floor instead
// of sliding it across. Tests: VR_SCENE_JITTER=0 none (half a step), 1 the screen's (before), 2 the
// world's (default).
inline constexpr float JitterCellM = 0.03f;
inline u32 Jitter() {
    static const u32 v = [] {
        const char* const env = std::getenv("VR_SCENE_JITTER");
        return env != nullptr ? std::clamp(static_cast<u32>(std::atoi(env)), 0u, 2u) : 2u;
    }();
    return v;
}
// The old march in the mask (2026-11-13 evening, the user: "we should just fix the old method and
// make it super performant while maintaining quality"; a review: put the earlier tracer's visibility
// into the shared mask unchanged first): each slot lamp reaching a pixel is marched by March (the
// lamp shaders' march: its steps, soft hits, the other eye's picture, the grid where neither eye
// sees), once a half-res pixel in the AO pass instead of in every lamp draw at full resolution. No
// cubes then (no splats, no cube lookups). VR_SCENE_MARCH=0: the trace and the cubes of before.
inline bool MarchMode() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_SCENE_MARCH");
        return env == nullptr || env[0] != '0';
    }();
    return on;
}
// The mask march's step offsets (2026-11-13 evening, the user: the shadows "a bit stripped, like if
// they have stripes or bands"): the lamp shaders' interleaved gradient noise has a diagonal
// structure, and at half resolution with fewer steps it shows as bands. VR_SCENE_MARCH_NOISE=1: a
// hash of the pixel's true world cell (Jitter's, light_shadow.h JitterCellM), turned by the frame
// (the golden ratio's steps): it stays on the floor as the head moves, has no diagonals, and still
// changes every frame for the upscaler to settle. Off again (the user, v136's snapshots: "No 6 has artifacts": its 3 cm cells show); it was on after its measure (v136, Oedon: the
// nearest to the old tracer's look in two of three views, 0.51 ms); 0: the lamp shaders' own.
// 2 (2026-11-13 night, the user: "our shadows look soft and kinda dotted again"): interleaved, each
// pixel of a 2 x 2 group its own quarter of the step (a 2 x 2 ordered dither: 0 2 / 3 1), the lamp
// shaders' noise within it: every 2 x 2 the lamps' read blends holds all four quarters, four
// times the steps' coverage, no cost. 2 by default since 2026-11-17, with the mask (the user's own
// launchers have run 2 since that night).
inline u32 MarchNoise() {
    static const u32 v = [] {
        const char* const env = std::getenv("VR_SCENE_MARCH_NOISE");
        return env != nullptr ? std::clamp(static_cast<u32>(std::atoi(env)), 0u, 2u) : 2u;
    }();
    return v;
}
// The smart search in the mask march (2026-11-13 evening, the user: "fire a ray if it doesn't hit
// anything the next ray is further away. if it hits something the next ray is closer. Like a smart
// search"): each next step is SmartFreeShare of the open air in front of the surface the eye sees at
// this one (long strides through empty space, short ones near the wall behind a statue or along
// her arm), at least SmartMinStepM (and a quarter of the even step), and never longer on the
// picture than SmartPixels pixels (nothing wider is stepped over). The step count is then the most
// a ray may take. VR_SCENE_MARCH_SMART=1 (tests until measured).
inline bool MarchSmart() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_SCENE_MARCH_SMART");
        return env != nullptr && env[0] == '1';
    }();
    return on;
}
inline constexpr float SmartFreeShare = 0.5f;
inline constexpr float SmartPixels = 2.0f;
inline constexpr float SmartMinStepM = 0.01f;
// ... with MarchStepShare of the lamp shaders' near and far step counts (Oedon, v132: the mask pass
// 0.75 ms with all of them, 0.57 with half, 0.51 with a third; the statues' shadows the same at
// half). VR_SCENE_MARCH_STEPS=<share> (tests).
// The mask pass's speed-ups (2026-11-13 review; each leaves the picture as it was: the same bits,
// or the last bit of a float's rounding): VR_MARCH_OPT=<mask> (tests, A/B; default all on).
// 1: a lamp whose share at this pixel (strength, edge-on fade, rim fade) is under 1/32 is not
//    marched (its 4 bits round to lit whatever the march finds); 2: the march step's divides (one
//    reciprocal for the steps' spacing, one for the depth ratio, one for both fades); 4: the other
//    eye's picture only where the step is off this one or within its edge band; 8: the grid's
//    lookups along the ray in the grid's space (three multiply-adds, not a 3x4 transform); 16: his
//    lamps flagged by the CPU (SlotHis: no per-pixel test, no body test for the others); 32: the
//    mask's words written with constant indices (registers, not local memory); 64: the CPU's body
//    upload under one lock, no memset first; 128: the blocker grid's block stamps (Grid::Coarse*).
inline bool Opt(u32 bit) {
    static const u32 mask = [] {
        const char* const env = std::getenv("VR_MARCH_OPT");
        // (128, the grid's block stamps, off by default: Oedon, alternated, the AO pass 0.52 / 0.53
        // ms with them, 0.50 / 0.51 without; the 4 MB of cells sit in the L2 anyway.)
        return env != nullptr ? static_cast<u32>(std::strtoul(env, nullptr, 0)) : ~0u & ~128u;
    }();
    return (mask & bit) != 0;
}
// The nearest-depth pyramid (2026-11-13, the review's item 7; vr_depth_pyramid.h): before the AO
// pass one dispatch takes the depth the pass reads to blocks of 2 to 32 texels a side, each its
// nearest and farthest depth. A march step checks its whole stretch on the picture against the
// blocks covering it first (2 x 2 of the level whose blocks span it; a stretch longer than the
// top level's blocks skips this check): the ray in front of everything there, no depth is read at
// all; else the stretch is read every PyramidRefine texels, evenly on the picture (at most
// PyramidMaxRefine reads: a stretch the check covers never reaches it), so a blocker about a texel
// wide or more between two steps is not stepped over (half steps' stripes), and the steps can be
// fewer (PyramidStepScale). Experimental (a code review of the first version: two coverage holes,
// fixed; its cost not yet measured): VR_SCENE_PYRAMID=1 turns it on; _SCALE / _REFINE (tests).
inline constexpr u32 PyramidLevels = 5;
inline constexpr u32 PyramidHeaderWords = 16; // w0, h0, levels, tag, level 1..5's first texel
inline constexpr float PyramidMinTexels = 1.5f; // a shorter stretch: the plain point test
inline constexpr u32 PyramidMaxRefine = 32;
// v2 (2026-11-13 evening; the user in the headset: "fantastic at stabilizing shadows where they
// appeared striped", but strips escaped from the Messengers round a lamp): a stretch that may hold a
// blocker is cut into pieces of about PyramidPieceTexels, each checked against finer blocks, and
// only a piece that still may is read, every PyramidRefine texels (at most PyramidMaxReads a
// piece: the budget; past it the reads spread out). The dense reads take a thinner blocker
// (PyramidThick: the old march's 40 cm made up for its sparse steps, and read every texel it put
// shadow behind every small thing's outline) at the depth texel's own value (its centre: no
// filtered mix of an outline and what lies behind it).
// v3 (the same evening: v1 cost 0.97 ms and v2 2.1 ms against the old march's 0.5, a block's check
// costing what the read it saves costs, the march's 10 to 20 steps leaving little to skip; the
// user: "both better AND faster, or at least the same cost"): one read a step as before, the
// nearest depth of the blocks at the stretch's two ends (the level whose blocks span it) in place
// of the texel under the step: a thin blocker between two steps is seen, at the cost of a
// shadow up to a block fuller. Not within PyramidStartM of the receiver (its own neighbourhood
// would shadow it); not for a stretch of a texel or less (the texel itself).
inline constexpr float PyramidStartM = 0.25f;
inline constexpr float PyramidPieceTexels = 4.0f;
inline constexpr u32 PyramidMaxPieces = 8;
inline constexpr u32 PyramidMaxReads = 8;
inline constexpr float PyramidThickPerM = 0.05f;
inline float PyramidThickM() {
    static const float v = [] {
        const char* const env = std::getenv("VR_SCENE_PYRAMID_THICK");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.02f, 1.0f) : 0.15f;
    }();
    return v;
}
inline bool Pyramid() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_SCENE_PYRAMID");
        return env != nullptr && env[0] == '1';
    }();
    return on;
}
inline float PyramidStepScale() {
    static const float v = [] {
        const char* const env = std::getenv("VR_SCENE_PYRAMID_SCALE");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 1.0f, 4.0f) : 1.0f;
    }();
    return v;
}
inline float PyramidRefine() {
    static const float v = [] {
        const char* const env = std::getenv("VR_SCENE_PYRAMID_REFINE");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.5f, 4.0f) : 1.0f;
    }();
    return v;
}
// The mask writer and its lamp readers must use the same adaptive profile.
inline bool Edges();
inline u32 EdgeTier();
// A step share's test switch: its value within [lo, hi], or -1 where it is not set.
inline float EnvStepShare(const char* name, float lo, float hi) {
    const char* const env = std::getenv(name);
    return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), lo, hi) : -1.0f;
}
// Low's march (2026-11-21, the user: "weak/gentle settings for low", "make the torch cheaper for
// low but let it be as it is for medium"): every lamp LowStepShare, a close lamp (the held torch)
// LowCloseStepShare, no near boost; Medium and High as before. At Oedon Chapel (2000x2160 a eye)
// with the torch 7.86 ms of GPU a frame against 8.13 with Medium's steps (no boosts alone: 7.95),
// without it 7.44 against 7.59. The tier is published before any program is made.
inline constexpr float LowStepShare = 0.3f;
inline constexpr float LowCloseStepShare = 0.4f;
inline bool LowSteps() {
    return Edges() && EdgeTier() == 0;
}
inline float MarchStepShare() {
    static const float env = EnvStepShare("VR_SCENE_MARCH_STEPS", 0.1f, 2.0f);
    // (With the edges: 0.22 from fa118f20e, 0.4 since 2026-11-20, the user: "let's give them
    // 0.4", the edge lamps' shadows past the edges' reach and outside the centre being theirs.)
    return env >= 0.0f ? env : !Edges() ? 0.5f : LowSteps() ? LowStepShare : 0.4f;
}
// The lamps without sharp edges (2026-11-20, the user: "I don't know if adaptive edges worsen the
// non-adaptive edges too much. I am wondering if we can increase a bit the number of rays for the
// lights that don't cast adaptive edges so that it is closer to how it was before, and slightly
// reduce the adaptive edges ones"): with the edges every slot lamp's march had taken
// MarchStepShare's 0.22 (0.5 before them) to pay for the edge lamps; a slot lamp that is not one
// of the frame's edge lamps (the slot header's edge bits) marches PlainStepShare instead, its close
// and near boosts scaled with it, and the edge lamps keep MarchStepShare (the edge pass sharpens
// theirs). VR_SCENE_PLAIN_STEPS=<share> (tests; at or under MarchStepShare: every lamp as before).
// (Low: LowStepShare, every lamp alike.)
inline float PlainStepShare() {
    static const float env = EnvStepShare("VR_SCENE_PLAIN_STEPS", 0.1f, 2.0f);
    return env >= 0.0f ? env : !Edges() ? MarchStepShare() : LowSteps() ? LowStepShare : 0.5f;
}
// Close lamps (2026-11-13 night, the user: "for very close lights (like the torch I hold) to have
// more detailed shadows. But only for like, super close ones"): a slot lamp within CloseInM of the
// eye marches CloseStepShare of the lamp shaders' step counts in place of MarchStepShare, back to
// MarchStepShare by CloseOutM: the held torch (half a metre to a metre from the eye) and a wall
// lamp within arm's reach; every other lamp as before. The step spacing on the picture stays the
// lamp shaders' (one step per PixelsNear at most), so a short ray takes no more than before.
// VR_SCENE_CLOSE_STEPS=<share> (tests; at or under MarchStepShare: off).
inline constexpr float CloseInM = 1.0f;
inline constexpr float CloseOutM = 1.6f;
// (2026-11-21 at Oedon Chapel, Medium: the torch's shadows ~0.5 ms of GPU a frame, ~0.2 of it this
// boost. Low: LowCloseStepShare.)
inline float CloseStepShare() {
    static const float env = EnvStepShare("VR_SCENE_CLOSE_STEPS", 0.0f, 3.0f);
    return env >= 0.0f ? env : !Edges() ? 1.0f : LowSteps() ? LowCloseStepShare : 0.8f;
}
// Near pixels (2026-11-13 night, the user: "increase ... how many rays we fire etc when we're
// nearby, and decrease in the distance"): a pixel within NearStepInM of the eye marches
// NearStepShare of the lamp shaders' step counts, back to MarchStepShare by NearStepOutM (the far
// steps already fall: MarchSteps' StepsFar at DetailFarM, StepsFarthest at DetailFarthestM). With
// a close lamp's share: the larger of the two shares at most. VR_SCENE_NEAR_STEPS=<share> (tests;
// at or under MarchStepShare: off).
inline constexpr float NearStepInM = 1.5f;
inline constexpr float NearStepOutM = 4.0f;
// (Low: none, its share MarchStepShare's.)
inline float NearStepShare() {
    static const float env = EnvStepShare("VR_SCENE_NEAR_STEPS", 0.0f, 3.0f);
    return env >= 0.0f ? env : !Edges() ? 0.75f : LowSteps() ? LowStepShare : 0.6f;
}
// The step dial (the same night, the user: "or be smart and reduce it if ... gpu time is increased
// too much"): the mask march's steps times StepDialScale[level], written in each slot's dword 11
// (Rasterizer: once a frame). The level follows the game's GPU time a frame (vk_gpu_frame_timer.h),
// the trace dial's rule: one level down once it stays over DialDownShare of the frame's time for
// DialDownFrames frames running, one back up after DialUpSeconds under DialUpShare. The shadows'
// shapes stay; only fewer steps find a thin blocker. VR_SCENE_STEP_DIAL=0: off (always 1).
// Foveated steps (the same night, the user: "are we doing foveated shadowing? meaning that... more
// detail in the center?", then "let's do it and see if it saves performance and/or if it alters
// visuals too much"): the AO pass's marches already follow the foveation map (one a mask pixel in
// the full zone, one a 2x1, 2x2 or 4x4 block outside it); their steps now follow it too:
// FoveaStepScale by the block's pixels (1, 2, 4, 8, 16). Without foveation every pixel is the
// full zone's. VR_SCENE_FOVEA_STEPS=0: off; VR_SCENE_FOVEA_CENTER=<scale>: the full zone's (tests).
inline constexpr float FoveaStepScale[5] = {1.0f, 0.85f, 0.7f, 0.6f, 0.5f};
inline bool FoveaSteps() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_SCENE_FOVEA_STEPS");
        return env == nullptr || env[0] != '0';
    }();
    return on;
}
inline float FoveaCenterScale() {
    static const float v = [] {
        const char* const env = std::getenv("VR_SCENE_FOVEA_CENTER");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.25f, 3.0f) : FoveaStepScale[0];
    }();
    return v;
}
inline constexpr unsigned StepDialLevels = 4;
inline constexpr float StepDialScale[StepDialLevels] = {1.0f, 0.8f, 0.65f, 0.5f};
inline bool StepDial() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_SCENE_STEP_DIAL");
        return env == nullptr || env[0] != '0';
    }();
    return on;
}
// Every slot lamp reaching a pixel is traced (on by default since 2026-11-13: Oedon's A/B, the same
// head turn, the AO pass 0.45 ms with it, 0.41..0.45 without; the old march's lamps 3.9 ms; its
// flicker 0.40 against the old 0.38). VR_SCENE_TRACE_ALL=0: the top lamp only (contact).
inline bool TraceAll() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_SCENE_TRACE_ALL");
        return env == nullptr || env[0] != '0';
    }();
    return on;
}
// Tests (2026-11-13 evening, a code review; the tester: "#4 + retest early exits"): VR_SCENE_EARLY=1
// ends a lamp's trace once its ray is fully blocked, and skips it where the cube (his body and the
// casters with it) is already dark below the mask's half step. The same image; it goes on by
// default only if it measures faster (the 11-13 try of the first saved nothing at Oedon).
inline bool EarlyOut() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_SCENE_EARLY");
        return env != nullptr && env[0] == '1';
    }();
    return on;
}
inline unsigned TraceSteps() {
    static const unsigned v = [] {
        const char* const env = std::getenv("VR_SCENE_TRACE_STEPS");
        return env != nullptr ? std::clamp(static_cast<unsigned>(std::atoi(env)), 2u, 32u) : TraceAll() ? 12u : ContactSteps;
    }();
    return v;
}
inline float TraceM() {
    static const float v = [] {
        const char* const env = std::getenv("VR_SCENE_TRACE_M");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.1f, 8.0f) : TraceAll() ? 4.0f : ContactM;
    }();
    return v;
}
// ... only where the AO pass shades every pixel (its foveated coarse blocks would show squares), and
// within ContactFarM of the eye (beyond, a frond's shadow is under a pixel).
inline constexpr float ContactFarM = 12.0f;
// On by default (VR_SCENE_CONTACT=0: off): Oedon's AO pass 0.41 vs 0.38 ms, Old Yharnam's 0.26 vs
// 0.25; the head-turn flicker the same (a fixed per-pixel jitter, half the light a hit).
inline bool Contact() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_SCENE_CONTACT");
        return env == nullptr || env[0] != '0';
    }();
    return on;
}
// The trace's reach from the eye (2026-11-13 evening, the user: "reduce the tracing steps based on
// frame rate - so that we do not drop (always leave a good leeway too)", then "I wouldn't mind some
// lamps get splats ... especially ones that are further out"): a pixel farther from the eye than
// its slot's trace reach (slot dword 7, view metres) keeps the cube's shadow only (a texel is a
// pixel or two there, and its frame of lag goes unseen). The reach follows the GPU's time
// (Rasterizer::VrEndFrame): the first of TraceFarLevels, one level nearer once the game's GPU time
// a frame stays over DialDownShare of its frame time for DialDownFrames frames running, one level
// back after DialUpSeconds under DialUpShare. A slot's first TraceNewFrames frames trace to the
// first level whatever the dial (a lamp just come into view has no whole cube yet: the snap-turn
// flash). The steps stay TraceSteps (fewer saved nothing measurable at Oedon; 6 flickered).
inline constexpr u32 TraceFarLevelCount = 4;
inline constexpr float TraceFarLevels[TraceFarLevelCount] = {12.0f, 8.0f, 5.0f, 3.0f};
static_assert(TraceFarLevels[0] == ContactFarM);
inline constexpr u32 TraceNewFrames = 20;
inline constexpr float DialDownShare = 0.85f, DialUpShare = 0.72f;
inline constexpr u32 DialDownFrames = 6;
inline constexpr float DialUpSeconds = 2.0f;
// Off by default since its first measure (2026-11-13 evening, Oedon's head turn, v127): the mask
// pass took 0.47 ms tracing to 3 m against 0.48 to 12 m, so a nearer reach buys nothing there, and
// it brings a reach's edge that can show and the GPU frame timer. Tests: VR_SCENE_DIAL=1 on;
// VR_SCENE_TRACE_FAR=<m> pins the reach.
inline float TraceFarPinned() {
    static const float v = [] {
        const char* const env = std::getenv("VR_SCENE_TRACE_FAR");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.0f, 40.0f) : -1.0f;
    }();
    return v;
}
inline bool Dial() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_SCENE_DIAL");
        return env != nullptr && env[0] == '1';
    }();
    return on;
}
// A lamp's resting spot (2026-11-13 evening, the user: "shadows do not blink, maybe their
// brightness changes a bit based on the light's intensity, but their shape stays put"): the game
// moves a flame's light about as it flickers and a fire pulses its reach, so its shadows wobbled
// with it. A slot's shadows (its cube, the trace, his body, the casters) come from where its lamp
// rests: its place followed over AnchorTauS, never more than AnchorHoldM behind it (a carried
// torch's shadows trail it by that at most), its reach followed over AnchorTauS too. A held light
// (yours) is where it is. Tests: VR_SCENE_HOLD_M=<m> (0: off, every frame's own place).
inline constexpr float AnchorTauS = 0.5f;
inline constexpr float AnchorHoldMDefault = 0.06f;
inline float AnchorHoldM() {
    static const float v = [] {
        const char* const env = std::getenv("VR_SCENE_HOLD_M");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.0f, 0.15f) : AnchorHoldMDefault;
    }();
    return v;
}
inline bool Enabled() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_SCENE_MAPS");
        return env == nullptr || env[0] != '0';
    }();
    return on;
}
// Stage 2 (the tester: "I want NPCs and enemies and misc geometry etc (from the grid) to be
// added"): the blocker grid's voxels (Grid: what the view saw lately, behind and beside it too)
// into the cubes as well, by the AO pass's pixels (each takes its share of the voxels). A grid
// entry carries the low bit set: the lookup lets it pass a voxel nearer (more on a slope, at most
// GridSlopeMax voxels' worth), so the coarse voxels never speckle the surfaces they came from. A
// voxel covers up to MaxFoot texels each side of its own (close to the lamp). VR_SCENE_GRID=0: off.
inline constexpr u32 MaxFoot = 3;
// The slot table (body_shadow.h SlotsAt): SlotHeader dwords (the slots' extent, the grid's flag,
// this frame's half of the cubes, the mask's capacity in pixels, the flat view's rotation into
// the world's axes: 9 floats), then SlotDwords a slot (its lamp: world axes, from this frame's
// camera; its reach dword; its shadow's strength; flags: SlotUsed, SlotFresh).
inline constexpr u32 SlotHeader = 16;
inline constexpr u32 SlotDwords = 12; // (7: its trace's reach from the eye, TraceFarLevels; 8..10:
                                      // where its lamp was when last frame's cube was made; 11:
                                      // the march's step scale, StepDialScale)
// (450 since 2026-11-13: a snap turn back finds the lamps behind you still in their slots, their cubes
// warm and traced, no flash; a new lamp still takes the longest-unseen one's slot. A lamp out of
// view lights no pixel in view: its slot costs a distance test a pixel.)
inline constexpr u32 LingerFrames = 450;
inline constexpr u32 MaxPixelFoot = 2;  // a screen pixel covers up to this many texels each side
inline constexpr u32 SlotUsed = 1u, SlotFresh = 2u;
// Set by the CPU in the upload only (VR_MARCH_OPT 16): the slot's lamp is one his shadow comes from
// (body_shadow.h BodyLampsAt within MapMatchM; none listed: every lamp), BodyVisibility's own test.
inline constexpr u32 SlotHis = 4u;
// Set by the CPU in the upload only, with High lamp shadows (LightShadow::High): the slot's lamp is
// one of the frame's HeroLamps, whose light the lamp shaders march at full resolution (the old
// method's march) instead of reading the mask. At most HeroLamps a frame (3: the tester, 2026-11-17,
// "how about we make that 3 lamps?"; was 2): a lamp in a hand first (the torch, the lantern), then
// the ones lighting the most of the view. A hero stays one while it lights at least HeroKeepShare
// of what the best other lamp does: two lamps lighting about as much never trade the sharp shadows
// back and forth as you walk.
inline constexpr u32 SlotHero = 8u;
// Slot header word 2: bit zero selects the cube half; one read supplies every lamp's edge flag.
inline constexpr u32 EdgeSlotShift = 1;
inline constexpr u32 EdgeLamps = 3; // (Medium's: EdgeTierLamps)
// Low / Medium / High (vr.ini light_shadow_detail=lite|medium|high, the tester, 2026-11-20:
// "medium, 4 lamps at 4m. low, 2 lamps at 4m. high, 5 lamps at 5m"): the frame's edge lamps and
// how far from the eye their edges are sharp: wholly sharp to EdgeTierInM, none from EdgeTierOutM
// (EdgeFar). Medium is the default. Set once at start (vk_pipeline_cache.cpp VrLightShadowLevel)
// before any program is made: the lamp shaders hold the reach and the edge pass's layout. The lamp
// programs hold it: a change here is a VrFragmentRevision.
// (2026-11-21, the user: Medium "detailed shadows up close ... for up to 4m or so", sharp to 4 m
// and gone by 5 m, three lamps: at Oedon Chapel it cost what the old Medium did, four lamps sharp
// to 2.5 m and gone by 4.5 m; a fourth lamp at 4 to 5 m was 0.1 to 0.2 ms more. The same day,
// "good settings for high, and weak/gentle settings for low": High sharp to 5 m and gone by 6
// (it cost what 3 to 5 m did; 6 to 7 m ~0.1 ms more), Low sharp to 2 m and gone by 3, with fewer
// march steps (LowSteps).)
inline constexpr std::array<u32, 3> EdgeTierLamps{2, 3, 5};
inline constexpr std::array<float, 3> EdgeTierInM{2.0f, 4.0f, 5.0f};
inline constexpr std::array<float, 3> EdgeTierOutM{3.0f, 5.0f, 6.0f};
static_assert(EdgeTierLamps[1] == EdgeLamps);
inline constexpr u32 EdgeTierMedium = 1;
inline std::atomic<u32> g_edge_tier{EdgeTierMedium};
// The tier of the program being made on this thread (TierScope, from its key: TranslateProgram
// and EmitSPIRV), -1 for none: the session's tier (g_edge_tier) switches live since 2026-11-21,
// and a program made while it changes must keep the tier its key says.
inline thread_local int t_program_tier = -1;
inline u32 EdgeTier() {
    const int own = t_program_tier;
    return own >= 0 ? std::min<u32>(static_cast<u32>(own), 2u)
                    : std::min<u32>(g_edge_tier.load(std::memory_order_relaxed), 2u);
}
struct TierScope {
    int previous;
    explicit TierScope(int tier) : previous{t_program_tier} {
        t_program_tier = tier;
    }
    ~TierScope() {
        t_program_tier = previous;
    }
    TierScope(const TierScope&) = delete;
    TierScope& operator=(const TierScope&) = delete;
};
inline u32 EdgeLampsNow() {
    return EdgeTierLamps[EdgeTier()];
}
// The edge pass's bits a pixel: two a lamp, a byte for four lamps, 16 bits beyond (High's five).
inline u32 EdgeBits() {
    return EdgeLampsNow() > 4 ? 16u : 8u;
}
inline constexpr u32 EdgeMaxLamps = 5; // vr_shadow_edges.comp's parameters hold five
// Each selected lamp has a two-bit verdict in the compute pass's packed pixel.
constexpr u32 EdgePassLampCapacity(u32 pixel_bits) {
    return std::min(pixel_bits / 2u, EdgeMaxLamps);
}
static_assert(Slots + EdgeSlotShift <= 32);
inline constexpr u32 HeroLamps = 3;
inline constexpr float HeroKeepShare = 0.67f;
// A lamp keeps its slot within SlotKeepM (and the same reach): a moving one (the torch in your
// hand) too, its last cube a frame behind it.
inline constexpr float SlotKeepM = 0.3f;
// Any other lamp within KeepFixedM, its reach within KeepReachShare: a fire pulses its reach by a
// few percent and an enemy's torch moves 5 cm and more a game frame (2026-11-13, the sewers: one
// pulsing lamp held 7 of 11 slots at 5 cm and 2%, every slot fresh, none ever cast). Slots go to
// the nearest lamp first, so lamps standing close (candles) still keep their own.
inline constexpr float KeepFixedM = 0.15f;
inline constexpr float KeepReachShare = 0.25f;
// The shadow mask (VR_SHADOW_MASK=1): the AO pass's half-res pixels, MaskWords each (4 bits a
// slot: 15 lit), after MaskHeader dwords (its width, height, eyes).
inline constexpr u32 MaskHeader = 4;
// The other casters (VrShadowCasters, the AO pass with the mask): enemies and NPCs not in view, as
// capsules (vr_session.h ShadowCasters): CasterHeader dwords (capsules, bodies), then CasterBodies
// bodies of 8 dwords (a sphere: x y z r, the world's axes from this frame's camera; its first
// capsule, count), then CasterCapsules of 8 (a: x y z, radius, b: x y z). A capsule darkens fully
// within CasterCore of its radius, fading out to CasterEdge (its soft edge).
inline constexpr u32 CasterHeader = 4, CasterBodies = 8, CasterCapsules = 48;
inline constexpr u32 CasterBodiesAt = CasterHeader, CasterCapsulesAt = CasterHeader + 8 * CasterBodies;
inline constexpr u32 CasterDwords = CasterCapsulesAt + 8 * CasterCapsules;
inline constexpr float CasterCore = 0.55f, CasterEdge = 0.95f;
// A lamp draw's lights' slots (the CPU's match, once a draw: a byte each, slot + 1, 0 none) in the
// slot header's last DrawSlotWords dwords: the lamp shaders do no matching.
inline constexpr u32 DrawSlotsAt = 13, DrawSlotWords = 3, DrawLights = DrawSlotWords * 4;
static_assert(DrawSlotsAt + DrawSlotWords <= SlotHeader);
inline constexpr u32 MaskWords = 3;
// (Words 0..MaskSlotWords-1: the slots' 4 bits each; the last: the pixel's view distance, for the
// lamp pixels' depth-aware read, neighbours past MaskDepthShare of their own distance left out.)
inline constexpr u32 MaskSlotWords = 2;
inline constexpr float MaskDepthShare = 0.05f;
// The lamp read's blend between the mask's blocks (emit_spirv_light_shadow.cpp MaskVisibility): the
// share of the way from a block's middle to its border over which it blends into the next block
// (0.5: the whole way, the softest; small: nearly the square steps again). VR_MASK_BLOCK_RAMP (tests).
// The mask's own foveation (2026-11-15; the user in the headset: lamp shadows "blocky", and at his
// settings, foveation level 3, the study showed why: clean in the full-rate centre, square steps
// from 0.63 of the way out, where the AO pass shades one fragment a 2 x 1 or 2 x 2 block of mask
// pixels and a shadow's hard edge shows every block): the pass that writes the mask is shaded at
// full rate out to MaskFoveaInner of the way from the eye's axis to the picture's edge (the
// foveation's own inner zone where that is wider), the usual rates beyond. Every other pass keeps
// the foveation as it is. VR_MASK_FOVEA_INNER=<share> (tests; 0: the foveation's own zones).
// 0.75 since 2026-11-17 (the tester: "a bit of a wider centre ... just a little bit"; was 0.70).
inline float MaskFoveaInner() {
    static const float v = [] {
        const char* const env = std::getenv("VR_MASK_FOVEA_INNER");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.0f, 4.0f) : 0.75f; // (measured 2026-11-15 at the chapel, level 3: 0.68 +0.03 ms, 0.70 +0.04, 0.72 +0.06, 0.75 +0.08, 0.80 +0.11, 0.85 +0.15)
    }();
    return v;
}
inline bool MaskBlockBlend() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_MASK_BLOCK_BLEND");
        return env != nullptr && env[0] == '1';
    }();
    return on;
}
inline float MaskBlockRamp() {
    static const float v = [] {
        const char* const env = std::getenv("VR_MASK_BLOCK_RAMP");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.02f, 0.5f) : 0.25f;
    }();
    return v;
}
// The mask holds at most this many pixels (a 2048x2048 pass a eye: 96 MiB would be absurd; a
// larger pass casts no lamp shadows rather than grow it further).
inline constexpr u32 MaskMaxPixels = 2u * 1600u * 1600u;
static_assert(Slots <= MaskWords * 8);
// The last word since 2026-11-13 evening (the user: the statue "a bit low res" with the mask): the
// pixel's view distance as a half float (its low 16 bits: the depth-aware read needs MaskDepthShare)
// and its surface's facing in this eye's view (octahedral, 8 bits a component: the high 16). A lamp
// pixel's read weighs each half-res neighbour by how near its facing is to its own as well (the
// cosine from MaskNormalFrom, in full at MaskNormalTo): a fold's two sides stay apart instead of
// smearing into each other. The same mask size. VR_SHADOW_MASK_NORMALS=0: the depth alone.
inline bool MaskNormals() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_SHADOW_MASK_NORMALS");
        return env == nullptr || env[0] != '0';
    }();
    return on;
}
inline constexpr float MaskNormalFrom = 0.5f, MaskNormalTo = 0.9f;
// On with the scene maps since 2026-11-17 (see SceneMaps above). VR_SHADOW_MASK=0 (tests): the
// maps without the mask.
inline bool Mask() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_SHADOW_MASK");
        return Enabled() && (env == nullptr || env[0] != '0');
    }();
    return on;
}
// The mask's edges marched again at the lamp draws' own resolution (on by default; VR_SHADOW_EDGES=0 restores the original mask step counts;
// the user: "razor sharp close by, ok for more messy far away"; the mask is half-res, in blocks of
// up to 4 x 4 in the foveated ring, and a lamp pixel blends its four neighbours: every edge a soft
// ramp). Where the four neighbours' values for the lamp's slot differ by EdgeRange or more (of 15)
// and the pixel is within EdgeFar() of the eye, a lamp pixel marches its own short ray toward the
// lamp on the lamp shader's own depth picture (this eye's only; no grid, no other eye, no body):
//  - a hit: the neighbours' lowest value (the pixel is in the shadow the mask saw beside it);
//  - clear, every step on the picture and none behind something: EdgeClear() of the way from the
//    blend to the neighbours' highest (0: the blend as it was);
//  - anything else (the ray left the picture, or went behind something): the blend as it was: the
//    mask may know of a caster the picture does not show (his body, the grid, the other eye).
// Never darker than the lowest neighbour or lighter than the highest: an edge is placed, no shadow
// made or removed. Pixels whose neighbours agree skip it all behind a branch. Off: the lamp
// shaders as they were, to the byte.
inline bool Edges() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_SHADOW_EDGES");
        return Mask() && (env == nullptr || env[0] == '1');
    }();
    return on;
}
// Its steps: one for each EdgePixels() pixels of the ray on the picture, EdgeMinSteps to
// EdgeSteps(), set as the mask's march sets them (the square of their share: denser near the
// surface), the first hit the last step. VR_SHADOW_EDGES_STEPS / VR_SHADOW_EDGES_PX (tests).
inline constexpr u32 EdgeMinSteps = 6;
inline u32 EdgeSteps() {
    static const u32 n = [] {
        const char* const env = std::getenv("VR_SHADOW_EDGES_STEPS");
        return env != nullptr ? std::clamp(static_cast<unsigned>(std::atoi(env)), 2u, 96u) : EdgeMinSteps;
    }();
    return n;
}
inline float EdgePixels() {
    static const float v = [] {
        const char* const env = std::getenv("VR_SHADOW_EDGES_PX");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.5f, 32.0f) : 5.0f;
    }();
    return v;
}
// The neighbours' spread (of 15) from which a pixel marches. VR_SHADOW_EDGES_RANGE (tests).
inline u32 EdgeRange() {
    static const u32 n = [] {
        const char* const env = std::getenv("VR_SHADOW_EDGES_RANGE");
        return env != nullptr ? std::clamp(static_cast<unsigned>(std::atoi(env)), 1u, 15u) : 8u;
    }();
    return n;
}
// A clear ray's share of the way to the highest neighbour (see above). VR_SHADOW_EDGES_CLEAR.
inline float EdgeClear() {
    static const float v = [] {
        const char* const env = std::getenv("VR_SHADOW_EDGES_CLEAR");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.0f, 1.0f) : 1.0f;
    }();
    return v;
}
// In full within the first of the eye, none from the second (metres): by the tier (EdgeTierInM,
// EdgeTierOutM; 1.5 and 3.5 until 2026-11-20). VR_SHADOW_EDGES_FAR=<in,out> (tests) for every tier.
inline std::pair<float, float> EdgeFar() {
    static const std::pair<float, float> pinned = [] {
        std::pair<float, float> out{-1.0f, -1.0f};
        if (const char* const env = std::getenv("VR_SHADOW_EDGES_FAR")) {
            char* end = nullptr;
            const float a = std::strtof(env, &end);
            if (end != env && *end == ',') {
                const float b = std::strtof(end + 1, nullptr);
                if (a >= 0.0f && b > a) {
                    out = {a, b};
                }
            }
        }
        return out;
    }();
    if (pinned.second > 0.0f) {
        return pinned;
    }
    return {EdgeTierInM[EdgeTier()], EdgeTierOutM[EdgeTier()]};
}
// The edge pass sharpens pixels within EdgeFar's second of the eye only, and a lamp lights nothing
// past its reach: one whose light ends before that zone (its distance less its reach) gets no edge
// pick. The 2026-11-24 review: on High two 10 m street lamps 20 m off took the picks of candles 4
// to 4.5 m away. EdgeZoneSlackM covers the eyes' and the slot anchor's offsets from the list's eye.
inline constexpr float EdgeZoneSlackM = 0.25f;
constexpr bool LightsEdgeZone(float eye_m, float reach_m, float zone_m) {
    return eye_m - reach_m < zone_m + EdgeZoneSlackM;
}
// The steps' offset: 1 a fixed pattern by the pixel's place (interleaved gradient noise, the same
// every frame: nothing crawls), 0 none. VR_SHADOW_EDGES_NOISE (tests).
inline u32 EdgeNoise() {
    static const u32 n = [] {
        const char* const env = std::getenv("VR_SHADOW_EDGES_NOISE");
        return env != nullptr ? static_cast<u32>(std::atoi(env)) : 1u;
    }();
    return n;
}
// A pixel marches for at most this many of a lamp draw's lights (the first with an edge there).
inline constexpr u32 EdgeLights = 2;
// The edge pass (2026-11-19, the user: "make the adaptive edges faster than before but higher
// quality"): the march above moved out of the lamp shaders into one dispatch of our own before the
// frame's first lamp draw (vr_shadow_edges.h; host_shaders/vr_shadow_edges.comp), both eyes at the
// lamp draws' own size. Held in the lamp shaders, the march's code cost every lamp pixel ~0.33 ms
// at 2000x2160 a eye even where nothing marched (its registers: measured with no lamp marching).
// The pass marches the frame's EdgeLamps (up to all four at a pixel, EdgePassSteps steps) where a
// lamp's four mask neighbours differ by EdgeRange or more, and writes two bits a lamp a pixel:
// EdgeHit (the lowest neighbour), EdgeClear (toward the highest), 0 (the blend as it was). A lamp
// shader reads one word a pixel; a pass that did not run this frame (its serial, the slot header's
// EdgeSerialShift bits, not the buffer's) leaves the blend. VR_SHADOW_EDGE_PASS=0: the march in the
// lamp shaders as before.
inline bool EdgePass() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_SHADOW_EDGE_PASS");
        return Edges() && (env == nullptr || env[0] != '0');
    }();
    return on;
}
inline u32 EdgePassSteps() {
    static const u32 n = [] {
        const char* const env = std::getenv("VR_SHADOW_EDGE_PASS_STEPS");
        return env != nullptr ? std::clamp(static_cast<unsigned>(std::atoi(env)), 2u, 64u) : 10u;
    }();
    return n;
}
inline constexpr u32 EdgeHit = 1u, EdgeClearCode = 2u;
// The pass sharpens only in the mask's full-rate centre (2026-11-20: "sharpen edges only
// in the centre"): beyond MaskFoveaInner (or the foveation's inner zone) the mask is worked out in
// 2x1, 2x2 and 4x4 blocks, and an edge made sharp from them was their staircase, stripes along
// every shadow's border; there the lamps keep the blend, as before the edges. This much inside the
// border (of the way from the eye's axis to the picture's edge), clear of the ring's first blocks.
inline constexpr float EdgeFoveaMargin = 0.03f;
// The pass's buffer: EdgeParamWords of its parameters (the host's), the header (serial, width,
// height, words a row), then a byte a pixel (four to a word), the left eye's rows, then the right's.
inline constexpr u32 EdgeParamWords = 160;
inline constexpr u32 EdgeHeaderAt = EdgeParamWords;
inline constexpr u32 EdgeDataAt = EdgeHeaderAt + 4;
// The serial of this frame's edge pass (0: none): the slot header's dword 2, bits EdgeSerialShift
// up (EdgeSerialMask), above the edge lamps' bits. (Until 2026-11-19 it took dword 13, which is
// DrawSlotsAt: every lamp draw lost its first four lights' slots.)
inline constexpr u32 EdgeSerialShift = 17;
inline constexpr u32 EdgeSerialMask = 0x7FFFu;
static_assert(Slots + EdgeSlotShift <= EdgeSerialShift && EdgeSerialShift + 15 <= 32);
// ... and only where the lamp's falloff (1 at the lamp, 0 at its reach) times the spread (of 15)
// is at least this: a faint edge at a lamp's rim stays the blend.
inline constexpr float EdgeMinContrast = 1.5f;
// Tests (VR_SHADOW_EDGES_SHOW): 1 the pixels that marched black; 2 a hit black, a clear ray lit, the
// rest as they were.
inline u32 EdgeShow() {
    static const u32 n = [] {
        const char* const env = std::getenv("VR_SHADOW_EDGES_SHOW");
        return env != nullptr ? static_cast<u32>(std::atoi(env)) : 0u;
    }();
    return n;
}
// Maps only (2026-11-13, Oedon: a lamp shader that merely holds the map code marched 2 ms slower,
// its size the cost): no march in the lamp shaders at all; a lamp outside the slots casts none.
// VR_SCENE_MAPS_ONLY=1 (tests).
inline bool Only() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_SCENE_MAPS_ONLY");
        return env != nullptr && env[0] == '1';
    }();
    return on;
}
inline constexpr float GridSlopeMax = 3.0f;
// Tests: VR_SCENE_SCREEN=0 leaves the screen's pixels out of the cubes (only the grid and the
// enemies' and NPCs' capsules cast). (Off for an hour on 2026-11-13 evening, trace-first: statues
// then cast only outlines, the user: "behind statues there are no shadows, before they were nice
// and thick and beautiful"; the trace sees only a statue's front. Back on, with Confirm.)
inline bool ScreenSplat() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_SCENE_SCREEN");
        return env == nullptr || env[0] != '0';
    }();
    return on;
}
// A cube's shadow checked against the view (2026-11-13 evening; the user: blobs "that do not seem
// connected to something", and statues' shadows "nice and thick"): where the trace runs, the cube's
// nearest blocker (its distance from the lamp, towards the pixel) is looked up in the eye's
// picture. Where the eye sees past that point by more than ConfirmM (plus its texel and the
// pixel's footprint), it is empty air: the cube's shadow there is a ghost and left out. Inside a
// statue, on a railing, behind what the eye sees, or off the picture: kept. VR_SCENE_CONFIRM=0: off.
inline bool Confirm() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_SCENE_CONFIRM");
        return env == nullptr || env[0] != '0';
    }();
    return on;
}
inline constexpr float ConfirmM = 0.05f;
inline bool GridSplat() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_SCENE_GRID");
        return env == nullptr || env[0] != '0';
    }();
    return on;
}
} // namespace SceneMaps

// High lamp shadows (vr.ini light_shadow_detail=high; 2026-11-17, the user: "I want to add a high
// setting available in settings", the old method's sharp look where it matters most): with the
// mask, the frame's SceneMaps::HeroLamps (SlotHero) march in the lamp shaders at full resolution
// (the old method's march: its steps, the other eye's picture, the grid where neither eye sees,
// his body) instead of the mask's half-res read; every other lamp reads the mask as at Low.
// Read at start (the lamp shaders hold it). VR_LIGHT_SHADOW_DETAIL=low|high (tests) overrides it.
// (Removed 2026-11-20, the user: "remove the old high which was full tracing at high res": High
// is now SceneMaps' edge tier, five lamps' sharp edges to 5 m. The heroes' march stays in the
// emitter, never compiled: no program asks for level 2.)
// The tier from the setting (vr.ini light_shadow_detail) or VR_LIGHT_SHADOW_DETAIL (tests): lite
// (Low), medium, high; "low", "0" and anything else Medium ("low" was every generated file's line
// and the old Low, which had four edge lamps), "high" and "1" High.
inline u32 TierOf(std::string_view value) {
    return value == "lite" ? 0u : value == "high" || value == "1" ? 2u : SceneMaps::EdgeTierMedium;
}
// A light rectangle's RuntimeInfo::vr_light_shadows: 0 none, else the shadows with the mask and the
// tier's edges: 1 Medium (the value Low had before the tiers: its cached programs still load), 3
// Low, 4 High. (2 was the old High's heroes, never made now.)
constexpr u8 Level(bool enabled, u32 tier) {
    return enabled ? (tier == 0 ? 3 : tier == 2 ? 4 : 1) : 0;
}
// The tier a program's key says (SceneMaps::TierScope), -1 for none: a light rectangle's level (3
// Low, 1 Medium, 4 High, 2 the old High), else the AO pass's mask writer's tier bits
// (RuntimeInfo::VrAoLampLow 4, VrAoLampHigh 32, with VrAmbientOcclusionBit 1 and VrAoLampShadows
// 16).
constexpr int ProgramTier(u8 light_level, u8 ao_bits) {
    if (light_level != 0) {
        return light_level == 3 ? 0 : light_level == 1 ? 1 : 2;
    }
    if ((ao_bits & (1u | 16u)) == (1u | 16u)) {
        return (ao_bits & 4u) != 0 ? 0 : (ao_bits & 32u) != 0 ? 2 : 1;
    }
    return -1;
}
// The AO mask writer's tier bits for a tier (ProgramTier's inverse).
constexpr u8 AoTierBits(u32 tier) {
    return tier == 0 ? u8{4} : tier == 2 ? u8{32} : u8{0};
}
static_assert(ProgramTier(Level(true, 0), 0) == 0 && ProgramTier(Level(true, 1), 0) == 1 &&
              ProgramTier(Level(true, 2), 0) == 2);
static_assert(ProgramTier(0, 1 | 16 | AoTierBits(0)) == 0 && ProgramTier(0, 1 | 16 | AoTierBits(1)) == 1 &&
              ProgramTier(0, 1 | 16 | AoTierBits(2)) == 2 && ProgramTier(0, 1) == -1 &&
              ProgramTier(0, 16 | 4) == -1);
// One loop over a lamp shader's lights (2026-11-12 night, the cost sweep at Oedon Chapel: with a
// copy of the shadow code per light a 12-light shader was huge, so few pixels ran at once and
// every extra part cost several times its own work). VR_LIGHT_LOOP=0 (tests): a copy each.
inline bool LoopLights() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_LIGHT_LOOP");
        return env == nullptr || env[0] != '0';
    }();
    return on;
}
// A hit counts less towards the picture's edge, over EdgeBand of its size: the shadow of a thing
// leaving the view fades out instead of going in one frame (the ray knows what the eye sees only).
inline constexpr float EdgeBand = 0.04f;
inline bool OldRule() {
    static const bool old = [] {
        const char* const env = std::getenv("VR_LIGHT_RULE_OLD");
        return env != nullptr && env[0] == '1';
    }();
    return old;
}
inline constexpr float NearFadeM = 4.0f;
// The ray: from StartM off the surface to the clearance short of the light, at most MaxM long:
// an enemy between your lantern and the wall behind them throws their shadow on it. A light in
// your hand (within arm's reach: HeldInM of the eye, blending out to HeldOutM) clears its
// holder's hand (ClearM); one farther away sits in a lamp, a sconce, a candle's holder or a fire's
// pyre, which must not shadow its own light: ClearFixtureM, or ClearPerRadius of the light's
// reach when that is more (a bonfire of 5 m clears 0.75 m of logs and bodies round its flame).
// (Until 2026-11-12 night: since then ClearSmallM above, 6 to 32 cm by the reach.)
inline constexpr float StartM = 0.03f;
inline constexpr float ClearM = 0.15f;
inline constexpr float HeldInM = 0.7f;
inline constexpr float HeldOutM = 1.0f;
inline constexpr float ClearFixtureM = 0.35f;
inline constexpr float ClearPerRadius = 0.15f;
inline constexpr float MaxM = 7.0f;
// Steps along it, closer together near the surface (t = (i + jitter)^2 / n^2): contact shadows
// stay sharp and a long shadow still finds a body far up the ray. n follows the light's strength
// there (its falloff w = 1 - reach * d^2, 1 at the light): MinSteps at the rim of its reach, Steps
// beside it; below MinWeight the light adds too little to shadow. The jitter moves every frame,
// so the upscaler smooths what few steps leave.
// The detail follows the pixel's distance from the eye (2026-11-09: "more detailed,
// maybe they have less detail afar and more detail nearby"): within DetailNearM the most steps
// (StepsNear), the finest spacing (PixelsNear) and the smallest light (LightNearM: crisp contact
// shadows), from DetailFarM on the fewest (StepsFar), the coarsest (PixelsFar) and the widest
// (LightFarM), linearly between.
inline constexpr float DetailNearM = 2.0f;
inline constexpr float DetailFarM = 8.0f;
// The far mode (2026-11-10): from DetailFarM to DetailFarthestM the fewest steps fall on to
// StepsFarthest (MinStepsFarthest at the rim of a light's reach), a step covers up to
// PixelsFarthest pixels and the light grows to LightFarthestM wide: a distant shadow is soft
// and coarse, as far as the eye can tell, and cheap.
inline constexpr float DetailFarthestM = 22.0f;
inline constexpr u32 StepsFarthest = 5;
inline constexpr u32 MinStepsFarthest = 3;
inline constexpr float PixelsFarthest = 8.0f;
inline constexpr float LightFarthestM = 0.15f;
// (2026-11-11: 34 and 7, were 56 and 12: with the gradient-noise jitter the shadows look the
// same in the dungeon and Old Yharnam stills, and the light pass costs about 0.14 ms less. Then
// 36 near, the user: "up a bit the samples of the shadows from 34 to 36".)
// (2026-11-12 evening, the user: "we need to increase a bit the quality of shadows": 42, 9 and 10,
// were 36, 7 and 8; with PixelsNear 1.2 and PixelsFar 3.4, were 1.35 and 4.)
inline constexpr u32 StepsNear = 42;
// (2026-11-12 night, the user: "up a bit the steps at medium distance": 14 and 2.6, were 9 and
// 3.4.)
inline constexpr u32 StepsFar = 14;
inline constexpr u32 MinSteps = 10;
inline constexpr float MinWeight = 0.02f;
// A surface turned away from a light gets none of its light, so its shadow is not marched
// (2026-11-10, the user: "make them faster without compromising on visuals"): skipped when the
// surface's normal (from its depth's neighbours) and the direction to the light make a cosine
// below FacingCull: a margin for normal maps bending a surface towards it. Not on a depth edge
// (a neighbour farther than FacingEdgeShare of the pixel's distance), where that normal is not
// the surface's.
inline constexpr float FacingCull = -0.25f;
inline constexpr float FacingEdgeShare = 0.05f;
// ... and no more than one step per that many pixels of the ray on screen (a short ray needs few;
// a long one is not skipped across a thin arm).
inline constexpr float PixelsNear = 1.2f;
inline constexpr float PixelsFar = 2.6f;
// The light is a ball this wide, each pixel aiming at another point of it (the noise moves every
// frame and the upscaler averages it): shadow edges soften with distance, as a flame's do.
inline constexpr float LightNearM = 0.025f;
inline constexpr float LightFarM = 0.06f;
// ... times BallShare (2026-11-11, the user: "make the shadows we cast sharper and less
// dotted/blurry/fuzzy"): each pixel aiming at another point of the ball made every soft edge a
// spray of dots that the upscaler smeared into fuzz. 0: every pixel aims at the light itself, so
// a lamp's shadow edge is as crisp as the depth allows (lamps, torches and candles are small).
// VR_LIGHT_BALL=<share> overrides it (tests).
inline constexpr float BallShare = 0.0f;
// A step is shadowed when the depth there is in front of it by more than BiasM and less than
// ThickM (things are taken to be that thick: no shadow from a wall far in front of the ray).
inline constexpr float BiasM = 0.02f;
// ... plus this much per metre along the ray (2026-11-11, the idea from Lantern Engine's point
// shadows: a step far up the ray lands on the depth image at a coarser spacing, and a surface
// seen at a glancing angle (a long floor, a wall beside you) then shadows itself in specks near
// the edge; the margin grows with the step's distance instead).
inline constexpr float BiasPerM = 0.03f;
// (2026-11-12, after the study of BBFSS and LanternEngine (0.6 m, a hard hit, a depth term in the
// bias): 0.5, was 0.35; a hit now full over the last HitBand of the thickness (was 0.4: partial
// hits made every shadow lighter), and the bias grows BiasPerEyeM a metre of the pixel's distance
// from the eye (a far floor seen at a glancing angle shadowed itself in specks).
// VR_LIGHT_SHADOW_OLD=1 (tests) keeps the old trace, fill lights' shadows and the cube-shadowed
// programs, for before/after pictures.)
// (2026-11-12 night, the user, after "thinner blockers" pictures and why they may hole a thick
// pillar's shadow: "Let's do 40cm and I'll test in game": 0.4, was 0.5. A thin thing's shadow
// is a smaller blob; a ray deep behind a big thing's visible face counts a little less.)
inline constexpr float ThickM = 0.4f;
inline constexpr float ThickMOld = 0.35f;
inline constexpr float HitBand = 0.15f;
inline constexpr float HitBandOld = 0.4f;
inline constexpr float BiasPerEyeM = 0.006f;
inline bool OldShadows() {
    static const bool old = [] {
        const char* const env = std::getenv("VR_LIGHT_SHADOW_OLD");
        return env != nullptr && env[0] == '1';
    }();
    return old;
}
// ... plus this much per metre along the ray (its steps grow: a step that lands behind a body
// still finds it).
inline constexpr float ThickPerM = 0.15f;


} // namespace Shader::Optimization::LightShadow
