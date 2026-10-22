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
#include <initializer_list>

// The belt vial (left_scheme=2, the tester's 2026-10-05 request): squeezing the left grip with the
// hand at the left hip draws a Blood Vial from the hunter's belt, exactly as X arms one (then stab
// to inject as before). Anywhere else the grip opens the quick-item arc (core/vr/quick_items.h);
// a squeeze at the hip never does.
//
// Where the hip is: a box beside the left hip and thigh in the torso's frame, measured from the
// body's vertical axis (the eyes are ~10 cm in front of it) and down from the eyes, standing or
// (seated=1) beside the lap. The torso's heading is the view's yaw followed lazily (Torso): a
// glance down at the hip turns the head 40-60 degrees toward it and must not turn the zone away
// from the hand.
//
// Pure (no OpenXR or emulator state): the presenter (renderer_vulkan/openxr_presenter.cpp) feeds
// it the tracked head and left grip every input frame; tests/test_quick_items_left.cpp.
namespace VR::Belt {

struct P3 {
    float x{0.0f}, y{0.0f}, z{0.0f};
};

// The box, metres. `back_m`: the body's axis behind the eyes along the torso's forward; `ahead`
// along that forward from the axis, `out` to the left of it, `below` below the eyes.
struct Zone {
    float back_m{0.10f};
    float ahead_min{-0.25f}, ahead_max{0.18f};
    float out_min{0.13f}, out_max{0.45f};
    float below_min{0.45f}, below_max{1.05f};
};

// Standing: from the belt (an adult's hip bone is ~0.55-0.65 m below the eyes) down to mid-thigh,
// beside it from the hip's side a few cm out to an arm hanging well away, from behind the hip to
// the front of the thigh. A hand held in front of the belly or the chest (the arc's place, the
// stab zones), at the right hip or above the belt is outside; one hanging at the left side is in.
// From 21 cm out (2026-10-05 review; it was 13): clear of the torso's stab zone (18 cm round a
// line 5 cm in front of the eyes), so a vial drawn there never starts inside it (with 13 cm, 4%
// of the box was inside it and a small move toward the body injected).
inline constexpr Zone Standing{0.10f, -0.25f, 0.18f, 0.21f, 0.45f, 0.45f, 1.05f};
// Seated: beside the lap (the thighs ~0.6 m below the eyes reach ~45 cm forward), clear of the lap
// itself (the lap's stab zone reaches 18 cm out from the middle).
inline constexpr Zone Seated{0.05f, -0.20f, 0.45f, 0.20f, 0.50f, 0.38f, 0.85f};

// `scale` (belt_vial_zone) grows or shrinks each range about its middle.
inline Zone Scaled(const Zone& z, float scale) {
    const float s = std::clamp(scale, 0.5f, 2.0f);
    const auto range = [s](float& lo, float& hi) {
        const float mid = 0.5f * (lo + hi);
        const float half = 0.5f * (hi - lo) * s;
        lo = mid - half;
        hi = mid + half;
    };
    Zone out = z;
    range(out.ahead_min, out.ahead_max);
    range(out.out_min, out.out_max);
    range(out.below_min, out.below_max);
    // Never closer to the body than the box itself (the stab zones) nor the 30 cm below the eyes:
    // a bigger box grows outward.
    out.out_min = std::max(out.out_min, z.out_min);
    out.below_min = std::max(out.below_min, 0.30f);
    return out;
}

// Your height (core/vr/player_body.h): every distance of the box k times (k = height / 167 cm), so
// the hip is where a taller or shorter body has it. k = 1: the box as it is.
inline Zone BodyScaled(const Zone& z, float k) {
    if (k == 1.0f || !std::isfinite(k) || k <= 0.0f) {
        return z;
    }
    Zone out = z;
    for (float* f : {&out.back_m, &out.ahead_min, &out.ahead_max, &out.out_min, &out.out_max,
                     &out.below_min, &out.below_max}) {
        *f *= k;
    }
    return out;
}

struct Place {
    float ahead{0.0f}; // m along the torso's forward from the body's axis
    float out{0.0f};   // m to the left of it (negative: the right)
    float below{0.0f}; // m below the eyes
    bool inside{false};
};

// The left grip `hand` against the zone for the eyes at `head` and the torso's horizontal forward
// `forward` (unit, LOCAL: +Y up; the right is (-f.z, 0, f.x)).
inline Place Locate(P3 head, P3 hand, P3 forward, const Zone& z) {
    const float flat = std::hypot(forward.x, forward.z);
    const P3 f = flat > 1e-4f ? P3{forward.x / flat, 0.0f, forward.z / flat} : P3{0.0f, 0.0f, -1.0f};
    const P3 left{f.z, 0.0f, -f.x};
    const P3 axis{head.x - f.x * z.back_m, head.y, head.z - f.z * z.back_m};
    const P3 d{hand.x - axis.x, hand.y - axis.y, hand.z - axis.z};
    Place p{};
    p.ahead = d.x * f.x + d.z * f.z;
    p.out = d.x * left.x + d.z * left.z;
    p.below = -d.y;
    p.inside = p.ahead >= z.ahead_min && p.ahead <= z.ahead_max && p.out >= z.out_min &&
               p.out <= z.out_max && p.below >= z.below_min && p.below <= z.below_max;
    return p;
}

// The torso's heading: the view's yaw, followed lazily. Never more than Deadzone from it (a real
// turn of the body turns it at once beyond that); while the view looks roughly ahead (less than
// LevelDeg down) it settles toward it over Tau; looking down (at the belt, the floor) it holds,
// so a glance at the hip leaves the zone where the hip is.
class Torso {
public:
    static constexpr float DeadzoneDeg = 40.0f;
    static constexpr float LevelDeg = 25.0f;
    static constexpr float Tau = 1.0f; // s

    // `view`: the head's forward (LOCAL, any length; looking almost straight up or down keeps the
    // last heading), `dt` seconds since the last update.
    void Update(P3 view, float dt) {
        constexpr float Rad = 3.14159265f / 180.0f;
        const float flat = std::hypot(view.x, view.z);
        const float length = std::sqrt(flat * flat + view.y * view.y);
        if (length < 1e-4f || flat < 0.2f * length) {
            return;
        }
        const float head = std::atan2(view.x, view.z);
        if (!valid_) {
            yaw_ = head;
            valid_ = true;
            return;
        }
        float d = Wrap(head - yaw_);
        const float dead = DeadzoneDeg * Rad;
        if (std::abs(d) > dead) {
            const float over = d - std::copysign(dead, d);
            yaw_ = Wrap(yaw_ + over);
            d -= over;
        }
        const float pitch = std::atan2(view.y, flat);
        if (pitch > -LevelDeg * Rad && dt > 0.0f) {
            yaw_ = Wrap(yaw_ + d * (1.0f - std::exp(-std::min(dt, 0.5f) / Tau)));
        }
    }
    void Reset() {
        valid_ = false;
    }
    [[nodiscard]] bool Valid() const {
        return valid_;
    }
    [[nodiscard]] P3 Forward() const {
        return {std::sin(yaw_), 0.0f, std::cos(yaw_)};
    }

private:
    static float Wrap(float a) {
        constexpr float Pi = 3.14159265f;
        a = std::fmod(a + Pi, 2.0f * Pi);
        if (a < 0.0f) {
            a += 2.0f * Pi;
        }
        return a - Pi;
    }
    float yaw_{0.0f};
    bool valid_{false};
};

} // namespace VR::Belt
