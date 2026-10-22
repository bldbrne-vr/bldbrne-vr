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

// Melee swing classification for the motion-controlled weapon hand. Pure (no OpenXR, Vulkan
// or emulator dependencies) so synthetic swings can drive it in unit tests.
//
// SwingDetector is the single commit gate: head-relative hand speed, time and travel (body turns,
// headset slides and velocity spikes left out), plus direction gates that ignore wind-ups,
// raising the weapon and pulling back. Without a grip clutch (Config::clutchless: the presenter's
// right_grip=transform, where every hand motion is judged) a commit also needs the blade turning
// with the hand or a stab's speed, so reaching and gesturing never attack, and a stroke that
// brings the weapon back (lowering a raised weapon across the body, back to the guard: the blade
// trails the hand, pommel first, while the hand stays slower than a strike) is a recovery, never
// a swing, unless the hand speeds up to a strike's. Classifier then decides what a committed swing
// is:
//  - the blade did not start wound up: light (R1) at once, no added latency;
//  - it started wound up (blade behind the head or shoulder, or raised high / arm out wide):
//    wait for the blade's swept arc, integrated from grip orientation. Heavy (R2) once the arc
//    reaches heavy_arc with real arm motion through the front; light as soon as the stroke
//    cannot get there any more, and at the latest 0.30 s after the commit.
//
// Conventions: OpenXR LOCAL space (+x right, +y up, +z back), meters, radians, seconds. The
// blade axis is the grip -Z axis (the hilt), optionally pitched by blade_pitch_deg.
namespace VR::Melee {

constexpr float Pi = 3.14159265358979f;
constexpr float Deg = Pi / 180.0f;

struct V3 {
    float x{}, y{}, z{};
};

struct Q4 {
    float x{}, y{}, z{}, w{1.0f};
};

inline V3 operator+(V3 a, V3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline V3 operator-(V3 a, V3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline V3 operator*(V3 a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}
inline float Dot(V3 a, V3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline V3 Cross(V3 a, V3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float Length(V3 a) {
    return std::sqrt(Dot(a, a));
}
inline V3 Normalize(V3 a) {
    const float length = Length(a);
    return length > 1e-12f ? a * (1.0f / length) : V3{};
}
// Angle between two directions, accurate for small and large angles.
inline float Angle(V3 a, V3 b) {
    return std::atan2(Length(Cross(a, b)), Dot(a, b));
}
// v' = q * v * q^-1
inline V3 Rotate(const Q4& q, V3 v) {
    const V3 u{q.x, q.y, q.z};
    const float s = q.w;
    return u * (2.0f * Dot(u, v)) + v * (s * s - Dot(u, u)) + Cross(u, v) * (2.0f * s);
}
// Rotation about +Y (positive = toward the left).
inline V3 RotY(V3 v, float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}
inline float WrapPi(float a) {
    while (a > Pi) {
        a -= 2.0f * Pi;
    }
    while (a <= -Pi) {
        a += 2.0f * Pi;
    }
    return a;
}
inline float Smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// ---- Tuning (validated against synthetic min-jerk swings at 72/90/120 Hz) ----------------
// Blade stroke tracker.
constexpr float RateTau = 0.015f;  // blade angular rate EMA (finite differences are noisy)
constexpr float DecelTau = 0.020f; // deceleration EMA (stop rule)
constexpr float AxisTau = 0.060f;  // stroke axis EMA (reversal reference)
constexpr float RestRate = 1.0f;   // rad/s: below = blade at rest (tremor never builds arc)
constexpr float OnsetRate = 3.0f;  // rad/s: a stroke starts; the arc since rest is added back
constexpr float PreAxisCos = 0.3f; // pre-onset turnarounds restart the pre-onset arc
constexpr float EndRate = 1.5f;    // a stroke ends below max(EndRate, EndFrac * peak) ...
constexpr float EndFrac = 0.30f;
constexpr float EndHold = 0.030f;    // ... held this long
constexpr float ReversalCos = -0.3f; // rotation axis flips (wind-up -> strike) ...
constexpr float ReversalRate = 1.0f;
constexpr int ReversalFrames = 2;   // ... for this many frames
constexpr float GlitchRate = 35.0f; // rad/s per frame from (near) rest: a pose jump
constexpr float GlitchMax = 60.0f;  // rad/s per frame: never a human blade
constexpr float OriLost = 0.10f;    // orientation lost this long ends the stroke
constexpr float LongStroke = 1.5f;  // bounds the accumulators
constexpr float KeepEnded = 0.10f;  // a commit just after a stroke ended still uses it
// Wound-up start.
constexpr float WindupPoseAngle = 75.0f * Deg; // with the hand high or out wide
constexpr float ChordMin = 0.20f;              // ~11.5 deg of blade chord before a verdict
// Arbiter.
constexpr float ThrustArc = 25.0f * Deg; // smaller arc and slower blade = a stab
constexpr float ThrustRate = 5.0f;
constexpr float FrontAngle = 50.0f * Deg; // a heavy has to pass in front of the player
constexpr float HeavyRate = 5.0f;         // rad/s blade peak for a heavy
constexpr float Planarity = 0.6f;         // |sum n dtheta| / theta: cone stirring never counts
constexpr float LostTravelPerRad = 0.30f; // hand travel estimate while position is lost
constexpr float EarlyFrac = 0.80f;        // early heavy at 80% of the arc while still at peak
constexpr float EarlyRate = 0.97f;
constexpr float EarlyTravel = 0.75f;
constexpr float StopHold = 0.020f; // stop rule: past the peak, slowing, and the stopping
constexpr float StopRate = 0.9f;   // distance cannot reach the heavy arc
constexpr float StopDecel = 20.0f;
constexpr float StopFrac = 0.95f;
constexpr float TailRate = 0.4f; // fallback when the deceleration is noisy
constexpr float TailFrac = 0.85f;
constexpr float MaxWait = 0.30f;      // decision cap after the commit (< MinInterval)
constexpr float InactiveHold = 0.15f; // immersion lost this long decides a pending swing
// Ready click (the weapon is wound up for a heavy).
constexpr float ClickHandSpeed = 1.5f;
constexpr float ClickRefractory = 0.8f;
constexpr float ClickExit = 60.0f * Deg;
// Without a grip clutch (Config::clutchless) the hand alone has to look like a swing: the blade
// turning with it (a stroke of at least TurnArc) or a fast stab (StabSpeed). A reach, however
// quick, keeps the blade's direction and stays slower.
constexpr float TurnArc = 15.0f * Deg;
constexpr float StabSpeed = 3.0f;
// A recovery stroke: while the hand (head-relative) moves at least RecoverMinHand, the blade
// points against its motion (cos(blade, hand velocity) below RecoverTrailCos: the pommel leads, as
// when a raised weapon is lowered or brought back) and the hand is slower than RecoverStrikeHand.
// The stroke stays a recovery (no window, no commit) until the hand reaches RecoverStrikeHand (a
// wound-up chop whose wrist lags behind still gets there). A strike's blade sweeps across its path
// (edge or flat first, cos about 0) or leads it. QA (2026-10-04): a raised arm lowered across the
// body in 0.7 s (hand 2.2-2.4 m/s, blade 3.1-3.7 rad/s, cos -0.6 .. -0.25 while it sped up)
// attacked 3 of 3 times.
constexpr float RecoverMinHand = 1.0f;
constexpr float RecoverTrailCos = -0.25f;
constexpr float RecoverStrikeHand = 3.0f;
// A wind-up that flows into its strike without the blade's rotation ever reversing (looped out
// over the shoulder or behind the head, then over the top: the axis turns gradually, so the
// reversal test never fires) is one long stroke, and its chord from the guard reads the strike as
// a wind-up: the gate ignored the chop and stayed disarmed for the whole stroke. A stroke that took
// the blade at least ApexAway further from the front than it started, to at least ApexMin from it
// (WindupPoseAngle: high, back, out wide or low), and has then come back toward the front by
// Config::windup_turn from that furthest point while still turning at OnsetRate, splits there as a
// reversal does: the strike is a stroke of its own from the wind-up's furthest point (the
// headset session of 2026-10-07: 9 of 24 windows aborted "not a swing (ignored)" 8-58 ms after
// they opened, tip speeds up to 19 m/s, one stroke 235 deg over 867 ms).
constexpr float ApexMin = WindupPoseAngle;
constexpr float ApexAway = 40.0f * Deg;

// The weapon hand at the face (the presenter's SwingGate, right_grip=transform): within `enter`
// of the eyes (out again past `exit`) and not more than `behind` behind them. A hand comes to the
// headset and stops there, so it counts only once it is slower than `enter_speed` (m/s, relative
// to the head) inside (swing_face_enter_speed; 0 = at any speed): a swing passing the face at
// speed, a chop from beside or behind the head, is not a hand at the headset.
struct FaceRule {
    float behind{-0.05f}; // m along the view
    float enter{0.20f};   // m
    float exit{0.26f};    // m
    float enter_speed{1.5f};
};

inline bool HandAtFace(bool was, float ahead, float distance, float speed, const FaceRule& r) {
    const bool inside = ahead > r.behind && distance < (was ? r.exit : r.enter);
    return inside && (was || r.enter_speed <= 0.0f || speed < r.enter_speed);
}

// Rebuilt every frame from the settings.
struct Config {
    bool arc_enabled{false};           // heavy_arc setting && the grip pose exists
    bool ready_tick{true};             // soft click when wound up
    float heavy_arc{140.0f * Deg};     // blade sweep for a heavy
    float windup{100.0f * Deg};        // blade angle from horizontal-forward = wound up
    float hand_up{-0.05f};             // m relative to eye height (wound-up pose)
    float hand_side{0.40f};            // m to the right (wound-up pose)
    float heavy_travel{0.45f};         // m of head-relative hand travel for a heavy
    float heavy_speed{2.5f};           // m/s peak hand speed for a heavy
    V3 blade_local{0.0f, 0.0f, -1.0f}; // blade axis in grip space
    // No grip clutch (right_grip=transform): every motion of the hand is judged, so a commit
    // also needs the blade turning (TurnArc) or a stab's speed (StabSpeed). Set after Make.
    bool clutchless{false};
    // m/s: the stab's speed that commits a clutchless stroke whose blade hardly turns (StabSpeed;
    // the left hand with a gun: GunPunchSpeed, 2026-10-18). Set after Make.
    float stab_speed{StabSpeed};
    // swing_windup_turn_deg: the blade back toward the front this far from a wind-up's furthest
    // point starts the strike as a stroke of its own (ApexMin / ApexAway; 0 = off: a wind-up
    // looped into its strike stays one stroke). Set after Make.
    float windup_turn{25.0f * Deg};
    // swing_toward_front_deg: a stroke that started with the blade away from the front (ApexMin)
    // and has brought it at least this much closer to the front (its angle from
    // horizontal-forward) is never a wind-up, whichever way it moved (a chop from behind the head
    // first rises over the top; 0 = off: the chord's direction alone). Set after Make.
    float toward_front{10.0f * Deg};
    // swing_windup_rejudge: a stroke ignored as a wind-up is judged again while it lasts
    // (SwingDetector::rejudge; false: it never commits). Set after Make.
    bool rejudge{true};

    static Config Make(bool arc_enabled, bool ready_tick, float heavy_arc_deg,
                       float heavy_windup_deg, bool seated, float blade_pitch_deg) {
        Config c{};
        c.arc_enabled = arc_enabled;
        c.ready_tick = ready_tick;
        c.heavy_arc = heavy_arc_deg * Deg * (seated ? 0.85f : 1.0f);
        c.windup = (heavy_windup_deg - (seated ? 5.0f : 0.0f)) * Deg;
        c.hand_up = seated ? -0.10f : -0.05f;
        c.hand_side = seated ? 0.32f : 0.40f;
        c.heavy_travel = seated ? 0.34f : 0.45f;
        c.heavy_speed = seated ? 2.2f : 2.5f;
        const float p = blade_pitch_deg * Deg;
        c.blade_local = {0.0f, std::sin(p), -std::cos(p)};
        return c;
    }
};

struct Input {
    double t{};                // seconds (XrTime * 1e-9) these poses were predicted for
    bool active{};             // gestures on and the weapon drawn
    bool immersive{true};      // gameplay immersive (drops shorter than InactiveHold are ignored)
    bool heavy_idle{true};     // the trigger heavy machine is idle (after this frame's edge)
    bool trigger_heavy_edge{}; // right trigger pressed this frame as a heavy input
    bool ori_valid{};          // right grip orientation valid
    Q4 q{};
    bool pos_valid{}; // right grip position tracked and linear velocity valid
    V3 p{}, v{};      // right grip position / linear velocity
    // Where the hand is for the pose tests (wound up: Config::hand_up / hand_side), when the
    // motion point `p` is not the hand itself (a two-handed weapon's lever point).
    bool pose_valid{};
    V3 pose_p{};
    bool head_ok{};   // head pose valid
    V3 head_p{}, head_v{}, head_fwd{0.0f, 0.0f, -1.0f};
};

// Commit = a swing that committed while the trigger heavy machine is busy (it decides).
// Ignored / Rejected are commit-gate verdicts (Output::gate).
enum class Kind : unsigned char { None, Light, Heavy, Commit, Ignored, Rejected };
enum class End : unsigned char { None, Pause, Reversal, Long, Tracking };

// One blade stroke: from motion onset to a pause, reversal or tracking loss.
struct Stroke {
    unsigned id{};
    V3 b0{}, h0{}, b{};      // blade at the start / now, hand (head-yaw frame) at the start
    float yaw0{}, yaw{};     // head yaw at the start / now
    float alpha0{};          // blade angle from horizontal-forward at the start
    float theta{};           // swept arc (sum of frame-to-frame blade angles)
    float T{};               // time since onset
    float peak{}, t_peak{};  // peak blade rate and when it happened
    float travel{}, vpeak{}; // head-relative hand path length and peak speed
    float amin{};            // smallest blade angle from forward (passed through the front)
    float slow{};            // time below the end rate
    float end_age{};         // time since the stroke ended
    V3 A{};                  // sum of axis * dtheta (planarity, turn direction)
    V3 axis{};               // smoothed rotation axis
    bool cocked{};           // started wound up
    bool recovery{};         // the blade trailed a slow hand: bringing the weapon back
    bool strike_speed{};     // the hand reached RecoverStrikeHand (never a recovery after that)
    bool pos_lost{};         // hand position was lost during the stroke
    int rev{};               // consecutive reversed frames
    End ended{End::None};
    // The blade's furthest point from the front so far (Config::windup_turn): its angle from
    // horizontal-forward, the blade, the hand, and the stroke's sums there.
    float apex{};
    V3 apex_b{}, apex_h{}, apex_A{};
    bool apex_pos{true};
    float apex_yaw{}, apex_theta{}, apex_T{}, apex_travel{};
};

// Snapshot for the log (angles in degrees).
struct Stats {
    unsigned id{};
    float theta{}, theta_eff{}, H{};
    float peak{}, w{}, decel{}; // rad/s, rad/s^2
    float travel{}, vpeak{};    // m, m/s
    float planar{};
    float alpha0{}, amin{};
    float ms_commit{}, ms_onset{};
    bool cocked{}, travel_est{};
    V3 chord{};
    float hand_peak{}, hand_travel{}, hand_ms{}; // commit gate
};

// A frame can carry both a gate verdict (a new swing ignored or rejected) and the decision of
// an earlier, pending one.
struct Output {
    Kind kind{Kind::None}; // Light, Heavy or Commit
    const char* reason{""};
    Kind gate{Kind::None}; // Ignored or Rejected (logs and haptics only)
    const char* gate_reason{""};
    bool consume_trigger{false}; // heavy from a trigger press during a pending decision
    float sustain{0.0f};         // > 0 while an arc decision is pending (haptic ramp)
    bool ready_click{false};
    // The commit gate passed this frame (also when the decision of a wound-up stroke is still
    // pending): physical melee's "the swing is real" (core/vr/melee_window.h).
    bool fired{false};
    unsigned fired_stroke{0}; // ... for this blade stroke (0: none)
    Stats stats{};
};

struct ChordResult {
    const char* windup{nullptr}; // "raising", "cocking", "outward" or none
    bool strike_like{false};     // exempt from the "toward the body" test
    V3 db{};                     // blade chord in the head-yaw frame
};

// Commit gate (v1.6 of the original head-relative hand-speed detector). A swing commits once
// the hand has moved faster than FireSpeed for MinTime and covered MinTravel. The hand has to
// slow below RearmSpeed before the next swing, so waggling cannot spam attacks; a new blade
// stroke after an ignored wind-up re-arms it at once so a wind-up can flow into the strike. With
// `rejudge` (swing_windup_rejudge) a stroke ignored as a wind-up (its chord) is judged again every
// frame while it lasts and the hand stays fast, and commits once its chord no longer reads as one
// (a chop whose blade still rose over the top or cocked back while the hand already struck, QA
// 2026-10-07); ignored for its direction (toward the body, upward) or rejected, it never does.
struct SwingDetector {
    static constexpr float FireSpeed = 2.0f;
    static constexpr float RearmSpeed = 1.4f;
    static constexpr float MinTime = 0.04f;
    static constexpr float MinTravel = 0.15f;
    static constexpr float MinInterval = 0.35f;
    static constexpr float OwnSpeed = 1.4f;  // without a body turn's carry (m/s)
    static constexpr float WorldFrac = 0.5f; // world hand speed / FireSpeed (head-only motion)
    static constexpr float PathFrac = 0.5f;  // hand displacement / velocity-integrated travel

    enum class Result { None, Fire, Ignored, Rejected };

    float above_time{};
    float travel{};
    float peak{};
    float since_fire{10.0f};
    float invalid_time{};
    float recover_time{};
    bool armed{true};
    int ignored_stroke{-1};  // stroke whose commit was ignored (-1 = none)
    bool rejudge{true};      // Config::rejudge
    bool windup_wait{false}; // ignored_stroke was ignored as a wind-up: judged again while it lasts
    const char* reason{""};
    V3 p_last{};  // head-relative hand position, last valid frame
    V3 p_start{}; // ... where the current above-FireSpeed run started

    void Reset() {
        above_time = travel = peak = 0.0f;
    }

    // `valid`: hand position tracked and velocity valid. `v` / `p`: head-relative hand
    // velocity / position. `own_speed`: |v| without a body turn's carry. `world_speed`: the
    // hand's speed in LOCAL space. `fwd_flat`: horizontal head forward. `cur`: the blade stroke
    // in progress (or null). `clutchless`: see Config::clutchless.
    template <typename ChordFn>
    Result Update(bool valid, V3 v, float own_speed, float world_speed, V3 p, V3 fwd_flat, float dt,
                  const Stroke* cur, ChordFn&& chord, bool clutchless = false,
                  float stab_speed = StabSpeed) {
        // A recovery stroke (Stroke::recovery) is not yet a swing: keep measuring while the hand
        // stays fast; it may still speed up to a strike.
        const bool recovery = cur != nullptr && cur->recovery;
        since_fire += dt;
        if (!valid) {
            // Quest controllers drop out briefly in big wind-ups: freeze, then reset.
            invalid_time += dt;
            if (invalid_time > 0.15f) {
                Reset();
                recover_time = 0.1f;
            }
            return Result::None;
        }
        invalid_time = 0.0f;
        const V3 p_prev = p_last;
        p_last = p;
        if (recover_time > 0.0f) {
            recover_time -= dt;
            return Result::None;
        }
        const float speed = Length(v);
        if (speed < RearmSpeed) {
            armed = true;
            ignored_stroke = -1;
            windup_wait = false;
            Reset();
            return Result::None;
        }
        if (!armed) {
            if (ignored_stroke != -1 && cur != nullptr &&
                static_cast<int>(cur->id) != ignored_stroke) {
                armed = true; // a new stroke after an ignored wind-up (or the draw)
                ignored_stroke = -1;
                windup_wait = false;
                Reset();
            } else if (!(windup_wait && cur != nullptr &&
                         static_cast<int>(cur->id) == ignored_stroke)) {
                return Result::None;
            }
            // (else the stroke ignored as a wind-up, judged again)
        }
        // Not a swing: the hand carried by a body turn, or a head-relative speed the hand does
        // not have in the room (the headset moving: re-localization slide, bump).
        if (speed < FireSpeed || own_speed < OwnSpeed || world_speed < WorldFrac * FireSpeed) {
            Reset();
            return Result::None;
        }
        if (above_time == 0.0f) {
            p_start = p_prev;
        }
        above_time += dt;
        travel += speed * dt;
        peak = std::max(peak, speed);
        if (above_time < MinTime || travel < MinTravel) {
            return Result::None;
        }
        if (Length(p - p_start) < PathFrac * travel) {
            // The reported velocity is not matched by the hand's positions (velocity spike).
            Reset();
            return Result::None;
        }
        if (clutchless && (cur == nullptr || cur->theta < TurnArc) && peak < stab_speed) {
            // Not yet a swing (a reach, a quick gesture): keep measuring while the hand stays
            // fast; the blade may still turn or the stab speed up.
            return Result::None;
        }
        if (clutchless && recovery) {
            reason = "recovery";
            return Result::None;
        }
        armed = false;
        const ChordResult ch = chord();
        const int stroke = cur != nullptr ? static_cast<int>(cur->id) : 0;
        if (windup_wait && stroke == ignored_stroke && !ch.strike_like) {
            return Result::None; // not (yet) a strike's chord: judged again next frame
        }
        if (ch.windup != nullptr) {
            reason = ch.windup;
            ignored_stroke = stroke;
            windup_wait = rejudge && stroke != 0;
            return Result::Ignored;
        }
        windup_wait = false; // from here on this stroke's verdict is final
        // Motion toward the body is a pull-back, unless the blade chord says it is a strike
        // and the hand moves mostly down (a chop that commits below shoulder height moves the
        // hand down and back; pulling the weapon back to the body moves it mostly back).
        const float flat = std::sqrt(v.x * v.x + v.z * v.z);
        const float forward = std::sqrt(fwd_flat.x * fwd_flat.x + fwd_flat.z * fwd_flat.z);
        const bool chop = ch.strike_like && -v.y >= flat;
        if (!chop && flat > 0.1f * speed && forward > 0.1f &&
            (v.x * fwd_flat.x + v.z * fwd_flat.z) / (flat * forward) < -0.8f) {
            reason = "toward the body";
            ignored_stroke = stroke;
            return Result::Ignored;
        }
        if (v.y > 0.75f * speed) {
            reason = "upward";
            ignored_stroke = stroke;
            return Result::Ignored;
        }
        // Rejected or fired: the hand has to slow before the next one, whatever stroke comes (a
        // stroke judged again after a wind-up never re-arms the gate for the next).
        ignored_stroke = -1;
        if (since_fire < MinInterval) {
            reason = "too soon";
            return Result::Rejected;
        }
        since_fire = 0.0f;
        reason = "";
        return Result::Fire;
    }
};

class Classifier {
public:
    // Call once per XR frame, also while the weapon is not drawn (the rest pose must be known
    // when it is drawn).
    Output Update(const Input& in, const Config& cfg) {
        Output out{};
        if (have_t && in.t <= t_prev) {
            return out;
        }
        const float dtx =
            have_t ? std::clamp(static_cast<float>(in.t - t_prev), 0.001f, 0.1f) : 1.0f / 90.0f;
        const bool first = !have_t;
        t_prev = in.t;
        have_t = true;
        toward_front = cfg.toward_front; // Chord()

        // Head yaw (+ = turned left); kept while looking straight up or down.
        const float yaw_before = yaw;
        if (in.head_ok && std::hypot(in.head_fwd.x, in.head_fwd.z) >= 0.2f) {
            yaw = std::atan2(-in.head_fwd.x, -in.head_fwd.z);
        }
        const float turn_rate = first ? 0.0f : WrapPi(yaw - yaw_before) / dtx;
        fwd_flat = {-std::sin(yaw), 0.0f, -std::cos(yaw)};
        const bool pos_ok = in.pos_valid && in.head_ok;
        const V3 p_rel = in.p - in.head_p;
        const V3 v_rel = in.v - in.head_v;
        const V3 h = RotY((in.pose_valid ? in.pose_p : in.p) - in.head_p, -yaw);
        h_now = h;
        pos_now = pos_ok;

        // A. Blade stroke tracker (also the clutchless commit's "is the blade turning").
        if (cfg.arc_enabled || cfg.clutchless) {
            Track(in, cfg, dtx, pos_ok, h, v_rel);
        } else if (have_prev || has_cur || has_last || pending.active) {
            DropTracker();
        }

        // A2. Ready click: the weapon is held still in a wound-up pose.
        since_click += dtx;
        if (in.active && in.immersive && in.heavy_idle && cfg.arc_enabled && cfg.ready_tick &&
            have_prev && pos_ok && Length(v_rel) < ClickHandSpeed) {
            const bool pose =
                alpha >= cfg.windup || (alpha >= WindupPoseAngle &&
                                        (h.y >= cfg.hand_up || std::abs(h.x) >= cfg.hand_side));
            if (pose && !in_zone && since_click >= ClickRefractory) {
                out.ready_click = true;
                since_click = 0.0f;
                in_zone = true;
            }
        }
        if (in_zone && alpha < ClickExit) {
            in_zone = false;
        }

        // B. Commit gate. A short loss of immersion (a hitch, a frame without a camera tick)
        // keeps the swing going; a released grip decides a pending swing at once, immersion
        // lost for InactiveHold too.
        inactive_time = in.immersive ? 0.0f : inactive_time + dtx;
        if (!in.active || inactive_time >= InactiveHold) {
            if (pending.active) {
                const float wait = pending.wait;
                out = Finalize(in.active ? "not immersive" : "released", cfg);
                out.stats.ms_commit = wait * 1000.0f;
            }
            det.Reset();
            return out;
        }
        // A body turn carries the hand around the head; the hand's own speed leaves that out.
        // Only motion the hand really has along the turn is removed, so a head-only turn (hand
        // still) never adds speed.
        V3 v_own = v_rel;
        const V3 carry = Cross(V3{0.0f, 1.0f, 0.0f}, p_rel) * turn_rate;
        if (const float c = Length(carry); c > 1e-3f) {
            const V3 u = carry * (1.0f / c);
            v_own = v_own - u * std::clamp(Dot(v_rel, u), 0.0f, c);
        }
        const Stroke* cur_ptr = has_cur ? &cur : nullptr;
        det.rejudge = cfg.rejudge;
        const auto result = det.Update(
            pos_ok, v_rel, Length(v_own), Length(in.v), p_rel, fwd_flat, dtx, cur_ptr,
            [this] { return Chord(); }, cfg.clutchless, cfg.stab_speed);

        // C. Arbiter.
        const Stroke* decided = nullptr;
        switch (result) {
        case SwingDetector::Result::Ignored:
            out.gate = Kind::Ignored;
            out.gate_reason = det.reason;
            break;
        case SwingDetector::Result::Rejected:
            out.gate = Kind::Rejected;
            out.gate_reason = det.reason;
            break;
        case SwingDetector::Result::Fire: {
            out.fired = true;
            out.fired_stroke =
                has_cur ? cur.id : (has_last && last.end_age <= KeepEnded ? last.id : 0);
            if (pending.active) {
                // Cannot happen (MinInterval 0.35 s > MaxWait 0.30 s); this commit is dropped.
                const bool click = out.ready_click;
                out = Finalize("busy", cfg);
                out.ready_click = click;
                return out;
            }
            if (!in.heavy_idle) {
                out.kind = Kind::Commit;
                break;
            }
            const Stroke* s =
                has_cur ? &cur : (has_last && last.end_age <= KeepEnded ? &last : nullptr);
            decided = s;
            if (!cfg.arc_enabled) {
                Set(out, Kind::Light, "arc off");
            } else if (s == nullptr || (s->theta < ThrustArc && s->peak < ThrustRate)) {
                Set(out, Kind::Light, "thrust");
            } else if (!s->cocked) {
                Set(out, Kind::Light, "guard"); // never heavy, no added latency
            } else if (s->ended != End::None) {
                Set(out, HeavyOK(*s, 1.0f, 1.0f, cfg) ? Kind::Heavy : Kind::Light, "ended");
            } else {
                pending = {true, s->id, 0.0f, true};
            }
            break;
        }
        default:
            break;
        }

        // D. Pending decision (also on the commit frame).
        float ms_commit = 0.0f;
        if (pending.active && out.kind == Kind::None) {
            if (pending.fresh) {
                pending.fresh = false;
            } else {
                pending.wait += dtx;
            }
            ms_commit = pending.wait * 1000.0f;
            const Stroke* s = Find(pending.id);
            decided = s;
            const float H = cfg.heavy_arc;
            if (s == nullptr) {
                Set(out, Kind::Light, "lost");
            } else if (in.trigger_heavy_edge) {
                Set(out, Kind::Heavy, "trigger");
                out.consume_trigger = true;
            } else if (HeavyOK(*s, 1.0f, 1.0f, cfg)) {
                Set(out, Kind::Heavy, "arc");
            } else if (s->ended == End::None && w >= EarlyRate * s->peak &&
                       HeavyOK(*s, EarlyFrac, EarlyTravel, cfg)) {
                Set(out, Kind::Heavy, "early");
            } else if (s->ended != End::None) {
                Set(out, Kind::Light, s->ended == End::Tracking ? "tracking" : "end");
            } else if (s->T - s->t_peak >= StopHold && w < StopRate * s->peak &&
                       decel >= StopDecel && ThetaEff(*s) + w * w / (2.0f * decel) < StopFrac * H) {
                Set(out, Kind::Light, "stop");
            } else if (w < TailRate * s->peak && ThetaEff(*s) < TailFrac * H) {
                Set(out, Kind::Light, "tail");
            } else if (pending.wait >= MaxWait) {
                Set(out, Kind::Light, "cap");
            }
            if (out.kind != Kind::None) {
                pending.active = false;
            }
        }
        if (pending.active) {
            // Rising buzz while a wound-up swing builds arc.
            const Stroke* s = Find(pending.id);
            const float arc = s != nullptr ? ThetaEff(*s) : 0.0f;
            out.sustain = 0.06f + 0.24f * Smoothstep(0.5f * cfg.heavy_arc, cfg.heavy_arc, arc);
        }
        out.stats = Snapshot(decided != nullptr ? decided : (has_cur ? &cur : nullptr), cfg);
        out.stats.ms_commit = ms_commit;
        return out;
    }

    // Context switch, recenter, focus loss: drop everything without a press.
    void Reset() {
        *this = Classifier{};
    }

    // The weapon was just drawn: a hand already moving must slow down first, or start a new
    // blade stroke (a swing begun while the hand was untracked must not be lost).
    void OnDraw() {
        det.armed = false;
        det.ignored_stroke = has_cur ? static_cast<int>(cur.id) : 0;
        det.windup_wait = false;
    }

    // The attack route changed under a stroke in progress (physical melee's windows -> the game's
    // R1 / R2, e.g. a flick transformed the weapon into a form without physical moves): that
    // stroke never becomes a press (a decision still pending is dropped), and a new swing needs
    // the hand slowed first, as after a draw.
    void DropStroke() {
        pending.active = false;
        OnDraw();
    }

    bool Pending() const {
        return pending.active;
    }

    // Trace / debugging.
    float CurrentArcDeg() const {
        return has_cur ? ThetaEff(cur) / Deg : 0.0f;
    }
    float Rate() const {
        return w;
    }
    unsigned CurrentStroke() const {
        return has_cur ? cur.id : 0;
    }
    bool CurrentCocked() const {
        return has_cur && cur.cocked;
    }
    // Physical melee: a blade stroke is in progress, and its peak blade rate (rad/s).
    bool StrokeLive() const {
        return has_cur;
    }
    // Physical melee: the stroke in progress brings the weapon back (Stroke::recovery): no window
    // opens on it.
    bool Recovery() const {
        return has_cur && cur.recovery;
    }
    float StrokePeak() const {
        return has_cur ? cur.peak : 0.0f;
    }
    // Physical melee's telegraph (core/vr/melee_window.h TelegraphTracker): the current stroke's
    // wind-up direction ("raising", "cocking", "outward"; null: none, or not a stroke), how long it
    // has lasted (s), and whether the weapon is held wound up now (the ready click's pose).
    const char* CurrentWindup() const {
        return Chord().windup;
    }
    float CurrentStrokeSeconds() const {
        return has_cur ? cur.T : 0.0f;
    }
    float BladeAngle() const { // rad, from horizontal-forward (0 before the grip is tracked)
        return have_prev ? alpha : 0.0f;
    }
    bool WoundUpPose(const Config& cfg) const {
        return have_prev && (alpha >= cfg.windup ||
                             (alpha >= WindupPoseAngle && pos_now &&
                              (h_now.y >= cfg.hand_up || std::abs(h_now.x) >= cfg.hand_side)));
    }

private:
    void SetRest(V3 b, V3 h, bool pos) {
        rest_b = b;
        rest_h = h;
        rest_pos = pos;
        rest_yaw = yaw;
    }

    void ClearPre() {
        pre_arc = 0.0f;
        pre_A = {};
        has_pre_axis = false;
    }

    void Begin(V3 b0, V3 h0, bool pos0, float yaw0, float theta, V3 A, V3 axis, const Config& cfg) {
        cur = Stroke{};
        cur.id = next_id++;
        cur.b0 = b0;
        cur.h0 = h0;
        cur.b = b0;
        cur.yaw0 = yaw0;
        cur.yaw = yaw0;
        cur.alpha0 = Angle(b0, fwd_flat);
        cur.theta = theta;
        cur.A = A;
        cur.axis = axis;
        cur.peak = w;
        cur.pos_lost = !pos0;
        cur.amin = cur.alpha0;
        // Out wide to either side: a backhand from far left is wound up like a forehand.
        cur.cocked = cur.alpha0 >= cfg.windup ||
                     (cur.alpha0 >= WindupPoseAngle &&
                      (!pos0 || h0.y >= cfg.hand_up || std::abs(h0.x) >= cfg.hand_side));
        cur.apex = cur.alpha0;
        cur.apex_b = b0;
        cur.apex_h = h0;
        cur.apex_pos = pos0;
        cur.apex_yaw = yaw0;
        has_cur = true;
    }

    // The strike out of a wind-up that never reversed (Config::windup_turn): past the furthest
    // point of a stroke that took the blade away from the front, the blade back toward it and
    // still fast.
    bool ApexTurned(const Config& cfg) const {
        return cfg.windup_turn > 0.0f && w >= OnsetRate && cur.apex >= ApexMin &&
               cur.apex - cur.alpha0 >= ApexAway && cur.apex - alpha >= cfg.windup_turn;
    }

    // Splits the stroke at its furthest point: the wind-up ends there (as a reversal) and the
    // strike is a new stroke from it, with the arc, time and hand travel since.
    void SplitAtApex(const Config& cfg) {
        const Stroke windup = cur;
        EndStroke(End::Reversal);
        Begin(windup.apex_b, windup.apex_h, windup.apex_pos, windup.apex_yaw,
              windup.theta - windup.apex_theta, windup.A - windup.apex_A, windup.axis, cfg);
        cur.T = std::max(windup.T - windup.apex_T, 0.0f);
        cur.t_peak = cur.T;
        cur.travel = std::max(windup.travel - windup.apex_travel, 0.0f);
        cur.pos_lost = windup.pos_lost;
        cur.b = windup.b;
        cur.yaw = windup.yaw;
    }

    // Keeps the stroke's furthest point from the front (Config::windup_turn).
    void MarkApex(V3 b, V3 h, bool pos_ok) {
        if (alpha <= cur.apex) {
            return;
        }
        cur.apex = alpha;
        cur.apex_b = b;
        cur.apex_h = h;
        cur.apex_pos = pos_ok;
        cur.apex_yaw = yaw;
        cur.apex_theta = cur.theta;
        cur.apex_A = cur.A;
        cur.apex_T = cur.T;
        cur.apex_travel = cur.travel;
    }

    void EndStroke(End why) {
        cur.ended = why;
        cur.end_age = 0.0f;
        last = cur;
        has_last = true;
        has_cur = false;
    }

    void DropTracker() {
        have_prev = false;
        has_cur = has_last = false;
        pending.active = false;
        w = decel = 0.0f;
        ClearPre();
    }

    void Track(const Input& in, const Config& cfg, float dtx, bool pos_ok, V3 h, V3 v_rel) {
        if (has_last) {
            last.end_age += dtx;
        }
        if (!in.ori_valid) {
            // Gaps never add arc; a long gap ends the stroke (the margin absorbs float sums of
            // frame times).
            ori_gap += dtx;
            if (ori_gap > OriLost - 1e-4f && has_cur) {
                EndStroke(End::Tracking);
            }
            have_prev = false;
            return;
        }
        ori_gap = 0.0f;
        const V3 b = Normalize(Rotate(in.q, cfg.blade_local));
        alpha = Angle(b, fwd_flat);
        if (!have_prev) {
            have_prev = true;
            b_prev = b;
            SetRest(b, h, pos_ok);
            return;
        }
        const V3 c = Cross(b_prev, b);
        const float c_len = Length(c);
        const float dtheta = std::atan2(c_len, Dot(b_prev, b));
        const bool has_n = c_len > 1e-6f;
        const V3 n = has_n ? c * (1.0f / c_len) : V3{};
        // A pose jump (no arc, no rate update): faster than any blade, or a sudden rate from
        // (near) rest. A fast blade's frames are kept, so snapped heavies keep their arc.
        if (const float raw = dtheta / dtx;
            raw > GlitchMax || (raw > GlitchRate && raw > 2.0f * w + 10.0f)) {
            b_prev = b;
            return;
        }
        const float w_old = w;
        w += (dtheta / dtx - w) * (1.0f - std::exp(-dtx / RateTau));
        decel += ((w_old - w) / dtx - decel) * (1.0f - std::exp(-dtx / DecelTau));

        if (has_cur) {
            cur.T += dtx;
            const float d = has_n ? Dot(n, cur.axis) : 1.0f;
            cur.rev = (has_n && w >= ReversalRate && d < ReversalCos) ? cur.rev + 1 : 0;
            if (cur.rev >= ReversalFrames) {
                // Wind-up -> strike (or forehand -> backhand): a new stroke from here.
                EndStroke(End::Reversal);
                Begin(b_prev, h, pos_ok, yaw, dtheta, n * dtheta, n, cfg);
                b_prev = b;
                return;
            }
            if (ApexTurned(cfg)) {
                // A wind-up looped into its strike: the strike from the furthest point.
                SplitAtApex(cfg);
            }
            if (w < std::max(EndRate, EndFrac * cur.peak)) {
                cur.slow += dtx;
                if (cur.slow >= EndHold) {
                    EndStroke(End::Pause);
                }
            } else {
                cur.slow = 0.0f;
            }
            if (has_cur && cur.T > LongStroke) {
                EndStroke(End::Long);
            }
            if (!has_cur) {
                // Ended this frame: the next stroke starts where this one stopped, not at the
                // rest pose from before it (stale when the blade never dropped below RestRate).
                SetRest(b_prev, h, pos_ok);
                ClearPre();
            }
        }
        if (!has_cur) {
            if (w < RestRate || !has_n) {
                SetRest(b, h, pos_ok);
                ClearPre();
            } else {
                if (!has_pre_axis || Dot(n, pre_axis) > PreAxisCos) {
                    pre_arc += dtheta;
                    pre_A = pre_A + n * dtheta;
                    pre_axis = Normalize(pre_A);
                    has_pre_axis = true;
                } else {
                    SetRest(b_prev, h, pos_ok);
                    pre_arc = dtheta;
                    pre_A = n * dtheta;
                    pre_axis = n;
                }
                if (w >= OnsetRate) {
                    Begin(rest_b, rest_h, rest_pos, rest_yaw, pre_arc, pre_A, pre_axis, cfg);
                    cur.amin = std::min(cur.amin, alpha);
                    cur.b = b;
                    cur.yaw = yaw;
                    if (pos_ok) {
                        cur.vpeak = Length(v_rel);
                    }
                    ClearPre();
                }
            }
        } else {
            cur.theta += dtheta;
            if (has_n) {
                cur.A = cur.A + n * dtheta;
                cur.axis = Normalize(cur.axis + (n - cur.axis) * (1.0f - std::exp(-dtx / AxisTau)));
            }
            if (w > cur.peak) {
                cur.peak = w;
                cur.t_peak = cur.T;
            }
            if (pos_ok) {
                const float speed = Length(v_rel);
                cur.travel += speed * dtx;
                cur.vpeak = std::max(cur.vpeak, speed);
            } else {
                cur.pos_lost = true;
            }
            cur.amin = std::min(cur.amin, alpha);
            cur.b = b;
            cur.yaw = yaw;
        }
        if (has_cur) {
            MarkRecovery(b, pos_ok, v_rel);
            MarkApex(b, h, pos_ok);
        }
        b_prev = b;
    }

    // A recovery stroke (RecoverTrailCos): the blade trailing a hand slower than a strike's. Once
    // the hand reaches a strike's speed the stroke is a swing candidate for good.
    void MarkRecovery(V3 b, bool pos_ok, V3 v_rel) {
        if (!pos_ok || cur.strike_speed) {
            return;
        }
        const float speed = Length(v_rel);
        if (speed >= RecoverStrikeHand) {
            cur.strike_speed = true;
            cur.recovery = false;
        } else if (speed >= RecoverMinHand && Dot(b, v_rel) < RecoverTrailCos * speed) {
            cur.recovery = true;
        }
    }

    // Blade chord since the stroke started, in the current head-yaw frame: a wind-up moves
    // the blade away from the front (up, back or out to the side), a strike toward it.
    ChordResult Chord() const {
        ChordResult r{};
        if (!has_cur) {
            return r;
        }
        const V3 b0h = RotY(cur.b0, -yaw);
        const V3 bh = RotY(cur.b, -yaw);
        const V3 db = bh - b0h;
        const float L = Length(db);
        r.db = db;
        if (L < ChordMin) {
            return r;
        }
        // Moving forward is never a wind-up, and neither is a blade that started away from the
        // front (ApexMin: high, back, out wide) coming closer to it (Config::toward_front: a chop
        // from behind the head rises over the top first).
        const V3 front{0.0f, 0.0f, -1.0f};
        const float a0 = Angle(b0h, front);
        const bool toward =
            toward_front > 0.0f && a0 >= ApexMin && Angle(bh, front) <= a0 - toward_front;
        const bool forward = db.z < -0.2f * L || toward;
        if (!forward) {
            if (db.y >= 0.5f * L) {
                r.windup = "raising";
                return r;
            }
            if (db.z >= 0.5f * L && db.y >= -0.2f * L) {
                r.windup = "cocking";
                return r;
            }
            if (std::abs(bh.x) - std::abs(b0h.x) >= 0.5f * L && db.y > -0.5f * L) {
                r.windup = "outward";
                return r;
            }
        }
        r.strike_like = toward || db.z <= -0.5f * L || db.y <= -0.5f * L ||
                        std::abs(b0h.x) - std::abs(bh.x) >= 0.5f * L;
        return r;
    }

    // Swept arc minus a head/body turn in the same direction (never adds arc).
    float ThetaEff(const Stroke& s) const {
        const V3 a = Normalize(s.A);
        return s.theta - std::max(0.0f, WrapPi(s.yaw - s.yaw0) * a.y);
    }

    float Travel(const Stroke& s) const {
        return s.pos_lost ? std::max(s.travel, LostTravelPerRad * s.theta) : s.travel;
    }

    bool HeavyOK(const Stroke& s, float arc_frac, float travel_frac, const Config& cfg) const {
        return ThetaEff(s) >= arc_frac * cfg.heavy_arc && s.amin <= FrontAngle &&
               Travel(s) >= travel_frac * cfg.heavy_travel &&
               (s.pos_lost || s.vpeak >= cfg.heavy_speed) && s.peak >= HeavyRate &&
               Length(s.A) >= Planarity * s.theta;
    }

    const Stroke* Find(unsigned id) const {
        if (has_cur && cur.id == id) {
            return &cur;
        }
        if (has_last && last.id == id) {
            return &last;
        }
        return nullptr;
    }

    // Grip released (or a busy commit) while a decision is pending: decide now.
    Output Finalize(const char* why, const Config& cfg) {
        Output out{};
        const Stroke* s = Find(pending.id);
        pending.active = false;
        Set(out, s != nullptr && HeavyOK(*s, 1.0f, 1.0f, cfg) ? Kind::Heavy : Kind::Light, why);
        out.stats = Snapshot(s, cfg);
        return out;
    }

    static void Set(Output& out, Kind kind, const char* why) {
        out.kind = kind;
        out.reason = why;
    }

    Stats Snapshot(const Stroke* s, const Config& cfg) const {
        Stats st{};
        st.H = cfg.heavy_arc / Deg;
        st.w = w;
        st.decel = decel;
        st.hand_peak = det.peak;
        st.hand_travel = det.travel;
        st.hand_ms = det.above_time * 1000.0f;
        if (s == nullptr) {
            return st;
        }
        st.id = s->id;
        st.theta = s->theta / Deg;
        st.theta_eff = ThetaEff(*s) / Deg;
        st.peak = s->peak;
        st.travel = Travel(*s);
        st.travel_est = s->pos_lost;
        st.vpeak = s->vpeak;
        st.planar = s->theta > 0.0f ? Length(s->A) / s->theta : 0.0f;
        st.alpha0 = s->alpha0 / Deg;
        st.amin = s->amin / Deg;
        st.ms_onset = s->T * 1000.0f;
        st.cocked = s->cocked;
        st.chord = RotY(s->b, -yaw) - RotY(s->b0, -yaw);
        return st;
    }

    // Time and head.
    float toward_front{10.0f * Deg}; // Config::toward_front, for Chord()
    bool have_t{false};
    double t_prev{};
    float yaw{};
    V3 fwd_flat{0.0f, 0.0f, -1.0f};
    // Blade tracker.
    bool have_prev{false};
    V3 b_prev{};
    float w{};     // smoothed blade angular rate (rad/s)
    float decel{}; // smoothed deceleration (rad/s^2, > 0 while slowing)
    float alpha{}; // blade angle from horizontal-forward
    V3 h_now{};    // the hand in the head-yaw frame this frame (WoundUpPose)
    bool pos_now{false};
    float ori_gap{};
    V3 rest_b{}, rest_h{};
    bool rest_pos{true};
    float rest_yaw{};
    float pre_arc{};
    V3 pre_A{};
    bool has_pre_axis{false};
    V3 pre_axis{};
    Stroke cur{}, last{};
    bool has_cur{false}, has_last{false};
    unsigned next_id{1};
    // Commit gate and arbiter.
    SwingDetector det{};
    float inactive_time{}; // continuous time without immersion
    struct PendingDecision {
        bool active{false};
        unsigned id{};
        float wait{};
        bool fresh{false};
    } pending{};
    bool in_zone{false};
    float since_click{10.0f};
};

} // namespace VR::Melee
