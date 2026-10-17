// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The submitted layers on a runtime without XR_KHR_composition_layer_color_scale_bias (SteamVR's),
// the submitted layers' kinds for the log, and XrTime against the steady clock. Header-only
// (tests/test_xr_layers_policy.cpp); used by OpenXRPresenter (Submit, BuildUiLayers,
// UpdateUiPlacement, MeasureXrClock).
//
// The base layer. The presenter took "no layer" for black: Meta's runtime shows black wherever no
// layer covers the view, so without the colour scale extension a fade dropped the layers below
// half brightness (a cut through black), and the virtual screen, a menu panel without the world
// around it and the time before the first frame showed black around them. SteamVR has no colour
// scale extension and shows its own light-grey void there: the user's Quest 3 through SteamVR
// (2026-10-15) had 0.3-1.4 s of an empty light view at every flat/VR switch, and the virtual screen
// floated in it. The base layer is one projection layer of a black 16x16 image (cleared once and
// released; the runtime keeps showing the last image released), submitted first whenever no eye
// image goes out, so what is not covered is black again. On a runtime with the colour scale
// extension (Meta's, the mock) it is never made: their frames are exactly as before.
//
// The HUD without the colour scale extension: a hand, an arm or the weapon in front of a HUD piece
// faded it (ui_hand_fade); with no colour scale to fade with, the check was skipped and the far HUD
// was drawn whole through the near hands (two depths at once). Now the check runs and a piece
// whose alpha would be below half is left out (the cut the brightness rule makes for a fade).
//
// XrTime. The presenter compares XrTime with the steady clock (the display time's lead, the shown
// image's age, the late warp's motion): on Meta's runtime XrTime is the steady clock (QPC
// nanoseconds). XR_KHR_win32_convert_performance_counter_time gives the runtime's time for a QPC
// value: the offset is measured once, and a runtime whose XrTime is 1 ms or more off gets it
// applied (never Meta's, whose times are as before).

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include "common/types.h"
#include "video_core/renderer_vulkan/vr_xr_queue_policy.h"

namespace Vulkan::VrXrLayers {

// VR_XR_BASE_LAYER (`env`, nullptr when unset): off (EnvOff: "0", "off", "false", "no") never, on
// (EnvOn: "1", "on", "true", "yes") on every runtime (a test on Meta's or the mock), unset or any
// other value only on a runtime without the colour scale extension.
struct BaseLayer {
    bool on{false};
    std::string_view reason{};
};

constexpr BaseLayer ChooseBaseLayer(const char* env, bool has_color_scale) {
    if (VrXrQueuePolicy::EnvOff(env)) {
        return {false, "VR_XR_BASE_LAYER is off"};
    }
    if (VrXrQueuePolicy::EnvOn(env)) {
        return {true, "VR_XR_BASE_LAYER"};
    }
    if (has_color_scale) {
        return {false, "the runtime has the colour scale extension (Meta's): the layers as before"};
    }
    return {true, "no colour scale extension (SteamVR): the runtime's own void would show where "
                  "no layer covers the view"};
}

// This frame gets the base layer under its layers: it is made (`ready`), the frame is rendered
// and no eye image goes out (the eyes' projection covers the whole view by itself).
constexpr bool BaseUnder(bool ready, bool should_render, bool eye_projection) {
    return ready && should_render && !eye_projection;
}

// After the GPU device was lost (VrDeviceLost) nothing new reaches the swapchains: with the base
// layer made, the frame is the base layer alone (black), not the last images over and over.
constexpr bool BaseAlone(bool ready, bool device_lost) {
    return ready && device_lost;
}

// xrEndFrame rejecting the frames with the base layer (Submit; the overlay's probe alike): not yet
// seen on a runtime (a 16x16 projection, views wider than the eyes', an image released once). A
// frame with it rejected (not a loss, not the session's state) and the next frame that would have
// it goes without it (`probe`); that one accepted twice in a row (no accepted base frame in
// between) and the base layer is off for the session; rejected too, the base layer was not the
// cause.
struct BaseProbe {
    enum class Verdict {
        NotCause, // the frame without it was rejected too
        Again,    // accepted without it once: the base layer is tried again
        Off,      // accepted without it twice: off for the session
    };
    bool probe{false}; // this frame goes without the base layer
    u32 strikes{0};

