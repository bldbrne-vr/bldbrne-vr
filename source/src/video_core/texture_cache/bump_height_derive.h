// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <vector>

// A height map for a bump map that has none (shader_recompiler/ir/passes/parallax.h BumpGaps; the
// user, 2026-11-12: "I was hoping the cobblestone floor would get parallax"). The game's plain
// bump materials keep the normal in red, green and alpha and never read blue; on brick walls,
// stone blocks and shingles of some materials blue is a height map (vr_bump_class.h), on most it
// is 255 everywhere: Central Yharnam's cobbled streets among them. Their relief is in the normal
// map, though: Derive integrates it into a height (Frankot-Chellappa: the least-squares surface of
// the normal's slopes, exact for a texture that tiles), reads each texel's depth below the stones'
// tops round it (the height under its neighbourhood's upper envelope, with a dead zone: a stone's
// own bumps stay its face), keeps the texture only when that depth looks like stonework (many
// box-like faces with joints between them: bricks, cobbles, tiles; not cloth, rubble, ornaments
// or atlases) and writes it into the bump map's own blocks' blue: faces 255, the deepest joints
// FloorBlue, the same form as the game's own height maps, so the same shader code reads both.
// BC1 gives a block two 5-bit blues and the indices the normal's red and green chose: the two are
// fitted (least squares) to the block's sixteen texels. Nothing of the game is stored: the result
// is made from the player's own copy while it plays (texture_cache/bump_height.h).
// No dependencies: scratch tests build this file alone.
namespace VideoCore::BumpHeight {

inline constexpr std::uint32_t MinSize = 128;  // square, a power of two, BC1, micro-tiled
inline constexpr std::uint32_t MaxSize = 1024;
inline constexpr std::uint32_t WorkSize = 512; // the height is made at this size at most
inline constexpr std::uint32_t MaxMips = 5;    // the levels that get the height
inline constexpr std::uint32_t MinMipSize = 32;
inline constexpr float FloorBlue = 65.0f;
// The shader's depth of a blue: clamp((FaceTop - blue) * scale, 0, 1), FaceTop 244 / 255.
inline constexpr float DepthScale = 255.0f / (244.0f - FloorBlue);

// One level of the guest's tiled bytes: its byte offset, its row length and row count in 4x4
// blocks as stored (padded to whole 8x8-block micro tiles).
struct Mip {
    std::uint32_t offset{};
    std::uint32_t pitch_blocks{};
    std::uint32_t height_blocks{};
    std::uint32_t bytes{};
};

struct Layout {
    std::uint32_t size{};   // texels, width and height
    std::uint32_t levels{}; // of `mips` filled (those of MinMipSize or more, MaxMips at most)
    std::array<Mip, MaxMips> mips{};
};

struct Stats {
    float relief{}; // the joints' depth in texels of the full-size texture
    float face{};   // the share of faces, of joints
    float joint{};
    std::uint32_t stones{}; // box-like faces
    float stone_share{};    // their share of all faces
};

// `bytes`: the texture's guest bytes (every level, tiled), changed in place: blue only, in the
// levels of `layout`. False (nothing changed): not stonework, or not readable as such.
bool Derive(std::uint8_t* bytes, std::uint64_t size, const Layout& layout, Stats& stats);

} // namespace VideoCore::BumpHeight
