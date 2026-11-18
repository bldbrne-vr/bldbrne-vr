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
// Held this long at the shoulder: unarmed (1.6 s until 2026-10-15, 1.2 s until 2026-10-16, 0.72 s
// until 2026-11-03, 0.60 s and 0.42 s until 2026-11-04: the user's picks; then 0.4).
constexpr float UnarmHoldS = .40f;
// A hold under way keeps going while the hand stays within this much of the zone (metres; 3 cm
// until 2026-10-16: the tester, going unarmed felt much harder than a tap, the zone follows the
// head and any slip past it cancelled the hold) ...
constexpr float HoldMarginM = .10f;
// A press this much outside the zone is a near miss (Output::near_miss): no switch, and the press
// stays the caller's (the quick-item fan still opens on it), but it never goes on to the game's
// own item use (2026-10-28: Y pressed a moment before the hand arrived went on as a
// quick-item tap and, where the fan cannot open, the game's Square used a quick item).
constexpr float NearMissM = .12f;
// ... and a slip further out shorter than this does not cancel it (a tracking jitter, a turn).
constexpr float HoldGraceS = .15f;
// A press at the shoulder counts however the hand arrives: a reach and a press in one motion, the
// hand still moving or turning (2026-11-10: "I do not want this case"; until then it had
// to sit 20 ms under 1 m/s and 5 rad/s, 80 ms, 0.65 m/s and 3.5 rad/s until 2026-10-16).
// One it cannot take yet (a swing still going out, the arm the game's for a moment, the last
// switch's short rest) is remembered this long and goes through the moment it can (the user,
// 2026-11-10: "remember it for 300ms"): a tap let go by then cycles at once, a press still held
// carries on as a hold. Another button pressed meanwhile, the hand untracked or the game's menus
// drop it.
constexpr float RememberS = .30f;
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
// A shoulder's zone, shared by the shoulder switch (the hand at its own shoulder + B / Y) and the
// shoulder vial (core/vr/holsters.h: the left hand over the right shoulder + grip; the user,
// 2026-10-26: the same shoulder position). From the eyes `head` along the torso's lazy heading
// `forward` (level, unit; Belt::Torso): a capsule from just in front of the shoulder (20 cm out to
// the side, 20 cm below the eyes, 3 cm ahead; 6 cm ahead until 2026-10-16) stretched 45 cm back so
// a hand reaching past the shoulder counts, RadiusM round (20 cm until 2026-10-14, 22 until
// 2026-10-15: the user, "+2 cm" each time); all times the body scale.
inline constexpr float RadiusM = .24f;
struct Zone {
    Body::Vec3 centre{}, forward{0, 0, -1};
    float k{1};
};
inline Zone ZoneAt(Body::Vec3 head, Body::Vec3 forward, float scale, bool left) {
    Zone z{};
    z.k = std::isfinite(scale) ? std::clamp(scale, .6f, 1.5f) : 1.f;
    z.forward = forward;
    const Body::Vec3 right{-forward.z, 0, forward.x};
    z.centre = head + right * ((left ? -.20f : .20f) * z.k) + forward * (.03f * z.k) +
               Body::Vec3{0, -.20f * z.k, 0};
    return z;
}
// The hand's distance from the zone's axis: inside below RadiusM * k.
inline float Reach(const Zone& z, Body::Vec3 hand) {
    const float back = std::clamp(-Body::Dot(hand - z.centre, z.forward), 0.f, .45f * z.k);
    return Body::Length(hand - (z.centre - z.forward * back));
}
struct Input {
    bool enabled{}, gameplay{}, tracked{}, b{}, available{}, attacking{}, chord{};
    Body::Vec3 head{}, forward{0, 0, -1}, hand{}, velocity{};
    float angular_speed{}, scale{1};
    bool left{}; // mirrored left-shoulder target, activated by physical Y
};
struct Output {
    bool request{}, consume{}, rejected{}, unarm{};
    // A fresh press NearMissM or less outside the zone, not the shoulder's (see NearMissM); set on
    // the press's first frame only (Output::fresh).
    bool near_miss{}, fresh{};
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
        out.fresh = fresh;
        if (!in.enabled || !in.gameplay || !in.tracked) {
            pressed_ = false;
            held_ = 0;
            if (!in.b)
                owned_ = false;
            remembered_ = 0;
            torso_.Reset();
            out.consume = owned_ && in.b;
            return out;
        }
        torso_.Update({in.forward.x, in.forward.y, in.forward.z}, dt);
        const auto f = torso_.Forward();
        // The shoulder's zone (ZoneAt); a hold already under way keeps going within HoldMarginM
        // more.
        const Zone zone = ZoneAt(in.head, {f.x, 0, f.z}, in.scale, in.left);
        const float reach = Reach(zone, in.hand);
        const bool inside = reach < RadiusM * zone.k;
        const bool still_inside = reach < (RadiusM + HoldMarginM) * zone.k;
        const bool close_by = reach < (RadiusM + NearMissM) * zone.k;
        // Free to switch now: no swing going out, the arm ours, the last switch's rest over (the
        // hand's speed and turn, its grip and its trigger never matter: RememberS).
        const bool free = !in.attacking && in.available;
        cooldown_ = std::max(0.f, cooldown_ - dt);
        out.near_miss = fresh && !inside && close_by && !in.chord;
        if (fresh && inside && !in.chord) {
            owned_ = true;
            pressed_ = free && cooldown_ <= 0;
            held_ = 0;
            out_ = 0;
            hold_valid_ = pressed_;
            // Not yet: remembered (RememberS), never dropped at once.
            remembered_ = pressed_ ? 0.f : RememberS;
            let_go_ = false;
        } else if (remembered_ > 0) {
            if (in.chord) {
                remembered_ = 0; // another button: another intent
                out.rejected = true;
            } else if (fresh) {
                // The same button pressed again away from the shoulder (B's dodge): that press is
                // the game's, and the waiting one is dropped, never a switch as well (review,
                // 2026-11-10).
                remembered_ = 0;
            } else if (free && cooldown_ <= 0) {
                remembered_ = 0;
                if (let_go_ || !in.b) {
                    out.request = true; // the tap, let go while it waited: the cycle now
                    cooldown_ = .35f;
                } else {
                    // Still held: on as a press, its wait counted toward the unarm hold (held_),
                    // the hand's time away from the shoulder meanwhile too (out_).
                    pressed_ = true;
                    hold_valid_ = out_ <= HoldGraceS;
                }
            } else {
                if (in.b)
                    held_ += dt; // (the wait counts toward the unarm hold)
                remembered_ -= dt;
                if (remembered_ <= 0) {
                    remembered_ = 0;
                    out.rejected = true; // waited RememberS: dropped (the soft tick)
                }
            }
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
            let_go_ = remembered_ > 0; // a remembered tap, let go: it cycles once it can
            pressed_ = owned_ = false;
            held_ = 0;
        }
        out.consume = owned_ && in.b;
        return out;
    }

  private:
    Belt::Torso torso_{};
    bool down_{true}, owned_{}, pressed_{}, hold_valid_{}; // startup held B cannot trigger
    bool let_go_{}; // the remembered press was let go (a tap)
    float cooldown_{}, held_{}, out_{};
    float remembered_{}; // s left for a press the switch could not take yet (RememberS)
};
} // namespace VR::ShoulderSwitch
