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

// These grips can point the controller's sword axis away from a deliberate forward shove.
// Behaviour variations, not model ids. Dual blades qualify only while split. This policy is
// for visceral intent only: it never changes the ordinary swing/punch classifier or hit range.
inline constexpr bool ForwardAssistWeapon(std::int32_t variation, bool form_b) {
    return variation == 900 || variation == 1200 || // Beast Claw, Logarius' Wheel
           variation == 1000 || variation == 710 || // Rifle Spear, Saw Spear
           (form_b && (variation == 400 || variation == 2700)); // Blade of Mercy, Rakuyo
}

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
    // Selected grips at an open visceral: an additional straight-punch route, independent
    // of the controller's sword axis. Ordinary thrust recognition stays unchanged.
    bool hand_path{false};
    // The hand drawn back, up or aside before the stab is not judged (MotionDetector's CockMaxM).
    bool draw_free{false};
    float punch_travel{0.12f};
    float punch_bend{8.0f * Deg};
    // The feel review (2026-10-06, minimum-jerk human thrusts):
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
    float rise_weight{1.0f};      // fast travel off the line that RISES, once the hand went half
                                  // of min_travel ahead, counts this much (a stab ending upward)
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
    // 2026-10-27, the user: a tiny bit easier, more forgiving with the angle, above all a stab
    // that goes straight and then up; 2026-10-28: a tiny bit harder again, between the two: 20%
    // less travel (8 cm, the user), 8 degrees off the line (53), 30 more turn (60), 2.5 cm off its axis
    // (8.5), the hand's path bending 12 degrees, and travel off the line that rises counts 60%.
    // The speed and the blade's 50 degrees above level stay (a reach, a weapon raised fast, are
    // still none). 2026-10-30, the user: a little easier, but never by mistake: only shorter, 7 cm
    // (30% less travel); the speed, the angles and the turn, which keep swings and reaches out,
    // stay as on 10-28. 2026-11-06, the user: a TINY bit easier, the travel staying 7 cm ("I think
    // it's the angle"): 1 degree more off the blade's line (54, the user; 56 tried) and the hand's
    // path bending 14; the blade off the path stays 60 (65 let 32 swings of the sweep through), as
    // do the speed and the blade's turn, which keep swings and reaches out.
    Config EasedForOpenVisceral(bool claw_punch = false) const {
        Config c = *this;
        c.min_speed = std::max(min_speed * 0.85f, 0.8f);
        c.min_travel = std::max(min_travel * 0.70f, 0.04f);
        const float align = std::acos(std::clamp(align_cos, -1.0f, 1.0f));
        c.align_cos = std::cos(std::min(align + 9.0f * Deg, 60.0f * Deg));
        c.max_turn = std::min(max_turn + 30.0f * Deg, 90.0f * Deg);
        c.misaligned_max = misaligned_max + 0.025f;
        c.max_path_bend = max_path_bend + 6.0f * Deg;
        c.rise_weight = 0.6f;
        c.draw_free = true;
        if (claw_punch) {
            // Only the extra axis-independent route needs this stricter path. Keep the
            // eased blade thresholds above, so previously valid stabs are not delayed.
            c.hand_path = true;
            c.punch_travel = min_travel * 1.2f;
            c.punch_bend = max_path_bend;
        }
        return c;
    }

    Config ForWeapon(std::int32_t variation, bool form_b, bool opening) const {
        if (!opening) {
            return *this;
        }
        return EasedForOpenVisceral(ForwardAssistWeapon(variation, form_b));
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
    float ahead{};       // m of the hand's level travel toward the facing
};

struct Output {
    bool fired{false};               // a thrust this frame
    bool by_path{false};             // ... by the hand's path (the blade off its line)
    Verdict verdict{Verdict::None};  // the current motion's (None: the hand at rest)
    const char* why{""};             // what decided it ("thrust", or why not)
    std::uint32_t motion{0};         // the current motion's id (from 1)
    Stats stats{};
};

