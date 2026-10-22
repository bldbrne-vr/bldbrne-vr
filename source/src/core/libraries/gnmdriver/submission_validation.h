// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <cstdint>

namespace Libraries::GnmDriver {
// Run before patching a flip or allocating its VR ticket. A later invalid DCB/CCB
// must not leave a partially prepared flip behind. Plain zero-count submits stay no-ops.
inline bool ValidCommandBuffers(std::uint32_t count, const std::uint32_t* const* dcbs,
                                const std::uint32_t* sizes, const std::uint32_t* ccb_sizes,
                                bool has_flip = false) {
    if (!dcbs || !sizes || (has_flip && count == 0))
        return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!dcbs[i] || sizes[i] == 0 || sizes[i] > 0x3ffffc || (sizes[i] & 3) != 0 ||
            (ccb_sizes && (ccb_sizes[i] > 0x3ffffc || (ccb_sizes[i] & 3) != 0)))
            return false;
    }
    return !has_flip || sizes[count - 1] >= 64 * sizeof(std::uint32_t);
}
} // namespace Libraries::GnmDriver
