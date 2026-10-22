// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <cmath>
#include <cstdlib>
#include <string_view>

#include "core/vr/live_tune.h"
#include "core/vr/quick_items.h"

// Opt-in inspection of the production held-item paths. No inventory request, use, throw or
// equip is issued. VR_GRIP_PREVIEW=1 and VR_TUNE_FILE: preview.goods selects QuickUsableGoods.
// A missing/invalid selection leaves the hand empty. Intended for an isolated test session.
namespace VR::GripPreview {
inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("VR_GRIP_PREVIEW");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled;
}
inline int Goods() {
    if (!Enabled()) return -1;
    const float value = Tune::Get("preview.goods", -1.0f);
    if (!std::isfinite(value)) return -1;
    for (const int goods : Quick::QuickUsableGoods) {
        if (value == static_cast<float>(goods)) return goods;
    }
    return -1;
}
inline Quick::Output Display(int goods, std::int64_t now) {
    Quick::Output out{};
    out.block_l2 = true;
    out.tap_taken = true;
    out.disarm_vial = true;
    if (goods >= 0 && goods != 1000) {
        out.held_goods = goods;
        out.scene.state = Quick::SceneState::Holding;
        out.scene.state_ns = now;
        out.scene.held_goods = goods;
        out.crank_box = goods == 700;
    }
    return out;
}
} // namespace VR::GripPreview
