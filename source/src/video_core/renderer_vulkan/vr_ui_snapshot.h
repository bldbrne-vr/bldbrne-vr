// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Vulkan::UiSnapshot {

// Source coordinates, end exclusive. Also the valid region supplied to the resolve;
// allocation capacity may be larger after a previous menu's snapshot.
struct Rect {
    std::int32_t x0{}, y0{}, x1{}, y1{};
    constexpr bool Empty() const { return x1 <= x0 || y1 <= y0; }
    constexpr std::uint32_t Width() const { return x1 - x0; }
    constexpr std::uint32_t Height() const { return y1 - y0; }
};

constexpr Rect Clip(std::uint32_t image_width, std::uint32_t image_height,
                    std::int32_t x, std::int32_t y, std::uint32_t width,
                    std::uint32_t height) {
    // Widen before adding: malformed/empty input must not wrap into a large allocation.
    const auto x0 = std::clamp<std::int64_t>(x, 0, image_width);
    const auto y0 = std::clamp<std::int64_t>(y, 0, image_height);
    const auto x1 = std::clamp<std::int64_t>(std::int64_t{x} + width, x0, image_width);
    const auto y1 = std::clamp<std::int64_t>(std::int64_t{y} + height, y0, image_height);
    return {static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0),
            static_cast<std::int32_t>(x1), static_cast<std::int32_t>(y1)};
}

// The rasterizer snapshots the status rows in the centred 16:9 UI band. Use the
// same bounds for initial allocation so opening a menu does not allocate images.
inline Rect MenuRows(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) {
        return {};
    }
    auto band_height = height;
    if (static_cast<float>(width) / height < 16.0f / 9.0f - 0.02f) {
        band_height = static_cast<std::uint32_t>(std::lround(width * 9.0f / 16.0f));
    }
    const auto y = (height - band_height) / 2;
    const auto rows = std::min(band_height, band_height * 260 / 1080 + 16);
    return Clip(width, height, 0, y, width, rows);
}

} // namespace Vulkan::UiSnapshot
