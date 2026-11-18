// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "common/types.h"

// The air pass (2026-11-13: the moon beams "shimmer or blink or show grid like
// patterns", and neither they nor the lamps' halos can be seen against the sky).
//
// Why: the moonlight in the air (moon_shafts.h) and the lights in the air (fog_light.h) were added
// in the light composite, which the game draws with a stencil test (only pixels with geometry) and
// before its forward draws (the sky dome, water, particles): nothing of them ever reached a sky
// pixel. And the moonlight was marched at every pixel with a per-pixel, per-frame jitter that only
// the upscaler's history hid (not while the head turns, not in the foveated edges' coarse blocks,
// and the same pattern in both eyes).
//
// Now, in two of the game's own full-screen passes (its frame's order, 2026-11-13, VR_DRAW_ORDER):
//  - Compute: the shadow mask's first blur (GXScreenSpaceShadow_Blur, right after the moon's
//    shadow pass, whose cascades and atlas the rasterizer has by then). One fragment in four (the
//    even pixels; every fragment of a coarse block) marches the moonlight for its half-size cell,
//    sky included (the ray then ends at the air's far distance), with one of four fixed jitters by
//    the cell's place in a 2x2 pattern (nothing changes from frame to frame), and stores the
//    amount, the air's transmittance and the cell's distance in the cells buffer
//    (Shader::BufferType::VrAirCells).
//  - Apply: the copy of the finished scene the bloom and the tone map read
//    (PointSampleColorClamp1; drawn after the sky and every particle). Each pixel takes the 3x3
//    cells round it, tent weights (every one of the four jitters counts the same: 4 x the steps
//    per pixel, no noise left to hide) times how close the cell's distance is to its own (no
//    light leaking over an edge); the colour times the transmittance, plus the moon's colour times
//    the amount. A sky pixel also gets the lamps' halos (the lights in the air's own closed form,
//    no surface to end the ray), which the composite could not give it.
// The moon's colour is no longer read at six screen points by every pixel: one fragment in 1024
// of the composite adds its own light set's colour to a small host-visible buffer
// (Shader::BufferType::VrAirProbe), the rasterizer reads the sums a few frames later, eases the
// colour over ProbeEaseS and holds the last one while no surface gives any (no blink).
//
// VR_AIR_PASS=0 (tests): the old way (both in the composite).
namespace Shader::Optimization::AirLight {

inline bool Pass() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_AIR_PASS");
        return env == nullptr || env[0] != '0';
    }();
    return on;
}

// The lamps' glow in the air too (VR_AIR_HALOS=0, tests: the composite's, as before): the scene's
// copy gives it at every pixel, after the game's smoke and mist sprites (2026-11-13:
// "halos drawn in front of smoke instead of behind it"), with the composite's own plane test and
// shadowed air (emit_spirv_light_shadow.cpp VrFogLightParts). The composite keeps what lights its
// surfaces (the flames' fill, the muzzle flash).
inline bool Halos() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_AIR_HALOS");
        return Pass() && (env == nullptr || env[0] != '0');
    }();
    return on;
}

