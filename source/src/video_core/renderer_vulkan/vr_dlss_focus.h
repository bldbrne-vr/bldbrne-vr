// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>

// Foveated upscaling (vr.ini upscale_focus: Off, Quality, Medium, Performance, read at start;
// the tester, 2026-11-06: "apply dlss/fsr only in the foveated area ... and do strong TAA in the
// rest"; VR_DLSS_FOCUS=<fraction> for tests): DLSS or FSR reconstructs only a rectangle around each
// eye's lens centre, `fraction` of the eye's width and height (Medium, 0.85: 72% of its pixels),
// cut out into images of its own first, so both eyes reach the upscaler the same way as any
// whole eye (vr_dlss_focus_pass.h); the whole eye is then composed from the upscaler's rectangle
// and the render enlarged around it with a TAA of its own (vr_dlss_focus.comp), blended over a
// band inside its edge. The evaluation's cost follows its output pixels (2026-11-03, RTX 5080,
// Ultra Quality model M at 2200x2376 per eye: 3.32 ms for both eyes, 1.88 ms at 56% of the area),
// and outside the rectangle foveation (when on) already shades 2x2 and 4x4 and the lenses blur.
// The rectangle is centred on the lens axes foveation uses (vr_eye_axes.h), foveation or not.
namespace Vulkan::DlssFocus {

// upscale_focus's levels: the rectangle's share of each side (0: off). Quality 0.90 (81% of
// the pixels upscaled), Medium 0.85 (72%), Performance 0.75 (56%).
inline float LevelFraction(int level) {
    switch (level) {
    case 1:
        return 0.90f;
    case 2:
        return 0.85f;
    case 3:
        return 0.75f;
    default:
        return 0.0f;
    }
}

// One eye's rectangle: in the render (DLSS input) and in the eye output, the same part of the
// view. Zero width: off.
struct Rect {
    std::uint32_t in_x{}, in_y{}, in_w{}, in_h{};
    std::uint32_t out_x{}, out_y{}, out_w{}, out_h{};
    bool On() const { return in_w != 0 && in_h != 0 && out_w != 0 && out_h != 0; }
    bool operator==(const Rect&) const = default;
};

// Off for the rest of the session: a cut-out or a composition failed (VrTemporalScene, which
// skips that frame). From the next frame the Plan asks for the whole eyes again; `why` names it
// in the log.
inline std::atomic<const char*>& OffReason() {
    static std::atomic<const char*> why{nullptr};
    return why;
}
inline void Off(const char* why) {
    OffReason().store(why, std::memory_order_relaxed);
}
inline bool IsOff() {
    return OffReason().load(std::memory_order_relaxed) != nullptr;
}

// VR_DLSS_FOCUS: the rectangle's share of the eye's width and height, 0.4 to 0.95; unset, 0 or
// anything else: off.
inline float Fraction(const char* value) {
    if (!value || !*value)
        return 0.0f;
    char* end{};
    const float f = std::strtof(value, &end);
    if (end == value || !std::isfinite(f) || f < 0.4f || f > 0.95f)
        return 0.0f;
    return f;
}

// The rectangle around the lens centre (cx, cy: fractions of the image from its left / top
// edge), `fraction` of each side, kept inside the image. Its input side is even and its output
// side the same part of the view at the output's scale, rounded; its output origin is the input
// origin scaled the same way, so both describe the same view to within half an output pixel.
inline Rect Plan(std::uint32_t in_w, std::uint32_t in_h, std::uint32_t out_w, std::uint32_t out_h,
                 float cx, float cy, float fraction) {
    if (!in_w || !in_h || !out_w || !out_h || in_w > out_w || in_h > out_h ||
        !(fraction >= 0.4f && fraction <= 0.95f) || !std::isfinite(cx) || !std::isfinite(cy))
        return {};
    const auto side = [&](std::uint32_t in, std::uint32_t out, float c, std::uint32_t& x,
                          std::uint32_t& w, std::uint32_t& ox, std::uint32_t& ow) {
        w = std::min(in, std::max<std::uint32_t>(16u, std::uint32_t(std::lround(fraction * in)) & ~1u));
        const float centre = std::clamp(c, 0.0f, 1.0f) * float(in);
        const long first = std::lround(centre - 0.5f * float(w));
        x = std::uint32_t(std::clamp<long>(first, 0, long(in - w)));
        const double scale = double(out) / double(in);
        ox = std::uint32_t(std::lround(double(x) * scale));
        ow = std::uint32_t(std::lround(double(w) * scale));
        ow = std::min(ow, out - std::min(ox, out));
    };
    Rect r;
    side(in_w, out_w, cx, r.in_x, r.in_w, r.out_x, r.out_w);
    side(in_h, out_h, cy, r.in_y, r.in_h, r.out_y, r.out_h);
    if (r.out_w < r.in_w || r.out_h < r.in_h)
        return {};
    return r;
}

} // namespace Vulkan::DlssFocus
