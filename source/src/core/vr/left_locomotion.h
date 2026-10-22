// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

// The left hand's locomotion in left_scheme=2 (the tester's 2026-10-05 layout; the presenter's
// UpdateGameplay):
//  - A left stick click latches the sprint: the game gets its sprint (Circle held) from the click
//    until the stick has been back near the centre (centre_dz) for centre_s. Steering never ends
//    it. The latch starts only with the stick past start_push (the game stands still below ~0.4,
//    and Circle held while standing is its backstep): a click with the stick centred or barely
//    pushed arms it for arm_s (it starts once the stick is pushed past start_push). Stamina and
//    the game's own rules are untouched (no stamina, no dash).
//  - The latch also ends when the game is not dashing although the stick is pushed ahead: its
//    dash (SpEffect 391, read from the game) off for no_dash_s with the jump window (390) off too.
//    Out of stamina, a swing or a shot that stopped the dash: Circle is let go (held, the game
//    dashes again by itself as soon as some stamina is back, and spends it) and a new click
//    sprints again. Ahead: the stick within dash_cone_deg of the hunter's facing (Frame::
//    facing_off_deg). Sideways or back of him (body_facing=1, a lock-on) the game strafes, which
//    never dashes: nothing is judged and the latch holds (steering a sprint never ends it); back
//    ahead, judged afresh (the game dashes again by itself). Judged once the game's dash was seen
//    (since the latch began, a leap's hold ended or the stick came back ahead), or dash_start_s
//    after that (the game takes ~0.35 s to break into its dash). A leap holds the judgement
//    until the game's jump (its clip a000_007800, Frame::jump) has started and ended (none by
//    leap_start_s: there was no jump), or for leap_hold_s without that reading: the jump turns
//    391 on for a moment as it starts, and its clip runs ~1.6 s before the game dashes again with
//    Circle held (measured in the mock). Real time throughout (the game's own clock: capped at
//    40 fps its jump still took 1.6 s); only the hold without the jump reading is stretched by the
//    game's frame time (Frame::game_rate), the safe side should a slower game run slow.
//    Ladders hold the latch too: a fast climb or slide is not a ground dash. The no-dash
//    judgement starts afresh after dismounting; centring the stick still ends the latch.
//  - A: not sprinting, the game's Circle as before (a tap dodges, a hold sprints too). Sprinting
//    (the latch, or within grace_s after it ended) with the stick within leap_cone_deg of the run
//    direction (or centred): a LEAP, the game's jump. Steering harder than that: a DODGE in the
//    stick's direction, out of the sprint (the latch ends). The A that did either is swallowed
//    until it is let go. The run direction is where the stick pointed run_delay_s ago: a curve
//    steered at up to ~400 deg/s stays inside the cone, a sudden push sideways or back is a dodge
//    for run_delay_s.
//  - A second click while sprinting (the latch, or A held with the stick past half way): a leap.
//    Not a bounce or a mash of the latching click: the latch must be second_click_s old and the
//    click let go for click_gap_s first (an earlier one is ignored).
// The game's jump (c0000.hks, the evasion function): a roll request (Arm_Rolling) while SpEffect
// 390 is on. The dash clips' TAE keeps SpEffect 391 on, which becomes 390 (0.2 s) through its
// replaceSpEffectId; the roll request is made when a short Circle press is let go. Measured in the
// mock (VR and keyboard alike): a tap let go 0.06 / 0.085 / 0.13 / 0.22 s after the sprint's own
// release jumped (a000_007800), one let go 0.40 / 0.50 / 0.55 s after it rolled (a000_007100);
// L3 while sprinting never jumped (the HKS never reads Arm_L3). So right after a sprint every
// Circle tap is a jump until about 0.3 s have passed: a dodge out of a sprint has to wait that
// long. The Sequencer below plays those Circle edges.
// Header-only and pure (no emulator dependencies): tests/test_left_locomotion.cpp.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace VR::Loco {

