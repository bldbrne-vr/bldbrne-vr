// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdlib>

#include "common/types.h"

// The hunter's own shadow from lamps (2026-11-12; the tester: "I am not getting a shadow from lamps
// ... random objects get proper shadows though"). The lamp shadows (light_shadow.h) march the
// eye's depth, where the hunter's body is not: in first person it is below and behind the eye.
// So the body is given to them as capsules on its own bones (his posed skeleton every frame,
// VR::RecordPlayerBones: thighs, calves, hips to chest, chest to neck, head, arms), in the flat
// camera's view (Shader::BufferType::VrBodyCapsules, Rasterizer::VrBodyUpload), and every lamp
// shadow's ray also tests them: one shadow of him per lamp near him (the screen march's result
// and the capsules' taken together, the darker). Soft: the edge widens with the distance from the
// receiver to the blocking limb, as a small flame's does. VR_BODY_SHADOW=0 turns it off.
namespace Shader::Optimization::BodyShadow {

inline constexpr u32 MaxCapsules = 12;
inline constexpr u32 CapsuleDwords = 8; // a x y z, radius, b x y z, 0
inline constexpr u32 HeaderDwords = 8;  // count, half shift, valid, 0, sphere x y z r
// His real silhouette (renderer_vulkan/vr_body_maps.h, 2026-11-12): up to MaxMaps lamps have a
// depth map of his body as the game's moon shadow pass drew it; a light within MapMatchM of a
// map's lamp tests that map instead of the capsules. Each map's header (MapDwords): valid, its
// first texel, f (the projection's scale), near (game units), the lamp (flat view), the map's
// size, then the flat view to the lamp's frame (3 rows of 4). The maps themselves are another
// buffer (Shader::BufferType::VrBodyMaps), the lamp's own z per texel.
inline constexpr u32 MaxMaps = 2;
inline constexpr u32 MapTexels = 256; // a side (renderer_vulkan/vr_body_maps.h MapSize)
inline constexpr u32 MapDwords = 20;
inline constexpr u32 MapsAt = HeaderDwords + MaxCapsules * CapsuleDwords;
// The blocker grid's header (light_shadow.h Grid; written whatever his body's state): on, the
// frame's stamp, the voxel's side (game units), the voxels' life (frames), the eyes' half shift
// (eye x = flat x - shift, the capsules' convention), the nearest a pixel fills it from (game
// units), the game units a metre, the march's blocker thickness (game units; 0: light_shadow.h
// ThickM; the live key lighting.shadow_thick_cm), then the flat view to the world (3 rows of 4:
// world = row . (v, 1)). The live key lighting.grid=0 turns the grid off (header on 0).
inline constexpr u32 GridAt = MapsAt + MaxMaps * MapDwords;
inline constexpr u32 GridDwords = 20;
// The lights the game keeps on the camera (light_shadow.h AttachedFromM): their count, the eyes'
// half shift, 1 when they are removed (vr.ini camera_fill_light off), a spare, then up to 4 of
// them: their place in the flat view and their reach dword (x y z k).
inline constexpr u32 AttachedAt = GridAt + GridDwords;
inline constexpr u32 AttachedDwords = 20;
// Tests (live keys): the light classes (by the light's dword 7: 1 a spawned light, 0.5 a lamp, 20
// the map's, 2 any other) that cast nothing (lighting.mute_cast), those moved for their shadows
// only (lighting.nudge_sel), and the move in the eye's view (lighting.nudge_x/y/z, cm): mask,
// x, y, z, selection, then spares.
inline constexpr u32 ProbeAt = AttachedAt + AttachedDwords;
inline constexpr u32 ProbeDwords = 8;
// The scene maps' slots (light_shadow.h SceneMaps, 8 of them): their count, three spares, then
// per slot its lamp's place in this frame's flat view and its reach dword (x y z k).
inline constexpr u32 SlotsAt = ProbeAt + ProbeDwords;
inline constexpr u32 SlotsDwords = 16 + 12 * 16; // light_shadow.h SceneMaps: SlotHeader, Slots x SlotDwords
// The lamps his shadow comes from (2026-11-12 night: "up to 3 shadows for our character,
// no more"): their count (0: any lamp, as before), three spares, then up to BodyLamps of them in
// this frame's flat view (x y z, a spare): the strongest at him, the first two with his maps.
inline constexpr u32 BodyLamps = 3;
inline constexpr u32 BodyLampsAt = SlotsAt + SlotsDwords;
inline constexpr u32 BodyLampsDwords = 4 + 4 * BodyLamps;
inline constexpr u32 BufferDwords = BodyLampsAt + BodyLampsDwords;
inline constexpr float MapMatchM = 0.3f;
// A pixel is behind him when it is farther from the lamp than his map says by MapBiasM plus
// MapBiasTexels of the map's texel there (its own limb, seen from the lamp, is not its shadow).
inline constexpr float MapBiasM = 0.04f;
inline constexpr float MapBiasTexels = 1.5f;
// VR_BODY_MAPS=0: capsules only; =eyes: the left eye capsules, the right eye the maps (one frame,
// before and after). Tests.
inline bool MapsEnabled() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_BODY_MAPS");
        return env == nullptr || env[0] != '0';
    }();
    return on;
}
// VR_BODY_MAPS_DEBUG=1 (tests): a pixel whose ray to the lamp meets his map at any depth is dark
// (where his silhouette falls from that lamp, whatever is in front).
inline bool MapsDebug() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_BODY_MAPS_DEBUG");
        return env != nullptr && env[0] == '1';
    }();
    return on;
}
// =2: the light is dark wherever a map is matched (where the matching works); =3: dark where the
// pixel falls inside the map's frustum (in front of the lamp, within its view).
inline u32 MapsDebugMode() {
    static const u32 mode = [] {
        const char* const env = std::getenv("VR_BODY_MAPS_DEBUG");
        return env != nullptr ? u32(env[0] - '0') : 0u;
    }();
    return mode;
}
inline bool MapsRightEyeOnly() {
    static const bool eyes = [] {
        const char* const env = std::getenv("VR_BODY_MAPS");
        return env != nullptr && env[0] == 'e';
    }();
    return eyes;
}

// The ray starts this far towards the light (not the limb the pixel is on), the edge's width is
// MinEdgeM plus LightM times the receiver's distance from the blocker over the blocker's from
// the light, and a limb the pixel sits within (its radius + OwnM) does not shadow it, nor one the
// light sits within (its radius + CarryM: the torch in its hand, the lantern at its hip).
inline constexpr float StartM = 0.08f;
inline constexpr float MinEdgeM = 0.015f;
inline constexpr float LightM = 0.06f;
inline constexpr float OwnM = 0.04f;
inline constexpr float CarryM = 0.15f;
inline constexpr float Strength = 1.0f;

inline bool Enabled() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_BODY_SHADOW");
        return env == nullptr || env[0] != '0';
    }();
    return on;
}

} // namespace Shader::Optimization::BodyShadow
