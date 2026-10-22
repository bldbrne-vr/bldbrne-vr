// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

// The lock-on marker on the UI layer (vr.ini ui_lock_marker_scale / ui_lock_marker_alpha):
// the glow the game draws on the locked-on target
// is shown a little smaller about its own centre and a little fainter, nothing else of the UI.
//
// The rasterizer draws the marker's UI draws (VrUiLayer marker pair, never K and W) alone into
// their own emission / transmittance pair Km, Tm (Km cleared to black, Wm to the grey g:
// Tm = (Wm - Km) / g). The resolve (vr_ui_resolve.comp, mode 8 then mode 1) measures the box of
// the marker's pixels, and every UI pixel p within the box's scaled image takes the marker sampled
// at c + (p - c) / s (c: the box's centre, s: the scale; bilinear), dimmed by the factor a and put
// over the rest of the UI (the game draws it last, over the target's HP bar; under it when some UI
// was drawn after it, e.g. a menu over the HUD). Everything here mirrors that shader's math (unit
// tested in tests/test_ui_lock_marker.cpp).
//
// Dimming a draw by a: whatever it did to a destination d (d' = Km + Tm * d per channel, for
// every blend affine in d: over, additive, premultiplied) it now does a of it:
//   d' = d + a * (Km + Tm * d - d) = a * Km + (1 - a * (1 - Tm)) * d,
// i.e. Km' = a * Km and Tm' = 1 - a * (1 - Tm): an over blend's alpha times a, an additive glow
// times a. The marker over the rest of the UI (K, T): Km' + Tm' * K, Tm' * T; under it:
// K + T * Km', T * Tm' (Under(above, below) is both).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace VR::Ui::LockMarker {

/// The marker's size and brightness factors in effect (vr.ini's, clamped as the parser does).
struct Factors {
    float scale{1.0f};
    float alpha{1.0f};
};

[[nodiscard]] inline Factors Clamp(float scale, float alpha) {
    return {std::clamp(std::isfinite(scale) ? scale : 1.0f, 0.25f, 2.0f),
            std::clamp(std::isfinite(alpha) ? alpha : 1.0f, 0.0f, 1.0f)};
}

/// Anything to do: 1 / 1 draws the marker exactly as the game does (no separate pair at all).
[[nodiscard]] inline bool Active(const Factors& f) {
    return std::abs(f.scale - 1.0f) > 1e-3f || f.alpha < 1.0f - 1e-3f;
}

/// The factors as the resolve's push constant carries them, in the upper 24 bits of `raw_order`
/// (its low 8 bits are the scene's channel order): scale in 1/1024 (12 bits, up to 3.999), alpha
/// in 1/4095 (12 bits).
[[nodiscard]] inline std::uint32_t Pack(const Factors& f) {
    const auto s = static_cast<std::uint32_t>(std::lround(std::clamp(f.scale, 0.0f, 3.999f) * 1024.0f));
    const auto a = static_cast<std::uint32_t>(std::lround(std::clamp(f.alpha, 0.0f, 1.0f) * 4095.0f));
    return (std::min(s, 4095u) << 8) | (std::min(a, 4095u) << 20);
}

[[nodiscard]] inline Factors Unpack(std::uint32_t bits) {
    return {static_cast<float>((bits >> 8) & 0xFFFu) / 1024.0f,
            static_cast<float>((bits >> 20) & 0xFFFu) / 4095.0f};
}

/// The marker's pixels (band px, inclusive: the shader's atomic min / max).
struct Box {
    std::int32_t x0{1}, y0{1}, x1{0}, y1{0};
    [[nodiscard]] bool Valid() const {
        return x1 >= x0 && y1 >= y0;
    }
};

struct Point {
    float x{}, y{};
};

/// The box's centre in continuous band coordinates (pixel i covers [i, i + 1)).
[[nodiscard]] inline Point Centre(const Box& b) {
    return {0.5f * static_cast<float>(b.x0 + b.x1 + 1), 0.5f * static_cast<float>(b.y0 + b.y1 + 1)};
}

/// Where the output position `p` (continuous) samples the marker: the inverse of the scale about
/// the centre.
[[nodiscard]] inline Point Source(Point p, Point c, float scale) {
    return {c.x + (p.x - c.x) / scale, c.y + (p.y - c.y) / scale};
}

/// The output pixels that can show the marker (inclusive, one pixel of filter margin around the
/// scaled box): the only ones the resolve samples it for.
[[nodiscard]] inline Box OutputBox(const Box& b, float scale) {
    const Point c = Centre(b);
    const float x0 = c.x + scale * (static_cast<float>(b.x0) - c.x);
    const float y0 = c.y + scale * (static_cast<float>(b.y0) - c.y);
    const float x1 = c.x + scale * (static_cast<float>(b.x1 + 1) - c.x);
    const float y1 = c.y + scale * (static_cast<float>(b.y1 + 1) - c.y);
    return {static_cast<std::int32_t>(std::floor(x0)) - 1,
            static_cast<std::int32_t>(std::floor(y0)) - 1,
            static_cast<std::int32_t>(std::ceil(x1)), static_cast<std::int32_t>(std::ceil(y1))};
}

/// One channel of a draw's effect: d' = k + t * d.
struct Effect {
    float k{0.0f}; ///< emission (blend domain)
    float t{1.0f}; ///< transmittance
    [[nodiscard]] float Apply(float d) const {
        return k + t * d;
    }
};

/// The effect `a` of it: d + a * (e(d) - d).
[[nodiscard]] inline Effect Dim(Effect e, float a) {
    return {a * e.k, 1.0f - a * (1.0f - e.t)};
}

/// `below` first, then `above` over it.
[[nodiscard]] inline Effect Under(Effect above, Effect below) {
    return {above.k + above.t * below.k, above.t * below.t};
}

/// The marker's effect at a continuous source position from its per-pixel effects (bilinear over
/// the four nearest pixel centres; pixels outside the image or the marker's box are empty).
template <typename Fetch>
[[nodiscard]] Effect Sample(Point src, const Box& box, Fetch&& fetch) {
    const float sx = src.x - 0.5f;
    const float sy = src.y - 0.5f;
    const auto ix = static_cast<std::int32_t>(std::floor(sx));
    const auto iy = static_cast<std::int32_t>(std::floor(sy));
    const float fx = sx - static_cast<float>(ix);
    const float fy = sy - static_cast<float>(iy);
    Effect out{0.0f, 0.0f};
    for (std::int32_t dy = 0; dy < 2; ++dy) {
        for (std::int32_t dx = 0; dx < 2; ++dx) {
            const float w = (dx != 0 ? fx : 1.0f - fx) * (dy != 0 ? fy : 1.0f - fy);
            const std::int32_t x = ix + dx;
            const std::int32_t y = iy + dy;
            const bool inside = x >= box.x0 && x <= box.x1 && y >= box.y0 && y <= box.y1;
            const Effect e = inside ? fetch(x, y) : Effect{};
            out.k += w * e.k;
            out.t += w * e.t;
        }
    }
    return out;
}

} // namespace VR::Ui::LockMarker
