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
#include <optional>

namespace VR {
// Usually consumes the head in O(1). Overlapping submissions may arrive out of
// ticket order; remove only the matched entry, retaining the other pending flips.
template <typename Entry, std::size_t N>
std::optional<Entry> TakeFlipTicket(std::array<Entry, N>& entries, std::size_t& head,
                                    std::size_t& count, std::uint64_t ticket) {
    if (ticket == 0)
        return std::nullopt;
    for (std::size_t i = 0; i < count; ++i) {
        const auto entry = entries[(head + i) % N];
        if (entry.ticket != ticket)
            continue;
        if (i == 0) {
            head = (head + 1) % N;
        } else {
            for (std::size_t j = i; j + 1 < count; ++j)
                entries[(head + j) % N] = entries[(head + j + 1) % N];
        }
        --count;
        return entry;
    }
    return std::nullopt;
}
} // namespace VR