struct Config {
    float centre_dz{0.25f};     // the stick this close to the centre counts as let go ...
    float centre_s{0.12f};      // ... for this long: the sprint latch ends
    float start_push{0.5f};     // the latch starts with the stick past this (not a light push)
    float arm_s{0.4f};          // a click below start_push latches if pushed past it this soon
    float grace_s{0.08f};       // an A this soon after the latch ended still leaps
    float leap_cone_deg{60.0f}; // A with the stick within this of the run direction: a leap
    float run_delay_s{0.12f};   // the run direction: where the stick pointed this long ago
    float sprint_push{0.5f};    // A held with the stick past this: the game's own sprint
    float second_click_s{0.25f}; // a second click leaps once the latch is this old ...
    float click_gap_s{0.10f};    // ... and the click was let go this long before it
    float no_dash_s{0.4f};       // latched, the stick pushed ahead, no dash (391) this long: ends
    float dash_start_s{1.0f};    // ... once a dash was seen, or this long after the judging began
    float dash_cone_deg{50.0f};  // ahead: the stick within this of the hunter's facing
    float leap_hold_s{1.3f};     // a leap, no reading of the game's jump: not judged this long
                                 // (x 60 fps over the game's: Frame::game_rate)
    float leap_start_s{0.5f};    // a reading: the jump started this soon after the leap, or none
    float leap_max_s{3.0f};      // a reading: not judged at most this long (a reading stuck on)
};

// Why a leap's hold of the no-dash judgement ended (Out::leap_end).
enum class LeapEnd : std::uint8_t { None, Jump, NoJump, Timeout, Time };

struct Frame {
    bool enabled{false}; // gameplay, motion layout, left_scheme=2 (false: everything off)
    bool click{false};   // the left stick click (gated)
    bool a{false};       // A (gated)
    float x{0.0f};       // the left stick (x right, y forward)
    float y{0.0f};
    float dt{0.0f};
    // The game's SpEffects at its last update: 391 (dashing) and 390 (the jump window after it):
    // 1 on, 0 off, -1 unknown (no reading: the latch is never ended for want of a dash). The
    // presenter also passes -1 while a leap's Circle sequence runs (the dash lapses as it lets go).
    int dash{-1};
    int window{-1};
    // The stick's angle off the hunter's facing (deg, 0..180; the game's stick reference against
    // his yaw): beyond Config::dash_cone_deg the game strafes. Negative: unknown (ahead).
    float facing_off_deg{-1.0f};
    // The game's jump (its clip a000_007800 playing): 1 on, 0 off, -1 no reading.
    int jump{-1};
    // 1/60 over the game's frame time (1 at full speed): a leap's hold without the jump reading
    // (Config::leap_hold_s) counts dt x this, so it lasts longer in a slow game.
    float game_rate{1.0f};
    bool ladder{false}; // a fresh ladder reading: fast climbing/sliding has no ground dash
};

enum class Action : std::uint8_t { None, Leap, Dodge };

enum class Cause : std::uint8_t { None, A, Click };

struct Out {
    bool sprint{false};   // the latch holds Circle for the game this frame
    bool circle_a{false}; // A goes to the game as Circle this frame (the raw hold: dodge / sprint)
    bool started{false};  // the latch began this frame (from the click, or the armed click)
    bool armed{false};    // a click with the stick centred: armed this frame
    bool ended{false};    // the latch ended this frame (the stick centred, a dodge, or a reset)
    bool no_dash{false};  // ... because the game was not dashing (Config::no_dash_s; no grace)
    float no_dash_s{0.0f}; // no_dash: how long the game's dash had been off
    // Latched, the stick pushed: ahead of the hunter (judged) or a strafe (not judged; the latch
    // holds). `ahead_again`: back ahead this frame after a strafe (judged afresh).
    bool ahead{true};
    bool ahead_again{false};
    // A leap's hold of the judgement ended this frame (judged afresh from here), why and after how
    // long.
    LeapEnd leap_end{LeapEnd::None};
    float leap_held_s{0.0f};
    Action action{Action::None};
    Cause cause{Cause::None};
    float off_deg{0.0f}; // an A: the stick against the run direction (deg; 0 when centred)
    float dir_x{0.0f};   // a dodge: the stick's direction (unit)
    float dir_y{0.0f};
    bool from_latch{false};  // the action came while the latch held Circle (else in its grace or
                             // with A held as the game's own sprint)
    float since_end_s{0.0f}; // an A in the grace: how long ago the latch ended (Circle let go)
    // A click while latched that was too soon for a second-click leap (a bounce or a mash of the
    // latching click: Config::second_click_s / click_gap_s): ignored.
    bool click_ignored{false};
};

