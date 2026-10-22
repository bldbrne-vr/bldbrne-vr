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

#include "core/vr/body_frame.h"

// Grip-pose smoothing before arm/weapon placement. Position adapts to runtime speed.
// Rotation separates error along the estimated turn direction from perpendicular wobble.
// Each component responds to its current error, restoring stabilization promptly on stops.
// Inspired by error-sensitive XR stabilization (Unity Labs Stabilizr); the directional
// decomposition and bounded residuals here are specific to this controller filter.
// Small deliberate corrections still converge; tracking discontinuities restart on raw.
namespace VR::Body::Tremor {

struct Config {
    float min_hz{6.5f};     // lighter position smoothing; rotation settings remain independent
    float rot_min_hz{3.3f}; // ... for its rotation (feedback: a bit stronger, a long blade's tip)
    float catchup_hz{8.0f}; // brief response after a fast turn
    float catchup_s{.04f}; // exponential transition into resting stabilization
    float settle_hz{2.5f}; // second-stage resting rotation stabilization; 0 = off
    float beta_pos{22.0f};  // Hz per m/s; reduce positional lag during deliberate movement
    float rot_velocity_hz{1.0f}; // estimates turn direction only; 0 = unfiltered
    float rot_range_rad{0.048869219f}; // 2.8 degree response range at the 90 Hz reference
    float rot_lag_rad{0.017453293f}; // soft 1 degree raw-to-filtered orientation limit; 0 = off
    float rot_lag_soft_rad{0.004363323f}; // ease the limit over +/-0.25 degrees
    float reset_m{0.20f};  // a jump this far between samples starts over (a teleport, a glitch)
    float reset_rad{0.6f}; // ... or this much of a turn
    float max_dt_s{0.1f};  // a longer gap starts over
};

class Filter {
public:
    // `t_ns`: the sample's time; `hand`: its pose (filtered in place) with the runtime's velocities
    // (without them, the raw pose's change since the last sample); `space`: the tracking space.
    void Step(std::int64_t t_ns, TrackedHand& hand, std::uint32_t space,
              const Config& c = Config{}) {
        if (!IsFinite(hand.p) || !IsFinite(hand.q) || Dot(hand.q, hand.q) < 1e-12f) {
            have_ = false; // never filter (or keep) a non-finite pose
            return;
        }
        hand.q = Normalize(hand.q);
        const double dt = have_ ? static_cast<double>(t_ns - t_ns_) * 1e-9 : 0.0;
        if (!have_ || space != space_ || dt <= 0.0 || dt > c.max_dt_s ||
            Length(hand.p - p_) > c.reset_m || Turn(q_, hand.q) > c.reset_rad) {
            Reset(t_ns, hand.p, hand.q, space);
            return;
        }
        const float s = static_cast<float>(dt);
        float speed = hand.velocity_valid ? Length(hand.v) : Length(hand.p - raw_p_) / s;
        Vec3 angular = hand.angular_valid && IsFinite(hand.w)
                           ? hand.w
                           : RotationDelta(raw_q_, hand.q) * (1.0f / s);
        if (!std::isfinite(speed))
            speed = 1e4f;
        const float velocity_alpha = c.rot_velocity_hz > 0.0f ? Alpha(c.rot_velocity_hz, s) : 1.0f;
        angular_ = angular_ + (angular - angular_) * velocity_alpha;
        t_ns_ = t_ns;
        raw_p_ = hand.p;
        raw_q_ = hand.q;
        p_ = p_ + (hand.p - p_) * Alpha(c.min_hz + c.beta_pos * speed, s);

        // Directional stabilization: a deliberate turn opens its own direction, rather
        // than passing perpendicular wrist wobble too. Adapt to current orientation
        // error, not a lingering speed magnitude, so stopping restores stabilization.
        const Quat previous_q = q_;
        const Vec3 error = RotationDelta(q_, hand.q);
        const float turning = Length(angular_);
        const Vec3 axis = turning > 1e-5f ? angular_ * (1.0f / turning) : Vec3{};
        const Vec3 along = axis * Dot(error, axis);
        const Vec3 across = error - along;
        const Vec3 correction = StabilizedStep(along, s, c) + StabilizedStep(across, s, c);
        const float angle = Length(correction);
        if (angle > 1e-8f)
            q_ = Normalize(AxisAngle(correction * (1.0f / angle), angle) * q_);
        // Retain the established fast-turn filter above. A separate resting stage
        // attenuates the slower wrist variation that becomes visible at a long tip.
        // It follows the first stage fully during a fast turn, so it adds no fast-turn
        // history. Use the first stage's angular speed so raw wrist noise cannot reopen
        // the settling stage. Briefly retain a higher cutoff after a fast turn: this
        // lets the remaining intentional turn converge before strong resting smoothing.
        float moving = std::clamp((Length(RotationDelta(previous_q,q_)) / s - 1.7453293f) / 1.7453293f, 0.0f, 1.0f);
        moving = moving * moving * (3.0f - 2.0f * moving);
        recent_turn_ = std::max(moving, recent_turn_ * std::exp(-s / std::max(c.catchup_s,.001f)));
        const float cutoff = c.settle_hz + (c.catchup_hz-c.settle_hz)*recent_turn_;
        const float resting = c.settle_hz > 0.0f ? Alpha(cutoff, s) : 1.0f;
        output_q_ = Nlerp(output_q_, q_, resting + moving * (1.0f - resting));
        // Bound the COMPLETE result, including both stages, against the raw pose.
        if (c.settle_hz > 0.0f && c.rot_lag_rad > 0.0f) {
            const Vec3 remaining = RotationDelta(output_q_, raw_q_);
            const float length = Length(remaining);
            if (length > 1e-8f) {
                const float excess = SoftExcess(length, c.rot_lag_rad,
                                               std::min(c.rot_lag_soft_rad,c.rot_lag_rad));
                output_q_ = Normalize(AxisAngle(remaining * (1.0f/length),excess) * output_q_);
            }
        }
        hand.p = p_;
        hand.q = output_q_;
    }

