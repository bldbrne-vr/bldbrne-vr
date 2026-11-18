// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cmath>

// Gamepad third person (play_style=gamepad_third; the tester, 2026-11-08: "if left stick is pressed
// then up and down on right stick controls zoom"): with the left stick click held, the right
// stick's up / down zooms the camera in and out (vr.ini tp_distance_scale, the VR page's Camera
// distance) instead of raising it. Bloodborne does nothing with L3 (its jump is a Circle tap after
// a sprint). The game's own distance is the farthest: its collision placed the camera there, and
// along the line to the hunter it never goes through a wall (core/vr/bloodborne_camera.cpp).
// Header-only (tests/test_pad_zoom.cpp).
namespace VR::PadZoom {

inline constexpr float Closest = 0.5f;  // of the game's distance
inline constexpr float Farthest = 1.0f; // the game's own
inline constexpr float Dead = 0.2f;     // the stick's dead zone (as the camera's pitch)
inline constexpr float RatePerS = 0.6f; // the whole range in under a second at full push

// The distance scale after `dt` s with the stick at `stick_y` (-1 up .. 1 down, as the pad's axis):
// up zooms in, down out.
[[nodiscard]] inline float Step(float scale, float stick_y, float dt) {
    if (!std::isfinite(scale)) {
        scale = Farthest;
    }
    if (!std::isfinite(stick_y) || !std::isfinite(dt) || std::abs(stick_y) <= Dead) {
        return std::clamp(scale, Closest, Farthest);
    }
    const float push = (std::min(std::abs(stick_y), 1.0f) - Dead) / (1.0f - Dead);
    const float step = (stick_y < 0.0f ? -1.0f : 1.0f) * push * RatePerS * std::clamp(dt, 0.0f, 0.1f);
    return std::clamp(scale + step, Closest, Farthest);
}

} // namespace VR::PadZoom
