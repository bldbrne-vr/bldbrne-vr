// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <span>
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

} // namespace VR::MapObjects