    void Clear() {
        have_ = false;
    }

private:
    static Vec3 StabilizedStep(const Vec3& error, float dt, const Config& c) {
        const float angle = Length(error);
        if (angle < 1e-8f)
            return error;
        // Error-sensitive blending follows the principle used by Unity Labs Stabilizr.
        // A positive minimum response avoids a dead zone for tiny intentional corrections.
        const float range = std::max(c.rot_range_rad, 1e-5f);
        const float reference = std::clamp(angle / range, 0.0f, 1.0f);
        float alpha = std::max(Alpha(c.rot_min_hz, dt),
                               1.0f - std::pow(1.0f - reference, dt * 90.0f));
        // Two orthogonal residuals share the old total angular-lag budget.
        if (c.rot_lag_rad > 0.0f) {
            const float limit = c.rot_lag_rad * 0.70710678f;
            const float soft = std::min(c.rot_lag_soft_rad * 0.70710678f, limit);
            const float residual = angle * (1.0f - alpha);
            alpha += SoftExcess(residual, limit, soft) / angle;
        }
        return error * std::clamp(alpha, 0.0f, 1.0f);
    }

    static float Alpha(float cutoff_hz, float dt) {
        const float tau = 1.0f / (6.2831853f * std::max(cutoff_hz, 1e-3f));
        return 1.0f / (1.0f + tau / dt);
    }

    // Shortest signed rotation vector in tracking space. atan2 preserves small rotations
    // that acos(abs(dot)) rounds to zero; q and -q produce the same result.
    static Vec3 RotationDelta(const Quat& a, const Quat& b) {
        Quat d = Normalize(b * Conjugate(a));
        if (d.w < 0.0f)
            d = {-d.x, -d.y, -d.z, -d.w};
        const Vec3 v{d.x, d.y, d.z};
        const float length = Length(v);
        return v * (length > 1e-8f ? 2.0f * std::atan2(length, d.w) / length : 2.0f);
    }

    static float Turn(const Quat& a, const Quat& b) {
        return Length(RotationDelta(a, b));
    }

    static Quat Nlerp(const Quat& a, Quat b, float k) {
        if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f) {
            b = {-b.x, -b.y, -b.z, -b.w};
        }
        return Normalize(Quat{a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k, a.z + (b.z - a.z) * k,
                              a.w + (b.w - a.w) * k});
    }

    void Reset(std::int64_t t_ns, const Vec3& p, const Quat& q, std::uint32_t space) {
        have_ = true;
        t_ns_ = t_ns;
        p_ = raw_p_ = p;
        q_ = raw_q_ = output_q_ = q;
        space_ = space;
        angular_ = {};
        recent_turn_ = 0;
    }

    float recent_turn_{};
    bool have_{false};
    std::int64_t t_ns_{0};
    Vec3 p_{}, raw_p_{};
    Vec3 angular_{};
    Quat q_{}, raw_q_{}, output_q_{};
    std::uint32_t space_{0};
};

} // namespace VR::Body::Tremor
