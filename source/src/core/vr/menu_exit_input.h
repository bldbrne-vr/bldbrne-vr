// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

namespace VR::MenuInput {

// Back owns its press. Other controls held while backing out must be released
// before another context can interpret them (menu grips are Square/Triangle).
// The short menu-context tail is neutral: the game has already resumed gameplay.
template <std::size_t N>
void GuardExit(const std::array<bool, N>& raw, std::array<bool, N>& input,
               std::array<bool, N>& blocked, bool menu_live,
               std::size_t back_right, std::size_t back_left) {
    const bool back = raw[back_right] || raw[back_left];
    if (menu_live && !back) return;
    for (std::size_t i = 0; i < N; ++i) {
        if (menu_live && (i == back_right || i == back_left)) continue;
        blocked[i] = blocked[i] || raw[i];
        input[i] = false;
    }
}

// Menu state is sampled on the guest update thread. A quick Back release can
// precede its closed sample; a grip squeezed in that interval must not become
// Triangle (a blood vial once gameplay resumes). Keep the same ownership briefly
// after release, and retain the ordinary release-before-reuse rule for each key.
class ExitGuard {
public:
    static constexpr float SettleSeconds = 0.15f;

    void Reset() { settle_left = 0.0f; }

    template <std::size_t N>
    void Update(const std::array<bool, N>& raw, std::array<bool, N>& input,
                std::array<bool, N>& blocked, bool menu_live,
                std::size_t back_right, std::size_t back_left, float dt) {
        const bool back = raw[back_right] || raw[back_left];
        settle_left = back ? SettleSeconds : std::max(0.0f, settle_left - dt);
        if (!menu_live || back) {
            GuardExit(raw, input, blocked, menu_live, back_right, back_left);
        } else if (settle_left > 0.0f) {
            for (std::size_t i = 0; i < N; ++i) {
                if (i == back_right || i == back_left) continue;
                blocked[i] = blocked[i] || raw[i];
                input[i] = false;
            }
        }
    }

private:
    float settle_left{};
};

} // namespace VR::MenuInput
