// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cmath>
#include <limits>
#include <gtest/gtest.h>
#include "core/vr/vial_liquid.h"

namespace {
namespace L = VR::Held::Liquid;
constexpr float Pi = 3.14159265359f;
constexpr L::V3 Up{0, 1, 0};
std::int64_t Ns(double t) { return 1'000'000'000 + std::llround(t * 1e9); }
L::Pose Vessel(L::V3 position = {}) {
    L::Pose p;
    p.valid = true;
    p.position = position;
    p.space = 1;
    return p;
}
float Rock(L::V3 n) { return std::hypot(n[0], n[2]); }

TEST(VialLiquid, StillAndConstantSpeedDoNotInventSlosh) {
    for (const L::V3 speed : {L::V3{}, L::V3{0.3f, -0.2f, 0.1f}}) {
        L::Motion motion;
        for (int i = 0; i < 360; ++i) {
            const float t = i / 90.0f;
            const auto n = motion.Update(Up, Ns(t), Vessel({0.2f + speed[0] * t,
                                                           1.1f + speed[1] * t,
                                                           -0.3f + speed[2] * t}));
            EXPECT_LT(Rock(n), 0.0001f);
        }
    }
}

TEST(VialLiquid, TiltAloneNeverExcitesAWaveOrRebounds) {
    for (int goods : {1000, 1120, 2030, 1260})
    for (int rate : {45, 90, 144}) for (bool tracked : {false, true}) {
        L::Motion motion;
        for (int i = 0; i < rate * 4; ++i) {
            const float t = float(i) / rate;
            // Rock, stop abruptly, then invert. The vessel centre does not translate.
            const float angle = t < 2 ? 1.3f * std::sin(t * 4) : t < 3 ? 0.8f : Pi;
            const float c = std::cos(angle), s = std::sin(angle);
            auto pose = Vessel({0.2f, 1.1f, -0.3f});
            pose.valid = tracked;
            pose.axes = {{{c, s, 0}, {-s, c, 0}, {0, 0, 1}}};
            const L::V3 up{s, c, 0};
            const auto n = motion.Update(up, Ns(t), pose, L::ResponseForGoods(goods));
            for (int axis = 0; axis < 3; ++axis) EXPECT_NEAR(n[axis], up[axis], 1e-6f);
        }
    }
}

struct ShakeResult {
    float peak{}, rebound{}, settled{}, rms{}, settle_time{};
};
ShakeResult Shake(float amplitude, int rate, bool vertical = true, int goods = 1000) {
    L::Motion motion;
    ShakeResult result;
    float sum = 0;
    for (int i = 0; i <= rate * 4; ++i) {
        const float t = float(i) / rate;
        auto p = Vessel({0.2f, 1.1f, -0.3f});
        if (t <= 1.0f) p.position[vertical ? 1 : 0] += amplitude * std::sin(2 * Pi * 3 * t);
        const auto n = motion.Update(Up, Ns(t), p, L::ResponseForGoods(goods));
        EXPECT_TRUE(L::Finite(n));
        EXPECT_NEAR(L::Dot(n, n), 1, 1e-5f);
        if (t <= 1) {
            result.peak = std::max(result.peak, Rock(n));
            sum += Rock(n) * Rock(n);
        } else if (t < 1.5f) {
            result.rebound = std::max(result.rebound, Rock(n));
        }
        if (t > 1 && Rock(n) > result.peak * 0.02f) result.settle_time = t - 1;
        if (t == 4) result.settled = Rock(n);
    }
    result.rms = std::sqrt(sum / rate);
    return result;
}

TEST(VialLiquid, VerticalAndSidewaysShakesReboundThenSettle) {
    for (bool vertical : {true, false}) {
        const auto gentle = Shake(0.012f, 90, vertical);
        const auto firm = Shake(0.055f, 90, vertical);
        EXPECT_GT(gentle.peak, 0.01f);
        EXPECT_GT(firm.peak, gentle.peak * 1.5f);
        EXPECT_LT(firm.peak, 0.5f); // no impossible surface flip
        EXPECT_GT(firm.rebound, 0.01f);
        EXPECT_LT(firm.settled, 0.001f);
    }
}

TEST(VialLiquid, NativeLiquidsHaveDistinctStableSettlingSpeeds) {
    for (bool vertical : {false, true}) {
        const auto blue = Shake(0.04f, 90, vertical, 1120);
        const auto blood = Shake(0.04f, 90, vertical, 1260);
        const auto lead = Shake(0.04f, 90, vertical, 2030);
        EXPECT_LT(blue.settle_time, blood.settle_time);
        EXPECT_LT(blood.settle_time, lead.settle_time);
        for (int goods : {1120, 1260, 2030}) {
            const auto reference = Shake(0.04f, 90, vertical, goods);
            EXPECT_GT(reference.peak, 0.01f);
            EXPECT_LT(reference.peak, 0.5f);
            EXPECT_LT(reference.settled, 0.001f);
            for (int rate : {45, 60, 72, 120, 144}) {
                const auto result = Shake(0.04f, rate, vertical, goods);
                EXPECT_NEAR(result.peak, reference.peak, reference.peak * 0.25f);
                EXPECT_NEAR(result.settle_time, reference.settle_time, 0.15f);
                EXPECT_LT(result.settled, 0.001f);
            }
        }
    }
}

TEST(VialLiquid, SimilarMotionAcrossGameFrameRates) {
    const auto reference = Shake(0.04f, 90);
    for (int rate : {45, 60, 72, 120, 144}) {
        const auto result = Shake(0.04f, rate);
        EXPECT_NEAR(result.peak, reference.peak, reference.peak * 0.25f) << rate;
        EXPECT_NEAR(result.rms, reference.rms, reference.rms * 0.25f) << rate;
    }
}

TEST(VialLiquid, TrackingSpaceRotationsDoNotChangeVesselLocalMotion) {
    L::Motion a, b;
    for (int i = 0; i < 180; ++i) {
        const float t = i / 90.0f;
        const float x = 0.04f * std::sin(2 * Pi * 3 * t);
        auto first = Vessel({x, 1, 0});
        auto rotated = Vessel({0, 1, -x});
        rotated.axes = {{{0, 0, -1}, {0, 1, 0}, {1, 0, 0}}};
        const auto n = a.Update(Up, Ns(t), first);
        const auto r = b.Update(Up, Ns(t), rotated);
        for (int axis = 0; axis < 3; ++axis) EXPECT_NEAR(n[axis], r[axis], 1e-5f);
    }
}

TEST(VialLiquid, TrackingLossRecenteringAndTeleportsCannotLaunchTheLiquid) {
    for (int failure = 0; failure < 5; ++failure) {
        L::Motion motion;
        double t = 0;
        auto p = Vessel({0, 1, 0});
        for (int i = 0; i < 80; ++i) {
            t = i / 90.0;
            p.position[1] = 1 + 0.05f * std::sin(2 * Pi * 3 * t);
            motion.Update(Up, Ns(t), p);
        }
        t += 1.0 / 90;
        if (failure == 0) ++p.space;
        if (failure == 1) p.position[0] += 2;
        if (failure == 2) t += 1;
        if (failure == 3) p.valid = false;
        if (failure == 4) p.position[0] = std::numeric_limits<float>::quiet_NaN();
        const auto reset = motion.Update(Up, Ns(t), p);
        EXPECT_EQ(reset, Up) << failure;
        p = Vessel({0, 1, 0});
        for (int i = 0; i < 90; ++i) {
            t += 1.0 / 90;
            EXPECT_LT(Rock(motion.Update(Up, Ns(t), p)), 0.001f) << failure;
        }
    }
}

TEST(VialLiquid, DuplicateRenderSamplesAndTinyTrackingNoiseStayQuiet) {
    L::Motion motion;
    for (int i = 0; i < 180; ++i) {
        const auto now = Ns(i / 90.0);
        const auto p = Vessel({i % 2 ? 0.0002f : -0.0002f, 1, 0});
        const auto n = motion.Update(Up, now, p);
        EXPECT_EQ(motion.Update(Up, now, p), n);
        EXPECT_EQ(motion.Update(Up, now + 100'000, p), n);
        EXPECT_LT(Rock(n), 0.003f);
    }
}

TEST(VialLiquid, TiltStillWorksAndShakeDoesNotChangeTheLiquidAmount) {
    L::Motion motion;
    const L::V3 tilted = L::Unit({0.4f, 1, 0.2f});
    motion.Update(Up, Ns(0));
    L::V3 n{};
    for (int i = 1; i < 360; ++i) n = motion.Update(tilted, Ns(i / 90.0));
    for (int axis = 0; axis < 3; ++axis) EXPECT_NEAR(n[axis], tilted[axis], 1e-5f);
    // Independent voxel integration of the cylindrical chamber, rather than reusing Fraction().
    constexpr float radius = 0.0222f, half = 0.036f;
    for (const auto normal : {Up, tilted, L::Unit({1, 0.1f, 0.4f}), L::V3{0, -1, 0}}) {
        for (float fill : {0.0f, 0.2f, 0.72f, 0.87f, 1.0f}) {
            const float d = L::Plane(normal, radius, half, fill);
            int total = 0, wet = 0;
            for (int ix = 0; ix < 40; ++ix) for (int iz = 0; iz < 40; ++iz) {
                const float x = radius * (2 * (ix + 0.5f) / 40 - 1);
                const float z = radius * (2 * (iz + 0.5f) / 40 - 1);
                if (x * x + z * z > radius * radius) continue;
                for (int iy = 0; iy < 100; ++iy) {
                    const float y = half * (2 * (iy + 0.5f) / 100 - 1);
                    ++total;
                    wet += L::Dot(normal, {x, y, z}) <= d;
                }
            }
            EXPECT_NEAR(float(wet) / total, fill, 0.011f);
        }
    }
}
} // namespace
