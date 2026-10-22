// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <algorithm>
#include <optional>

namespace VR::Lamps {
// Called only after native storage replenishment, never on vial consumption.
// A partial refill (even one vial) is left alone. Unknown counts are not empty.
constexpr int EmptyVialRefillGrant(std::optional<int> carried, std::optional<int> stored,
                                  int room) {
    return carried && stored && *carried == 0 && *stored == 0 && room >= 3 && room <= 99
               ? 3
               : 0;
}
struct VialRestPlan {
    int take;
    int grant;
    int carried_after;
    int stored_after;
};

// Disabling the bonus grant must not disable ordinary storage replenishment or rest.
constexpr std::optional<VialRestPlan> PlanVialRest(std::optional<int> carried,
                                                  std::optional<int> stored, int room,
                                                  bool bonus_enabled) {
    if (!carried || !stored || *carried < 0 || *carried > 999 || *stored < 0 || *stored > 999 ||
        room < 0 || room > 99)
        return {};
    const int take = std::min(room, *stored);
    const int grant = bonus_enabled ? EmptyVialRefillGrant(*carried + take, *stored - take,
                                                         room - take) : 0;
    return VialRestPlan{take, grant, *carried + take + grant, *stored - take};
}
} // namespace VR::Lamps
