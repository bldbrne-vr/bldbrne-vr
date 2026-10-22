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
#include <cmath>

#include "common/types.h"

// Status build-ups drain a little faster in VR (vr.ini status_drain_pct, default 10: +10% of the
// game's own recovery), for the main player only. Pure: no emulator state, unit tested in
// tests/test_status_drain.cpp; the game side is core/vr/bloodborne_status.cpp.
//
// The game keeps each character's build-ups in its resist module as the REMAINING resistance
// (`cur`, out of `max`): build-up = max - cur. A hit lowers cur (at <= 0 the status triggers and
// cur is reset to max; beasthood stays at 0); the game's own recovery raises cur by RATE points
// per game second (one global, 5) up to max, keeping the fraction. Raising cur toward max (never
// past it) can therefore only shrink a build-up: it can never trigger or extend a status.
//
// One step per main-player update (site A, after the game's own recovery that update):
//  - only slow poison, rapid poison, bleed and frenzy (Types); beasthood is a buff the hunter
//    builds on purpose and is never touched;
//  - never on the way up: a build-up that rose (cur fell) in the last RiseHoldNs gets nothing
//    extra, so while a source keeps building (a swamp, a lantern, a brain's gaze, a weapon's
//    hits) the game's numbers are untouched; the extra starts once it is purely falling;
//  - the extra (RATE * dt * pct / 100) accumulates its fraction like the game does and is
//    written as whole points, never above max, through a compare-exchange that expects the value
//    read (a build-up landing in between is never lost);
//  - a new owner (another PlayerIns: a load, a death) starts over.
namespace VR::StatusDrain {

constexpr u32 Types = 4;               // slow poison, rapid poison, bleed, frenzy (not beasthood)
constexpr s64 RiseHoldNs = 1'000'000'000;
constexpr float MaxDt = 0.25f;         // a longer update (a hitch, a load) counts as this long
constexpr s32 DefaultRate = 5;         // the game's points per second (eboot global), fallback
constexpr s32 MaxPct = 100;

constexpr const char* TypeName(u32 type) {
    return type == 0 ? "slow poison" : type == 1 ? "rapid poison" : type == 2 ? "bleed" : "frenzy";
}

struct TypeState {
    bool known{false}; // last_cur holds a value read from this owner
    s32 last_cur{0};
    s64 rise_ns{0};    // the last time the build-up rose (cur fell)
    bool rose{false};  // rise_ns is valid
    float rem{0.0f};   // the extra's fraction carried to the next update
};

struct State {
    const void* owner{nullptr};
    std::array<TypeState, Types> type{};
    u64 points{0}; // extra points written (log)
    u32 writes{0};
    u32 lost{0};   // compare-exchanges that found another value (a build-up landed)
};

// The extra points per second at `rate` and `pct` (the log line).
constexpr float ExtraPerSecond(s32 rate, s32 pct) {
    return static_cast<float>(std::max(rate, 0)) * static_cast<float>(std::clamp(pct, 0, MaxPct)) /
           100.0f;
}

// One update. `cur` / `max`: the module's values for Types (read this update), `rate` the game's
// recovery (points / s), `dt` this update's game seconds, `pct` the setting, `now_ns` a steady
// clock. `cas(type, expected, value)` writes `value` if the module still holds `expected` and
// says whether it did. Returns the points written this update.
template <typename Cas>
u32 Step(State& st, const void* owner, const std::array<s32, Types>& cur,
         const std::array<s32, Types>& max, s32 rate, float dt, s32 pct, s64 now_ns, Cas&& cas) {
    if (owner != st.owner) {
        st = State{};
        st.owner = owner;
    }
    pct = std::clamp(pct, 0, MaxPct);
    rate = std::max(rate, 0);
    dt = std::isfinite(dt) ? std::clamp(dt, 0.0f, MaxDt) : 0.0f;
    u32 written = 0;
    for (u32 i = 0; i < Types; ++i) {
        TypeState& t = st.type[i];
        const s32 c = cur[i];
        const s32 m = max[i];
        if (t.known && c < t.last_cur) {
            t.rose = true;
            t.rise_ns = now_ns;
            t.rem = 0.0f;
        }
        t.known = true;
        t.last_cur = c;
        if (m <= 0 || c >= m || c < 0) {
            t.rem = 0.0f; // nothing built up (or a value that is not a build-up)
            continue;
        }
        if (pct == 0 || (t.rose && now_ns - t.rise_ns < RiseHoldNs)) {
            continue; // the game's own rate (rising, or the setting off)
        }
        t.rem += static_cast<float>(rate) * dt * static_cast<float>(pct) / 100.0f;
        const float whole = std::floor(t.rem);
        if (whole < 1.0f) {
            continue;
        }
        t.rem -= whole;
        const s32 value = static_cast<s32>(std::min<s64>(static_cast<s64>(c) + static_cast<s64>(whole), m));
        if (value <= c) {
            continue;
        }
        if (cas(i, c, value)) {
            t.last_cur = value;
            written += static_cast<u32>(value - c);
            st.points += static_cast<u64>(value - c);
            ++st.writes;
        } else {
            ++st.lost; // the next update reads the new value (a rise then holds the extra)
        }
    }
    return written;
}

} // namespace VR::StatusDrain
