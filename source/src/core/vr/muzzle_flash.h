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

} // namespace VR::MuzzleFlash
