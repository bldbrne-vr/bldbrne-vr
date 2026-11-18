// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstring>

#include "common/types.h"
#include "shader_recompiler/ir/passes/parallax.h"
#include "video_core/texture_cache/bump_height_derive.h"

// The bump maps' blue (shader_recompiler/ir/passes/parallax.h BumpGaps; 2026-11-12, the user: "We
// could even do a very quick look up table?"). A plain bump material's normal is in red, green and
// alpha; its blue is, by texture (the raw bytes of Central and Old Yharnam's 504 bump maps,
// scratch pfix/study.py, each looked at): 255 everywhere (474: cobbles, clothes, props); a real
// height map (9: brick walls, big stone blocks, roof shingles: white faces, soft grey joints); a
// cut-out mask (11: black on white, hard edges); a constant grey (3); each floor tile its own grey
// (2: no joints); a stone slab's grain (1); cloth and props in mid greys (4). Only the height maps
// may move: until 2026-11-12 the masks with little black, the tiles and the grain passed as
// height maps too and their colour was dragged about by them (the tester: "it is changing how
// textures are applied and looks wrong").
// Classify reads the blue of up to SampleBlocks of the texture's own BC blocks (BC1, or BC3's
// colour block) spread over its first level (tiling only moves whole blocks). A height map has
// most of its texels at the faces' white (FaceLevel or above), a real share below JointBelow, and
// those joints grey, not black (a mask's are black). Its depth is
// clamp((FaceTop - blue) * DepthScale, 0, 1): 0 on every face (they stay on the wall), 1 at a
// blue of 65 or less.
namespace Vulkan::VrBump {

struct Class {
    float scale{0.0f}; // 0: not a height map
    u8 kind{0};        // 0 unknown, 1 flat, 2 height, 3 mask, 4 other
    // The shares of faces, joints and black texels, and the joints' floor (logs).
    float face{}, joint{}, black{}, floor{};
};

inline constexpr u32 SampleBlocks = 512;
// A 5-bit blue is 247 or 255 at the top; BC's in-between colours of the two are 250 and 252.
inline constexpr u32 FaceLevel = 244;
inline constexpr u32 JointBelow = 232;
inline constexpr u32 BlackBelow = 24;
// (The nine height maps: faces 0.68 to 0.81, joints 0.17 to 0.25, black at most 0.05 of the
// joints. The masks: black 0.5 or more of what is not white. Everything else: faces under 0.15.)
inline constexpr float FlatFace = 0.985f;
inline constexpr float MinFace = 0.5f;
inline constexpr float MinJoint = 0.10f;
inline constexpr float MaxBlackOfJoint = 0.2f;
inline constexpr float FloorShare = 0.02f; // the joints' floor: the blue's 2nd percentile

// `data` holds `bytes` of BC blocks (`block_bytes` each, the colour block at `colour_offset`).
inline Class Classify(const u8* data, u64 bytes, u32 block_bytes, u32 colour_offset) {
    Class c{};
    const u64 blocks = bytes / block_bytes;
    if (data == nullptr || blocks == 0) {
        return c;
    }
    std::array<u32, 256> histogram{};
    const u64 step = std::max<u64>(1, blocks / SampleBlocks);
    u32 texels = 0;
    for (u64 b = 0; b < blocks && texels < SampleBlocks * 16; b += step) {
        const u8* const block = data + b * block_bytes + colour_offset;
        u16 c0, c1;
        u32 indices;
        std::memcpy(&c0, block, 2);
        std::memcpy(&c1, block + 2, 2);
        std::memcpy(&indices, block + 4, 4);
        const auto blue = [](u16 v) {
            const u32 b5 = v & 31u;
            return (b5 << 3) | (b5 >> 2);
        };
        const u32 b0 = blue(c0), b1 = blue(c1);
        // BC3's colour block is always the four-colour mode; BC1's when c0 > c1.
        const bool four = colour_offset != 0 || c0 > c1;
        const std::array<u32, 4> palette{b0, b1, four ? (2 * b0 + b1) / 3 : (b0 + b1) / 2,
                                         four ? (b0 + 2 * b1) / 3 : 0u};
        for (u32 t = 0; t < 16; ++t) {
            ++histogram[palette[(indices >> (2 * t)) & 3u]];
            ++texels;
        }
    }
    if (texels == 0) {
        return c;
    }
    const auto share = [&](u32 lo, u32 hi) {
        u32 n = 0;
        for (u32 v = lo; v <= hi; ++v) {
            n += histogram[v];
        }
        return static_cast<float>(n) / static_cast<float>(texels);
    };
    c.face = share(FaceLevel, 255);
    c.joint = share(0, JointBelow - 1);
    c.black = share(0, BlackBelow - 1);
    if (c.face >= FlatFace) {
        c.kind = 1; // flat
        return c;
    }
    const u32 want = static_cast<u32>(FloorShare * static_cast<float>(texels));
    u32 floor = 255, seen = 0;
    for (u32 v = 0; v < 256; ++v) {
        seen += histogram[v];
        if (seen > want) {
            floor = v;
            break;
        }
    }
    c.floor = static_cast<float>(floor) / 255.0f;
    if (c.joint > 0.0f && c.black >= 0.5f * c.joint) {
        c.kind = 3; // a cut-out mask
        return c;
    }
    const float reach = Shader::Optimization::BumpGaps::FaceTop - c.floor;
    if (c.face < MinFace || c.joint < MinJoint || c.black > MaxBlackOfJoint * c.joint ||
        reach <= 0.0f) {
        c.kind = 4; // greys, tiles, grain, a mask's soft edge: not ours to move
        return c;
    }
    c.kind = 2;
    // One scale for every height map (the shader's constant: bump_height_derive.h DepthScale;
    // until 2026-11-12 each map's own, 1 to 1.5, pushed per draw: every other draw then pushed
    // its whole block again).
    c.scale = VideoCore::BumpHeight::DepthScale;
    return c;
}

inline const char* KindName(u8 kind) {
    switch (kind) {
    case 1: return "flat";
    case 2: return "height";
    case 3: return "cut-out mask";
    case 4: return "not a height map";
    default: return "unknown";
    }
}

} // namespace Vulkan::VrBump
