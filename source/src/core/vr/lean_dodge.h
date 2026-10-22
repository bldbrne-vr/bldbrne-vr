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
#include <cstddef>

// Lean to dodge (experimental, standing play, off by default: vr.ini lean_dodge=1). A quick
// sideways or backward lean or step of the head is a dodge: the headset's horizontal speed at
// least `speed` for `hold_s`, at least `dist_m` of travel within `window_s`, while the head is
// not turning (yaw rate below `max_yaw_rate`), the right stick is centred, no swing is in progress
// and the dodge button is not held. Forward leans never count. The caller sends the left stick in
// the lean direction (relative to the current view) and one Circle press a game frame later.
// One dodge per lean; then `cooldown_s`.
//
// Pure (no OpenXR or emulator dependencies) so unit tests can drive it
// (tests/test_lean_dodge.cpp). Conventions: OpenXR LOCAL space (+x right, +y up, +z back),
// meters, radians, seconds; the stick is +x right, +y forward.
namespace VR::Lean {

struct Config {
    float speed{1.3f};           // m/s, headset horizontal speed ...
    float hold_s{0.080f};        // ... held this long
    float dist_m{0.15f};         // horizontal travel ...
    float window_s{0.250f};      // ... within this window
    float max_yaw_rate{1.5708f}; // rad/s (90 deg/s): a turning head is not a lean
    float cooldown_s{0.6f};
    float forward_deg{50.0f}; // leans within this angle of the view's forward are ignored
    float stick_dead{0.3f};   // the right stick counts as centred below this
};

struct Input {
    double t{};          // seconds
    bool enabled{false}; // lean_dodge on, standing, gameplay shown in VR
    bool head_ok{false}; // headset position and orientation tracked
    float x{}, z{};      // headset position (LOCAL)
    bool velocity_ok{false};
    float vx{}, vz{};            // headset linear velocity (LOCAL)
    float fwd_x{}, fwd_z{-1.0f}; // headset forward (LOCAL, not normalized)
    float look_x{}, look_y{};    // right stick
    bool swing{false};           // a swing is in progress (weapon hand)
    bool circle_held{false};     // the dodge / sprint button is held by hand
};

struct Output {
    bool fire{false};
    float stick_x{}, stick_y{}; // fire: unit stick direction (+y = view forward)
    // A lean that reached the speed for `hold_s` but ended without a dodge: the last gate that
    // refused it ("distance", "yaw", "stick", "swing", "circle", "cooldown", "forward"; "" none).
    const char* rejected{""};
    // Measurements of the fire or the rejection (logs).
    float speed{};     // m/s
    float dist{};      // m
    float yaw_rate{};  // rad/s
    float angle_deg{}; // lean direction from the view's forward, + = right
};

class Detector {
public:
    static constexpr size_t Capacity = 128;
    static constexpr float YawTau = 0.030f; // yaw-rate smoothing

