// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstdlib>

#include "common/types.h"

// Deeper stones (2026-11-11: "bumpy 3D stones", the idea from Lantern Engine's parallax
// patch, our code): three of the game's G-buffer programs give their cobbles and bricks depth with
// parallax occlusion mapping. Each picks a layer count from the view's angle (a float the game
// mixes between two constants), converts it to an integer for its loop and takes 1 / count as the
// layer's step; it then walks the height map one layer at a time until it goes below it. Few
// layers show as stacked slices on a grazing street. ParallaxPass (parallax_pass.cpp) finds that
// float (the operand of the ConvertS32F32 whose result bounds the loop, through SGreaterThan32,
// and is converted back and inverted for the step) and multiplies it by StepScale: the count and
// the step change together, so the walk covers the same depth in finer layers. In the
// ambient_occlusion setting's VR sessions (the New graphics page's Shading); VR_PARALLAX=0|1
// overrides it.
namespace Shader::Optimization::Parallax {

inline bool Enabled(bool vr, bool setting, const char* override_value) {
    return override_value ? override_value[0] != '0' : vr && setting;
}

// The three parallax programs (the shader archive's GXFlver POM variants). Sorted.
inline constexpr std::array<u32, 3> Programs{0x053bbf3bu, 0x08ccd4c7u, 0x505acc58u};
static_assert(std::ranges::is_sorted(Programs));

inline bool IsParallaxProgram(u64 hash) {
    return hash <= 0xffffffffull && std::ranges::binary_search(Programs, static_cast<u32>(hash));
}

inline constexpr float StepScale = 1.5f;
// The layer count at most after the scale (a bound on the loop whatever the game's constants).
inline constexpr float MaxLayers = 96.0f;

// The free ones (2026-11-11: "Let's do the free ones. And see what it looks like"; the
// game only walks within its texture detail level 3, some 3 to 5 m of you on a street, fading
// from level 2, so more layers alone barely showed):
//  - darker gaps (Lantern Engine's cavity idea, our code): the walk's final spot already samples
//    the height map's texture for its bump map, the height channel unused there. The albedo
//    (render target 2) takes 1 - CavityStrength x (1 - height) and the reflectance (render
//    target 3, sqrt F0) its square root, both faded with the walk itself (the game's own fade,
//    detail level 2 to 3): the low parts of the stones darker, the tops as they were.
//    VR_PARALLAX_CAVITY=<0..1> overrides it (tests);
//  - deeper bumps: the height scale (the walk's offset per unit of height) times DepthScale (1:
//    the game's; 1.4 since the tester, 2026-11-11, from the three-way pictures: "#1 yes").
//    VR_PARALLAX_DEPTH=<0.5..3> overrides it (tests);
//  - a random start: the walk starts a random fraction of a layer lower, per pixel and per frame
//    (a hash of the texture coordinates' bits, which move with the upscaler's sub-pixel jitter),
//    so the layers' steps become fine noise the upscaler averages away instead of stacked slices;
//    its last layer then lies below the bottom, so the deepest spots are still found.
//    VR_PARALLAX_JITTER=0|1 overrides it (tests).
inline constexpr float CavityStrength = 0.5f;
inline constexpr float DepthScale = 1.4f;
inline constexpr bool Jitter = true;
// The game's walk is full up to this texture detail level and gone at FadeEndLevel (its own
// constants, recognized in the IR: the cavity fades the same way).
inline constexpr float FadeEndLevel = 3.0f;

} // namespace Shader::Optimization::Parallax

