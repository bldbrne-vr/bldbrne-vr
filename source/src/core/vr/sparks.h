// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "core/vr/arm_ik.h"

namespace VR::Sparks {

using Body::Vec3;

struct Spark {
    Vec3 p{};
    Vec3 v{};
    float age{0.0f};
    float life{0.0f};
    float size{1.0f}; // its streak's thickness (x StreakM)
};

constexpr std::size_t MaxSparks = 512;
constexpr float Gravity = 9.8f;      // m/s^2, down (-y)
constexpr float Drag = 1.5f;         // 1/s: a tiny ember slows in the air
constexpr float MinSlide = 0.1f;
constexpr float FullSlide = 2.0f;
constexpr float MaxRate = 240.0f;
constexpr float StreakS = 0.012f;    // a streak is its last 12 ms of flight (the eye's blur) ...
constexpr float StreakM = 0.0007f;   // ... 0.7 mm thick

// 0 .. 1: how hard a scrape at `slide` m/s throws.
constexpr float SlideLevel(float slide) {
    const float k = (slide - MinSlide) / (FullSlide - MinSlide);
    return k < 0.0f ? 0.0f : k > 1.0f ? 1.0f : k;
}

// A spark's colour and glow at `heat` (1 new .. 0 dead): white-yellow, then orange, then dull red,
// fading out over its last fifth.
struct Color {
    float r{0.0f}, g{0.0f}, b{0.0f}, a{0.0f};
};
inline Color Heat(float heat) {
    const float h = std::clamp(heat, 0.0f, 1.0f);
    const float r = 1.0f;
    const float g = std::clamp(0.25f + 0.85f * h, 0.0f, 1.0f);
    const float b = std::clamp(1.6f * h - 0.8f, 0.0f, 1.0f);
    const float a = std::clamp(h * 5.0f, 0.0f, 1.0f) * (0.55f + 0.45f * h);
    return {r, g, b, a};
}

class Field {
public:
    // A scrape this update: at `point` (on the surface), `normal` out of it, `slide` the contact's
    // motion along it (m/s), over `dt`. `carry`: the sparks owed to this scrape (each contact its
    // own, kept by the caller: one scraping slowly never holds back another's).
    void Scrape(float& carry, Vec3 point, Vec3 normal, Vec3 slide, float dt) {
        const float speed = Body::Length(slide);
        const float level = SlideLevel(speed);
        if (level <= 0.0f || !(dt > 0.0f)) {
            carry = 0.0f;
            return;
        }
        carry += MaxRate * level * dt;
        const Vec3 along = Body::Normalize(slide, Body::AnyPerpendicular(normal));
        while (carry >= 1.0f) {
            carry -= 1.0f;
            Spawn(point, normal, along, 0.8f + 1.6f * level, 0.6f, 1.5f);
        }
    }

    // A strike at `speed` m/s into the surface: a little spray, out of it.
    void Strike(Vec3 point, Vec3 normal, Vec3 slide, float speed) {
        const int count = std::clamp(static_cast<int>(2.0f + 1.5f * speed), 2, 8);
        const Vec3 along = Body::Normalize(slide, Body::AnyPerpendicular(normal));
        for (int i = 0; i < count; ++i) {
            Spawn(point, normal, along, 1.2f + 0.4f * std::min(speed, 4.0f), 0.25f);
        }
    }

    void Step(float dt) {
        if (n_ == 0 || !(dt > 0.0f)) {
            return;
        }
        const float slow = std::exp(-Drag * dt);
        for (std::size_t i = 0; i < n_;) {
            Spark& s = s_[i];
            s.age += dt;
            if (s.age >= s.life) {
                s_[i] = s_[--n_]; // gone: the last one takes its place
                continue;
            }
            s.v.y -= Gravity * dt;
            s.v = s.v * slow;
            s.p = s.p + s.v * dt;
            ++i;
        }
    }

    void Clear() {
        n_ = 0;
    }
    std::size_t Count() const {
        return n_;
    }
    const Spark& operator[](std::size_t i) const {
        return s_[i];
    }

private:
    // One spark from `point`: along the scrape (`along_share`, the rest out of the surface), in a
    // cone, at about `speed`.
    void Spawn(Vec3 point, Vec3 normal, Vec3 along, float speed, float out_share,
               float life_scale = 1.0f) {
        const Vec3 side =
            Body::Normalize(Body::Cross(normal, along), Body::AnyPerpendicular(normal));
        const Vec3 dir =
            Body::Normalize(along * (1.0f - out_share) + normal * (out_share + 0.25f) +
                                side * (Rand() - 0.5f) * 0.9f + along * (Rand() - 0.5f) * 0.5f +
                                normal * Rand() * 0.4f,
                            normal);
        Spark s{};
        s.p = point + normal * 0.004f;
        s.v = dir * (speed * (0.6f + 0.8f * Rand()));
        s.life = (0.15f + 0.3f * Rand()) * life_scale;
        s.size = 0.6f + 0.4f * Rand();
        if (n_ < s_.size()) {
            s_[n_++] = s;
            return;
        }
        // Full: the oldest one goes.
        std::size_t oldest = 0;
        for (std::size_t i = 1; i < n_; ++i) {
            if (s_[i].age / s_[i].life > s_[oldest].age / s_[oldest].life) {
                oldest = i;
            }
        }
        s_[oldest] = s;
    }

    float Rand() { // xorshift32: 0 .. 1
        seed_ ^= seed_ << 13;
        seed_ ^= seed_ >> 17;
        seed_ ^= seed_ << 5;
        return static_cast<float>(seed_ >> 8) * (1.0f / 16777216.0f);
    }

    std::array<Spark, MaxSparks> s_{};
    std::size_t n_{0};
    std::uint32_t seed_{0x9E3779B9u};
};

} // namespace VR::Sparks
