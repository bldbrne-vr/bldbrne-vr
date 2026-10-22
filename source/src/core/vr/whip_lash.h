// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include "core/vr/arm_ik.h"

namespace VR::Whip {
// One bounded pickup per deliberate stroke, including a backhand with the chain still out.
// A velocity reversal or a short pause arms it; sustained motion and small tremors do not.
// The pulse travels down the proximal chain, then releases it to carry the lash to the tip.
class LashDrive {
public:
    void Reset() {
        *this = {};
    }
    void Carry(VR::Body::Quat q) {
        direction_ = VR::Body::Rotate(q, direction_);
    }
    void Step(VR::Body::Vec3 velocity, float dt, bool enabled, bool closed_launch) {
        namespace B = VR::Body;
        if (!enabled) {
            Reset();
            return;
        }
        age_ = std::min(age_ + dt, 10.0f);
        const float speed = B::Length(velocity);
        if (closed_launch) {
            age_ = dt;
            closed_ = true;
            armed_ = false;
            travel_ = fast_ = 0;
        }
        if (speed > .75f) {
            const auto direction = velocity * (1.0f / speed);
            if (B::LengthSq(direction_) > .5f && B::Dot(direction, direction_) < -.25f) {
                armed_ = true;
                travel_ = fast_ = 0;
            }
            direction_ = direction;
            quiet_ = 0;
        } else {
            quiet_ += dt;
            if (quiet_ >= .035f)
                armed_ = true;
        }
        // A 240 ms separation and at least 20 ms / 4 cm of committed motion prevent
        // a single jittery sample or the first throw's own deceleration from repumping it.
        if (armed_ && age_ >= .24f && speed >= 1.5f) {
            travel_ += speed * dt;
            fast_ += dt;
            if (travel_ >= .04f && fast_ >= .020f) {
                age_ = 0;
                closed_ = false;
                armed_ = false;
                travel_ = fast_ = 0;
            }
        } else {
            travel_ = fast_ = 0;
        }
    }
    float Strength() const {
        return closed_ ? 1.0f : 1.8f;
    }
    float Weight(float u) const {
        // Preserve the established first throw. Follow-up pickup releases the root first,
        // with no direct drive at the tip: its speed comes from tension and its tapered mass.
        if (closed_)
            return std::clamp((.32f - age_) / .06f, 0.0f, 1.0f);
        const float t = age_ - .06f * u;
        if (t <= 0 || t >= .26f)
            return 0;
        const float attack = std::clamp(t / .025f, 0.0f, 1.0f);
        const float release = std::clamp((.26f - t) / .06f, 0.0f, 1.0f);
        return (1 - u * u * u * u) * attack * attack * (3 - 2 * attack) * release * release *
               (3 - 2 * release);
    }

private:
    VR::Body::Vec3 direction_{};
    float age_{10}, quiet_{0}, travel_{0}, fast_{0};
    bool armed_{false}, closed_{false};
};
} // namespace VR::Whip