    Output Update(const Input& in, const Config& cfg) {
        Output out{};
        if (!in.enabled || !in.head_ok) {
            EndLean(out);
            count = 0;
            have_yaw = false;
            return out;
        }
        // Yaw rate (finite difference of the forward's heading, smoothed).
        const float yaw = std::atan2(-in.fwd_x, -in.fwd_z);
        if (have_yaw && in.t > last_t) {
            const float dt = static_cast<float>(in.t - last_t);
            float d = yaw - last_yaw;
            constexpr float Pi = 3.14159265358979f;
            while (d > Pi) {
                d -= 2.0f * Pi;
            }
            while (d < -Pi) {
                d += 2.0f * Pi;
            }
            yaw_rate += (std::abs(d) / dt - yaw_rate) * (1.0f - std::exp(-dt / YawTau));
        }
        have_yaw = true;
        last_yaw = yaw;
        last_t = in.t;

        // Position history for the travel window.
        if (count == Capacity) {
            std::move(samples.begin() + 1, samples.end(), samples.begin());
            --count;
        }
        samples[count++] = {in.t, in.x, in.z};
        size_t drop = 0;
        while (drop + 1 < count && in.t - samples[drop].t > cfg.window_s + 0.05) {
            ++drop;
        }
        if (drop > 0) {
            std::move(samples.begin() + drop, samples.begin() + count, samples.begin());
            count -= drop;
        }

        float speed = 0.0f;
        if (in.velocity_ok) {
            speed = std::hypot(in.vx, in.vz);
        } else if (count >= 2 && samples[count - 1].t > samples[count - 2].t) {
            const Sample& a = samples[count - 2];
            const Sample& b = samples[count - 1];
            speed = std::hypot(b.x - a.x, b.z - a.z) / static_cast<float>(b.t - a.t);
        }
        if (speed < cfg.speed) {
            EndLean(out);
            return out;
        }
        if (fast_since < 0.0) {
            fast_since = in.t;
        }
        if (fired || in.t - fast_since < cfg.hold_s) {
            return out;
        }

        // Travel: the largest displacement from a sample within the window, and its direction.
        float dist = 0.0f, dx = 0.0f, dz = 0.0f;
        for (size_t i = 0; i < count; ++i) {
            if (in.t - samples[i].t > cfg.window_s) {
                continue;
            }
            const float ex = in.x - samples[i].x;
            const float ez = in.z - samples[i].z;
            const float d = std::hypot(ex, ez);
            if (d > dist) {
                dist = d;
                dx = ex;
                dz = ez;
            }
        }
        // The lean direction in the view's frame: forward = the headset's flattened forward,
        // right = forward x up.
        const float fl = std::hypot(in.fwd_x, in.fwd_z);
        const float fx = fl > 1e-4f ? in.fwd_x / fl : 0.0f;
        const float fz = fl > 1e-4f ? in.fwd_z / fl : -1.0f;
        const float sx = dist > 0.0f ? (dx * -fz + dz * fx) / dist : 0.0f;
        const float sy = dist > 0.0f ? (dx * fx + dz * fz) / dist : 0.0f;
        const float angle = std::atan2(sx, sy) * 57.2957795f;
        out.speed = speed;
        out.dist = dist;
        out.yaw_rate = yaw_rate;
        out.angle_deg = angle;
        candidate = out;

        const char* refused = nullptr;
        if (dist < cfg.dist_m) {
            refused = "distance";
        } else if (yaw_rate >= cfg.max_yaw_rate) {
            refused = "yaw";
        } else if (std::abs(in.look_x) >= cfg.stick_dead || std::abs(in.look_y) >= cfg.stick_dead) {
            refused = "stick";
        } else if (in.swing) {
            refused = "swing";
        } else if (in.circle_held) {
            refused = "circle";
        } else if (in.t - last_fire < cfg.cooldown_s) {
            refused = "cooldown";
        } else if (std::abs(angle) < cfg.forward_deg) {
            refused = "forward";
        }
        if (refused != nullptr) {
            rejected = refused; // still leaning: a later frame may pass
            return out;
        }
        fired = true;
        rejected = nullptr;
        last_fire = in.t;
        out.fire = true;
        out.stick_x = sx;
        out.stick_y = sy;
        return out;
    }

    void Reset() {
        *this = Detector{};
    }

private:
    struct Sample {
        double t;
        float x, z;
    };

    // The speed fell: a lean that was refused on every frame reports its last refusal.
    void EndLean(Output& out) {
        if (fast_since >= 0.0 && !fired && rejected != nullptr) {
            out = candidate;
            out.fire = false;
            out.rejected = rejected;
        }
        fast_since = -1.0;
        fired = false;
        rejected = nullptr;
    }

    std::array<Sample, Capacity> samples{};
    size_t count{0};
    bool have_yaw{false};
    float last_yaw{0.0f};
    double last_t{0.0};
    float yaw_rate{0.0f};
    double fast_since{-1.0};
    bool fired{false};
    const char* rejected{nullptr};
    Output candidate{};
    double last_fire{-1e9};
};

} // namespace VR::Lean
