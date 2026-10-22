// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The VR tile's icon in the game's own icon atlas (header-only, no emulator dependencies; zlib is
// the caller's; tests/test_vr_tile.cpp).
//
// The Options grid's row-0 icons are 80 x 80 cells of MENU_Common_00100 (menu/common.tpf.dcx: BC1,
// 1024 x 1024, one mip level, PS4 tile mode 13), drawn by the movie as an external image. Our
// goggles go into a cell of its unused (black) area, so the grid draws them exactly like its own
// icons: the game loads the atlas as always and its Scaleform creates no texture of its own. (An
// embedded DefineBitsLossless2 bitmap did: a path no retail movie takes, and with it the GPU
// command processor's load crash came up in about 1 in 10 starts.)
namespace VR::TpfPatch {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using s32 = std::int32_t;

constexpr const char* AtlasPath = "/app0/dvdroot_ps4/menu/common.tpf.dcx";
// The retail v1.09 file (only it is patched: a mod's atlas is left as it is).
constexpr std::size_t AtlasFileSize = 9415151;
constexpr const char16_t* AtlasTexture = u"MENU_Common_00100";
// Our cell: (160, 640) px, in the atlas's unused area below the last row of icons (y >= 480 is
// black in the retail atlas: every BC1 block 00000000 aaaaaaaa).
constexpr u32 CellX = 160;
constexpr u32 CellY = 640;

// ---- DCX ("DCX\0" ... "DCS\0" unpacked, packed (big endian) ... "DCA\0" size, a zlib stream) ----

struct Dcx {
    std::size_t dcs{0};    // the "DCS\0" chunk
    std::size_t stream{0}; // the zlib stream (after "DCA\0" and its size)
    u32 unpacked{0};
    u32 packed{0};
};

inline u32 BigEndian(std::span<const u8> d, std::size_t o) {
    return (u32{d[o]} << 24) | (u32{d[o + 1]} << 16) | (u32{d[o + 2]} << 8) | u32{d[o + 3]};
}

inline std::optional<Dcx> ParseDcx(std::span<const u8> file) {
    if (file.size() < 0x50 || std::memcmp(file.data(), "DCX\0", 4) != 0) {
        return std::nullopt;
    }
    Dcx dcx;
    std::size_t dcs = 4;
    while (dcs + 12 <= 0x40 && std::memcmp(file.data() + dcs, "DCS\0", 4) != 0) {
        ++dcs;
    }
    std::size_t dca = dcs;
    while (dca + 8 <= 0x80 && dca + 8 <= file.size() &&
           std::memcmp(file.data() + dca, "DCA\0", 4) != 0) {
        ++dca;
    }
    if (dcs + 12 > 0x40 || dca + 8 > 0x80 || dca + 8 > file.size() ||
        std::memcmp(file.data() + dca, "DCA\0", 4) != 0) {
        return std::nullopt;
    }
    dcx.dcs = dcs;
    dcx.unpacked = BigEndian(file, dcs + 4);
    dcx.packed = BigEndian(file, dcs + 8);
    dcx.stream = dca + 8;
    if (dcx.unpacked == 0 || dcx.stream + dcx.packed > file.size()) {
        return std::nullopt;
    }
    return dcx;
}

// The file with a new zlib stream: the original header (its DCS sizes updated), then the stream.
inline std::vector<u8> RebuildDcx(std::span<const u8> original, const Dcx& dcx,
                                  std::span<const u8> stream, u32 unpacked) {
    std::vector<u8> out(original.begin(), original.begin() + dcx.stream);
    const auto put = [&out](std::size_t o, u32 v) {
        out[o] = static_cast<u8>(v >> 24);
        out[o + 1] = static_cast<u8>(v >> 16);
        out[o + 2] = static_cast<u8>(v >> 8);
        out[o + 3] = static_cast<u8>(v);
    };
    put(dcx.dcs + 4, unpacked);
    put(dcx.dcs + 8, static_cast<u32>(stream.size()));
    out.insert(out.end(), stream.begin(), stream.end());
    return out;
}

// ---- TPF (PS4: "TPF\0", data size, count, platform 4; entries of 0x24 bytes) --------------------

struct Texture {
    u32 offset{0};
    s32 size{0};
    u8 format{0}; // 0 / 1: BC1
    u8 mips{0};
    u16 width{0};
    u16 height{0};
    u32 tile_mode{0};
};

inline std::optional<Texture> FindTexture(std::span<const u8> tpf, std::u16string_view name) {
    if (tpf.size() < 0x10 || std::memcmp(tpf.data(), "TPF\0", 4) != 0 || tpf[0x0C] != 4) {
        return std::nullopt;
    }
    s32 count{};
    std::memcpy(&count, tpf.data() + 8, 4);
    if (count <= 0 || count > 65536 || 0x10 + static_cast<std::size_t>(count) * 0x24 > tpf.size()) {
        return std::nullopt;
    }
    for (s32 i = 0; i < count; ++i) {
        const std::size_t e = 0x10 + static_cast<std::size_t>(i) * 0x24;
        u32 name_at{};
        std::memcpy(&name_at, tpf.data() + e + 0x18, 4);
        std::size_t end = name_at;
        while (end + 1 < tpf.size() && (tpf[end] != 0 || tpf[end + 1] != 0)) {
            end += 2;
        }
        if (end + 1 >= tpf.size() || (end - name_at) / 2 != name.size()) {
            continue;
        }
        bool same = true;
        for (std::size_t c = 0; c < name.size() && same; ++c) {
            same = (tpf[name_at + 2 * c] | (tpf[name_at + 2 * c + 1] << 8)) == name[c];
        }
        if (!same) {
            continue;
        }
        Texture t;
        std::memcpy(&t.offset, tpf.data() + e, 4);
        std::memcpy(&t.size, tpf.data() + e + 4, 4);
        t.format = tpf[e + 8];
        t.mips = tpf[e + 10];
        std::memcpy(&t.width, tpf.data() + e + 12, 2);
        std::memcpy(&t.height, tpf.data() + e + 14, 2);
        std::memcpy(&t.tile_mode, tpf.data() + e + 0x14, 4);
        return t;
    }
    return std::nullopt;
}

// Byte offset of BC block (bx, by) in level 0 of a tile-mode-13 texture `blocks_wide` blocks wide:
// 8 x 8 blocks per tile in Morton order (x0 y0 x1 y1 x2 y2), tiles row by row (the inverse of
// bloodborne_assets.cpp's DetileLevel).
inline std::size_t TiledBlockOffset(u32 bx, u32 by, u32 blocks_wide, u32 block_bytes) {
    const u32 tiles_x = (blocks_wide + 7) / 8;
    const u32 tile = (by / 8) * tiles_x + bx / 8;
    const u32 lx = bx & 7, ly = by & 7;
    const u32 e = (lx & 1) | ((ly & 1) << 1) | ((lx & 2) << 1) | ((ly & 2) << 2) |
                  ((lx & 4) << 2) | ((ly & 4) << 3);
    return (static_cast<std::size_t>(tile) * 64 + e) * block_bytes;
}

// Writes `blocks` (BC1, row by row, cell_w x cell_h px) into the atlas texture at (x, y) px, in
// place. Refuses (an error, nothing written) unless the texture is the expected BC1 atlas and every
// block of the cell is black (c0 = c1 = 0: unused) now.
inline std::string WriteBc1Cell(std::span<u8> tpf, std::u16string_view texture, u32 x, u32 y,
                                u32 cell_w, u32 cell_h, std::span<const u8> blocks) {
    const auto t = FindTexture(tpf, texture);
    if (!t) {
        return "no such texture";
    }
    if (t->format > 1 || t->tile_mode != 13 || t->mips != 1 || t->width != 1024 ||
        t->height != 1024) {
        return "not the BC1 1024 x 1024 tile-mode-13 atlas";
    }
    const u32 bw = t->width / 4;
    const std::size_t level_bytes = static_cast<std::size_t>(bw) * (t->height / 4) * 8;
    if (t->size < 0 || static_cast<std::size_t>(t->size) < level_bytes ||
        static_cast<std::size_t>(t->offset) + level_bytes > tpf.size()) {
        return "texture data out of the file";
    }
    if (x % 4 != 0 || y % 4 != 0 || cell_w % 4 != 0 || cell_h % 4 != 0 || x + cell_w > t->width ||
        y + cell_h > t->height || blocks.size() != static_cast<std::size_t>(cell_w / 4) * (cell_h / 4) * 8) {
        return "cell outside the atlas or blocks of the wrong size";
    }
    u8* const base = tpf.data() + t->offset;
    for (u32 by = 0; by < cell_h / 4; ++by) {
        for (u32 bx = 0; bx < cell_w / 4; ++bx) {
            const u8* at = base + TiledBlockOffset(x / 4 + bx, y / 4 + by, bw, 8);
            if (at[0] != 0 || at[1] != 0 || at[2] != 0 || at[3] != 0) {
                return "the cell is not empty";
            }
        }
    }
    for (u32 by = 0; by < cell_h / 4; ++by) {
        for (u32 bx = 0; bx < cell_w / 4; ++bx) {
            std::memcpy(base + TiledBlockOffset(x / 4 + bx, y / 4 + by, bw, 8),
                        blocks.data() + (static_cast<std::size_t>(by) * (cell_w / 4) + bx) * 8, 8);
        }
    }
    return {};
}

// Reads a cell back (BC1 blocks, row by row) -- tests.
inline std::vector<u8> ReadBc1Cell(std::span<const u8> tpf, const Texture& t, u32 x, u32 y,
                                   u32 cell_w, u32 cell_h) {
    std::vector<u8> out;
    const u32 bw = t.width / 4;
    for (u32 by = 0; by < cell_h / 4; ++by) {
        for (u32 bx = 0; bx < cell_w / 4; ++bx) {
            const u8* at = tpf.data() + t.offset + TiledBlockOffset(x / 4 + bx, y / 4 + by, bw, 8);
            out.insert(out.end(), at, at + 8);
        }
    }
    return out;
}

} // namespace VR::TpfPatch