class MotionDetector {
public:
    Output Update(const Input& in, const Config& c, bool may_fire = true) {
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
        const V3 fwd = Normalize(V3{in.head_fwd.x, 0.0f, in.head_fwd.z});
        const bool facing = Length(fwd) > 0.5f;
        const float toward = facing ? Dot(rel, fwd) : 0.0f;
        // A wind-up starts the motion over. A claw punch uses the facing, not a sword axis.
        if (speed >= c.fast_speed &&
            (c.hand_path ? (facing && toward <= c.back_cos * speed)
                         : along <= c.back_cos * speed)) {
            if (verdict_ == Verdict::Pending || verdict_ == Verdict::Swing) {
                Begin(false);
                why_ = "pulled back (a wind-up)";
            }
            out = Report();
            return out;
        }
        stats_.time += dt;
        stats_.turn += turn;
        stats_.ahead += std::max(toward, 0.0f) * dt;
        if (along >= c.align_cos * speed) {
            stats_.travel += std::max(along, 0.0f) * dt;
            stats_.peak = std::max(stats_.peak, along);
        } else if (speed >= c.fast_speed) {
            stats_.misaligned += speed * dt * RiseWeight(rel - b * along, c);
        }
        // The hand's own path toward the facing; back toward the body: that path starts over.
        if (speed >= c.fast_speed && toward <= c.back_cos * speed) {
            stats_.path_travel = stats_.path_peak = stats_.path_bend = stats_.path_off = 0.0f;
            stats_.time = 0.0f; // (a wind-up the blade route did not see)
            path_ = {};
            path_dir_ = {};
        } else if (facing && (c.hand_path || c.draw_free) && toward < c.path_fwd_cos * speed &&
                   (c.hand_path || (along < c.align_cos * speed && stats_.travel < PathChordM)) &&
                   stats_.path_travel < PathChordM && cock_ < CockMaxM && cock_time_ < CockMaxS) {
            // A fist drawn up or aside before the punch (2026-11-15, the user's Beast Claw punches
            // at open viscerals in the Forbidden Woods: 21 cm ahead at 2.8 m/s "bent 90 degrees",
            // 12 cm at 2.2 "bent 152": the bend was measured from where the hand went FIRST, the
            // drawing back; only one straight back started the path over). Until the hand has
            // gone ahead, at most CockMaxM of fast travel and CockMaxS: not the punch's path yet.
            if (speed >= c.fast_speed) {
                cock_ += speed * dt;
            }
            cock_time_ += dt;
            // A blade that turns while the hand goes aside is a swing under way, not a hand
            // drawn back: no more of it is let off (a punch's fist has no blade to hold still).
            cock_turn_ += turn;
            if (!c.hand_path && cock_turn_ > CockMaxTurn) {
                cock_ = CockMaxM;
            }
            stats_ = {};
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
                stats_.path_off += speed * dt * RiseWeight(rel - fwd * toward, c);
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
                                 (!c.hand_path && blade_no != nullptr &&
                                  stats_.path_travel < PathChordM);
            const bool by_blade =
                !c.hand_path && blade_no == nullptr && stats_.travel >= c.min_travel &&
                stats_.peak >= c.min_speed;
            const bool by_path = !path_no && stats_.path_travel >= c.min_travel &&
                                 stats_.path_peak >= c.min_speed &&
                                 (c.hand_path || Angle(path_, b) <= c.path_blade_max);
            if ((c.hand_path || blade_no != nullptr) && path_no) {
                Decide(Verdict::Swing,
                       c.hand_path ? "the punch curved or moved off the facing" : blade_no);
            } else if (stats_.time > c.max_time) {
                Decide(Verdict::Swing, "too slow (a push)");
            } else if (by_blade || by_path) {
                if (!c.hand_path && stats_.yaw_off > std::acos(c.blade_fwd_cos)) {
                    Decide(Verdict::Swing, "the blade not ahead");
                } else if (!c.hand_path && std::abs(stats_.elev) > c.blade_max_elev) {
                    Decide(Verdict::Swing, stats_.elev > 0.0f ? "the blade raised"
                                                              : "the blade lowered");
                } else if (!may_fire || since_fire_ < c.refractory) {
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
        *this = MotionDetector{};
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
    // How much fast travel off the line counts: rise_weight for the part of it going up, once the
    // hand has gone half of min_travel ahead, level, toward the facing (straight, then up; a weapon
    // only raised counts all of it).
    float RiseWeight(V3 off, const Config& c) const {
        const float n = Length(off);
        if (n < 1e-4f || stats_.ahead < 0.5f * c.min_travel) return 1.0f;
        const float rise = std::clamp(off.y / n, 0.0f, 1.0f);
        return 1.0f - rise * (1.0f - c.rise_weight);
    }

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
        cock_ = cock_time_ = cock_turn_ = 0.0f;
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
    static constexpr float CockMaxM = 0.20f;    // a fist drawn back before its punch: fast travel
    static constexpr float CockMaxS = 0.30f;    // ... and time, not judged
    static constexpr float CockMaxTurn = 15.0f * Deg; // ... the blade's turn meanwhile (not a punch's)
    float cock_{0.0f}, cock_time_{0.0f}, cock_turn_{0.0f};
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

// Keep both histories: a blade wind-up and a fist wind-up have different axes. Sharing
// their rejection/reset state would let one route discard a valid motion in the other.
// Only selected grips at an open visceral run the extra detector. Both routes share one event,
// motion identity and refractory period; no allocations or changes to native hit rules.
class Detector {
public:
    Output Update(const Input& in, const Config& c) {
        const double raw = have_t_ ? in.t - last_t_ : 0.0;
        have_t_ = true;
        last_t_ = in.t;
        since_fire_ += static_cast<float>(std::clamp(raw, 0.0, 0.05));
        const bool may_fire = since_fire_ >= c.refractory;

        Config ordinary = c;
        ordinary.hand_path = false;
        ordinary.refractory = 0.0f; // the actual event, not a duplicate candidate, starts cooldown
        Output out = ordinary_.Update(in, ordinary, may_fire);
        Output punch{};
        // The extra route needs head-relative motion to distinguish a punch from a body
        // step. Lose that reference: discard its partial travel, rather than use stale aim.
        if (c.hand_path && in.head_ok) {
            Config extra = c;
            extra.min_travel = c.punch_travel;
            extra.max_path_bend = c.punch_bend;
            extra.refractory = 0.0f;
            punch = punch_.Update(in, extra, may_fire);
            punch_active_ = true;
        } else if (punch_active_) {
            punch_.Reset();
            punch_active_ = false;
        }
        const auto motion = out.motion;
        const float motion_age = std::max(out.stats.time, punch.stats.time);
        if (motion != motion_) {
            motion_ = motion;
            decided_ = Verdict::None;
        }
        if (out.verdict == Verdict::None) return out; // tracking lost, stopped or disabled
        if (decided_ != Verdict::None) {
            if (punch_active_ && used_punch_) out = punch;
            out.motion = motion;
            out.fired = false;
            out.verdict = decided_;
            out.why = why_;
            out.stats.time = motion_age;
            return out;
        }
        if (out.fired || punch.fired) {
            used_punch_ = !out.fired;
            if (used_punch_) out = punch;
            out.motion = motion;
            decided_ = Verdict::Thrust;
            why_ = out.why;
            out.verdict = decided_;
            out.why = why_;
            since_fire_ = 0.0f;
        } else if (may_fire && punch_active_ && out.verdict != Verdict::Pending &&
                   punch.verdict == Verdict::Pending) {
            // An axis rejection is not final while the straight-punch route is undecided.
            out = punch;
            out.motion = motion;
        }
        out.stats.time = motion_age;
        return out;
    }

    void Reset() { *this = Detector{}; }
    void Rejudge() {
        ordinary_.Rejudge();
        punch_.Rejudge();
        decided_ = Verdict::None;
    }
    [[nodiscard]] std::uint32_t Motion() const { return ordinary_.Motion(); }

private:
    MotionDetector ordinary_{}, punch_{};
    bool punch_active_{false}, used_punch_{false}, have_t_{false};
    std::uint32_t motion_{};
    Verdict decided_{Verdict::None};
    const char* why_{""};
    double last_t_{};
    float since_fire_{1e3f};
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
