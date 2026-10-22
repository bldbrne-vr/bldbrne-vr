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
#include <cstdint>

#include "core/vr/arm_ik.h"
#include "core/vr/whip_lash.h"

// The physical whip (vr.ini whip=physical; core/vr/bloodborne_whip.h is the game side): the
// segments of a whip form (the Beast Cutter's form B) as a chain driven by the hunter's hand, so
// the whip you swing is the whip you see and the whip that hits. Pure math, unit-tested
// (tests/test_whip_chain.cpp); an offline prototype came first.
//
// What the game does (offline analysis): the chain is animation only. In its idle
// clip (a999) the 26 chain bones are a straight, rigid line 1.17 m from the grip; every attack clip
// stretches the links (their translations) up to x4.8 (R1: the tip 3.7 m out) or x7 (R2: 5.5 m) for
// 0.1-0.3 s while the clip's own lash sweeps them. No physics, no cloth: swung without that clip
// the chain is a rigid stick in the hand.
//
// The chain here, every game frame (substeps of 1/240 s, the hand's frame interpolated between the
// frames):
//  - points p_0..p_n-1 are the chain bones; p_0 (the blade root on the handle) is pinned to the
//    hand's frame;
//  - a soft pull toward the idle shape scaled by the extension (firm at the root, soft at the tip,
//    relaxed while the whip is out), so at rest it holds the game's idle line (no flop) and in a
//    swing it lags, bends and lashes;
//  - gravity and air drag; a bending pass (each point toward its neighbours' middle) against kinks;
//  - Beast Cutter: mass-weighted distance constraints, with a heavier front and gravity.
//    A hand tug transfers tension along the chain. Other whips use dynamic follow-the-leader
//    (Mueller et al. 2012): every link exactly its length in one pass
//    from the root, stable at any speed, with its velocity correction, and no link turned more
//    than 75 degrees from the one before (a joint limit: no fold);
//  - the payout: the links stretch (as the game's attack clips do) to the extension the swing's
//    speed asks for (core/vr/whip_spin.h Payout: up to the reach of the game's own move, light
//    x4.0: the tip 3.6 m out, heavy x6.0, charged x6.8) at most in `t_out` (0.12 s, the game's
//    0.13 s); the stretch carries the chain out along its own shape (the lash); it reels back in
//    `t_in` without an inward kick. Out and swung, the idle shape's pull is `release`d (a spin
//    stands out like a helicopter blade); out and slow, `release_slow` (it hangs like a chain).
//  - Bone frames (ChainFrames): each chain bone's +X runs along its link; the twist is carried down
//    the chain from the root bone by parallel transport (never flips when the whip trails behind).
namespace VR::Whip {

namespace B = VR::Body;

constexpr int MaxPoints = 40;

struct ChainConfig {
    // The pull toward the idle shape, the fraction of the gap closed per 1/240 s: root .. tip
    // (k = k_tip + k_root * (1 - i / (n - 1))^k_pow).
    float k_root{0.35f};
    float k_pow{2.0f};
    float k_tip{0.01f};
    float release{0.97f}; // ... x (1 - release * e) while the whip is out (e = 0 idle .. 1 full)
    // ... and while the swing is slow (ReleaseForSpeed): the chain out but no longer swung hangs
    // and falls like a chain (it cannot stand up as a 3.5 m pole) until it reels in.
    float release_slow{0.995f};
    float release_span{1.0f}; // ... in full only over this much extension over the idle (x2)
    float damp{1.0f};    // air drag: velocity x exp(-damp * t) (3 before the spin: it bled a
                         // spun chain's speed, the cone stood 1 m above the hand)
    // The drag on the chain's sideways (horizontal) motion while the swing is slow (the hook sets
    // `side_damp` from the swing's speed, SideDampForSpeed: 0 swung .. `side_damp_slow` slow). A
    // stroke that stops leaves the tip whipping past and settling to the side instead of carrying
    // on round the hunter: with the air drag alone a forehand's tip went on 130-180 degrees past
    // the front, behind him (the verification run wv_bc1; whipver/exp_windup.cpp).
    float side_damp{0.0f};
    float side_damp_slow{6.0f};
    float gravity{9.81f};
    float ftl_damp{0.9f}; // DFTL's velocity correction
    float bend{0.25f};    // the bending pass: fraction toward the neighbours' middle
    int bend_iters{2};
    float max_bend{1.309f}; // rad (75 degrees): the most one link turns from the one before
    float t_out{0.12f};  // s, idle to the full payout
    float t_in{0.10f};   // s, full back to idle
    // Fraction of geometric payout retained as outward velocity (0: no extension kick).
    float payout_momentum{1.0f};
    // Mass-weighted constraint solve for the Beast Cutter: tension travels both ways.
    // The old follow-the-leader velocity correction cancels much of gravity on a free chain.
    bool weighted{false};
    float tip_mass{1.0f};
    float link_velocity_damp{0.0f}; // internal damping conserves free-chain linear momentum
    float mass_taper{2.0f}; // exponent along the chain; >1 concentrates weight near the end
    float mass_ramp_end{1.0f}; // normalized position where the mass reaches tip_mass
    int constraint_iters{64};
    int handle_links{0}; // proximal links coupled to the handle; the remaining chain stays free
    // Bounded, damped drive on the chain during a deliberate swing. It fades
    // toward the tip and switches off with hand motion; resting weight stays gravitational.
    float swing_drive{0.0f}; // angular frequency (1/s), zero disables the assistance
    float launch_drive{0.0f}; // travelling pickup on each deliberate Cane stroke
    float drive_span{1.3f}; // linear taper: the heavy tip receives 23% of the handle drive
    float drive_accel{240.0f}; // m/s^2 maximum before the spatial taper
    bool lock_closed{false}; // hold the closed chain rigidly while the Cutter trigger is down
    float idle_velocity_damp{0.0f}; // settle relative to the moving handle, not world space
    float substep{1.0f / 240.0f};
    int max_substeps{64};
    float max_dt{0.25f}; // a longer gap (a load, a menu): the chain starts again at rest
    // The ground under the hunter (world y, the hook sets it from his feet each frame; below
    // -1e8: none): no chain point goes below it, and a point on it slides with `floor_slide` of
    // its sideways motion. Without it the 3.5 m chain, out and slow, hung 1.5 m through the floor
    // (the verification run wv_bc1: the tip at y -52.1 with the feet at -50.6).
    float floor_y{-1e9f};
    float floor_slide{0.85f};
    bool inelastic_floor{false}; // remove landing bounce, never pin a point to the ground
};

inline float ChainPointMass(float u, const ChainConfig& c) {
    const float along = std::clamp(u / std::max(c.mass_ramp_end, 0.001f), 0.0f, 1.0f);
    return 1.0f + (std::max(c.tip_mass, 0.05f) - 1.0f) * std::pow(along, c.mass_taper);
}

// Both chain weapons keep their proximal links coupled to the handle, so wrist rotation
// transfers torque even at full extension. The front remains free; the Cane is lighter and
// returns sooner. At idle, settle relative to the moving hand instead of dragging in world space.
inline ChainConfig ConfigFor(std::int32_t variation) {
    ChainConfig c{};
    if (variation == 2400 || variation == 2200) {
        c.weighted = true;
        c.substep = 1.0f / 240.0f;
        c.constraint_iters = 192;
        c.max_substeps = 128;
        // A light flail: most of the extra mass belongs to the terminal blade.
        // A whip: mass decreases toward the tip, so a travelling flick accelerates it.
        c.tip_mass = variation == 2400 ? 2.0f : 0.20f;
        c.mass_taper = variation == 2400 ? 8.0f : 1.0f;
        c.max_bend = (variation == 2400 ? 50.0f : 60.0f) * B::Deg;
        c.k_pow = 3.0f;
        c.k_tip = 0.02f;
        c.handle_links = 4;
        c.idle_velocity_damp = 45.0f;
        c.release = 1.0f;
        c.release_slow = 1.0f;
        c.damp = 2.0f;
        c.side_damp_slow = 2.0f;
        c.floor_slide = 0.995f;
        // Cane: slightly more gradual payout; keep the accepted return motion.
        // Its shorter pause before returning is set separately by HoldFor.
        c.t_out = 0.18f;
        c.t_in = variation == 2400 ? 0.35f : 0.25f;
        c.payout_momentum = 1.0f;
        if (variation == 2200) {
            c.launch_drive = 10.0f;
        }
        if (variation == 2400) {
            // Beast Cutter: a hinged chain carrying distributed blade weight. Do not turn
            // the first four paid-out links into a rigid half-metre extension of the handle.
            c.handle_links = 1;
            // Distributed weight: 1 -> 1.75 -> 2.5 at the midpoint, then level to the tip.
            c.tip_mass = 2.5f;
            c.mass_taper = 1.0f;
            c.mass_ramp_end = 0.5f;
            // Slightly reduce relative link rebound without changing bend limits or mass.
            c.link_velocity_damp = 20.0f; // (18 on 2026-10-14..15 felt floaty: the tester, back to 20)
            c.swing_drive = 22.0f * 1.08f;
            // Preserve the response boost with the new 8% longer reach.
            c.drive_accel = 240.0f * 1.08f * 1.08f;
            c.damp = 1.45f; // air drag (1.35 until 2026-10-15: the tester, settles a little sooner)
            c.payout_momentum = 0.35f;
            c.t_out = 0.14f;
            c.t_in = 0.40f;
            c.inelastic_floor = true;
            c.floor_slide = 0.85f;
        }
    }
    return c;
}

// Gentle direction assistance for an outgoing lash. Both the tip's sightline and its motion
// must already be near the view. Never adds speed, grabs a return stroke, or steers an idle tip.
struct GazeAssist {
    bool active{false};
    B::Vec3 eye{};
    B::Vec3 forward{};
};

inline B::Vec3 AssistLash(B::Vec3 velocity, B::Vec3 tip, B::Vec3 root, const GazeAssist& gaze,
                          float dt) {
    const float speed = B::Length(velocity);
    if (!gaze.active || !(dt > 0.0f) || !B::IsFinite(velocity) || !B::IsFinite(tip) ||
        !B::IsFinite(root) || !B::IsFinite(gaze.eye) || !B::IsFinite(gaze.forward) ||
        B::Length(gaze.forward) < 0.5f || speed < 2.0f || B::Dot(velocity, tip - root) <= 0.0f) {
        return velocity;
    }
    const B::Vec3 forward = B::Normalize(gaze.forward);
    if (B::Dot(B::Normalize(tip - gaze.eye), forward) < std::cos(30.0f * B::Deg)) {
        return velocity;
    }
    const B::Vec3 along = velocity * (1.0f / speed);
    const float angle = B::Angle(along, forward);
    const float cone = 20.0f * B::Deg;
    if (!(angle > 1e-5f && angle < cone)) {
        return velocity;
    }
    const float weight = 1.0f - angle / cone;
    const float turn = std::min(angle, 45.0f * B::Deg * std::min(dt, 0.05f) * weight * weight);
    const B::Vec3 axis = B::Normalize(B::Cross(along, forward), B::AnyPerpendicular(along));
    return B::RotateAbout(along, axis, turn) * speed;
}

// A rigid frame in the world: x_world = t + Rotate(q, x_part).
struct Frame {
    B::Vec3 t{};
    B::Quat q{};
};

inline B::Vec3 Apply(const Frame& f, B::Vec3 p) { return f.t + B::Rotate(f.q, p); }
inline B::Vec3 ApplyInverse(const Frame& f, B::Vec3 p) {
    return B::Rotate(B::Conjugate(f.q), p - f.t);
}
inline Frame LerpFrame(const Frame& a, const Frame& b, float k) {
    return {B::Lerp(a.t, b.t, k), B::Slerp(a.q, b.q, k)};
}

class Chain {
public:
    // The idle shape: the chain bones' positions in the part's frame (the game's idle clip), from
    // the blade root out. Returns false for fewer than 3 points, too many, or a degenerate link.
    bool Init(const B::Vec3* rest, int n) {
        if (n < 3 || n > MaxPoints) {
            return false;
        }
        n_ = n;
        for (int i = 0; i < n; ++i) {
            rest_[i] = rest[i];
        }
        for (int i = 1; i < n; ++i) {
            len_[i] = B::Length(rest[i] - rest[i - 1]);
            if (!(len_[i] > 1e-5f)) {
                return false;
            }
        }
        ready_ = false;
        s_ = 1.0f;
        return true;
    }

