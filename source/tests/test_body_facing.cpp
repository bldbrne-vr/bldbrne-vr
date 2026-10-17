// SPDX-License-Identifier: GPL-2.0-or-later

// The hunter's VR heading (core/vr/body_facing.h): glancing within the deadzone leaves him, looking
// further turns him toward the view (time constant, settle, the smaller deadzone while moving), a
// fast head turn drags him along, a snap or smooth turn turns him by exactly as much at once, a
// recenter (the heading of tracking forward changes, the view does not) leaves him, wrap-around at
// +-180 degrees, and the stand-in lock point ahead of him along the heading.

#include <cmath>

#include <gtest/gtest.h>

#include "core/vr/body_facing.h"

namespace {

namespace B = VR::Body;

constexpr float kDeg = 3.14159265358979f / 180.0f;
constexpr float kDt = 1.0f / 60.0f;

float Deg(float radians) {
    return radians / kDeg;
}

B::FacingParams Defaults() {
    return B::FacingParams{};
}

// Steps `seconds` with the view at `view` (degrees) and the heading of tracking forward at `base`.
float Run(B::FacingTracker& t, float base, float view, float seconds, bool moving = false) {
    float facing = t.Facing();
    for (int i = 0; i * kDt < seconds; ++i) {
        facing = t.Step({base * kDeg, view * kDeg}, moving, kDt, Defaults());
    }
    return Deg(facing);
}

TEST(BodyFacing, Defaults) {
    const B::FacingParams p = Defaults();
    EXPECT_NEAR(Deg(p.deadzone), 35.0f, 0.1f);
    EXPECT_NEAR(Deg(p.moving_deadzone), 15.0f, 0.1f);
    EXPECT_NEAR(p.tau, 0.25f, 1e-6f);
    EXPECT_NEAR(Deg(p.settle), 2.0f, 0.1f);
}

TEST(BodyFacing, GlancesWithinTheDeadzoneLeaveHim) {
    B::FacingTracker t;
    t.Reset(0.0f);
    EXPECT_TRUE(t.Valid());
    for (const float view : {10.0f, -20.0f, 34.0f, -34.0f, 0.0f}) {
        EXPECT_NEAR(Run(t, 0.0f, view, 1.0f), 0.0f, 1e-4f) << view;
        EXPECT_FALSE(t.Catching());
    }
}

TEST(BodyFacing, LookingFurtherTurnsHimToTheView) {
    B::FacingTracker t;
    t.Reset(0.0f);
    // 60 degrees right: he follows (never more than the deadzone behind) and ends within the
    // settle angle of the view.
    float facing = 0.0f;
    for (int i = 0; i < 6; ++i) {
        facing = Run(t, 0.0f, 60.0f, 0.1f);
        EXPECT_LE(60.0f - facing, 35.0f + 1e-3f);
        EXPECT_GE(60.0f - facing, 0.0f);
    }
    facing = Run(t, 0.0f, 60.0f, 1.5f);
    EXPECT_LE(std::abs(60.0f - facing), 2.0f + 1e-3f);
    EXPECT_FALSE(t.Catching());
    // Glancing back within the deadzone of the new heading leaves him there.
    EXPECT_NEAR(Run(t, 0.0f, 40.0f, 1.0f), facing, 1e-4f);
    // The time constant: from a 60 degree jump the gap shrinks by e^-1 in about 0.25 s once the
    // deadzone stops dragging him.
    B::FacingTracker u;
    u.Reset(0.0f);
    Run(u, 0.0f, 50.0f, kDt * 0.5f);
    const float first = 50.0f - u.Facing() / kDeg;
    Run(u, 0.0f, 50.0f, 0.25f);
    const float later = 50.0f - u.Facing() / kDeg;
    EXPECT_NEAR(later / first, std::exp(-1.0f), 0.08f);
}

TEST(BodyFacing, MovingUsesTheSmallerDeadzone) {
    B::FacingTracker t;
    t.Reset(0.0f);
    EXPECT_NEAR(Run(t, 0.0f, 25.0f, 1.0f, false), 0.0f, 1e-4f);
    const float facing = Run(t, 0.0f, 25.0f, 1.5f, true);
    EXPECT_LE(std::abs(25.0f - facing), 2.0f + 1e-3f);
}

TEST(BodyFacing, FastHeadTurnsDragHimAlong) {
    B::FacingTracker t;
    t.Reset(0.0f);
    // The head swings round at 360 deg/s: he is never more than the deadzone behind.
    float view = 0.0f;
    for (int i = 0; i < 30; ++i) {
        view += 6.0f;
        const float facing = Deg(t.Step({0.0f, view * kDeg}, false, kDt, Defaults()));
        EXPECT_LE(std::abs(Deg(B::FacingWrap((view - facing) * kDeg))), 35.0f + 1e-3f) << i;
    }
    // Turned round fully: physically turning round turns him round.
    const float facing = Run(t, 0.0f, 180.0f, 2.0f);
    EXPECT_LE(std::abs(Deg(B::FacingWrap((180.0f - facing) * kDeg))), 2.0f + 1e-3f);
}

TEST(BodyFacing, TurnsTurnHimAtOnceRecentersDoNot) {
    B::FacingTracker t;
    t.Reset(0.0f);
    Run(t, 0.0f, 10.0f, 0.5f); // looking 10 degrees right, inside the deadzone
    EXPECT_NEAR(Deg(t.Facing()), 0.0f, 1e-4f);
    // A snap turn of 30 degrees: the heading of tracking forward and the view move together.
    float facing = Deg(t.Step({30.0f * kDeg, 40.0f * kDeg}, false, kDt, Defaults()));
    EXPECT_NEAR(facing, 30.0f, 1e-3f);
    EXPECT_NEAR(Deg(t.Turned()), 30.0f, 1e-3f);
    // And back the other way, across the wrap.
    t.Reset(170.0f * kDeg);
    t.Step({170.0f * kDeg, 170.0f * kDeg}, false, kDt, Defaults());
    facing = Deg(t.Step({-160.0f * kDeg, -160.0f * kDeg}, false, kDt, Defaults()));
    EXPECT_NEAR(facing, -160.0f, 1e-3f);
    // Smooth turning: a little every frame, for 1 s at 120 deg/s.
    t.Reset(0.0f);
    float base = 0.0f;
    t.Step({0.0f, 0.0f}, false, kDt, Defaults());
    for (int i = 0; i < 60; ++i) {
        base += 2.0f;
        facing = Deg(t.Step({base * kDeg, base * kDeg}, false, kDt, Defaults()));
    }
    EXPECT_NEAR(facing, 120.0f, 0.01f);
    // A recenter: the heading of tracking forward changes by the room's yaw, the view keeps its
    // direction (the head's tracking yaw drops by as much): he stays.
    t.Reset(0.0f);
    t.Step({0.0f, 20.0f * kDeg}, false, kDt, Defaults());
    facing = Deg(t.Step({-75.0f * kDeg, 20.0f * kDeg}, false, kDt, Defaults()));
    EXPECT_NEAR(facing, 0.0f, 1e-4f);
}

TEST(BodyFacing, AimPointAlongTheHeading) {
    const B::FacingPoint p = B::FacingAim(10.0f, -50.0f, 20.0f, 90.0f * kDeg, 50.0f, 1.3f);
    EXPECT_NEAR(p.x, 60.0f, 1e-3f); // forward = (sin yaw, 0, cos yaw)
    EXPECT_NEAR(p.y, -48.7f, 1e-3f);
    EXPECT_NEAR(p.z, 20.0f, 1e-3f);
    const B::FacingPoint q = B::FacingAim(0.0f, 0.0f, 0.0f, 180.0f * kDeg, 50.0f, 0.0f);
    EXPECT_NEAR(q.z, -50.0f, 1e-3f);
    EXPECT_NEAR(std::abs(Deg(std::atan2(q.x, q.z))), 180.0f, 1e-2f);
    for (const float a : {-540.0f, -181.0f, -180.0f, 0.0f, 179.0f, 181.0f, 720.0f}) {
        const float w = Deg(B::FacingWrap(a * kDeg));
        EXPECT_GE(w, -180.0f - 1e-3f) << a;
        EXPECT_LT(w, 180.0f + 1e-3f) << a;
        EXPECT_NEAR(std::sin(w * kDeg), std::sin(a * kDeg), 1e-4f) << a;
        EXPECT_NEAR(std::cos(w * kDeg), std::cos(a * kDeg), 1e-4f) << a;
    }
}

} // namespace
