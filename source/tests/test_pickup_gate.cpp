// SPDX-License-Identifier: GPL-2.0-or-later

// Instant item pickups (core/vr/pickup_gate.h): when a pickup leaves the game's animation out
// (pickup_instant on, VR shown in first person) and when it keeps it, and the log text for the
// lot's items.

#include <array>
#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "core/vr/pickup_gate.h"

namespace {

namespace P = VR::Pickup;

TEST(PickupGate, InstantInFirstPersonVr) {
    EXPECT_EQ(P::Decide(P::GateInputs{}), P::Reason::Instant);
}

TEST(PickupGate, SettingOffKeepsTheAnimation) {
    P::GateInputs in{};
    in.setting = false;
    EXPECT_EQ(P::Decide(in), P::Reason::SettingOff);
    // The setting wins over every other state.
    in.vr_enabled = false;
    in.flat = true;
    EXPECT_EQ(P::Decide(in), P::Reason::SettingOff);
}

TEST(PickupGate, EveryOtherStateKeepsTheAnimation) {
    {
        P::GateInputs in{};
        in.vr_enabled = false;
        EXPECT_EQ(P::Decide(in), P::Reason::VrOff);
    }
    {
        P::GateInputs in{};
        in.display_auto = false;
        EXPECT_EQ(P::Decide(in), P::Reason::ScreenChosen);
    }
    {
        P::GateInputs in{};
        in.first_person = false;
        EXPECT_EQ(P::Decide(in), P::Reason::ThirdPerson);
    }
    {
        P::GateInputs in{};
        in.flat = true;
        EXPECT_EQ(P::Decide(in), P::Reason::Flat);
    }
    {
        P::GateInputs in{};
        in.immersive = false;
        EXPECT_EQ(P::Decide(in), P::Reason::NotImmersive);
    }
}

TEST(PickupGate, AllCombinations) {
    // Instant exactly when the setting is on, VR runs in Auto mode, first person, no flat period
    // and VR frames are shown (64 combinations).
    int instant = 0;
    for (int m = 0; m < 64; ++m) {
        const P::GateInputs in{
            .setting = (m & 1) != 0,
            .vr_enabled = (m & 2) != 0,
            .display_auto = (m & 4) != 0,
            .immersive = (m & 8) != 0,
            .first_person = (m & 16) != 0,
            .flat = (m & 32) != 0,
        };
        const bool expect = in.setting && in.vr_enabled && in.display_auto && in.immersive &&
                            in.first_person && !in.flat;
        EXPECT_EQ(P::Decide(in) == P::Reason::Instant, expect) << "mask " << m;
        instant += P::Decide(in) == P::Reason::Instant ? 1 : 0;
        EXPECT_NE(std::string{P::ReasonText(P::Decide(in))}, "?");
    }
    EXPECT_EQ(instant, 1);
}

TEST(PickupGate, ReasonTexts) {
    EXPECT_EQ(std::string{P::ReasonText(P::Reason::Instant)}, "instant: no pickup animation");
    EXPECT_NE(std::string{P::ReasonText(P::Reason::SettingOff)}.find("pickup_instant=0"),
              std::string::npos);
}

TEST(PickupGate, KindNames) {
    EXPECT_EQ(std::string{P::KindName(P::Kind::Drop)}, "enemy drop");
    EXPECT_EQ(std::string{P::KindName(P::Kind::World)}, "world item");
    EXPECT_EQ(std::string{P::KindName(P::Kind::Treasure)}, "treasure");
}

TEST(PickupGate, ItemTexts) {
    EXPECT_EQ(P::ItemText(0x40000000u | 1000u), "Blood Vial (1000)");
    EXPECT_EQ(P::ItemText(0x40000000u | 900u), "Quicksilver Bullets (900)");
    EXPECT_EQ(P::ItemText(0x40000000u | 4321u), "goods 4321");
    EXPECT_EQ(P::ItemText(7000000u), "weapon 7000000");
    EXPECT_EQ(P::ItemText(0x10000000u | 220000u), "armour 220000");
    EXPECT_EQ(P::ItemText(0x20000000u | 5000u), "rune 5000");
    EXPECT_EQ(P::ItemText(0x80000000u | 12u), "item 0x8000000c");
    EXPECT_EQ(P::ItemText(0xFFFFFFFFu), "none");
    EXPECT_EQ(P::ItemText(0x4FFFFFFFu), "none");
    EXPECT_TRUE(P::IsNoItem(0x0FFFFFFFu));
    EXPECT_FALSE(P::IsNoItem(0x40000000u | 1000u));
}

} // namespace