    // At rest in the idle shape in frame `f`.
    void Reset(const Frame& f) {
        for (int i = 0; i < n_; ++i) {
            x_[i] = Apply(f, rest_[i]);
            v_[i] = {};
        }
        s_ = 1.0f;
        payout_rate_ = 0.0f;
        drive_left_ = drive_idle_ = 0.0f;
        lash_drive_.Reset();
        drive_direction_ = {};
        span_ = 0.0f;
        ready_ = true;
    }

    // The hunter turned (a snap turn) or moved at once: the whole chain goes with him, rigidly
    // (`r` about `from`, then to `to`), velocities turned with it. Without it a 30 degree snap
    // turn left the idle stick 0.68 m behind the hand for a frame, springing back at 32 m/s over
    // 0.18 s (whipver/exp_adv.cpp).
    void Carry(const B::Quat& r, B::Vec3 from, B::Vec3 to) {
        drive_direction_ = B::Rotate(r, drive_direction_);
        lash_drive_.Carry(r);
        for (int i = 0; i < n_; ++i) {
            x_[i] = to + B::Rotate(r, x_[i] - from);
            v_[i] = B::Rotate(r, v_[i]);
        }
    }

    // At rest in the pose `points` (world): a clip's pose the chain takes over from.
    void ResetTo(const B::Vec3* points, float s) {
        for (int i = 0; i < n_; ++i) {
            x_[i] = points[i];
            v_[i] = {};
        }
        payout_rate_ = 0.0f;
        drive_left_ = drive_idle_ = 0.0f;
        lash_drive_.Reset();
        drive_direction_ = {};
        s_ = std::max(s, 1.0f);
        span_ = s_ - 1.0f;
        ready_ = true;
    }

