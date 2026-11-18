// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include "common/types.h"

// Tests only (VR_BUMP_PROBE=<folder>): the raw texture bytes of the plain bump materials' bump
// map and of the three parallax programs' textures (the control: their blue is the height they
// walk), written once per texture as the game stores them (2026-11-11, the user doubting "blue
// is flat": the earlier test sampled through the T#'s swizzle and lit the result; the bytes
// show what the texture really holds, every channel, alpha too). BumpHeightPass notes which
// image is a program's bump map; vk_rasterizer.cpp writes the files at bind time.
namespace Shader::Optimization::BumpProbe {

inline const char* Dir() {
    static const char* const dir = [] {
        const char* const env = std::getenv("VR_BUMP_PROBE");
        return env != nullptr && env[0] != '\0' ? env : nullptr;
    }();
    return dir;
}

namespace Detail {
inline std::mutex mutex;
inline std::unordered_map<u64, u32> bumps;
} // namespace Detail

inline constexpr u32 NotProbed = ~0u;
inline constexpr u32 AllImages = ~1u;

inline void Note(u64 hash, u32 image) {
    std::scoped_lock lock{Detail::mutex};
    Detail::bumps[hash] = image;
}

// For a program: its bump map's image (every image is written, that one marked), AllImages for
// the parallax programs, else NotProbed.
inline u32 Find(u64 hash) {
    if (hash == 0x053bbf3bu || hash == 0x505acc58u || hash == 0x08ccd4c7u) {
        return AllImages;
    }
    std::scoped_lock lock{Detail::mutex};
    const auto it = Detail::bumps.find(hash);
    return it == Detail::bumps.end() ? NotProbed : it->second;
}

} // namespace Shader::Optimization::BumpProbe