inline float AngleBetweenDeg(float ax, float ay, float bx, float by) {
    const float la = std::hypot(ax, ay);
    const float lb = std::hypot(bx, by);
    if (la < 1e-4f || lb < 1e-4f) {
        return 0.0f;
    }
    const float c = std::clamp((ax * bx + ay * by) / (la * lb), -1.0f, 1.0f);
    return std::acos(c) * 57.2957795f;
}

class Controller {
public:
    Out Update(const Config& c, const Frame& f) {
        Out o{};
        if (!f.enabled) {
            if (latched_) {
                o.ended = true;
            }
            Reset();
            // A button still held when it comes back is no new press.
            click_was_ = f.click;
            a_was_ = f.a;
            // Outside gameplay A is nobody's here: the caller's own layout decides.
            return o;
        }
        const float mag = std::hypot(f.x, f.y);
        const bool pushed = mag > c.centre_dz;
        const bool start_push = mag > c.start_push;
        const bool click_edge = f.click && !click_was_;
        const bool a_edge = f.a && !a_was_;
        click_was_ = f.click;
        a_was_ = f.a;
        // The game's own sprint, A held (no latch): a click is the second-click jump there too.
        const bool a_sprint = f.a && !a_swallow_ && mag > c.sprint_push;
        time_ += f.dt;
        if (latched_) {
            latch_age_ += f.dt;
        }
        // How long the click has been let go (a second click must come after a real release).
        click_up_ = f.click ? 0.0f : std::min(click_up_ + f.dt, 10.0f);

        if (grace_left_ > 0.0f) {
            grace_left_ = std::max(0.0f, grace_left_ - f.dt);
            since_end_ += f.dt;
        }
        if (click_edge) {
            if (latched_ && (latch_age_ + 1e-5f < c.second_click_s ||
                             click_up_before_ + 1e-5f < c.click_gap_s)) {
                o.click_ignored = true; // a bounce or a mash of the latching click
            } else if (latched_ || a_sprint) {
                o.action = Action::Leap;
                o.cause = Cause::Click;
                o.from_latch = latched_;
                StartLeapHold(); // the jump, then the run: judged afresh
            } else if (start_push) {
                Start(f, mag, o);
            } else {
                armed_left_ = c.arm_s;
                o.armed = true;
            }
        } else if (armed_left_ > 0.0f) {
            armed_left_ -= f.dt;
            if (start_push) {
                Start(f, mag, o);
            } else if (armed_left_ <= 0.0f) {
                armed_left_ = 0.0f;
            }
        }
        click_up_before_ = click_up_;

        // The latch ends once the stick has been back at the centre for centre_s.
        if (latched_ && !o.started) {
            if (pushed) {
                centred_s_ = 0.0f;
            } else {
                centred_s_ += f.dt;
                if (centred_s_ >= c.centre_s) {
                    End(o);
                    grace_left_ = c.grace_s;
                    since_end_ = 0.0f;
                }
            }
        }

        // ... and once the game has not been dashing (391 off) for no_dash_s with the stick pushed
        // ahead (a stick in the centre leaves it to the centre rule, no reading never judges, a
        // strafe never dashes). Only once its dash was seen or dash_start_s after the judging
        // began (the start, a leap's hold, the stick back ahead); never while the jump window
        // (390) is on. No grace: the game has no dash to leap out of. (A leap this frame, its
        // hold's end or the stick back ahead: judged afresh from the next frame.)
        if (latched_ && !o.started && o.action != Action::Leap) {
            if (leap_wait_) {
                LeapHold(c, f, o);
            }
            o.ahead = !pushed || f.facing_off_deg < 0.0f ||
                      f.facing_off_deg <= c.dash_cone_deg + 1e-3f;
            if (f.ladder) {
                RestartJudge(); // keep Circle on the ladder; allow the dash to start on exit
            } else if (leap_wait_ || o.leap_end != LeapEnd::None) {
                no_dash_t_ = 0.0f; // a leap's jump: nothing judged
            } else if (!o.ahead) {
                no_dash_t_ = 0.0f; // the game's strafe run: never a dash, the latch holds
                strafed_ = true;
            } else if (strafed_ && pushed) {
                strafed_ = false; // back ahead: the game dashes again by itself
                RestartJudge();
                o.ahead_again = true;
            } else {
                judge_age_ += f.dt;
                if (f.dash < 0) {
                    no_dash_t_ = 0.0f; // no reading: nothing judged
                } else if (f.dash == 1) {
                    seen_dash_ = true;
                    no_dash_t_ = 0.0f;
                } else if (pushed) {
                    no_dash_t_ += f.dt;
                    if (no_dash_t_ + 1e-5f >= c.no_dash_s && f.window != 1 &&
                        (seen_dash_ || judge_age_ + 1e-5f >= c.dash_start_s)) {
                        o.no_dash = true;
                        o.no_dash_s = no_dash_t_;
                        End(o);
                        grace_left_ = 0.0f;
                    }
                }
            }
        }

        // A: a leap or a dodge while sprinting (the latch or its grace); else the game's Circle.
        // The run direction: where the stick pointed run_delay_s ago (RunDirection).
        RunDirection(c);
        if (a_edge && (latched_ || grace_left_ > 0.0f)) {
            a_swallow_ = true;
            o.cause = Cause::A;
            o.from_latch = latched_;
            o.since_end_s = latched_ ? 0.0f : since_end_;
            o.off_deg = pushed ? AngleBetweenDeg(f.x, f.y, dir_x_, dir_y_) : 0.0f;
            if (!pushed || !dir_valid_ || o.off_deg <= c.leap_cone_deg) {
                o.action = Action::Leap;
                StartLeapHold();
            } else {
                o.action = Action::Dodge;
                o.dir_x = f.x / mag;
                o.dir_y = f.y / mag;
                if (latched_) {
                    End(o); // out of the sprint: a dodge never runs on
                }
                grace_left_ = 0.0f;
            }
        }
        // The stick's direction while it is pushed, for the run direction of the frames to come
        // (after this frame's A: an A is judged against where the run was going).
        if (pushed) {
            Remember(f.x / mag, f.y / mag);
        }

        if (!f.a) {
            a_swallow_ = false;
        }
        o.sprint = latched_;
        o.circle_a = f.a && !a_swallow_;
        return o;
    }

