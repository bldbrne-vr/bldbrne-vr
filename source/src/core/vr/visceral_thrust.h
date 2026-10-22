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

#include "core/vr/swing_detector.h"

// Physical viscerals (visceral_thrust=1, physical melee): after a gun parry (or a charged swing's
// back stagger) the visceral is a deliberate THRUST of the right hand, not any swing. Pure (no
// OpenXR or emulator dependencies) so synthetic hand paths drive it in the unit tests
// (tests/test_visceral_thrust.cpp).
//
// Detector: one hand motion at a time (from the head-relative hand speed rising above start_speed
// to it staying below end_speed). A motion is a thrust once the hand has moved min_travel along
// the blade (the controller's forward axis, the swing classifier's blade: grip -Z pitched by
// blade_pitch_deg) at a peak of min_speed or more along it, with the motion within align of the
// blade all the way (at most misaligned_max of fast travel off it), the blade turning less than
// max_turn (a swing turns its blade 90-170 degrees, a thrust hardly at all), the blade pointing
// ahead (within blade_fwd of where the head faces, within blade_max_elev of level: not raising the
// weapon) and within max_time of the motion's start. A pull back along the blade (a wind-up: the
// hand moving backward along it) starts the motion over, so wind-up and thrust is one thrust.
// Or by the hand's own path (the stab is good, the blade held off its line: a wrist turned, a
// hammer grip): min_travel toward where the head faces (the motion within path_fwd of it) at a
// peak of min_speed, with at most misaligned_max of fast travel off that cone, the hand's
// direction within max_path_bend of its starting one (a swing's hand arcs) and the blade within
// path_blade_max of that path; the hand moving back toward the body starts that path over. A motion that breaks both is a swing for good (Verdict::Swing); one thrust per
// motion, none within refractory of the last.
//
// WindowVerdict: whether the physical melee window of this stroke (core/vr/melee_window.h) is a
// thrust, for the game's side (core/vr/bloodborne_melee.cpp): while a visceral is open a new
// window waits for this verdict before its attack is created, so a thrust starts the visceral
// instead of hitting the staggered enemy with a light attack, and a swing still hits normally.
//
// Cue: the heartbeat on the right hand when a visceral opens (the game's side says a target the
// hunter hit is in its visceral stagger): one at the start (visceral_cue=1) or one every
// repeat_s while it stays open (visceral_cue=2, at most max_beats).
//
// Conventions: OpenXR LOCAL space (+x right, +y up, +z back), meters, seconds.
namespace VR::Melee::Thrust {

enum class Verdict : std::uint8_t { None, Pending, Thrust, Swing };

inline const char* VerdictName(Verdict v) {
    switch (v) {
    case Verdict::None:
        return "none";
    case Verdict::Pending:
        return "pending";
    case Verdict::Thrust:
        return "thrust";
    case Verdict::Swing:
        return "swing";
    }
    return "?";
}

struct Config {
    // The feel review (offline analysis):
    // 1.8 m/s / 12 cm / 37 deg missed a tentative 25 cm stab in 0.3 s (1.56 m/s peak), a seated
    // 18 cm jab (1.53) and a lunge (the head's 15-20 cm taken out: 1.56), and a stab with the blade
    // held 40 deg above the motion; a reach (1.25 m/s) and a slow push (1.2) stay no thrust at 1.5.
    // A false thrust only matters with a visceral open (it is the visceral), a missed one is the
    // move failing when you mean it: so the lenient side.
    float min_speed{1.5f};        // m/s: the hand's peak speed along the blade (head-relative)
    float min_travel{0.10f};      // m: the hand's travel along the blade
    float start_speed{0.6f};      // m/s: a motion starts
    float end_speed{0.4f};        // m/s: it ends (after end_hold below it)
    float end_hold{0.04f};        // s
    float fast_speed{0.9f};       // m/s: motion slower than this is not judged for its direction
    float align_cos{0.7071f};     // the motion within 45 degrees of the blade
    float back_cos{-0.5f};        // the motion this far backward along the blade: a pull back
    float max_turn{30.0f * Deg};  // the blade's turn over the motion
    float misaligned_max{0.06f};  // m of fast travel off the blade's axis: a swing
    float blade_fwd_cos{0.5f};    // the blade within 60 degrees of the head's facing (yaw)
    float blade_max_elev{50.0f * Deg}; // ... and within 50 degrees of level
    float path_fwd_cos{0.8660f};  // the hand's path: within 30 degrees of the head's facing
    float max_path_bend{8.0f * Deg};  // ... the hand's direction this close to its start
    float path_blade_max{60.0f * Deg}; // ... and the blade within this of the path
    float max_time{0.45f};        // s: a motion that is not a thrust by then is not one
    float refractory{0.35f};      // s after a thrust: no other

