// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

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

} // namespace VR::MenuInput
