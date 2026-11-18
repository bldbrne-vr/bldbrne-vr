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
    // The grips (the menu's Square / Triangle: an item, a blood vial once gameplay resumes) stay
    // held back after Back while the menu is still up, at most this long: a quick Back's settle
    // can end before the game's closing fade does (2026-11-06: "sometimes when I close
    // the menu by pressing B quickly, it still uses an item (blood vial)").
    static constexpr float GripHoldSeconds = 0.6f;

    void Reset() { settle_left = 0.0f; blocked_hold_was = false; grip_hold_left = 0.0f; }
    // A custom VR page closes without sending the game's Back button. Give that
    // hand-over the same protection as backing out of a game menu.
    void BeginClose() { settle_left = SettleSeconds; blocked_hold_was = false; }
    [[nodiscard]] bool Settling() const { return settle_left > 0.0f; }
    // A grip squeezed in GripHoldSeconds was held back since the last call (logs).
    bool TakeGripCaught() { const bool c = grip_caught; grip_caught = false; return c; }

    template <std::size_t N>
    void Update(const std::array<bool, N>& raw, std::array<bool, N>& input,
                std::array<bool, N>& blocked, bool menu_live,
                std::size_t back_right, std::size_t back_left, float dt,
                std::size_t grip_r = N, std::size_t grip_l = N) {
        const bool back = raw[back_right] || raw[back_left];
        // A blocked squeeze can outlast Back. Do not let another squeeze become
        // a fresh menu action while the hands are still finishing that hand-over.
        bool blocked_hold = false;
        if (Settling()) {
            for (std::size_t i = 0; i < N; ++i) {
                if (i != back_right && i != back_left) blocked_hold |= raw[i] && blocked[i];
            }
        }
        // Once every blocked control is released, a fresh deliberate press can work
        // immediately; there is no extra dead time after finishing the hand-over.
        const bool released = blocked_hold_was && !blocked_hold && !back;
        settle_left = released ? 0.0f : back || blocked_hold ? SettleSeconds
                                                           : std::max(0.0f, settle_left - dt);
        blocked_hold_was = blocked_hold;
        if (!menu_live || back) {
            GuardExit(raw, input, blocked, menu_live, back_right, back_left);
        } else if (settle_left > 0.0f) {
            for (std::size_t i = 0; i < N; ++i) {
                if (i == back_right || i == back_left) continue;
                blocked[i] = blocked[i] || raw[i];
                input[i] = false;
            }
        }
        // GripHoldSeconds: from Back, in a live menu; gone the moment the menu is (a squeeze it
        // held stays blocked until let go, as any other).
        grip_hold_left = !menu_live ? 0.0f
                         : back     ? GripHoldSeconds
                                    : std::max(0.0f, grip_hold_left - dt);
        if (grip_hold_left > 0.0f) {
            for (const std::size_t g : {grip_r, grip_l}) {
                if (g < N && raw[g]) {
                    grip_caught = grip_caught || !blocked[g];
                    blocked[g] = true;
                    input[g] = false;
                }
            }
        }
    }

private:
    float settle_left{};
    bool blocked_hold_was{};
    float grip_hold_left{};
    bool grip_caught{};
};

} // namespace VR::MenuInput
