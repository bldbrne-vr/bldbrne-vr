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

// The Beast Cutter's flick recall, its whip form only (vr.ini whip_flick_recall; core/vr/bloodborne_whip.cpp feeds it,
// core/vr/whip_spin.h Payout reels the chain in). Pure, unit-tested (tests/test_whip_flick.cpp).
//
// While the chain is out, a small sharp snap of the wrist brings it straight back, as the passive
// reel-in after whip_hold_s does (the same reel-in, now). A snap: the grip turning at `omega` or
// more for at most `max_s`, through `min_angle` in all, the hand itself nearly still (`hand_speed`:
// a lash moves the whole arm), judged when the turn falls back below `release` (a wrist circle or a
// spin never ends, so it never fires). Both speeds are the grip's relative to the hunter (his own
// turn and walk taken out). It never fires on the lash's own wrist: the snap must start `after_out_s`
// after the chain was asked out, with the chain paid out past `extension` when it ends. Once fired
// the recall holds (the snap's own tip speed would pay the chain out again) until the chain is back
// on its stick and the wrist still, or the arm starts a new stroke (`relaunch_speed`). One flick per
// `refractory_s`.
//
// A crack (core/vr/whip_spin.h CrackGate) is the same wrist-only snap, so since 2026-10-15 the
// direction tells them apart (whip_flick_recall=1): a recall flicks the weapon BACK, its far end
// lifting (up; back over the shoulder once it points up: FlickLift), for at least `lift_share` of
// the snap's turn; a crack throws it OUT (down, forward or sideways) and never recalls. Two
// out-and-back motions are not a lift on their own either: a lift starting within `rebound_s` of
// the wrist snapping out is that crack's rebound, and a lift the wrist snaps straight out of within
// `confirm_s`, as fast as the lift itself or faster (`windup`), is a crack's wind-up (the recall
// waits that long; the slower bounce at the end of a sharp flick back, or the wrist's quick return
// to where it was, does not undo it). A hitch's long frame inside the wait is judged on its
// average turn; losing the hunter's frame lets the lift go (Dropped says why). While a lift is a
// recall in the making (Lifting) the crack gate holds off, so a recall never spends a swing from
// the combo budget.
// whip_flick_recall=any keeps the earlier any-direction snap (AnyDirectionFlick: a crack reels in
// too).
namespace VR::Whip {

struct FlickConfig {
    float omega{9.42f};        // rad/s (540 deg/s): the grip turning this fast is a snap ...
    float release{4.71f};      // ... until it falls below this (270 deg/s): the snap is over
    float min_angle{0.436f};   // rad (25 deg): the snap's turn in all, at least
    float max_s{0.16f};        // s above `omega`, at most: longer is a spin or a sweep
    float hand_speed{1.5f};    // m/s, the grip: never faster during the snap (the wrist alone)
    float after_out_s{0.25f};  // s: the snap starts this long after the chain was asked out
    float extension{1.1f};     // the chain paid out past this (1 = the idle stick)
    float refractory_s{0.5f};  // s after a flick: no other
    float relaunch_speed{2.0f}; // m/s, the grip: a stroke this fast ends the recall
    float lift_share{0.5f};    // the snap's turn lifting the far end, this share at least (0: any)
    float confirm_s{0.10f};    // s: the lift's recall waits this long; snapping out first: a wind-up
    float rebound_s{0.20f};    // s: a lift starting this soon after a snap out (`omega`): a rebound
    // A snap out while the lift's recall waits is that crack's wind-up only turning this x the
    // lift's own peak or faster (slower: the wrist easing back; 0: any snap out)
    float windup{1.0f};
};

// whip_flick_recall=any: a snap in any direction recalls at once (before 2026-10-15; a crack, the
// same wrist snap thrown out, reels the chain in too).
inline FlickConfig AnyDirectionFlick() {
    FlickConfig c{};
    c.lift_share = 0.0f;
    c.confirm_s = 0.0f;
    c.rebound_s = 0.0f;
    return c;
}

struct FlickDir {
    float x{0.0f}, y{0.0f}, z{0.0f};
};

// rad/s the weapon's far end lifts: its axis (unit, world: `from` the frame before, `to` now)
// turning toward up, or back over the shoulder (against `fwd`, the hunter's horizontal forward) as
// it comes to point up. Negative: it turns down / out. 0 for a turn sideways, with the weapon
// already pointing up and back (nothing left to lift toward), or within about 11 degrees of
// straight down (every way lifts its far end there: no direction to tell).
inline float FlickLift(FlickDir from, FlickDir to, FlickDir fwd, float dt) {
    if (!(dt > 0.0f)) {
        return 0.0f;
    }
    // Up, and half as much back: perpendicular to the axis it is "up" for a weapon held forward
    // or to the side, "back" for one held upright. The back fades out as the weapon points below
    // horizontal (none from 30 degrees down): a fixed back would point straight against a weapon
    // aimed about 63 degrees down and ahead, and past that turn a dip into a lift.
    const float back = 0.5f * std::clamp(1.0f + 2.0f * from.y, 0.0f, 1.0f);
    const float rx = -back * fwd.x, ry = 1.0f - back * fwd.y, rz = -back * fwd.z;
    const float d = rx * from.x + ry * from.y + rz * from.z;
    const float ux = rx - d * from.x, uy = ry - d * from.y, uz = rz - d * from.z;
    const float n = std::sqrt(ux * ux + uy * uy + uz * uz);
    if (!(n >= 0.2f)) {
        return 0.0f;
    }
    const float lift = (to.x - from.x) * ux + (to.y - from.y) * uy + (to.z - from.z) * uz;
    return std::isfinite(lift) ? lift / (n * dt) : 0.0f;
}

struct FlickInput {
    float dt{0.0f};          // s (0: the same update evaluated again)
    float omega{0.0f};       // rad/s, the grip's turn relative to the hunter
    float hand_speed{0.0f};  // m/s, the grip relative to the hunter
    float extension{1.0f};   // the chain's (Chain::Extension)
    bool asked_out{false};   // the payout asks the chain out (Payout::Out, before this frame)
    bool valid{true};        // a usable sample (the hunter's frame known, no snap turn)
    float lift{0.0f};        // rad/s of the turn lifting the far end (FlickLift; < 0: out)
};

class WristFlick {
public:
    // One game frame. True while the flick recalls the chain (pass it to Payout::Step).
    bool Step(const FlickInput& in, const FlickConfig& c, bool enabled = true) {
        fired_ = false;
        dropped_ = nullptr;
        if (!enabled) {
            Reset();
            return false;
        }
        if (!(in.dt > 0.0f)) {
            return returning_; // the same update again: nothing moved
        }
        const float dt = std::min(in.dt, 0.25f);
        since_out_ = in.asked_out && !was_out_ ? 0.0f : std::min(since_out_ + dt, 10.0f);
        was_out_ = in.asked_out;
        since_fire_ = std::min(since_fire_ + dt, 10.0f);
        since_turn_out_ = std::min(since_turn_out_ + dt, 10.0f);
        lifting_ = false;
        const bool directed = c.lift_share > 0.0f;
        const bool usable = in.valid && std::isfinite(in.omega) && std::isfinite(in.hand_speed) &&
                            std::isfinite(in.extension) && std::isfinite(in.lift);
        // This frame's turn at a snap's speed, not lifting the far end: snapping out (down,
        // forward, sideways: a crack's; the slower bounce a sharp stop leaves is not).
        const bool snapping_out =
            usable && directed && in.omega >= c.omega && in.lift < c.lift_share * in.omega;
        if (snapping_out) {
            since_turn_out_ = 0.0f;
        }
        // ... and with a lift's recall waiting, as fast as that lift's own peak (`windup`) or
        // faster: the crack the lift was the wind-up of (a slower turn out is the wrist's return).
        const bool winding_out =
            snapping_out && (!pending_ || in.omega >= c.windup * pending_snap_.peak);
        if (in.dt > 0.05f || !usable) {
            snap_ = false; // a hitch or no hunter: no snap spans it
            if (pending_ && !usable) {
                pending_ = false;
                dropped_ = "the hunter's frame lost (or his turn too quick) while it waited";
            } else if (pending_) {
                Wait(in, c, dt, winding_out); // a hitch: judged on the long frame's average turn
            }
            lifting_ = directed && pending_;
            return returning_;
        }
        if (returning_ && (in.hand_speed >= c.relaunch_speed ||
                           (in.extension <= 1.05f && in.omega < c.release))) {
            returning_ = false; // a new stroke, or back on the stick and still
        }
        if (pending_) {
            Wait(in, c, dt, winding_out);
        }
        if (in.omega >= c.omega) {
            if (!snap_) {
                snap_ = true;
                eligible_ = since_out_ >= c.after_out_s;
                rebound_ = directed && c.rebound_s > 0.0f && since_turn_out_ < c.rebound_s;
                fast_s_ = angle_ = peak_hand_ = lift_ = peak_ = 0.0f;
            }
            fast_s_ += dt;
        }
        if (snap_) {
            angle_ += in.omega * dt;
            lift_ += in.lift * dt;
            peak_hand_ = std::max(peak_hand_, in.hand_speed);
            peak_ = std::max(peak_, in.omega);
            const bool recall = eligible_ && !returning_ && since_fire_ >= c.refractory_s &&
                                fast_s_ <= c.max_s && peak_hand_ <= c.hand_speed &&
                                in.asked_out && in.extension > c.extension &&
                                (!directed || lift_ >= c.lift_share * angle_);
            if (in.omega >= c.release) {
                // Still turning: a lift that would recall holds the crack gate off meanwhile (not
                // once the wrist snaps out: a wind-up turned into its crack cracks at once).
                lifting_ = directed && !winding_out && ((recall && !rebound_) || pending_);
                return returning_;
            }
            snap_ = false;
            if (recall && angle_ >= c.min_angle) {
                const Snap snap{fast_s_, angle_, peak_hand_, lift_, peak_};
                if (rebound_) {
                    dropped_ = "a rebound: the wrist coming back from snapping out";
                } else if (directed && c.confirm_s > 0.0f) {
                    pending_ = true; // the wrist turning straight out next would be a crack
                    pending_s_ = 0.0f;
                    pending_snap_ = snap;
                } else {
                    Fire(snap);
                }
            }
        }
        lifting_ = directed && pending_;
        return returning_;
    }
    // The flick fired this frame.
    bool Fired() const {
        return fired_;
    }
    bool Returning() const {
        return returning_;
    }
    // A lift that would recall is under way or waiting for its confirm: no crack (CrackInput).
    bool Lifting() const {
        return lifting_;
    }
    // A lift that would have recalled was let go this frame, and why (a static string), else null.
    const char* Dropped() const {
        return dropped_;
    }
    std::uint32_t Flicks() const {
        return flicks_;
    }
    // The last flick: s above `omega`, rad turned, the grip's peak speed (m/s), rad of it lifting
    // the far end (the log), its peak turn (rad/s).
    struct Snap {
        float fast_s{0.0f}, angle{0.0f}, hand{0.0f}, lift{0.0f}, peak{0.0f};
    };
    Snap Last() const {
        return last_;
    }
    void Reset() {
        returning_ = fired_ = snap_ = was_out_ = pending_ = lifting_ = rebound_ = false;
        since_out_ = since_fire_ = since_turn_out_ = 10.0f;
        dropped_ = nullptr;
    }

private:
    void Fire(const Snap& snap) {
        returning_ = fired_ = true;
        since_fire_ = 0.0f;
        ++flicks_;
        last_ = snap;
    }
    // A lift's recall waiting `confirm_s` (a hitch's long frame counts, judged on its average).
    void Wait(const FlickInput& in, const FlickConfig& c, float dt, bool winding_out) {
        if (winding_out) {
            pending_ = false; // the wrist straight back out: the lift was a crack's wind-up
            dropped_ = "a wind-up: the wrist snapped straight back out";
        } else if (in.hand_speed >= c.relaunch_speed) {
            pending_ = false;
            dropped_ = "a new stroke: the arm moved";
        } else if (!in.asked_out || !(in.extension > c.extension)) {
            pending_ = false; // reeled in meanwhile: nothing left to recall
        } else if ((pending_s_ += dt) >= c.confirm_s - 1e-4f) {
            pending_ = false;
            Fire(pending_snap_);
        }
    }

    bool returning_{false};
    bool fired_{false};
    bool snap_{false};
    bool eligible_{false};
    bool was_out_{false};
    bool rebound_{false};
    bool pending_{false};
    bool lifting_{false};
    float since_out_{10.0f};
    float since_fire_{10.0f};
    float since_turn_out_{10.0f};
    float pending_s_{0.0f};
    float fast_s_{0.0f}, angle_{0.0f}, peak_hand_{0.0f}, lift_{0.0f}, peak_{0.0f};
    std::uint32_t flicks_{0};
    Snap last_{};
    Snap pending_snap_{};
    const char* dropped_{nullptr};
};

} // namespace VR::Whip
