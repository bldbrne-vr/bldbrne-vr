// SPDX-License-Identifier: GPL-2.0-or-later

// render_steady (src/core/vr/render_steady.h): the render camera holds through standing still's
// micro-motion, is pulled along at the band's edge beyond it and follows real head motion exactly.

#include <cmath>
#include <gtest/gtest.h>
#include "core/vr/render_steady.h"

namespace {

using namespace VR::Steady;

constexpr float Deg = 3.14159265358979f / 180.0f;
constexpr int64_t Frame = 16'666'667; // 60 fps

Q Yaw(float deg) {
    return {0.0f, std::sin(0.5f * deg * Deg), 0.0f, std::cos(0.5f * deg * Deg)};
}

// A standing-still sway: yaw 0.3 deg, x 2 mm, at 0.4 / 0.7 Hz.
void Sway(int i, Q& q, V& p) {
    const float t = static_cast<float>(i) / 60.0f;
    q = Yaw(0.3f * std::sin(6.2831853f * 0.4f * t));
    p = {0.002f * std::sin(6.2831853f * 0.7f * t), 1.6f, 0.0f};
}

TEST(RenderSteady, StartsAtTheHead) {
    State s{};
    Q q_out{};
    V p_out{};
    const Step step = Update(s, Yaw(10.0f), {1.0f, 2.0f, 3.0f}, 1000, Config{}, q_out, p_out);
    EXPECT_EQ(step, Step::Start);
    EXPECT_NEAR(AngleDeg(q_out, Yaw(10.0f)), 0.0f, 1e-3f);
    EXPECT_NEAR(p_out.x, 1.0f, 1e-6f);
    EXPECT_NEAR(p_out.z, 3.0f, 1e-6f);
}

TEST(RenderSteady, HoldsThroughMicroMotion) {
    State s{};
    Q q{}, q_out{}, first_q{};
    V p{}, p_out{}, first_p{};
    int moved = 0;
    for (int i = 0; i < 600; ++i) {
        Sway(i, q, p);
        Update(s, q, p, 1'000'000'000LL + i * Frame, Config{}, q_out, p_out);
        if (i == 0) {
            first_q = q_out;
            first_p = p_out;
        } else if (AngleDeg(first_q, q_out) > 1e-4f || DistMm(first_p, p_out) > 1e-4f) {
            ++moved;
        }
        // Never further from the head than the band.
        EXPECT_LE(AngleDeg(q, q_out), Config{}.band_deg + 1e-3f);
        EXPECT_LE(DistMm(p, p_out), Config{}.band_mm + 1e-3f);
    }
    EXPECT_EQ(moved, 0); // a 0.3 deg / 2 mm sway never moves the render camera
}

TEST(RenderSteady, PulledAtTheBandsEdgeOnASlowDrift) {
    State s{};
    Q q_out{};
    V p_out{};
    const Config c{};
    for (int i = 0; i < 120; ++i) {
        const float yaw = 3.0f * static_cast<float>(i) / 60.0f; // 3 deg/s: below follow_deg_s
        const Step step = Update(s, Yaw(yaw), {0.0f, 1.6f, 0.0f}, i * Frame, c, q_out, p_out);
        if (i > 0) {
            EXPECT_NE(step, Step::Follow);
        }
        EXPECT_LE(AngleDeg(Yaw(yaw), q_out), c.band_deg + 1e-3f);
    }
    // After 2 s at 3 deg/s the render camera trails by exactly the band.
    EXPECT_NEAR(AngleDeg(Yaw(6.0f - 3.0f / 60.0f), q_out), c.band_deg, 0.02f);
}

TEST(RenderSteady, FollowsARealTurnExactly) {
    State s{};
    Q q_out{};
    V p_out{};
    for (int i = 0; i < 60; ++i) {
        const float yaw = 30.0f * static_cast<float>(i) / 60.0f; // 30 deg/s
        const Step step =
            Update(s, Yaw(yaw), {0.0f, 1.6f, 0.0f}, i * Frame, Config{}, q_out, p_out);
        if (i >= 2) {
            EXPECT_EQ(step, Step::Follow);
            EXPECT_NEAR(AngleDeg(Yaw(yaw), q_out), 0.0f, 1e-3f);
        }
    }
}

TEST(RenderSteady, FollowsALeanExactly) {
    State s{};
    Q q_out{};
    V p_out{};
    for (int i = 0; i < 60; ++i) {
        const float x = 0.1f * static_cast<float>(i) / 60.0f; // 10 cm/s
        const Step step = Update(s, Yaw(0.0f), {x, 1.6f, 0.0f}, i * Frame, Config{}, q_out, p_out);
        if (i >= 2) {
            EXPECT_EQ(step, Step::Follow);
            EXPECT_NEAR(p_out.x, x, 1e-6f);
        }
    }
}

TEST(RenderSteady, HoldsAfterATurnEnds) {
    State s{};
    Q q_out{};
    V p_out{};
    int64_t t = 0;
    for (int i = 0; i < 30; ++i, t += Frame) {
        Update(s, Yaw(40.0f * static_cast<float>(i) / 60.0f), {0.0f, 1.6f, 0.0f}, t, Config{},
               q_out, p_out);
    }
    // The head stops at 19.33 deg and sways 0.3 deg around it: after the speed's hold the render
    // camera stays put.
    Q q{};
    V p{};
    Q held{};
    int moved = 0;
    for (int i = 0; i < 300; ++i, t += Frame) {
        Sway(i, q, p);
        const Q head = Yaw(40.0f * 29.0f / 60.0f + 2.0f * std::asin(q.y) / Deg);
        Update(s, head, {0.0f, 1.6f, 0.0f}, t, Config{}, q_out, p_out);
        if (i == 30) {
            held = q_out;
        } else if (i > 30 && AngleDeg(held, q_out) > 1e-4f) {
            ++moved;
        }
    }
    EXPECT_EQ(moved, 0);
}

TEST(RenderSteady, GapStartsOver) {
    State s{};
    Q q_out{};
    V p_out{};
    Update(s, Yaw(0.0f), {0.0f, 1.6f, 0.0f}, 0, Config{}, q_out, p_out);
    const Step step = Update(s, Yaw(0.5f), {0.0f, 1.6f, 0.0f}, 500'000'000, Config{}, q_out, p_out);
    EXPECT_EQ(step, Step::Start);
    EXPECT_NEAR(AngleDeg(q_out, Yaw(0.5f)), 0.0f, 1e-3f);
}

TEST(RenderSteady, AngleIsAccurateForSmallAngles) {
    EXPECT_NEAR(AngleDeg(Yaw(0.0f), Yaw(0.01f)), 0.01f, 1e-4f);
    EXPECT_NEAR(AngleDeg(Yaw(10.0f), Yaw(10.5f)), 0.5f, 1e-3f);
    const Q n{-Yaw(1.0f).x, -Yaw(1.0f).y, -Yaw(1.0f).z, -Yaw(1.0f).w}; // the same orientation
    EXPECT_NEAR(AngleDeg(Yaw(0.0f), n), 1.0f, 1e-3f);
    EXPECT_NEAR(AngleDeg(Toward(Yaw(0.0f), n, 0.5f), Yaw(0.5f)), 0.0f, 1e-3f);
}


TEST(RenderSteady, NativeHoldsTinyRotationsButNeverQuantizesTranslation) {
    for (const int hz : {60, 90, 120}) {
        State s{};
        Config c{.2f, 5.0f, 8.0f, 50.0f}; // wrapper must ignore the positional band
        Q out{};
        V pos{};
        for (int i = 0; i < hz * 5; ++i) {
            const float t = float(i) / hz;
            const Q q = Yaw(.15f * std::sin(6.2831853f * .4f * t));
            const V p{.002f * std::sin(6.2831853f * .7f * t),
                      1.6f + .001f * std::sin(t), -.0001f * t};
            UpdateRotation(s, q, p, 1'000'000'000LL + i * (1'000'000'000LL / hz), c, out, pos);
            EXPECT_FLOAT_EQ(pos.x, p.x);
            EXPECT_FLOAT_EQ(pos.y, p.y);
            EXPECT_FLOAT_EQ(pos.z, p.z);
            EXPECT_NEAR(AngleDeg(out, Yaw(0)), 0.0f, 1e-5f);
            EXPECT_LE(AngleDeg(out, q), .2f + 1e-5f);
        }
    }
}

TEST(RenderSteady, NativeReleasesOnFirstDeliberateTurnOrLean) {
    for (bool lean : {false, true}) {
        State s{};
        const Config c{.2f, 5.0f, 8.0f, 50.0f};
        Q out{};
        V pos{};
        UpdateRotation(s, Yaw(0), {0, 1.6f, 0}, Frame, c, out, pos);
        const V p{lean ? .002f : 0.0f, 1.6f, 0};
        const Q q = Yaw(lean ? .1f : 1.0f);
        EXPECT_EQ(UpdateRotation(s, q, p, Frame * 2, c, out, pos), Step::Follow);
        EXPECT_NEAR(AngleDeg(q, out), 0.0f, 1e-5f);
        EXPECT_FLOAT_EQ(pos.x, p.x);
    }
}

TEST(RenderSteady, NativeBoundsSlowDriftAndResetsAfterGapOrRecenter) {
    State s{};
    const Config c{.2f, 5.0f, 8.0f, 50.0f};
    Q out{};
    V pos{};
    for (int i = 0; i < 120; ++i) {
        const Q q = Yaw(float(i) / 60.0f);
        UpdateRotation(s, q, {0, 1.6f, 0}, i * Frame, c, out, pos);
        EXPECT_LE(AngleDeg(q, out), .2f + 1e-4f);
    }
    EXPECT_EQ(UpdateRotation(s, Yaw(30), {1, 2, 3}, 200 * Frame, c, out, pos), Step::Start);
    EXPECT_NEAR(AngleDeg(Yaw(30), out), 0, 1e-5f);
    s.valid = false; // camera hook's recenter/menu/takeover reset
    EXPECT_EQ(UpdateRotation(s, Yaw(-70), {4, 5, 6}, 201 * Frame, c, out, pos), Step::Start);
    EXPECT_NEAR(AngleDeg(Yaw(-70), out), 0, 1e-5f);
    EXPECT_EQ(UpdateRotation(s, Yaw(-60), {4, 5, 6}, 200 * Frame, c, out, pos), Step::Start);
}

TEST(RenderSteady, NativeZeroBandPassesOrientationAndPositionThrough) {
    State s{};
    const Config c{0, 5.0f, 8.0f, 50.0f};
    Q out{};
    V pos{};
    for (int i = 0; i < 200; ++i) {
        Q q{};
        V p{};
        Sway(i, q, p);
        UpdateRotation(s, q, p, i * Frame, c, out, pos);
        EXPECT_NEAR(AngleDeg(q, out), 0, 1e-4f);
        EXPECT_FLOAT_EQ(p.x, pos.x);
    }
}

} // namespace
