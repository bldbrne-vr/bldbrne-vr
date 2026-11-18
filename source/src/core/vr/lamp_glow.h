// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cmath>
#include <cstdlib>

// The save lamps' glow made brighter (TEST, 2026-11-12, the user: "I do want to make the lamp
// cast shadows"): a lamp's own light is a dim warm one (Central Yharnam's start: r 6 m, lum 0.4,
// rgb 0.6 0.4 0.2), too faint for any shadow of it to show. VR_LAMP_GLOW=k multiplies such a
// light's colour by k: warm (red over blue by 1.15), dimmer than the lamp shadows' cut-off
// (lum 0.15..0.6) and off the camera (past 0.7 m: the game's grey light sits on it). Brighter
// than the cut-off then, it casts (the screen march and his body maps). Off (1) by default.
namespace VR::LampGlow {

inline float Boost() {
    static const float k = [] {
        const char* const env = std::getenv("VR_LAMP_GLOW");
        const float v = env != nullptr ? float(std::atof(env)) : 1.0f;
        return std::isfinite(v) && v >= 1.0f && v <= 8.0f ? v : 1.0f;
    }();
    return k;
}

// A light of colour (r, g, b) at `metres` from the eye: one of the glows Boost brightens.
inline bool IsGlow(float r, float g, float b, float metres) {
    const float lum = 0.3f * r + 0.6f * g + 0.1f * b;
    return r > 1.15f * b && lum >= 0.15f && lum < 0.6f && metres > 0.7f;
}

} // namespace VR::LampGlow

// The game's light on the camera (TEST, 2026-11-12, the user: "can we weaken the camera's gray
// light? or maybe even fully remove it?"): a grey point light about 0.4 m from the camera (r 3.5 m,
// lum 0.47, rgb 0.47 0.47 0.47; light list audit, Central Yharnam and the Grand Cathedral). In the
// flat game it keeps the hunter out of the dark from the third-person camera; in VR it lights the
// hands, the weapon and the ground near the eye from the eye. VR_CAMERA_LIGHT=k scales it (0: off;
// 1, the default: as the game has it).
namespace VR::CameraLight {

inline float Scale() {
    static const float k = [] {
        const char* const env = std::getenv("VR_CAMERA_LIGHT");
        const float v = env != nullptr ? float(std::atof(env)) : 1.0f;
        return std::isfinite(v) && v >= 0.0f && v <= 1.0f ? v : 1.0f;
    }();
    return k;
}

// A light of colour (r, g, b) at `metres` from the eye: the camera's (grey, within 0.7 m).
inline bool Is(float r, float g, float b, float metres) {
    const float hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
    const float lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
    return metres < 0.7f && hi > 0.0f && hi - lo < 0.05f * hi;
}

} // namespace VR::CameraLight