// No flame, no halo (2026-11-13: "an atmospheric point light is added, it has no lamp
// or fire nearby ... and it ends up as a halo just floating. ... if there is no light nearby, then
// it's not a halo -- but it affects any smoke etc nearby"). VR_AIR_SOURCES=0 (tests): the old
// rule (fog_light.h's steady wide lights only).
// The scene's copy looks for a flame at each light's place: of the fragments whose ray passes
// within SourceOuterM of a light that is not behind their surface (SourceBehindM), the brightest
// within SourceInnerM and the brightest of the ring beyond it go to the probe's buffer (four
// dwords a light: the inner's brightest and count, the ring's). A flame, a lamp's glass or a
// candle is a small bright thing: the inner's brightest is SourceRatio times the ring's (a wall
// the light itself brightens is about as bright in both). The rasterizer reads the votes a few
// frames later and counts a light as having a source once SourceConfirm votes say so; it then
// keeps its glow for as long as the lamp memory keeps the light. Lights the game spawns (a
// torch, your lantern) and lights that flicker (fires) count without a vote. A light without a
// source gives no glow of its own but stays in the list for the smoke (particle_air.h).
inline bool Sources() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_AIR_SOURCES");
        return Halos() && (env == nullptr || env[0] != '0');
    }();
    return on;
}
// (0.35 m until 2026-11-14: an Oedon Chapel light hangs some 0.3 m over its candle, the candle's
// flame fell inside 0.35 m and the halo sat on the bare pillar above it: "it should have removed
// that" (the user). The flame must be AT the light's place now; a flame beside it counts against.)
inline constexpr float SourceInnerM = 0.18f;
inline constexpr float SourceMatchM = 0.2f; // readback belongs to a halo still within this distance
inline constexpr float SourceOuterM = 0.8f;
inline constexpr float SourceBehindM = 0.35f;
inline constexpr float SourceNearM = 0.8f;  // nearer lights are not looked at (the disc fills the view)
inline constexpr float SourceRatio = 2.5f;
// A confirmed light votes for itself from SourceRatioKeep on: one whose ratio hovers at
// SourceRatio must not gain and lose its glow in turns (the chapel's log, 2026-11-14).
inline constexpr float SourceRatioKeep = 1.8f;
// A light that flickers (a fire, a candle) counted without a vote until 2026-11-14: the chapel's
// candle light, 0.3 m over its candle, kept its halo on the bare pillar that way. It is voted on
// too now, by a rule a wide fire passes: something bright at its place (SourceFireLum of the
// scene's brightness) and no dimmer than what is round it (SourceFireRatio); a bonfire fills the
// ring as well, so the steady lights' ratio would turn it down.
inline constexpr float SourceFireRatio = 0.8f;
inline constexpr float SourceFireLum = 1.2f; // (0.7 let two flickering lights on lit chapel walls through, 0.72..0.95; a candle flame read 1.3..2.5)
inline constexpr float SourceLumScale = 256.0f; // the brightness's fixed point
inline constexpr u32 SourceMinInner = 3;  // fragments (of the first eye, every second column and row)
inline constexpr u32 SourceMinRing = 6;
// (2026-11-14: 0.25 for, 0.03 against, and a confirmed light kept its glow for as long as it was
// remembered. The user: a halo with no flame on an Oedon Chapel pillar, "it should have removed
// that. Also, I see sometimes such lights appearing after a while". The chapel's log: two lights
// "with a source" whose votes had long been against, confidence 0.31 and 0.64; a few votes for
// from some angle (something bright behind the light's place) had been enough, for good. Now a
// light must keep being seen with a flame: one vote against undoes half a vote for, and a
// confirmed light whose confidence falls to SourceDrop loses its glow again (it fades over
// fog_light.h FlameOutS). A light that is not looked at (off the screen, hidden) keeps its state.)
inline constexpr float SourceYes = 0.12f; // a vote for moves the light's confidence by this
inline constexpr float SourceNo = 0.06f;
inline constexpr float SourceConfirm = 0.7f;
inline constexpr float SourceDrop = 0.2f;
inline constexpr u32 SourceLights = 24; // fog_light.h MaxLights
inline constexpr u32 SourceDwords = 4;

inline constexpr bool IsComputeProgram(u64 hash) {
    return hash == 0x11ffdfb5u; // GXScreenSpaceShadow_Blur
}
// The compute's real half-size host: the game's screen-space reflections pass (GXSsr, half the
// eye's size, after the shadow mask's blur and before the composite). Every one of its fragments
// is a cell, so the march costs a quarter of the full-size host's: there one fragment in four
// marches but the GPU runs the other three's lanes alongside (measured 2026-11-13: no saving).
// The blur stays the host while no reflections pass is drawn (it computes when the half-size host
// was not seen in the last frames).
inline constexpr bool IsHalfProgram(u64 hash) {
    return hash == 0x6c2323c2u; // GXSsr
}
inline constexpr u32 HalfHostFrames = 3; // the blur takes over after this many frames without it
// A frame's passes need not carry the same camera record: one FrameSlack records apart still
// counts as this frame's (the user in the headset, 2026-11-14: a dark blink, every part of the air
// gone for a frame; the scene copy had asked for the very record the shadow pass was drawn with).
inline constexpr u64 FrameSlack = 3;
inline constexpr bool FrameNear(u64 a, u64 b, u64 slack = FrameSlack) {
    return a != ~0ull && b != ~0ull && (a >= b ? a - b : b - a) <= slack;
}

