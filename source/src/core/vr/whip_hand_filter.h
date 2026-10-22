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

// Release 0.3.6 compatibility filter for flexible whip forms.
// Keep the chain drive on its established hand trajectory; rigid weapons use the new filter.
// hand_tremor_filter (2026-10-18, the tester: "slight dampening to absorb the micro-frequencies of
// hand tremors", no added latency): a One Euro filter on each grip's pose, position and rotation,
// before the arms see it. Its cutoff rises with the hand's speed, taken from the runtime's own
// linear and angular velocity (no filtered derivative, so a swing opens it on its first frame):
// held still, `min_hz` damps tremor and tracking noise; moving, the cutoff climbs at `beta_*` Hz
// per m/s (rad/s) and the lag (1 / (2 pi cutoff)) is a few milliseconds at most. A jump, a lost
// track, a recentre or a gap starts it over on the raw pose. Pure (no OpenXR or emulator state)
// for tests/test_hand_tremor.cpp.
namespace VR::Body::WhipTremor {

struct Config {
    float min_hz{5.5f};      // the cutoff with the hand still
    float rot_min_hz{3.3f};  // ... for its rotation (feedback: a bit stronger, a long blade's tip)
    // How fast it opens with speed (2026-10-18: filter noise whenever we can, less the
    // faster you go; then a tiny amount less): 0.5 m/s 14.5 Hz (5 mm behind), 5 m/s 96 Hz;
    // 1 rad/s 12 Hz, 10 rad/s 90 Hz.
    float beta_pos{18.0f};   // Hz per m/s of the grip's speed
    float beta_rot{8.7f};    // Hz per rad/s of its turning
    float reset_m{0.20f};    // a jump this far between samples starts over (a teleport, a glitch)
    float reset_rad{0.6f};   // ... or this much of a turn
    float max_dt_s{0.1f};    // a longer gap starts over
};

class Filter {
public:
    // `t_ns`: the sample's time; `hand`: its pose (filtered in place) with the runtime's velocities
    // (without them, the raw pose's change since the last sample); `space`: the tracking space.
    void Step(std::int64_t t_ns, TrackedHand& hand, std::uint32_t space,
              const Config& c = Config{}) {
        if (!IsFinite(hand.p) || !IsFinite(hand.q)) {
            have_ = false; // never filter (or keep) a non-finite pose
            return;
        }
        const double dt = have_ ? static_cast<double>(t_ns - t_ns_) * 1e-9 : 0.0;
        if (!have_ || space != space_ || dt <= 0.0 || dt > c.max_dt_s ||
            Length(hand.p - p_) > c.reset_m || Turn(q_, hand.q) > c.reset_rad) {
            Reset(t_ns, hand.p, hand.q, space);
            return;
        }
        const float s = static_cast<float>(dt);
        float speed = hand.velocity_valid ? Length(hand.v) : Length(hand.p - raw_p_) / s;
        float turning = hand.angular_valid ? Length(hand.w) : Turn(raw_q_, hand.q) / s;
        if (!std::isfinite(speed) || !std::isfinite(turning)) {
            // A bad velocity: the pose passes as it is (no filtering this sample).
            speed = turning = 1e4f;
        }
        t_ns_ = t_ns;
        raw_p_ = hand.p;
        raw_q_ = hand.q;
        p_ = p_ + (hand.p - p_) * Alpha(c.min_hz + c.beta_pos * speed, s);
        q_ = Nlerp(q_, hand.q, Alpha(c.rot_min_hz + c.beta_rot * turning, s));
        hand.p = p_;
        hand.q = q_;
    }

    void Clear() {
        have_ = false;
    }

private:
    static float Alpha(float cutoff_hz, float dt) {
        const float tau = 1.0f / (6.2831853f * std::max(cutoff_hz, 1e-3f));
        return 1.0f / (1.0f + tau / dt);
    }

    // The turn between two rotations (radians).
    static float Turn(const Quat& a, const Quat& b) {
        const float d = std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
        return 2.0f * std::acos(std::min(d, 1.0f));
    }

    static Quat Nlerp(const Quat& a, Quat b, float k) {
        if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f) {
            b = {-b.x, -b.y, -b.z, -b.w};
        }
        return Normalize(Quat{a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k,
                              a.z + (b.z - a.z) * k, a.w + (b.w - a.w) * k});
    }

    void Reset(std::int64_t t_ns, const Vec3& p, const Quat& q, std::uint32_t space) {
        have_ = true;
        t_ns_ = t_ns;
        p_ = raw_p_ = p;
        q_ = raw_q_ = q;
        space_ = space;
    }

    bool have_{false};
    std::int64_t t_ns_{0};
    Vec3 p_{}, raw_p_{};
    Quat q_{}, raw_q_{};
    std::uint32_t space_{0};
};

} // namespace VR::Body::WhipTremor
