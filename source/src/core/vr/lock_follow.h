// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cmath>

// Lock-on follow (vr.ini lockon_follow; the tester, 2026-11-27: "making the camera and hunter's body
// follow left-right the target (no up down) ... if the user moves their head a certain degree,
// then the targeting cancels"). Locked on in the first-person VR view, the game turns the hunter
// to the target but the view stays yours, and the turn stick switches targets, so a target that
// moves round you leaves the view. This turns the view (as the turn stick does, yaw only) to bring
// it back:
//  - Locking on puts the target straight where you look at once (one snap, in either mode; the
//    user, 2026-11-27: "on holding R3 immediately snap and follow the boss").
//  - The target has a home in your play space: where your head looked when the lock started, and
//    afterwards wherever it is when your head turns onto it (within LookDeg, the head moving at
//    least HomeTurnDegS: you turned to it; a target drifting into a still gaze keeps its home).
//    Your head moving never turns the view; only the target moving away from its home does (or
//    the hunter moving round it).
//  - Snap (1): once it is more than zone_deg from home, one turn puts it back there (at most one
//    every snap_gap_s). Smooth (2): past zone_deg the view turns, easing up to speed_deg_s, until
//    it is back within half the zone; a target more than SmoothFarDeg from home (behind you) is
//    snapped back at once, as Snap does. Centred (3; the tester, 2026-11-27: "if I move left or
//    right I want to remain focused/centered on the boss ... that follow is smooth but if there
//    is a large jump eg rolling towards the boss then its snap"): no zone, the view keeps it at
//    home all the time (a critically damped camera, smooth_s, never more than max_off_deg off),
//    as the flat game's lock-on camera does; a jump (more than CentredSnapDeg off, or one the cap
//    could only follow faster than CentredCapSnapDegS: a roll past it, a leap over you), a snap.
//  - Looking away: your head more than cancel_deg from the target and more than CancelHomeShare of
//    that from its home, for cancel_s, asks for the lock to be released (once per lock; the
//    presenter sends the game's lock-on button). Both: a target that outruns the view is not you
//    looking away (its home stays where you look), and a head that sweeps past the target on its
//    way (re-homing it as it passes) still is.
//  - Up close (the target's lock point less than near_m from the hunter along the ground: under a
//    big boss, or one lunging through you), its direction swings with every step you take, so the
//    view holds still and looking away releases nothing (your head turning onto it still re-homes
//    it); past near_m + NearLeaveM the rules above bring it back (a big drift, one snap). A lock
//    that starts up close is put where you look once there is room.
// Pure: one Step per game camera update. Angles in radians, game yaw convention (+ = right).
namespace VR::LockFollow {

constexpr float Pi = 3.14159265f;
constexpr float DegToRad = Pi / 180.0f;
// Looking at the target (within this of the head's view): that is where it belongs.
constexpr float LookDeg = 10.0f;
// The smooth turn's easing in and out (seconds to most of its speed).
constexpr float EaseS = 0.12f;
// Smooth: a target further than this from home is snapped back at once. The game lets go of a
// lock it cannot see (2026-11-27, Gascoigne: locked on 165 degrees behind, the lock was gone 0.7 s
// later while the smooth turn had made 2 degrees; Snap's one 144-degree turn kept it 9 s).
constexpr float SmoothFarDeg = 90.0f;
// Centred: the target within this of home is left alone (its lock point bobs with its animations).
constexpr float CentredSoftDeg = 1.0f;
// Centred: a jump this far is snapped, not swept.
constexpr float CentredSnapDeg = 45.0f;
// Centred: holding the target within max_off_deg would turn the view faster than this: a jump (a
// roll past it, a leap over you), snapped instead of swept.
constexpr float CentredCapSnapDegS = 240.0f;
// The head turning onto the target at least this fast re-homes it (a deliberate look; a head held
// still with its small tremor does not).
constexpr float HomeTurnDegS = 20.0f;
// Looking away: the head this share of cancel_deg from the target's home, too (2026-11-27 mock: a
// 75-degree look swept past Gascoigne, re-homing him, and ended 70 degrees from him but 58 from
// his home; the old home-only rule at 60 missed it).
constexpr float CancelHomeShare = 0.75f;
// Up close: the view follows again this much further out than near_m (no flicker at the edge).
constexpr float NearLeaveM = 0.4f;

enum Mode : int { Off = 0, Snap = 1, Smooth = 2, Centred = 3 };

struct Settings {
    int mode{Off};
    float zone_deg{30.0f};    // the target may move this far from home before the view turns
    float cancel_deg{40.0f};  // looking this far from the target's home ...
    float cancel_s{0.25f};    // ... this long releases the lock (0 = never)
    float speed_deg_s{90.0f}; // smooth: the fastest turn
    float snap_gap_s{0.25f};  // snap: at least this between two snaps
    float smooth_s{0.15f};    // centred: the camera's smoothing (its lag behind a moving target)
    float max_off_deg{10.0f}; // centred: never further off where it belongs than this
    // Closer than this (metres from the hunter to the lock point, along the ground) the view
    // holds still (the Gascoigne test, 2026-11-27: 7 of 9 snaps came with him within 1 m, lunging
    // through you; circling him at 1-2 m is ordinary melee and keeps following). 0 = never.
    float near_m{0.8f};
};

struct Input {
    bool locked{false};      // locked on, in the first-person VR view
    bool have_target{false}; // a fresh bearing to the lock point
    float target_yaw{0.0f};  // world yaw from the eye to the lock point
    float base_yaw{0.0f};    // the game heading of the play space's forward (turns included)
    float view_yaw{0.0f};    // world yaw the head looks along (base_yaw - head yaw)
    float dt{0.0f};          // seconds since the last step
    float distance{1e9f};    // metres from the hunter to the lock point, along the ground
};

struct Output {
    float turn{0.0f};     // turn the view this much (+ = right)
    bool cancel{false};   // release the lock (once)
    bool started{false};  // the lock started following this step
    bool snapped{false};  // a snap this step
    float error{0.0f};    // the target off its home (radians)
    float away{0.0f};     // the head off the target's home (radians)
};

[[nodiscard]] inline float Wrap(float a) {
    return std::remainder(a, 2.0f * Pi);
}

class Follower {
public:
    Output Step(const Settings& s, const Input& in) {
        Output out{};
        if (s.mode == Off || !in.locked) {
            Reset();
            return out;
        }
        const float head = Wrap(in.view_yaw - in.base_yaw); // the head's yaw in the play space
        const bool head_turning =
            active_ && in.dt > 0.0f &&
            std::abs(Wrap(head - head_prev_)) > HomeTurnDegS * DegToRad * in.dt;
        head_prev_ = head;
        if (!active_) {
            active_ = true;
            home_ = head; // where you looked as you locked on
            away_s_ = 0.0f;
            since_snap_s_ = 1e9f;
            rate_ = 0.0f;
            turning_ = false;
            cancel_sent_ = false;
            centred_ = false;
            up_close_ = false;
            out.started = true;
        }
        since_snap_s_ += in.dt;
        if (!in.have_target) {
            rate_ = 0.0f;
            return out;
        }
        const float rel = Wrap(in.target_yaw - in.base_yaw); // the target in the play space
        // Up close (2026-11-27: "Fix the close-range swing under big bosses"): the view
        // holds still; your head turning onto it is still where it belongs.
        if (s.near_m <= 0.0f) {
            up_close_ = false;
        } else if (in.distance < s.near_m) {
            up_close_ = true;
        } else if (in.distance > s.near_m + NearLeaveM) {
            up_close_ = false;
        }
        if (up_close_) {
            rate_ = 0.0f;
            away_s_ = 0.0f;
            turning_ = false;
            if (head_turning && std::abs(Wrap(rel - head)) < LookDeg * DegToRad) {
                home_ = rel;
            }
            out.error = Wrap(rel - home_);
            out.away = Wrap(home_ - head);
            return out;
        }
        // The lock's first reading: the target straight where you look, in one turn.
        if (!centred_) {
            centred_ = true;
            home_ = head;
            const float off = Wrap(rel - head);
            if (std::abs(off) > LookDeg * DegToRad) {
                out.turn = off;
                out.snapped = true;
                out.error = off;
                since_snap_s_ = 0.0f;
                return out;
            }
        }
        if (head_turning && std::abs(Wrap(rel - head)) < LookDeg * DegToRad) {
            home_ = rel;
        }
        const float error = Wrap(rel - home_);
        out.error = error;
        out.away = Wrap(home_ - head);

        // Looking away from it releases the lock.
        if (s.cancel_s > 0.0f && std::abs(Wrap(rel - head)) > s.cancel_deg * DegToRad &&
            std::abs(out.away) > CancelHomeShare * s.cancel_deg * DegToRad) {
            away_s_ += in.dt;
            if (away_s_ >= s.cancel_s && !cancel_sent_) {
                cancel_sent_ = true;
                out.cancel = true;
            }
        } else {
            away_s_ = 0.0f;
        }

        const float zone = s.zone_deg * DegToRad;
        if (s.mode == Snap) {
            if (std::abs(error) > zone && since_snap_s_ >= s.snap_gap_s) {
                out.turn = error;
                out.snapped = true;
                since_snap_s_ = 0.0f;
            }
            return out;
        }
        // Smooth: far round (behind you), at once; past the zone, turn until it is back within
        // half of it. Centred: a jump, at once; else kept at home.
        const float far = (s.mode == Centred ? CentredSnapDeg : SmoothFarDeg) * DegToRad;
        if (std::abs(error) > far && since_snap_s_ >= s.snap_gap_s) {
            out.turn = error;
            out.snapped = true;
            since_snap_s_ = 0.0f;
            rate_ = 0.0f;
            turning_ = false;
            return out;
        }
        if (s.mode == Centred) {
            // A critically damped camera (2026-11-27: "it needs some camera smoothing
            // when following the enemy around"): the turn speed builds and settles smoothly; it
            // trails a target moving round you by about smooth_s of its movement.
            const float w = 2.0f / std::max(s.smooth_s, 0.02f);
            float e = error;
            float turn = 0.0f;
            for (float left = in.dt; left > 0.0f;) {
                const float h = std::min(left, 0.005f);
                const float soft =
                    std::copysign(std::max(0.0f, std::abs(e) - CentredSoftDeg * DegToRad), e);
                rate_ += (w * w * soft - 2.0f * w * rate_) * h;
                turn += rate_ * h;
                e -= rate_ * h;
                left -= h;
            }
            out.turn = turn;
            // The cap (2026-11-27: "if I keep strafing the enemy gets pushed out of the
            // view ... it needs to cap so the enemy doesn't end up off center by much"): never
            // more than max_off_deg off after this turn; the smoothing works inside it.
            const float after = Wrap(error - turn);
            const float cap = s.max_off_deg * DegToRad;
            if (std::abs(after) > cap) {
                const float extra = after - std::copysign(cap, after);
                if (std::abs(extra) > CentredCapSnapDegS * DegToRad * in.dt &&
                    since_snap_s_ >= s.snap_gap_s) {
                    out.turn = error;
                    out.snapped = true;
                    since_snap_s_ = 0.0f;
                    rate_ = 0.0f;
                } else {
                    out.turn += extra;
                }
            }
            return out;
        }
        if (std::abs(error) > zone) {
            turning_ = true;
        } else if (std::abs(error) <= 0.5f * zone) {
            turning_ = false;
        }
        const float want = turning_ ? std::copysign(s.speed_deg_s * DegToRad, error) : 0.0f;
        const float k = in.dt > 0.0f ? 1.0f - std::exp(-in.dt / EaseS) : 0.0f;
        rate_ += (want - rate_) * k;
        if (!turning_ && std::abs(rate_) < 1.0f * DegToRad) {
            rate_ = 0.0f; // eased out
        }
        float turn = rate_ * in.dt;
        // Never past home, never against the error.
        if (turn * error <= 0.0f) {
            turn = 0.0f;
        } else if (std::abs(turn) > std::abs(error)) {
            turn = error;
        }
        out.turn = turn;
        return out;
    }