inline constexpr bool IsApplyProgram(u64 hash) {
    return hash == 0xccbf44a6u; // PointSampleColorClamp1
}

// The parameters (Shader::BufferType::VrAir, Rasterizer::VrAirUpload), dwords:
//  0 flags (u32): FlagCompute, this draw computes the cells; FlagApply, this draw is the scene's
//    copy (the depth and the camera are this frame's); FlagHalos, sky pixels take the lamps'
//    halos; FlagCells, the cells are this frame's
//  1 the cells a row (u32)   2 the cell rows (u32)   3 an eye's cells (u32: the second eye's first)
//  4..6 the moon's colour   7 the distances' cap (view units: farther counts as this far)
//  8..11 ndc = fragment coordinates * (8, 9) + (10, 11)
//  12 beyond this view distance (or not a number) a pixel is sky
//  13 the sky's share of the moonlight in the air (1: as in front of everything else; live key
//     lighting.air_sky)   14..15 0
//  16..31 the first eye's ndc-to-view matrix (rows; m[r][c] = dword 16 + 4 r + c), 32..47 the
//  second eye's (the same without native stereo).
inline constexpr u32 FlagCompute = 1;
inline constexpr u32 FlagApply = 2;
inline constexpr u32 FlagHalos = 4;
inline constexpr u32 FlagCells = 8;
inline constexpr u32 FlagSources = 16; // the fragments vote on the lights' sources
inline constexpr u32 FlagMoonAim = 32; // sky fragments report the brightest of them
inline constexpr u32 FlagGrid = 64;    // the fog grid (fog_grid.h) was built for this frame
inline constexpr u32 MatrixDword = 16;
//  48 the probe's slot this frame (u32)   49..50 1 / the picture's width and height   51 0
//  52..63 this frame's view to world (three rows of four: the rotation, then the camera's place)
//  64.. the mist (Mist below)
inline constexpr u32 SlotDword = 48;
inline constexpr u32 ViewDword = 52;
inline constexpr u32 MistDword = 64;
//  84.. the moon's rays (Rays below)
inline constexpr u32 RaysDword = 84;
//  96.. the distance haze (Mist::HazeDword)   99 the glow's knee (GlowKneeDword)
inline constexpr u32 ParamDwords = 100;
// A glow never outshines what it sits on (the user in the headset, 2026-11-14: "certain fires
// washed out because their glow was brighter than the fire (that needs attention)"; "the lamp
// glow capped to its flame"): the lamps' glow in the air counts 1 / (1 + L / GlowKnee) at a pixel
// whose own picture is L bright (the scene copy's own sample), and the air's dimming of that pixel
// 1 / (1 + L / (GlowDimKnees GlowKnee)): on the flame itself and on what it lights brightly the
// glow steps back and the mist dims less, in the dark air round it everything stays. So a fire
// is the brightest thing of its own glow, and a flame seen through mist stays hot. Live key
// lighting.glow_knee (0: the old way).
inline constexpr float GlowKnee = 1.0f;
inline constexpr float GlowDimKnees = 2.0f;
inline constexpr u32 GlowKneeDword = 99;

