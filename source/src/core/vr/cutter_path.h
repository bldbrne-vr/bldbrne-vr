// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cmath>

#include "core/vr/whip_chain.h"
#include "core/vr/whip_settings.h"

// The Beast Cutter's swing-path lash, frame by frame (whip_cutter_lab=model=path,...; the chain's
// half is ChainConfig::path in whip_chain.h). The user, 2026-11-08: a heavy chain that expands
// and smashes and curves (but not too much), out while the swing has momentum, shorter as it
// fades and all in once it is spent; a smash keeps its force a moment; the trigger and the flick
// back bring it in; the wrist never steers it. This is the hook's per-frame logic for it - its
// length from the swing's momentum, the arm's pivot, the pull's share - and the offline replay
// of a recorded session runs this same code, so what the replay shows is what the headset does.
namespace VR::Whip {

struct PathFrame {
    float dt{0.0f};
    B::Vec3 hand{};   // the grip (world)
    B::Vec3 weapon{}; // the weapon's line out of the hand (world, unit; zero: unknown)
    bool body_ok{false};
    B::Vec3 feet{};   // the hunter's feet (world)
    B::Vec3 facing{}; // ... and his facing
    bool head_ok{false};
    B::Vec3 eye{};     // the player's eye (world)
    B::Vec3 forward{}; // ... and view
    bool forced_in{false}; // the trigger held, the flick back, a recall pull
    float impact{0.0f};    // the chain's last landing (Chain::Impact, m/s)
    float cap{1.0f};       // the reach cap (extension)
};

// The Cutter Chain Lab's chain dials onto the Beast Cutter's config (whip_cutter_lab).
inline void ApplyLabChain(const CutterLab& lab, ChainConfig& c) {
    c.damp = lab.air;
    c.link_velocity_damp = lab.inner;
    c.tip_mass = lab.tipmass;
    c.swing_drive = lab.drive;
    c.drive_span = lab.dspan;
    c.drive_accel = lab.daccel;
    c.max_bend = lab.bend * B::Deg;
    c.constraint_iters = lab.iters;
    c.handle_links = lab.pinned;
    c.t_out = lab.tout;
    c.t_in = lab.tin;
    c.guide = lab.guide;
    c.guide_zeta = lab.gzeta;
    c.guide_taper = lab.gtaper;
    c.guide_w0 = lab.w0;
    c.guide_hzeta = lab.zeta;
    c.guide_drag = lab.rdrag;
    c.side_damp = 0.0f; // the lab's chain keeps its own swing
}

class CutterPath {
public:
    static constexpr float ChestDownM = 0.30f; // the arm's pivot: below the eye
    static constexpr float ChestBackM = 0.08f; // ... and behind it
    static constexpr float FeetUpM = 1.35f;    // without the head: above the hunter's feet
    static constexpr float SpeedS = 0.03f;     // the hand's speed smoothed over this (s)
    static constexpr float LockS = 0.3f;       // after a forced reel-in, no new throw for this (s)
    static constexpr float JumpMs = 40.0f;     // faster: a jump of the hand's frame, no swing
    static constexpr float LandMs = 2.0f;      // a landing this fast holds the momentum
    static constexpr float SpinS = 0.06f;      // the swing's circling smoothed over this (s)
    static constexpr float SpinBuildS = 0.12f; // ... and its lean built up over this (s)
    static constexpr float PickUpMs = 1.0f;    // the swing picking up again: this much over its low
    static constexpr float PickUpHoldS = 0.1f; // ... holds a reel-in this long after it
    static constexpr float FarM = 0.4f;        // the swing read this far out along the weapon (m)