    // One game frame: the part frame went from `from` to `to` over `dt`; `s_target` is the
    // extension asked for (1 = idle). Returns the substeps run (0: reset, the gap was too long).
    int Step(const Frame& from, const Frame& to, float dt, float s_target, const ChainConfig& c,
             const GazeAssist& gaze = {}) {
        if (!ready_ || !(dt >= 0.0f) || dt > c.max_dt) {
            Reset(to);
            return 0;
        }
        if (dt <= 0.0f) {
            return 0;
        }
        Frame start = from;
        if (c.weighted) {
            const float travel = B::Length(to.t - from.t);
            const float turn = 2.0f * std::acos(std::clamp(std::abs(B::Dot(from.q, to.q)), 0.0f, 1.0f));
            // Tracking/animation discontinuities must not be interpreted as a physical flick.
            // Restart at short reach: rotating an extended chain through the floor would
            // itself create a large contact impulse. Normal locomotion carries happen upstream.
            if ((travel > 0.30f && travel / dt > 20.0f) ||
                (turn > 60.0f * B::Deg && turn / dt > 70.0f)) {
                Reset(to);
                start = to;
            }
        }
        const int steps =
            std::clamp(static_cast<int>(std::ceil(dt / c.substep)), 1, std::max(c.max_substeps, 1));
        const float h = dt / static_cast<float>(steps);
        s_target = std::max(s_target, 1.0f);
        for (int k = 1; k <= steps; ++k) {
            Substep(LerpFrame(start, to, static_cast<float>(k - 1) / steps),
                    LerpFrame(start, to, static_cast<float>(k) / steps), h, s_target, c, gaze);
        }
        for (int i = 0; i < n_; ++i) {
            if (!B::IsFinite(x_[i]) || !B::IsFinite(v_[i])) {
                Reset(to); // never hand the game a NaN
                return 0;
            }
        }
        return steps;
    }

