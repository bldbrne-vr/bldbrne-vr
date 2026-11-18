// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstring>
#include <iterator>
#include <span>

#include "common/types.h"

// Moon shadows in the Forbidden Woods (2026-11-14: "we need more shadows in the forbidden
// woods.. not sure why the moon is not casting any shadows there"; "yes the trees need to throw
// shadows"). The game draws into its moon shadow atlas only the map parts whose placement carries
// the shadow-source bit, and the woods are authored without it: of 2871 map pieces 8 have it, none
// of the 1300 standing trees, none of the 195 rock blocks (Central Yharnam: 919 of its 1692
// objects). A test with strong moonlit air showed it: buildings cut the air in Yharnam, nothing
// did in the woods. And the woods' shadows fade out from 5 m to 15 m.
//
// So the woods' map file (MapStudio/m27_00_00_0x.msb, inside its DCX) is served with that bit set
// on its trees', fallen trees' and rocks' placements (Models below: 226 models, 1568 placements a
// file), and its draw parameters (param/drawparam/m27_00_0000.gparam) with the shadows' far fade
// from FadeStartM over FadeDistM instead of 5 m over 10 m. Nothing on disk changes
// (core/vr/bloodborne_woods_shadows.cpp, the file override).
//
// The MSB (64-bit offsets): "MSB " then a chain of tables from 0x10: i32 version, i32 count, i64
// name offset (UTF-16), count - 1 i64 entry offsets, i64 next table. In PARTS_PARAM_ST an entry
// has: +0x08 i64 its name's offset (from the entry; "m009001_1000": the model, then the
// placement), +0x14 u32 type (0: a map piece), +0xB0 i64 its entity data's offset (from the
// entry), whose byte +0x0E holds bit 0 point-light shadow source, bit 1 shadow source, bit 2
// shadow destination, bit 3 shadow only, ... (the names in this order are in the game's
// executable; every placement of both maps was checked against this layout, 2026-11-14).
namespace VR::WoodsShadows {

inline constexpr const char* MsbPaths[] = {
    "/app0/dvdroot_ps4/map/MapStudio/m27_00_00_00.msb.dcx",
    "/app0/dvdroot_ps4/map/MapStudio/m27_00_00_01.msb.dcx",
};
// The draw parameters, loose and inside their binder (the same bytes at the same place from the
// file's start in the first, from the binder's first entry in the second).
inline constexpr const char* GparamPath = "/app0/dvdroot_ps4/param/drawparam/m27_00_0000.gparam.dcx";
inline constexpr const char* GparamBinderPath =
    "/app0/dvdroot_ps4/param/drawparam/m27_00_0000.gparambnd.dcx";

inline constexpr u8 ShadowSource = 0x02;
// The woods' trees, fallen trees and roots, and rock blocks: the model numbers (m%06u), sorted.
inline constexpr u32 Models[] = {
    1020, 1021, 1022, 1120, 1121, 1122, 1220, 1221, 1222, 1310, 1320, 1321, 1322, 1362,
    1382, 1420, 1421, 1422, 1490, 1520, 1521, 1522, 1620, 1621, 1622, 1720, 1721, 1722,
    1820, 1821, 1822, 1920, 1921, 1922, 1995, 2020, 2021, 2022, 2023, 2120, 2121, 2122,
    2220, 2221, 2222, 2320, 2321, 2322, 2323, 2375, 2420, 2421, 2422, 2423, 2475, 2520,
    2521, 2522, 2523, 2575, 2620, 2621, 2622, 4020, 4021, 4022, 4061, 4081, 4120, 4121,
    4122, 4141, 4220, 4221, 4222, 4320, 4321, 4322, 4420, 4421, 4422, 4520, 4521, 4522,
    4523, 4620, 4621, 4622, 4623, 4690, 4720, 4721, 4722, 4820, 4821, 4822, 4920, 4921,
    4922, 4941, 5020, 5021, 5022, 5041, 5090, 5094, 5095, 5099, 5120, 5121, 5122, 5141,
    5220, 5221, 5241, 5320, 5321, 5420, 5421, 5422, 5441, 5520, 5521, 5522, 5620, 5621,
    5622, 5641, 5690, 5720, 5721, 5722, 5723, 5741, 5780, 5820, 5821, 5822, 5841, 5920,
    5921, 5922, 6020, 6021, 6022, 6041, 6120, 6121, 6122, 6141, 6220, 6221, 6222, 6241,
    6320, 6321, 6322, 6380, 6420, 6421, 6422, 6459, 6480, 6481, 6520, 6521, 6522, 6541,
    6580, 6620, 6621, 6622, 6641, 6720, 6721, 6722, 6741, 6820, 6821, 6822, 6920, 6921,
    6922, 6941, 7020, 7021, 7022, 7041, 7120, 7121, 7122, 7123, 7124, 7125, 7180, 7500,
    7520, 7521, 7522, 7525, 8020, 8021, 8022, 8520, 8521, 8522, 8620, 8621, 8622, 8624,
    8683, 8720, 8721, 8722, 8900, 9001, 9002, 9003, 9004, 9005, 9006, 9007, 9012, 9204,
    56080, 80010,
};

struct Result {
    bool understood{false};
    u32 parts{0};   // map pieces seen
    u32 changed{0}; // placements that got the bit
};

namespace Detail {
template <typename T>
inline bool Get(std::span<const u8> d, u64 at, T& out) {
    if (at > d.size() || d.size() - at < sizeof(T)) {
        return false;
    }
    std::memcpy(&out, d.data() + at, sizeof(T));
    return true;
}
// A UTF-16 name equal to an ASCII one.
inline bool NameIs(std::span<const u8> d, u64 at, const char* ascii) {
    for (;; ++ascii, at += 2) {
        u16 c{};
        if (!Get(d, at, c) || c != static_cast<u8>(*ascii)) {
            return false;
        }
        if (c == 0) {
            return true;
        }
    }
}
// "m001234..." -> 1234; ~0u: not a map piece's name.
inline u32 ModelOf(std::span<const u8> d, u64 at) {
    u16 c{};
    if (!Get(d, at, c) || c != u'm') {
        return ~0u;
    }
    u32 n = 0, digits = 0;
    for (at += 2; digits < 9; at += 2, ++digits) {
        if (!Get(d, at, c) || c < u'0' || c > u'9') {
            break;
        }
        n = n * 10 + (c - u'0');
    }
    return digits == 6 ? n : ~0u;
}
} // namespace Detail

// Sets the shadow-source bit on the listed models' placements of an unpacked MSB.
inline Result PatchMsb(std::span<u8> msb) {
    using namespace Detail;
    Result result;
    if (msb.size() < 0x40 || std::memcmp(msb.data(), "MSB ", 4) != 0) {
        return result;
    }
    u64 table = 0x10;
    for (u32 guard = 0; guard < 32 && table != 0; ++guard) {
        s32 version{}, count{};
        s64 name{};
        if (!Get<s32>(msb, table, version) || !Get<s32>(msb, table + 4, count) ||
            !Get<s64>(msb, table + 8, name) || count < 1 || count > (1 << 20) || name < 0) {
            return result;
        }
        const u64 entries = table + 16;
        s64 next{};
        if (!Get<s64>(msb, entries + 8ull * static_cast<u32>(count - 1), next) || next < 0) {
            return result;
        }
        if (NameIs(msb, static_cast<u64>(name), "PARTS_PARAM_ST")) {
            for (u32 i = 0; i + 1 < static_cast<u32>(count); ++i) {
                s64 entry{}, name_at{}, entity{};
                u32 type{};
                if (!Get<s64>(msb, entries + 8ull * i, entry) || entry < 0 ||
                    !Get<s64>(msb, static_cast<u64>(entry) + 0x08, name_at) ||
                    !Get<u32>(msb, static_cast<u64>(entry) + 0x14, type) ||
                    !Get<s64>(msb, static_cast<u64>(entry) + 0xB0, entity) || name_at < 0 || entity <= 0) {
                    return Result{};
                }
                if (type != 0) {
                    continue;
                }
                ++result.parts;
                const u32 model = ModelOf(msb, static_cast<u64>(entry + name_at));
                if (model == ~0u || !std::binary_search(std::begin(Models), std::end(Models), model)) {
                    continue;
                }
                const u64 flag = static_cast<u64>(entry + entity) + 0x0E;
                if (flag >= msb.size()) {
                    return Result{};
                }
                if ((msb[flag] & ShadowSource) == 0) {
                    msb[flag] = static_cast<u8>(msb[flag] | ShadowSource);
                    ++result.changed;
                }
            }
            result.understood = true;
            return result;
        }
        table = static_cast<u64>(next);
    }
    return result;
}

// The shadows' reach in the woods' draw parameters (ShadowParam row 0): the far fade's start and
// length, floats at FadeAt and FadeAt + 4 from the gparam's start, followed by the three cascade
// distances (2, 8, 15 m: kept, so the near shadows stay as sharp; the fourth cascade takes the
// rest). Only where the game's own numbers stand there (5, 10, 2, 8, 15).
inline constexpr u64 FadeAt = 0xB3A8;
inline constexpr float FadeStartM = 28.0f;
inline constexpr float FadeDistM = 14.0f;
inline bool PatchGparam(std::span<u8> data, u64 base) {
    const float game[5] = {5.0f, 10.0f, 2.0f, 8.0f, 15.0f};
    for (u32 i = 0; i < 5; ++i) {
        float v{};
        if (!Detail::Get<float>(data, base + FadeAt + 4ull * i, v) || v != game[i]) {
            return false;
        }
    }
    std::memcpy(data.data() + base + FadeAt, &FadeStartM, sizeof(float));
    std::memcpy(data.data() + base + FadeAt + 4, &FadeDistM, sizeof(float));
    return true;
}
// The binder's first entry's data (BND4: its offset is the u32 at 0x58).
inline bool PatchGparamBinder(std::span<u8> binder) {
    u32 base{};
    if (binder.size() < 0x60 || std::memcmp(binder.data(), "BND4", 4) != 0 ||
        !Detail::Get<u32>(binder, 0x58, base)) {
        return false;
    }
    return PatchGparam(binder, base);
}

} // namespace VR::WoodsShadows
