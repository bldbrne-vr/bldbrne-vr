// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The headset swapchains' sizes against the runtime's two size limits. Header-only
// (tests/test_xr_size.cpp); used by OpenXRPresenter (EnsureSwapchains, EnsureDibrPair, the DIBR
// eye check) and Presenter::RecordVrStereo.
//
// - The largest swapchain IMAGE (XrSystemGraphicsProperties::maxSwapchainImageWidth/Height;
//   Meta Link 4096x4096) bounds every swapchain we create.
// - The largest image RECT of a view (XrViewConfigurationView::maxImageRectWidth/Height; Meta
//   Link at the 1x slider 2848x3136, twice its recommended 1424x1568) bounds what ONE eye submits
//   as its imageRect.
//
// The centre image (mono frames, AER, the menu panel, CopyStereoFrame) is the whole render, and
// each eye submits a crop of it: only the crops are image rects. Until 2026-10-07 the whole render
// was fitted into one eye's largest rect, so on Meta Link a 3200-wide render was scaled to
// 2848x1893, and DIBR, which required that swapchain at the render's size, refused every frame:
// mono to both eyes (no stereo depth: hands and weapons looked huge), no DIBR pair, so no late warp
// (wobble) and no render pitch bias. DIBR eyes never use the centre image's swapchains: each eye
// goes to its own swapchain of a DIBR pair, sized from its crop (and vr.ini eye_size), and is
// checked against both limits here (CheckDibrEyes).

#include <algorithm>
#include <array>
#include <string_view>

#include "common/types.h"

namespace Vulkan::VrXrSize {

struct Size {
    u32 width{};
    u32 height{};
    bool operator==(const Size&) const = default;
};

// The centre image's swapchains for a `frame`-sized render: the frame itself, or (larger than the
// runtime's largest swapchain image `max_swapchain`) scaled down to fit it, aspect kept, at least
// 1 px. Never limited by the per-eye largest image rect: that is checked per crop
// (CropFitsRect), not against the whole frame.
struct FrameFit {
    Size size{};
    bool scaled{false};
};

inline FrameFit CentreSwapchain(Size frame, Size max_swapchain) {
    FrameFit fit{frame, false};
    if (frame.width == 0 || frame.height == 0) {
        return fit;
    }
    const u32 max_w = std::max<u32>(max_swapchain.width, 1);
    const u32 max_h = std::max<u32>(max_swapchain.height, 1);
    if (frame.width <= max_w && frame.height <= max_h) {
        return fit;
    }
    const double scale = std::min(static_cast<double>(max_w) / frame.width,
                                  static_cast<double>(max_h) / frame.height);
    fit.size = {std::clamp<u32>(static_cast<u32>(frame.width * scale), 1, max_w),
                std::clamp<u32>(static_cast<u32>(frame.height * scale), 1, max_h)};
    fit.scaled = true;
    return fit;
}

// One crop (an eye's imageRect) within the runtime's largest image rect.
inline bool CropFitsRect(Size crop, Size max_rect) {
    return crop.width <= max_rect.width && crop.height <= max_rect.height;
}

// Why DIBR eyes cannot go to the headset at their size (CheckDibrEyes).
enum class EyeRefusal : u8 {
    None,
    Empty,     // an eye of 0 pixels (no crop)
    ImageRect, // an eye larger than the runtime's largest image rect (its whole image is its rect)
    Swapchain, // an eye larger than the runtime's largest swapchain image
};

inline std::string_view RefusalText(EyeRefusal refusal) {
    switch (refusal) {
    case EyeRefusal::Empty:
        return "an eye crop is empty";
    case EyeRefusal::ImageRect:
        return "an eye is larger than the runtime's largest eye image rect";
    case EyeRefusal::Swapchain:
        return "an eye is larger than the runtime's largest swapchain image";
    default:
        return "";
    }
}

// DIBR eyes `eyes` (each its crop of the render, or vr.ini eye_size's enlargement of it): each goes
// to its own swapchain of a DIBR pair and is submitted whole as its view's imageRect, so each must
// fit the largest image rect `max_rect` and the largest swapchain `max_swapchain`. The render's own
// size plays no part (the capture stays inside the emulator).
inline EyeRefusal CheckDibrEyes(const std::array<Size, 2>& eyes, Size max_swapchain,
                                Size max_rect) {
    for (const Size& eye : eyes) {
        if (eye.width == 0 || eye.height == 0) {
            return EyeRefusal::Empty;
        }
    }
    for (const Size& eye : eyes) {
        if (eye.width > max_swapchain.width || eye.height > max_swapchain.height) {
            return EyeRefusal::Swapchain;
        }
    }
    for (const Size& eye : eyes) {
        if (!CropFitsRect(eye, max_rect)) {
            return EyeRefusal::ImageRect;
        }
    }
    return EyeRefusal::None;
}

// A DIBR pair's swapchain size for eyes up to `need`: rounded up to 64 px (small crop changes keep
// the swapchains), never past the largest swapchain (`need` itself must fit: CheckDibrEyes).
inline Size DibrAlloc(Size need, Size max_swapchain) {
    const auto up = [](u32 v, u32 limit) {
        return std::max(v, std::min((v + 63u) & ~63u, limit));
    };
    return {up(need.width, max_swapchain.width), up(need.height, max_swapchain.height)};
}

} // namespace Vulkan::VrXrSize
