// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "common/types.h"
#include "core/vr/fog_emitters.h"

// The objects a map file places (MapStudio/mAA_BB_00_SS.msb, see woods_shadows.h for its tables:
// in PARTS_PARAM_ST an entry's +0x08 i64 is its name's offset from the entry, UTF-16,
// "o270104_1401": an object's model, then its placement; +0x28 its place, 3 f32, the map's own
// world), for the lights that belong to them.
//
// The user in the headset, 2026-11-17, the Forbidden Woods: "I GET BLINKING LIGHTS ... the lantern
// just in front of me". Measured that day (VR_LIGHT_AUDIT, three runs from his spot): the hanging
// lanterns' lights are in no light file of the map (map_lights.h: one lamp within 35 m); the game
// spawns each with the lantern's flame (record word 7 = 1, radius 2.5 m, luminance 1.2..2.0), and
// lists it only in frames where it lights a surface in view: the flame 8 m off was left out of 87
// of 990 logged frames as the head swept, the one 2 m off of 1267 of 1357 once he stood a few
// steps aside, looked at or not. The lamp memory (fog_light.h) kept only the map's own lights
// through such gaps, so the glow came on at once and went in 0.12 s with every listing.
// All seven measured flames sit within 0.25 m of a hanging lantern (o270102 or o270104).
// Index just those models. A flash beside a crate must keep its normal short lifetime.
//
// Not covered: a fixture whose light sits further than OnM from its object's own place (a tall
// post with its flame at the top: its model's dummy point is not read), and a map not in the list
// the loader reads (the chalice dungeons). Those lights stay as they were.
namespace VR::MapObjects {

inline constexpr float OnM = 0.5f;       // a light is on an object this near its place
inline constexpr float CellM = 1.0f;     // (not under 2 * OnM: a point's ball meets 8 cells at most)
inline constexpr u32 MaxObjects = 65536; // of one map (the Woods' two files: 1058 each)
static_assert(CellM >= 2.0f * OnM);

// The light record stores the inverse squared radius. The two verified Woods lanterns use 2.5 m.
// Allow 10 cm either way without depending on their flickering brightness.
inline constexpr bool HasLanternReach(float reach) {
    return reach >= 1.0f / (2.6f * 2.6f) && reach <= 1.0f / (2.4f * 2.4f);
}

struct Place {
    float x, y, z;
};

// The verified lantern placements of an unpacked map file, added to `out`. False: not a map file this
// understands (nothing added).
inline bool Read(std::span<const u8> msb, std::vector<Place>& out) {
    namespace D = FogEmitters::Detail;
    std::vector<u64> parts;
    if (!D::Table(msb, "PARTS_PARAM_ST", parts)) {
        return false;
    }
    std::vector<Place> found;
    for (const u64 entry : parts) {
        s64 name;
        Place place;
        if (entry > msb.size() || !D::Read(msb, entry + 0x08, name) || name < 0 ||
            static_cast<u64>(name) > msb.size() - entry ||
            !D::Read(msb, entry + 0x28, place.x) || !D::Read(msb, entry + 0x2C, place.y) ||
            !D::Read(msb, entry + 0x30, place.z)) {
            return false;
        }
        const auto at = entry + static_cast<u64>(name);
        // Include the placement separator so a longer model number cannot match.
        constexpr auto bytes = sizeof(u"o270102_") - sizeof(char16_t);
        if (msb.size() - at >= bytes &&
            (std::memcmp(msb.data() + at, u"o270102_", bytes) == 0 ||
             std::memcmp(msb.data() + at, u"o270104_", bytes) == 0) &&
            std::isfinite(place.x) && std::isfinite(place.y) && std::isfinite(place.z)) {
            found.push_back(place);
        }
    }
    out.insert(out.end(), found.begin(), found.end());
    return true;
}

// One map's objects for "is one within OnM of this point": sorted by the cell each lies in.
class Table {
public:
    Table() = default;
    explicit Table(std::vector<Place> places) {
        if (places.size() > MaxObjects) {
            places.resize(MaxObjects);
        }
        sorted.reserve(places.size());
        for (const Place& p : places) {
            sorted.push_back({Cell(Axis(p.x), Axis(p.y), Axis(p.z)), p});
        }
        std::ranges::sort(sorted, {}, &Entry::cell);
    }
    std::size_t Size() const {
        return sorted.size();
    }
    bool On(float x, float y, float z) const {
        if (sorted.empty() || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
            return false;
        }
        const s64 x0 = Axis(x - OnM), x1 = Axis(x + OnM), y0 = Axis(y - OnM), y1 = Axis(y + OnM), z0 = Axis(z - OnM),
                  z1 = Axis(z + OnM);
        for (s64 cx = x0; cx <= x1; ++cx) {
            for (s64 cy = y0; cy <= y1; ++cy) {
                for (s64 cz = z0; cz <= z1; ++cz) {
                    const u64 cell = Cell(cx, cy, cz);
                    for (auto it = std::ranges::lower_bound(sorted, cell, {}, &Entry::cell);
                         it != sorted.end() && it->cell == cell; ++it) {
                        const float dx = it->place.x - x, dy = it->place.y - y, dz = it->place.z - z;
                        if (dx * dx + dy * dy + dz * dz <= OnM * OnM) {
                            return true;
                        }
                    }
                }
            }
        }
        return false;
    }

private:
    struct Entry {
        u64 cell;
        Place place;
    };
    // An axis' cell (the maps lie within a few km of their origins; the ends clamp).
    static s64 Axis(float v) {
        return static_cast<s64>(std::floor(std::clamp(v, -500'000.0f, 500'000.0f) / CellM));
    }
    static u64 Cell(s64 x, s64 y, s64 z) {
        constexpr s64 Bias = s64{1} << 20;
        return (static_cast<u64>(x + Bias) << 42) | (static_cast<u64>(y + Bias) << 21) | static_cast<u64>(z + Bias);
    }
    std::vector<Entry> sorted;
};

// Every map's table by its id's high half ((area << 8) | block, as game_area.h's >> 16). Filled
// once, on the main thread before the game runs (bloodborne_woods_shadows.cpp), then only read:
// Publish makes it visible, Of gives a map's table (nullptr: none, or not published yet).
inline std::vector<std::pair<u16, Table>>& All() {
    static std::vector<std::pair<u16, Table>> all;
    return all;
}
inline std::atomic<bool>& Published() {
    static std::atomic<bool> published{false};
    return published;
}
inline void Publish() {
    Published().store(true, std::memory_order_release);
}
inline const Table* Of(u16 map) {
    if (!Published().load(std::memory_order_acquire)) {
        return nullptr;
    }
    for (const auto& [id, table] : All()) {
        if (id == map) {
            return &table;
        }
    }
    return nullptr;
}

// Lights the map hangs where no flame is (2026-11-23, of the Fishing Hamlet: "random orbs
// again, that they do not match nearby candles and that are wayyy to strong", then "disable halos
// for the 'invalid' fishing hamlet lights ... unless near a candle or a flame or a candelara"). Each
// of the Hamlet's 19 placements of o366000 (in no other map file) carries a strong flickering
// light (4 m) 2.62 m over its place and 0.7 to 1.3 m to its side (VR_LIGHT_AUDIT, 16 sightings:
// the height the same to 1 cm); the flame votes found no flame where it is (0.06 to 0.19, a candle's
// flame 1.2 to 1.6), and its glow was the orb hanging beside the pole. The Hamlet's real flames are
// its fire pots (o364300: a 0.6 m light 0.67 m over each), left as they are. Such a light gets no
// glow in the air (it still lights the walls and the smoke: fog_light.h). By its object's model
// (NoHaloModels), or by hand: a map's spot (NoHaloSpots, within NoHaloSpotM).
// The Underground Corpse Pile (the Hunter's Nightmare, m34_00; the tester, 2026-11-25: an orb goes
// "only if an orb is randomly floating like we did for fishing hamlet"): each corpse heap's fire
// (o349020, eight placements, in no other map file) carries a flickering 3 m light right on the
// heap, left as it is; four of them carry a second light too, steady and deep orange (5.00 1.78
// 0.32), 5 to 5.5 m, hung 0.85 to 2.0 m over the heap and up to 1.8 m aside (VR_LIGHT_AUDIT): its
// glow was the ball floating over the heap with no flame in it.
struct NoHaloModel {
    std::u16string_view prefix; // the model and the placement's separator ("o366000_")
    float up;                   // the light's height over the placement
    float up_slack;             // ... give or take
    float side;                 // at most this far from it sideways
    float radius_lo, radius_hi; // the light's reach, m
};
inline constexpr std::array NoHaloModels{
    NoHaloModel{u"o366000_", 2.62f, 0.3f, 1.6f, 3.5f, 4.5f},
    NoHaloModel{u"o349020_", 1.42f, 0.62f, 2.0f, 4.5f, 5.75f},
};
struct NoHaloSpot {
    u16 map; // (area << 8) | block
    float x, y, z;
};
// By hand. The wall candles of Laurence's room (the Hunter's Nightmare, m34_00: the user,
// 2026-11-23, "those candles have floating orbs ... disable the candle orbs"): the game keeps each
// one's light about half a metre out from its candle into the room, and the sconces are part of the
// room's own mesh (no object of the map file), so the halo hung in the air in front of the candle.
// Eight lights, all alike (4 m, warm 2.00 1.29 0.56, steady, 1539.09 high), two rows of four along
// the side walls (VR_LIGHT_AUDIT, 2026-11-18 and 11-23). The fires by the throne keep theirs.
// The Research Hall (m35_00, 2026-11-25): a steady warm light (5 m, 2.00 1.22 0.54) hung in the
// middle of the archway past its lamp, nearly 10 m from any object: its glow floated mid-arch
// between the candles on either side. The lamp's own light keeps its glow.
inline constexpr std::array NoHaloSpots{
    NoHaloSpot{0x2200, -449.84f, 1539.09f, -277.38f}, NoHaloSpot{0x2200, -445.24f, 1539.09f, -269.51f},
    NoHaloSpot{0x2200, -440.80f, 1539.09f, -261.64f}, NoHaloSpot{0x2200, -436.59f, 1539.09f, -254.46f},
    NoHaloSpot{0x2200, -438.28f, 1539.09f, -283.89f}, NoHaloSpot{0x2200, -433.68f, 1539.09f, -276.03f},
    NoHaloSpot{0x2200, -429.24f, 1539.09f, -268.16f}, NoHaloSpot{0x2200, -425.02f, 1539.09f, -260.98f},
    NoHaloSpot{0x2300, -327.46f, 1559.85f, -824.03f},
};
inline constexpr float NoHaloSpotM = 0.5f;
// The maps a no-halo model lies in (MapFiles' area and block): their files are read. (The spots
// need no file.)
inline constexpr std::array<std::array<u8, 2>, 2> NoHaloMaps{{{34, 0}, {36, 0}}};

// A light's place where it gets no halo: a cylinder round `x y z` (`side` across, `up_slack` up
// and down), for a light whose reach (1 / radius^2) lies within [reach_lo, reach_hi].
struct NoHaloAnchor {
    float x, y, z;
    float up_slack, side;
    float reach_lo, reach_hi;
};
inline NoHaloAnchor AnchorOf(const NoHaloModel& m, const Place& at) {
    return {at.x, at.y + m.up, at.z, m.up_slack, m.side, 1.0f / (m.radius_hi * m.radius_hi),
            1.0f / (m.radius_lo * m.radius_lo)};
}
inline NoHaloAnchor AnchorOf(const NoHaloSpot& s) {
    return {s.x, s.y, s.z, NoHaloSpotM, NoHaloSpotM, 0.0f, std::numeric_limits<float>::infinity()};
}
// The anchors of the no-halo models an unpacked map file places, added to `out`. False: not a map
// file this understands (nothing added).
inline bool ReadNoHalo(std::span<const u8> msb, std::vector<NoHaloAnchor>& out) {
    namespace D = FogEmitters::Detail;
    std::vector<u64> parts;
    if (!D::Table(msb, "PARTS_PARAM_ST", parts)) {
        return false;
    }
    std::vector<NoHaloAnchor> found;
    for (const u64 entry : parts) {
        s64 name;
        Place place;
        if (entry > msb.size() || !D::Read(msb, entry + 0x08, name) || name < 0 ||
            static_cast<u64>(name) > msb.size() - entry ||
            !D::Read(msb, entry + 0x28, place.x) || !D::Read(msb, entry + 0x2C, place.y) ||
            !D::Read(msb, entry + 0x30, place.z)) {
            return false;
        }
        if (!std::isfinite(place.x) || !std::isfinite(place.y) || !std::isfinite(place.z)) {
            continue;
        }
        const auto at = entry + static_cast<u64>(name);
        for (const NoHaloModel& m : NoHaloModels) {
            const std::size_t bytes = m.prefix.size() * sizeof(char16_t);
            if (msb.size() - at >= bytes && std::memcmp(msb.data() + at, m.prefix.data(), bytes) == 0) {
                found.push_back(AnchorOf(m, place));
            }
        }
    }
    out.insert(out.end(), found.begin(), found.end());
    return true;
}
// Whether a light at `x y z` with that reach is one of them (a few a map: a walk over them).
inline bool NoHalo(std::span<const NoHaloAnchor> anchors, float x, float y, float z, float reach) {
    for (const NoHaloAnchor& a : anchors) {
        const float dx = x - a.x, dz = z - a.z;
        if (reach >= a.reach_lo && reach <= a.reach_hi && std::abs(y - a.y) <= a.up_slack &&
            dx * dx + dz * dz <= a.side * a.side) {
            return true;
        }
    }
    return false;
}
// Every map's no-halo anchors, filled with the lantern tables before the game runs and published
// with them (Publish); NoHaloOf gives a map's (nullptr: none, or not published yet).
inline std::vector<std::pair<u16, std::vector<NoHaloAnchor>>>& NoHaloAll() {
    static std::vector<std::pair<u16, std::vector<NoHaloAnchor>>> all;
    return all;
}
inline const std::vector<NoHaloAnchor>* NoHaloOf(u16 map) {
    if (!Published().load(std::memory_order_acquire)) {
        return nullptr;
    }
    for (const auto& [id, anchors] : NoHaloAll()) {
        if (id == map) {
            return &anchors;
        }
    }
    return nullptr;
}

} // namespace VR::MapObjects