// The mist (2026-11-14: "How about having real thick mist"): a layer of drifting mist
// over the ground, in the cells with the moonlight (the same half-size march, no shadow lookups:
// MistSteps points to MistFarM, denser near the eye, the cell's jitter). At a point: Density
// (per metre) times exp(-height / HeightM) over the ground (the hunter's feet, eased: the
// rasterizer gives the eye's height over it) times a slow noise of the world place (two octaves
// of a gyroid, drifting with the wind: thick banks and thin gaps, Patchy of it) and clear within
// NearStartM to NearFullM of the eye (the hands stay clear). It scatters the moon's colour
// towards the eye (Ambient of it from all round, Forward of it by the moon's own phase) and dims
// what is behind it (its transmittance joins the cell's). The parameters' dwords from MistDword:
//  0 time (s)   1 density (0: no mist)   2 1 / height   3 the eye's height over the ground
//  4 far   5 ambient   6 forward   7 the noise's scale (1 / m)   8..10 the wind (m/s, world)
//  11 extinction   12 near start   13 near full   14 patchy   15 strength
// Live keys (VR_TUNE_FILE): lighting.mist (density), lighting.mist_height, lighting.mist_far,
// lighting.mist_strength, lighting.mist_patchy. VR_AIR_MIST=0: none.
namespace Mist {
inline bool Enabled() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_AIR_MIST");
        return Pass() && (env == nullptr || env[0] != '0');
    }();
    return on;
}
// The mist belongs to the Forbidden Woods (2026-11-14, after pictures of eight places
// with and without it: "we need to remove the mist everywhere except the forbidden woods"; then
// "if you think there are areas in game that could benefit from having it as a thin veil, maybe?").
// Elsewhere it greyed Cathedral Ward, muddied the Tomb of Oedon's own ground fog and dimmed Old
// Yharnam's fires. AreaShare: the share of its density by the game's area (core/vr/game_area.h),
// eased over AreaEaseS as the hunter crosses into another; 0 where the area is not known. A thin
// veil for another area is one line here. Live key lighting.mist_area (0..1; tests: the share
// whatever the area). vr.ini ground_mist turns it off everywhere.
inline constexpr float AreaShare(u32 area) {
    switch (area) {
    case 27: // the Forbidden Woods
        return 1.0f;
    default:
        return 0.0f;
    }
}
inline constexpr float AreaEaseS = 4.0f;
inline constexpr u32 Steps = 8;
// (0.12 a metre and 2 m, were 0.05 and 1.8: "real thick mist"; the pictures of 2026-11-14.)
inline constexpr float Density = 0.12f;
inline constexpr float HeightM = 2.0f;
inline constexpr float FarM = 60.0f;
inline constexpr float Ambient = 0.6f;
inline constexpr float Forward = 0.3f;
// The mist's own phase: wide (the moonlight's sharp one made the far banks towards the moon glow
// like a lamp in the first pictures).
inline constexpr float PhaseG = 0.3f;
inline constexpr float NoiseScale = 0.16f;
inline constexpr float WindX = 0.35f, WindY = 0.03f, WindZ = 0.2f;
inline constexpr float Extinction = 1.0f;
inline constexpr float NearStartM = 1.0f;
inline constexpr float NearFullM = 5.0f;
inline constexpr float Patchy = 0.85f;
inline constexpr float Strength = 0.045f; // (0.08: the far banks too bright)
inline constexpr float GroundEaseS = 2.0f; // the ground's height follows the hunter's feet over this
// Under a roof (a room, a cave) the mist thins to Indoors of itself: no sky fragment seen (the
// moon finder's report, air_light.h MoonAim) for IndoorsAfterS, eased over IndoorsEaseS; back as
// soon as the sky is seen again. Live key lighting.mist_indoors.
inline constexpr float Indoors = 0.25f;
inline constexpr float IndoorsAfterS = 12.0f;
inline constexpr float IndoorsEaseS = 4.0f;
// A lamp or fire standing in the mist lights more of it: its glow in the air (fog_light.h) times
// 1 + LampGlow at the ground, less with its height over it (the layer's own falloff), by the
// mist's density over the default. (The rasterizer's VrFogUpload; no noise: a glow that pulsed
// with the drifting banks would read as a flicker.)
// (0.4, was 2: the woods lamp became a white ball, the fireflies green ones. Live key
// lighting.mist_lamp.)
// (1.0 since the morning of 2026-11-14, the user in the headset: lamps "need to impact the fog
// more.. they feel kind of washed out ... if they are close then they get higher intensity, but I
// think a general bump is worth doing"; was 0.4.)
inline constexpr float LampGlow = 0.5f; // (1.0 until 2026-11-14: with NearGlow the woods lamp was a white ball that hid the lamp)
// ... more for a lamp near the eye: + NearGlow within NearGlowInM, none past NearGlowOutM (live
// key lighting.mist_near_glow); and its glow reaches further in the mist: its reach in the air
// times 1 + LampWide at the ground (the scene copy's list only: the flames' fill on surfaces keeps
// the light's own reach; live key lighting.mist_lamp_wide).
inline constexpr float NearGlow = 0.0f; // (1.0 until 2026-11-14, see LampGlow)
inline constexpr float NearGlowInM = 4.0f;
inline constexpr float NearGlowOutM = 14.0f;
inline constexpr float LampWide = 0.6f;
// A thin haze above the layer too (the tester: "the castle doesn't have it at all.. or the sky"):
// High of the layer's density at every height, so far things and the sky sit in the same air
// (0.04: a fifth of the way to the mist's colour over the mist's reach). Live key
// lighting.mist_high. The parameters' dword HighDword.
inline constexpr float High = 0.04f;
inline constexpr u32 HighDword = 87;
// The distance haze (2026-11-14: "I am not sure I felt the distant haze like on distant
// geometry"; the pictures: the mist ended at its march's reach, so the bushes 20 m off were
// fogged and lit while a cliff 300 m off got nothing and stood as a dark block against them).
// That thin haze thins out with height over HazeHeightM (its own scale: looking up the sky and
// the moon stay clear, towards the horizon it gathers), and past the march's reach it goes on in
// closed form (an exponential height fog's own integral along the ray) to the surface, HazeFarM
// at most (the sky: its SkyM): the farther a thing, the more of the mist's colour it takes. No
// samples: a few instructions a cell. Live keys lighting.mist_high (its density's share),
// lighting.haze_height, lighting.haze_far. The parameters' dwords from HazeDword: 0 1 / height,
// 1 far.
inline constexpr float HazeHeightM = 25.0f;
inline constexpr float HazeFarM = 600.0f;
inline constexpr u32 HazeDword = 96;
// What the haze past the march's reach scatters counts HazeGlow times (dword HazeDword + 2; live
// key lighting.haze_glow): the mist's own light is tuned for the thick banks near you (Strength:
// "0.08: the far banks too bright") and comes out as dark as the night behind it, so far things
// faded into a colour no different from their own (2026-11-14, tests: the far haze could not be
// seen at any density; at 4.4 times the strength it was a white wall).
inline constexpr float HazeGlow = 1.6f;
// (The apply works the haze past the march out for every pixel's own distance, and brings every
// cell's mist to the pixel's distance: the user in the headset, 2026-11-14, "a light blinking ...
// it's a distance lamp", stopped by lighting.mist=0. A far lamp is a few pixels; whether one of
// the nine cells round it sat on the lamp changed with each sway of the head, and without one it
// took the dimming of what is behind it. A cell's optical depth counts in the share of the marched
// way, (min(d, far) - near start) of the pixel over the cell's, CellStretch at most; the haze
// past the march has no moon phase of its own there: HazePhase of the forward share.)
inline constexpr float CellStretch = 4.0f;
inline constexpr float HazePhase = 0.8f;
// A lamp seen through the mist glows more (the pictures of 2026-11-14: the thick mist dimmed the
// far lanterns and gave nothing back): its glow times 1 + FarGlow * (1 - the mist's transmittance
// from the eye to it), the layer's own closed form along the straight way (no noise, the banks'
// mean BanksMean; at most the mist's reach). The flame itself and what it lights are dimmed by
// the mist like everything else; the glow, drawn over the mist, takes their place. Live key
// lighting.mist_far_glow.
inline constexpr float FarGlow = 1.5f;
inline constexpr float BanksMean = 0.8f;
// Under the blood moon (core/vr/game_area.h BloodMoon: the game's own flag, in the maps whose sky
// it changes) the air takes the night's own colour instead of the probe's. There the shadow
// light's colour is a strong warm orange while the scene is blue-black under a violet sky: the
// moonlit thin air lay over it as an even grey-brown veil, up to a third brighter than the game's
// picture at the Tomb of Oedon (2026-11-17, pictures of six looks). The user: "I want our haze to
// still be there... just to look good with the blood moon", and of the six: "violet, dimmer".
// Before the hue is greyed (Saturation below); blended over the probe's colour by a share eased
// over EaseS (the probe goes on measuring under it, and its own ease lets a dimmer colour in only
// over ProbeDownS, half a minute).
// VR_BLOOD_MOON_AIR="r,g,b" (tests; "0": the probe's colour, as it was).
// The game's fog taken over (fog_patches.h) takes its light's level from the air's luminance:
// with the probe's colour its banks were a bright grey pool under the blood moon, with the night's
// violet (a luminance of 0.15) all but gone. Under the blood moon their level is BankLevel, a
// night's (Cathedral Ward's own night air measures 0.23). VR_BLOOD_MOON_BANKS=level (tests).
namespace BloodMoon {
inline constexpr std::array<float, 3> Colour{0.14f, 0.12f, 0.36f};
inline constexpr float EaseS = 4.0f;
inline constexpr float BankLevel = 0.24f;
inline float BankLevelNow() {
    static const float v = [] {
        const char* const env = std::getenv("VR_BLOOD_MOON_BANKS");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.0f, 4.0f) : BankLevel;
    }();
    return v;
}
// r, g, b, and whether it is on.
inline std::array<float, 4> ColourNow() {
    static const std::array<float, 4> v = [] {
        std::array<float, 4> c{Colour[0], Colour[1], Colour[2], 1.0f};
        if (const char* const env = std::getenv("VR_BLOOD_MOON_AIR")) {
            if (std::sscanf(env, "%f,%f,%f", &c[0], &c[1], &c[2]) != 3) {
                c[3] = 0.0f;
            }
        }
        for (u32 k = 0; k < 3; ++k) {
            c[k] = std::isfinite(c[k]) ? std::clamp(c[k], 0.0f, 4.0f) : 0.0f;
        }
        return c;
    }();
    return v;
}
} // namespace BloodMoon
// The air's colour (the moonlight's and the mist's): the moon's, Saturation of its hue kept (the
// full blue read as paint in the first pictures). Live key lighting.air_saturation.
inline constexpr float Saturation = 0.6f;
} // namespace Mist

