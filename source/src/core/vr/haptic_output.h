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

// Controller vibration: when to call the runtime, pure (no OpenXR) so the unit tests
// (tests/test_haptic_output.cpp) can drive it. The presenter (openxr_presenter.cpp UpdateHaptics)
// asks Output::Step once per XR frame and hand, makes the call it answers
// (xrApplyHapticFeedback with the amplitude and duration, or xrStopHapticFeedback) and reports
// an apply's result with Output::Applied.
//
// The level: the strongest of the hand's feedback pattern ("tick", raised to floor .. 1: Quest
// Touch motors hardly show less), its continuous hum and the game's rumble (not raised), each
// x gain. The calls: only when that level changes (by 0.05, or 15% of it), when a new tick starts
// (a pattern just played, or its level back from silence) or before the last call runs out. A
// steady tick is one call for its whole length (+ two display periods); a hum, the rumble or a ramp
// one call per keepalive or per step. Silence stops the vibration, never before min_on after the
// newest tick began (a shorter tick keeps its level that long), and only while the last call
// still runs. Before this, every frame re-sent the level for 20 ms: ~120 calls a second per hand,
// each replacing the vibration playing, while a Touch controller takes tens of ms to act on one.
namespace VR::Haptics {

struct Config {
    float gain{1.0f};                       // haptic_gain (0 = off)
    float floor{0.3f};                      // haptic_floor: the weakest tick
    std::int64_t min_on_ns{25'000'000};     // haptic_min_ms: the shortest tick
    std::int64_t period_ns{8'333'333};      // the display period (frames come this often)
    std::int64_t keepalive_ns{100'000'000}; // a call with no steady end in sight lasts this long
};

// One frame of one hand, before the gain.
struct Input {
    float tick{};     // the pattern's level this frame (0 .. 1)
    float steady_s{}; // how long the pattern keeps that level from now (0: a ramp, or none)
    bool fresh{};     // a pattern was played since the last frame
    float hum{};      // continuous level (charge build-up, a big swing's arc, the armed vial)
    float rumble{};   // the game's rumble for this hand
    // The pattern owns its hand while it plays (a melee hit's impact, a gunshot, a failure
    // pattern): the hum and the rumble are left out of the level (still in the diagnostics), so
    // an enemy's blows (0.8) or a swing's arc never smear its shape; they come back after it.
    bool own{};
};

enum class Call { None, Apply, Stop };

struct Decision {
    Call call{Call::None};
    float amplitude{};          // Apply: 0 .. 1
    std::int64_t duration_ns{}; // Apply
    // This frame's levels after the gain (diagnostics): tick before the floor, hum, rumble, and
    // the level played (0 below the audible threshold).
    float tick{};
    float hum{};
    float rumble{};
    float level{};
};

struct Output {
    static constexpr float Audible = 0.02f;

    bool vibrating{false};   // a call started a vibration that was neither stopped nor lapsed
    float applied{};         // its amplitude
    std::int64_t until_ns{}; // when it runs out by itself
    std::int64_t onset_ns{}; // when the newest tick began (min_on counts from here)
    bool tick_on{false};     // the pattern was audible last frame

    Decision Step(const Input& in, const Config& c, std::int64_t now) {
        Decision d{};
        d.tick = std::clamp(in.tick * c.gain, 0.0f, 1.0f);
        d.hum = std::clamp(in.hum * c.gain, 0.0f, 1.0f);
        d.rumble = std::clamp(in.rumble * c.gain, 0.0f, 1.0f);
        const float floor = std::clamp(c.floor, 0.0f, 1.0f);
        const float tick_level = d.tick >= Audible ? floor + (1.0f - floor) * d.tick : 0.0f;
        const float hum_level = in.own ? 0.0f : std::max(d.hum, d.rumble);
        const float level = std::max(tick_level, hum_level);
        const bool new_tick = tick_level > 0.0f && (in.fresh || !tick_on);
        tick_on = tick_level > 0.0f;
        const std::int64_t period = std::max<std::int64_t>(c.period_ns, 1'000'000);
        if (level >= Audible) {
            d.level = level;
            if (!vibrating || new_tick) {
                onset_ns = now;
            }
            const bool steady = tick_level >= hum_level && in.steady_s > 0.0f;
            const std::int64_t want = std::max<std::int64_t>(
                steady ? static_cast<std::int64_t>(static_cast<double>(in.steady_s) * 1e9) +
                             2 * period // the next frame may come late: no gap before its call
                       : c.keepalive_ns,
                onset_ns + c.min_on_ns - now);
            const bool changed = !vibrating || new_tick ||
                                 std::abs(level - applied) >= std::max(0.05f, 0.15f * applied);
            const bool running_out =
                until_ns - now < period + period / 2 && now + want > until_ns + period / 2;
            if (changed || running_out) {
                d.call = Call::Apply;
                d.amplitude = level;
                d.duration_ns = std::max<std::int64_t>(want, 1'000'000);
            }
            return d;
        }
        if (vibrating && now - onset_ns >= c.min_on_ns) {
            if (now < until_ns) {
                d.call = Call::Stop;
            }
            vibrating = false;
        }
        return d;
    }