    void Reset() {
        latched_ = dir_valid_ = click_was_ = a_was_ = a_swallow_ = seen_dash_ = false;
        strafed_ = leap_wait_ = jump_seen_ = false;
        centred_s_ = armed_left_ = grace_left_ = since_end_ = latch_age_ = 0.0f;
        judge_age_ = no_dash_t_ = leap_t_ = leap_slow_t_ = 0.0f;
        click_up_ = click_up_before_ = 10.0f;
        dir_x_ = 0.0f;
        dir_y_ = 1.0f;
        hist_count_ = 0;
    }

    [[nodiscard]] bool Latched() const {
        return latched_;
    }
    [[nodiscard]] bool Sprinting() const {
        return latched_ || grace_left_ > 0.0f;
    }
    [[nodiscard]] float RunX() const {
        return dir_x_;
    }
    [[nodiscard]] float RunY() const {
        return dir_y_;
    }

private:
    void Start(const Frame& f, float mag, Out& o) {
        latched_ = true;
        o.started = true;
        armed_left_ = 0.0f;
        centred_s_ = 0.0f;
        grace_left_ = 0.0f;
        latch_age_ = 0.0f;
        RestartJudge();
        leap_wait_ = false;
        strafed_ = false;
        // The run starts where the stick points (no older steering counts).
        hist_count_ = 0;
        Remember(f.x / mag, f.y / mag);
        dir_x_ = f.x / mag;
        dir_y_ = f.y / mag;
        dir_valid_ = true;
    }
    void End(Out& o) {
        latched_ = false;
        o.ended = true;
        centred_s_ = 0.0f;
    }
    // The game's dash is judged afresh (the latch's start, a leap's hold ended, the stick back
    // ahead): not seen yet, no time off.
    void RestartJudge() {
        seen_dash_ = false;
        judge_age_ = 0.0f;
        no_dash_t_ = 0.0f;
    }
    // A leap: nothing judged until the game's jump has started and ended (LeapHold).
    void StartLeapHold() {
        RestartJudge();
        leap_wait_ = true;
        jump_seen_ = false;
        leap_t_ = leap_slow_t_ = 0.0f;
    }
    // One frame of a leap's hold: with a reading of the game's jump, until it has been seen and
    // is gone (none by leap_start_s: it was no jump; leap_max_s at the most); without one,
    // leap_hold_s stretched by the game's frame time (Frame::game_rate).
    void LeapHold(const Config& c, const Frame& f, Out& o) {
        leap_t_ += f.dt;
        leap_slow_t_ += f.dt * std::clamp(f.game_rate, 0.1f, 1.0f);
        jump_seen_ = jump_seen_ || f.jump == 1;
        LeapEnd end = LeapEnd::None;
        if (f.jump < 0) {
            end = leap_slow_t_ + 1e-5f >= c.leap_hold_s ? LeapEnd::Time : LeapEnd::None;
        } else if (jump_seen_) {
            end = f.jump == 0 ? LeapEnd::Jump : LeapEnd::None;
        } else if (leap_t_ + 1e-5f >= c.leap_start_s) {
            end = LeapEnd::NoJump;
        }
        if (end == LeapEnd::None && leap_t_ + 1e-5f >= c.leap_max_s) {
            end = LeapEnd::Timeout;
        }
        if (end != LeapEnd::None) {
            leap_wait_ = false;
            RestartJudge();
            o.leap_end = end;
            o.leap_held_s = leap_t_;
        }
    }