// Raised brickwork (vr.ini raised_bricks; the tester, 2026-11-11: "bumpy 3D stones", "I care a lot
// about perf"; 2026-11-12: "it is changing how textures are applied and looks wrong ... make it
// look super good but also super performant and safe"). The game's bump maps keep the normal in
// red, green and alpha; the plain bump materials never read their blue, which is 255 everywhere on
// most of them and, on brick walls, big stone blocks and roof shingles, a height map: faces white,
// joints grey (video_core/renderer_vulkan/vr_bump_class.h tells them apart from the texture's own
// bytes, per draw: VrPushData::bump_depth / bump_scale). On those only, BumpHeightPass
// (parallax_pass.cpp), in the G-buffer programs (five or more colour targets) whose single bump
// map is decoded from red, green and alpha:
//  - parallax occlusion: VrBumpMarch walks down the view ray through the height map from the
//    surface to the draw's depth below it and moves every texture read of the material to the
//    spot it meets (VrBumpSample: a gradient read there with the unmoved spot's gradients, so the
//    joints keep their detail level). The faces are the surface: a pixel on a face moves nothing
//    and reads nothing more; only the joints' pixels march. Each eye sees it from its own side:
//    real depth in the headset;
//  - darker joints: the albedo (render target 2) takes 1 - Strength x the joint's depth at the
//    spot found (times JointGain, at most 1), the reflectance (render target 3) its square root:
//    the shading that makes a joint read as sunk in flat light.
// Both fade with the texture's detail level (FadeStart to FadeEnd) and need the push data.
// Every other draw of those programs (flat blue, masks, greys: 495 of 504 bump maps in Central
// and Old Yharnam) takes the game's own reads, untouched: each of ours sits behind one test of the
// draw's pushed depth, the same for every pixel of the draw.
// Left alone: a program with two bump maps (a layered material: one layer's height would drag
// the other's colour), one whose reads cannot each be moved, the parallax programs above.
// VR_BUMP_GAPS=0|1 overrides the setting; VR_BUMP_PARALLAX=<metres> the depth (0: darker joints
// only); the live-tuning key lighting.brick_depth_cm the depth too, without a restart.
// (History, 2026-11-11..12: darker gaps by the height's drop below its surroundings, two gathers;
// a one-step offset, 3 to 12 cm, the surface at the map's mean then its median: whole stones
// slid; the march at 12 cm with the median as the surface: half of every face still sank, and
// masks, tiles and grain were taken for height maps.)
namespace Shader::Optimization::BumpGaps {

inline bool Enabled(bool vr, bool setting, const char* override_value) {
    return override_value ? override_value[0] != '0' : vr && setting;
}

// The programs it may rewrite: the map's G-buffer programs (the shader archive's GXFlver_*DOL*_Gbuf
// pixel programs, 114, every one; no character program shares a hash with one). Until 2026-11-13
// night any G-buffer program with a red, green, alpha bump decode was taken: the characters' skin,
// faces, eyes, hair and cloth (GXFlver_*Ibl*_Gbuf, the _Bloody_ twins among them) were rewritten
// too, and the hunter's bloodied coat (ColDifSpcBumpIbl_Bloody_Cloth) showed as flat black, green
// or magenta shapes in some frames, its depth 0 or not (the tester: "it's the blood on cloth"; "we
// should be careful what we rewrite"). Stonework is the map's. Sorted.
inline constexpr std::array<u32, 114> MapPrograms{
    0x01d570b2u, 0x02be56bdu, 0x034d505eu, 0x035fe8e8u, 0x053bbf3bu, 0x07e58ebbu, 0x0836b01bu,
    0x08ccd4c7u, 0x0dece0d0u, 0x0e1e22e5u, 0x0ebd3526u, 0x10ee741au, 0x1854e3fbu, 0x1c3953c6u,
    0x1e8684adu, 0x1f36bb8fu, 0x20f23dc2u, 0x23ba1403u, 0x266fa394u, 0x2abca547u, 0x2b7dbce4u,
    0x2e0f9a28u, 0x2e63b84eu, 0x2f7f23ceu, 0x36b17f89u, 0x36be4409u, 0x379756fbu, 0x38aa2913u,
    0x3ba50ca6u, 0x3c9823b5u, 0x4073d335u, 0x43df8173u, 0x44d46294u, 0x4510b8ecu, 0x4566da5bu,
    0x460d9b81u, 0x48b3fd92u, 0x4dcf4a9cu, 0x50071941u, 0x5030bf87u, 0x520ce3b6u, 0x5a3d7025u,
    0x5ba0882fu, 0x5ca6f6b8u, 0x6077dc0du, 0x6396b110u, 0x6981b505u, 0x6b45aeffu, 0x6bf44241u,
    0x6d9c1ec9u, 0x70590eb4u, 0x7252ae4fu, 0x7519581au, 0x7b4396f0u, 0x7d4a4c29u, 0x806cb150u,
    0x84458196u, 0x8559099eu, 0x86c17040u, 0x879bbf50u, 0x889123a5u, 0x88996cb3u, 0x88adafbau,
    0x891e0833u, 0x8a4ae47du, 0x8aba0ecdu, 0x8ec8cf38u, 0x8eeb1889u, 0x8f860ac6u, 0x9ba8e2dau,
    0x9e56839fu, 0x9f59685fu, 0xa04f8586u, 0xa15b17a0u, 0xa79163e9u, 0xa83a8157u, 0xa8789812u,
    0xaa601a86u, 0xb79164a3u, 0xb895cd57u, 0xbb10e440u, 0xbb934409u, 0xbc1845d3u, 0xbd9f1566u,
    0xbf4e1684u, 0xbf838991u, 0xc275f10cu, 0xc4d5f5efu, 0xc54923bfu, 0xc7a71f2cu, 0xca7226ecu,
    0xcb598d18u, 0xce0c3786u, 0xcf2b4ebau, 0xd1243d56u, 0xd580e83bu, 0xd879ad9du, 0xdae34d14u,
    0xdb659719u, 0xdbb222ebu, 0xdc1abd4bu, 0xdca13fdau, 0xdf65bd57u, 0xe009d313u, 0xe0f8ffb0u,
    0xe23bc29eu, 0xe54449c7u, 0xe7088569u, 0xeb96b30fu, 0xf2feabdeu, 0xf6250d60u, 0xf65d1efau,
    0xfb50d8ccu, 0xfdc954bcu,
};
static_assert(std::ranges::is_sorted(MapPrograms));

inline bool IsMapProgram(u64 hash) {
    return hash <= 0xffffffffull && std::ranges::binary_search(MapPrograms, static_cast<u32>(hash));
}

// The faces' level: a blue at or above it has no depth (vr_bump_class.h FaceLevel, 244 of 255).
inline constexpr float FaceTop = 244.0f / 255.0f;
// The joints' depth at the class's full depth (metres): mortar and the gaps between stone blocks
// are 1 to 3 cm deep; until 2026-11-12 12 cm, which slid the joints' colour over the bricks.
// Then 3 cm, too shallow to notice in the headset (the user, the same night: "I didn't feel the
// parallax mapping"); 6 cm since ("let's do 6cm, I think it looks better"). 3 cm again since
// 2026-11-13 night (the user, once the flat-parallax bug was fixed, 775d0b758: "the 6cm increase
// ... is too strong. It worked in the headset and it felt great, but we should make it 3cm"; the
// first 3 cm may have been the bug's flat surfaces).
inline constexpr float ParallaxDepthM = 0.030f;
inline constexpr float MaxDepthM = 0.080f; // a bound on the overrides
// (0.5 until the first brick wall with light mortar, 2026-11-12: its mortar lines went as dark as
// its bricks. A recessed joint keeps most of its own colour.)
inline constexpr float Strength = 0.35f;
inline constexpr float JointGain = 2.0f;
inline constexpr float FadeStart = 2.5f;
inline constexpr float FadeEnd = 4.0f;
// The march: one read of the blue per PomPixelsPerStep of the ray's length on screen,
// PomMinSteps to PomMaxSteps, then the crossing between the last two (linear); none where the
// ray is shorter than PomMinPixels (nothing to see). The ray grows as 1 / cos of the view's angle
// to the surface (at most 1 / PomMinCos) and is held softly under PomMaxPixels on screen.
inline constexpr float PomPixelsPerStep = 2.5f;
inline constexpr u32 PomMinSteps = 2;
inline constexpr u32 PomMaxSteps = 12;
inline constexpr float PomMinCos = 0.35f;
inline constexpr float PomMaxPixels = 32.0f;
inline constexpr float PomMinPixels = 0.5f;

} // namespace Shader::Optimization::BumpGaps
