// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>

// The torch held up (2026-11-12, the user: "when we lift the torch ... only slightly bump the
// intensity up (less than we do now), but increase its light range a tiny bit too (as if it's
// higher so it reaches further)"). torch_lift added one or two more copies of the torch's own
// flame, each a full light of the same reach: up to three times the light, no further. Now
// (torch_lift_copies 0) the torch update (bloodborne_held.cpp) publishes where its flame is and
// how high it is held, and the renderer (vk_rasterizer.cpp, the muzzle flash's slot: the torch and
// the guns share the left hand) adds a gentle glow there: a share of the game's own torch light,
// reaching further (shader_recompiler/ir/passes/fog_light.h TorchGlow*).
namespace VR::TorchGlow {

namespace Detail {
inline std::mutex mutex;
inline std::array<float, 3> where{};
inline int level{};
inline std::atomic<long long> published_ns{0};
} // namespace Detail

inline long long NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The flame at `at` (the game's world space), held `level` high (1 raised, 2 held high).
inline void Publish(const std::array<float, 3>& at, int level) {
    {
        std::scoped_lock lock{Detail::mutex};
        Detail::where = at;
        Detail::level = level;
    }
    Detail::published_ns.store(NowNs(), std::memory_order_release);
}

// The flame and its level, when published within `fresh_s` (a lowered or put-away torch stops
// publishing: the glow goes with it).
inline bool Take(float fresh_s, std::array<float, 3>& at, int& level) {
    const long long when = Detail::published_ns.load(std::memory_order_acquire);
    if (when == 0 || static_cast<float>(NowNs() - when) * 1e-9f > fresh_s) {
        return false;
    }
    std::scoped_lock lock{Detail::mutex};
    at = Detail::where;
    level = Detail::level;
    return level > 0;
}

} // namespace VR::TorchGlow