    static Config Make(float min_speed, float min_travel_cm, float align_deg, float max_turn_deg) {
        Config c{};
        c.min_speed = std::clamp(min_speed, 0.8f, 6.0f);
        c.min_travel = std::clamp(min_travel_cm, 4.0f, 40.0f) * 0.01f;
        c.align_cos = std::cos(std::clamp(align_deg, 10.0f, 60.0f) * Deg);
        c.max_turn = std::clamp(max_turn_deg, 10.0f, 90.0f) * Deg;
        return c;
    }

    // While a visceral is open (a staggered target in reach): a little easier, so a short or
    // slightly sloppy stab starts it (2026-10-18, the tester: easier, but not so much that a swing
    // ends in a visceral by mistake). 15% less speed and travel (1.5 m/s / 10 cm: 1.28 / 8.5, so
    // a reach at 1.25 m/s is still none), 5 degrees more off the blade's line, 25 more turn, 2 cm
    // more off its axis. A swing (its blade turning 90-170 degrees) still is none. The turn: the
    // user's real stabs at the Blood-starved Beast (2026-10-18 log) turned the blade 23-29 degrees
    // and the ones at its open viscerals were judged swings for turning it (40 then): 55 now.
    Config EasedForOpenVisceral() const {
        Config c = *this;
        c.min_speed = std::max(min_speed * 0.85f, 0.8f);
        c.min_travel = std::max(min_travel * 0.85f, 0.04f);
        const float align = std::acos(std::clamp(align_cos, -1.0f, 1.0f));
        c.align_cos = std::cos(std::min(align + 5.0f * Deg, 60.0f * Deg));
        c.max_turn = std::min(max_turn + 25.0f * Deg, 90.0f * Deg);
        c.misaligned_max = misaligned_max + 0.02f;
        return c;
    }
};

struct Input {
    double t{};       // seconds (the pose's predicted display time)
    bool active{};    // swings may attack now (the swing gate), physical melee
    bool valid{};     // the grip's position, linear velocity and orientation tracked
    V3 v{};           // grip linear velocity (LOCAL)
    V3 blade{};       // the blade's direction (LOCAL): the grip's rotation x the blade axis
    bool head_ok{};   // head pose valid
    V3 head_v{};      // head linear velocity
    V3 head_fwd{0.0f, 0.0f, -1.0f};
};

// The current (or the last) motion, for the log.
struct Stats {
    float travel{};     // m along the blade (forward)
    float peak{};       // m/s along the blade
    float turn{};       // rad, the blade's turn
    float misaligned{}; // m of fast travel off the blade's axis
    float time{};       // s since the motion (re)started
    float elev{};       // rad, the blade above level (at the verdict)
    float yaw_off{};    // rad, the blade off the head's facing (flat)
    float path_travel{}; // m of the hand's path toward the facing
    float path_peak{};   // m/s toward the facing
    float path_bend{};   // rad, the hand's direction off its path so far (the most)
    float path_off{};    // m of fast travel off the facing's cone
};

struct Output {
    bool fired{false};               // a thrust this frame
    bool by_path{false};             // ... by the hand's path (the blade off its line)
    Verdict verdict{Verdict::None};  // the current motion's (None: the hand at rest)
    const char* why{""};             // what decided it ("thrust", or why not)
    std::uint32_t motion{0};         // the current motion's id (from 1)
    Stats stats{};
};

class Detector {
public:
    Output Update(const Input& in, const Config& c) {
        Output out{};
        const double raw = have_t_ ? in.t - last_t_ : 0.0;
        have_t_ = true;
        last_t_ = in.t;
        const float dt = static_cast<float>(std::clamp(raw, 0.0, 0.05));
        since_fire_ += dt;
        if (!in.active || !in.valid || Length(in.blade) < 0.5f) {
            live_ = false;
            have_b_ = false;
            out.motion = motion_;
            return out;
        }
        const V3 b = Normalize(in.blade);
        const V3 rel = in.head_ok ? in.v - in.head_v : in.v;
        const float speed = Length(rel);
        const float along = Dot(rel, b);
        const float turn = have_b_ ? Angle(prev_b_, b) : 0.0f;
        prev_b_ = b;
        have_b_ = true;
        if (!live_) {
            if (speed < c.start_speed) {
                out.motion = motion_;
                return out;
            }
            Begin(true);
        }
        // The end of the motion: the hand slow for end_hold.
        if (speed < c.end_speed) {
            slow_ += dt;
            if (slow_ >= c.end_hold) {
                live_ = false;
                out.motion = motion_;
                out.stats = stats_;
                return out;
            }
        } else {
            slow_ = 0.0f;
        }
        // A pull back along the blade (a wind-up): the motion starts over from here.
        if (speed >= c.fast_speed && along <= c.back_cos * speed) {
            if (verdict_ == Verdict::Pending || verdict_ == Verdict::Swing) {
                Begin(false);
                why_ = "pulled back (a wind-up)";
            }
            out = Report();
            return out;
        }
        stats_.time += dt;
        stats_.turn += turn;
        if (along >= c.align_cos * speed) {
            stats_.travel += std::max(along, 0.0f) * dt;
            stats_.peak = std::max(stats_.peak, along);
        } else if (speed >= c.fast_speed) {
            stats_.misaligned += speed * dt;
        }
        const V3 fwd = Normalize(V3{in.head_fwd.x, 0.0f, in.head_fwd.z});
        const bool facing = Length(fwd) > 0.5f;
        // The hand's own path toward the facing; back toward the body: that path starts over.
        const float toward = facing ? Dot(rel, fwd) : 0.0f;
        if (speed >= c.fast_speed && toward <= c.back_cos * speed) {
            stats_.path_travel = stats_.path_peak = stats_.path_bend = stats_.path_off = 0.0f;
            stats_.time = 0.0f; // (a wind-up the blade route did not see)
            path_ = {};
            path_dir_ = {};
        } else if (facing) {
            // The bend against the path's first PathChordM (its direction then): a swing's hand
            // arcs off it at once; not summed frame to frame, so tracking noise does not add up.
            path_ = path_ + rel * dt;
            if (Length(path_dir_) < 0.5f && Length(path_) >= PathChordM) {
                path_dir_ = Normalize(path_);
            }
            if (speed >= c.fast_speed && Length(path_dir_) > 0.5f) {
                stats_.path_bend = std::max(stats_.path_bend, Angle(path_dir_, rel));
            }
            if (toward >= c.path_fwd_cos * speed) {
                stats_.path_travel += toward * dt;
                stats_.path_peak = std::max(stats_.path_peak, toward);
            } else if (speed >= c.fast_speed) {
                stats_.path_off += speed * dt;
            }
        }
        // The blade's aim: ahead and level enough.
        const V3 flat = Normalize(V3{b.x, 0.0f, b.z});
        stats_.elev = std::asin(std::clamp(b.y, -1.0f, 1.0f));
        stats_.yaw_off = facing && Length(flat) > 0.5f ? Angle(fwd, flat) : Pi;
        if (verdict_ == Verdict::Pending) {
            const char* blade_no = stats_.turn > c.max_turn ? "the blade turned (a swing)"
                                   : stats_.misaligned > c.misaligned_max
                                       ? "the hand moved off the blade's axis (a swing)"
                                       : nullptr;
            // (the blade turned before the hand went anywhere: a wrist flick, a slow swing)
            const bool path_no = !facing || stats_.path_bend > c.max_path_bend ||
                                 stats_.path_off > c.misaligned_max ||
                                 (blade_no != nullptr && stats_.path_travel < PathChordM);
            const bool by_blade =
                blade_no == nullptr && stats_.travel >= c.min_travel && stats_.peak >= c.min_speed;
            const bool by_path = !path_no && stats_.path_travel >= c.min_travel &&
                                 stats_.path_peak >= c.min_speed &&
                                 Angle(path_, b) <= c.path_blade_max;
            if (blade_no != nullptr && path_no) {
                Decide(Verdict::Swing, blade_no);
            } else if (stats_.time > c.max_time) {
                Decide(Verdict::Swing, "too slow (a push)");
            } else if (by_blade || by_path) {
                if (stats_.yaw_off > std::acos(c.blade_fwd_cos)) {
                    Decide(Verdict::Swing, "the blade not ahead");
                } else if (std::abs(stats_.elev) > c.blade_max_elev) {
                    Decide(Verdict::Swing, stats_.elev > 0.0f ? "the blade raised"
                                                              : "the blade lowered");
                } else if (since_fire_ < c.refractory) {
                    Decide(Verdict::Swing, "right after a thrust");
                } else {
                    Decide(Verdict::Thrust, by_blade ? "thrust" : "thrust (the hand's path)");
                    since_fire_ = 0.0f;
                    out.fired = true;
                    out.by_path = !by_blade;
                }
            }
        }
        const bool fired = out.fired, by_path = out.by_path;
        out = Report();
        out.fired = fired;
        out.by_path = by_path;
        return out;
    }

