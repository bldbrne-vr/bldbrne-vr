// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.h"

// The game's lingering fog, taken over by the fog grid (2026-11-16: "what if we replace
// all the game's fog billboards with our fog ... the patches of lingering steam/fog"; "detecting
// the fog emitter might be cleaner"; "keep the game's smoke, we only target the fog that lingers
// over the floor"; only "if it looks really good and ends up being FASTER").
//
// A map file (MapStudio/mAA_BB_00_0x.msb, see woods_shadows.h for its tables) lists every effect
// the map starts, by name, in EVENT_PARAM_ST: an entry's +0x0C i32 is its type (2: an effect),
// +0x18 i64 its base data (from the entry: i32 part, i32 region, i32 entity), +0x20 i64 its type
// data (i32 the effect's id). The region is an entry of POINT_PARAM_ST: +0x14 its place (3 f32).
// Central Yharnam's file names them: 床の霧 "floor fog" (ids 924300, 924301, 924303; 69 of
// them), 遠景霧 "distant-view fog" (924305..924308; 83), 室内霧 "indoor fog" (924310; 21),
// ボス部屋床の霧 "boss room floor fog" (924309); and, never touched: 霧壁 "fog wall" (8241xx:
// boss and multiplayer gates), fires' smoke, torches, falling ash, flies, drips, dust.
//
// Measured before any of this was built (2026-11-16, the fence across from Central Yharnam's
// ladder, the user's settings): the sprite pass that draws them, 19..21 draws, 0.37..0.38 ms of a
// 7.0..7.1 ms frame.
//
// (The ids are Central Yharnam's; every map has its own, so the names decide: KindOfName.)
// Find lists the effects of the kinds asked for with their places; Silence makes the map start
// none of them (their effect id becomes NoEffect). Pure: bytes in, bytes changed.
namespace VR::FogEmitters {

enum class Kind : u8 { Floor, Far, Indoor, BossFloor, Plain };

struct Emitter {
    Kind kind;
    u32 effect;
    float x, y, z;
};

// (0: what the game's own authors put where they switched one off: the Nightmare Frontier's map
// has "霧C02(一旦OFF)", "fog C02 (off for now)", with effect id 0.)
inline constexpr s32 NoEffect = 0;

// How heavy and how wide an effect's fog is against its map's plain floor fog, where its own file
// says (sfx/frpg_sfxbnd_mAA_BB.ffxbnd.dcx, the effect's FXR; read by hand 2026-11-16, not at run
// time: the files' layouts differ from map to map). Central Yharnam's 924301..924304 are one file
// with nine numbers changed: 924302 half the strength; 924303 a puff every 0.28 s and 30 alive at
// once, not every 0.55 s and 15; 924304 every 0.22 s and 40 alive, born over 20 x 50 and not
// 11 x 35. An effect not listed: 1 and 1.
struct Weight {
    float density;
    float size;
};
inline constexpr Weight WeightOf(u32 effect) {
    switch (effect) {
    case 924302:
        return {0.55f, 1.0f};
    case 924303:
        return {1.35f, 1.0f};
    case 924304:
        return {1.5f, 1.45f};
    default:
        return {1.0f, 1.0f};
    }
}

// The kind of an effect by its name in the map file (UTF-16; false: not a lingering fog). Every
// map names them (2026-11-16, all sixteen maps' files read): 床の霧 / 地面霧 / 足元霧 / 上層地面霧 /
// 下層地面霧 "floor / ground / foot / upper / lower ground fog", 遠景霧 "distant-view fog" (and
// 階層霧 "tier fog", 空中霧 "mid-air fog"), 室内霧 "indoor fog", ボス部屋床の霧 "boss room floor
// fog", and plain 霧 "fog" in the maps that name no kind (Plain). Never: 霧壁 / 霧の壁 "fog wall"
// (boss and multiplayer gates), anything with 煙 "smoke" (chimneys, snow smoke: the user: "the
// game's smoke... keep that one"), 蒸 "steam", 雨 "rain", 出現 "appearing" (an enemy's entrance),
// 中ボス "mid boss", 光 "light", OFF.
inline bool KindOfName(std::u16string_view name, Kind& kind) {
    const auto has = [&](std::u16string_view part) { return name.find(part) != std::u16string_view::npos; };
    if (!has(u"\u9727")) {
        return false;
    }
    for (const std::u16string_view never : {u"\u9727\u58C1", u"\u9727\u306E\u58C1", u"\u7159", u"\u84B8", u"\u96E8", u"\u51FA\u73FE", u"\u4E2D\u30DC\u30B9", u"\u5149", u"OFF"}) {
        if (has(never)) {
            return false;
        }
    }
    if (has(u"\u9060\u666F\u9727") || has(u"\u968E\u5C64\u9727") || has(u"\u7A7A\u4E2D\u9727")) {
        kind = Kind::Far;
    } else if (has(u"\u5BA4\u5185\u9727")) {
        kind = Kind::Indoor;
    } else if (has(u"\u30DC\u30B9\u90E8\u5C4B")) {
        kind = Kind::BossFloor;
    } else if (has(u"\u5E8A\u306E\u9727") || has(u"\u5730\u9762\u9727") || has(u"\u8DB3\u5143\u9727")) {
        kind = Kind::Floor;
    } else {
        kind = Kind::Plain;
    }
    return true;
}

namespace Detail {

template <typename T>
inline bool Read(std::span<const u8> b, u64 at, T& out) {
    if (at > b.size() || b.size() - at < sizeof(T)) {
        return false;
    }
    std::memcpy(&out, b.data() + at, sizeof(T));
    return true;
}

// A table's entries by its name (UTF-16, ASCII letters), walking the chain from 0x10.
inline bool Table(std::span<const u8> msb, std::string_view wanted, std::vector<u64>& entries) {
    if (msb.size() < 0x20 || std::memcmp(msb.data(), "MSB ", 4) != 0) {
        return false;
    }
    u64 at = 0x10;
    for (u32 guard = 0; guard < 16 && at != 0; ++guard) {
        s32 version, count;
        s64 name;
        if (!Read(msb, at, version) || !Read(msb, at + 4, count) || !Read(msb, at + 8, name) || count < 1 ||
            count > 1'000'000) {
            return false;
        }
        bool match = true;
        for (std::size_t i = 0; i <= wanted.size(); ++i) {
            u16 c;
            if (!Read(msb, static_cast<u64>(name) + 2 * i, c)) {
                return false;
            }
            match = match && c == (i < wanted.size() ? static_cast<u16>(wanted[i]) : u16{0});
            if (!match) {
                break;
            }
        }
        if (match) {
            entries.resize(static_cast<std::size_t>(count - 1));
            for (s32 i = 0; i < count - 1; ++i) {
                if (!Read(msb, at + 16 + 8 * static_cast<u64>(i), entries[i])) {
                    return false;
                }
            }
            return true;
        }
        s64 next;
        if (!Read(msb, at + 16 + 8 * static_cast<u64>(count - 1), next)) {
            return false;
        }
        at = static_cast<u64>(next);
    }
    return false;
}

} // namespace Detail

struct Found {
    Emitter emitter;
    u64 effect_at; // where its id is in the file
};

// A place whose fog stays the game's own, whatever the kinds taken over (2026-11-20:
// "in the blood starved beast arena can we disable our fog patches and let the game use its
// default?"): the church the Blood-starved Beast waits in, Old Yharnam (m23_00, map 0x1700). Its
// gate is at (-87.7, -126.4, 13.5) and its torches run from x -85 to -120, z 10 to 42; inside:
// "SFX_霧_ボス部屋" ("fog, boss room", 923012, at (-115.5, -126.2, 36.9)), the one we took, and
// two distant-view fogs under its roof. The box holds no other fog of the map (the nearest
// outside it: a lower ground fog 12 m from the gate at x -72). `map`: its area and block
// (0x1700 = m23_00).
inline bool GamesOwn(u16 map, const Emitter& e) {
    return map == 0x1700 && e.x > -125.0f && e.x < -86.0f && e.y > -131.0f && e.y < -95.0f && e.z > 4.0f && e.z < 49.0f;
}

// The lingering fog effects of the map with their places. False: not a map file it understands.
inline bool Find(std::span<const u8> msb, std::vector<Found>& out) {
    std::vector<u64> events, points;
    if (!Detail::Table(msb, "EVENT_PARAM_ST", events) || !Detail::Table(msb, "POINT_PARAM_ST", points)) {
        return false;
    }
    for (const u64 e : events) {
        s32 type;
        s64 base, data;
        if (!Detail::Read(msb, e + 0x0C, type) || !Detail::Read(msb, e + 0x18, base) ||
            !Detail::Read(msb, e + 0x20, data)) {
            return false;
        }
        if (type != 2 || base <= 0 || data <= 0) {
            continue;
        }
        s32 region, effect;
        s64 name_at;
        if (!Detail::Read(msb, e + static_cast<u64>(base) + 4, region) ||
            !Detail::Read(msb, e + static_cast<u64>(data), effect) || !Detail::Read(msb, e, name_at)) {
            return false;
        }
        std::u16string name;
        for (u64 at = e + static_cast<u64>(name_at); name.size() < 96; at += 2) {
            char16_t c;
            if (!Detail::Read(msb, at, c)) {
                return false;
            }
            if (c == 0) {
                break;
            }
            name.push_back(c);
        }
        Kind kind;
        if (effect <= 0 || !KindOfName(name, kind) || region < 0 ||
            static_cast<std::size_t>(region) >= points.size()) {
            continue;
        }
        float place[3];
        if (!Detail::Read(msb, points[region] + 0x14, place)) {
            return false;
        }
        if (!(place[0] == place[0] && place[1] == place[1] && place[2] == place[2])) {
            continue;
        }
        out.push_back({Emitter{kind, static_cast<u32>(effect), place[0], place[1], place[2]}, e + static_cast<u64>(data)});
    }
    return true;
}

// The map starts none of `found` whose kind's bit is in `kinds` (1 << Kind). How many.
inline u32 Silence(std::span<u8> msb, std::span<const Found> found, u32 kinds) {
    u32 count = 0;
    for (const Found& f : found) {
        if ((kinds >> static_cast<u32>(f.emitter.kind) & 1u) == 0 || f.effect_at + sizeof(s32) > msb.size()) {
            continue;
        }
        std::memcpy(msb.data() + f.effect_at, &NoEffect, sizeof(s32));
        ++count;
    }
    return count;
}

} // namespace VR::FogEmitters
