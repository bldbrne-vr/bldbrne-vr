// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace VR::ExecutionerGlove {
inline constexpr int Model = 12080;
inline constexpr unsigned TextureSize = 1024, TextureLevels = 9;
inline constexpr unsigned MaxVertices = 8192, MaxIndices = 18000;
inline constexpr unsigned MaxDecodedBytes = 4 * 1024 * 1024;
// Resting mesh extents for snug body storage only. Keep the larger animated
// envelope for toss clearance and asset validation.
inline constexpr std::array<float,3> StoredMin{-.02582605f,-.01544920f,-.13334489f};
inline constexpr std::array<float,3> StoredMax{ .02559116f, .04824543f, .02269054f};
// Monotonic bind-to-clutch palette. Release returns to bind before floor seating.
inline float Frame(float charge, bool detached) {
    return detached || !std::isfinite(charge) ? 0.0f : std::clamp(charge, 0.0f, 1.0f) * 254.0f;
}
} // namespace VR::ExecutionerGlove