    // The result of the Apply that Step asked for. A failed call (or XR_SESSION_NOT_FOCUSED) left
    // whatever played before to run out by itself: the next audible frame tries again.
    void Applied(bool ok, float amplitude, std::int64_t duration_ns, std::int64_t now) {
        if (!ok) {
            vibrating = false;
            return;
        }
        vibrating = true;
        applied = amplitude;
        until_ns = now + duration_ns;
    }
};

// ---- Combat feedback: the hierarchy -----------------------------------------------------------
// The shapes the presenter's combat patterns are built from (pattern amplitudes, before
// haptic_floor: 0 .. 1 plays as floor .. 1), pure so tests/test_haptic_output.cpp holds the
// order every event must keep to be told apart (the tester's 2026-10-06 session: a melee hit's
// 0.8-0.9 under the swing's ticks and arc hum did not read as THE hit):
//   1. a melee hit landing and a gunshot: a sharp near-full attack that decays (0.9 .. 1), owning
//      the hand while it plays (Input::own: no hum, no rumble under it); a landed melee hit is
//      full for 65+ ms (the 22:40 test: the 24 ms crack did not read as THE hit) and
//      guarded for ImpactGuardMs (Replaces: nothing but another impact cuts it);
//   2. the injection's THUMP (1.0 / 0.85, flat 70 ms, both hands);
//   3. a blow on the hunter: a small knock on both hands (about 0.43 .. 0.6 played), a little
//      stronger toward the blow and for a big one, under 1 and 2 (lower priority: it never cuts
//      the weapon hand's own impact or the gun's recoil);
//   4. a bullet or a thrown item connecting: one light tick (about 0.5 played) after the shot;
//   5. a swing's own tick (0.44 played), the whiff's: the lightest.
namespace Combat {

struct Shape {
    float peak{};      // flat for hold_ms ...
    float hold_ms{};
    float tail_from{}; // ... then a ramp from this to 0 over tail_ms (0: none)
    float tail_ms{};
    // A heavy hit's second "crunch" (crunch 0: none): crunch_gap_ms of silence after the tail,
    // then crunch flat for crunch_ms, fading from crunch_tail_from to 0 over crunch_tail_ms.
    float crunch_gap_ms{};
    float crunch{};
    float crunch_ms{};
    float crunch_tail_from{};
    float crunch_tail_ms{};
};

// A shape's length (ms), the crunch included.
inline float ShapeMs(const Shape& s) {
    float ms = s.hold_ms + (s.tail_ms > 0.0f && s.tail_from > 0.0f ? s.tail_ms : 0.0f);
    if (s.crunch > 0.0f && s.crunch_ms > 0.0f) {
        ms += s.crunch_gap_ms + s.crunch_ms +
              (s.crunch_tail_ms > 0.0f && s.crunch_tail_from > 0.0f ? s.crunch_tail_ms : 0.0f);
    }
    return ms;
}

// A shape as the presenter's pattern segments (openxr_presenter.cpp CombatPattern: linear
// ramps from -> to over ms; a gap is 0 -> 0), padded with silence to at least `min_total_ms`
// (an impact's guard: the hand stays its own that long). Returns the count (at most 8).
struct Segment {
    float from, to, ms;
};
inline int ShapeSegments(const Shape& s, float min_total_ms, Segment* out) {
    int n = 0;
    out[n++] = {s.peak, s.peak, s.hold_ms};
    if (s.tail_ms > 0.0f && s.tail_from > 0.0f) {
        out[n++] = {s.tail_from, 0.0f, s.tail_ms};
    }
    if (s.crunch > 0.0f && s.crunch_ms > 0.0f) {
        if (s.crunch_gap_ms > 0.0f) {
            out[n++] = {0.0f, 0.0f, s.crunch_gap_ms};
        }
        out[n++] = {s.crunch, s.crunch, s.crunch_ms};
        if (s.crunch_tail_ms > 0.0f && s.crunch_tail_from > 0.0f) {
            out[n++] = {s.crunch_tail_from, 0.0f, s.crunch_tail_ms};
        }
    }
    const float total = ShapeMs(s);
    if (std::isfinite(min_total_ms) && min_total_ms > total + 0.5f) {
        out[n++] = {0.0f, 0.0f, min_total_ms - total};
    }
    return n;
}

// A landed melee hit's guard: its impact keeps the hand for at least this long (padded with
// silence, Input::own all along: no arc hum, no rumble). The tester's 2026-10-06 session: half the
// hits had the same swing's wall clank 1 .. 20 ms after them (the blade going on into the floor
// or a wall), which replaced the 24 ms crack at once.
constexpr float ImpactGuardMs = 200.0f;

// HandHaptics::Play's rule: may a pattern (`next_priority`, a landed hit's impact or not) replace
// the one playing? A landed hit's impact replaces anything that is not an impact (a refused
// swing's double tick still playing: the arena mock's #18 lost its impact to the one of the
// swing refused 0.3 s before it). A playing impact (its guard included) gives way only to another
// impact (a later swing's hit, a heavier second target, a visceral) or the startup test (4): not
// the same swing's wall clank, the next swing's tick, a refused swing's buzz or a blow's knock.
// Otherwise the newer wins unless the playing one has a higher priority (as before).
inline bool Replaces(int playing_priority, bool playing_impact, int next_priority,
                     bool next_impact) {
    if (next_impact && !playing_impact) {
        return true;
    }
    if (playing_impact && !next_impact) {
        return next_priority >= 4;
    }
    return next_priority >= playing_priority;
}

inline float Clamp01(float v) {
    return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
}

// How heavy a melee hit is, 0 .. 1: its damage (ImpactDamageLow .. ImpactDamageHigh: the tester's
// Saw Cleaver light at 95, its heavy at 123, the Hunter Axe's heavy at 194) for 0.6 of it, the
// row's hit stop (the weapon's weight class: 0.06 s a dagger's or a claw's rows, 0.09 most, 0.12
// the heavy rows) for 0.4, and the two-handed grip +0.25.
constexpr float ImpactDamageLow = 40.0f;
constexpr float ImpactDamageHigh = 200.0f;
inline float ImpactWeight(float damage, float hitstop_s, bool two_hand) {
    const float d = Clamp01((damage - ImpactDamageLow) / (ImpactDamageHigh - ImpactDamageLow));
    const float h = std::isfinite(hitstop_s) ? Clamp01((hitstop_s - 0.06f) / 0.06f) : 0.5f;
    return Clamp01(0.6f * d + 0.4f * h + (two_hand ? 0.25f : 0.0f));
}

// A hit with less damage than this (a guarded or parried blow, an immune target): the knock.
constexpr float ImpactMinDamage = 1.0f;

// A hit this heavy (ImpactWeight: the Saw Cleaver's heavy, any two-handed hit, the axe) gets the
// crunch (haptic_melee_crunch).
constexpr float ImpactCrunchWeight = 0.5f;

// The impact (x `gain`, haptic_melee_hit), with `hold_ms` (haptic_melee_hit_ms, 65):
// full (1.0) for hold_ms + 15 ms x weight (65 .. 80 ms: a Quest Touch motor needs the length to
// read as a blow; 1.0 is its most, so the energy is the time), then fading from 0.70 .. 0.85 to
// 0 over 80 .. 115 ms: about 150 ms for a light nick, 165 for a Saw Cleaver light. A heavy one
// (weight >= ImpactCrunchWeight, `crunch`) instead drops away over 25 ms, and after 40 ms of
// silence a second shorter blow, the crunch (0.85 for 30 ms, fading from 0.55 over 50 ms):
// about 220 ms. A gain above 1 lengthens the full part (x gain), below 1 lowers every level.
// hold_ms 0: the old impact (0.9 .. 1.0 for 20 .. 30 ms, then from 0.6 .. 0.8 of that over
// 65 .. 100 ms; never a crunch).
inline Shape MeleeImpact(float weight, float gain, float hold_ms = 0.0f, bool crunch = false) {
    const float w = Clamp01(weight);
    const float g = std::isfinite(gain) ? std::clamp(gain, 0.0f, 1.5f) : 1.0f;
    if (!(std::isfinite(hold_ms) && hold_ms > 0.0f)) {
        const float peak = std::min((0.9f + 0.1f * w) * g, 1.0f);
        return {peak, 20.0f + 10.0f * w, (0.6f + 0.2f * w) * peak, 65.0f + 35.0f * w};
    }
    Shape s{};
    s.peak = std::min(g, 1.0f);
    s.hold_ms = (std::min(hold_ms, 300.0f) + 15.0f * w) * std::max(g, 1.0f);
    if (crunch && w >= ImpactCrunchWeight && s.peak > 0.0f) {
        s.tail_from = std::min(0.6f * g, 1.0f);
        s.tail_ms = 25.0f;
        s.crunch_gap_ms = 40.0f;
        s.crunch = std::min(0.85f * g, 1.0f);
        s.crunch_ms = 30.0f;
        s.crunch_tail_from = std::min(0.55f * g, 1.0f);
        s.crunch_tail_ms = 50.0f;
        return s;
    }
    s.tail_from = std::min((0.70f + 0.15f * w) * g, 1.0f);
    s.tail_ms = 80.0f + 35.0f * w;
    return s;
}

// A melee hit that did no damage (blocked, parried, immune): a dull short knock (0.51 played for
// 30 ms, then fading 40 ms), well under the impact.
inline Shape MeleeKnock(float gain) {
    const float g = std::isfinite(gain) ? std::clamp(gain, 0.0f, 1.5f) : 1.0f;
    return {0.3f * g, 30.0f, 0.15f * g, 40.0f};
}

// A blow on the hunter that took `fraction` of his max HP, on a hand with the direction's
// `side_gain` (core/vr/hit_direction.h HandGains: 1 toward the blow, down to FarHandMin away from
// it; 1 without a direction), x `gain` (haptic_hit_taken): 0.22 for a scratch to 0.40 from a
// quarter of max HP, the far hand x0.8 .. 1 (0.43 .. 0.58 played toward the blow, with the floor).
// 30 ms flat, then 40 ms fading from 0.6 of it: about 70 ms.
inline Shape HitTaken(float fraction, float side_gain, float gain) {
    const float big = Clamp01(fraction / 0.25f);
    const float side = 0.8f + 0.2f * Clamp01(side_gain);
    const float g = std::isfinite(gain) ? std::clamp(gain, 0.0f, 1.5f) : 1.0f;
    const float a = std::min((0.22f + 0.18f * big) * side * g, 0.6f);
    return {a, 30.0f, 0.6f * a, 40.0f};
}

// A bullet of the hunter's gun hitting a character (x haptic_gun_hit): one light tick, 0.35 for
// 20 ms (0.55 played for haptic_min_ms), after the shot's own recoil has played out.
inline Shape GunHit(float gain) {
    const float g = std::isfinite(gain) ? std::clamp(gain, 0.0f, 1.5f) : 1.0f;
    return {std::min(0.35f * g, 1.0f), 20.0f, 0.0f, 0.0f};
}

// A thrown item (a knife, a Molotov, a pebble) hitting a character (x haptic_throw_hit): a
// lighter tick, 0.28 for 18 ms (0.5 played).
inline Shape ThrowHit(float gain) {
    const float g = std::isfinite(gain) ? std::clamp(gain, 0.0f, 1.5f) : 1.0f;
    return {std::min(0.28f * g, 1.0f), 18.0f, 0.0f, 0.0f};
}

// The amplitude a pattern amplitude plays at (haptic_floor raises every audible tick).
inline float Played(float amplitude, float floor = 0.3f) {
    return amplitude >= Output::Audible ? floor + (1.0f - floor) * Clamp01(amplitude) : 0.0f;
}

} // namespace Combat

} // namespace VR::Haptics
