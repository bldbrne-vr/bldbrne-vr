// SPDX-License-Identifier: GPL-2.0-or-later
#include <limits>
#include <sstream>
#include "core/vr/quick_settings.h"
#include <gtest/gtest.h>
#include "core/vr/body_mounts.h"
#include "core/vr/holsters.h"

using namespace VR::Holsters;
namespace B = VR::Body;
namespace M = VR::BodyMounts;
namespace {
Layout Mounted() {
    Layout l{};
    l.valid = true;
    l.assignment = {1, 2, 3};
    l.centre = {P3{.16f, 1.0f, -.10f}, P3{-.16f, 1.0f, -.10f}, P3{-.035f, 1.3f, -.12f}};
    return l;
}
void Near(B::Vec3 a, B::Vec3 b) {
    EXPECT_LT(B::Length(a - b), .00001f);
}
} // namespace
TEST(Holsters, PickupUsesRenderedPositionsAndHysteresis) {
    Tracker t;
    const auto l = Mounted();
    auto hand = l.centre[0];
    hand.x += .095f;
    auto out = t.Follow(l, hand, true, .10f, 1);
    ASSERT_EQ(out.hover, 0);
    EXPECT_TRUE(out.entered);
    hand.x += .02f;
    out = t.Follow(l, hand, true, .10f, 1);
    EXPECT_EQ(out.hover, 0);
    EXPECT_FALSE(out.entered);
    hand.x += .03f;
    EXPECT_EQ(t.Follow(l, hand, true, .10f, 1).hover, -1);
}
TEST(Holsters, ReachingNeverFreezesMountAwayFromMovingBody) {
    Tracker t;
    auto l = Mounted();
    ASSERT_EQ(t.Follow(l, l.centre[0], true, .10f, 1).hover, 0);
    const auto old = l.centre[0];
    l.centre[0].x += .2f;
    const auto out = t.Follow(l, old, true, .10f, 1);
    EXPECT_FLOAT_EQ(out.centre[0].x, l.centre[0].x);
    EXPECT_EQ(out.hover, -1);
    EXPECT_EQ(t.Follow(l, l.centre[0], true, .10f, 1).hover, 0);
}
TEST(Holsters, DisabledAndUntrackedNeverExposeTargets) {
    Tracker t;
    auto l = Mounted();
    l.assignment[0] = 0;
    EXPECT_EQ(t.Follow(l, l.centre[0], true, .10f, 1).hover, -1);
    EXPECT_FALSE(t.Follow(l, l.centre[1], false, .10f, 1).valid);
    l.valid = false;
    EXPECT_FALSE(t.Follow(l, {}, true, .10f, 1).valid);
}
TEST(Holsters, StaleFutureInvalidAndRecenteredTargetsAreRejected) {
    Targets t{Mounted(), 1'000'000'000, 4};
    EXPECT_TRUE(Fresh(t, t.ns + 30'000'000, 4));
    EXPECT_FALSE(Fresh(t, t.ns - 1, 4));
    EXPECT_FALSE(Fresh(t, t.ns + 101'000'000, 4));
    EXPECT_FALSE(Fresh(t, t.ns, 5));
    t.layout.centre[0].x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(Fresh(t, t.ns, 4));
}
TEST(BodyMounts, ReferenceBoneAxesDoNotTurnTheCaseBackwards) {
    const auto bind = B::AxisAngle({0, 0, 1}, 1.3f);
    B::QsTransform bone{{0, 1, 0}, bind};
    const auto f = M::OfBone(bone, bind);
    Near(f.right, {-1, 0, 0});
    Near(f.up, {0, 1, 0});
    Near(f.back, {0, 0, 1});
    M::Pose pose{{f, f}, 0, true};
    auto right = M::Surface(pose, 0, 0, 0, 0), left = M::Surface(pose, 1, 0, 0, 0);
    EXPECT_LT(right.origin.x, 0);
    EXPECT_GT(left.origin.x, 0);
    EXPECT_LT(right.origin.z, 0);
    EXPECT_LT(left.origin.z, 0);
    EXPECT_GT(B::Dot(right.back, f.back), .8f);
}
TEST(BodyMounts, PelvisAndChestFollowTheirOwnBonesThroughLeanAndTurns) {
    for (float angle : {-.8f, 0.f, .8f, 2.f}) {
        M::Pose rest{};
        rest.frame[0].origin = {0, 1, 0};
        rest.frame[1].origin = {0, 1.4f, 0};
        const auto turn = B::AxisAngle(B::Normalize(B::Vec3{1, 2, .3f}), angle);
        const B::Vec3 shift{.3f, -.12f, .2f};
        M::Pose moved = rest;
        for (auto& f : moved.frame) {
            f.origin = B::Rotate(turn, f.origin) + shift;
            f.right = B::Rotate(turn, f.right);
            f.up = B::Rotate(turn, f.up);
            f.back = B::Rotate(turn, f.back);
        }
        for (int slot = 0; slot < 3; ++slot) {
            const auto a = M::Surface(rest, slot, .01f, .02f, -.01f);
            const auto b = M::Surface(moved, slot, .01f, .02f, -.01f);
            Near(b.origin, B::Rotate(turn, a.origin) + shift);
            Near(b.back, B::Rotate(turn, a.back));
            EXPECT_NEAR(B::Dot(b.right, b.up), 0, .00001f);
        }
        moved = rest;
        moved.frame[1].origin.x += .15f;
        Near(M::Surface(moved, 0, 0, 0, 0).origin, M::Surface(rest, 0, 0, 0, 0).origin);
        EXPECT_NEAR(M::Surface(moved, 2, 0, 0, 0).origin.x - M::Surface(rest, 2, 0, 0, 0).origin.x,
                    .15f, .00001f);
    }
}
TEST(BodyMounts, ActualModelBackFaceSeatsFlushAcrossItemDepthsAndScales) {
    const B::Vec3 surface{.2f, 1.2f, -.1f}, back = B::Normalize(B::Vec3{-.5f, 0, 1});
    for (float scale : {.75f, 1.f, 1.3f})
        for (float depth : {.006f, .053f, .064f}) {
            const auto centre = M::Seat(surface, back, depth * scale, .002f * scale);
            Near(centre + back * (depth * scale), surface + back * (.002f * scale));
        }
}

TEST(BodyMounts, BeltAndPocketAreBelowPelvisAndChestStaysOnTorso) {
    M::Pose pose{};
    pose.frame[0].origin = {0, 1, 0};
    pose.frame[1].origin = {0, 1.4f, 0};
    const auto vial = M::Surface(pose, 0, 0, 0, 0);
    const auto pocket = M::Surface(pose, 1, 0, 0, 0);
    const auto chest = M::Surface(pose, 2, 0, 0, 0);
    EXPECT_LT(vial.origin.y, pose.frame[0].origin.y);
    EXPECT_LT(pocket.origin.y, vial.origin.y);
    EXPECT_GT(pocket.origin.x, -vial.origin.x);
    EXPECT_GT(chest.origin.y, pose.frame[0].origin.y);
    EXPECT_GT(chest.origin.x, 0);
    // A height adjustment translates all interaction/visual mount origins equally.
    for (int i=0;i<3;++i) {
        auto normal = M::Surface(pose,i,0,0,0);
        auto raised = M::Surface(pose,i,.03f,0,0);
        EXPECT_NEAR(raised.origin.y-normal.origin.y,.03f,.00001f);
    }
}

TEST(Holsters, InventoryOwnsAssignmentsAndLegacySettingsCannotMoveVials) {
    const auto before = Config{}.assignment;
    for (const char* key : {"holster_right", "holster_left", "holster_chest"})
        for (const char* value : {"0", "1", "3", "7"})
            EXPECT_TRUE(VR::Quick::ParseQuickSetting(key, value));
    EXPECT_EQ(Config{}.assignment, before);
    EXPECT_EQ(before, (std::array<int, 3>{1, 2, 3}));
    std::ostringstream defaults;
    VR::Quick::WriteQuickDefaults(defaults);
    EXPECT_EQ(defaults.str().find("holster_right="), std::string::npos);
    EXPECT_EQ(defaults.str().find("holster_left="), std::string::npos);
    EXPECT_EQ(defaults.str().find("holster_chest="), std::string::npos);
}
TEST(BodyMounts, PocketPlacementIsUnchanged) {
    const auto pocket = M::Surface({}, 1, 0, 0, 0);
    Near(pocket.origin, {.160f, -.145f, -.067f});
}

TEST(BodyMounts, RootTransformAndBonesStayTogetherBetweenPoseUpdates) {
    M::Pose pose{};
    pose.frame[0].origin = {0, 1, 0};
    pose.frame[1].origin = {0, 1.4f, 0};
    B::ChrFrame character{};
    ASSERT_TRUE(B::ChrFrameFromRows({1,0,0,2, 0,1,0,3, 0,0,1,4}, character));
    pose.character = character;
    const auto local = M::Surface(pose, 2, 0, 0, 0);
    const auto initial = M::WorldSurface(pose, 2, 0, 0, 0);
    Near(initial.origin, local.origin + B::Vec3{2,3,4});
    // Locomotion advances ChrCtrl before the game's next pose/skinning update.
    ASSERT_TRUE(B::ChrFrameFromRows({0,0,1,2.05f, 0,1,0,3, -1,0,0,4.03f}, character));
    Near(M::WorldSurface(pose, 2, 0, 0, 0).origin, initial.origin);
    Near(M::WorldSurface(pose, 2, 0, 0, 0).back, initial.back);
    // Once the body receives that update, all mounts follow together, without smoothing.
    pose.character = character;
    for (int slot=0; slot<3; ++slot) {
        const auto model = M::Surface(pose, slot, 0, 0, 0);
        const auto world = M::WorldSurface(pose, slot, 0, 0, 0);
        Near(world.origin, B::ToWorld(character, model.origin));
        Near(world.back, B::RotateToWorld(character, model.back));
    }
}

TEST(BodyMounts, HunterPouchClearsCoatAndFollowsTheMountedBody) {
    for (const int armor : {71000, 281000}) {
        for (const float scale : {.75f, 1.f, 1.3f}) {
            for (const float angle : {0.f, .7f, 2.f}) {
                const auto turn = B::AxisAngle(B::Normalize(B::Vec3{1, 2, .3f}), angle);
                auto surface = M::Surface({}, 0, 0, 0, 0);
                surface.origin = B::Rotate(turn, surface.origin) + B::Vec3{2, 3, 4};
                surface.right = B::Rotate(turn, surface.right);
                surface.up = B::Rotate(turn, surface.up);
                surface.back = B::Rotate(turn, surface.back);
                const auto fit = M::FitVialPouch(surface, armor, scale);
                const auto centre = M::Seat(fit.origin, fit.back, .064f * scale, .002f * scale);
                const auto back = centre + fit.back * (.064f * scale);
                const auto coat = surface.origin - surface.back * (.05870f * scale);
                EXPECT_GT(B::Dot(coat - back, surface.back), .004f * scale);
                EXPECT_LT(B::Length(fit.origin - surface.origin), .07f * scale);
                EXPECT_NEAR(B::Dot(fit.origin - surface.origin, surface.up), 0, .000001f);
                EXPECT_NEAR(B::Dot(fit.origin - surface.origin, surface.right), 0, .000001f);
                Near(fit.back, surface.back);
                Near(fit.right, surface.right);
                Near(fit.up, surface.up);
            }
        }
    }
}

TEST(BodyMounts, ShortTopsAndUnknownArmorKeepTheirPouchFit) {
    const auto surface = M::Surface({}, 0, .02f, .01f, -.01f);
    for (const int armor : {-1, 0, 41000, 221000, 231000, 321000}) {
        const auto fit = M::FitVialPouch(surface, armor, 1.3f);
        Near(fit.origin, surface.origin);
        Near(fit.right, surface.right);
        Near(fit.up, surface.up);
        Near(fit.back, surface.back);
    }
}

TEST(BodyMounts, ArmorFitsAreDistinctAndMoveWithThePelvisAtEveryScale) {
    EXPECT_LT(M::PouchOutwardM(231000), M::PouchOutwardM(71000));
    EXPECT_LT(M::PouchOutwardM(71000), M::PouchOutwardM(61000));
    EXPECT_LT(M::PouchOutwardM(61000), M::PouchOutwardM(121000));
    int previous = 0;
    for (const auto& fit : M::ArmorPouchFits) {
        EXPECT_GT(fit.armor, previous);
        previous = fit.armor;
        EXPECT_GE(fit.outward_m, 0.f);
        EXPECT_LE(fit.outward_m, .2f);
        for (const float scale : {.75f, 1.f, 1.3f}) {
            const auto turn = B::AxisAngle(B::Normalize(B::Vec3{.5f, 1, .2f}), 1.1f);
            auto surface = M::Surface({}, 0, .01f, -.02f, .01f);
            const auto base = M::FitVialPouch(surface, fit.armor, scale);
            const B::Vec3 shift{2, 3, 4};
            surface.origin = B::Rotate(turn, surface.origin) + shift;
            surface.right = B::Rotate(turn, surface.right);
            surface.up = B::Rotate(turn, surface.up);
            surface.back = B::Rotate(turn, surface.back);
            const auto moved = M::FitVialPouch(surface, fit.armor, scale);
            Near(moved.origin, B::Rotate(turn, base.origin) + shift);
            Near(moved.right, surface.right);
            Near(moved.up, surface.up);
            Near(moved.back, surface.back);
        }
    }
}

TEST(Holsters, ChangingArmorKeepsTheGrabOnTheVisiblePouch) {
    const auto surface = M::Surface({}, 0, 0, 0, 0);
    const auto centre = [&](int armor) {
        const auto fit = M::FitVialPouch(surface, armor, 1.f);
        const auto p = M::Seat(fit.origin, fit.back, .064f, .002f);
        return P3{p.x, p.y, p.z};
    };
    const auto shirt = centre(231000);
    const auto robe = centre(121000);
    Layout layout{};
    layout.valid = true;
    layout.assignment = {1, 0, 0};
    layout.centre[0] = robe;
    Tracker tracker;
    EXPECT_EQ(tracker.Follow(layout, shirt, true, .06f, 1.f).hover, -1);
    EXPECT_EQ(tracker.Follow(layout, robe, true, .06f, 1.f).hover, 0);
    layout.centre[0] = shirt;
    EXPECT_EQ(tracker.Follow(layout, robe, true, .06f, 1.f).hover, -1);
    EXPECT_EQ(tracker.Follow(layout, shirt, true, .06f, 1.f).hover, 0);
}

TEST(Holsters, QuickPocketAndChestGainFifteenMillimetresWithoutExpandingVials) {
    for (int target = 0; target < 3; ++target) {
        auto layout = Mounted();
        auto hand = layout.centre[target];
        hand.x += .114f;
        Tracker tracker;
        EXPECT_EQ(tracker.Follow(layout, hand, true, .10f, 1.f).hover, target == 0 ? -1 : target);
        tracker.Reset();
        hand.x += .002f;
        EXPECT_EQ(tracker.Follow(layout, hand, true, .10f, 1.f).hover, -1);
    }
}
