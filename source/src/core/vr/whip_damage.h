// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace VR::Whip {
// Per enemy contact, measured from the grip to the impact, not to the free tip.
// Strong close hits; -10 percentage points per metre beyond 1.5 m, floor 60%.
// The caller supplies the ATTACK's weapon identity, so a weapon switch cannot
// give an earlier Cane hit the Cutter's falloff. Compact/canned hits pass false.
inline float ContactDamageFactor(std::int32_t variation, bool physical_chain, float distance_m) {
    if (variation != 2400 || !physical_chain || !std::isfinite(distance_m) || distance_m < 0) {
        return 1.0f;
    }
    return std::clamp(1.0f - 0.10f * std::max(0.0f, distance_m - 1.5f), 0.60f, 1.0f);
}

// ---- The whip's damage pace (vr.ini whip_pace) --------------------------------------------------
//
// The whip forms register quicker than the game lets them attack: their combo-budget charges come
// back sooner than the game's own recovery (the presenter: the Threaded Cane's light 0.317 s against
// its clip's 0.767 s, the Beast Cutter's 0.744 s against 1.233 s), every revolution of a spin and
// every wrist crack is a window of its own, and their capsules are wider. That is for the feel: a
// lash you see lands. The damage stays the game's. The game starts a move only once the move before
// it has had its recovery (whip_moves.h WhipRecover: each clip's R1 / R2 cancel). A whip window that
// hurts something deals a full hit when the game could have started its move by then; one that
// comes earlier loses the share of its own move's recovery it is early by. So a lash after a pause,
// lashes at the game's own pace and the game's own R1 -> R2 hit in full; at twice the pace, half
// each; a light right after a heavy pays for the heavy's longer recovery. A window counts from its
// lash (its attack opening: the whip's attack waits for the chain's tip to come up to speed), so a
// stroke, a spin's revolution and a crack are timed alike. Each window adds its share of its own
// recovery to the game's clock, which never runs back and runs ahead of a window by (1 + floor) of
// its recovery at most: no flurry, spin or crack, of any mix of moves and whatever the budget lets
// through, deals more damage per second than the whip's best move at the game's pace. A window that
// hit nothing costs nothing; every enemy one window hits takes the same share (as one game attack
// hits them all); a pause banks one full hit. A window right on top of the last one still gets
// `floor`, which the next ones pay back, so even a double lash a frame apart keeps the game's rate
// over the two; once the clock owes that much, a window gets only what is left of it (nothing when
// it is full: windows closer than `floor` of a recovery apart, or a light that soon after a heavy;
// the default combo budget's two charges and refills seldom let one through).

struct PaceConfig {
    // The least share of a full hit a window that hurts gets while the clock has room for it
    // (owed back by the next ones).
    float floor{0.15f};
};

class PaceMeter {
public:
    // A whip window's first hit: its share of a full hit (0 .. 1; at least `floor` while the clock
    // has room). `at_ns` the window's lash (any steady clock, the same for every call), `recover_s`
    // the game's recovery of its move (WhipRecover; <= 0: not paced, a full hit, the meter
    // untouched).
    float Pay(std::int64_t at_ns, float recover_s, const PaceConfig& c = {}) {
        if (!(recover_s > 0.0f) || !std::isfinite(recover_s)) {
            return 1.0f;
        }
        const double floor = std::clamp(std::isfinite(c.floor) ? c.floor : 0.0f, 0.0f, 1.0f);
        const double r = recover_s;
        // A window that lashed before the last one that paid (its hit came late) counts from that
        // one: the clock never goes back.
        const std::int64_t t = have_ ? std::max(at_ns, last_ns_) : at_ns;
        // How early it comes: the game starts it only once the moves before it had their recovery.
        const double early =
            have_ && ready_ns_ > t ? static_cast<double>(ready_ns_ - t) * 1e-9 : 0.0;
        // Its share: what is left of its recovery once it is early, at least `floor`, but never
        // more than the clock has room for (it runs at most (1 + floor) recoveries ahead of this
        // window): a full meter pays nothing, so no run of windows, however close, beats the rate.
        const double room = 1.0 + floor - early / r;
        const double share = std::clamp(std::min(std::max(1.0 - early / r, floor), room), 0.0, 1.0);
        // The game's clock runs on by what it was worth of its own recovery: a full recovery after
        // it (what was early and what it was worth add up to one), or more for a floor it was not
        // worth, owed (never past the room).
        ready_ns_ = t + Ns(early + share * r);
        last_ns_ = t;
        have_ = true;
        return static_cast<float>(share);
    }

    void Reset() {
        *this = {};
    }

private:
    static std::int64_t Ns(double s) {
        return static_cast<std::int64_t>(std::llround(s * 1e9));
    }

    bool have_{false};
    std::int64_t last_ns_{0};  // the last window that paid lashed (the clock)
    std::int64_t ready_ns_{0}; // the game could start the next move from here
};

} // namespace VR::Whip
