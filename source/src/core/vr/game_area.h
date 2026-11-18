// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>

#include "common/types.h"

// The game's own id of the map the hunter stands in, for looks that belong to one area (the ground
// mist: shader_recompiler/ir/passes/air_light.h Mist::AreaShare). Bloodborne 1.09 keeps it in the
// player's character at +0x3F8 (and in the game manager at +0x15E0): (area << 24) | (block << 16),
// the map folder's numbers, m27_00 = 0x1B000000. Read in five places (2026-11-14, a test scan):
// Forbidden Woods 0x1B000000, Central Yharnam 0x18010000, Cathedral Ward 0x18000000, Old Yharnam
// 0x17000000, Hunter's Dream 0x15000000. The game thread publishes it once a frame
// (bloodborne_body.cpp); 0: not known (no hunter yet, or a value that is no map id).
namespace VR::GameArea {

inline constexpr u64 PlayerMapOffset = 0x3F8;

inline std::atomic<u32>& Slot() {
    static std::atomic<u32> map{0};
    return map;
}

// A map id as the game writes one: an area 1..99, a small block, nothing below.
inline constexpr bool IsMapId(u32 map) {
    return (map & 0xFFFFu) == 0 && (map >> 24) >= 1 && (map >> 24) <= 99 && ((map >> 16) & 0xFFu) <= 9;
}

inline void Publish(u32 map) {
    Slot().store(IsMapId(map) ? map : 0u, std::memory_order_relaxed);
}

// Where the hunter stands (his feet; the game's world), as the game's loop last had it, and when
// (steady clock ms; -1: never): for what must be his place and not the camera's, in third person
// too (the fog's push round his body, fog_patches.h Wake).
inline std::array<std::atomic<float>, 3>& HunterPlace() {
    static std::array<std::atomic<float>, 3> place{};
    return place;
}
inline std::atomic<s64>& HunterWhenMs() {
    static std::atomic<s64> when{-1};
    return when;
}
inline s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
inline void PublishHunter(float x, float y, float z) {
    auto& place = HunterPlace();
    place[0].store(x, std::memory_order_relaxed);
    place[1].store(y, std::memory_order_relaxed);
    place[2].store(z, std::memory_order_relaxed);
    HunterWhenMs().store(NowMs(), std::memory_order_release);
}
// His place when it is at most `max_age_ms` old.
inline bool Hunter(std::array<float, 3>& out, s64 max_age_ms) {
    const s64 when = HunterWhenMs().load(std::memory_order_acquire);
    if (when < 0 || NowMs() - when > max_age_ms) {
        return false;
    }
    const auto& place = HunterPlace();
    out = {place[0].load(std::memory_order_relaxed), place[1].load(std::memory_order_relaxed),
           place[2].load(std::memory_order_relaxed)};
    return true;
}

// The strange moon (the blood moon after Rom): the game's event flag 9802, read every
// StrangeMoonEvery-th frame of the game's loop (bloodborne_frame_rate.cpp, by the cutscene limit's
// fault-safe reader); false while it is not known. And the maps whose own time-of-day event gives
// that flag a sky of its own (their events' part switches, read from the game's scripts
// 2026-11-17: Cathedral Ward 12400300, Central Yharnam 12410310, Upper Cathedral Ward 12420400,
// Yahar'gul 12800400, the old workshop 12110400). Old Yharnam (12300300) and Hemwick (12200300)
// show their night sky under it; the Hunter's Dream and the Forbidden Woods switch no sky.
inline constexpr u32 StrangeMoonFlag = 9802;
inline constexpr u32 StrangeMoonEvery = 64;
inline std::atomic<bool>& StrangeMoonSlot() {
    static std::atomic<bool> up{false};
    return up;
}
inline void PublishStrangeMoon(bool up) {
    StrangeMoonSlot().store(up, std::memory_order_relaxed);
}
inline constexpr bool BloodMoonSky(u32 map) {
    const u32 area = map >> 24, block = (map >> 16) & 0xFFu;
    return IsMapId(map) && (area == 24 ? block <= 2 : area == 28 ? block == 0 : area == 21 && block == 1);
}
// The blood moon stands in the sky where the hunter is.
inline bool BloodMoon() {
    return StrangeMoonSlot().load(std::memory_order_relaxed) && BloodMoonSky(Slot().load(std::memory_order_relaxed));
}
// When the flag is to be read (one caller, one thread): only in a map whose sky it can change
// (elsewhere BloodMoon is false whatever the flag says, and nothing is asked of the game), at once
// in the tick he comes into such a map (from another map, or from none), then every
// StrangeMoonEvery-th tick while he stays.
struct StrangeMoonPoll {
    u32 last_map{};
    u32 ticks{};
    constexpr bool Due(u32 map) {
        const bool entered = map != last_map;
        last_map = map;
        if (!BloodMoonSky(map)) {
            return false;
        }
        if (entered) {
            ticks = 0;
            return true;
        }
        return ++ticks % StrangeMoonEvery == 0;
    }
};

// The area's number (27: the Forbidden Woods); 0: not known.
inline u32 Area() {
    return Slot().load(std::memory_order_relaxed) >> 24;
}

} // namespace VR::GameArea
