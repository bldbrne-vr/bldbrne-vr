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

// The physical plunging attack (melee_plunge; core/vr/bloodborne_melee.cpp): no plunge animation.
// You drop off a ledge (or leap) and swing: a swing opened while the hunter falls keeps its attack
// alive until he lands (plus a short grace), so the blade you hold out hits what you land on, and
// a hit after a fall at least `min_drop_m` deep deals x`damage`. Pure (no emulator dependencies;
// tests/test_plunge.cpp). Heights are the physics module's position in game units (metres).
//
// Air comes from the main player's update (core/vr/bloodborne_motion.cpp LatestAir): the physics
// module's landed byte (HKS env 20) and height. (Not the motion clips: the newest fall clip stays
// the newest one long after landing, as idle clips are not counted.)
namespace VR::Melee::Plunge {

constexpr std::int64_t Ms = 1'000'000;

struct Config {
    bool enabled{true};
    float min_drop_m{1.2f};             // a fall at least this deep is a plunge
    float descend_mps{1.0f};            // ... while falling at least this fast
    std::int64_t airborne_ns{100 * Ms}; // the landed byte off this long: airborne (not a step)
    std::int64_t coyote_ns{150 * Ms};   // a swing this soon after a deep landing still plunges
    std::int64_t land_grace_ns{150 * Ms}; // the attack stays this long after landing
    std::int64_t max_hold_ns{2500 * Ms};  // and never longer than this after it opened
    float damage{1.35f};                  // the plunge's damage factor
};

struct Sample {
    bool valid{false};
    bool grounded{true};
    float y{0.0f};
    std::int64_t ns{0}; // the update that read it (steady clock)
};

struct State {
    bool airborne{false};
    bool descending{false};
    float vy{0.0f};          // m/s, + up
    float top_y{0.0f};       // the highest point of this airtime (or the ground's height)
    float drop_m{0.0f};      // fallen so far this airtime
    std::int64_t air_ns{0};  // the landed byte off since (0: on the ground)
    std::int64_t landed_ns{0};
    float landed_drop_m{0.0f}; // the last airtime's whole drop
    bool landed{false};        // this step was the landing
    std::uint32_t airtimes{0}; // +1 at each takeoff that counted as airborne
};

class Tracker {
public:
    const State& Current() const {
        return st;
    }

    // One main-player update's sample (the same update again changes nothing).
    const State& Step(const Sample& s, const Config& c) {
        st.landed = false;
        if (!s.valid) {
            const std::int64_t landed_ns = st.landed_ns;
            const float landed_drop = st.landed_drop_m;
            const std::uint32_t airtimes = st.airtimes;
            st = {};
            st.landed_ns = landed_ns;
            st.landed_drop_m = landed_drop;
            st.airtimes = airtimes;
            last_ns = 0;
            return st;
        }
        if (s.ns == last_ns) {
            return st;
        }
        if (last_ns == 0) {
            st.top_y = s.y; // the first sample (or the first after a gap): no fall measured yet
        }
        if (last_ns != 0) {
            const double dt = static_cast<double>(s.ns - last_ns) * 1e-9;
            if (dt > 0.0 && dt <= 0.25) {
                const float v = static_cast<float>((s.y - last_y) / dt);
                st.vy = std::isfinite(v) ? 0.5f * (st.vy + v) : st.vy;
            } else {
                st.vy = 0.0f;
            }
        }
        last_ns = s.ns;
        last_y = s.y;
        if (s.grounded) {
            if (st.airborne) {
                st.landed = true;
                st.landed_ns = s.ns;
                st.landed_drop_m = std::max(st.top_y - s.y, 0.0f);
            }
            st.airborne = false;
            st.descending = false;
            st.air_ns = 0;
            st.top_y = s.y;
            st.drop_m = 0.0f;
            return st;
        }
        if (st.air_ns == 0) {
            st.air_ns = s.ns;
            st.top_y = std::max(st.top_y, s.y);
        }
        const bool was = st.airborne;
        st.airborne = s.ns - st.air_ns >= c.airborne_ns;
        if (st.airborne && !was) {
            ++st.airtimes;
        }
        st.top_y = std::max(st.top_y, s.y);
        st.drop_m = std::max(st.top_y - s.y, 0.0f);
        st.descending = st.airborne && st.vy <= -c.descend_mps;
        return st;
    }

    // A swing opening now is a plunge: falling, or just landed from a deep fall.
    bool Opens(std::int64_t now, const Config& c) const {
        if (!c.enabled) {
            return false;
        }
        if (st.airborne && st.descending) {
            return true;
        }
        return !st.airborne && st.landed_ns != 0 && now - st.landed_ns <= c.coyote_ns &&
               st.landed_drop_m >= c.min_drop_m;
    }

    // A plunge opened at `created_ns` still holds its attack: airborne, or within the grace
    // after landing, never past max_hold.
    bool Holds(std::int64_t now, std::int64_t created_ns, const Config& c) const {
        if (!c.enabled || now - created_ns > c.max_hold_ns) {
            return false;
        }
        if (st.airborne) {
            return true;
        }
        return st.landed_ns >= created_ns - c.coyote_ns && now - st.landed_ns <= c.land_grace_ns;
    }

    // The depth that counts for a hit now: this fall so far, or the one just landed from.
    float Drop(std::int64_t now, const Config& c) const {
        if (st.airborne) {
            return st.drop_m;
        }
        return st.landed_ns != 0 && now - st.landed_ns <= c.land_grace_ns + c.coyote_ns
                   ? st.landed_drop_m
                   : 0.0f;
    }

private:
    State st{};
    std::int64_t last_ns{0};
    float last_y{0.0f};
};

// A plunge hit's damage factor: x`damage` from a fall at least min_drop_m deep, else none.
inline float DamageFactor(float drop_m, const Config& c) {
    return c.enabled && drop_m >= c.min_drop_m ? c.damage : 1.0f;
}

} // namespace VR::Melee::Plunge
