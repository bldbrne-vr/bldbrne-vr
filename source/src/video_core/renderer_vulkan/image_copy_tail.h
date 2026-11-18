// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <limits>
#include <optional>

namespace Vulkan {

// bbport's image-plus-padding copy: only complete dword dispatches, at most 64 KiB
// past the image, with disjoint, non-wrapping guest ranges. Exact-size copies keep
// their existing path. The caller must also validate the shader and image layout.
constexpr std::optional<std::uint32_t> ImageCopyTailBytes(std::uint64_t source,
                                                          std::uint64_t destination,
                                                          std::uint32_t image_bytes,
                                                          std::uint32_t copy_bytes) {
    constexpr std::uint32_t MaxTail = 64 * 1024;
    if (!source || !destination || !image_bytes || copy_bytes <= image_bytes ||
        copy_bytes - image_bytes > MaxTail || (copy_bytes % 256) != 0 ||
        ((source | destination | image_bytes) & 3) != 0) {
        return std::nullopt;
    }
    constexpr auto MaxAddress = std::numeric_limits<std::uint64_t>::max();
    if (source > MaxAddress - copy_bytes || destination > MaxAddress - copy_bytes ||
        (source < destination + copy_bytes && destination < source + copy_bytes)) {
        return std::nullopt;
    }
    return copy_bytes - image_bytes;
}

} // namespace Vulkan
