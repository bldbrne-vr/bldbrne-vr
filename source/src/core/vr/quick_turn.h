// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <cmath>
#include "core/vr/space_shift.h"

namespace VR::QuickTurn {
// A short flick keeps next-item on release; a deliberate hold consumes that flick.
struct Gesture {
    enum class Event { None, Item, Turn };
    bool armed{}, pending{};
    float held{};
    void Reset() { *this = {}; }
    Event Step(float x, float y, float dt, bool allowed) {
        if (!allowed || !std::isfinite(dt) || dt < 0 || dt > .1f ||
            !std::isfinite(x) || !std::isfinite(y)) {
            Reset(); return Event::None;
        }
        const bool neutral = std::abs(x) < .35f && std::abs(y) < .35f;
        if (neutral) {
            const bool item = pending;
            pending = false; held = 0; armed = true;
            return item ? Event::Item : Event::None;
        }
        const bool down = y < -.7f && -y > 1.5f * std::abs(x);
        if (armed && down) { pending = true; armed = false; held = 0; }
        if (pending) {
            // Permit the return through the dead band, but cancel a change of direction.
            if (std::abs(x) > .5f || y > 0) { Reset(); return Event::None; }
            if (down) held += dt;
            else held = 0;
            if (held >= .11f) { pending = false; return Event::Turn; }
        }
        return Event::None;
    }
};
} // namespace VR::QuickTurn
