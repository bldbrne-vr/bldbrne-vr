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

// The A+B chord (vr.ini roll_chord, motion controls, gameplay): A and B pressed together is the
// game's real ROLL, or, held standing still, the weapon grip adjust (core/vr/grip_adjust.h):
//  - moving (the left stick pushed, or let go at most dir_memory_s ago): the roll toward the stick
//    the moment the chord is complete (a hold means nothing more while moving);
//  - standing still: let go before hold_s, a roll back on the release; the stick pushed while
//    held (before hold_s), the roll toward it at once; held hold_s, the grip adjust's A+B
//    (Output::adjust feeds it) and no roll.
// The presenter does the roll (StartRoll: the stick, the game's own facing for that dodge so the
// game rolls instead of quickstepping, and the Circle tap through the leap / dodge sequencer, so
// right after a sprint it comes after the game's jump window: a roll, never a leap; locked on, the
// game only quicksteps).
// Neither A nor B reaches the game during a chord. A pressed first waits at most a_window_s for
// B. While the movement stick is pushed, B first waits up to b_window_s (150 ms) for A,
// including after B is released. Standing still, B alone reaches the game immediately;
// stopping during grace also forwards B and cancels its pending chord. Direction memory
// does not delay interaction. A-first and simultaneous stationary chords still work.
// A released inside its window is sent at release (Output::a_tap), preserving dodge timing.
// A buffered B is sent once if A does not join (Output::b_tap if released). Buffered presses
// are shown for at least min_press_s so the game can sample them.
// A press while the other button already reaches the game (A held to sprint, then B) is never a
// chord and goes straight through, and so does every press while Input::bypass is set (the sprint
// latch and a leap's sequence own A; the modifier owns B). After a chord both buttons are ignored
// until both are let go.
// With Input::enabled false (roll_chord=0, the classic layout) A and B go straight through, as
// before; the chord (pass_window_s) still feeds the grip adjust.
//
// Pure (no OpenXR or emulator state): the presenter runs it on the OpenXR frame thread, the unit
// tests (tests/test_roll_chord.cpp) on scripted inputs.
namespace VR::Roll {

struct Config {
    float a_window_s{0.06f};    // A pressed first: B within this makes the chord
    float b_window_s{0.15f};    // B first: A may join even after B is released
    float pass_window_s{0.30f}; // the same with the buttons going straight through (roll_chord=0)
    float hold_s{1.2f};         // held this long standing still: the grip adjust
    float push{0.30f};          // the left stick past this: moving
    float dir_memory_s{0.10f};  // the stick let go this recently still counts (its direction)
    float min_press_s{0.04f};   // a held-back press reaches the game at least this long
};

struct Input {
    float dt{0.0f};
    bool a{false};
    bool b{false};
    float x{0.0f}; // the left stick (move): + right, + forward
    float y{0.0f};
    bool enabled{true}; // hold back, suppress and roll (roll_chord=1, motion controls)
    bool bypass{false}; // a new press goes straight through (no chord can start)
};

enum class Event : std::uint8_t {
    None,
    Chord,    // both pressed standing still: counting to hold_s (nothing reaches the game)
    Still,    // held hold_s standing still: the grip adjust's moment
    Roll,     // roll now toward (roll_x, roll_y)
    Released, // let go after Still (or roll_chord=0): nothing
};

struct Output {
    bool a{false};      // A reaches the game this frame (Circle)
    bool b{false};      // B reaches the game this frame (Cross)
    bool a_tap{false};  // A let go inside its window: its press is sent now (min_press_s)
    bool b_tap{false};  // B released during grace: send its tap when grace expires
    bool adjust{false}; // the grip adjust's A+B: the chord held standing still
    bool chord{false};  // the chord was recognised this frame (roll_chord=1: the tick)
    Event event{Event::None};
    float held_s{0.0f};  // Roll / Released / Still: how long the chord was held
    bool moved{false};   // Roll: toward the stick (moving, or pushed while held)
    bool centred{false}; // Roll: no stick direction (the roll goes back)
    float roll_x{0.0f};  // Roll: the direction, unit (left stick space)
    float roll_y{-1.0f};
    float gap_s{0.0f}; // the chord: the time between the two presses
};

enum class Phase : std::uint8_t { Idle, Chord, Wait };

class Chord {
public:
    Output Update(const Config& c, const Input& in) {
        constexpr float Eps = 1e-5f;
        Output o{};
        const float dt = std::max(in.dt, 0.0f);
        const bool a_edge = in.a && !a_was_;
        const bool b_edge = in.b && !b_was_;
        a_was_ = in.a;
        b_was_ = in.b;
        const float mag = std::hypot(in.x, in.y);
        const bool pushed = mag > c.push;
        dir_age_ += dt;
        if (pushed) {
            dir_x_ = in.x / mag;
            dir_y_ = in.y / mag;
            dir_age_ = 0.0f;
        }
        a_t_ = a_edge ? 0.0f : a_t_ + dt;
        b_t_ = b_edge ? 0.0f : b_t_ + dt;
        bool want_a = false; // the game's buttons before the minimum press
        bool want_b = false;
        switch (phase_) {
        case Phase::Idle: {
            if (!in.enabled) {
                // Straight through; A+B pressed within pass_window_s still feeds the grip adjust.
                a_pend_ = b_pend_ = false;
                if ((a_edge || b_edge) && in.a && in.b &&
                    std::abs(a_t_ - b_t_) <= c.pass_window_s + Eps) {
                    StartChord(o, std::abs(a_t_ - b_t_), false);
                    o.event = Event::Chord;
                    o.adjust = true;
                }
                want_a = in.a;
                want_b = in.b;
                break;
            }
            // The press that completes a chord: the other one pressed first, still held back.
            // B may already be released; its pending press remembers the roll intent.
            bool chord = false;
            if (!in.bypass) {
                if (a_edge && b_edge) {
                    chord = true;
                } else if (a_edge) {
                    chord = b_pend_ && pushed && b_t_ <= c.b_window_s + Eps;
                } else if (b_edge) {
                    chord = a_pend_ && in.a && a_t_ <= c.a_window_s + Eps;
                }
            }
            if (chord) {
                StartChord(o, a_edge && b_edge ? 0.0f : (a_edge ? b_t_ : a_t_), true);
                o.chord = true;
                if (pushed || dir_age_ <= c.dir_memory_s + Eps) {
                    // Moving: the roll now (a hold means nothing more).
                    Roll(o, true);
                    phase_ = Phase::Wait;
                } else {
                    o.event = Event::Chord;
                    o.adjust = true;
                }
                break;
            }
            // New presses: wait for the other button, unless it already reaches the game (then
            // this is no chord) or a bypass owns the buttons.
            if (a_edge) {
                if (in.bypass || b_pass_) {
                    a_pass_ = true;
                    a_back_ = false;
                } else {
                    a_pend_ = true;
                }
            }
            if (b_edge) {
                if (in.bypass || a_pass_ || !pushed) {
                    b_pass_ = true;
                    b_back_ = false;
                } else {
                    b_pend_ = true;
                }
            }
            // Held-back presses: let go inside the window = the press sent now; past it = the
            // button's own, at once.
            if (a_pend_) {
                if (!in.a) {
                    a_pend_ = false;
                    a_back_ = true;
                    o.a_tap = true;
                } else if (a_t_ + dt > c.a_window_s + Eps) { // the next frame would be past it
                    a_pend_ = false;
                    a_pass_ = true;
                    a_back_ = true;
                }
            }
            // B's full window survives release, so sliding B -> A cannot first interact.
            // Only B waits longer; the A-only path above retains its existing timing.
            if (b_pend_ && (in.bypass || !pushed || b_t_ + Eps >= c.b_window_s)) {
                b_pend_ = false;
                b_back_ = true;
                b_pass_ = in.b;
                o.b_tap = !in.b;
            }
            a_pass_ = a_pass_ && in.a;
            b_pass_ = b_pass_ && in.b;
            want_a = a_pass_ || o.a_tap;
            want_b = b_pass_ || o.b_tap;
            break;
        }
        case Phase::Chord: {
            held_ += dt;
            o.held_s = held_;
            if (!buffered_) {
                // roll_chord=0: the buttons reach the game as they are; the stick pushed before
                // hold_s ends the grip adjust's count.
                if (!still_ && pushed) {
                    moved_ = true;
                }
                if (!in.a || !in.b) {
                    phase_ = in.a || in.b ? Phase::Wait : Phase::Idle;
                    o.event = Event::Released;
                } else if (!still_ && !moved_ && held_ + Eps >= c.hold_s) {
                    still_ = true;
                    o.event = Event::Still;
                }
                want_a = in.a;
                want_b = in.b;
                o.adjust = phase_ == Phase::Chord && !moved_;
                break;
            }
            if (!still_ && pushed) {
                // Pushed while held standing still: the roll toward the stick at once.
                Roll(o, true);
                phase_ = in.a || in.b ? Phase::Wait : Phase::Idle;
                break;
            }
            if (!in.a || !in.b) {
                phase_ = in.a || in.b ? Phase::Wait : Phase::Idle;
                if (still_) {
                    o.event = Event::Released;
                } else {
                    Roll(o, false); // let go before hold_s, the stick centred: a roll back
                }
                break;
            }
            if (!still_ && held_ + Eps >= c.hold_s) {
                still_ = true;
                o.event = Event::Still;
            }
            o.adjust = true;
            break;
        }
        case Phase::Wait:
            if (!in.a && !in.b) {
                phase_ = Phase::Idle;
            } else if (!buffered_) {
                want_a = in.a;
                want_b = in.b;
            }
            break;
        }
        o.a = Show(want_a, a_back_, a_game_, a_game_t_, dt, c.min_press_s);
        o.b = Show(want_b, b_back_, b_game_, b_game_t_, dt, c.min_press_s);
        return o;
    }

