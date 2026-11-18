// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include "core/vr/call_beyond.h"

namespace VR::CallBeyond::Sky {
// Unlit quad subtype. No texture, allocation, resource upload or particle state.
inline constexpr float ShaderTag = -2010.0f;
inline constexpr int EyeCount = 4;
// Match the lighting system's distant-sky cutoff (air_light.h SkyM), in game-world units.
// The game draws a finite-depth sky dome: placing the eyes at depth 1 hides them behind it.
inline constexpr float SkyDepthM = 450.0f;
// Read only the already-published item snapshot. No game pointers or inventory scan.
// Kept generic so the small state tests need no renderer or quick-item dependencies.
template <class Snapshot>
inline bool Eligible(bool held, const State& state, int slot_index,
                     const Snapshot& snapshot, std::int64_t now) {
    if (!held || !state.held || !Fresh(now, state.published_ns) ||
        !snapshot.valid || !Fresh(now, snapshot.steady_ns) || !snapshot.alive ||
        snapshot.busy || slot_index < 0 ||
        static_cast<std::size_t>(slot_index) >= snapshot.slot.size()) return false;
    const auto& slot = snapshot.slot[static_cast<std::size_t>(slot_index)];
    return slot.goods == Goods && slot.qty > 0 && slot.usable &&
           (slot.cost <= 0 || snapshot.bullets < 0 || snapshot.bullets >= slot.cost);
}
inline float Smooth(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}
struct Fade {
    std::int64_t last_ns{};
    std::int64_t session_ns{};
    std::uint64_t space{};
    float value{};
    void Reset() { *this = {}; }
    float Update(std::int64_t now, std::uint64_t current_space, bool held,
                 std::int64_t current_session = 0) {
        if (!held) {
            if (last_ns != 0) Reset(); // once on loss of usability, not every inactive tick
            return 0.0f;
        }
        if (now <= 0) { Reset(); return 0.0f; }
        // A warp, pause or lost camera must not resume a previous world's apparition.
        if (now < last_ns || now - last_ns > 250'000'000 || space != current_space ||
            session_ns != current_session) {
            value = 0.0f;
            last_ns = now;
            space = current_space;
            session_ns = current_session;
            return value;
        }
        const float dt = float(now - last_ns) * 1e-9f;
        last_ns = now;
        value = std::clamp(value + dt / 1.6f, 0.0f, 1.0f);
        return Smooth(value);
    }
};
// Independent slow blinks, computed per eye on the CPU, not per pixel.
inline float Openness(double seconds, int eye) {
    const double period = 9.7 + double(eye) * 2.3;
    const float phase = float(std::fmod(seconds + double(eye) * 3.7, period));
    const float close = Smooth(phase / .16f);
    const float reopen = Smooth((phase - .20f) / .48f);
    return 1.0f - close * (1.0f - reopen);
}
} // namespace VR::CallBeyond::Sky
