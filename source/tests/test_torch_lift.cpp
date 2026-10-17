// SPDX-License-Identifier: GPL-2.0-or-later

// The torch held up (core/vr/torch_lift.h): the levels by the hand's height against the eyes,
// their hysteresis, the flames per level, and finding the torch's own flame among the character's
// slot effects (never our own, never one on a body dummy).

#include <array>

#include <gtest/gtest.h>

#include "core/vr/torch_lift.h"

namespace {

namespace T = VR::Torch;

constexpr float Dt = 1.0f / 30.0f; // the game's updates

TEST(TorchLift, LevelsByTheHandsHeight) {
    const T::Settings s{};
    T::Lift lift;
    EXPECT_EQ(lift.Step(-0.60f, s, Dt), 0); // at the hip
    EXPECT_EQ(lift.Step(-0.25f, s, Dt), 1); // at the shoulder
    EXPECT_EQ(lift.Step(0.10f, s, Dt), 2);  // above the head
    for (int i = 0; i < 11; ++i) {          // lowered: kept a moment (0.37 s) ...
        EXPECT_EQ(lift.Step(-0.45f, s, Dt), 2);
    }
    EXPECT_EQ(lift.Step(-0.45f, s, Dt), 0); // ... then let go
}

// Within the hysteresis it is kept however long; under it only once it stays there DropS (the
// torch bobbing, swung down and up, never flickers); raising is at once.
TEST(TorchLift, NoFlickerAtAThreshold) {
    const T::Settings s{};
    T::Lift lift;
    EXPECT_EQ(lift.Step(s.raised_m + 0.001f, s, Dt), 1);
    for (int i = 0; i < 30; ++i) {
        EXPECT_EQ(lift.Step(s.raised_m - 0.08f, s, Dt), 1); // within the hysteresis: kept
    }
    for (int i = 0; i < 10; ++i) { // a dip under it, back in time: kept
        EXPECT_EQ(lift.Step(s.raised_m - 0.15f, s, Dt), 1);
    }
    EXPECT_EQ(lift.Step(s.raised_m - 0.05f, s, Dt), 1);
    for (int i = 0; i < 11; ++i) {
        EXPECT_EQ(lift.Step(s.raised_m - 0.15f, s, Dt), 1);
    }
    EXPECT_EQ(lift.Step(s.raised_m - 0.15f, s, Dt), 0);
    EXPECT_EQ(lift.Step(s.raised_m - 0.02f, s, Dt), 0); // not raised again until the threshold
    EXPECT_EQ(lift.Step(s.high_m, s, Dt), 2);
    EXPECT_EQ(lift.Step(s.high_m - 0.08f, s, Dt), 2);
    for (int i = 0; i < 12; ++i) { // under the high keep: down to raised, not lowered
        lift.Step(s.high_m - 0.15f, s, Dt);
    }
    EXPECT_EQ(lift.Level(), 1);
}

TEST(TorchLift, FlamesPerLevel) {
    T::Settings s{};
    EXPECT_EQ(T::Copies(0, s), 0);
    EXPECT_EQ(T::Copies(1, s), 1);
    EXPECT_EQ(T::Copies(2, s), 2);
    s.copies = 1;
    EXPECT_EQ(T::Copies(2, s), 1);
    s.copies = 0;
    EXPECT_EQ(T::Copies(2, s), 0);
}

TEST(TorchLift, FindsTheTorchsOwnFlame) {
    std::array<T::Node, 4> nodes{{
        {30000, 173, 173, 3},       // the vial in the palm (a body dummy)
        {30020, 9000, 9000, 20100}, // ours: never picked
        {12, -1, 8123, 20100},      // the torch's flame, spawned
        {13, 8200, -1, 20200},
    }};
    std::int32_t ffx = 0, dummy = 0;
    ASSERT_TRUE(T::PickFlame(nodes.data(), nodes.size(), 30020, 30029, ffx, dummy));
    EXPECT_EQ(ffx, 8123);
    EXPECT_EQ(dummy, 20100);
    // Nothing on the torch's dummies: nothing found.
    std::array<T::Node, 2> none{{{30000, 173, 173, 3}, {5, 400, 400, 220}}};
    EXPECT_FALSE(T::PickFlame(none.data(), none.size(), 30020, 30029, ffx, dummy));
}

TEST(TorchLift, SettingsParse) {
    T::Settings& s = T::GetTorchSettings();
    const T::Settings saved = s;
    EXPECT_TRUE(T::ParseTorchSetting("torch_lift_raised_cm", "-20"));
    EXPECT_NEAR(s.raised_m, -0.20f, 1e-5f);
    EXPECT_TRUE(T::ParseTorchSetting("torch_lift_ffx", "8123"));
    EXPECT_EQ(s.ffx, 8123);
    EXPECT_TRUE(T::ParseTorchSetting("torch_lift_copies", "9"));
    EXPECT_EQ(s.copies, 3);
    EXPECT_FALSE(T::ParseTorchSetting("torch", "1"));
    s = saved;
}

} // namespace
