// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <cstddef>
#include <cstdint>

namespace ScratchStream {
// cursor counts live entries; arrays retain stale slots after a ring wrap.
template <class Watches>
void CommitWatch(Watches& watches, std::size_t& cursor, std::uint64_t tick,
                 std::uint64_t upper_bound, bool merge, std::size_t grow) {
    if (cursor != 0) {
        auto& previous = watches[cursor - (merge ? 1 : 0)];
        if (previous.tick == tick) {
            previous.upper_bound = upper_bound;
            return;
        }
    }
    if (cursor + 1 >= watches.size()) watches.resize(watches.size() + grow);
    auto& watch = watches[cursor++];
    watch.upper_bound = upper_bound;
    watch.tick = tick;
}
}
