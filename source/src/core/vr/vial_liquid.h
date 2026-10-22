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
#include <cstdint>

// Cosmetic only. No inventory, injection, input or drain state is written here.
namespace VR::Held::Liquid {
using V3 = std::array<float, 3>;
inline float Dot(V3 a, V3 b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline V3 Unit(V3 v) {
    const float len = std::sqrt(Dot(v, v));
    if (!std::isfinite(len) || len < 1e-5f)
        return {0, 1, 0};
    return {v[0] / len, v[1] / len, v[2] / len};
}

inline bool Finite(V3 v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

// The drawn vessel in tracking space, in metres, after hand carry. Camera motion and game
// locomotion must not enter the acceleration estimate. Axes are unit vessel axes in that space.
struct Pose {
    V3 position{};
    std::array<V3, 3> axes{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    std::uint32_t space{};
    bool valid{false};
};

// Cosmetic response of the existing spring: no additional simulation or render work.
// Defaults retain the established blood vial / syringe behavior.
struct Response {
    float stiffness{144.0f};
    float damping{9.0f};
};
constexpr Response ResponseForGoods(std::int32_t goods) {
    switch (goods) {
    case 1120: return {225.0f, 22.0f}; // Blue Elixir: quick, light rebound
    case 2030: return {36.0f, 14.0f};  // Lead Elixir: slow, heavy, little ringing
    case 1260: return {81.0f, 17.0f};  // Blood Cocktail: syrupy, between the elixirs
    default: return {};
    }
}

// Gravity sets the level directly; only filtered vessel acceleration excites the damped wave;
// a small axial coupling makes up/down shakes rock the surface too. Plane() keeps the amount
// fixed as the edges rise and fall. No per-pixel work, particles or new draw calls are needed.
class Motion {
public:
    V3 Update(V3 up, std::int64_t ns, const Pose& pose = {}, Response response = {}) {
        up = Unit(up);
        const bool tracked = pose.valid && Finite(pose.position) &&
                             Finite(pose.axes[0]) && Finite(pose.axes[1]) && Finite(pose.axes[2]);
        const float dt = static_cast<float>(ns - last_ns) * 1e-9f;
        if (last_ns == 0 || dt > 0.15f || dt < 0.0f || Dot(up, last_up) < -0.25f ||
            tracked != have_position || (tracked && pose.space != space)) {
            return ResetAt(up, ns, pose, tracked);
        }
        // Duplicate eye/render samples cannot differentiate a position twice or amplify noise.
        if (dt < 0.002f) return Surface(last_up);

        // Spring only the acceleration offset, never gravity itself: rotating a still
        // bottle must not create a wave or rebound when the tilt stops.
        V3 target{};
        if (tracked) {
            V3 measured{};
            for (int i = 0; i < 3; ++i) measured[i] = (pose.position[i] - position[i]) / dt;
            if (Dot(measured, measured) > 36.0f) {
                return ResetAt(up, ns, pose, true); // tracking jump/teleport, not a shake
            }
            if (baseline_ns == 0) {
                // The first position after a gap can still be the old held pose.
                baseline_position = pose.position;
                baseline_ns = ns;
            } else if (ns - baseline_ns <= 50'000'000) {
                // A short observed baseline avoids differentiating one noisy recovery sample.
                const float elapsed = static_cast<float>(ns - baseline_ns) * 1e-9f;
                for (int i = 0; i < 3; ++i) {
                    speed[i] = (pose.position[i] - baseline_position[i]) / elapsed;
                }
            } else {
                const float velocity_blend = dt / (0.035f + dt);
                const float acceleration_blend = dt / (0.045f + dt);
                for (int i = 0; i < 3; ++i) {
                    const float next = speed[i] + velocity_blend * (measured[i] - speed[i]);
                    acceleration[i] += acceleration_blend * ((next - speed[i]) / dt - acceleration[i]);
                    speed[i] = next;
                }
            }
            V3 local{};
            for (int i = 0; i < 3; ++i) local[i] = Dot(acceleration, pose.axes[i]);
            const float axial = Dot(local, up);
            // A weak, stable wave direction in the surface plane. An axial shake excites a
            // rocking mode, not a changing fill level or an artificial floating liquid blob.
            V3 tangent{0.8f, 0.0f, 0.6f};
            if (std::abs(Dot(tangent, up)) > 0.9f) tangent = {0, 1, 0};
            const float along = Dot(tangent, up);
            for (int i = 0; i < 3; ++i) tangent[i] -= along * up[i];
            tangent = Unit(tangent);
            V3 push{};
            for (int i = 0; i < 3; ++i) {
                push[i] = 0.055f * (local[i] - axial * up[i]) + 0.025f * axial * tangent[i];
            }
            const float amount = std::sqrt(Dot(push, push));
            const float limit = amount > 0.30f ? 0.30f / amount : 1.0f;
            V3 bent = up;
            for (int i = 0; i < 3; ++i) bent[i] += push[i] * limit;
            bent = Unit(bent);
            for (int i = 0; i < 3; ++i) target[i] = bent[i] - up[i];
            position = pose.position;
        }
        const int steps = std::max(1, static_cast<int>(std::ceil(dt * 240.0f)));
        const float h = dt / steps;
        for (int i = 0; i < steps; ++i) {
            for (int j = 0; j < 3; ++j) {
                velocity[j] += (response.stiffness * (target[j] - wave[j]) - response.damping * velocity[j]) * h;
                wave[j] += velocity[j] * h;
            }
        }
        last_ns = ns;
        last_up = up;
        return Surface(up);
    }

private:
    V3 Surface(V3 up) const {
        for (int i = 0; i < 3; ++i) up[i] += wave[i];
        return Unit(up);
    }
    V3 ResetAt(V3 up, std::int64_t ns, const Pose& pose, bool tracked) {
        *this = {};
        last_up = up;
        last_ns = ns;
        position = baseline_position = pose.position;
        space = pose.space;
        have_position = tracked;
        return up;
    }
    V3 wave{}, velocity{}, last_up{0, 1, 0};
    V3 position{}, speed{}, acceleration{}, baseline_position{};
    std::int64_t last_ns{0}, baseline_ns{0};
    std::uint32_t space{};
    bool have_position{};
};
inline float CircleFraction(float x) {
    if (x <= -1.0f)
        return 0.0f;
    if (x >= 1.0f)
        return 1.0f;
    return (std::acos(-x) + x * std::sqrt(1.0f - x * x)) / 3.14159265358979323846f;
}
// Volume below dot(normal,p)=distance in a centred cylindrical chamber. Circular cross
// sections keep the volume constant when the syringe is turned sideways or upside down.
inline float Fraction(V3 n, float distance, float radius, float half_height) {
    const float radial = std::hypot(n[0], n[2]) * radius;
    if (radial < 1e-6f)
        return std::clamp(0.5f + distance / std::max(2.0f * half_height * std::abs(n[1]), 1e-6f),
                          0.0f, 1.0f);
    constexpr int Slices = 48;
    float sum = 0;
    for (int i = 0; i < Slices; ++i) {
        const float y = half_height * (2.0f * (static_cast<float>(i) + 0.5f) / Slices - 1.0f);
        sum += CircleFraction((distance - n[1] * y) / radial);
    }
    return sum / Slices;
}
inline float Plane(V3 n, float radius, float half_height, float fill) {
    const float extent = std::abs(n[1]) * half_height + std::hypot(n[0], n[2]) * radius;
    if (fill <= 0.0f)
        return -extent - 0.0001f;
    if (fill >= 1.0f)
        return extent + 0.0001f;
    float lo = -extent, hi = extent;
    for (int i = 0; i < 18; ++i) {
        const float mid = (lo + hi) * 0.5f;
        if (Fraction(n, mid, radius, half_height) < fill)
            lo = mid;
        else
            hi = mid;
    }
    return (lo + hi) * 0.5f;
}
} // namespace VR::Held::Liquid
