// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <cmath>

namespace BenchmarkStartup {
// Presentation-aware recovery for the private benchmark runtime only. This does
// not recognize title/menu labels. Never send input after immersive presentation.
struct Navigator {
    bool gameplay = false;
    unsigned attempts = 0;
    double first_flat = -1, stable_since = -1, last_flat = -1;
    double press_start = -1, next_press = 0;
    void Observe(double now, bool projection, bool flat) {
        if (!std::isfinite(now) || now < 0) return;
        if (projection) { gameplay = true; press_start = -1; }
        if (gameplay) return;
        if (!flat) { stable_since = -1; press_start = -1; return; }
        if (first_flat < 0) first_flat = now;
        if (last_flat < 0 || now-last_flat > .5 || stable_since < 0) stable_since=now;
        last_flat=now;
        // Start promptly, then retry with a release interval; bound retries if a menu sticks.
        if (attempts >= 60 || now-first_flat > 60 || now-stable_since < .25 || now < next_press) return;
        ++attempts;
        press_start=now;
        next_press=now+1;
    }
    bool Down(double now) const {
        return !gameplay && press_start>=0 && now>=press_start && now-press_start<.2 &&
               last_flat>=0 && now-last_flat<=.5;
    }
};
}
