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

// The physical whip's swing (vr.ini whip=physical; core/vr/whip_chain.h is the chain,
// core/vr/bloodborne_whip.h the game side). Pure, unit-tested (tests/test_whip_chain.cpp).
//
// Payout: the chain's paid-out length follows the swing's speed, like a flail or a chain whip under
// centrifugal force. The swing's speed is the presenter's tip speed (the weapon's far end, head-
// relative: the hand's speed plus the controller's turn, so a wrist spin counts as much as an arm
// swing). Fast, the chain pays out smoothly toward the configured reach cap for the current move;
// it stays out while the speed is kept up (a fast overhead spin keeps
// it extended, circling like a helicopter blade, the chain trailing the spin) and for `hold_s` of
// slow motion after it (a sudden stop lets the tip whip past and settle); then it reels back in to
// its rest length (the game's 1.17 m stick) with a quick recoil.
//
// SpinCounter: the revolutions of a spun whip (the hand's velocity turning round one axis): each
// revolution counts as a swing, a window of its own paid from the combo budget like any other (two
// charges, the whip's refill), its hits held to the game's own damage per second (core/vr/
// whip_damage.h PaceMeter): continuous spinning is not a free lawnmower. A figure eight
// or a back-and-forth never adds up to a revolution (their turns cancel or the hand stops).
//
// The chain hurts only while it moves fast: its attack waits for the tip to reach `HurtTipSpeed`
// and ends when it falls below (core/vr/bloodborne_melee.cpp), and a hit's speed factor comes from
// the chain's own tip speed (BladeEquivalentTip).
namespace VR::Whip {

inline float SmoothRamp(float a, float b, float x) {
    if (!(b > a)) {
        return x >= b ? 1.0f : 0.0f;
    }
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// The payout's start and keep speeds as shares of whip_payout_speed (its full payout).
inline constexpr float PayoutStartShare = 0.45f;
inline constexpr float PayoutKeepShare = 0.36f;

struct PayoutConfig {
    float v_start{2.5f}; // m/s, the swing's speed: the chain starts paying out ...
    float v_full{5.5f};  // ... and is fully out (whip_payout_speed)
    float v_keep{2.0f};  // slower than this ...
    float hold_s{0.6f};  // ... this long: it reels in (whip_hold_s)
};

// Give the heavy chain time to finish its flight and land before passive recall.
// Beast Cutter adds 500 ms to its pause (default 0.90 -> 1.40 s).
// Cane: shorten only the slow-motion pause (default 0.60 -> 0.42 s), keeping its return
// duration unchanged. Payout resets this timer whenever swing speed recovers, so brief
// direction reversals do not accumulate toward a recall.
inline float HoldFor(std::int32_t variation, float hold_s) {
    return variation == 2400 ? hold_s * 1.5f + 0.50f
                             : hold_s * (variation == 2200 ? 0.70f : 1.0f);
}

inline bool CompactCutter(std::int32_t variation, bool form_b, bool physical, bool trigger) {
    return variation == 2400 && form_b && physical && trigger;
}

// The paid-out length (the chain's extension: 1 = the idle stick) the swing asks for.
class Payout {
public:
    // One frame: `speed` the swing's speed (m/s), `cap` the extension of the reach allowed now (the
    // light move's; a heavy or charged window's), `dt` (s). Returns the extension to pay out to.
    float Step(float speed, float cap, float dt, const PayoutConfig& c,
               bool recall = false) {
        if (recall) {
            Recall();
            return target_;
        }
        cap = std::max(cap, 1.0f);
        dt = std::clamp(dt, 0.0f, 0.25f);
        if (!std::isfinite(speed)) {
            speed = 0.0f;
        }
        const float f = SmoothRamp(c.v_start, std::max(c.v_full, c.v_start + 0.1f), speed);
        const float goal = 1.0f + (cap - 1.0f) * f;
        slow_ = speed >= c.v_keep ? 0.0f : slow_ + dt;
        reeled_ = false;
        if (goal > target_) {
            target_ = goal; // pays out with the speed
        } else if (target_ > 1.0f && slow_ >= c.hold_s) {
            target_ = 1.0f; // slow for the hold: reels in
            reeled_ = true;
        }
        target_ = std::min(target_, cap); // a heavy window's reach over: back to the light's
        return target_;
    }
    void Recall() {
        reeled_ = target_ > 1.02f;
        target_ = 1.0f;
        slow_ = 0.0f;
    }
    float Target() const {
        return target_;
    }
    // The chain is asked out (past 2% of the idle length).
    bool Out() const {
        return target_ > 1.02f;
    }
    // The reel-in started this frame.
    bool Reeled() const {
        return reeled_;
    }
    float Slow() const {
        return slow_;
    }
    void Reset() {
        target_ = 1.0f;
        slow_ = 0.0f;
        reeled_ = false;
    }

private:
    float target_{1.0f};
    float slow_{0.0f};
    bool reeled_{false};
};

// Beast Cutter only: distinguish an inward tug from a new throwing stroke. The input is
// the velocity of a short point on the handle, relative to the hunter, projected toward
// the chain tip. Tangential swings/spins are not recalls. A few centimetres, rather than
// a single noisy frame, starts the return; speed alone cannot cancel that same return.
class PullRecall {
public:
    bool Step(float radial_speed, float gesture_speed, float swing_speed, float extension,
              float dt, const PayoutConfig& c) {
        if (!(dt > 0.0f && dt <= 0.05f) || !std::isfinite(radial_speed) ||
            !std::isfinite(gesture_speed) || !std::isfinite(swing_speed)) {
            pull_m_ = pull_s_ = 0.0f;
            return returning_;
        }
        constexpr float pull_max_speed = 2.2f;
        constexpr float relaunch_speed = 2.6f;
        if (returning_) {
            // A renewed sideways/outward swing can launch immediately. A continuing inward
            // flick needs a stronger stroke: leave a speed margin so its own peak cannot
            // cancel the recall it just started.
            if (swing_speed >= c.v_start && gesture_speed >= 0.5f &&
                (radial_speed >= -0.25f * gesture_speed || gesture_speed >= relaunch_speed))
                returning_ = false;
        } else if (extension > 1.4f && gesture_speed < pull_max_speed &&
                   swing_speed < c.v_full * 1.25f && radial_speed < -0.35f &&
                   -radial_speed > 0.60f * gesture_speed) {
            pull_m_ += -radial_speed * dt;
            pull_s_ += dt;
            if (pull_m_ >= 0.020f && pull_s_ >= 0.035f) returning_ = true;
        } else {
            pull_m_ = pull_s_ = 0.0f;
        }
        if (returning_) pull_m_ = pull_s_ = 0.0f;
        return returning_;
    }
    void Reset() { returning_ = false; pull_m_ = pull_s_ = 0.0f; }
    bool Returning() const { return returning_; }
private:
    bool returning_{false};
    float pull_m_{0.0f}, pull_s_{0.0f};
};

struct SpinConfig {
    float min_speed{1.6f}; // m/s: the hand this fast (head-relative) turns the count ...
    float reset_s{0.30f};  // ... slower this long: the spin is over (the count starts again)
    float rev{6.2831853f}; // rad of the velocity's turn per revolution
};

// The revolutions of a spun whip: the hand's velocity direction turning round one axis. The turn
// is summed as a vector (axis x angle), so turns round opposite axes (a figure eight, the two
// halves of a waggle) cancel.
class SpinCounter {
public:
    // `vx, vy, vz` the hand's velocity (head-relative, m/s). True when a revolution completed.
    bool Step(float vx, float vy, float vz, float dt, const SpinConfig& c) {
        dt = std::clamp(dt, 0.0f, 0.25f);
        const float speed = std::sqrt(vx * vx + vy * vy + vz * vz);
        if (!(speed >= c.min_speed)) {
            slow_ += dt;
            if (slow_ >= c.reset_s) {
                Restart();
                have_ = false;
            }
            return false;
        }
        slow_ = 0.0f;
        const float ux = vx / speed, uy = vy / speed, uz = vz / speed;
        bool rev = false;
        if (have_) {
            const float cx = py_ * uz - pz_ * uy;
            const float cy = pz_ * ux - px_ * uz;
            const float cz = px_ * uy - py_ * ux;
            const float s = std::sqrt(cx * cx + cy * cy + cz * cz);
            const float d = px_ * ux + py_ * uy + pz_ * uz;
            const float angle = std::atan2(s, d);
            if (s > 1e-6f) {
                ax_ += cx / s * angle;
                ay_ += cy / s * angle;
                az_ += cz / s * angle;
            }
            const float turned = Turned();
            if (turned >= c.rev) {
                // One revolution: what is over it starts the next.
                const float k = (turned - c.rev) / turned;
                ax_ *= k;
                ay_ *= k;
                az_ *= k;
                ++revs_;
                rev = true;
            }
        }
        px_ = ux;
        py_ = uy;
        pz_ = uz;
        have_ = true;
        return rev;
    }
    // A window opened (a stroke's own): the revolution under way starts again from here, so a
    // spin opens at most one window per revolution.
    void Restart() {
        ax_ = ay_ = az_ = 0.0f;
    }
    // rad turned toward the next revolution.
    float Turned() const {
        return std::sqrt(ax_ * ax_ + ay_ * ay_ + az_ * az_);
    }
    std::uint32_t Revolutions() const {
        return revs_;
    }

private:
    bool have_{false};
    float px_{0.0f}, py_{0.0f}, pz_{0.0f};
    float ax_{0.0f}, ay_{0.0f}, az_{0.0f};
    float slow_{0.0f};
    std::uint32_t revs_{0};
};

// The chain's tip this fast (m/s, world) hurts: below it the chain is dragged or hangs.
inline constexpr float HurtTipSpeed = 4.0f; // was 6: a slower lash (a slap on the ground) hurts too

// A crack: the chain lashed out by a motion the swing detector does not count as a swing (the
// wrist alone: the payout follows the weapon's far end, the swing gate the arm), so what you see
// lashing would hurt nothing. The presenter opens it a window of its own (as a revolution's: a
// swing paid from the combo budget) when no window is open, the chain is asked out and paid out
// past `out` of the light's reach, its tip is at `tip` (three times the speed that hurts: a crack,
// not a drag), a violent drive came within `drive_s` (the swing's speed at the full payout's,
// whip_payout_speed: a flick, not the stroke's return) and the arm is not bringing the weapon
// back, nor the wrist lifting it back (the Beast Cutter's flick recall, core/vr/whip_flick.h: a
// crack is the snap thrown out). Once a surge: a committed window during the lash covers it, and
// only a spell of `slow_s` with the tip below HurtTipSpeed (or a new lash) lets the next surge
// crack. `gap_s` after a window closed: the stroke's own follow-through is never billed twice.
struct CrackConfig {
    float out{0.5f};     // the payout (0 the stick .. 1 the light's reach)
    float tip{12.0f};    // m/s, the chain's tip
    float drive_s{0.25f}; // the drive (the swing at v_drive) this recent
    float slow_s{0.15f}; // the tip slow this long: the next surge may crack
    float gap_s{0.25f};  // s after a window closed
};

struct CrackInput {
    float dt{0.0f};
    bool swings{false};          // swings may attack (the presenter's route)
    bool window_open{false};     // the tracker's window after this frame's Step
    bool window_committed{false};
    float since_close_s{10.0f};  // since the last window closed
    std::uint32_t lash{0};       // the chain's lash (bloodborne_whip.cpp counts them; 0: none yet)
    bool asked_out{false};       // the payout asks the chain out (not reeling in)
    float out{0.0f};             // the payout now (as WhipFeel::out)
    float tip{0.0f};             // the chain's tip, m/s
    float swing_speed{0.0f};     // the presenter's swing speed (the weapon's far end), m/s
    float v_drive{5.5f};         // a drive: the swing this fast (whip_payout_speed)
    bool recovery{false};        // the classifier: the weapon brought back
    // The Beast Cutter's flick recall: the wrist lifting the weapon back to reel the chain in
    // (core/vr/whip_flick.h WristFlick::Lifting; WhipFeel::lifting): a recall, never a crack.
    bool lifting{false};
};

class CrackGate {
public:
    // One presenter frame: true when this surge cracks (open its window now).
    bool Step(const CrackInput& in, const CrackConfig& c) {
        const float dt = std::clamp(in.dt, 0.0f, 0.25f);
        since_drive_ = in.swing_speed >= in.v_drive ? 0.0f : std::min(since_drive_ + dt, 10.0f);
        if (!in.swings || in.lash == 0) {
            covered_ = in.lash; // nothing cracks what swings could not hit
            slow_ = 0.0f;
            return false;
        }
        if (in.window_open) {
            if (in.window_committed) {
                covered_ = in.lash;
            }
            slow_ = 0.0f;
            return false;
        }
        if (!(in.tip >= HurtTipSpeed)) {
            slow_ += dt;
            if (slow_ >= c.slow_s) {
                covered_ = 0; // the surge is over
            }
            return false;
        }
        slow_ = 0.0f;
        if (in.lash == covered_ || !in.asked_out || !(in.out >= c.out) || !(in.tip >= c.tip) ||
            since_drive_ > c.drive_s || in.recovery || in.lifting || in.since_close_s < c.gap_s) {
            return false;
        }
        covered_ = in.lash;
        ++cracks_;
        return true;
    }
    std::uint32_t Cracks() const {
        return cracks_;
    }

private:
    std::uint32_t covered_{0};
    float slow_{0.0f};
    float since_drive_{10.0f};
    std::uint32_t cracks_{0};
};

// A hit's speed factor for a whip (core/vr/melee_window.h DamageFactor takes a blade's tip speed,
// x0.75 at 3.5 m/s .. x1.10 at 9 m/s): the chain's tip speed mapped onto that scale, 8 m/s (a
// slow lash) to 30 m/s (a full one; the game's own light lash: 44 m/s in the grip's frame).
inline float BladeEquivalentTip(float chain_tip_mps) {
    if (!std::isfinite(chain_tip_mps)) {
        return 0.0f;
    }
    return std::max(0.0f, 3.5f + (chain_tip_mps - 8.0f) * (5.5f / 22.0f));
}

} // namespace VR::Whip
