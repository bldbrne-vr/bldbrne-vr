// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>

namespace VR::OutgoingDamage {
inline std::atomic<int> live_percent{0};
inline int Clamp(float percent) {
    return std::isfinite(percent) ? static_cast<int>(std::lround(std::clamp(percent, -30.f, 30.f))) : 0;
}
inline float Scale(float damage, int percent, bool player_attacker, bool other_target) {
    if (!player_attacker || !other_target || percent == 0 ||
        !std::isfinite(damage) || damage <= 0.f) return damage;
    return std::min(damage * (1.f + std::clamp(percent, -30, 30) * .01f), 2.0e9f);
}
}
