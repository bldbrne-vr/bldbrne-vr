// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>

// Foveated upscaling (vr.ini upscale_focus: Off, Quality, Medium, Performance, sampled per stereo frame;
// the tester, 2026-11-06: "apply dlss/fsr only in the foveated area ... and do strong TAA in the
// rest"; VR_DLSS_FOCUS=<fraction> for tests): DLSS or FSR reconstructs only a rectangle around each
// eye's lens centre, `fraction` of the eye's width and height (Medium, 0.85: 72% of its pixels),
// cut out into images of its own first, so both eyes reach the upscaler the same way as any
// whole eye (vr_dlss_focus_pass.h); the whole eye is then composed from the upscaler's rectangle
// and the render enlarged around it with a TAA of its own (vr_dlss_focus.comp), blended over a
// band inside its edge. The evaluation's cost follows its output pixels (2026-11-03, RTX 5080,
// Ultra Quality model M at 2200x2376 per eye: 3.32 ms for both eyes, 1.88 ms at 56% of the area),
// and outside the rectangle foveation (when on) already shades 2x2 and 4x4 and the lenses blur.
// The rectangle follows the lens axes (vr_eye_axes.h), with proportional margins above and below.
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

// Eye dimensions can stay constant through a live level change.
struct CropKey {
    std::uint32_t input_width{}, input_height{}, output_width{}, output_height{};
    float fraction{};
    bool operator==(const CropKey&) const = default;
};

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

// The lens axis (cx, cy) is measured from the left and top edges. Keep `fraction` of each side.
// Horizontally centre on the axis where it fits. Vertically distribute the unused height in
// proportion to the space above and below the axis. Centring and clamping on a Quest 3 put the
// entire unused strip at the bottom on Medium and Quality. The scaled input origin and size
// describe the same region in the output to within half an output pixel of rounding.
inline Rect Plan(std::uint32_t in_w, std::uint32_t in_h, std::uint32_t out_w, std::uint32_t out_h,
                 float cx, float cy, float fraction) {
    if (!in_w || !in_h || !out_w || !out_h || in_w > out_w || in_h > out_h ||
        !(fraction >= 0.4f && fraction <= 0.95f) || !std::isfinite(cx) || !std::isfinite(cy))
        return {};
    const auto side = [&](std::uint32_t in, std::uint32_t out, float c, bool vertical, std::uint32_t& x,
                          std::uint32_t& w, std::uint32_t& ox, std::uint32_t& ow) {
        w = std::min(in, std::max<std::uint32_t>(16u, std::uint32_t(std::lround(fraction * in)) & ~1u));
        const float axis = std::clamp(c, 0.0f, 1.0f);
        const float start = vertical ? axis * float(in - w) : axis * float(in) - 0.5f * float(w);
        const long first = std::lround(start);
        x = std::uint32_t(std::clamp<long>(first, 0, long(in - w)));
        const double scale = double(out) / double(in);
        ox = std::uint32_t(std::lround(double(x) * scale));
        ow = std::uint32_t(std::lround(double(w) * scale));
        ow = std::min(ow, out - std::min(ox, out));
    };
    Rect r;
    side(in_w, out_w, cx, false, r.in_x, r.in_w, r.out_x, r.out_w);
    side(in_h, out_h, cy, true, r.in_y, r.in_h, r.out_y, r.out_h);
    if (r.out_w < r.in_w || r.out_h < r.in_h)
        return {};
    return r;
}

// Dynamic resolution (core/vr/s1_dynamic_resolution.h; the tester, 2026-11-22: "make dynamic
// resolution work with upscaler's foveation"): under load the view is drawn into the middle
// (100 - drop)% of each side of the render, about its centre, and DLSS reads only a sub-rectangle
// of its input, which its bridge centres in the input it was made for, `size * drop / 200` pixels
// in from each side (bbport_dlss_bridge.cpp Upscale). The rectangle's cut-out keeps its size and
// its images: it is copied from `src` on instead of from the rectangle's start, so that the
// bridge's sub-rectangle (`base`, `count` pixels) holds the rectangle's part of the view as it was
// drawn (to within half a render pixel), and the pixels around it are the render's own
// neighbours. One axis; drop 0 (or a span that does not fit): the rectangle itself, unchanged.
struct DrsSpan {
    std::uint32_t src{};   // the cut-out's first render pixel (the crop's source origin)
    std::uint32_t base{};  // the upscaler's sub-rectangle in the cut-out: its first pixel
    std::uint32_t count{}; // and its pixels
    bool operator==(const DrsSpan&) const = default;
};
inline DrsSpan Span(std::uint32_t render, std::uint32_t start, std::uint32_t size,
                    std::uint32_t drop) {
    if (drop == 0 || drop >= 100 || size == 0 || size > render || start > render - size)
        return {start, 0, size};
    const std::uint32_t base = size * drop / 200; // the bridge's own integer inset
    const std::uint32_t count = size - 2 * base;
    const double p = double(100 - drop) / 100.0;
    // Where the rectangle's first pixel was drawn, with the inset's rounding (its sub-rectangle is
    // up to two pixels wider than p * size) shared between both sides.
    const double first = double(render) * double(drop) / 200.0 + p * double(start) +
                         (p * double(size) - double(count)) / 2.0;
    // Kept inside the view's integer extent (S1Drs::Margin's), which this frame's motion vectors
    // cover (S1Drs::MotionGroups): a rectangle on the eye's edge could otherwise round one column
    // outside it, onto vectors left from an earlier frame (2026-11-24 review). A pixel at most.
    const long view_first = long(render * drop / 200);
    const long last_start = long(render) - view_first - long(count);
    long sub_first = std::lround(first);
    if (view_first <= last_start)
        sub_first = std::clamp(sub_first, view_first, last_start);
    const long src = sub_first - long(base);
    return {std::uint32_t(std::clamp<long>(src, 0, long(render - size))), base, count};
}
struct DrsWindow {
    DrsSpan x, y;
    bool operator==(const DrsWindow&) const = default;
};
inline DrsWindow Window(std::uint32_t render_w, std::uint32_t render_h, const Rect& r,
                        std::uint32_t drop) {
    return {Span(render_w, r.in_x, r.in_w, drop), Span(render_h, r.in_y, r.in_h, drop)};
}

} // namespace Vulkan::DlssFocus
