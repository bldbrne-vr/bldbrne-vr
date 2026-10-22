// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

// The UI layer's pixels without UI (vr.ini ui_empty_snap).
// The rasterizer draws every UI draw into K (cleared to black) and W
// (cleared to the grey g, vr.ini ui_grey 0.5); the resolve (vr_ui_resolve.comp) reads the UI alone
// from them: transmittance T = (W - K) / g, alpha from a two-point fit (exact over a black and a
// white world). K and W are 8-bit: a GPU stores the grey 0.5 as 127 or 128 of 255, and where no UI
// drew, 127 reads as T = 127 / 127.5 = 0.996, a black alpha of 2/255 over every pixel without UI
// (the rest quad's 14.4 x 8.1 m: the whole view 0.8% darker). Where K and W still hold their clear
// values to within one 8-bit step (Empty::IsEmpty) the resolve makes the pixel exactly transparent;
// every pixel any UI moved K or W further on is resolved exactly as before. Everything here
// mirrors that shader's math (unit tested in tests/test_ui_empty_pixel.cpp).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace VR::Ui::Empty {

using Rgb = std::array<float, 3>;

[[nodiscard]] inline float SrgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

[[nodiscard]] inline float LinearToSrgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c < 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

[[nodiscard]] inline float Luma(const Rgb& c) {
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

/// The grey W was cleared to, in its raw channel values: encoded when the UI drew through sRGB
/// views (`srgb_rt`, the clear goes through the view), else the grey itself.
[[nodiscard]] inline float GreyRaw(float grey, bool srgb_rt) {
    return srgb_rt ? LinearToSrgb(grey) : grey;
}

/// K's tolerance above black (raw: byte 0, its clear value is exact) and W's about the grey (raw:
/// the grey as any rounding within 0.6 LSB stores it).
inline constexpr float EmptyKMax = 0.5f / 255.0f;
inline constexpr float EmptyWTolerance = 0.6f / 255.0f;

/// No UI drew on this pixel, to within one 8-bit step: K and W (raw channels, 0..1, as the resolve
/// reads them through the targets' UNORM views) at their clear values. Anything such a pixel
/// drops changed the game's own image by about 1.5 LSB at most where the GPU stores the grey 0.5
/// as 127 (3.5 where it stores 128: a black blend that took W from 128 to 127).
[[nodiscard]] inline bool IsEmpty(const Rgb& k, const Rgb& w, float grey, bool srgb_rt) {
    const float g = GreyRaw(grey, srgb_rt);
    for (int c = 0; c < 3; ++c) {
        if (!(k[c] < EmptyKMax) || !(std::abs(w[c] - g) <= EmptyWTolerance)) {
            return false;
        }
    }
    return true;
}

/// The resolve's transmittance per channel (vr_ui_resolve.comp Transmittance): in the domain the
/// game blended in; a channel where W clipped at 1 (an additive glow) takes the largest T of the
/// unclipped ones, or 1.
[[nodiscard]] inline Rgb Transmittance(const Rgb& k, const Rgb& w, float grey, bool srgb_rt) {
    Rgb t{};
    std::array<bool, 3> clipped{};
    float free_t = -1.0f;
    for (int c = 0; c < 3; ++c) {
        const float d = srgb_rt ? SrgbToLinear(w[c]) - SrgbToLinear(k[c]) : w[c] - k[c];
        t[c] = std::clamp(d / grey, 0.0f, 1.0f);
        clipped[c] = w[c] >= 254.5f / 255.0f && k[c] < w[c] - 1.5f / 255.0f;
        if (!clipped[c]) {
            free_t = std::max(free_t, t[c]);
        }
    }
    const float fill = free_t >= 0.0f ? free_t : 1.0f;
    for (int c = 0; c < 3; ++c) {
        if (clipped[c]) {
            t[c] = std::max(t[c], fill);
        }
    }
    return t;
}

/// One UI-layer pixel: its colour bytes (premultiplied, raw) and alpha, as stored (8-bit).
struct Pixel {
    std::array<std::uint8_t, 3> rgb{};
    std::uint8_t alpha{};
};

/// A UI-layer pixel from its emission K (raw) and transmittance T at the eyes' gamma 1
/// (vr_ui_resolve.comp UiPixelKT, no veil): the colour bytes are K's, alpha the two-point fit.
[[nodiscard]] inline Pixel FromKT(const Rgb& k, const Rgb& t, bool srgb_rt) {
    Rgb k_lin{}, white_lin{};
    for (int c = 0; c < 3; ++c) {
        k_lin[c] = SrgbToLinear(k[c]);
        white_lin[c] = srgb_rt ? std::min(k_lin[c] + t[c], 1.0f)
                               : SrgbToLinear(std::min(k[c] + t[c], 1.0f));
    }
    const float transmit = Luma({white_lin[0] - k_lin[0], white_lin[1] - k_lin[1],
                                 white_lin[2] - k_lin[2]});
    const float alpha = std::clamp(1.0f - transmit, 0.0f, 1.0f);
    const auto byte = [](float v) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    return {{byte(k[0]), byte(k[1]), byte(k[2])}, byte(alpha)};
}

/// K and T of one pixel from K and W (vr_ui_resolve.comp ReadKT): an empty pixel is exactly
/// K = 0, T = 1 with `snap` (ui_empty_snap), else Transmittance.
inline Rgb ReadKT(Rgb& k, const Rgb& w, float grey, bool srgb_rt, bool snap) {
    if (snap && IsEmpty(k, w, grey, srgb_rt)) {
        k = {0.0f, 0.0f, 0.0f};
        return {1.0f, 1.0f, 1.0f};
    }
    return Transmittance(k, w, grey, srgb_rt);
}

/// The resolve's pixel from K and W (ReadKT, then FromKT).
[[nodiscard]] inline Pixel Resolve(Rgb k, const Rgb& w, float grey, bool srgb_rt, bool snap) {
    const Rgb t = ReadKT(k, w, grey, srgb_rt, snap);
    return FromKT(k, t, srgb_rt);
}

} // namespace VR::Ui::Empty
