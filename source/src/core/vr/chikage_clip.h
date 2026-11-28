// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace VR::Chikage {
// The native blade and its blood overlay use this same model-space plane for both eyes.
// Rounding the signed ten-bit normal costs at most about 1 mm per metre along each axis.
struct BladeClip {
    std::uint32_t normal{};
    float offset{};
    static constexpr std::int64_t ReleaseNs = 80'000'000; // follow the guide as it lets go
    static BladeClip AtMouth(const float* sword, const float* sheath, float seat_y,
                             float release = 0.0f) {
        if (release >= 1.0f) return {};
        std::array<float, 3> n{};
        float d = 0.0f;
        for (int row = 0; row < 3; ++row) {
            const int j = row * 4;
            const float outward = sheath[j + 1]; // model +Y points out of the opening
            for (int axis = 0; axis < 3; ++axis) n[axis] += sword[j + axis] * outward;
            d += (sword[j + 3] - sheath[j + 3] + seat_y * outward) * outward;
        }
        const float length = std::hypot(n[0], n[1], n[2]);
        if (!(length > 1e-6f) || !std::isfinite(length)) return {};
        BladeClip out{0, d / length * 511.0f};
        if (!std::isfinite(out.offset)) return {};
        float fully_visible = 0.0f;
        for (int axis = 0; axis < 3; ++axis) {
            const int q = static_cast<int>(std::round(n[axis] / length * 511.0f));
            out.normal |= (static_cast<std::uint32_t>(q) & 1023u) << (10 * axis);
            if (release > 0.0f) {
                // A conservative box around both native blades, including the hilt.
                constexpr float low[]{-0.16f, -1.20f, -0.10f}, high[]{0.16f, 0.35f, 0.10f};
                fully_visible -= q * (q < 0 ? high[axis] : low[axis]);
            }
        }
        if (release > 0.0f && fully_visible > out.offset)
            out.offset += release * (fully_visible - out.offset);
        return out;
    }
};

// Index count and FNV-1a of each native Chikage blade face set (all LODs and motion passes).
// The case is a separate mesh and shares none of these keys.
struct BladeFace {
    std::uint64_t hash{};
    unsigned slot{};
    static constexpr unsigned Count = 12;
};
inline constexpr BladeFace FindBladeFace(std::uint32_t count) {
    switch (count) {
    case 5556: return {0xb4f3d7fc9b8f31c3ull, 0};
    case 3330: return {0xf98ab3fe5132d38dull, 1};
    case 1107: return {0xc4964c0d0bce33b0ull, 2};
    case 5724: return {0xc2188cd5942d75cdull, 3};
    case 3438: return {0xc315313df8baac0aull, 4};
    case 1155: return {0xb42afc23b764ec76ull, 5};
    case 6762: return {0x31a6b40a298977f3ull, 6};
    case 4053: return {0x3529fe4d716718e4ull, 7};
    case 1344: return {0x520876aaa24e22a6ull, 8};
    case 6930: return {0x58935364b1d7e153ull, 9};
    case 4137: return {0x93756eb16379e9e8ull, 10};
    case 1374: return {0x1c97db5618a45ae8ull, 11};
    default: return {};
    }
}
} // namespace VR::Chikage
