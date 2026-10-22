// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <array>
#include <cstdint>
namespace ScratchRegistry {
// Caller serializes access and changes generation on every registry mutation.
// Cache values, including misses, never survive a generation change.
template<class Value> class LookupCache {
    struct Entry { std::uint64_t key{}, generation{}; Value value{}; bool valid{}; };
    std::array<Entry, 256> entries{};
public:
    template<class Resolve> Value Get(std::uint64_t key, std::uint64_t generation,
                                      Resolve&& resolve) {
        const auto mixed = (key ^ (key >> 32)) * 0x9e3779b97f4a7c15ULL;
        auto& entry = entries[mixed >> 56];
        if (!entry.valid || entry.key != key || entry.generation != generation) {
            entry = {key, generation, resolve(), true};
        }
        return entry.value;
    }
};
}