    void Reset() {
        *this = Detector{};
    }

    // A visceral just opened: the motion under way is judged again from here (the same motion), so
    // a stab begun as the stagger landed is not a swing for what the hand did before it (a wind-up's
    // turn, the gun's recoil).
    void Rejudge() {
        if (live_) {
            Begin(false);
            why_ = "judged again (a visceral opened)";
        }
    }

    [[nodiscard]] std::uint32_t Motion() const {
        return motion_;
    }

private:
    void Begin(bool new_motion) {
        live_ = true;
        if (new_motion) {
            ++motion_;
        }
        verdict_ = Verdict::Pending;
        why_ = "";
        slow_ = 0.0f;
        stats_ = {};
        path_ = {};
        path_dir_ = {};
    }

    void Decide(Verdict v, const char* why) {
        verdict_ = v;
        why_ = why;
    }

    Output Report() const {
        Output o{};
        o.verdict = live_ ? verdict_ : Verdict::None;
        o.why = why_;
        o.motion = motion_;
        o.stats = stats_;
        return o;
    }

    bool have_t_{false};
    double last_t_{0.0};
    bool have_b_{false};
    V3 prev_b_{};
    static constexpr float PathChordM = 0.02f; // m of path before its bend is judged
    V3 path_{}; // the hand's path since the motion (or its pull back) started
    V3 path_dir_{}; // ... its direction over the first PathChordM (zero: not yet)
    bool live_{false};
    std::uint32_t motion_{0};
    Verdict verdict_{Verdict::None};
    const char* why_{""};
    float slow_{0.0f};
    float since_fire_{1e3f};
    Stats stats_{};
};

// ---- The verdict of a melee window ---------------------------------------------------------------

struct WindowVerdict {
    std::uint32_t window{0};   // the window judged (0: none yet)
    Verdict verdict{Verdict::None};
    const char* why{""};
    float age{0.0f};           // s since that window opened
    float since_fire{1e3f};    // s since the detector's last thrust
    std::uint32_t fire_motion{0};

