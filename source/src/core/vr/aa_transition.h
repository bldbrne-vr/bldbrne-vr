// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include <atomic>
#include "core/vr/aa_mode.h"

namespace VR::Aa {
// Requests can arrive on the UI thread. Only the command processor advances the active
// mode, between frames. A request arriving during the wait remains pending for next time.
class FrameTransition {
public:
    void Initialize(Mode mode) {
        requested.store(mode);
        active.store(mode);
    }
    void Request(Mode mode) { requested.store(mode); }
    Mode Requested() const { return requested.load(); }
    Mode Active() const { return active.load(); }

    template <typename FinishRendering>
    bool AtBoundary(FinishRendering&& finish_rendering) {
        const Mode next = Requested();
        if (next == Active()) {
            return false;
        }
        // Drain CPU workers, replay and GPU work before publishing the new mode.
        // If completion fails, keep the previous mode and leave the request pending.
        finish_rendering();
        active.store(next);
        return true;
    }
private:
    std::atomic<Mode> requested{DefaultMode};
    std::atomic<Mode> active{DefaultMode};
};
} // namespace VR::Aa
