// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-FileCopyrightText: 2014 Intel Corporation (the BPTC partition and anchor tables, as in Mesa)
// SPDX-License-Identifier: GPL-2.0-or-later AND MIT

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// A CPU decoder for BC7 (BPTC unorm) blocks, header-only: the VR settings page draws the game's
// own menu art (menu/common.tpf.dcx, BC7) into its page image on the CPU. The partition and
// anchor tables and the bit layout follow the BPTC specification (the tables as in Mesa's
// texcompress_bptc_tmp.h, MIT, Intel 2014). Unit-tested against Pillow's decoder
// (tests/test_vr_page.cpp).
namespace VR::Bc7 {

namespace detail {

struct Mode {
    int subsets;
    int partition_bits;
    bool rotation;
    bool index_selection;
    int color_bits;
    int alpha_bits;
    bool endpoint_pbits;
    bool shared_pbits;
    int index_bits;
    int secondary_index_bits;
};

inline constexpr std::array<Mode, 8> Modes{{
    {3, 4, false, false, 4, 0, true, false, 3, 0},
    {2, 6, false, false, 6, 0, false, true, 3, 0},
    {3, 6, false, false, 5, 0, false, false, 2, 0},
    {2, 6, false, false, 7, 0, true, false, 2, 0},
    {1, 0, true, true, 5, 6, false, false, 2, 3},
    {1, 0, true, false, 7, 8, false, false, 2, 2},
    {1, 0, false, false, 7, 7, true, false, 4, 0},
    {2, 6, false, false, 5, 5, true, false, 2, 0},
}};

// Two subsets: 2 bits per texel (texel 0 in the low bits).
inline constexpr std::array<std::uint32_t, 64> Partitions2{
    0x50505050U, 0x40404040U, 0x54545454U, 0x54505040U, 0x50404000U, 0x55545450U, 0x55545040U,
    0x54504000U, 0x50400000U, 0x55555450U, 0x55544000U, 0x54400000U, 0x55555440U, 0x55550000U,
    0x55555500U, 0x55000000U, 0x55150100U, 0x00004054U, 0x15010000U, 0x00405054U, 0x00004050U,
    0x15050100U, 0x05010000U, 0x40505054U, 0x00404050U, 0x05010100U, 0x14141414U, 0x05141450U,
    0x01155440U, 0x00555500U, 0x15014054U, 0x05414150U, 0x44444444U, 0x55005500U, 0x11441144U,
    0x05055050U, 0x05500550U, 0x11114444U, 0x41144114U, 0x44111144U, 0x15055054U, 0x01055040U,
    0x05041050U, 0x05455150U, 0x14414114U, 0x50050550U, 0x41411414U, 0x00141400U, 0x00041504U,
    0x00105410U, 0x10541000U, 0x04150400U, 0x50410514U, 0x41051450U, 0x05415014U, 0x14054150U,
    0x41050514U, 0x41505014U, 0x40011554U, 0x54150140U, 0x50505500U, 0x00555050U, 0x15151010U,
    0x54540404U,
};

// Three subsets.
inline constexpr std::array<std::uint32_t, 64> Partitions3{
    0xaa685050U, 0x6a5a5040U, 0x5a5a4200U, 0x5450a0a8U, 0xa5a50000U, 0xa0a05050U, 0x5555a0a0U,
    0x5a5a5050U, 0xaa550000U, 0xaa555500U, 0xaaaa5500U, 0x90909090U, 0x94949494U, 0xa4a4a4a4U,
    0xa9a59450U, 0x2a0a4250U, 0xa5945040U, 0x0a425054U, 0xa5a5a500U, 0x55a0a0a0U, 0xa8a85454U,
    0x6a6a4040U, 0xa4a45000U, 0x1a1a0500U, 0x0050a4a4U, 0xaaa59090U, 0x14696914U, 0x69691400U,
    0xa08585a0U, 0xaa821414U, 0x50a4a450U, 0x6a5a0200U, 0xa9a58000U, 0x5090a0a8U, 0xa8a09050U,
    0x24242424U, 0x00aa5500U, 0x24924924U, 0x24499224U, 0x50a50a50U, 0x500aa550U, 0xaaaa4444U,
    0x66660000U, 0xa5a0a5a0U, 0x50a050a0U, 0x69286928U, 0x44aaaa44U, 0x66666600U, 0xaa444444U,
    0x54a854a8U, 0x95809580U, 0x96969600U, 0xa85454a8U, 0x80959580U, 0xaa141414U, 0x96960000U,
    0xaaaa1414U, 0xa05050a0U, 0xa0a5a5a0U, 0x96000000U, 0x40804080U, 0xa9a8a9a8U, 0xaaaaaa44U,
    0x2a4a5254U,
};

// The second subset's anchor (two subsets); the second's and the third's (three subsets).
inline constexpr std::array<std::uint8_t, 64> Anchor2{
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8, 2,  2, 8,
    8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2, 8,  2, 2,
    2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2, 15,
};
inline constexpr std::array<std::uint8_t, 64> Anchor3a{
    3, 3, 15, 15, 8,  3, 15, 15, 8, 8, 6,  6,  6, 5,  3, 3, 3, 3,  8, 15, 3, 3,
    6, 10, 5, 8,  8,  6, 8,  5,  15, 15, 8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5,
    15, 15, 15, 15, 3, 15, 5, 5,  5, 8,  5, 10, 5, 10, 8, 13, 15, 12, 3, 3,
};
inline constexpr std::array<std::uint8_t, 64> Anchor3b{
    15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8,  15, 8, 15, 3, 15, 8,
    15, 8, 3, 15, 6, 10, 15, 15, 10, 8, 15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15,
    3,  6, 6, 8, 15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8,
};

inline int Bits(const std::uint8_t* block, int offset, int count) {
    int value = 0;
    for (int i = 0; i < count; ++i) {
        const int bit = offset + i;
        value |= ((block[bit >> 3] >> (bit & 7)) & 1) << i;
    }
    return value;
}

inline std::uint8_t Expand(int value, int bits) {
    return static_cast<std::uint8_t>((value << (8 - bits)) | (value >> (2 * bits - 8)));
}

inline int Interpolate(int a, int b, int index, int bits) {
    static constexpr std::array<int, 4> W2{0, 21, 43, 64};
    static constexpr std::array<int, 8> W3{0, 9, 18, 27, 37, 46, 55, 64};
    static constexpr std::array<int, 16> W4{0, 4, 9, 13, 17, 21, 26, 30,
                                            34, 38, 43, 47, 51, 55, 60, 64};
    const int w = bits == 2 ? W2[index & 3] : bits == 3 ? W3[index & 7] : W4[index & 15];
    return ((64 - w) * a + w * b + 32) >> 6;
}

inline bool IsAnchor(int subsets, int partition, int texel) {
    if (texel == 0) {
        return true;
    }
    if (subsets == 2) {
        return Anchor2[partition] == texel;
    }
    if (subsets == 3) {
        return Anchor3a[partition] == texel || Anchor3b[partition] == texel;
    }
    return false;
}

inline int AnchorsBefore(int subsets, int partition, int texel) {
    if (texel == 0) {
        return 0;
    }
    int count = 1;
    if (subsets == 2) {
        count += texel > Anchor2[partition] ? 1 : 0;
    } else if (subsets == 3) {
        count += texel > Anchor3a[partition] ? 1 : 0;
        count += texel > Anchor3b[partition] ? 1 : 0;
    }
    return count;
}

} // namespace detail

// Decodes one 16-byte block into 16 RGBA8 texels (row-major 4 x 4, straight alpha). A reserved
// block (mode byte 0) decodes to transparent black, as the specification says.
inline void DecodeBlock(const std::uint8_t* block, std::uint8_t out[64]) {
    using namespace detail;
    int mode_index = -1;
    for (int m = 0; m < 8; ++m) {
        if ((block[0] >> m) & 1) {
            mode_index = m;
            break;
        }
    }
    if (mode_index < 0) {
        std::memset(out, 0, 64);
        return;
    }
    const Mode& mode = Modes[mode_index];
    int at = mode_index + 1;
    const int partition = Bits(block, at, mode.partition_bits);
    at += mode.partition_bits;
    const std::uint32_t subsets_map = mode.subsets == 2   ? Partitions2[partition]
                                      : mode.subsets == 3 ? Partitions3[partition]
                                                          : 0u;
    int rotation = 0;
    if (mode.rotation) {
        rotation = Bits(block, at, 2);
        at += 2;
    }
    int selection = 0;
    if (mode.index_selection) {
        selection = Bits(block, at, 1);
        at += 1;
    }
    // Endpoints: [subset * 2 + endpoint][channel]
    int ep[6][4]{};
    for (int c = 0; c < 3; ++c) {
        for (int e = 0; e < mode.subsets * 2; ++e) {
            ep[e][c] = Bits(block, at, mode.color_bits);
            at += mode.color_bits;
        }
    }
    const int components = mode.alpha_bits > 0 ? 4 : 3;
    for (int e = 0; e < mode.subsets * 2; ++e) {
        if (mode.alpha_bits > 0) {
            ep[e][3] = Bits(block, at, mode.alpha_bits);
            at += mode.alpha_bits;
        } else {
            ep[e][3] = 255;
        }
    }
    if (mode.endpoint_pbits) {
        for (int e = 0; e < mode.subsets * 2; ++e) {
            const int p = Bits(block, at, 1);
            at += 1;
            for (int c = 0; c < components; ++c) {
                ep[e][c] = (ep[e][c] << 1) | p;
            }
        }
    } else if (mode.shared_pbits) {
        for (int s = 0; s < mode.subsets; ++s) {
            const int p = Bits(block, at, 1);
            at += 1;
            for (int e = 0; e < 2; ++e) {
                for (int c = 0; c < components; ++c) {
                    ep[s * 2 + e][c] = (ep[s * 2 + e][c] << 1) | p;
                }
            }
        }
    }
    const int pbit = (mode.endpoint_pbits || mode.shared_pbits) ? 1 : 0;
    for (int e = 0; e < mode.subsets * 2; ++e) {
        for (int c = 0; c < 3; ++c) {
            ep[e][c] = Expand(ep[e][c], mode.color_bits + pbit);
        }
        if (mode.alpha_bits > 0) {
            ep[e][3] = Expand(ep[e][3], mode.alpha_bits + pbit);
        }
    }
    const int index_start = at;
    for (int texel = 0; texel < 16; ++texel) {
        const int before = AnchorsBefore(mode.subsets, partition, texel);
        const bool anchor = IsAnchor(mode.subsets, partition, texel);
        const int primary_at = index_start + mode.index_bits * texel - before;
        const int secondary_at = index_start + 16 * mode.index_bits - mode.subsets +
                                 mode.secondary_index_bits * texel - before;
        int indices[2]{};
        indices[0] = Bits(block, primary_at, mode.index_bits - (anchor ? 1 : 0));
        if (mode.secondary_index_bits != 0) {
            indices[1] = Bits(block, secondary_at, mode.secondary_index_bits - (anchor ? 1 : 0));
        }
        const int subset = static_cast<int>((subsets_map >> (texel * 2)) & 3u);
        const int color_index = indices[selection];
        const int color_bits = selection != 0 ? mode.secondary_index_bits : mode.index_bits;
        std::uint8_t* px = out + texel * 4;
        for (int c = 0; c < 3; ++c) {
            px[c] = static_cast<std::uint8_t>(
                Interpolate(ep[subset * 2][c], ep[subset * 2 + 1][c], color_index, color_bits));
        }
        int alpha_index = indices[0];
        int alpha_bits = mode.index_bits;
        if (mode.secondary_index_bits != 0 && selection == 0) {
            alpha_index = indices[1];
            alpha_bits = mode.secondary_index_bits;
        }
        px[3] = static_cast<std::uint8_t>(
            Interpolate(ep[subset * 2][3], ep[subset * 2 + 1][3], alpha_index, alpha_bits));
        if (rotation != 0) {
            const std::uint8_t t = px[rotation - 1];
            px[rotation - 1] = px[3];
            px[3] = t;
        }
    }
}

// Decodes `blocks_x` x `blocks_y` linear blocks (row by row) into an RGBA8 image of
// (4 * blocks_x) x (4 * blocks_y) texels.
inline void DecodeImage(const std::uint8_t* blocks, int blocks_x, int blocks_y, std::uint8_t* rgba) {
    const int width = blocks_x * 4;
    std::uint8_t texels[64];
    for (int by = 0; by < blocks_y; ++by) {
        for (int bx = 0; bx < blocks_x; ++bx) {
            DecodeBlock(blocks + (static_cast<std::size_t>(by) * blocks_x + bx) * 16, texels);
            for (int y = 0; y < 4; ++y) {
                std::memcpy(rgba + ((static_cast<std::size_t>(by) * 4 + y) * width + bx * 4) * 4,
                            texels + y * 16, 16);
            }
        }
    }
}

} // namespace VR::Bc7
