// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

// The rest quad on a cylinder (vr.ini ui_rest_curved): the enemy tags (HP bars, damage numbers),
// the lock-on cursor and whatever else the game draws where things are in the world.
//
// Why: the game projects an enemy's position onto its flat screen and draws the tag there. On the
// flat rest quad (a plane facing the frame's head direction) a tag is where its enemy is, but it
// lies in that plane: turn your head and the plane turns with it while the tag stays on its enemy,
// so a tag off to the side is seen at a changing slant (it "spins" like a sprite facing the view
// plane). Every point of a cylinder around the head faces the head: on it a tag slides along as
// you turn and keeps facing you.
//
// The game's image is a flat projection: column x is at angle atan(x) (x in units of the distance,
// half width X), a cylinder spaces its columns evenly in angle. So the image is pre-warped
// before it goes on the cylinder: a column at angle phi of the cylinder takes the flat image's
// column tan(phi), and a cylinder point at height h (same units) is in the direction of flat row
// h / cos(phi): the column, scaled by cos(phi) about the centre row (above and below it:
// transparent). Every tag then lands in exactly its enemy's direction, as on the flat quad.
//
// The warp is done by strips (Strips: blits of a few columns each, linearly filtered; every strip
// boundary within half a source pixel of the exact warp). The cylinder: radius = the quad's
// distance, CentralAngle(X), AspectRatio(X, Y) (OpenXR: height = radius * angle / aspect = 2 Y
// radius, the flat quad's height at its centre).
namespace VR::Ui::RestCurve {

// A strip of the warp: source columns [src_x0, src_x1) (all rows) to destination columns
// [dst_x0, dst_x1), rows [dst_y0, dst_y1).
struct Strip {
    int32_t src_x0, src_x1;
    int32_t dst_x0, dst_x1;
    int32_t dst_y0, dst_y1;
};

// The cylinder's angle for a flat image of half width `half_w` (tangent units).
inline float CentralAngle(float half_w) {
    return 2.0f * std::atan(half_w);
}

// OpenXR's aspect ratio (width / height of the visible section) for half extents X, Y.
inline float AspectRatio(float half_w, float half_h) {
    return half_h > 0.0f ? std::atan(half_w) / half_h : 1.0f;
}

// The flat image's column (0 .. w) a cylinder column edge `u` (0 .. w) takes.
inline float SourceColumn(float u, float w, float half_w) {
    const float phi = (2.0f * u / w - 1.0f) * std::atan(half_w);
    return (std::tan(phi) / half_w + 1.0f) * 0.5f * w;
}

// The strips warping a w x h flat image of half width `half_w` onto the cylinder, `strip_px`
// destination columns each. Returns how many were written to `out` (0: nothing to warp).
inline uint32_t Strips(uint32_t w, uint32_t h, float half_w, uint32_t strip_px,
                       std::span<Strip> out) {
    if (w == 0 || h == 0 || !(half_w > 0.0f) || !std::isfinite(half_w) || strip_px == 0) {
        return 0;
    }
    const float fw = static_cast<float>(w);
    const float max_angle = std::atan(half_w);
    const auto column = [&](uint32_t u) {
        return std::clamp<int32_t>(
            static_cast<int32_t>(std::lround(SourceColumn(static_cast<float>(u), fw, half_w))), 0,
            static_cast<int32_t>(w));
    };
    uint32_t n = 0;
    int32_t s0 = column(0);
    for (uint32_t x0 = 0; x0 < w && n < out.size(); x0 += strip_px) {
        const uint32_t x1 = std::min(w, x0 + strip_px);
        const int32_t s1 = column(x1);
        if (s1 > s0) {
            const float phi = (static_cast<float>(x0 + x1) / fw - 1.0f) * max_angle;
            const float half = 0.5f * static_cast<float>(h) * std::cos(phi);
            const float mid = 0.5f * static_cast<float>(h);
            const int32_t y0 = static_cast<int32_t>(std::lround(mid - half));
            const int32_t y1 = std::max(y0 + 1, static_cast<int32_t>(std::lround(mid + half)));
            out[n++] = {s0, s1, static_cast<int32_t>(x0), static_cast<int32_t>(x1), y0, y1};
            s0 = s1;
        }
    }
    return n;
}

} // namespace VR::Ui::RestCurve
