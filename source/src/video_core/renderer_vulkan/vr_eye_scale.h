// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

// vr.ini eye_size: how large each eye image goes to the
// headset, from its crop of the render. Header-only (tests/test_eye_scale.cpp); used by
// OpenXRPresenter::EyeOutputExtents and VrEyeUpscale.

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.h"

namespace Vulkan::VrEyeScale {

struct Size {
    u32 width{};
    u32 height{};
    bool operator==(const Size&) const = default;
};

// vr.ini eye_size: auto = the runtime's recommended eye size (with Meta Link the Link app's
// render resolution); panel = the headset's own panel pixels per eye (Quest 3: 2064x2208),
// whatever the Link app's slider says; a number = that factor of the render's crop.
enum class Mode : u8 { Auto = 0, Panel = 1, Factor = 2 };

// The largest factors: auto as before (2), panel up to 3 for the smaller presets (FSR 1's EASU is
// still an edge-directed Lanczos there, and better than the compositor's bilinear stretch).
inline constexpr float MaxAutoFactor = 2.0f;
inline constexpr float MaxPanelFactor = 3.0f;
inline constexpr float MaxFactor = 4.0f; // Scaled's own limit

// An eye of `in` pixels enlarged `scale` times (1 or less: as it is): both sides even, never
// smaller than `in`, at most 4x.
inline Size Scaled(Size in, float scale) {
    if (!(scale > 1.0f + 1e-3f)) {
        return in;
    }
    scale = std::min(scale, MaxFactor);
    const auto even = [scale](u32 v) {
        return std::max<u32>(2u * static_cast<u32>(std::lround(0.5 * v * scale)), v);
    };
    return {even(in.width), even(in.height)};
}

// A headset's panel: its pixels per eye and its model, from the runtime's system name
// (xrGetSystemProperties::systemName, e.g. "Meta Quest 3" or "Oculus Quest2"). The name is read
// as words split at anything but letters and digits and between letters and digits ("Quest3S" =
// quest 3 s), case ignored. size {0, 0}: not a headset we know (eye_size=panel then follows the
// runtime's recommended size).
struct Panel {
    Size size{};
    std::string_view model{};
};

inline std::vector<std::string> NameWords(std::string_view name) {
    std::vector<std::string> words;
    std::string word;
    int kind = 0; // 1 letter, 2 digit
    for (const char ch : name) {
        const unsigned char u = static_cast<unsigned char>(ch);
        const int k = (u >= '0' && u <= '9') ? 2 : ((u | 0x20) >= 'a' && (u | 0x20) <= 'z') ? 1 : 0;
        if (k == 0 || (k != kind && !word.empty())) {
            if (!word.empty()) {
                words.push_back(word);
                word.clear();
            }
        }
        if (k != 0) {
            word += static_cast<char>(k == 1 ? (u | 0x20) : u);
        }
        kind = k;
    }
    if (!word.empty()) {
        words.push_back(word);
    }
    return words;
}

inline Panel KnownPanel(std::string_view system_name) {
    const std::vector<std::string> w = NameWords(system_name);
    const auto next = [&w](size_t i, size_t n) -> std::string_view {
        return i + n < w.size() ? std::string_view{w[i + n]} : std::string_view{};
    };
    for (size_t i = 0; i < w.size(); ++i) {
        if (w[i] == "quest") {
            const std::string_view a = next(i, 1);
            if (a == "3") {
                return next(i, 2) == "s" ? Panel{{1832, 1920}, "Quest 3S"}
                                         : Panel{{2064, 2208}, "Quest 3"};
            }
            if (a == "2") {
                return {{1832, 1920}, "Quest 2"};
            }
            if (a == "pro") {
                return {{1800, 1920}, "Quest Pro"};
            }
            if (a.empty() || a == "1") {
                return {{1440, 1600}, "Quest"};
            }
            return {}; // a later Quest: not known yet
        }
        if (w[i] == "rift") {
            return next(i, 1) == "s" ? Panel{{1280, 1440}, "Rift S"} : Panel{{1080, 1200}, "Rift"};
        }
    }
    return {};
}

// What capped the factor (for the log).
enum class Limit : u8 {
    None,
    MaxFactor, // MaxAutoFactor / MaxPanelFactor / MaxFactor
    Swapchain, // both eyes side by side (the late warp's swapchain, + 64 px) in the largest one
    ImageRect, // an eye larger than the runtime's largest image rect
    TooSmall,  // below x1.02: not worth a pass (the eyes as warped)
};

inline std::string_view LimitText(Limit limit) {
    switch (limit) {
    case Limit::MaxFactor:
        return "capped at the largest factor";
    case Limit::Swapchain:
        return "capped so both eyes fit the runtime's largest swapchain side by side";
    case Limit::ImageRect:
        return "capped at the runtime's largest eye image";
    case Limit::TooSmall:
        return "the render's crop is already about that size";
    default:
        return "";
    }
}

struct Choice {
    float factor{1.0f};
    Mode mode{Mode::Auto}; // the mode that applied: panel without a known panel is auto
    Size target{};         // auto: the recommended (left eye); panel: the panel; factor: {}
    Limit limit{Limit::None};
    bool floored{false}; // auto: raised to eye_size_min_factor (the crop is about the recommended
                         // size or larger)
};

// The default of vr.ini eye_size_min_factor: auto never shows the crop as rendered with only RCAS
// on its raw pixels (3200x2400 at Link 1x: x1.0, +80% stair-steps in the Dream against 2560's
// x1.23 EASU); a small EASU step does as well as the panel size (+29% / +5% at equal sharpness,
// +0.13 ms of late warp; 2026-10-07).
inline constexpr float DefaultMinFactor = 1.13f;

// The factor for eye crops `crops` (render pixels):
// - Factor: `setting` (vr.ini eye_size 1 .. 2);
// - Auto: the runtime's recommended eye size `recommended`: the larger of the width and height
//   ratios, at least 1 and at most MaxAutoFactor;
// - Panel: the same with the headset's panel `panel` (both eyes), at most MaxPanelFactor; with
//   no panel (an unknown headset) Auto;
// - Auto (eye_size_min_factor `min_factor`, 1 = none): at least `min_factor`, so a crop already
//   about the recommended size or larger still gets FSR's EASU instead of RCAS alone on its raw
//   pixels; that raise never makes an eye larger than a known `panel` (in both directions);
// - then capped so both eyes side by side (the late warp's swapchain, rounded up to 64 px) fit
//   the runtime's largest swapchain `max_swapchain`, and each eye its largest image rect
//   `max_rect`;
// - below 1.02: 1 (not worth a pass).
inline Choice Choose(Mode mode, float setting, const std::array<Size, 2>& crops,
                     const std::array<Size, 2>& recommended, Size panel, Size max_swapchain,
                     Size max_rect, float min_factor = 1.0f) {
    Choice c;
    if (mode == Mode::Panel && (panel.width == 0 || panel.height == 0)) {
        mode = Mode::Auto;
    }
    c.mode = mode;
    for (const Size& s : crops) {
        if (s.width == 0 || s.height == 0) {
            return c;
        }
    }
    const auto ratio = [&crops](const std::array<Size, 2>& target) {
        float r = 0.0f;
        for (u32 i = 0; i < 2; ++i) {
            r = std::max({r,
                          static_cast<float>(target[i].width) / static_cast<float>(crops[i].width),
                          static_cast<float>(target[i].height) /
                              static_cast<float>(crops[i].height)});
        }
        return r;
    };
    float scale = 1.0f;
    float cap = MaxFactor;
    if (mode == Mode::Factor) {
        scale = setting;
    } else if (mode == Mode::Auto) {
        c.target = recommended[0];
        scale = std::max(ratio(recommended), 1.0f);
        cap = MaxAutoFactor;
        // eye_size_min_factor: never the raw crop with RCAS alone; never past a known panel.
        float floor_to = std::min(min_factor, MaxAutoFactor);
        if (panel.width != 0 && panel.height != 0) {
            for (const Size& s : crops) {
                floor_to = std::min({floor_to,
                                     static_cast<float>(panel.width) / static_cast<float>(s.width),
                                     static_cast<float>(panel.height) /
                                         static_cast<float>(s.height)});
            }
        }
        if (floor_to > scale) {
            scale = floor_to;
            c.floored = true;
        }
    } else {
        c.target = panel;
        scale = std::max(ratio({panel, panel}), 1.0f);
        cap = MaxPanelFactor;
    }
    if (!(scale > 0.0f)) {
        scale = 1.0f;
    }
    if (scale > cap) {
        scale = cap;
        c.limit = Limit::MaxFactor;
    }
    const float both_w = static_cast<float>(crops[0].width + crops[1].width);
    const float tall = static_cast<float>(std::max(crops[0].height, crops[1].height));
    const float wide = static_cast<float>(std::max(crops[0].width, crops[1].width));
    const float swapchain =
        std::min((static_cast<float>(max_swapchain.width) - 64.0f) / both_w,
                 (static_cast<float>(max_swapchain.height) - 64.0f) / tall);
    if (swapchain < scale) {
        scale = swapchain;
        c.limit = Limit::Swapchain;
    }
    const float rect = std::min(static_cast<float>(std::max<u32>(max_rect.width, 1)) / wide,
                                static_cast<float>(std::max<u32>(max_rect.height, 1)) / tall);
    if (rect < scale) {
        scale = rect;
        c.limit = Limit::ImageRect;
    }
    if (scale < 1.02f) {
        if (c.limit == Limit::None && mode != Mode::Factor) {
            c.limit = Limit::TooSmall;
        }
        scale = 1.0f;
        c.floored = false;
    }
    c.factor = scale;
    return c;
}

// The factor alone, as before eye_size=panel: `setting` > 0 that factor, else auto.
inline float Factor(float setting, const std::array<Size, 2>& crops,
                    const std::array<Size, 2>& recommended, Size max_swapchain, Size max_rect) {
    return Choose(setting > 0.0f ? Mode::Factor : Mode::Auto, setting, crops, recommended, {},
                  max_swapchain, max_rect)
        .factor;
}

} // namespace Vulkan::VrEyeScale
