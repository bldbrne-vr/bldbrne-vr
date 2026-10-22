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

// VR body: the hunter's facing (vr.ini body_facing), pure (no emulator state) so the unit tests
// (tests/test_body_facing.cpp) can drive it. The VR rule: the hunter's heading changes only with
// the turn stick (snap or smooth turns) and with your own real heading, never with locomotion.
// Walking, sprinting, rolling or stepping in any direction moves him relative to that heading
// (core/vr/bloodborne_facing.cpp makes the game strafe instead of turning him round).
//
// The heading follows the view's yaw lazily: glancing sideways leaves him as he is until the view
// is more than `deadzone` from him (`moving_deadzone` while the left stick moves him, so he walks
// where you look), then he turns toward the view (time constant `tau`) until he is within `settle`
// of it. An artificial turn (snap or smooth: the game heading of tracking forward and the view yaw
// change together) turns him by exactly as much at once; a recenter (the heading of tracking
// forward changes, the view does not) leaves him. Angles are game yaw (radians, + = turned right),
// times seconds.
namespace VR::Body {

struct FacingParams {
    float deadzone{0.6108652f};        // 35 deg
    float moving_deadzone{0.2617994f}; // 15 deg
    float tau{0.25f};
    float settle{0.0349066f}; // 2 deg
};

inline float FacingWrap(float a) {
    constexpr float Pi = 3.14159265358979f;
    a = std::fmod(a + Pi, 2.0f * Pi);
    if (a < 0.0f) {
        a += 2.0f * Pi;
    }
    return a - Pi;
}

// The view of one camera tick: the game heading of tracking forward (`base`, the snap / smooth
// turns and recenters) and the view's yaw (base - the head's yaw).
struct FacingView {
    float base{0.0f};
    float view{0.0f};
};

class FacingTracker {
public:
    // Starts over at the hunter's current heading.
    void Reset(float chr_yaw) {
        have_ = true;
        have_view_ = false;
        facing_ = FacingWrap(chr_yaw);
        catching_ = false;
    }

    [[nodiscard]] bool Valid() const {
        return have_;
    }
    [[nodiscard]] float Facing() const {
        return facing_;
    }
    [[nodiscard]] bool Catching() const {
        return catching_;
    }
    // The artificial turns taken so far (logs, tests).
    [[nodiscard]] float Turned() const {
        return turned_;
    }

    // One update with this tick's view; returns the heading he should have now.
    float Step(const FacingView& v, bool moving, float dt, const FacingParams& p) {
        if (!have_) {
            Reset(v.view);
        }
        if (have_view_) {
            const float d_base = FacingWrap(v.base - last_.base);
            const float d_view = FacingWrap(v.view - last_.view);
            // A turn moves the view with the heading of tracking forward; a recenter moves only the
            // heading (the view keeps its direction), a head turn only the view.
            if (std::abs(d_base) > 1e-4f && std::abs(d_view - d_base) <= 0.25f * std::abs(d_base)) {
                facing_ = FacingWrap(facing_ + d_base);
                turned_ += d_base;
            }
        }
        last_ = v;
        have_view_ = true;
        const float error = FacingWrap(v.view - facing_);
        const float dead = moving ? std::min(p.moving_deadzone, p.deadzone) : p.deadzone;
        if (std::abs(error) > dead) {
            catching_ = true;
        } else if (std::abs(error) <= p.settle) {
            catching_ = false;
        }
        if (catching_) {
            const float k = p.tau > 0.0f ? 1.0f - std::exp(-std::max(dt, 0.0f) / p.tau) : 1.0f;
            float step = error * k;
            // Never left further behind than the deadzone (a fast head turn drags him along).
            if (std::abs(error - step) > dead) {
                step = error - std::copysign(dead, error);
            }
            facing_ = FacingWrap(facing_ + step);
            if (std::abs(FacingWrap(v.view - facing_)) <= p.settle) {
                catching_ = false;
            }
        }
        return facing_;
    }

private:
    bool have_{false};
    bool have_view_{false};
    bool catching_{false};
    float facing_{0.0f};
    float turned_{0.0f};
    FacingView last_{};
};

// The game's lock point stand-in that turns him to `yaw`: `distance` ahead of his position along
// the heading, at `height` above it (game units). Game axes: forward = (sin yaw, 0, cos yaw).
struct FacingPoint {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};
inline FacingPoint FacingAim(float x, float y, float z, float yaw, float distance, float height) {
    return {x + std::sin(yaw) * distance, y + height, z + std::cos(yaw) * distance};
}

} // namespace VR::Body
