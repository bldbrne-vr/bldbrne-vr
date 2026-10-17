// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdlib>
#include <cstring>

#include "common/types.h"

// FAST_GPU_SMAA_TRIM (vr_smaa.cpp): the pure parts, for tests/test_smaa_trim.cpp. The switch's
// value is a mask of two trims of SMAA's GPU passes; FAST_GPU_SMAA_TRIM_VERIFY compares a trimmed
// frame's A with the full passes' A, byte for byte, split into colour and alpha differences.
namespace Vulkan::SmaaTrim {

/// 1: the blend pass leaves A untouched where no weight applies (vr_smaa.comp SMAA_KEEP).
constexpr u32 Keep = 1u;
/// 2: the weights pass with its edge pixels packed into the first warps of 16x16 groups
/// (vr_smaa.comp SMAA_WEIGHTS + SMAA_COMPACT).
constexpr u32 Compact = 2u;
/// SMAA_COMPACT's local size.
constexpr u32 CompactGroup = 16u;

/// FAST_GPU_SMAA_TRIM's value: decimal or 0x hex; unset, empty or 0: no trim. Bits other than
/// Keep and Compact are ignored.
[[nodiscard]] inline u32 ParseMask(const char* value) noexcept {
    if (value == nullptr || value[0] == '\0') {
        return 0;
    }
    return static_cast<u32>(std::strtoul(value, nullptr, 0)) & (Keep | Compact);
}

/// FAST_GPU_SMAA_TRIM_VERIFY's value: check every Nth trimmed frame (1: every one); unset or 0:
/// never.
[[nodiscard]] inline u64 ParseEvery(const char* value) noexcept {
    if (value == nullptr || value[0] == '\0') {
        return 0;
    }
    return static_cast<u64>(std::strtoull(value, nullptr, 0));
}

/// Two copies of A, 4 bytes per texel with alpha in the last byte (every format SMAA takes:
/// R8G8B8A8, B8G8R8A8, A8B8G8R8 packed, UNORM or sRGB).
struct TexelDiff {
    u64 color{};        // texels whose first, second or third byte differs (alpha may too)
    u64 alpha_only{};   // texels whose alpha byte alone differs
    u64 first{~u64{0}}; // the first differing texel's index; ~0: none
    std::array<u8, 4> first_fast{}; // ... its bytes in `fast` and in `ref`
    std::array<u8, 4> first_ref{};
};

[[nodiscard]] inline TexelDiff CountTexelDiffs(const u8* fast, const u8* ref, u64 texels) noexcept {
    TexelDiff d{};
    for (u64 i = 0; i < texels; ++i) {
        const u8* f = fast + i * 4;
        const u8* r = ref + i * 4;
        if (std::memcmp(f, r, 4) == 0) {
            continue;
        }
        if (f[0] != r[0] || f[1] != r[1] || f[2] != r[2]) {
            ++d.color;
        } else {
            ++d.alpha_only;
        }
        if (d.first == ~u64{0}) {
            d.first = i;
            std::memcpy(d.first_fast.data(), f, 4);
            std::memcpy(d.first_ref.data(), r, 4);
        }
    }
    return d;
}

} // namespace Vulkan::SmaaTrim