    // One game frame: the extension asked for (1 = in); `Apply` then sets the chain's config.
    float Step(const PathFrame& in, const CutterLab& lab) {
        const float dt = std::isfinite(in.dt) ? std::clamp(in.dt, 0.0f, 0.25f) : 0.0f;
        // The swing's own movement around the hunter, read FarM out along the weapon (the arm's
        // swing and the wrist's: a helicopter overhead is mostly the wrist): from where that point
        // would be had it gone with his walk and his turn (a snap turn, the stick's) since the
        // last frame.
        const B::Vec3 point =
            B::IsFinite(in.weapon) && B::LengthSq(in.weapon) > 0.5f
                ? in.hand + B::Normalize(in.weapon) * FarM
                : in.hand;
        // His turn since the last frame. What was measured in the world turns with him: the swing's
        // motion and its lean, as well as its point (below). Kept as they were, a turn read as the
        // swing circling, and a snap turn mid-swing kinked the chain sideways for ~0.3 s (a review,
        // 2026-11-09). No turn: the identity, every value as before.
        const bool body = in.body_ok && body_ok_;
        float turn = 0.0f;
        if (body) {
            turn = std::atan2(facing_.z * in.facing.x - facing_.x * in.facing.z,
                              facing_.x * in.facing.x + facing_.z * in.facing.z);
            turn = std::isfinite(turn) ? turn : 0.0f;
            const B::Quat turned = B::AxisAngle({0.0f, 1.0f, 0.0f}, turn);
            vel_ = B::Rotate(turned, vel_);
            acc_ = B::Rotate(turned, acc_);
            spin_dir_ = B::Rotate(turned, spin_dir_);
        }
        float v = 0.0f;
        B::Vec3 vel{};
        if (have_ && dt > 0.0f && dt < 0.1f) {
            B::Vec3 was = hand_;
            if (body) {
                was = in.feet + B::Rotate(B::AxisAngle({0.0f, 1.0f, 0.0f}, turn), hand_ - feet_);
            }
            vel = (point - was) * (1.0f / dt);
            v = B::Length(vel);
            if (!std::isfinite(v) || v > JumpMs) {
                v = 0.0f;
                vel = {};
            }
        }
        hand_ = point;
        feet_ = in.feet;
        facing_ = in.facing;
        body_ok_ = in.body_ok && B::IsFinite(in.feet) && B::IsFinite(in.facing);
        have_ = B::IsFinite(point);
        if (dt > 0.0f) speed_ += (v - speed_) * (1.0f - std::exp(-dt / SpeedS));
        // How long the swing has not picked up again (its speed 1 m/s over its low since): the
        // reel-in quickens over this (pslow -> tin).
        if (speed_ > low_ + PickUpMs) {
            settle_ = 0.0f;
            low_ = speed_;
        } else {
            settle_ += dt;
            low_ = std::min(low_, speed_);
        }
        // Circling: the swing's acceleration across its motion (toward the centre of its circle),
        // smoothed; the chain leans out away from that centre as strongly as it circles.
        if (dt > 0.0f) {
            const B::Vec3 vel_was = vel_;
            vel_ = vel_ + (vel - vel_) * (1.0f - std::exp(-dt / SpeedS));
            acc_ = acc_ + ((vel_ - vel_was) * (1.0f / dt) - acc_) * (1.0f - std::exp(-dt / SpinS));
            const float sv = B::Length(vel_);
            B::Vec3 out{};
            float strength = 0.0f;
            if (sv > 0.3f && B::IsFinite(acc_)) {
                const B::Vec3 along = vel_ * (1.0f / sv);
                const B::Vec3 across = acc_ - along * B::Dot(along, acc_);
                const float a = B::Length(across);
                const float k = std::clamp(a / std::max(lab.pspinacc, 1.0f), 0.0f, 1.0f) *
                                std::clamp((sv - 0.3f) / 1.2f, 0.0f, 1.0f);
                if (a > 1e-3f) {
                    out = across * (-1.0f / a);
                    strength = k * k * (3.0f - 2.0f * k);
                }
            }
            // The direction follows at once (it turns with the circling); the strength builds up.
            if (B::LengthSq(out) > 0.5f) spin_dir_ = out;
            spin_w_ += (strength - spin_w_) * (1.0f - std::exp(-dt / SpinBuildS));
            spin_ = spin_dir_ * spin_w_;
        }
        forced_ = in.forced_in;
        if (in.forced_in) {
            momentum_ = 0.0f;
            lock_ = LockS;
            hit_ = 0.0f;
        } else {
            lock_ = std::max(0.0f, lock_ - dt);
            // A landing holds the momentum hithold, once a stroke (a chain bouncing on the
            // ground does not keep itself out).
            if (in.impact > LandMs && landing_) {
                hit_ = lab.hithold;
                landing_ = false;
            } else {
                hit_ = std::max(0.0f, hit_ - dt);
            }
            const float swing = lock_ > 0.0f ? 0.0f : speed_;
            // (The fade waits pturn of slowness first: a turn of the stroke keeps the length.)
            slow_ = swing >= momentum_ ? 0.0f : slow_ + dt;
            if (swing >= momentum_) {
                // (No more than full length takes: a big swing's excess would only hold it out
                // longer once it stops.)
                momentum_ = std::min(swing, std::max(lab.pout, lab.pin));
                landing_ = true;
            } else if (hit_ <= 0.0f && (slow_ > lab.pturn || lock_ > 0.0f)) {
                momentum_ = std::max(swing, momentum_ * std::exp2(-dt / std::max(lab.mhold, 0.02f)));
            }
        }
        // The pull: in full while the swing has momentum (a turn of the stroke keeps it), pkeep of
        // it once that is spent.
        const float swinging = std::clamp(momentum_ / std::max(lab.pout, 0.1f), 0.0f, 1.0f);
        share_ = lab.pkeep + (1.0f - lab.pkeep) * swinging;
        // The arm's pivot: the chest, below and behind the eye (else above the hunter's feet).
        pivot_ok_ = false;
        if (in.head_ok && B::IsFinite(in.eye) && B::IsFinite(in.forward)) {
            B::Vec3 flat{in.forward.x, 0.0f, in.forward.z};
            const float l = B::Length(flat);
            flat = l > 1e-3f ? flat * (1.0f / l) : B::Vec3{};
            pivot_ = in.eye - B::Vec3{0.0f, ChestDownM, 0.0f} - flat * ChestBackM;
            pivot_ok_ = true;
        } else if (in.body_ok && B::IsFinite(in.feet)) {
            pivot_ = in.feet + B::Vec3{0.0f, FeetUpM, 0.0f};
            pivot_ok_ = true;
        }
        // Out from pin, at least pmin of the full length at once (no pop-and-close just above the
        // start), and so until the momentum falls below 85% of pin; full from pout.
        if (in.forced_in || momentum_ < lab.pin * 0.85f) {
            out_ = false;
        } else if (momentum_ > lab.pin) {
            out_ = true;
        }
        const float f = std::clamp((momentum_ - lab.pin) / std::max(lab.pout - lab.pin, 0.1f),
                                   0.0f, 1.0f);
        const float least = std::clamp(lab.pmin, 0.0f, 1.0f);
        const float share = out_ ? least + (1.0f - least) * f * f * (3.0f - 2.0f * f) : 0.0f;
        return 1.0f + (std::max(in.cap, 1.0f) - 1.0f) * share;
    }

