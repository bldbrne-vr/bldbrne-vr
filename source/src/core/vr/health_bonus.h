// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace VR::Assists {

struct HealthChange {
    int current;
    int tracked_max;
};

inline std::optional<HealthChange> PlanHealthChange(int base, float product, float previous,
                                                  float next, int current, int tracked_max) {
    const float before = static_cast<float>(base) * (product * previous);
    const float after = static_cast<float>(base) * (product * next);
    const float limit = static_cast<float>(std::numeric_limits<int>::max());
    if (base <= 0 || current < 0 || tracked_max <= 0 || product <= 0.0f || previous <= 0.0f ||
        next <= 0.0f || !std::isfinite(before) ||
        !std::isfinite(after) || before <= 0.0f || after <= 0.0f || before >= limit ||
        after >= limit) {
        return std::nullopt;
    }
    const int old_max = std::max(1, static_cast<int>(before));
    const int new_max = std::max(1, static_cast<int>(after));
    const auto tracked = static_cast<std::int64_t>(tracked_max) + new_max - old_max;
    return HealthChange{
        std::min(current, new_max),
        static_cast<int>(std::clamp<std::int64_t>(tracked, 0, std::numeric_limits<int>::max()))};
}

}
