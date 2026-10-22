// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <cstdint>

// VR body: the injection's drain (vr.ini vial_inject_drain=1). The moment an injection heals (the
// THUMP on both hands, the green heal vignette, the HP itself), the Blood Vial in the hunter's hand
// empties: its blood drops from full to empty over vial_inject_drain_ms (420 ms) as a plunger
// pushes it out (DrainFill: a soft start, a steady pour, a soft stop), then the empty vial stays
// vial_inject_empty_ms (200 ms) and goes away like the full one did: put away 620 ms after the
// THUMP. Nothing else changes: the hands' freeze (vial_inject_hold_frames, core/vr/inject_hold.h)
// and its release stay as they are, the hunter keeps moving, no animation is played. (Until
// 2026-10-09: the empty vial stayed 80 ms, gone 500 ms after the THUMP; the tester, asked whether
// to keep it a bit longer so its emptying registers: "yes do it for 200ms". Until 2026-10-06
// 23:00: 240 ms, an ease-in 1 - t^2, gone 60 ms after; the tester: "make the syringe emptying a
// little more slowly".)
//
// How it is drawn: the game's own vial is an effect (FFX 173, model s10002) that cannot show a
// level. With the drain on, the presenter flags its vial request (VR::Held::FlagDrain) from the
// moment the vial is armed: the main player's update does not ask for the effect, keeps the left
// weapon hidden and the vial grip closed, and the in-frame renderer draws the same model s10002 at
// the same point of the palm (its silhouette within a pixel or two of the game's), full of dark
// blood, its level from the drain word below once the injection heals (vr_inframe.frag
// ShadeVialDrain). Nothing is handed over at the THUMP: the level just starts to fall.
//
// Pure (no emulator state): the presenter, the game's update and the renderer share it, the unit
// tests (tests/test_vial_drain.cpp) drive it with scripted clocks.
namespace VR::Held {

// The plunger's ramps: the fraction of the drain it takes to get going and to come to a stop.
// At the default 420 ms: accelerate for 42 ms, pour until 258 ms, then ease out
// for 162 ms. The asymmetric ramps put half the liquid out at 180 ms.
constexpr float DrainRampIn = 0.1f;
constexpr float DrainRampOut = 27.0f / 70.0f;

// The blood level (1 full .. 0 empty) at `t` (0 .. 1) of the drain: a plunger pushed at a steady
// speed (a trapezoidal speed: a short acceleration, a constant pour, a longer soft stop).
// Half is gone at 3/7 of the duration (180 ms of the default 420 ms), so the drain registers
// while the hands are still held. It settles on empty instead of vanishing.
constexpr float DrainFill(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    constexpr float a = DrainRampIn;
    constexpr float b = DrainRampOut;
    constexpr float v = 1.0f / (1.0f - 0.5f * (a + b)); // unit area under the speed curve
    float out = 0.0f;
    if (t < a) {
        out = v * t * t / (2.0f * a);
    } else if (t <= 1.0f - b) {
        out = v * (t - 0.5f * a);
    } else {
        const float u = 1.0f - t;
        out = 1.0f - v * u * u / (2.0f * b);
    }
    return std::clamp(1.0f - out, 0.0f, 1.0f);
}

constexpr std::int32_t MaxDrainMs = 2000;

// The empty vial stays this long after the drain reaches empty before it is put away
// (vial_inject_empty_ms): long enough that the empty glass registers (200 ms: 12 game frames at
// 60 fps), and the request's trip to the game's update fits in. 80 = the behaviour before
// 2026-10-09 (a few frames, gone 500 ms after the THUMP); 0 = put away as the drain ends.
constexpr std::int32_t DefaultEmptyHoldMs = 200;
constexpr std::int32_t MaxEmptyHoldMs = 1000;

// How long the vial is shown from the THUMP (ms): the drain, then the empty vial's hold (the
// arguments clamped as Start does). 0 when nothing drains (`drain_ms` <= 0).
constexpr std::int32_t VisibleMs(std::int32_t drain_ms, std::int32_t empty_ms) {
    if (drain_ms <= 0) {
        return 0;
    }
    return std::min(drain_ms, MaxDrainMs) + std::clamp(empty_ms, 0, MaxEmptyHoldMs);
}

// The presenter's side (its frame thread): one drain per healed injection.
class VialDrain {
public:
    // A drain from `now` over `drain_ms` (clamped to MaxDrainMs), the empty vial then kept
    // `empty_ms` (vial_inject_empty_ms, clamped to 0 .. MaxEmptyHoldMs). False (nothing started)
    // when `drain_ms` <= 0. A new start replaces one that still runs.
    bool Start(std::int64_t now, std::int32_t drain_ms,
               std::int32_t empty_ms = DefaultEmptyHoldMs) {
        if (drain_ms <= 0) {
            return false;
        }
        on_ = true;
        start_ns_ = now;
        drain_ms_ = std::min(drain_ms, MaxDrainMs);
        empty_ms_ = std::clamp(empty_ms, 0, MaxEmptyHoldMs);
        return true;
    }

