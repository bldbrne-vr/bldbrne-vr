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

namespace VR::Assists {

inline constexpr int DefaultHpBonus = 10;
inline constexpr int DefaultDodgeFrames = 1;

class LiveSettings {
public:
    void SetHpBonus(int percent) {
        hp_bonus.store(std::clamp(percent, 0, 100), std::memory_order_relaxed);
    }
    int HpBonus() const {
        return hp_bonus.load(std::memory_order_relaxed);
    }
    float HpMultiplier() const {
        return 1.0f + static_cast<float>(HpBonus()) / 100.0f;
    }
    void SetDodgeFrames(int frames) {
        dodge_frames.store(std::clamp(frames, 0, 10), std::memory_order_relaxed);
    }
    int DodgeFrames() const {
        return dodge_frames.load(std::memory_order_relaxed);
    }

private:
    std::atomic<int> hp_bonus{DefaultHpBonus};
    std::atomic<int> dodge_frames{DefaultDodgeFrames};
};

inline LiveSettings live;

}
