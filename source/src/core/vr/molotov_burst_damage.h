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

#include "core/vr/quick_settings.h"

namespace VR::Quick::BurstDamage {

inline constexpr float Multiplier = 6.0f;

inline bool Enabled(const QuickSettings& s) {
    // Custom bullets (including the Molotov's own child) can also be used by ground fire.
    // Only the default thrown Cannon explosion has a distinct damage-source combination.
    return s.molotov_burst && s.molotov_burst_bullet == MolotovBurstBullet;
}

struct Hit {
    int bullet{-1};
    int behavior{-1};
    int hand{-1};
    bool player_attack{false};
    bool self_hit{false};
};

inline float Scale(float damage, const Hit& hit, const QuickSettings& s) {
    // The existing damage hook identifies thrown items by behavior == -1, hand == -1.
    // A real Cannon shot has a weapon behavior; bullet 701 alone is NOT sufficient.
    if (!Enabled(s) || !hit.player_attack || hit.self_hit || hit.bullet != MolotovBurstBullet ||
        hit.behavior != -1 || hit.hand != -1 || !std::isfinite(damage) || damage <= 0.0f) {
        return damage;
    }
    // Match the damage hook's safe range before the game's signed-integer conversion.
    return static_cast<float>(std::min(static_cast<double>(damage) * Multiplier, 2.0e9));
}

} // namespace VR::Quick::BurstDamage