    int Count() const { return n_; }
    bool Ready() const { return ready_; }
    B::Vec3 Point(int i) const { return x_[i]; }
    B::Vec3 Velocity(int i) const { return v_[i]; }
    B::Vec3 Rest(int i) const { return rest_[i]; }
    float Extension() const { return s_; }
    // 0 at idle .. 1 at the payout's peak so far.
    float Out() const { return span_ > 1e-4f ? std::clamp((s_ - 1.0f) / span_, 0.0f, 1.0f) : 0.0f; }
    // The fastest point (m/s, world) and the tip's speed.
    float MaxSpeed() const {
        float m = 0.0f;
        for (int i = 0; i < n_; ++i) {
            m = std::max(m, B::Length(v_[i]));
        }
        return m;
    }
    float TipSpeed() const { return n_ > 0 ? B::Length(v_[n_ - 1]) : 0.0f; }
    // The chain's length along its links now (m).
    float Length() const {
        float l = 0.0f;
        for (int i = 1; i < n_; ++i) {
            l += B::Length(x_[i] - x_[i - 1]);
        }
        return l;
    }
    // How far the chain is from its closed idle shape in frame `f` (m): its farthest point.
    float ClosedDeviation(const Frame& f) const {
        float m = 0.0f;
        for (int i = 0; i < n_; ++i) {
            m = std::max(m, B::LengthSq(x_[i] - Apply(f, rest_[i])));
        }
        return std::sqrt(m);
    }
    // The sharpest bend between two consecutive links (rad).
    float MaxBend() const {
        float m = 0.0f;
        for (int i = 1; i + 1 < n_; ++i) {
            m = std::max(m, B::Angle(x_[i] - x_[i - 1], x_[i + 1] - x_[i]));
        }
        return m;
    }

private:
    void Substep(const Frame& previous, const Frame& f, float h, float s_target, const ChainConfig& c,
                 const GazeAssist& gaze) {
        // The payout (or reeling in) at a bounded rate.
        const float s_old = s_;
        if (s_target > s_) {
            span_ = std::max(span_, s_target - 1.0f);
            const float rate = std::max(s_target - 1.0f, 0.5f) / std::max(c.t_out, 1e-3f);
            s_ = std::min(s_ + rate * h, s_target);
        } else if (s_target < s_) {
            const float rate = std::max(span_, 0.5f) / std::max(c.t_in, 1e-3f);
            s_ = std::max(s_ - rate * h, s_target);
            if (s_ <= 1.0f + 1e-4f) {
                span_ = 0.0f;
            }
        }
        const float s = s_;
        if (c.lock_closed && s <= 1.0001f) {
            // Once reeled in, it is a club. Report hand motion, not the final closure's
            // positional correction, as velocity so recall cannot manufacture a hit speed.
            for (int i = 0; i < n_; ++i) {
                x_[i] = Apply(f, rest_[i]);
                v_[i] = (x_[i] - Apply(previous, rest_[i])) * (1.0f / h);
            }
            payout_rate_ = 0.0f;
            return;
        }
        // The release: the chain's share of this payout's peak, but of at least `release_span`
        // over the idle: a small payout (a moderate motion, x1.03) once released the whole stick
        // (its share of a peak of x1.03 is all of it), which drooped 0.9 m to the floor for the
        // hold and snapped back up at the reel-in (40 m/s, bends of 75 degrees: the verification
        // run wv_bc2). A real payout (the light's x4) is released as before.
        // The weighted chain releases by its actual length, not its target/peak: increasing the
        // requested reach must never switch the straightening motor back on midway through a lash.
        const float e = std::clamp(
            (s - 1.0f) / std::max(c.weighted ? 0.0f : span_, std::max(c.release_span, 1e-3f)), 0.0f,
            1.0f);
        const float release = 1.0f - c.release * e;
        const B::Vec3 root = Apply(f, rest_[0]);
        if (s > s_old && c.weighted) {
            // Changing reach is a geometric actuator, not a multiplier on existing velocity.
            // Previously prediction (x + h*v) was scaled by s/s_old EVERY substep, so payout
            // amplified momentum even when payout_momentum was zero.
            const float k = s / s_old;
            const float rate = (s - s_old) / h * e;
            for (int i = 1; i < n_; ++i) {
                const B::Vec3 material = (x_[i] - x_[0]) * (1.0f / s_old);
                const B::Vec3 delta = (x_[i] - x_[0]) * (k - 1.0f);
                x_[i] += delta;
                // Couple changes in payout speed once, not payout speed as an acceleration
                // every substep. At constant payout this adds no further kinetic energy.
                v_[i] += material * ((rate - payout_rate_) * c.payout_momentum);
            }
            payout_rate_ = rate;
        } else {
            payout_rate_ = 0.0f;
        }
        if (s < s_old) {
            // Reeling in: the chain shrinks toward its root, its velocity with it (no inward kick
            // that would fold the tip).
            const float k = s / s_old;
            for (int i = 1; i < n_; ++i) {
                x_[i] = x_[0] + (x_[i] - x_[0]) * k;
                v_[i] = v_[i] * k;
            }
        }
        const float drag = std::exp(-c.damp * h);
        // (The sideways drag acts on the motion relative to the hand: a hunter running or turning
        // carries the chain with him.)
        const float side = std::exp(-std::max(c.side_damp, 0.0f) * h);
        const B::Vec3 hand_v = (root - x_[0]) * (1.0f / h);
        const B::Vec3 g{0.0f, -c.gravity * h * h * (c.weighted ? e : 1.0f), 0.0f};
        const float settle = 1.0f - std::exp(-c.idle_velocity_damp * (1.0f - e) * h);
        // Apply one bounded rotation to the outer links, tapering toward the pinned root.
        // Use relative motion so walking sideways cannot aim a resting chain.
        const B::Vec3 tip_v = v_[n_ - 1] - hand_v;
        const B::Vec3 assisted = AssistLash(tip_v, x_[n_ - 1], root, gaze, h);
        const bool steering = B::LengthSq(assisted - tip_v) > 1e-12f;
        const B::Quat steer = steering ? B::FromTo(tip_v, assisted) : B::Quat{};
        const B::Vec3 drive_probe = rest_[0] + B::Normalize(rest_[1] - rest_[0]) * 0.7f;
        const B::Vec3 drive_velocity = (Apply(f, drive_probe) - Apply(previous, drive_probe)) * (1.0f / h);
        const float drive_speed = B::Length(drive_velocity);
        lash_drive_.Step(drive_velocity, h,
                         c.launch_drive > 0 && s_target > 1 && !c.lock_closed, s_old == 1);
        if (drive_speed > 0.75f) {
            const B::Vec3 direction = drive_velocity * (1.0f / drive_speed);
            if (B::LengthSq(drive_direction_) < 0.5f ||
                B::Dot(direction, drive_direction_) < -0.25f) drive_left_ = 0.30f;
            drive_direction_ = direction;
            drive_idle_ = 0.0f;
        } else if ((drive_idle_ += h) > 0.35f) {
            drive_direction_ = {};
        }
        // Payout can prolong a drive, but must not cut a fresh reversal short.
        if (s > s_old) drive_left_ = std::max(drive_left_, 0.12f);
        drive_left_ = std::max(0.0f, drive_left_ - h);
        const float drive_ramp = std::clamp((drive_speed - 0.75f) / 1.75f, 0.0f, 1.0f);
        // Only feed the throw/reversal. Continuous angular damping fought a sustained spin
        // and robbed the front of the momentum that should hold it out against gravity.
        const float motion_drive = c.lock_closed || s_target <= 1.0f ? 0.0f : e *
            drive_ramp * drive_ramp * (3.0f - 2.0f * drive_ramp);
        std::array<B::Vec3, MaxPoints> p{};
        std::array<bool, MaxPoints> contact{};
        p[0] = root;
        for (int i = 1; i < n_; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(n_ - 1);
            const B::Vec3 guided =
                steering ? hand_v + B::Rotate(B::ScaleRotation(steer, u * u), v_[i] - hand_v)
                         : v_[i];
            B::Vec3 v{hand_v.x + (guided.x - hand_v.x) * side, guided.y,
                      hand_v.z + (guided.z - hand_v.z) * side};
            // At short reach settle in the hand's moving frame. World-space drag made gentle
            // movements feel sticky and left the undamped idle spring oscillating after a stop.
            const B::Vec3 rigid_v = (Apply(f, rest_[i]) - Apply(previous, rest_[i])) * (1.0f / h);
            v = v + (rigid_v - v) * settle;
            const float swing_drive = c.swing_drive > 0 ? c.swing_drive :
                c.launch_drive * lash_drive_.Strength();
            const float drive = c.swing_drive > 0 ?
                (c.lock_closed || s_target <= 1.0f ? 0.0f : e *
                 std::clamp(drive_left_ / .06f, 0.0f, 1.0f) *
                 drive_ramp * drive_ramp * (3.0f - 2.0f * drive_ramp)) :
                motion_drive * lash_drive_.Weight(u);
            if (swing_drive > 0.0f && drive > 0.0f && u < c.drive_span &&
                i > c.handle_links) {
                // Soft coupling, not additional pinned links. Taper the moving handle
                // drive down the chain so the weighted front keeps its own inertia.
                const B::Vec3 local = rest_[0] + (rest_[i] - rest_[0]) * s;
                const B::Vec3 target = Apply(f, local);
                const B::Vec3 target_v = (target - Apply(previous, local)) * (1.0f / h);
                const float omega = swing_drive;
                B::Vec3 acceleration = (target - x_[i]) * (omega * omega) +
                                       (target_v - v) * (2.0f * omega);
                const float magnitude = B::Length(acceleration);
                if (magnitude > c.drive_accel)
                    acceleration = acceleration * (c.drive_accel / magnitude);
                const float taper = 1.0f - u / std::max(c.drive_span, 1e-3f);
                v += acceleration * (h * drive * taper);
            }
            p[i] = x_[i] + v * (h * drag) + g;
            contact[i] = c.floor_y > -1e8f && p[i].y <= c.floor_y;
        }
        if (s > s_old && !c.weighted) {
            // The payout: the chain stretches out from its root along its own shape (the lash).
            const float k = s / s_old;
            for (int i = 1; i < n_; ++i) {
                p[i] = x_[0] + (p[i] - x_[0]) * k;
                // Move the velocity reference with the imposed extension. Only the requested
                // share becomes momentum; otherwise payout itself launches the tip upwards.
                x_[i] = x_[i] + (x_[i] - x_[0]) * ((k - 1.0f) * (1.0f - c.payout_momentum));
            }
        }
        // Toward the idle shape (scaled by the extension), firm at the root, soft at the tip.
        const float steps240 = h * 240.0f;
        for (int i = 1; i < n_; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(n_ - 1);
            const float k = std::clamp((c.k_tip + c.k_root * std::pow(1.0f - u, c.k_pow)) * release,
                                       0.0f, 0.99f);
            const float a = 1.0f - std::pow(1.0f - k, steps240);
            const B::Vec3 target = root + B::Rotate(f.q, rest_[i] - rest_[0]) * s;
            p[i] = p[i] + (target - p[i]) * a;
        }
        // Bending: each inner point toward its neighbours' middle.
        for (int it = 0; it < (c.weighted ? 0 : c.bend_iters); ++it) {
            for (int i = 1; i + 1 < n_; ++i) {
                const B::Vec3 mid = (p[i - 1] + p[i + 1]) * 0.5f;
                p[i] = p[i] + (mid - p[i]) * c.bend;
            }
        }
        if (c.weighted) {
            const int pinned = std::clamp(c.handle_links, 0, n_ - 2);
            for (int i = 1; i <= pinned; ++i) {
                p[i] = root + B::Rotate(f.q, rest_[i] - rest_[0]) * s;
            }
            std::array<float, MaxPoints> inv_mass{};
            std::array<float, MaxPoints> min_span{};
            const float cos_bend = std::cos(c.max_bend);
            for (int i = 1; i < n_; ++i) {
                const float u = static_cast<float>(i) / static_cast<float>(n_ - 1);
                inv_mass[i] = i <= pinned ? 0.0f :
                    1.0f / ChainPointMass(u, c);
                if (i + 1 < n_) {
                    const float a = len_[i] * s, b = len_[i + 1] * s;
                    min_span[i] = std::sqrt(a * a + b * b + 2 * a * b * cos_bend);
                }
            }
            const auto distance = [&](int a, int b, float length, bool minimum) {
                const B::Vec3 delta = p[b] - p[a];
                const float actual = B::Length(delta);
                if (inv_mass[a] + inv_mass[b] <= 0.0f || actual <= 1e-7f ||
                    (minimum && actual >= length))
                    return;
                const B::Vec3 correction =
                    delta * ((actual - length) / (actual * (inv_mass[a] + inv_mass[b])));
                p[a] += correction * inv_mass[a];
                p[b] = p[b] - correction * inv_mass[b];
            };
            // Alternating sweeps prevent a preferred travel direction. A backwards hand tug
            // now transfers tension to the weighted front instead of waiting for timed reel-in.
            for (int pass = 0; pass < c.constraint_iters; ++pass) {
                for (int k = 1; k < n_; ++k) {
                    const int i = (pass & 1) ? n_ - k : k;
                    distance(i - 1, i, len_[i] * s, false);
                }
                for (int i = 1; i + 1 < n_; ++i) {
                    distance(i - 1, i + 1, min_span[i], true);
                }
                if (c.floor_y > -1e8f) {
                    for (int i = pinned + 1; i < n_; ++i) {
                        contact[i] = contact[i] || p[i].y <= c.floor_y;
                        p[i].y = std::max(p[i].y, c.floor_y);
                    }
                }
            }
        }
        // Follow the leader: every link exactly its (stretched) length, from the root out, and
        // never turned more than `max_bend` from the link before it (a joint limit: the game's
        // frames folded the chain's tip back on itself for a frame or two, bends of 145-166
        // degrees, when the hand stopped hard or the chain reeled in curled).
        std::array<B::Vec3, MaxPoints> d{};
        const float cos_max = std::cos(c.max_bend);
        B::Vec3 prev_dir{};
        for (int i = 1; i < n_; ++i) {
            B::Vec3 dir = p[i] - p[i - 1];
            float l = B::Length(dir);
            if (!(l > 1e-7f)) {
                dir = B::Rotate(f.q, rest_[i] - rest_[i - 1]);
                l = B::Length(dir);
            }
            dir = dir * (1.0f / l);
            if (i >= 2 && B::Dot(dir, prev_dir) < cos_max) {
                // Turn it back toward the link before, onto the cone's edge.
                const B::Vec3 axis =
                    B::Normalize(B::Cross(prev_dir, dir), B::AnyPerpendicular(prev_dir));
                dir = B::RotateAbout(prev_dir, axis, c.max_bend);
            }
            prev_dir = dir;
            const B::Vec3 q = p[i - 1] + dir * (len_[i] * s);
            d[i] = q - p[i];
            p[i] = q;
        }
        if (c.floor_y > -1e8f) {
            // The ground: the chain lies on it (a link on the ground may come out a little short:
            // the next substeps' pass restores it as the chain lifts).
            for (int i = 1; i < n_; ++i) {
                if (p[i].y < c.floor_y) {
                    p[i].y = c.floor_y;
                    contact[i] = true;
                    // Contact friction belongs in velocity, not a second position correction.
                    // Scaling x/z here after the length solve collapsed links on the floor.
                }
            }
        }
        const float inv_h = 1.0f / h;
        for (int i = 1; i < n_; ++i) {
            B::Vec3 v = (p[i] - x_[i]) * inv_h;
            if (!c.weighted && i + 1 < n_) {
                v = v - d[i + 1] * (c.ftl_damp * inv_h);
            }
            if (c.floor_y > -1e8f && p[i].y <= c.floor_y + 1e-5f) {
                v.y = std::max(v.y, 0.0f);
                const float friction = std::pow(c.floor_slide, h * 240.0f);
                v.x *= friction;
                v.z *= friction;
            }
            v_[i] = v;
        }
        v_[0] = (p[0] - x_[0]) * inv_h;
        if (c.link_velocity_damp > 0.0f) {
            // Dissipate relative link motion instead of world-space swing momentum. The
            // paired mass-weighted impulses are equal and opposite; pinned handle links
            // are excluded. This branch is disabled for the accepted Threaded Cane profile.
            const int first = std::clamp(c.handle_links + 1, 1, n_ - 1);
            const float blend = 1.0f - std::exp(-c.link_velocity_damp * e * h * 0.5f);
            const auto inverse_mass = [&](int i) {
                const float u = static_cast<float>(i) / (n_ - 1);
                return 1.0f / ChainPointMass(u, c);
            };
            for (int pass = 0; pass < 2; ++pass) {
                for (int k = first + 1; k < n_; ++k) {
                    const int b = pass == 0 ? k : n_ - 1 - (k - first - 1);
                    const int a = b - 1;
                    const float wa = inverse_mass(a), wb = inverse_mass(b);
                    const B::Vec3 impulse = (v_[b] - v_[a]) * (blend / (wa + wb));
                    v_[a] += impulse * wa;
                    v_[b] = v_[b] - impulse * wb;
                }
            }
        }
        if (c.inelastic_floor && c.floor_y > -1e8f) {
            for (int i = std::max(1, c.handle_links + 1); i < n_; ++i) {
                if (contact[i] && p[i].y <= c.floor_y + 0.002f) {
                    // No restitution at a resting/landing contact. Once tension lifts a link
                    // clear of the surface, it is entirely free again (no grounded latch).
                    v_[i].y = 0.0f;
                }
            }
        }
        for (int i = 0; i < n_; ++i) {
            x_[i] = p[i];
        }
    }

