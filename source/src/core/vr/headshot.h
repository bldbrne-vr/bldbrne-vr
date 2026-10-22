// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace VR::Headshot {
enum class Region { Unknown, Other, Head };
struct Part {
    Region region{Region::Unknown};
    std::array<char, 64> name{};
};
// The tester's rule: ASCII case-insensitive substring, applied to the HIT collision bone.
inline bool HeadName(std::string_view name) {
    constexpr std::string_view word = "head";
    for (size_t i = 0; i + word.size() <= name.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < word.size(); ++j) {
            const char c = name[i + j];
            const char lower = c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
            match &= lower == word[j];
        }
        if (match)
            return true;
    }
    return false;
}
inline bool Eligible(int hand, int bullet, int behavior, float damage) {
    return hand == -2 && bullet >= 0 && behavior >= 0 && std::isfinite(damage) && damage > 0;
}
inline float Scale(float damage, Region region, float multiplier) {
    if (region != Region::Head || !std::isfinite(damage) || damage <= 0 ||
        !std::isfinite(multiplier) || multiplier <= 1)
        return damage;
    return static_cast<float>(std::min(double(damage) * multiplier, 2.0e9));
}
// Retail 1.09: damage+0x1B0 is the collision ragdoll-body index (NOT the visual
// skeleton index). Injected reader must be fault-safe; unavailable data means Unknown.
// No caching: body layouts and pointers can change on enemy transformations / reloads.
template <class Reader>
Part Resolve(std::uint64_t target, int index, Reader&& copy) {
    Part out;
    if (target < 0x10000 || index < 0 || index >= 256)
        return out;
    const auto read = [&](std::uint64_t address, auto& value) {
        return address >= 0x10000 && address < 0x0000800000000000ULL &&
               sizeof(value) <= 0x0000800000000000ULL - address &&
               copy(address, &value, sizeof(value));
    };
    std::uint64_t ctrl{}, owner{}, rag{}, system{}, skeleton{}, bones{}, name{};
    int count{}, bodies{};
    // Enemy skeletons may append nonphysical autoTerminal bones after the bodies.
    // The hit must be within body count; the skeleton must cover every body.
    if (!read(target + 0x58, ctrl) || !read(ctrl + 8, owner) || owner != target ||
        !read(ctrl + 0x20, rag) || !read(rag + 0x10, owner) || owner != target ||
        !read(rag + 0x38, system) || !read(system + 0x28, bodies) || bodies <= index ||
        bodies > 256 || !read(system + 0x40, skeleton) || !read(skeleton + 0x30, count) ||
        count < bodies || count > 256 || !read(skeleton + 0x28, bones) ||
        !read(bones + 16 * std::uint64_t(index), name))
        return out;
    name &= ~std::uint64_t{1}; // hkStringPtr owns-bit
    if (!read(name, out.name)) {
        // A short valid string may end immediately before an unreadable page.
        // Keep the common path to one read; only retry bytewise on failure.
        out.name.fill(0);
        for (size_t i = 0; i < out.name.size(); ++i) {
            if (!read(name + i, out.name[i]))
                return Part{};
            if (out.name[i] == '\0')
                break;
        }
    }
    const auto end = std::find(out.name.begin(), out.name.end(), '\0');
    if (end == out.name.end() || end == out.name.begin())
        return Part{};
    const std::string_view text(out.name.data(), static_cast<size_t>(end - out.name.begin()));
    out.region = HeadName(text) ? Region::Head : Region::Other;
    return out;
}
} // namespace VR::Headshot