    // A frame with the base layer was rejected: the next that would have it goes without it.
    constexpr void Rejected() {
        probe = true;
    }
    // A frame with the base layer was accepted.
    constexpr void Accepted() {
        strikes = 0;
    }
    // The probe frame's (without the base layer) xrEndFrame.
    constexpr Verdict Probed(bool accepted) {
        probe = false;
        if (!accepted) {
            strikes = 0;
            return Verdict::NotCause;
        }
        return ++strikes >= 2 ? Verdict::Off : Verdict::Again;
    }
};

// `base` first, under the `count` layers already in `layers` (each moved one place up). False,
// nothing changed, when the array is full.
template <typename T, std::size_t N>
constexpr bool InsertFirst(std::array<const T*, N>& layers, u32& count, const T* base) {
    if (count >= N) {
        return false;
    }
    for (u32 i = count; i > 0; --i) {
        layers[i] = layers[i - 1];
    }
    layers[0] = base;
    ++count;
    return true;
}

// An eye's field of view (radians, as XrFovf: left and down negative). The base layer's views get
// a margin on every side (a black image: nothing to see stretched), so the runtime's reprojection
// of a frame never shows its edge; each half angle stays within MaxHalfAngle.
struct Fov {
    float left{};
    float right{};
    float up{};
    float down{};
};

inline constexpr float BaseFovMarginRad = 0.0873f; // 5 degrees
inline constexpr float MaxHalfAngleRad = 1.4835f;  // 85 degrees

constexpr Fov WidenFov(const Fov& f, float margin = BaseFovMarginRad) {
    return {std::clamp(f.left - margin, -MaxHalfAngleRad, MaxHalfAngleRad),
            std::clamp(f.right + margin, -MaxHalfAngleRad, MaxHalfAngleRad),
            std::clamp(f.up + margin, -MaxHalfAngleRad, MaxHalfAngleRad),
            std::clamp(f.down - margin, -MaxHalfAngleRad, MaxHalfAngleRad)};
}

// The kinds of layer one frame submitted (Submit, the `VR layers:` line when they change).
enum Kind : u32 {
    Base = 1u << 0,       // the black base layer
    Projection = 1u << 1, // the eye images (also the frozen world around a menu panel)
    Screen = 1u << 2,     // the virtual screen (a quad)
    Panel = 1u << 3,      // the menu panel (a quad or a cylinder)
    Ui = 1u << 4,         // the game's UI on its own layers
    Page = 1u << 5,       // the VR settings page
    Overlay = 1u << 6,    // the controllers, the vial, the laser
};

// "base+screen+overlay"; "none" for a frame without a layer.
inline std::string KindsText(u32 kinds) {
    static constexpr std::array<std::string_view, 7> names{
        "base", "projection", "screen", "panel", "ui", "page", "overlay"};
    std::string text;
    for (u32 i = 0; i < names.size(); ++i) {
        if ((kinds & (1u << i)) != 0) {
            if (!text.empty()) {
                text += '+';
            }
            text += names[i];
        }
    }
    return text.empty() ? std::string{"none"} : text;
}

// The HUD's hand check (UpdateUiPlacement): with the colour scale extension it runs when the HUD
// fades (ui_fade_ms, as before); without it always, its pieces then cut (UiCut).
constexpr bool UiHandCheck(bool fade, bool has_color_scale) {
    return fade || !has_color_scale;
}

// A UI layer group (BuildUiLayers) at `alpha` is left out: without the colour scale extension
// nothing can draw it partly, so below half it is cut (as a fade's brightness cuts every layer).
constexpr bool UiCut(bool has_color_scale, float alpha) {
    return !has_color_scale && alpha < 0.5f;
}

// One reading of both clocks: the steady clock (ns) before and after the runtime's time (XrTime,
// ns) for one QPC value read between them.
struct ClockSample {
    s64 steady_before{};
    s64 steady_after{};
    s64 xr{};
};

// XrTime minus the steady clock, from the sample read in the shortest time (its midpoint).
template <std::size_t N>
constexpr s64 ClockOffset(const std::array<ClockSample, N>& samples) {
    static_assert(N > 0);
    const ClockSample* best = &samples[0];
    for (const ClockSample& s : samples) {
        if (s.steady_after - s.steady_before < best->steady_after - best->steady_before) {
            best = &s;
        }
    }
    return best->xr - (best->steady_before + (best->steady_after - best->steady_before) / 2);
}

inline constexpr s64 ClockOffsetMinNs = 1'000'000; // 1 ms

// Whether the measured offset is applied to the presenter's XrTime <-> steady clock conversions:
// 1 ms or more, on any runtime but Meta's (XrTime is the steady clock there; its frames stay as
// they were).
constexpr bool ApplyClockOffset(s64 offset_ns, VrXrQueuePolicy::Runtime runtime) {
    const s64 size = offset_ns < 0 ? -offset_ns : offset_ns;
    return size >= ClockOffsetMinNs && runtime != VrXrQueuePolicy::Runtime::Meta;
}

} // namespace Vulkan::VrXrLayers