    int n_{0};
    bool ready_{false};
    float s_{1.0f};
    float payout_rate_{0.0f};
    float drive_left_{0.0f}, drive_idle_{0.0f};
    LashDrive lash_drive_{};
    B::Vec3 drive_direction_{};
    float span_{0.0f}; // the payout's peak extension - 1 (the reel-in rate, Out())
    std::array<B::Vec3, MaxPoints> rest_{};
    std::array<float, MaxPoints> len_{};
    std::array<B::Vec3, MaxPoints> x_{};
    std::array<B::Vec3, MaxPoints> v_{};
};

// The chain bones' model rotations for points `p` (the part's frame), from their idle rotations
// `idle_q` (whose local +X runs along the idle links): the root bone turns by the shortest arc
// from its idle link to its link now, each next bone by the shortest arc from its parent's link to
// its own (parallel transport), so the twist never flips. The last bone takes its parent's link.
inline void ChainRotations(const B::Vec3* p, const B::Quat* idle_q, int n, B::Quat* out) {
    if (n < 2) {
        return;
    }
    const B::Vec3 x_axis{1.0f, 0.0f, 0.0f};
    B::Vec3 prev_dir = B::Normalize(p[1] - p[0], B::Rotate(idle_q[0], x_axis));
    out[0] = B::Normalize(B::FromTo(B::Rotate(idle_q[0], x_axis), prev_dir) * idle_q[0]);
    for (int i = 1; i < n; ++i) {
        const B::Vec3 dir = i + 1 < n ? B::Normalize(p[i + 1] - p[i], prev_dir) : prev_dir;
        // The idle twist offset between neighbours (straight in the game: none) is kept.
        const B::Quat rel = B::Conjugate(idle_q[i - 1]) * idle_q[i];
        const B::Quat carried = B::Normalize(out[i - 1] * rel);
        out[i] = B::Normalize(B::FromTo(B::Rotate(carried, x_axis), dir) * carried);
        prev_dir = dir;
    }
}

// The chain bones' local transforms (what the game's hkaPose takes) for model-space points `p` and
// rotations `q` under `parent` (the model transform of the first chain bone's parent), blended by
// `weight` with the clip's locals `clip` (whose scales are kept). Every rotation is renormalised at
// every bone: the clip's decompressed quaternions are not exactly unit, and the conjugate in
// LocalFromModel and the product in Compose square such an error at each of the 26 bones (in the
// first game test a 0.2% error left the tip dummy kilometres away). Returns the model transform of
// the last chain bone as the locals give it.
inline B::QsTransform ChainLocals(B::QsTransform parent, const B::Vec3* p, const B::Quat* q,
                                  const B::QsTransform* clip, int n, float weight,
                                  B::QsTransform* out) {
    parent.q = B::Normalize(parent.q);
    for (int k = 0; k < n; ++k) {
        const B::QsTransform model{p[k], q[k], B::Mul(parent.s, clip[k].s)};
        B::QsTransform lc = B::LocalFromModel(parent, model);
        lc.q = B::Normalize(lc.q);
        lc.s = clip[k].s;
        B::QsTransform blended = B::Blend(clip[k], lc, weight);
        blended.q = B::Normalize(blended.q);
        out[k] = blended;
        parent = B::Compose(parent, blended);
        parent.q = B::Normalize(parent.q);
    }
    return parent;
}

// The idle shape's pull while the whip is out, for the swing's speed (m/s): swung (a lash, a spin)
// `release` (the handle still drives the chain round: a spin stands out like a helicopter blade);
// slow (the hold before the reel-in), `release_slow`: the chain hangs and settles.
inline float ReleaseForSpeed(float speed, float v_keep, const ChainConfig& c) {
    const float t =
        std::clamp((speed - 0.5f * v_keep) / std::max(0.5f * v_keep, 1e-3f), 0.0f, 1.0f);
    return c.release_slow + (c.release - c.release_slow) * t * t * (3.0f - 2.0f * t);
}

// The sideways drag for the swing's speed (m/s): none swung (a spin keeps circling), up to
// `side_damp_slow` once the swing is slow (a stopped stroke's tip settles beside the hunter).
inline float SideDampForSpeed(float speed, float v_keep, const ChainConfig& c) {
    const float t =
        std::clamp((speed - 0.5f * v_keep) / std::max(0.5f * v_keep, 1e-3f), 0.0f, 1.0f);
    return c.side_damp_slow * (1.0f - t * t * (3.0f - 2.0f * t));
}

// The idle line along the blade: a whip whose idle clip holds its chain back along the forearm (the
// Threaded Cane's runs along the part's +Y; every blade, and the Beast Cutter's chain, points -Y,
// out of the hand: its canned idle turns the hunter's wrist instead) is turned 180 degrees about
// the part's Z through its root, so in VR the chain comes out of the front of the hand like the
// blade. Positions and rotations (x, y, z, w) in place; true when turned.
inline bool RestToBlade(B::Vec3* rest, B::Quat* rest_q, int n) {
    if (n < 2 || !(rest[n - 1].y - rest[0].y > 0.0f)) {
        return false;
    }
    const B::Quat half_turn{0.0f, 0.0f, 1.0f, 0.0f}; // 180 degrees about Z
    for (int i = 0; i < n; ++i) {
        const B::Vec3 o = rest[i] - rest[0];
        rest[i] = rest[0] + B::Vec3{-o.x, -o.y, o.z};
        rest_q[i] = B::Normalize(half_turn * rest_q[i]);
    }
    return true;
}

// Use the same blade-side basis for the Cane's animated chain as its corrected idle
// and physics chain. Rotate once at the chain root, preserving every child's coiling
// animation. Blending opposite bases otherwise sweeps the whole chain through 180 degrees.
inline B::QsTransform ClipRootToBlade(const B::QsTransform& parent,
                                     const B::QsTransform& local) {
    auto model = B::Compose(parent, local);
    model.q = B::Normalize(B::Quat{0, 0, 1, 0} * model.q);
    auto corrected = B::LocalFromModel(parent, model);
    corrected.q = B::Normalize(corrected.q);
    corrected.s = local.s;
    return corrected;
}

// How far out the payout goes for a move whose own clip reaches `reach_m` from the grip, for a
// chain whose idle tip dummy is `idle_tip_m` out with `links_m` of links (only the links stretch:
// the handle up to the root and the tip blade do not).
inline float ExtensionForReach(float reach_m, float idle_tip_m, float links_m) {
    if (!(links_m > 0.05f) || !(reach_m > idle_tip_m)) {
        return 1.0f;
    }
    return 1.0f + (reach_m - idle_tip_m) / links_m;
}

// Increase paid-out chain length only; keep the handle and closed weapon unchanged.
inline float ReachExtension(std::int32_t variation, float extension,
                            float idle_tip_m, float links_m) {
    // The existing extra paid-out lengths, retaining the Cane's accepted reach.
    const float extra = variation == 2400 ? 1.1536f : variation == 2200 ? 1.144f : 1.0f;
    const float out = std::max(0.0f, extension - 1.0f) * extra;
    if (variation == 2400 && out > 0.0f && links_m > 0.05f && idle_tip_m > 0.0f) {
        // Another 8% of the full grip-to-tip reach, added entirely to the paid-out
        // chain. The handle and closed weapon never grow. This is a reach CAP;
        // Payout still blends toward it using the swing's speed.
        const float previous_reach = idle_tip_m + out * links_m;
        return ExtensionForReach(previous_reach * 1.08f, idle_tip_m, links_m);
    }
    return 1.0f + out;
}

// The payout of move `kind` (0 light, 1 heavy, 2 charged; the window's MoveKind) for a whip whose
// own clips reach `reach` (m from the grip), x whip_reach_scale.
inline float PayoutFor(const std::array<float, 3>& reach, std::size_t kind, float scale,
                       float idle_tip_m, float links_m) {
    return ExtensionForReach(reach[std::min<std::size_t>(kind, 2)] * scale, idle_tip_m, links_m);
}

// ---- When the chain is driven, and when it lashes (the hook in bloodborne_whip.cpp) -------------
//
// The part's clip decides: in its idle clip (999000000) the chain is the simulation's; in any clip
// the game plays on it (an attack, a visceral) the clip's own pose stays. A transform's clip
// (a000_019xxx, played on the weapon only by the weapon-only transform, core/vr/transform.h) keeps
// its pose (the whip uncoils / coils in the hand) UNLESS a swing lashes while it plays in the whip
// form: a flick into the whip (the stroke goes on as the transform attack) or a swing started
// after the form locked. Then the simulation takes the chain over for the rest of that clip, so
// the flick lashes the whip out with your stroke.

inline constexpr std::int32_t IdleClipId = 999000000;

inline bool IsTransformClip(std::int32_t id) {
    return id >= 0 && id < 100000000 && id % 1000000 >= 19000 && id % 1000000 <= 19999;
}

struct DriveState {
    bool taken{false}; // the simulation took the chain over from a transform's clip
};

// `rig`: the whip is ours (whip=physical, its form B, the part in the right hand, the body on);
// `playing`: the part's clip; `swing`: the swing's speed has the chain out (Payout::Out). True:
// drive the chain.
inline bool StepDrive(DriveState& s, bool rig, std::int32_t playing, bool swing) {
    const bool transform = IsTransformClip(playing);
    s.taken = rig && transform && (s.taken || swing);
    return rig && (playing == IdleClipId || s.taken);
}

} // namespace VR::Whip
