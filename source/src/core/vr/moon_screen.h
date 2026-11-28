// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <bit>
#include <chrono>

#include "common/types.h"

// The visible moon (or sun) on the flat screen: the centre of the game's own light shafts (FS
// 0xd506a1, published by native_stereo.cpp ScreenPointBuffer as the flat camera's 0..1 screen
// point). The god rays' glow aims at it (vk_rasterizer_moon.cpp VrMoonUpload): the game's shadow
// light is not where its moon is drawn (BBFSS measured it about 25 degrees above the sun's disk in
// Central Yharnam; 2026-11-12, the user: "it might explain why our god rays were mistargeted").
namespace VR::MoonScreen {

inline std::atomic<u64> g_point{0}; // u (low) and v (high) as float bits
inline std::atomic<s64> g_when_ms{-1};

inline s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline void Publish(float u, float v) {
    g_point.store(u64{std::bit_cast<u32>(u)} | (u64{std::bit_cast<u32>(v)} << 32),
                  std::memory_order_relaxed);
    g_when_ms.store(NowMs(), std::memory_order_release);
}

// The newest point when it is at most `max_age_ms` old.
inline bool Take(float& u, float& v, s64 max_age_ms) {
    const s64 when = g_when_ms.load(std::memory_order_acquire);
    if (when < 0 || NowMs() - when > max_age_ms) {
        return false;
    }
    const u64 bits = g_point.load(std::memory_order_relaxed);
    u = std::bit_cast<float>(static_cast<u32>(bits));
    v = std::bit_cast<float>(static_cast<u32>(bits >> 32));
    return true;
}

// When the game last drew its own moon rays (its light shafts, FS 0xd506a1, or its glow god rays,
// FS 0xe55e45eb: the blood moon after Rom, the Hunter's Dream's moon); -1: never. The fog grid's
// moonlit thin air steps back while they draw (fog_grid.h GameRaysShare).
inline std::atomic<s64> g_rays_ms{-1};
inline constexpr bool IsGameRaysProgram(u64 hash) {
    return hash == 0xe55e45eb || hash == 0xd506a1;
}

inline void GameRaysDrawn() {
    g_rays_ms.store(NowMs(), std::memory_order_release);
}

} // namespace VR::MoonScreen