    // The vial is put away (its request ended), re-armed, a menu (a context switch): the next
    // vial is full again.
    void Cancel() {
        on_ = false;
    }

    [[nodiscard]] bool On() const {
        return on_;
    }
    [[nodiscard]] std::int64_t StartNs() const {
        return start_ns_;
    }
    [[nodiscard]] std::int32_t DrainMs() const {
        return drain_ms_;
    }
    [[nodiscard]] std::int64_t DrainNs() const {
        return std::int64_t{drain_ms_} * 1'000'000;
    }
    // The empty vial's hold after the drain (vial_inject_empty_ms).
    [[nodiscard]] std::int32_t EmptyHoldMs() const {
        return empty_ms_;
    }
    [[nodiscard]] std::int64_t EmptyHoldNs() const {
        return std::int64_t{empty_ms_} * 1'000'000;
    }

    // The level at `now`: 1 before the drain and when none runs, 0 once it is over.
    [[nodiscard]] float Fill(std::int64_t now) const {
        if (!on_) {
            return 1.0f;
        }
        return DrainFill(static_cast<float>(now - start_ns_) /
                         static_cast<float>(std::max<std::int64_t>(DrainNs(), 1)));
    }

    // The drain is over (the vial is empty).
    [[nodiscard]] bool Empty(std::int64_t now) const {
        return on_ && now >= start_ns_ + DrainNs();
    }