// The moon's rays through the gaps (2026-11-14: "next gen ... mystical"; the woods'
// trees are not in the moon's shadow atlas, so the shadowed march shows no shafts there). In the
// cells' march: from the cell towards the moon's place on the screen (this eye's; the rasterizer
// projects the direction the moon finder holds), Steps depths of the picture, the cell's jitter;
// a point with something in front of the sky (nearer than the sky's distance and farther than
// NearM: your own hands and weapon cast no rays) is blocked. The open share lights the mist
// towards the moon (Strength times a sharp phase, at most Peak, times the open share: added to the
// mist's own phase) and shapes the moonlight's own thin air (CoreShare of it follows twice the
// open share: gaps brighter, what is behind a trunk darker). Fades out as the moon leaves the
// view (the rasterizer's weight); none while the moon was never found. The parameters' dwords
// from RaysDword: 0..2 the first eye's moon (ndc x, y, the weight), 4..6 the second eye's,
// 8 strength, 10 the longest reach (ndc), 11 near. Live key lighting.moon_rays (0: none).
// OFF unless VR_AIR_RAYS=1 (2026-11-14: in the woods' pictures they showed nothing, even at eight
// times the strength: the canopy between the ground mist and the moon is closed, and the thin air
// above the mist carries too little light; 0.013 ms of the half-size pass. Kept for open places,
// untested there and in the headset.) (They come with the mist: its camera.)
namespace Rays {
inline bool Enabled() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_AIR_RAYS");
        return Mist::Enabled() && env != nullptr && env[0] == '1';
    }();
    return on;
}
inline constexpr u32 Steps = 8;
inline constexpr float Strength = 0.5f;
inline constexpr float PhaseG = 0.7f;
inline constexpr float Peak = 4.0f;
inline constexpr float MaxNdc = 1.4f;
inline constexpr float NearM = 1.5f;
inline constexpr float NearSoftM = 2.0f;
inline constexpr float CoreShare = 0.7f;
} // namespace Rays

