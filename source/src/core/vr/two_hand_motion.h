// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include "core/vr/two_hand_grip.h"

namespace VR::TwoHand {

// Strictly opt in: separate dual weapons and a shared weapon used one-handed keep the
// original controller-based combat path. Native form alone does not attach a second hand.
inline bool SharedMotion(int family, int style, bool seated) {
    return seated && family > 0 && TwoHandedForm(family, style);
}

// The two points combat samples on the steered weapon, along its blade from the right fist:
// - the tip at the one-handed estimate's 80 cm (damage speed, the swing hum, the window's tip
//   speed): the same scale for every weapon, one-handed or two (SpeedFactor is tuned on it);
// - a lever point 30 cm out for the swing classifier and the window's hand speed: its thresholds
//   (2 m/s, 15 cm of travel) were tuned on the hand. A push-pull chop with the right hand still
//   (10 rad/s) moves it 3 m/s; a 3 rad/s wrist flick only 0.9 m/s, so it stays no attack.
// The hand's own position (the weapon pose) stays the wound-up pose test's point.
inline constexpr float BladeTipM = 0.8f;
inline constexpr float BladeLeverM = 0.30f;

// A point `m` metres along the unit blade direction `dir` from the bone origin `origin` (both in
// the right controller's grip frame).
inline Vec3 AlongBlade(Vec3 origin, Vec3 dir, float m) {
    return origin + dir * m;
}

struct BladeSample {
    bool active{}, valid{};
    std::int64_t ns{};
    Pose weapon{};
    Vec3 tip{}, velocity{}, relative_velocity{}, head{};
    Vec3 head_forward{0, 0, -1};
    float speed{};
    // The lever point (BladeLeverM).
    Vec3 lever{}, lever_velocity{}, lever_relative_velocity{};
    float lever_speed{};
};

// Differentiate the same final XR pose published to the body, at predicted display time.
// Tracking space deliberately excludes artificial locomotion/snap turning; head translation is
// removed from the speeds. A gap, a change of point or a jump gives an invalid frame (no speed),
// never a fabricated one.
class BladeMotion {
public:
    BladeSample Step(bool active, bool tracked, std::int64_t ns, std::uint64_t space,
                     int family, int style, const Pose& weapon, Vec3 local_tip,
                     Vec3 local_lever, Vec3 head) {
        BladeSample out{};
        out.active = active;
        out.ns = ns;
        out.weapon = weapon;
        out.head = head;
        if (!active || !tracked || !Body::IsFinite(weapon.p) || !Body::IsFinite(weapon.q) ||
            !Body::IsFinite(local_tip) || !Body::IsFinite(local_lever) || !Body::IsFinite(head)) {
            Reset();
            return out;
        }
        const Quat q = Body::Normalize(weapon.q);
        out.tip = weapon.p + Body::Rotate(q, local_tip);
        out.lever = weapon.p + Body::Rotate(q, local_lever);
        const double dt = static_cast<double>(ns - ns_) * 1e-9;
        const bool same = have_ && space == space_ && family == family_ && style == style_ &&
                          dt >= 0.001 && dt <= 0.05 &&
                          Body::Length(local_tip - local_tip_) < 0.002f &&
                          Body::Length(local_lever - local_lever_) < 0.002f &&
                          Body::AngleBetween(weapon.q, weapon_.q) < 60.0f * Body::Deg &&
                          Body::Length(head - head_) < 0.3f;
        if (same) {
            const float inv = static_cast<float>(1.0 / dt);
            out.velocity = (out.tip - tip_) * inv;
            out.relative_velocity = ((out.tip - head) - (tip_ - head_)) * inv;
            out.speed = Body::Length(out.relative_velocity);
            out.lever_velocity = (out.lever - lever_) * inv;
            out.lever_relative_velocity = ((out.lever - head) - (lever_ - head_)) * inv;
            out.lever_speed = Body::Length(out.lever_relative_velocity);
            out.valid = Body::IsFinite(out.velocity) && Body::IsFinite(out.relative_velocity) &&
                        Body::Length(out.velocity) < 40.0f && out.speed < 40.0f &&
                        Body::IsFinite(out.lever_velocity) &&
                        Body::IsFinite(out.lever_relative_velocity) && out.lever_speed < 40.0f;
        }
        if (!out.valid) {
            out.velocity = out.relative_velocity = {};
            out.lever_velocity = out.lever_relative_velocity = {};
            out.speed = out.lever_speed = 0.0f;
        }
        have_ = true;
        ns_ = ns;
        space_ = space;
        family_ = family;
        style_ = style;
        weapon_ = weapon;
        tip_ = out.tip;
        lever_ = out.lever;
        local_tip_ = local_tip;
        local_lever_ = local_lever;
        head_ = head;
        return out;
    }
    void Reset() { *this = {}; }
private:
    bool have_{};
    std::int64_t ns_{};
    std::uint64_t space_{};
    int family_{-1}, style_{-1};
    Pose weapon_{};
    Vec3 tip_{}, lever_{}, local_tip_{}, local_lever_{}, head_{};
};

class SwingHum {
public:
    float Step(bool enabled, float speed, float dt, float gain) {
        if (!enabled || !std::isfinite(speed) || !std::isfinite(dt) || dt <= 0 ||
            !std::isfinite(gain) || gain <= 0) return level_ = 0;
        const float want = 0.08f * std::clamp(gain, 0.0f, 1.0f) *
                           Body::Smoothstep(3.0f, 10.0f, speed);
        const float tau = want > level_ ? 0.025f : 0.06f;
        level_ += (want - level_) * (1.0f - std::exp(-std::min(dt, 0.05f) / tau));
        if (level_ < 0.001f) level_ = 0;
        return level_;
    }
private:
    float level_{};
};

} // namespace VR::TwoHand