    // The stick's pushed directions (time_, unit), newest last.
    void Remember(float x, float y) {
        if (hist_count_ == Hist) {
            for (int i = 1; i < Hist; ++i) {
                hist_[i - 1] = hist_[i];
            }
            --hist_count_;
        }
        hist_[hist_count_++] = {time_, x, y};
    }
    // The run direction: the newest remembered direction at least run_delay_s old (the oldest one
    // while the run is younger than that). A curve steered at w deg/s is w x run_delay_s behind;
    // a sudden push sideways stays a full turn off for run_delay_s.
    void RunDirection(const Config& c) {
        if (hist_count_ == 0) {
            return; // nothing pushed yet: dir_ keeps its last value (dir_valid_ says if any)
        }
        int pick = 0;
        for (int i = hist_count_ - 1; i >= 0; --i) {
            if (time_ - hist_[i].t >= static_cast<double>(c.run_delay_s) - 1e-5) {
                pick = i;
                break;
            }
        }
        dir_x_ = hist_[pick].x;
        dir_y_ = hist_[pick].y;
        dir_valid_ = true;
    }

    struct Sample {
        double t{0.0};
        float x{0.0f};
        float y{0.0f};
    };
    static constexpr int Hist = 96; // > run_delay_s at 500 Hz
    Sample hist_[Hist]{};
    int hist_count_{0};
    double time_{0.0};