    // Every frame: `open_window` is the newest window's serial while it is open (0 when none is
    // open), `det` the detector's output this frame. A window opening up to `pre_s` after a thrust
    // of the same motion (a thrust's window opens at its commit, usually after the thrust) is that
    // thrust's; one with the hand at rest for `idle_s` after it opened is a swing (a wrist flick).
    void Step(std::uint32_t open_window, const Output& det, float dt, float pre_s = 0.25f,
              float idle_s = 0.05f) {
        since_fire += dt;
        if (det.fired) {
            since_fire = 0.0f;
            fire_motion = det.motion;
        }
        if (open_window != 0 && open_window != window) {
            window = open_window;
            age = 0.0f;
            const bool same_motion = det.verdict != Verdict::None && det.motion == fire_motion;
            if (det.fired || (since_fire <= pre_s && same_motion)) {
                Set(Verdict::Thrust, "thrust");
            } else if (det.verdict == Verdict::Swing) {
                Set(Verdict::Swing, det.why);
            } else {
                Set(Verdict::Pending, "");
            }
            return;
        }
        if (window == 0 || verdict != Verdict::Pending) {
            return;
        }
        age += dt;
        if (det.fired) {
            Set(Verdict::Thrust, "thrust");
        } else if (open_window == 0) {
            Set(Verdict::Swing, "the window closed");
        } else if (det.verdict == Verdict::Swing) {
            Set(Verdict::Swing, det.why);
        } else if (det.verdict == Verdict::None && age >= idle_s) {
            Set(Verdict::Swing, "no hand motion (a wrist flick)");
        }
    }

private:
    void Set(Verdict v, const char* w) {
        verdict = v;
        why = w;
    }
};

// ---- The heartbeat cue -----------------------------------------------------------------------------

struct Cue {
    std::uint32_t serial{0}; // the game's visceral-open count seen
    bool open{false};
    float since_beat{0.0f};
    int beats{0};

    // `mode` 0 off, 1 one beat when a visceral opens, 2 one every repeat_s while it stays open (at
    // most max_beats). `serial` counts the visceral openings (the game's side), `open` whether one
    // is open now. True: play a heartbeat now.
    bool Step(int mode, std::uint32_t game_serial, bool game_open, float dt, float repeat_s = 1.1f,
              int max_beats = 3) {
        since_beat += dt;
        const bool rising = game_serial != serial && game_open;
        serial = game_serial;
        open = game_open;
        if (mode <= 0 || !open) {
            if (!open) {
                beats = 0;
            }
            return false;
        }
        if (rising) {
            beats = 1;
            since_beat = 0.0f;
            return true;
        }
        if (mode >= 2 && beats > 0 && beats < max_beats && since_beat >= repeat_s) {
            ++beats;
            since_beat = 0.0f;
            return true;
        }
        return false;
    }
};

} // namespace VR::Melee::Thrust
