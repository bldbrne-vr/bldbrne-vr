// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

// Moon shadows (2026-11-09: "AAA 2026" lighting; moon and sun shadows "sharp where an
// object meets the ground and softer with distance", and small things at your feet casting
// shadows too). Bloodborne draws its moon's (or sun's) shadow into a screen-space mask before the
// lighting: GXScreenSpaceCascadedShadow_PCF9 rebuilds each pixel's view position from the depth
// with its buffer's ndc-to-view matrix (rows at dwords 0..15), picks one of four cascades by view
// depth (splits at 80..82), maps the pixel into the 4096x4096 shadow atlas with that cascade's
// screen-to-shadow matrix (16 + 16c..), takes 4 hardware-PCF taps a texel or so apart and writes
// the lit share (faded out with distance) to its alpha, the pixel's packed depth to the rest; a
// bilateral blur and the composite follow. Every shadow is as soft as every other: a lamp post's
// foot as blurry as its top, a railing's thin bars lost.
// MoonShadowPass (moon_shadow_pass.cpp) puts VrMoonShadow in place of the 4 taps:
//  - contact hardening (PCSS, Fernando 2005): a blocker search round the pixel in the atlas gives
//    the occluders' mean distance above it; the moon's apparent size over that distance is the
//    penumbra: sharp where an object stands on the ground, soft at the shadow of a roof's edge;
//  - contact shadows: a short march from the pixel towards the moon through the eye's own depth
//    finds what the atlas is too coarse for (grass, hands, railings, things at your feet).
// The moon's direction comes from the cascade matrices (the direction they do not move along).
namespace Shader::Optimization::MoonShadow {

// Startup policy (vr.ini moon_shadows, VR_MOON_SHADOWS=0|1 overrides it): with an upscaler or
// without, like the light shadows (with one the mask is its input size and its noise averaged).
inline bool Enabled(bool vr, bool setting, const char* override_value) {
    return override_value ? override_value[0] != '0' : vr && setting;
}

// The game's screen-space cascaded shadow programs (the flag is set only for them; the pass
// checks the shape: the cascades' view position and the atlas taps).
inline constexpr bool IsShadowMaskProgram(u64 hash) {
    return hash == 0xf55643deu || hash == 0xb89e316bu;
}

// The moon's apparent radius (tangent): a penumbra widens by this much per metre between an
// occluder and the ground it shadows. The real moon's is 0.0045; a sharper-than-life light looks
// like a lamp, so a wider one (a hazy, moonlit sky): 1 m above the ground, 2 cm either side; a
// roof's edge 8 m up, 16 cm. (0.03 blurred the Hunter's Dream's leaf shadows on its steps to a
// flat grey.) 0.008 since 2026-11-11 (the tester: "make the shadows we cast sharper and less
// dotted/blurry/fuzzy ... AGAIN WITH INSANE PERFORMANCE"; 0.02, Lantern's, made 2.5 cm soft edges
// a metre up and 16 cm ones under a roof's edge): 1.8x the real moon, 8 mm and 6 cm; narrower
// penumbrae take the 12-tap filter more often. Then 0.0163 (2026-11-11, from the
// four-way pictures: "Moon 3.6x size please"): 3.6x the real moon, 1.6 cm soft a metre up, 13 cm
// under a roof's edge. Back to 0.008 on 2026-11-12 (the user: the sun's and the moon's shadows
// "are too faint now, too soft"; "make the moon be 1.8x"): 8 mm soft a metre up, 6 cm under a
// roof's edge. VR_MOON_LIGHT_TAN overrides it (tests).
inline constexpr float LightTan = 0.008f;
// The widest penumbra (metres either side): the blocker search reaches this far round the pixel.
// (0.15 with the sharper moon, was 0.3: the 9 gathers sample a smaller disc more densely.)
inline constexpr float MaxPenumbraM = 0.15f;
// ... but at least MinSearchTexels and at most MaxSearchTexels of the atlas (the near cascade
// has some 640 texels a metre, the farthest some 30, in Old Yharnam).
inline constexpr float MinSearchTexels = 3.0f;
inline constexpr float MaxSearchTexels = 192.0f;
// LanternEngine's tuning (2026-11-10, the user: "We keep their moon shadows"; Lantern Engine by
// Awaken, contact_shadow.comp, GPL-2.0-or-later): their blocker search's size, their filter that
// takes more taps the wider the penumbra, their bias and slope limit, and their contact march.
// Seamless cascades (LanternEngine's idea, our code; the tester, 2026-11-11: "seamless moon
// shadows"): in the last CascadeBlendShare of a cascade's depth range (view z, up to its split) a
// pixel takes the next cascade with the chance smoothstep(split - band, split, z), by per-pixel
// noise that moves every frame (the game's blur and the upscaler average it): one filter, never
// two, and no line where the shadows turn coarser. Only where the next cascade holds the pixel
// CascadeMarginTexels inside its quarter, at a depth inside its range (the game's cascades
// overlap by 640+ texels there); elsewhere the game's own pick. VR_MOON_BLEND overrides the
// share (0: off; tests).
inline constexpr float CascadeBlendShare = 0.2f;
inline constexpr float CascadeMarginTexels = MaxSearchTexels; // the widest blocker search
// The sharpest filter (atlas texels either side).
inline constexpr float MinFilterTexels = 0.5f;
// Blocker search: the pixel's own texels and BlockerRing more gathers (4 texels each) on a
// rotated spiral (36 texels; theirs 32 taps); PCF: FilterNear / FilterMid / FilterFar
// hardware-filtered compares on another, for a penumbra up to FilterNearTexels, FilterMidTexels,
// or wider.
inline constexpr u32 BlockerRing = 8;
inline constexpr u32 FilterNear = 12;
inline constexpr u32 FilterMid = 20;
inline constexpr u32 FilterFar = 32;
inline constexpr float FilterNearTexels = 1.0f;
inline constexpr float FilterMidTexels = 4.0f;
// Depth bias (metres along the moon, but at most half an atlas texel's width in metres) on top of
// the receiver plane's own slope, and the steepest slope the plane may take (metres of depth per
// metre across: a wall seen edge-on would otherwise shadow itself).
inline constexpr float BiasM = 0.04f;
inline constexpr float MaxSlope = 3.0f;

// Contact shadows: ContactSteps points over ContactM towards the moon, each tested against the
// eye's depth (an occluder in front of a point by more than the bias, ContactBiasM +
// ContactBiasPerM per metre of the pixel's distance + ContactBiasAlong per metre along the march,
// and less than ContactThickM: things are taken to be that thick). A hit near the pixel takes
// ContactStrength of the moon, one at the march's end half that. Only within ContactFarM of the
// eye (faded from ContactFadeM), only where the moon lights the pixel.
inline constexpr u32 ContactSteps = 16;
inline constexpr float ContactM = 0.6f;
inline constexpr float ContactBiasM = 0.03f;
inline constexpr float ContactBiasPerM = 0.004f;
inline constexpr float ContactBiasAlong = 0.04f;
inline constexpr float ContactThickM = 0.35f;
inline constexpr float ContactFadeM = 10.0f;
inline constexpr float ContactFarM = 16.0f;
inline constexpr float ContactStrength = 0.75f;

} // namespace Shader::Optimization::MoonShadow
