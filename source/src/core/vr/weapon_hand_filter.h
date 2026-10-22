// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include "core/vr/hand_tremor_filter.h"
#include "core/vr/whip_hand_filter.h"

namespace VR::Body::Tremor {
inline bool FlexibleWhip(int family, bool form_b) {
    return form_b && (family == 220 || family == 240);
}

// Only one filter runs per hand. Changing form resets its history so a rigid
// weapon's settling tail cannot become an unintended whip stroke (or vice versa).
class WeaponFilter {
public:
    void Step(std::int64_t time, TrackedHand& hand, std::uint32_t space,
              const Config& config, bool whip) {
        if (whip != whip_) {
            Clear();
            whip_ = whip;
        }
        if (whip) legacy_.Step(time, hand, space);
        else current_.Step(time, hand, space, config);
    }
    void Clear() { current_.Clear(); legacy_.Clear(); }
private:
    Filter current_;
    WhipTremor::Filter legacy_;
    bool whip_{};
};
} // namespace VR::Body::Tremor
