// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>

#include "core/vr/root_height_hold.h"

namespace {

constexpr float Scale = 1.0f; // game units per metre
constexpr float Deg = 3.14159265f / 180.0f;

struct Hunter {
    float phys[3]{-3.298f, -1.0886f, 3.8079f};
    float model_y{-1.0484f};
    float yaw{72.0f * Deg};
};

// The mock's in-place turn (2026-10-15): 72 deg -> -2 deg over 250 ms (90 ticks/s), the model's
// height easing from -1.0484 to -1.0280 over 0.5 s while the physics root stays put.
TEST(RootHeightHold, AnInPlaceTurnKeepsTheEyeHeight) {
    VR::RootHeightHold hold;
    Hunter h;
    const float start = hold.Step(h.phys, h.model_y, h.yaw, Scale);
    EXPECT_FLOAT_EQ(start, h.model_y);
    for (int i = 1; i <= 45; ++i) {
        const float t = static_cast<float>(i) / 45.0f;
        h.yaw = (72.0f - 74.0f * std::min(1.0f, i / 22.5f)) * Deg;
        h.model_y = -1.0484f + (0.0204f) * t;
        EXPECT_NEAR(hold.Step(h.phys, h.model_y, h.yaw, Scale), start, 1e-6f) << i;
    }
    EXPECT_TRUE(hold.Held());
    // Standing still after the turn: still held.
    for (int i = 0; i < 90; ++i) {
        EXPECT_NEAR(hold.Step(h.phys, h.model_y, h.yaw, Scale), start, 1e-6f);
    }
}

// The eye never moves against the floor: a tick where the model rises does not lower it, one where
// it drops does not raise it.
void ExpectWithTheFloor(float d_root, float d_model, int tick) {
    if (d_model > 0.0f) {
        EXPECT_GE(d_root, -1e-6f) << tick;
    } else if (d_model < 0.0f) {
        EXPECT_LE(d_root, 1e-6f) << tick;
    }
}

// Walking off after a turn, either way round, up a slope (2.5 cm a tick, 0.19 cm higher a tick):
// a root held above the model waits for the floor to rise to it, one held below fades up on top
// of the rise; neither ever steps, and 0.75 m on it is all the model's.
TEST(RootHeightHold, WalkingAgainGoesBackToTheModel) {
    for (const float eased : {-0.0274f, 0.0274f}) {
        VR::RootHeightHold hold;
        Hunter h;
        const float start = hold.Step(h.phys, h.model_y, h.yaw, Scale);
        h.yaw += 5.0f * Deg;
        h.model_y += eased; // the model eases during the turn; the root stays
        EXPECT_NEAR(hold.Step(h.phys, h.model_y, h.yaw, Scale), start, 1e-6f);
        ASSERT_TRUE(hold.Held());
        float last = start;
        for (int i = 0; i < 32; ++i) {
            h.phys[2] += 0.025f;
            h.phys[1] += 0.0019f;
            h.model_y += 0.0019f;
            const float root = hold.Step(h.phys, h.model_y, h.yaw, Scale);
            EXPECT_FALSE(hold.Held());
            ExpectWithTheFloor(root - last, 0.0019f, i);
            EXPECT_LE(root - last, 0.0019f + 0.0274f * 0.025f / 0.75f + 1e-5f) << eased << " " << i;
            last = root;
        }
        EXPECT_NEAR(last, h.model_y, 1e-6f) << eased;
    }
}

// The mock's first walk (2026-10-15): held 2.74 cm above the model, the floor then rising 1.3 cm,
// level for 0.3 m, rising 3.2 cm. The eye never sinks on the way, and ends on the model.
TEST(RootHeightHold, ARiseThatPausesNeverDipsTheEye) {
    VR::RootHeightHold hold;
    Hunter h;
    hold.Step(h.phys, h.model_y, h.yaw, Scale);
    h.yaw += 37.0f * Deg;
    h.model_y -= 0.0274f;
    float last = hold.Step(h.phys, h.model_y, h.yaw, Scale);
    ASSERT_TRUE(hold.Held());
    for (int i = 0; i < 60; ++i) {
        h.phys[2] += 0.025f;
        const float rise = i < 6 ? 0.0022f : i < 18 ? 0.0f : i < 34 ? 0.002f : 0.0f;
        h.model_y += rise;
        const float root = hold.Step(h.phys, h.model_y, h.yaw, Scale);
        ExpectWithTheFloor(root - last, rise, i);
        EXPECT_GE(root - last, -0.0274f * 0.025f / 0.75f - 1e-6f) << i; // a level glide at most
        last = root;
    }
    EXPECT_NEAR(last, h.model_y, 1e-6f);
}

// After a turn, stairs straight away: the physics root steps 18 cm, the model climbs smoothly,
// and the root follows the model (waiting for it at first), never the steps.
TEST(RootHeightHold, StairsAfterATurnFollowTheModelNotTheSteps) {
    VR::RootHeightHold hold;
    Hunter h;
    hold.Step(h.phys, h.model_y, h.yaw, Scale);
    h.yaw += 20.0f * Deg;
    h.model_y -= 0.03f;
    hold.Step(h.phys, h.model_y, h.yaw, Scale);
    ASSERT_TRUE(hold.Held());
    float last = hold.Step(h.phys, h.model_y, h.yaw, Scale);
    for (int i = 0; i < 40; ++i) {
        h.phys[2] += 0.02f;
        if (i % 10 == 9) {
            h.phys[1] += 0.18f;
        }
        const float model_before = h.model_y;
        h.model_y += 0.018f;
        const float root = hold.Step(h.phys, h.model_y, h.yaw, Scale);
        EXPECT_GE(root - last, -1e-6f) << i;
        EXPECT_LE(root - last, h.model_y - model_before + 1e-5f) << i;
        last = root;
    }
    EXPECT_NEAR(last, h.model_y, 1e-5f);
}

TEST(RootHeightHold, StairsWithoutATurnFollowTheModelExactly) {
    VR::RootHeightHold hold;
    Hunter h;
    hold.Step(h.phys, h.model_y, h.yaw, Scale);
    for (int i = 0; i < 60; ++i) {
        h.phys[2] += 0.02f;
        if (i % 10 == 9) {
            h.phys[1] += 0.18f; // the physics capsule steps
        }
        h.model_y += 0.018f; // the model climbs smoothly
        EXPECT_NEAR(hold.Step(h.phys, h.model_y, h.yaw, Scale), h.model_y, 1e-5f) << i;
    }
}

TEST(RootHeightHold, YawNoiseBelowTheThresholdDoesNotHold) {
    VR::RootHeightHold hold;
    Hunter h;
    hold.Step(h.phys, h.model_y, h.yaw, Scale);
    for (int i = 0; i < 30; ++i) {
        h.yaw += (i % 2 ? 0.05f : -0.05f) * Deg;
        h.model_y += 0.0001f;
        EXPECT_NEAR(hold.Step(h.phys, h.model_y, h.yaw, Scale), h.model_y, 1e-6f);
    }
    EXPECT_FALSE(hold.Held());
}

TEST(RootHeightHold, AJumpOfTheModelLetsGo) {
    VR::RootHeightHold hold;
    Hunter h;
    hold.Step(h.phys, h.model_y, h.yaw, Scale);
    h.yaw += 10.0f * Deg;
    hold.Step(h.phys, h.model_y, h.yaw, Scale);
    ASSERT_TRUE(hold.Held());
    h.model_y += 0.5f; // a lift's platform under him, the physics root not yet moved
    EXPECT_NEAR(hold.Step(h.phys, h.model_y, h.yaw, Scale), h.model_y, 1e-6f);
    EXPECT_FALSE(hold.Held());
}

TEST(RootHeightHold, TheYawWrapIsATurnOfItsShortSide) {
    VR::RootHeightHold hold;
    Hunter h;
    h.yaw = 179.99f * Deg;
    hold.Step(h.phys, h.model_y, h.yaw, Scale);
    h.yaw = -179.99f * Deg; // 0.02 deg across the wrap: not a turn
    h.model_y += 0.001f;
    EXPECT_NEAR(hold.Step(h.phys, h.model_y, h.yaw, Scale), h.model_y, 1e-6f);
    EXPECT_FALSE(hold.Held());
}

TEST(RootHeightHold, BothEyesOfAPairGetTheSameHeight) {
    VR::RootHeightHold hold;
    Hunter h;
    hold.Step(h.phys, h.model_y, h.yaw, Scale);
    h.yaw += 2.0f * Deg;
    h.model_y += 0.004f;
    const float first = hold.Step(h.phys, h.model_y, h.yaw, Scale);
    h.model_y += 0.0005f; // the second eye's read a moment later
    EXPECT_FLOAT_EQ(hold.Step(h.phys, h.model_y, h.yaw, Scale), first);
}

TEST(RootHeightHold, BadInputPassesTheModelThroughAndRestarts) {
    VR::RootHeightHold hold;
    Hunter h;
    hold.Step(h.phys, h.model_y, h.yaw, Scale);
    h.yaw += 3.0f * Deg;
    hold.Step(h.phys, h.model_y, h.yaw, Scale);
    ASSERT_TRUE(hold.Held());
    EXPECT_FLOAT_EQ(hold.Step(h.phys, h.model_y, h.yaw, 0.0f), h.model_y);
    EXPECT_FALSE(hold.Held());
}

} // namespace
