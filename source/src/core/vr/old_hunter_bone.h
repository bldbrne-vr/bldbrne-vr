// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace VR::OldHunterBone {
inline constexpr int Goods = 2060;
inline constexpr float HeldScale = .40f;
inline constexpr float StoredRatio = .274235748f / HeldScale;
inline constexpr std::int64_t PulseNs = 650'000'000;
// Site A publishes only successful uses; no timers, particles or mutable render state.
struct Use {
    std::int64_t ns{};
};
inline float Pulse(Use use, std::int64_t held_since, std::int64_t now) {
    if (held_since <= 0 || use.ns < held_since || now < use.ns)
        return 0.0f;
    const auto elapsed = static_cast<std::uint64_t>(now) - static_cast<std::uint64_t>(use.ns);
    if (elapsed >= PulseNs)
        return 0.0f;
    const float t = static_cast<float>(elapsed) / static_cast<float>(PulseNs);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}
} // namespace VR::OldHunterBone
