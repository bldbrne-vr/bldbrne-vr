// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace VideoCore {
// Exact small comparisons. memcpy permits unaligned pointers; overlapping head/tail
// reads stay inside the span, including at the end of a page.
inline bool KeptBytesDiffer(const unsigned char* a, const unsigned char* b,
                            std::uint64_t size) {
    if (size == 1) return *a != *b;
    if (size > 32) return std::memcmp(a, b, size) != 0;
    if (size > 8 && size <= 32) {
        std::uint64_t ah, bh, at, bt;
        std::memcpy(&ah, a, 8); std::memcpy(&bh, b, 8);
        std::memcpy(&at, a + size - 8, 8); std::memcpy(&bt, b + size - 8, 8);
        if (ah != bh || at != bt) return true;
        if (size > 16) {
            std::memcpy(&ah, a + 8, 8); std::memcpy(&bh, b + 8, 8);
            std::memcpy(&at, a + size - 16, 8); std::memcpy(&bt, b + size - 16, 8);
            return ah != bh || at != bt;
        }
        return false;
    }
    if (size >= 4 && size <= 8) {
        std::uint32_t ah, bh, at, bt;
        std::memcpy(&ah, a, 4); std::memcpy(&bh, b, 4);
        std::memcpy(&at, a + size - 4, 4); std::memcpy(&bt, b + size - 4, 4);
        return ah != bh || at != bt;
    }
    if (size == 2 || size == 3) {
        std::uint16_t ah, bh;
        std::memcpy(&ah, a, 2); std::memcpy(&bh, b, 2);
        return ah != bh || a[size - 1] != b[size - 1];
    }
    return std::memcmp(a, b, size) != 0;
}
// Ranges must be ordered and disjoint (RangeSet::ForEachInRange). Callbacks must
// not mutate that set. Stream them: fragmentation must never disable preservation.
template <class Ranges, class Emit, class Lost>
std::uint64_t EmitKeptPageUpload(std::uint64_t page, std::uint64_t page_size,
                               const void* snapshot, const void* current,
                               std::uint64_t write_lo, std::uint64_t write_hi,
                               std::uint64_t lo, std::uint64_t hi,
                               Ranges&& ranges, Emit&& emit, Lost&& lost) {
    const auto* saved = static_cast<const unsigned char*>(snapshot);
    const auto* guest = static_cast<const unsigned char*>(current);
    // Delay uploads until a preserved range: adjacent CPU-owned spans can share
    // one copy command. Never merge across bytes that must stay on the GPU.
    std::uint64_t pending = lo, preserved = 0;
    ranges(page, page_size, [&](std::uint64_t first, std::uint64_t last) {
        const auto start = std::clamp(first, lo, hi);
        const auto stop = std::clamp(last, lo, hi);
        if (start >= stop) return;
        const bool touched = write_hi > write_lo && write_lo < stop && write_hi > start;
        const auto offset = start - page;
        const auto length = stop - start;
        // Tiny fragmented spans should not pay for an out-of-line memcmp call.
        // Both paths compare exactly the range, without reading outside it.
        const bool changed = !touched && KeptBytesDiffer(guest + offset, saved + offset, length);
        if (touched || changed) {
            lost(start, stop, touched);
        } else {
            if (pending < start) emit(pending, start - pending);
            pending = stop;
            preserved += length;
        }
    });
    if (pending < hi) emit(pending, hi - pending);
    return preserved;
}
} // namespace VideoCore