    // From outside (a context switch): back to idle; the presenter blocks buttons held through
    // it until they are let go, so they come back as new presses.
    void Reset() {
        phase_ = Phase::Idle;
        a_was_ = b_was_ = a_pend_ = b_pend_ = a_pass_ = b_pass_ = false;
        a_back_ = b_back_ = a_game_ = b_game_ = false;
        still_ = moved_ = false;
        held_ = 0.0f;
    }

    [[nodiscard]] Phase State() const {
        return phase_;
    }
    // A chord owns the buttons (held, or waiting for both to be let go).
    [[nodiscard]] bool Owns() const {
        return phase_ != Phase::Idle;
    }
    // A press is waiting for the other button.
    [[nodiscard]] bool Pending() const {
        return a_pend_ || b_pend_;
    }

private:
    void StartChord(Output& o, float gap, bool buffer) {
        phase_ = Phase::Chord;
        buffered_ = buffer; // the game saw neither press
        a_pend_ = b_pend_ = false;
        if (buffer) {
            a_pass_ = b_pass_ = false;
        }
        still_ = moved_ = false;
        held_ = 0.0f;
        o.gap_s = gap;
    }

    // The roll: toward the stick (`toward`: pushed now, or let go at most dir_memory_s ago),
    // else back.
    void Roll(Output& o, bool toward) {
        o.event = Event::Roll;
        o.held_s = held_;
        o.moved = toward;
        o.centred = !toward;
        o.roll_x = toward ? dir_x_ : 0.0f;
        o.roll_y = toward ? dir_y_ : -1.0f;
        o.adjust = false;
    }