    bool latched_{false};
    bool dir_valid_{false};
    bool click_was_{false};
    bool a_was_{false};
    bool a_swallow_{false};
    float centred_s_{0.0f};
    float armed_left_{0.0f};
    float grace_left_{0.0f};
    float since_end_{0.0f};
    float latch_age_{0.0f};
    bool seen_dash_{false}; // the game's dash (391) seen since the judging began
    float judge_age_{0.0f}; // since the judging began (the latch, a leap's hold, back ahead)
    float no_dash_t_{0.0f}; // the game not dashing with the stick pushed ahead, this long
    bool strafed_{false};   // the stick was sideways or back of the hunter (the game's strafe)
    bool leap_wait_{false}; // a leap's hold: nothing judged (LeapHold)
    bool jump_seen_{false}; // ... the game's jump seen since the leap
    float leap_t_{0.0f};    // ... since the leap
    float leap_slow_t_{0.0f}; // ... the same, x Frame::game_rate (the hold without a reading)
    float click_up_{10.0f};        // the click has been let go this long (0 while held)
    float click_up_before_{10.0f}; // ... as of the previous frame (a click's edge reads it)
    float dir_x_{0.0f};
    float dir_y_{1.0f};
};

// The Circle edges the game needs for a leap or a dodge out of a sprint (see the top). Each frame
// the Sequencer says whether Circle is forced up (0), down (1) or left to the caller (-1: the
// latch, A).
//  - Leap: Circle up (the sprint let go), the tap pressed 1.5 game frames later and let go once it
//    has been seen (2 game frames) with the jump window on (SpEffect 390 read from the game), or
//    leap_release_s after the sprint's release without a reading. A reading that shows no dash at
//    all (no 391, no 390): let go at once (the game has no jump to give: it rolls, as its own
//    Circle tap would).
//  - Dodge: the whole tap after the window: pressed once 390 has been seen and is gone (a
//    reading), or tap_s before dodge_release_s after the sprint's release (none); let go 2 game
//    frames later (a reading) or at dodge_release_s.
struct SeqConfig {
    float leap_release_s{0.10f};  // no reading: the leap's tap let go this long after the release
    float leap_max_s{0.15f};      // a reading but no window by then: let go anyway (measured:
                                  // let go 0.06-0.22 s after the release jumps)
    float dodge_release_s{0.40f}; // no reading: the dodge's tap let go this long after it
    float dodge_max_s{0.60f};     // a reading: pressed and let go by then whatever 390 says
    float tap_s{0.1f};            // the dodge's press without a reading
    float min_down_s{0.03f};      // a tap is down at least this long ...
    float min_down_frames{2.0f};  // ... and this many game frames (the game must see it)
    float min_up_frames{1.5f};    // Circle up at least this long before the tap (a new edge)
    float gap_s{0.05f};           // Circle up after the tap before the caller's (and 1.5 frames)
};

struct SeqFrame {
    float dt{0.0f};
    float frame_s{1.0f / 60.0f}; // the game's frame time
    // The game's SpEffects at its last update: 390 (the jump window) and 391 (dashing): 1 on,
    // 0 off, -1 unknown (no reading).
    int window{-1};
    int dash{-1};
};

struct SeqOut {
    int circle{-1};        // -1: the caller's Circle, 0: up, 1: down
    bool pressed{false};   // the tap went down this frame
    bool released{false};  // the tap was let go this frame (the game's roll request)
    bool done{false};      // the sequence finished this frame
    bool by_window{false}; // released: decided by the SpEffect reading
    bool no_dash{false};   // released: the reading showed no dash (a leap became a roll)
    float since_s{0.0f};   // released: how long after the sprint's release
};

class Sequencer {
public:
    // Starts a leap or a dodge (replaces one under way). `circle_down`: the game sees Circle held
    // now (the sprint): it is let go first. `since_release_s`: Circle has been up this long
    // already (the grace after the latch ended; large when the hunter was not sprinting).
    void Start(Action action, bool circle_down, float since_release_s = 0.0f) {
        action_ = action;
        phase_ = Phase::Up;
        t_ = 0.0f;
        since_ = circle_down ? 0.0f : since_release_s;
        circle_down_ = circle_down;
        seen_window_ = false;
        first_ = true;
        game_sprint_ = -1;
    }

