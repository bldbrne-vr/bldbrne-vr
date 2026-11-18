// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

// Wet streets (vr.ini wet_streets; core/vr/game_reflections.cpp turns the game's unused
// screen-space reflections on). The game's trace (FS 0x6c2323c2)
// marches one ray a pixel through the depth,
// a texel a step, up to the target's width in steps; takes any point behind the depth by less
// than an NDC-depth "thickness" as a hit; reads last frame's colour there through its
// reprojection without checking the result is on the screen; and weighs the hit by
// 1 - (2 |uv - 0.5|)^2 of the pixel's OWN place on the screen. Its composite (FS 0x3fe9e964)
// then multiplies the already weighted colour by the weight again. Reflections faded out over
// the lower half of the view (where the street is), darker than none where they were partly
// found, different in each eye, and an infinity in the scene's colour reached the upscaler.
// ReflectionsPass (reflections_pass.cpp) puts VrReflection in place of the trace:
//  - a normal steadied towards the depth's own plane (flat puddles mirror, the G-buffer's
//    normal-map ripple kept to RippleMax);
//  - MaxSteps points along the ray, spaced closer near the pixel, jittered per pixel and per
//    frame (the upscaler averages it), RefineSteps halvings at the first crossing, the crossing
//    kept only where ray and surface are within a thickness in metres (a ray passing behind a
//    pole goes on);
//  - last frame's colour only from inside last frame's screen, finite and clamped;
//  - faded at the screen's edges where the HIT is (not where the pixel is), for rays turned
//    towards the eye and near the game's own reach.
// The composite takes the hit colour back out of the weighted one (rgb / a), so the reflection
// counts once. Neither adds a pass, an image or a buffer.
namespace Shader::Optimization::Reflections {

// Startup policy: with wet streets on (VR_REFLECTIONS_TRACE=0|1 overrides it, tests).
inline bool Enabled(bool vr, bool setting, const char* override_value) {
    return override_value ? override_value[0] != '0' : vr && setting;
}

// The game's trace and its composite (the flags are set only for them; the pass checks shape).
inline constexpr bool IsTraceProgram(u64 hash) {
    return hash == 0x6c2323c2u;
}
inline constexpr bool IsCompositeProgram(u64 hash) {
    return hash == 0x3fe9e964u;
}

// ---- The trace ----
// Points along the ray (the game: up to the target's width, ~730). Spaced (i + jitter)^2 / n^2
// of the way, so the first ones are a texel or two apart (contact: a boot in a puddle) and the
// last ~2 L / MaxSteps (L the ray's length on screen). 32 since 2026-11-11 (the tester: "a cap 32
// for wet streets"; was 40): a fifth less of the trace, the far end of a long reflection a little
// coarser (the halvings still find the exact crossing).
inline constexpr u32 MaxSteps = 32;
// Halvings between the last point in front of the depth and the first behind it.
inline constexpr u32 RefineSteps = 5;
// The first point this many depth texels from the pixel (its own surface is never a hit).
inline constexpr float StartTexels = 1.5f;
// The farthest a ray reaches across the screen (uv units: 1 = the screen's width or height).
inline constexpr float MaxRayUv = 1.0f;
// A crossing is a hit when ray and surface are this close in view depth after the halvings: a
// share of the distance, at least ThickMinM, at most ThickMaxM (metres). A ray that passed
// behind an object's edge is metres behind it there and goes on.
inline constexpr float ThickRel = 0.012f;
inline constexpr float ThickMinM = 0.05f;
inline constexpr float ThickMaxM = 0.6f;
// The hit must lie at least this far along the ray (metres, plus a share of the pixel's
// distance): no reflection of the surface in itself.
inline constexpr float MinTravelM = 0.03f;
inline constexpr float MinTravelRel = 0.002f;
// Fade within this much of the screen's edge (uv) where the hit is, now and last frame.
inline constexpr float EdgeFadeUv = 0.06f;
// Rays turned towards the eye (view z of the reflected ray): none below FacingMin, full from
// FacingMin + FacingFade. In VR the street's rays go away from the eye; a wall's may not.
inline constexpr float FacingMin = -0.3f;
inline constexpr float FacingFade = 0.35f;
// Fade from this share of the game's own reach (its dword 78, view metres) to all of it.
inline constexpr float FarFadeStart = 0.8f;
// The normal: blended towards the depth's plane where the depth is continuous round the pixel
// (relative change ContinuityLo .. ContinuityHi) and the two agree (cosine AgreeLo .. AgreeHi),
// by PlaneBlend at most; what is left of the G-buffer's ripple across the plane at most
// RippleMax (the sine of its tilt).
inline constexpr float ContinuityLo = 0.01f;
inline constexpr float ContinuityHi = 0.03f;
inline constexpr float AgreeLo = 0.85f;
inline constexpr float AgreeHi = 0.97f;
inline constexpr float PlaneBlend = 0.9f;
inline constexpr float RippleMax = 0.04f;
// Last frame's colour is clamped to this (the scene's linear units; fire is ~10): a lamp's
// highlight cannot become a bright speck in a puddle, and an infinity cannot pass.
inline constexpr float MaxRadiance = 32.0f;

// ---- The composite ----
// The reflection's weight below this counts as none (rgb / a is not taken).
inline constexpr float MinWeight = 1e-3f;
// The reflection's weight times this (1 = as found; the colour is not changed).
inline constexpr float Strength = 1.0f;

// Sharper wet streets (2026-11-11, the polish list): the trace runs at about half the
// eye's size, so the composite's bilinear sample of it bleeds a reflected post into the street
// beside it. VrReflectionUpsample weighs the 4 texels round the pixel by how near their depth is
// to the pixel's (a joint bilateral upsample; one gather of the full-size depth gives all four),
// only on glossy pixels (lod below UpsampleLodMax: the sharp mip), nearer than UpsampleMaxM, and
// only where the 4 depths differ by more than UpsampleEdgeRel of the pixel's (an edge); elsewhere
// the game's sample as it is. VR_REFLECTION_UPSAMPLE=0 turns it off (tests).
inline constexpr float UpsampleLodMax = 1.0f;
inline constexpr float UpsampleMaxM = 50.0f;
inline constexpr float UpsampleEdgeRel = 0.05f;
// A texel's depth weight: 1 / (1 + (rel / UpsampleTol)^2), rel its depth's difference from the
// pixel's as a share of the pixel's.
inline constexpr float UpsampleTol = 0.04f;
// Less weight than this in all (every texel another surface): the game's bilinear sample.
inline constexpr float UpsampleMinSum = 1e-3f;

// Blood on glossy floors (2026-11-13: the blood pools round the corpse in the 1st
// Floor Sickroom "feel like dirty water"; the reflections are "a nice touch", but not every wet
// street should change). The game's blood decals leave the floor glossy with a grey F0, so the
// room's reflection lies on the dark red like on a puddle. The composite reads the surface's own
// colour (the G-buffer's albedo: the game's own read of it, moved before the reflection's sample,
// so no texture is read twice) and only where it is blood - red well above green and blue
// (1 - max(g, b) / r from ChromaLo to ChromaHi), and dark (r below DarkLo, none from DarkHi) -
// the reflection is seen through the blood: its green and blue scaled as the blood's own colour
// scales them against its red (by Tint), its mip LodAdd blurrier (a smaller mip: cheaper) and its
// weight WeightCut lower. Grey and brown (1 - max(g, b) / r at most ChromaLo) give a share of
// exactly 0: stone and puddles keep their reflection bit for bit. About 20 ALU operations a pixel
// (one reciprocal, linear ramps), no pass, image, buffer or texture read added. The limits are
// not yet checked against the game's own colours (reddish wood): BloodByDefault stays off.
inline constexpr float BloodChromaLo = 0.55f;
inline constexpr float BloodChromaHi = 0.8f;
inline constexpr float BloodDarkLo = 0.6f;
inline constexpr float BloodDarkHi = 0.9f;
inline constexpr float BloodTint = 1.0f;
inline constexpr float BloodLodAdd = 1.5f;
inline constexpr float BloodWeightCut = 0.3f;
// With wet streets (RuntimeInfo::VrBloodFloors on the composite: its own key bit, so turning it
// on or off retranslates that one program only; VR_BLOOD_REFLECTION=0|1 overrides it).
// BloodByDefault: off until checked on the mock headset and picked by the user.
inline constexpr bool BloodByDefault = false;
inline bool BloodEnabled(bool vr, bool wet_streets, const char* override_value) {
    return override_value ? override_value[0] == '1' : vr && wet_streets && BloodByDefault;
}

} // namespace Shader::Optimization::Reflections
