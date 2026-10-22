// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <cstdint>
#include <limits>
#include <span>
namespace ScratchBatch {
struct Range { std::uint64_t address; std::uint32_t size; };
inline bool Aligned(std::uint64_t value, std::uint64_t alignment, std::uint64_t& result) {
    if (!alignment || (alignment & (alignment - 1)) != 0 ||
        value > std::numeric_limits<std::uint64_t>::max() - (alignment - 1)) return false;
    result = (value + alignment - 1) & ~(alignment - 1);
    return true;
}
inline bool Plan(std::span<const Range> ranges, std::uint64_t alignment,
                 std::span<std::uint32_t> offsets, std::uint64_t& total) {
    total = 0;
    if (ranges.empty() || offsets.size() < ranges.size()) return false;
    for (std::size_t i=0; i<ranges.size(); ++i) {
        if (!ranges[i].size || !Aligned(total, alignment, total) ||
            total > std::numeric_limits<std::uint32_t>::max() - ranges[i].size) return false;
        offsets[i] = static_cast<std::uint32_t>(total);
        total += ranges[i].size;
    }
    return true;
}
inline bool FitsWithoutWait(std::uint64_t cursor, std::uint64_t size,
                            std::uint64_t alignment, std::uint64_t capacity,
                            bool has_pending, std::uint64_t wait_bound) {
    std::uint64_t start;
    if (!Aligned(cursor, alignment, start) || start > capacity || size > capacity-start)
        return false;
    return !has_pending || start+size <= wait_bound;
}
// Read-only preflight: no watch cursor mutation, waiting, or callbacks.
template <class Watches, class IsFree>
bool RetiredThrough(const Watches& watches, std::size_t cursor, std::size_t end,
                    std::uint64_t bound, std::uint64_t requested, IsFree&& is_free) {
    if (cursor > end || end > watches.size()) return false;
    while (requested > bound && cursor < end) {
        const auto& watch = watches[cursor++];
        if (!is_free(watch.tick)) return false;
        bound = watch.upper_bound;
    }
    return requested <= bound;
}

}
