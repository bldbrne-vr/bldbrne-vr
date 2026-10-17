// SPDX-License-Identifier: GPL-2.0-or-later

// The held vial's policy (core/vr/held_item.h): the request word's round trip and age, every
// suppression, the whole animation class table (held and not), the arm gate's hard reasons, the
// left weapon hide masks and the private effect slot rotation.

#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

#include "core/vr/held_item.h"

namespace {

namespace H = VR::Held;
namespace B = VR::Body;

H::Inputs Armed() {
    H::Inputs in{};
    in.request = {H::Kind::Vial, 0, 1000};
    in.age_ms = 5;
    in.hp = 500;
    return in;
}

TEST(HeldItem, RequestRoundTrip) {
    for (const H::Kind kind : {H::Kind::None, H::Kind::Vial}) {
        for (const std::uint8_t flags : {std::uint8_t{0}, H::FlagGun, std::uint8_t{0xFF}}) {
            for (const std::int64_t ms : {std::int64_t{0}, std::int64_t{1}, std::int64_t{123456789},
                                          static_cast<std::int64_t>(H::MsMask)}) {
                const H::Request r = H::UnpackRequest(H::PackRequest({kind, flags, ms}));
                EXPECT_EQ(r.kind, kind);
                EXPECT_EQ(r.flags, flags);
                EXPECT_EQ(r.ms, ms);
            }
        }
    }
    // A steady clock past 48 bits of ms keeps its low bits.
    const std::int64_t big = (std::int64_t{1} << 50) + 77;
    EXPECT_EQ(H::UnpackRequest(H::PackRequest({H::Kind::Vial, 0, big})).ms, 77);
    EXPECT_EQ(H::UnpackRequest(0).kind, H::Kind::None); // nothing published yet
}

TEST(HeldItem, RequestAge) {
    const H::Request r{H::Kind::Vial, 0, 5000};
    EXPECT_EQ(H::RequestAgeMs(r, 5000), 0);
    EXPECT_EQ(H::RequestAgeMs(r, 5120), 120);
    EXPECT_EQ(H::RequestAgeMs(r, 4999), -1); // stamped just after the game read its clock
    // Across the 48-bit wrap of the stamp.
    const std::int64_t top = static_cast<std::int64_t>(H::MsMask);
    const H::Request late{H::Kind::Vial, 0, top - 10};
    EXPECT_EQ(H::RequestAgeMs(late, top + 1 + 20), 31);
    EXPECT_EQ(H::RequestAgeMs({H::Kind::Vial, 0, 30}, top - 9), -40);
}

TEST(HeldItem, NotArmedOrStale) {
    H::Inputs in = Armed();
    in.request.kind = H::Kind::None;
    H::Decision d = H::Decide(in, {});
    EXPECT_FALSE(d.requested);
    EXPECT_FALSE(d.show);
    EXPECT_EQ(d.hide_left_mask, 0);

    in = Armed();
    for (const std::int64_t age : {std::int64_t{0}, std::int64_t{250}, std::int64_t{-250}}) {
        in.age_ms = age;
        EXPECT_TRUE(H::Decide(in, {}).show) << age;
    }
    for (const std::int64_t age : {std::int64_t{251}, std::int64_t{-251}, std::int64_t{100000}}) {
        in.age_ms = age;
        d = H::Decide(in, {});
        EXPECT_FALSE(d.requested) << age;
        EXPECT_FALSE(d.show) << age;
        EXPECT_EQ(std::string_view{d.reason}, "request stale");
    }
}

TEST(HeldItem, ShownHidesTheLeftWeapon) {
    const H::Inputs in = Armed();
    H::Policy p{};
    p.hide_left = 1;
    H::Decision d = H::Decide(in, p);
    EXPECT_TRUE(d.requested);
    EXPECT_TRUE(d.show);
    EXPECT_EQ(std::string_view{d.reason}, "");
    EXPECT_EQ(d.hide_left_mask, 0x01); // the vanilla clip's byte: left part 0
    p.hide_left = 0;
    EXPECT_EQ(H::Decide(in, p).hide_left_mask, 0x00);
    p.hide_left = 2;
    EXPECT_EQ(H::Decide(in, p).hide_left_mask, 0x0F); // never a right-hand byte
}

TEST(HeldItem, HardSuppressions) {
    struct Case {
        const char* name;
        void (*set)(H::Inputs&);
        std::string_view reason;
    };
    const Case cases[] = {
        {"dead", [](H::Inputs& in) { in.hp = 0; }, "dead"},
        {"cinematic", [](H::Inputs& in) { in.cinematic = true; }, "cinematic"},
        {"menu", [](H::Inputs& in) { in.menu = true; }, "menu"},
        {"gun", [](H::Inputs& in) { in.request.flags = H::FlagGun; }, "gun"},
        {"item use", [](H::Inputs& in) { in.item_use_state = 3; }, "item use"},
    };
    for (const Case& c : cases) {
        for (const bool gate : {false, true}) {
            H::Inputs in = Armed();
            in.gate_valid = gate;
            c.set(in);
            const H::Decision d = H::Decide(in, {});
            EXPECT_TRUE(d.requested) << c.name;
            EXPECT_FALSE(d.show) << c.name;
            EXPECT_EQ(d.hide_left_mask, 0) << c.name; // the gun comes back
            EXPECT_EQ(std::string_view{d.reason}, c.reason) << c.name;
        }
    }
}

TEST(HeldItem, GateReasons) {
    struct Case {
        B::GateReason reason;
        bool show;
    };
    for (const Case c : {Case{B::GateReason::None, true}, Case{B::GateReason::Anim, true},
                         Case{B::GateReason::Dead, false}, Case{B::GateReason::Throw, false},
                         Case{B::GateReason::Ladder, false}, Case{B::GateReason::Event, false}}) {
        H::Inputs in = Armed();
        in.gate_valid = true;
        in.reason = c.reason;
        EXPECT_EQ(H::Decide(in, {}).show, c.show) << B::GateReasonName(c.reason);
        // Without a gate (no body hook) the reason is not known: shown.
        in.gate_valid = false;
        EXPECT_TRUE(H::Decide(in, {}).show) << B::GateReasonName(c.reason);
    }
}

TEST(HeldItem, ClassTable) {
    struct Case {
        B::AnimClass owner;
        bool show;            // held, two-handed attacks hidden (the default)
        bool show_two_handed; // held, vial_hide_two_handed=0
    };
    const Case cases[] = {
        {B::AnimClass::Idle, true, true},        {B::AnimClass::Locomotion, true, true},
        {B::AnimClass::Dodge, true, true},       {B::AnimClass::FallJump, true, true},
        {B::AnimClass::Grabbed, false, false},   {B::AnimClass::GuardReaction, true, true},
        {B::AnimClass::WeaponSwitch, true, true}, {B::AnimClass::Unverified, true, true},
        {B::AnimClass::Transform, true, true},   {B::AnimClass::Damage, true, true},
        {B::AnimClass::Death, false, false},     {B::AnimClass::AttackRight, true, true},
        {B::AnimClass::AttackBoth, false, true}, {B::AnimClass::AttackLeft, false, false},
        {B::AnimClass::ItemUse, false, false},   {B::AnimClass::Event, false, false},
        {B::AnimClass::Pickup, true, true},      {B::AnimClass::Gesture, false, false},
        {B::AnimClass::Unknown, true, true},
    };
    static_assert(sizeof(cases) / sizeof(cases[0]) == B::AnimClassCount);
    for (const Case& c : cases) {
        H::Inputs in = Armed();
        in.gate_valid = true;
        in.reason = B::GateReason::Anim;
        in.owner = c.owner;
        in.held = true;
        H::Policy p{};
        EXPECT_EQ(H::Decide(in, p).show, c.show) << B::AnimClassName(c.owner);
        p.hide_two_handed = false;
        EXPECT_EQ(H::Decide(in, p).show, c.show_two_handed) << B::AnimClassName(c.owner);
        // The last move that held the arms, no longer playing: shown.
        in.held = false;
        EXPECT_TRUE(H::Decide(in, p).show) << B::AnimClassName(c.owner);
        // Not shown means not requested this update and nothing hidden.
        in.held = true;
        const H::Decision d = H::Decide(in, {});
        if (!d.show) {
            EXPECT_TRUE(d.requested);
            EXPECT_EQ(d.hide_left_mask, 0);
        }
    }
}

TEST(HeldItem, SlotRotation) {
    EXPECT_EQ(H::NextVialSlot(H::FirstVialSlot), H::FirstVialSlot + 1);
    EXPECT_EQ(H::NextVialSlot(H::FirstVialSlot + H::VialSlotCount - 1), H::FirstVialSlot);
    EXPECT_EQ(H::NextVialSlot(-1), H::FirstVialSlot);
    EXPECT_EQ(H::NextVialSlot(2989), H::FirstVialSlot); // the node constructor's 0xBAD
    std::int32_t slot = H::FirstVialSlot;
    for (int i = 0; i < 3 * H::VialSlotCount; ++i) {
        slot = H::NextVialSlot(slot);
        EXPECT_GE(slot, H::FirstVialSlot);
        EXPECT_LT(slot, H::FirstVialSlot + H::VialSlotCount);
        EXPECT_NE(slot, 2989);
    }
    EXPECT_EQ(slot, H::FirstVialSlot);
}

TEST(HeldItem, StateNames) {
    for (const H::State s : {H::State::Off, H::State::Pending, H::State::Shown,
                             H::State::Suppressed, H::State::Failed, H::State::Unavailable}) {
        EXPECT_NE(std::string_view{H::StateName(s)}, "?");
    }
}

} // namespace