    [[nodiscard]] bool Active() const {
        return phase_ != Phase::Idle;
    }
    [[nodiscard]] Action Current() const {
        return action_;
    }

    void Cancel() {
        phase_ = Phase::Idle;
        action_ = Action::None;
    }

    SeqOut Update(const SeqConfig& c, const SeqFrame& f) {
        SeqOut o{};
        if (phase_ == Phase::Idle) {
            return o;
        }
        if (!first_) {
            t_ += f.dt;
            since_ += f.dt;
        }
        const float frame = std::max(f.frame_s, 1e-3f);
        const float min_up = circle_down_ ? c.min_up_frames * frame : 0.0f;
        const float min_down = std::max(c.min_down_s, c.min_down_frames * frame);
        if (first_) {
            first_ = false;
            // Is the game dashing (or in the window already)? Only a reading can say.
            if (f.window >= 0 && f.dash >= 0) {
                game_sprint_ = (f.window == 1 || f.dash == 1) ? 1 : 0;
            }
        }
        if (f.window == 1) {
            seen_window_ = true;
        }
        switch (phase_) {
        case Phase::Up: {
            o.circle = 0;
            bool go = t_ + 1e-5f >= min_up;
            if (action_ == Action::Dodge && game_sprint_ != 0) {
                // The whole tap after the window: with a reading, pressed once 390 has been seen
                // and is gone (never seen by dodge_release_s: there was none; dodge_max_s at the
                // latest); without one, pressed tap_s before dodge_release_s.
                if (f.window >= 0) {
                    go = go && ((f.window == 0 && seen_window_) ||
                                (!seen_window_ && since_ + 1e-5f >= c.dodge_release_s) ||
                                since_ + 1e-5f >= c.dodge_max_s);
                } else {
                    go = go && since_ + 1e-5f >= c.dodge_release_s - std::max(c.tap_s, min_down);
                }
            }
            if (go) {
                phase_ = Phase::Down;
                down_ = 0.0f;
                o.circle = 1;
                o.pressed = true;
            }
            break;
        }
        case Phase::Down: {
            o.circle = 1;
            down_ += f.dt;
            if (down_ + 1e-6f < min_down) {
                break;
            }
            bool release = false;
            if (game_sprint_ == 0) {
                release = true; // no dash: no jump to wait for, no window to avoid
                o.no_dash = true;
            } else if (action_ == Action::Leap) {
                if (f.window >= 0) {
                    release = f.window == 1 || since_ + 1e-5f >= c.leap_max_s;
                    o.by_window = f.window == 1;
                } else {
                    release = since_ + 1e-5f >= c.leap_release_s;
                }
            } else if (f.window >= 0) {
                // A dodge pressed after the window: let go once seen (a window back on would
                // make it a jump: wait, up to dodge_max_s).
                release = f.window == 0 || since_ + 1e-5f >= c.dodge_max_s;
                o.by_window = release && seen_window_;
            } else {
                release = since_ + 1e-5f >= c.dodge_release_s;
            }
            if (release) {
                phase_ = Phase::Gap;
                t_ = 0.0f;
                o.circle = 0;
                o.released = true;
                o.since_s = since_;
            }
            break;
        }
        case Phase::Gap:
            o.circle = 0;
            if (t_ + 1e-5f >= std::max(c.gap_s, 1.5f * frame)) {
                phase_ = Phase::Idle;
                action_ = Action::None;
                o.circle = -1;
                o.done = true;
            }
            break;
        case Phase::Idle:
            break;
        }
        return o;
    }

private:
    enum class Phase : std::uint8_t { Idle, Up, Down, Gap };
    Phase phase_{Phase::Idle};
    Action action_{Action::None};
    float t_{0.0f};
    float since_{0.0f};
    float down_{0.0f};
    int game_sprint_{-1};
    bool circle_down_{false};
    bool seen_window_{false};
    bool first_{false};
};

} // namespace VR::Loco
