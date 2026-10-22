// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <bit>
#include <cmath>
#include <cstdint>

namespace Vulkan {

/// FAST_CPU_TAN_MEMO (Rasterizer::DrawRecord / DrawIndirectRecord, native stereo): every world
/// draw's eye shift divides by std::tan(0.5f * vfov) of its camera record, and the vfov changes
/// per camera update, not per draw. The last vfov's tangent, keyed by the exact bits of the vfov
/// (so -0.0f, +0.0f and each NaN payload are separate keys): the same float the expression gives,
/// from the same expression, computed once per change. Owned by one thread.
struct HalfTanMemo {
    std::uint32_t bits{};
    float value{};
    bool valid{};

    /// std::tan(0.5f * vfov), bit for bit.
    float operator()(float vfov) {
        const std::uint32_t key = std::bit_cast<std::uint32_t>(vfov);
        if (!valid || key != bits) [[unlikely]] {
            value = Compute(vfov);
            bits = key;
            valid = true;
        }
        return value;
    }
    /// The expression the draws used before (and FAST_CPU_TAN_MEMO=0 / _VERIFY use).
    static float Compute(float vfov) {
        return std::tan(.5f * vfov);
    }
};

} // namespace Vulkan
