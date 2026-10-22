// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace VR::TestStats {
inline constexpr std::array<std::uint32_t, 6> Offsets{0x40, 0x48, 0x58, 0x60, 0x68, 0x70};
inline constexpr std::uint32_t LevelOffset = 0x90;
inline constexpr std::int32_t Maximum = 99;
inline constexpr std::int32_t MaximumLevel = 6 * Maximum - 50;
using Stats = std::array<std::int32_t, 6>;

// Bloodborne's origins all satisfy level = sum of the six attributes - 50.
// Refuse an unrecognised/transient layout instead of overwriting it.
inline bool Valid(const Stats &stats, std::int32_t level) {
    if (level < 4 || level > MaximumLevel)
        return false;
    std::int32_t sum = 0;
    for (const auto value : stats) {
        if (value < 1 || value > Maximum)
            return false;
        sum += value;
    }
    return level == sum - 50;
}
inline bool Maxed(const Stats &stats, std::int32_t level) {
    if (level != MaximumLevel)
        return false;
    for (const auto value : stats)
        if (value != Maximum)
            return false;
    return true;
}
// The game's level-up request handler at RVA 0x1C13D00 reads only +8 through +0x20.
// Its leading vtable and trailing padding are not used by this direct call.
struct Request {
    std::uint64_t unused_vtable{};
    std::int32_t level{MaximumLevel};
    Stats stats{Maximum, Maximum, Maximum, Maximum, Maximum, Maximum};
    std::uint32_t padding{};
};
static_assert(offsetof(Request, level) == 0x08);
static_assert(offsetof(Request, stats) == 0x0C);
static_assert(sizeof(Request) == 0x28);
} // namespace VR::TestStats
