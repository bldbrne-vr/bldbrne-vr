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

namespace VR::Roomscale {
struct Vec2 {
    float x{}, z{};
};
inline Vec2 operator+(Vec2 a, Vec2 b) {
    return {a.x + b.x, a.z + b.z};
}
inline Vec2 operator-(Vec2 a, Vec2 b) {
    return {a.x - b.x, a.z - b.z};
}
inline Vec2 operator*(Vec2 a, float k) {
    return {a.x * k, a.z * k};
}
inline float Length(Vec2 a) {
    return std::hypot(a.x, a.z);
}
inline bool Finite(Vec2 a) {
    return std::isfinite(a.x) && std::isfinite(a.z);
}

// Tracking +X is right, -Z forward; game +Z is forward at yaw zero.
inline Vec2 ToWorld(Vec2 v, float yaw) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    return {c * v.x - s * v.z, -s * v.x - c * v.z};
}
inline Vec2 ToTracking(Vec2 v, float yaw) {
    return ToWorld(v, yaw);
}
// The in-game Side lean limit controls how much lateral room the body can cover before its
// feet follow. Normalise against its default 20 degrees; blend in the hunter's local direction,
// so changing side lean does not unexpectedly change forward walking. Keep a small quiet zone
// even at 0 degrees / posture Off. Legacy shift uses its configured translation range instead.
inline float LeanScale(Vec2 world_offset, float hunter_yaw, float side_deg, bool align = true,
                       bool lean = true, float shift_m = 0.20f) {
    if (!align)
        return 0.35f;
    if (!lean)
        return std::clamp(shift_m / 0.20f, 0.35f, 1.75f);
    const Vec2 local = ToTracking(world_offset, hunter_yaw);
    const float length = Length(local);
    if (length < 0.001f)
        return 1.0f;
    constexpr float deg = 3.14159265358979323846f / 180.0f;
    const float ratio =
        std::max(0.35f, std::sin(std::clamp(side_deg, 0.0f, 35.0f) * deg) / std::sin(20.0f * deg));
    return length / std::hypot(local.x / ratio, local.z);
}
struct Config {
    float start_m{0.28f};
    float stop_m{0.12f};
    float max_stick{0.45f}; // walking, never a sprint button
    float lean_scale{1.0f}; // current posture setting, in the direction of this step
};
struct Frame {
    bool allowed{false};
    bool applied{false}; // presenter actually supplied our stick since the previous tick
    double time{};       // steady-clock seconds
    std::uint32_t space{}, turn{};
    float scale{1.0f}, base_yaw{}, view_yaw{};
    Vec2 base{};   // comfort camera base, game units, before head/eye offsets
    Vec2 offset{}; // neck from neutral, tracking metres
    Vec2 head{};   // raw head, tracking metres (tracking-jump rejection)
};
struct Output {
    Vec2 consume{};           // move both head and neck origins by this, tracking metres
    float right{}, forward{}; // camera-relative game stick
    bool walking{false};
};

// Camera-thread controller. Only measured travel is consumed: a wall produces no travel and
// cannot falsely centre the head. No guest transform or collision state is ever written.
class Controller {
public:
    void Reset() {
        *this = {};
    }
    Output Update(const Frame& f, const Config& cfg = {}) {
        Output out{};
        const double dt = f.time - last.time;
        const bool sane = std::isfinite(f.time) && std::isfinite(f.scale) && f.scale > 0.0f &&
                          std::isfinite(f.base_yaw) && std::isfinite(f.view_yaw) &&
                          Finite(f.base) && Finite(f.offset) && Finite(f.head);
        const bool continuous = have && sane && f.space == last.space && f.turn == last.turn &&
                                f.scale == last.scale && dt > 0.0 && dt <= 0.15 &&
                                Length(f.base - last.base) / f.scale <= 0.5f &&
                                Length(f.head - last.head) <= 0.10f + 3.0f * dt;
        if (!sane || !f.allowed || !continuous) {
            walking = false;
            dwell = tail = 0.0f;
            have = sane;
            last = f;
            return out;
        }
        const Vec2 travel = (f.base - last.base) * (1.0f / f.scale);
        // A short coast accounts for walk release and the comfort spring settling. Manual
        // input / animation gates clear it immediately; no dodge or knockback is eaten.
        if (f.applied)
            tail = 0.15f;
        if (tail > 0.0f && last.allowed &&
            Length(travel) <= 0.02f + 3.0f * static_cast<float>(dt) &&
            travel.x * direction.x + travel.z * direction.z >= -0.001f) {
            out.consume = ToTracking(travel, f.base_yaw);
        }
        tail = std::max(0.0f, tail - static_cast<float>(dt));
        const Vec2 remaining = f.offset - out.consume;
        const float distance = Length(remaining);
        const float lean =
            std::clamp(std::isfinite(cfg.lean_scale) ? cfg.lean_scale : 1.0f, 0.35f, 1.75f);
        const float start =
            std::clamp(std::isfinite(cfg.start_m) ? cfg.start_m : 0.28f, 0.15f, 0.75f) * lean;
        const float stop = std::clamp(std::isfinite(cfg.stop_m) ? cfg.stop_m : 0.12f, 0.03f,
                                      start / lean - 0.05f) *
                           lean;
        if (walking && distance <= stop)
            walking = false;
        if (!walking) {
            dwell = distance >= start ? dwell + static_cast<float>(dt) : 0.0f;
            if (dwell >= 0.12f)
                walking = true;
        }
        if (walking && distance > 0.001f) {
            direction = ToWorld(remaining * (1.0f / distance), f.base_yaw);
            const Vec2 stick = ToTracking(direction, f.view_yaw);
            const float maximum =
                std::clamp(std::isfinite(cfg.max_stick) ? cfg.max_stick : 0.45f, 0.25f, 0.60f);
            const float push =
                0.25f + (maximum - 0.25f) * std::clamp((distance - stop) / 0.25f, 0.0f, 1.0f);
            out.right = stick.x * push;
            out.forward = -stick.z * push;
            out.walking = true;
        }
        last = f;
        return out;
    }

private:
    Frame last{};
    Vec2 direction{};
    bool have{false}, walking{false};
    float dwell{}, tail{};
};
} // namespace VR::Roomscale
