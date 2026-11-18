// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cmath>

// The light glow's god rays (the moon through the Hunter's Dream window, the blood moon, the Tomb
// of Oedon shafts) fan out from a point Bloodborne's FFX light-shaft renderer pushes away from the
// projected light: a direction run through the view-projection (clip x, -y and depth, normalized)
// times a fixed length, in screen units (0.066 of the screen, measured at every field of view).
// On the flat game's 16:9 screen at its own field of view that is a few degrees; on a VR render
// twice as wide it is twice the angle, and its outward part swings round the light as the head
// turns. Scale is the factor for that length that gives the push the angle the flat game gives it.
namespace VR::GlowPush {

// A projection: tan(half the vertical field of view) and width / height.
struct View {
    float tan_half_v{};
    float aspect{};
};

// `dir`: the game's normalized push direction (clip x, -y, depth) as `render` projects it. The
// same view direction through `game` has its x and y larger by rx and ry; normalized there and
// carried back to `render`'s screen it is the push times 1 / |(x rx, y ry, depth)|. 1 for the same
// view or any input that is not usable.
inline float Scale(const float dir[3], View render, View game) {
    if (!(render.tan_half_v > 0.0f && render.aspect > 0.0f && game.tan_half_v > 0.0f &&
          game.aspect > 0.0f) ||
        !std::isfinite(render.tan_half_v * render.aspect * game.tan_half_v * game.aspect)) {
        return 1.0f;
    }
    const float rx = (render.aspect * render.tan_half_v) / (game.aspect * game.tan_half_v);
    const float ry = render.tan_half_v / game.tan_half_v;
    const float x = dir[0] * rx, y = dir[1] * ry, z = dir[2];
    const float n = std::sqrt(x * x + y * y + z * z);
    return n > 1e-6f && std::isfinite(n) ? 1.0f / n : 1.0f;
}

} // namespace VR::GlowPush