    // What the game gets: `want`, and a held-back press kept down until it has lasted `min`.
    static bool Show(bool want, bool& back, bool& game, float& t, float dt, float min) {
        if (game) {
            t += dt;
        }
        const bool out = want || (game && back && t + 1e-5f < min);
        if (out && !game) {
            t = 0.0f;
        }
        game = out;
        if (!out) {
            back = false;
        }
        return out;
    }

    Phase phase_{Phase::Idle};
    bool a_was_{false};
    bool b_was_{false};
    bool a_pend_{false}; // pressed, held back up to its window for the other one
    bool b_pend_{false};
    bool a_pass_{false}; // reaching the game (alone)
    bool b_pass_{false};
    bool a_back_{false}; // the press reaching the game was held back (min_press_s applies)
    bool b_back_{false};
    bool a_game_{false}; // what the game got last frame, and for how long
    bool b_game_{false};
    float a_game_t_{0.0f};
    float b_game_t_{0.0f};
    float a_t_{10.0f}; // since the press
    float b_t_{10.0f};
    bool buffered_{true}; // this chord holds the buttons back (roll_chord=1)
    bool still_{false};
    bool moved_{false}; // roll_chord=0: the stick pushed while held
    float held_{0.0f};
    float dir_x_{0.0f};
    float dir_y_{-1.0f};
    float dir_age_{10.0f};
};

} // namespace VR::Roll
