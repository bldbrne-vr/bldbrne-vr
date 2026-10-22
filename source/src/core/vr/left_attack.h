// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <cmath>
#include <cstdint>

namespace VR::Melee::LeftAttack {
// Only native torch/Gratia attacks need the weapon-relative dummy mapping. Gun/shield
// punches use the proven body-fist row; dual weapons use their right weapon's part IDs.
constexpr bool NativeWeapon(std::int32_t variation, bool punch) {
    return !punch && (variation == 2000 || variation == 3400);
}
inline thread_local const void* character{};
class Scope {
public:
    explicit Scope(const void* target) : previous(character) { character = target; }
    ~Scope() { character = previous; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    const void* previous;
};
inline bool Applies(const void* target) { return target != nullptr && target == character; }

// Compare a native weapon hit to its actual weapon dummy, even when it lies closer to
// the other hand. Keep the existing 1 m attachment bound; never accept NaN or stale geometry.
inline bool AtWeapon(float hit_to_dummy, float dummy_to_grip) {
    return std::isfinite(hit_to_dummy) && std::isfinite(dummy_to_grip) &&
           hit_to_dummy >= 0.0f && hit_to_dummy <= 0.20f &&
           dummy_to_grip >= 0.0f && dummy_to_grip <= 1.0f;
}
} // namespace VR::Melee::LeftAttack