    // The chain's config for the lash (after the frame's Step): the path's pull, its pivot, the
    // physical chain's dials; no drive, no guide vector.
    void Apply(const CutterLab& lab, ChainConfig& c) const {
        c.swing_drive = 0.0f;
        c.guide = 0.0f;
        c.aim_share = 0.0f;
        c.path = lab.path;
        c.path_taper = lab.ptaper;
        c.path_zeta = lab.pzeta;
        c.path_delay = lab.pdelay;
        // pcurl is the curl you see (the chain's middle off the line from the hand to its tip):
        // with the lag growing evenly down the chain, a quarter of the tip's own lag.
        c.path_curl = std::min(4.0f * lab.pcurl, 170.0f) * B::Deg;
        c.path_share = share_;
        c.path_follow = lab.pfollow;
        c.path_rate = lab.prate * B::Deg;
        c.path_ground = lab.pground;
        c.pivot = pivot_;
        c.pivot_ok = pivot_ok_;
        c.spin_out = spin_ * std::clamp(lab.pspin, 0.0f, 1.0f);
        // The reel-in starts slow (pslow from full to closed) and speeds up while the swing stays
        // slow, to tin after paccel of it: a slowing swing or a turn only shortens it a little (the
        // next stroke pays it out again), a stop brings it in. A forced reel-in stays quick.
        // (A forced reel-in stays quick through its lockout: a flick back forces one frame only.)
        // The swing picking up again (to the start speed at least) stops a reel-in outright: the
        // next stroke pays it out again (the tester: "after I have started swinging it needs to
        // stop its reelback and go again").
        if (!forced_ && lock_ <= 0.0f && settle_ < PickUpHoldS && speed_ > lab.pin) {
            c.t_in = 1e3f;
        } else if (!forced_ && lock_ <= 0.0f && lab.pslow > c.t_in) {
            const float k = lab.paccel > 0.0f ? std::clamp(settle_ / lab.paccel, 0.0f, 1.0f) : 0.0f;
            c.t_in = lab.pslow + (c.t_in - lab.pslow) * k * k;
        }
        c.path_gravity = lab.pgrav;
    }

    void Reset() {
        *this = {};
    }

    float Momentum() const {
        return momentum_;
    }
    float HandSpeed() const {
        return speed_;
    }
    float Share() const {
        return share_;
    }
    B::Vec3 Spin() const {
        return spin_;
    }

private:
    B::Vec3 hand_{}, feet_{}, facing_{}; // (hand_: the swing's point, FarM out)
    bool have_{false}, body_ok_{false};
    float speed_{0.0f};    // the hand's speed around the hunter, smoothed (m/s)
    float momentum_{0.0f}; // the swing's momentum (m/s)
    float lock_{0.0f}, hit_{0.0f};
    bool landing_{false}; // the stroke's landing still to hold
    bool out_{false};     // paid out (pmin at least) since the momentum passed pin
    float slow_{0.0f};    // s the swing has been slower than its momentum
    bool forced_{false};  // this frame: the trigger or the flick back
    float settle_{0.0f};  // s since the swing last picked up again
    float low_{0.0f};     // its lowest speed since (m/s)
    B::Vec3 vel_{}, acc_{}; // the hand's velocity and acceleration around the hunter, smoothed
    B::Vec3 spin_{};        // out from the centre of the hand's circling (unit) x how strongly
    B::Vec3 spin_dir_{};
    float spin_w_{0.0f};
    float share_{1.0f};
    B::Vec3 pivot_{};
    bool pivot_ok_{false};
};

} // namespace VR::Whip