    // How long (seconds) from `now` the vial must still be shown: the rest of the drain and the
    // empty vial's hold (0 when none runs or it is past).
    [[nodiscard]] float VisibleS(std::int64_t now) const {
        if (!on_) {
            return 0.0f;
        }
        const std::int64_t left = start_ns_ + DrainNs() + EmptyHoldNs() - now;
        return left > 0 ? static_cast<float>(left) * 1e-9f : 0.0f;
    }

private:
    bool on_{false};
    std::int64_t start_ns_{0};
    std::int32_t drain_ms_{0};
    std::int32_t empty_ms_{DefaultEmptyHoldMs};
};

// The drain word (presenter -> renderer, one atomic u64; 0 = none): the start (steady clock ms,
// 48 bits, as the held request's stamp) and the length in ms (16 bits).
struct DrainWord {
    std::int64_t start_ms{0};
    std::int32_t drain_ms{0};
};

constexpr std::uint64_t DrainMsMask = (std::uint64_t{1} << 48) - 1;

constexpr std::uint64_t PackDrain(const DrainWord& d) {
    if (d.drain_ms <= 0) {
        return 0;
    }
    return (static_cast<std::uint64_t>(std::min(d.drain_ms, MaxDrainMs)) << 48) |
           (static_cast<std::uint64_t>(d.start_ms) & DrainMsMask);
}

constexpr DrainWord UnpackDrain(std::uint64_t word) {
    return {static_cast<std::int64_t>(word & DrainMsMask), static_cast<std::int32_t>(word >> 48)};
}

// A drain long over (1 ms from the clock's start): the level 0. The game's update hands it to the
// renderer for a vial armed with no Blood Vials left (core/vr/bloodborne_held.cpp): the hand holds
// an empty vial, a stab is refused (the double tick) and nothing drains.
constexpr std::uint64_t EmptyDrainWord = PackDrain({0, 1});

// The game's update's side (bloodborne_held.cpp, while the vial in the hand is ours): the word it
// hands the renderer. Whether the vial is an empty one is decided as it is armed (ours from this
// update, or a new vial armed over a drain: the presenter's word back to 0) from the Blood Vials
// carried then (-1 unknown: full), and kept until it is put away (Reset), so a refused stab's hold
// stays empty and a stab's own drain (the last vial's included) plays out from full.
class ArmedLook {
public:
    // The word for this update; `fresh` (out) is set when the look was decided again.
    std::uint64_t Word(std::uint64_t drain_word, std::int32_t carried, bool& fresh) {
        fresh = !on_ || (last_ != 0 && drain_word == 0);
        if (fresh) {
            empty_ = carried == 0;
        }
        on_ = true;
        last_ = drain_word;
        return drain_word == 0 && empty_ ? EmptyDrainWord : drain_word;
    }
    void Reset() {
        on_ = empty_ = false;
        last_ = 0;
    }
    [[nodiscard]] bool Empty() const {
        return empty_;
    }

private:
    bool on_{false};
    bool empty_{false};
    std::uint64_t last_{0};
};

// A drawn vial's liquid looks a little different each time (visual only: the heal is the game's).
// Each new vial gets its own level scale in [1 + lo, 1 + hi] (e.g. -0.10 .. +0.05), picked from a
// hash of when it appeared. A vial is new when none was drawn for GapNs, or when the level jumps
// back up (a fresh vial after a finished drain).
class FillVariation {
public:
    float Scale(std::int64_t now_ns, float fill, float lo, float hi) {
        const bool fresh =
            last_ns_ == 0 || now_ns - last_ns_ > GapNs || now_ns < last_ns_ || fill > last_fill_ + 0.5f;
        if (fresh) {
            scale_ = 1.0f + lo + (hi - lo) * Unit(static_cast<std::uint64_t>(now_ns));
        }
        last_ns_ = now_ns;
        last_fill_ = fill;
        return scale_;
    }
    static constexpr std::int64_t GapNs = 500'000'000;
    // A splitmix-style hash to [0, 1).
    static constexpr float Unit(std::uint64_t x) {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33;
        return static_cast<float>(x >> 40) / static_cast<float>(1ULL << 24);
    }

private:
    std::int64_t last_ns_{0};
    float last_fill_{1.0f};
    float scale_{1.0f};
};

// The level of a published drain at `now_ns` (the same steady clock; the 48-bit wrap is safe).
// No drain (word 0): full.
constexpr float DrainFillAt(std::uint64_t word, std::int64_t now_ns) {
    const DrainWord d = UnpackDrain(word);
    if (d.drain_ms <= 0) {
        return 1.0f;
    }
    const std::int64_t now_ms = now_ns / 1'000'000;
    const std::uint64_t diff =
        (static_cast<std::uint64_t>(now_ms) - static_cast<std::uint64_t>(d.start_ms)) & DrainMsMask;
    // Stamped just after `now` was read: slightly negative, full.
    if (diff >= (DrainMsMask >> 1)) {
        return 1.0f;
    }
    const float elapsed_ms = static_cast<float>(diff) +
                             static_cast<float>(now_ns % 1'000'000) * 1e-6f;
    return DrainFill(elapsed_ms / static_cast<float>(d.drain_ms));
}

} // namespace VR::Held