    void Reset() {
        active_ = false;
        away_s_ = 0.0f;
        rate_ = 0.0f;
        turning_ = false;
        cancel_sent_ = false;
        up_close_ = false;
    }

    [[nodiscard]] bool Active() const {
        return active_;
    }
    // Up close: holding the view still right now.
    [[nodiscard]] bool UpClose() const {
        return up_close_;
    }
    // Smooth: turning back toward the target's home right now.
    [[nodiscard]] bool Turning() const {
        return turning_;
    }

private:
    bool active_{false};
    float home_{0.0f};     // the target's home in the play space
    float away_s_{0.0f};   // how long the head has been away from it
    float since_snap_s_{1e9f};
    float rate_{0.0f};     // smooth: the current turn speed
    bool turning_{false};  // smooth: turning back toward home
    bool cancel_sent_{false};
    bool centred_{false}; // the lock's first reading put it in front
    float head_prev_{0.0f};  // the head's yaw in the play space at the last step
    bool up_close_{false};   // the target up close: the view holds still
};

// Your run keeps its direction (vr.ini lockon_move_commit_s; the tester, 2026-11-27: "if we dodge
// or jump in one direction near the enemy - the camera movement might make us switch direction
// ... commit to the movement despite the camera rotation"). The left stick steers along the view,
// so a follow turn would bend a held run, a strafe or a dodge's follow-through by the turn. While
// the stick is held, the follow's turns are taken off the stick instead: the run keeps going the
// same way in the world, and the difference eases back to the view over ease_s (a long strafe
// still curves round the target, smoothly instead of in steps). Letting go of the stick, or
// pushing it a new way (more than ReaimDeg from where it was at the last turn), ends it at once:
// a new push goes where you look. ease_s 0 = off (the stick always follows the view).
constexpr float StickHeld = 0.2f; // the stick's length that counts as held
constexpr float ReaimDeg = 45.0f; // a push this far round is a new direction
constexpr float CommitMinDeg = 0.5f; // eased out below this

class MoveCommit {
public:
    // One step per input frame: `follow_turn` the view's follow turns since the last step
    // (radians, + = right), (x, y) the stick (x right, y forward). Returns the angle to take off
    // the stick's direction (RotateStick); 0 = none.
    float Step(float ease_s, float follow_turn, float x, float y, float dt) {
        if (ease_s <= 0.0f || std::hypot(x, y) < StickHeld) {
            offset_ = 0.0f;
            return 0.0f;
        }
        const float angle = std::atan2(x, y);
        if (offset_ != 0.0f) {
            if (std::abs(Wrap(angle - ref_)) > ReaimDeg * DegToRad) {
                offset_ = 0.0f; // pushed a new way: that goes where you look
            } else if (dt > 0.0f) {
                offset_ *= std::exp(-dt / ease_s);
            }
        }
        if (follow_turn != 0.0f) {
            offset_ = Wrap(offset_ + follow_turn);
            ref_ = angle;
        }
        if (std::abs(offset_) < CommitMinDeg * DegToRad) {
            offset_ = 0.0f;
        }
        return offset_;
    }

    void Reset() {
        offset_ = 0.0f;
    }

    [[nodiscard]] bool Active() const {
        return offset_ != 0.0f;
    }

private:
    float offset_{0.0f}; // taken off the stick's direction (radians)
    float ref_{0.0f};    // the stick's direction at the last turn
};

// Turns the stick's direction by -offset (x right, y forward; angles + = right): a view turned
// right by `offset` and the stick turned left by it point the same way in the world.
inline void RotateStick(float& x, float& y, float offset) {
    const float c = std::cos(offset), s = std::sin(offset);
    const float nx = x * c - y * s;
    const float ny = y * c + x * s;
    x = nx;
    y = ny;
}

} // namespace VR::LockFollow
