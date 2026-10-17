// SPDX-License-Identifier: GPL-2.0-or-later
#include <limits>
#include <gtest/gtest.h>
#include "core/vr/head_room.h"
#include "core/vr/roomscale_walk.h"
#include "video_core/renderer_vulkan/vr_late_warp_math.h"

namespace {
namespace R = VR::Roomscale;
struct Walk {
    R::Controller controller;
    R::Frame f{};
    R::Config config{};
    R::Vec2 origin{};
    R::Output out{};
    Walk() {
        f.allowed = true;
        f.time = 1.0;
    }
    R::Output Tick(float dx = 0, float dz = 0, double dt = 1.0 / 60.0) {
        f.time += dt;
        f.base = f.base + R::Vec2{dx, dz};
        f.offset = f.head - origin;
        out = controller.Update(f, config);
        origin = origin + out.consume;
        f.applied = out.walking;
        return out;
    }
    void Settle(int frames = 16) {
        for (int i = 0; i < frames; ++i)
            Tick();
    }
};
TEST(RoomscaleWalk, SmallLeanAndShortExcursionStayPlanted) {
    Walk w;
    w.f.head = {0.20f, 0};
    w.Settle();
    EXPECT_FALSE(w.out.walking);
    w.f.head = {0.30f, 0};
    w.Settle(4);
    EXPECT_FALSE(w.out.walking);
    w.f.head = {0.20f, 0};
    w.Settle();
    EXPECT_FALSE(w.out.walking);
}
TEST(RoomscaleWalk, AllDirectionsWalkAndNeverExceedWalkingCap) {
    for (R::Vec2 head : {R::Vec2{0.4f, 0}, {-0.4f, 0}, {0, -0.4f}, {0, 0.4f}, {0.4f, -0.4f}}) {
        Walk w;
        w.f.head = head;
        w.Settle();
        ASSERT_TRUE(w.out.walking);
        EXPECT_GT(w.out.right * head.x - w.out.forward * head.z, 0);
        EXPECT_LE(std::hypot(w.out.right, w.out.forward), 0.450001f);
    }
}
TEST(RoomscaleWalk, HysteresisDoesNotChatterBetweenStartAndStop) {
    Walk w;
    w.f.head = {0.30f, 0};
    w.Settle();
    ASSERT_TRUE(w.out.walking);
    w.f.head = {0.20f, 0};
    EXPECT_TRUE(w.Tick().walking);
    w.f.head = {0.11f, 0};
    EXPECT_FALSE(w.Tick().walking);
    for (int i = 0; i < 30; ++i) {
        w.f.head = {i % 2 ? 0.27f : 0.20f, 0};
        EXPECT_FALSE(w.Tick().walking);
    }
}
TEST(RoomscaleWalk, BodyCatchesUpWithoutMovingTheCameraTwice) {
    for (float scale : {0.5f, 1.0f, 2.0f}) {
        for (float yaw : {0.0f, 0.7f, 1.5707963f, -2.0f}) {
            Walk w;
            w.f.scale = scale;
            w.f.base_yaw = yaw;
            w.f.view_yaw = yaw;
            w.f.head = {0.4f, -0.2f};
            w.Settle();
            ASSERT_TRUE(w.out.walking);
            const auto before = R::ToWorld(w.f.head, yaw) * scale;
            const auto step = R::ToWorld({0.01f, -0.005f}, yaw) * scale;
            for (int i = 0; i < 30; ++i) {
                w.Tick(step.x, step.z);
                const auto eye = w.f.base + R::ToWorld(w.f.head - w.origin, yaw) * scale;
                EXPECT_NEAR(eye.x, before.x, 1e-5f);
                EXPECT_NEAR(eye.z, before.z, 1e-5f);
            }
            EXPECT_FALSE(w.out.walking);
        }
    }
}
TEST(RoomscaleWalk, BlockedByWallNeverConsumesRequestedTravel) {
    Walk w;
    w.f.head = {0.6f, 0};
    w.Settle(180);
    EXPECT_TRUE(w.out.walking);
    EXPECT_FLOAT_EQ(w.origin.x, 0);
    EXPECT_FLOAT_EQ(w.origin.z, 0);
}
TEST(RoomscaleWalk, SlidingAlongWallConsumesOnlyActualTravel) {
    Walk w;
    w.f.head = {0.4f, -0.4f};
    w.Settle();
    w.Tick(0, 0.015f);
    EXPECT_NEAR(w.origin.x, 0, 1e-6f);
    EXPECT_NEAR(w.origin.z, -0.015f, 1e-6f);
}
TEST(RoomscaleWalk, NoCompensationUnlessWalkingInputWasApplied) {
    Walk w;
    w.f.head = {0.4f, 0};
    for (int i = 0; i < 16; ++i) {
        w.f.applied = false;
        w.Tick();
    }
    w.f.applied = false;
    w.Tick(0.02f, 0);
    EXPECT_FLOAT_EQ(w.origin.x, 0);
}
TEST(RoomscaleWalk, ReleaseCoastIsBounded) {
    Walk w;
    w.f.head = {0.4f, 0};
    w.Settle();
    w.Tick(0.29f, 0, 0.1); // inside stop radius; a legitimate step over 100 ms
    ASSERT_FALSE(w.out.walking);
    const float before = w.origin.x;
    w.Tick(0.01f, 0);
    EXPECT_GT(w.origin.x, before);
    w.Settle(20);
    const float settled = w.origin.x;
    w.Tick(0.01f, 0);
    EXPECT_FLOAT_EQ(w.origin.x, settled);
}
TEST(RoomscaleWalk, ManualMovementAndAnimationGatesCancelImmediately) {
    Walk w;
    w.f.head = {0.4f, 0};
    w.Settle();
    w.f.allowed = false;
    w.Tick(0.05f, 0);
    EXPECT_FALSE(w.out.walking);
    EXPECT_FLOAT_EQ(w.origin.x, 0);
    w.f.allowed = true;
    EXPECT_FALSE(w.Tick().walking);
    w.Settle();
    EXPECT_TRUE(w.out.walking);
}
TEST(RoomscaleWalk, RecenterTeleportTrackingJumpAndStaleFramesReset) {
    for (int mode = 0; mode < 5; ++mode) {
        Walk w;
        w.f.head = {0.4f, 0};
        w.Settle();
        if (mode == 0)
            ++w.f.space;
        if (mode == 1)
            ++w.f.turn;
        if (mode == 2)
            w.f.head.x += 1.0f;
        auto out = w.Tick(mode == 3 ? 4.0f : 0.01f, 0, mode == 4 ? 0.5 : 1.0 / 60.0);
        EXPECT_FALSE(out.walking);
        EXPECT_FLOAT_EQ(w.origin.x, 0);
    }
}
TEST(RoomscaleWalk, BadTrackingNeverProducesInput) {
    Walk w;
    w.f.head = {0.4f, 0};
    w.Settle();
    w.f.head.x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(w.Tick().walking);
    EXPECT_TRUE(R::Finite(w.out.consume));
}
TEST(RoomscaleWalk, HeadAndGameYawMapStickToWorldDirection) {
    Walk w;
    w.f.head = {0.4f, 0};
    w.f.base_yaw = 0;
    w.f.view_yaw = 1.5707963f;
    w.Settle();
    EXPECT_NEAR(w.out.right, 0, 1e-6f);
    EXPECT_GT(w.out.forward, 0.25f);
}
TEST(RoomscaleWalk, LeanSettingChangesSidewaysThresholdLive) {
    Walk w;
    w.f.head = {0.25f, 0};
    w.config.lean_scale = R::LeanScale({0.25f, 0}, 0, 35);
    w.Settle();
    EXPECT_FALSE(w.out.walking);
    w.config.lean_scale = R::LeanScale({0.25f, 0}, 0, 10);
    w.Settle();
    EXPECT_TRUE(w.out.walking);
}
TEST(RoomscaleWalk, LeanSettingIsRelativeToHunterAndLeavesForwardThresholdAlone) {
    EXPECT_NEAR(R::LeanScale({1, 0}, 0, 20), 1, 1e-6f);
    EXPECT_GT(R::LeanScale({1, 0}, 0, 35), 1.6f);
    EXPECT_LT(R::LeanScale({1, 0}, 0, 10), 0.6f);
    EXPECT_FLOAT_EQ(R::LeanScale({0, 1}, 0, 35), 1);
    EXPECT_FLOAT_EQ(R::LeanScale({0, 1}, 0, 0), 1);
    EXPECT_NEAR(R::LeanScale({0, 1}, 1.5707963f, 10), R::LeanScale({1, 0}, 0, 10), 1e-6f);
    EXPECT_FLOAT_EQ(R::LeanScale({1, 0}, 0, 0), 0.35f);
    EXPECT_FLOAT_EQ(R::LeanScale({1, 0}, 0, 20, false), 0.35f);
    EXPECT_FLOAT_EQ(R::LeanScale({1, 0}, 0, 20, true, false, 0.1f), 0.5f);
}
TEST(RoomscaleWalk, HorizontalTrackingStaysOneToOnePastOldLeanCap) {
    float x = 2, y = -1, z = 3;
    VR::HeadRoom::Clamp(x, y, z, 0.5f, 0.8f, true);
    EXPECT_FLOAT_EQ(x, 2);
    EXPECT_FLOAT_EQ(z, 3);
    EXPECT_FLOAT_EQ(y, -0.8f);
    const auto p = Vulkan::VrLate::CameraPoint({2, -1, 3}, 0, {0, 0, 0}, 0.5f, 0, 0.8f, true);
    EXPECT_FLOAT_EQ(p.x, x);
    EXPECT_FLOAT_EQ(p.y, y);
    EXPECT_FLOAT_EQ(p.z, z);
    VR::HeadRoom::Clamp(x, y, z, 0, 0.8f, true);
    EXPECT_FLOAT_EQ(x, 0);
    EXPECT_FLOAT_EQ(y, 0);
}
TEST(RoomscaleWalk, SettingsAlwaysLeaveAStopZoneAndFiniteOutput) {
    Walk w;
    w.config = {0.15f, 0.7f, 5.0f, 0.35f};
    w.f.head = {0.4f, 0};
    w.Settle();
    EXPECT_TRUE(w.out.walking);
    EXPECT_LE(w.out.right, 0.6f);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    w.config = {nan, nan, nan, nan};
    w.Settle();
    EXPECT_TRUE(std::isfinite(w.out.right));
}
} // namespace
