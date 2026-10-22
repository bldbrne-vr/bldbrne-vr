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

#include "core/vr/belt_vial.h"
#include "core/vr/body_frame.h"
namespace VR::ShoulderSwitch {
// Track the selected equipment slot, not the weapon type: identical copies still switch.
enum class Result { None, Changed, TimedOut };
// Held this long at the shoulder: unarmed (1.6 s until 2026-10-15, 1.2 s until 2026-10-16: the
// picks).
constexpr float UnarmHoldS = .72f;
// A hold under way keeps going while the hand stays within this much of the zone (metres; 3 cm
// until 2026-10-16: the tester, going unarmed felt much harder than a tap, the zone follows the
// head and any slip past it cancelled the hold) ...
constexpr float HoldMarginM = .10f;
// ... and a slip further out shorter than this does not cancel it (a tracking jitter, a turn).
constexpr float HoldGraceS = .15f;
// A press counts once the hand has been at the shoulder this long and this slow (0.08 s, 0.65 m/s
// and 3.5 rad/s until 2026-10-16: a reach and a press in one motion was often refused; 20 ms the
// user's pick).
constexpr float SettleS = .02f;
constexpr float SettleSpeed = 1.0f;       // m/s
constexpr float SettleAngularSpeed = 5.f; // rad/s
class Confirmation {
  public:
    void Reset() { *this = {}; }
    bool Pending() const { return pending_; }
    void Begin(std::int32_t slot, std::int64_t now, std::int32_t target = -1) {
        target_ = target;
        slot_ = slot;
        started_ = now;
        pending_ = true;
    }
    Result Update(bool gameplay, bool fresh, std::int32_t slot, std::int64_t now) {
        if (!pending_)
            return Result::None;
        if (!gameplay || now < started_) {
            Reset();
            return Result::None;
        }
        if (fresh && slot >= 0 && slot != slot_ && (target_ < 0 || slot == target_)) {
            Reset();
            return Result::Changed;
        }
        if (now - started_ >= 1'200'000'000) {
            Reset();
            return Result::TimedOut;
        }
        return Result::None;
    }

  private:
    bool pending_{};
    std::int32_t slot_{-1}, target_{-1};
    std::int64_t started_{};
};
struct Input {
    bool enabled{}, gameplay{}, tracked{}, b{}, available{}, attacking{}, chord{};
    Body::Vec3 head{}, forward{0, 0, -1}, hand{}, velocity{};
    float angular_speed{}, scale{1};
    bool left{}; // mirrored left-shoulder target, activated by physical Y
};
struct Output {
    bool request{}, consume{}, rejected{}, unarm{};
};
class Gate {
  public:
    void Reset() { *this = {}; }
    Output Update(const Input &in, float dt) {
        dt = std::isfinite(dt) ? std::clamp(dt, 0.f, .1f) : 0.f;
        const bool fresh = in.b && !down_;
        const bool released = !in.b && down_;
        down_ = in.b;
        Output out{};
        if (!in.enabled || !in.gameplay || !in.tracked) {
            pressed_ = false;
            held_ = 0;
            if (!in.b)
                owned_ = false;
            ready_ = false;
            calm_ = 0;
            torso_.Reset();
            out.consume = owned_ && in.b;
            return out;
        }
        torso_.Update({in.forward.x, in.forward.y, in.forward.z}, dt);
        const auto f = torso_.Forward();
        const Body::Vec3 forward{f.x, 0, f.z}, right{-f.z, 0, f.x};
        const float k = std::isfinite(in.scale) ? std::clamp(in.scale, .6f, 1.5f) : 1.f;
        const auto centre = in.head + right * ((in.left ? -.20f : .20f) * k) +
                            forward * (.03f * k) + Body::Vec3{0, -.20f * k, 0}; // 6 cm until 10-16
        // A capsule: the same front, stretched 45 cm back so a hand reaching past the shoulder
        // counts. 24 cm round (20 until 2026-10-14, 22 until 2026-10-15: the tester, "+2 cm" each
        // time). A hold already under way keeps going within HoldMarginM more.
        const float back = std::clamp(-Body::Dot(in.hand - centre, forward), 0.f, .45f * k);
        const float reach = Body::Length(in.hand - (centre - forward * back));
        const bool inside = reach < .24f * k;
        const bool still_inside = reach < (.24f + HoldMarginM) * k;
        // A deliberate reach must settle before the fresh button edge. Never queue a rejected
        // press.
        const bool calm = inside && Body::Length(in.velocity) < SettleSpeed &&
                          in.angular_speed < SettleAngularSpeed && !in.attacking && in.available;
        const bool was_ready = ready_;
        calm_ = calm ? calm_ + dt : 0;
        ready_ = calm_ >= SettleS;
        cooldown_ = std::max(0.f, cooldown_ - dt);
        if (fresh && inside && !in.chord) {
            owned_ = true;
            pressed_ = was_ready && calm && cooldown_ <= 0;
            held_ = 0;
            out_ = 0;
            hold_valid_ = pressed_;
            out.rejected = !pressed_;
        }
        // A short tap belongs to the shoulder where it began. Moving the hand
        // away on release must not swallow it. A long hold needs the hand to stay at the
        // shoulder, not still (it was settled for the press; the hand drifting or turning
        // a little cancelled it, 2026-10-15); combat, tracking and context guards cancel both.
        out_ = still_inside ? 0.f : out_ + dt;
        if (out_ > HoldGraceS || in.attacking || !in.available) hold_valid_ = false;
        if (pressed_ && (!in.available || in.attacking || in.chord))
            pressed_ = false;
        // Include the final held interval when the release is first observed.
        if (pressed_ && !fresh) {
            held_ += dt;
            if (held_ >= UnarmHoldS && hold_valid_) {
                out.request = out.unarm = true;
                pressed_ = false;
                cooldown_ = .35f;
            }
        }
        if (released) {
            out.request = out.request || (pressed_ && held_ < UnarmHoldS);
            if (pressed_)
                cooldown_ = .35f;
            pressed_ = owned_ = false;
            held_ = 0;
        }
        out.consume = owned_ && in.b;
        return out;
    }

  private:
    Belt::Torso torso_{};
    bool down_{true}, owned_{}, ready_{}, pressed_{}, hold_valid_{}; // startup held B cannot trigger
    float calm_{}, cooldown_{}, held_{}, out_{};
};
} // namespace VR::ShoulderSwitch
