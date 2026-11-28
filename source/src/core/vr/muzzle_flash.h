// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <mutex>

#include "common/types.h"

// The muzzle flash's trigger (shader_recompiler/ir/passes/fog_light.h Muzzle*; the user,
// 2026-11-11: "I have a fear it is wrong and the light isn't applied properly"). The flash was lit
// when the game's own shot light first showed in the frame's light lists near the eye; in the
// Grand Cathedral its hundreds of candles fill the lists' first CaptureLights before the shot's
// light, so no flash. Now the gun (bloodborne_gun.cpp, every shot of a pistol, rifle, Blunderbuss
// or cannon) publishes where its muzzle is, world space, and the renderer lights the flash there
// on its next frame (vk_rasterizer.cpp VrFogUpload); the lists' way stays for other flashes.
namespace VR::MuzzleFlash {

namespace Detail {
inline std::mutex mutex;
inline std::array<float, 3> where{};
inline std::atomic<u64> shots{0};
} // namespace Detail

// A shot at `x, y, z` (the game's world space).
inline void Publish(float x, float y, float z) {
    {
        std::scoped_lock lock{Detail::mutex};
        Detail::where = {x, y, z};
    }
    Detail::shots.fetch_add(1, std::memory_order_release);
}

// The newest shot after `seen` (updated): true and its position, or false.
inline bool Take(u64& seen, std::array<float, 3>& where) {
    const u64 shots = Detail::shots.load(std::memory_order_acquire);
    if (shots == seen) {
        return false;
    }
    seen = shots;
    std::scoped_lock lock{Detail::mutex};
    where = Detail::where;
    return true;
}

// An explosion (fog_light.h Blast*: a light the game spawns beside it lights the air at once while
// young): where the game launched a bullet that stands with a hit radius of a metre or more
// (bloodborne_quickitems.cpp FogBurstLaunch), world space. The renderer's lamp memory listens
// (ListenBlasts, at every update); while nothing listens, the launch hook does not look.
namespace Detail {
inline std::array<float, 3> blast_where{};
inline std::atomic<u64> blasts{0};
inline std::atomic<s64> blast_until{0};
} // namespace Detail

inline void ListenBlasts(s64 now_ns) {
    Detail::blast_until.store(now_ns + 500'000'000, std::memory_order_relaxed);
}
inline bool BlastsWanted(s64 now_ns) {
    return now_ns <= Detail::blast_until.load(std::memory_order_relaxed);
}
inline void PublishBlast(float x, float y, float z) {
    {
        std::scoped_lock lock{Detail::mutex};
        Detail::blast_where = {x, y, z};
    }
    Detail::blasts.fetch_add(1, std::memory_order_release);
}
// The newest explosion after `seen` (updated): true and its position, or false.
inline bool TakeBlast(u64& seen, std::array<float, 3>& where) {
    const u64 blasts = Detail::blasts.load(std::memory_order_acquire);
    if (blasts == seen) {
        return false;
    }
    seen = blasts;
    std::scoped_lock lock{Detail::mutex};
    where = Detail::blast_where;
    return true;
}

} // namespace VR::MuzzleFlash