// A cell (one u32): the amount as a half float (bits 0..15), the air's transmittance (8 bits),
// the distance (8 bits: log2 from DistanceLoM over DistanceOctaves octaves).
inline constexpr float DistanceLoM = 0.125f;
inline constexpr float DistanceOctaves = 13.0f;
// A cell counts fully within EdgeNear of the pixel's log2 distance and not at all past EdgeFar
// (a quarter and three quarters of an octave: 19% and 68% farther or nearer).
inline constexpr float EdgeNear = 0.25f;
inline constexpr float EdgeFar = 0.75f;
// The four jitters of the 2x2 pattern (by x & 1 + 2 (y & 1)): the ordered dither's.
inline constexpr float Jitter[4] = {0.125f, 0.625f, 0.875f, 0.375f};
// A pixel farther than this, or whose position is not a number, is sky: its ray has no end.
// (450 m, was 5000: the game's sky is a dome it draws with depth, some way off, not the far
// plane; at 5000 no pixel ever counted as sky and the moon finder saw none. The far backdrop
// beyond it counts as sky too.)
inline constexpr float SkyM = 450.0f;

// The colour probe (Shader::BufferType::VrAirProbe): ProbeSlots slots of ProbeDwords (the red,
// green and blue sums times the weight in 1/ProbeColourScale, the weights' sum in
// 1/ProbeWeightScale), one a frame; every ProbeEvery-th pair of columns and rows of the first eye.
inline constexpr u32 ProbeSlots = 4;
inline constexpr u32 ProbeDwords = 4;
// A slot: the colour's sums, then the lights' source votes (Sources above), then the brightest
// sky fragment (MoonAim below).
inline constexpr u32 MoonAimDword = ProbeDwords + SourceLights * SourceDwords;
inline constexpr u32 ProbeSlotDwords = MoonAimDword + 1;

// The glow aims at the moon you can see (2026-11-14: "aim the glow at the moon you can
// see"; it sat some 33 degrees off, at the shadow light's direction: project-godray-aim). The
// scene's copy reports the brightest sky fragment of the first eye (every MoonAimEvery-th column
// and row, cool or white ones only: a fire against the sky is not the moon) as one word in the
// probe's slot: its brightness (12 bits, 1/MoonAimLumScale), half its column and half its row (10
// bits each); an atomic maximum. The rasterizer turns it into a direction with that frame's
// camera, into the moon's light space (which does not turn with the head), and takes it for the
// moon once MoonAimFrames frames in a row agree within MoonAimAgreeDeg, it is within
// moon_shafts.h GlowMaxDeg of the shadow light, and it is not much dimmer (MoonAimKeep) than the
// one held. Held as moon_shafts.h GlowHoldMs. VR_AIR_MOON_AIM=0 (tests): the shadow light's.
inline bool MoonAim() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_AIR_MOON_AIM");
        return Pass() && (env == nullptr || env[0] != '0');
    }();
    return on;
}
// The finder no longer aims the glow (2026-11-14 midday: "LanternEngine's way": the
// glow along the shadow light. In the Forbidden Woods the finder held a bright cloud some 19
// degrees up while the moon stood near the zenith: "the moon shafts seem to be in the wrong
// spot"). It still runs: its sky reports are the mist's open-sky rule (Mist::Indoors).
// VR_AIR_MOON_AIM_GLOW=1 (tests): the glow at the moon it holds, as before.
inline bool MoonAimGlow() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_AIR_MOON_AIM_GLOW");
        return MoonAim() && env != nullptr && env[0] == '1';
    }();
    return on;
}
inline constexpr u32 MoonAimEvery = 4;
inline constexpr float MoonAimLumScale = 8192.0f;
inline constexpr float MoonAimCool = 0.8f; // blue at least this share of red
inline constexpr u32 MoonAimFrames = 12;
inline constexpr float MoonAimAgreeDeg = 2.0f;
inline constexpr float MoonAimKeep = 0.7f;
// The moon is held (the user in the headset, 2026-11-14: "IT BLINKS WHEN I AM WALKING AROUND ...
// the blink is when the moon gets hidden": with the moon behind a tree the next brightest patch of
// sky agreed for twelve frames and the glow jumped to it, and back when the moon returned). Once
// found:
//  - a sighting within MoonAimSameDeg of it (and not much dimmer, MoonAimKeep) only refines it
//    (MoonAimRefine of the way);
//  - anything else replaces it only when it is MoonAimReplace times brighter than the moon was
//    and agrees for MoonAimReplaceFrames (the first find was something else);
//  - a candidate counts only within MoonAimShadowDeg of the shadow light and MoonAimMinUp above
//    the horizon (a lamp's glow against the sky is not the moon). 80 degrees: in the Forbidden
//    Woods the moon in the sky is 64 from the shadow light (a still head at the lamp, 2026-11-14;
//    moon_shafts.h GlowMaxDeg's 50 turned the real moon away);
//  - it is kept while the shadow light points the same way in the world (within MoonAimAreaDeg:
//    the same place and hour), seen or not; another area starts again.
// And the glow never jumps: its direction glides to where it should point over MoonAimGlideS
// (the first find, a replacement, another area).
inline constexpr float MoonAimShadowDeg = 80.0f;
inline constexpr float MoonAimMinUp = 0.1f; // the world's y of its direction
inline constexpr float MoonAimSameDeg = 6.0f;
inline constexpr float MoonAimRefine = 0.1f;
inline constexpr float MoonAimReplace = 1.3f;
inline constexpr u32 MoonAimReplaceFrames = 90;
inline constexpr float MoonAimAreaDeg = 4.0f;
inline constexpr float MoonAimGlideS = 1.5f;
inline constexpr u32 ProbeEvery = 32;
inline constexpr float ProbeColourScale = 256.0f;
inline constexpr float ProbeWeightScale = 65536.0f;
inline constexpr u32 ProbeReadBehind = 2;
inline constexpr float ProbeEaseS = 0.5f; // (unused since the morning of 2026-11-14)
// The colour holds (the user in the headset, 2026-11-14: "IT BLINKS WHEN I AM WALKING AROUND. IT
// BLINKS TERRIBLY"; a walking test's log: the probe's colour went 0.36 -> 0.14 -> 0.37 red within
// two seconds as the view changed, and the whole mist with it, eased over half a second). What the
// probe gives is the mean of the light sets of the pixels in view: fewer moonlit pixels on screen,
// a dimmer mean. The moon's own colour is the brightest it gets, so the colour follows a brighter
// reading over ProbeUpS and a dimmer one only over ProbeDownS (walking into a darker place still
// takes it down, a glance at a wall does not).
// And a reading counts ProbeLevel at most (its brightest channel; its hue kept): arriving at the
// woods lamp the probe read 0.85 for a moment and the slow fall kept the air far too bright for
// half a minute (a walking test's log, 2026-11-14 midday); the Hunter's Dream reads 0.85 all the
// time and its veil was the strongest of the tour (the tester: "I do not want it to be super
// strong"). LanternEngine normalises its tint to 1 everywhere; ours keeps dim places dim.
inline constexpr float ProbeLevel = 0.6f;
inline constexpr float ProbeUpS = 2.0f;
inline constexpr float ProbeDownS = 30.0f;

} // namespace Shader::Optimization::AirLight
