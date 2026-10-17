// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>
#include <limits>
#include <utility>

#include "core/vr/lever_channel.h"
#include "core/vr/lever_grip.h"
#include "core/vr/lever_models.h"
#include "core/vr/lever_acquisition.h"
#include "core/vr/hand_grip.h"
#include "core/vr/lever_pose_edit.h"
#include "core/vr/lever_space.h"
#include "core/vr/lever_update_queue.h"
#include "core/vr/physical_lever.h"

namespace {
namespace L = VR::Lever;
using VR::Body::Vec3;

L::Input Frame() {
    L::Input in{};
    in.enabled = true;
    in.gameplay = true;
    in.dt = 1.0f / 90.0f;
    in.candidate = {true, 42, 7, 1, 0.0f, {{}, {1, 0, 0}, {0, 0.5f, 0}, 1.2f}, 0.0f};
    for (auto &hand : in.hands) {
        hand.tracked = true;
        hand.grip_valid = true;
        hand.position = {0, 0.5f, 0};
    }
    return in;
}

L::Output Grab(L::Tracker &t, L::Input &in, int hand = 0) {
    t.Step(in); // a real release must be observed before a squeeze
    in.hands[hand].grip = 1;
    return t.Step(in);
}

L::Output Move(L::Tracker &t, L::Input &in, float angle, int hand = 0) {
    const float start = t.Angle();
    L::Output out{};
    for (int i = 1; i <= 30; ++i) {
        in.hands[hand].position =
            L::HandleAt(in.candidate.rig, start + (angle - start) * i / 30.0f);
        out = t.Step(in);
    }
    return out;
}

TEST(PhysicalLever, HingeGeometryRotatesInEitherDirection) {
    auto in = Frame();
    auto p = L::HandleAt(in.candidate.rig, VR::Body::Pi / 2);
    EXPECT_NEAR(p.y, 0, 1e-6f);
    EXPECT_NEAR(p.z, 0.5f, 1e-6f);
    in.candidate.rig.sweep = -1.2f;
    EXPECT_LT(L::HandleAt(in.candidate.rig, in.candidate.rig.sweep).z, 0);
}

TEST(PhysicalLever, EitherHandGrabsWithoutAnAngleJump) {
    for (int hand = 0; hand < 2; ++hand) {
        auto in = Frame();
        L::Tracker t;
        in.candidate.angle = 0.3f;
        in.hands[hand].position = L::HandleAt(in.candidate.rig, 0.4f);
        const auto o = Grab(t, in, hand);
        EXPECT_TRUE(o.grabbed);
        EXPECT_EQ(o.hand, hand);
        EXPECT_TRUE(o.owns_grip[hand]);
        EXPECT_FLOAT_EQ(o.angle, 0.3f);
        EXPECT_NEAR(o.progress, 0.25f, 1e-6f);
    }
}

TEST(PhysicalLever, ForgivingGrabZonePinsToHandleAndRejectsOutsideReach) {
    for (int hand = 0; hand < 2; ++hand) {
        auto in = Frame();
        L::Tracker tracker;
        const auto handle = L::HandleAt(in.candidate.rig, 0);
        in.hands[hand].position = handle + Vec3{0.20f, 0, 0};
        const auto grabbed = Grab(tracker, in, hand);
        ASSERT_TRUE(grabbed.holding);
        EXPECT_TRUE(grabbed.grabbed);
        EXPECT_NEAR(VR::Body::Length(grabbed.pin - handle), 0, 1e-6f);
        EXPECT_FLOAT_EQ(grabbed.angle, 0);
        auto outside = Frame();
        L::Tracker other;
        outside.hands[hand].position = handle + Vec3{0.23f, 0, 0};
        EXPECT_FALSE(Grab(other, outside, hand).holding);
    }
}

TEST(PhysicalLever, MovesContinuouslyAndReversesBeforeCompletion) {
    auto in = Frame();
    L::Tracker t;
    ASSERT_TRUE(Grab(t, in).grabbed);
    auto o = Move(t, in, 0.6f);
    EXPECT_TRUE(o.holding);
    EXPECT_FALSE(o.completed);
    EXPECT_NEAR(o.progress, 0.5f, 1e-5f);
    EXPECT_NEAR(o.pin.z, L::HandleAt(in.candidate.rig, 0.6f).z, 1e-5f);
    o = Move(t, in, 0.2f);
    EXPECT_NEAR(o.angle, 0.2f, 1e-5f);
    EXPECT_NEAR(o.progress, 1.0f / 6, 1e-5f);
}

TEST(PhysicalLever, HeldGripNeverGrabsOnStartupOrApproach) {
    auto in = Frame();
    L::Tracker t;
    in.hands[0].grip = 1;
    EXPECT_FALSE(t.Step(in).grabbed);
    in.hands[0].grip = 0;
    t.Step(in);
    in.hands[0].position = {0, 2, 0};
    in.hands[0].grip = 1;
    EXPECT_FALSE(t.Step(in).grabbed);
    in.hands[0].position = {0, 0.5f, 0};
    EXPECT_FALSE(t.Step(in).grabbed);
    in.hands[0].grip = 0;
    t.Step(in);
    in.hands[0].grip = 1;
    EXPECT_TRUE(t.Step(in).grabbed);
}

TEST(PhysicalLever, GrabOffsetIsPreservedDuringMotion) {
    auto in = Frame();
    L::Tracker t;
    in.hands[0].position = L::HandleAt(in.candidate.rig, 0.15f);
    ASSERT_TRUE(Grab(t, in).grabbed);
    L::Output o{};
    for (int i = 1; i <= 30; ++i) {
        in.hands[0].position = L::HandleAt(in.candidate.rig, 0.15f + i * 0.01f);
        o = t.Step(in);
    }
    EXPECT_NEAR(o.angle, 0.3f, 1e-5f);
}

TEST(PhysicalLever, HingeAxialMotionDoesNotPull) {
    auto in = Frame();
    L::Tracker t;
    Grab(t, in);
    for (int i = 1; i <= 20; ++i) {
        in.hands[0].position.x = i * 0.01f;
        const auto o = t.Step(in);
        EXPECT_FLOAT_EQ(o.angle, 0);
        EXPECT_FALSE(o.completed);
    }
}

TEST(PhysicalLever, LetGoAtPartialPullDoesNotComplete) {
    auto in = Frame();
    L::Tracker t;
    Grab(t, in);
    Move(t, in, 0.6f);
    in.hands[0].grip = 0;
    const auto o = t.Step(in);
    EXPECT_EQ(o.ended, L::End::Released);
    EXPECT_FALSE(o.holding);
    EXPECT_FALSE(o.completed);
    EXPECT_FALSE(o.owns_grip[0]);
    EXPECT_FLOAT_EQ(o.strain, 0);
    EXPECT_NEAR(o.angle, 0.6f, 1e-5f);
}

TEST(PhysicalLever, SlipKeepsSqueezeOwnedUntilRelease) {
    auto in = Frame();
    L::Tracker t;
    Grab(t, in);
    L::Output o{};
    for (int i = 1; i <= 35; ++i) {
        in.hands[0].position.x = i * 0.01f;
        o = t.Step(in);
        if (o.ended == L::End::Slipped)
            break;
    }
    EXPECT_EQ(o.ended, L::End::Slipped);
    EXPECT_TRUE(o.owns_grip[0]);
    EXPECT_FALSE(o.holding);
    in.hands[0].position = {0, 0.5f, 0};
    EXPECT_FALSE(t.Step(in).grabbed);
    in.hands[0].grip = 0;
    EXPECT_FALSE(t.Step(in).owns_grip[0]);
}

TEST(PhysicalLever, CompletesOnceAtTheStopAndCannotRegrabTheSameOffer) {
    auto in = Frame();
    L::Tracker t;
    Grab(t, in);
    int completions = 0;
    for (int i = 1; i <= 125; ++i) {
        in.hands[0].position = L::HandleAt(in.candidate.rig, i * 0.01f);
        const auto o = t.Step(in);
        completions += o.completed;
        if (o.completed) {
            EXPECT_EQ(o.id, 42u);
            EXPECT_EQ(o.generation, 7u);
            EXPECT_FLOAT_EQ(o.progress, 1);
            EXPECT_FLOAT_EQ(o.strain, 0);
        }
    }
    EXPECT_EQ(completions, 1);
    in.hands[0].grip = 0;
    t.Step(in);
    in.hands[0].position = {0, 0.5f, 0};
    in.hands[0].grip = 1;
    EXPECT_FALSE(t.Step(in).grabbed);
    in.hands[0].grip = 0;
    t.Step(in);
    ++in.candidate.generation;
    in.hands[0].grip = 1;
    EXPECT_TRUE(t.Step(in).grabbed);
}

TEST(PhysicalLever, NegativeSweepFollowsHandAndCompletes) {
    auto in = Frame();
    in.candidate.rig.sweep = -1.2f;
    L::Tracker t;
    Grab(t, in);
    auto o = Move(t, in, -0.6f);
    EXPECT_NEAR(o.progress, 0.5f, 1e-5f);
    o = Move(t, in, -1.21f);
    EXPECT_NEAR(t.Angle(), -1.2f, 1e-5f);
}

TEST(PhysicalLever, RestStopHasNoAccumulatedSlack) {
    auto in = Frame();
    L::Tracker t;
    Grab(t, in);
    Move(t, in, -0.2f);
    EXPECT_FLOAT_EQ(t.Angle(), 0);
    in.hands[0].position = L::HandleAt(in.candidate.rig, -0.18f);
    EXPECT_NEAR(t.Step(in).angle, 0.02f, 1e-5f);
}

TEST(PhysicalLever, RatchetsOnlyAtNewProgressAndStrainTracksReversal) {
    auto in = Frame();
    L::Tracker t;
    Grab(t, in);
    int clicks = 0;
    for (int i = 1; i <= 60; ++i) {
        in.hands[0].position = L::HandleAt(in.candidate.rig, i * 0.01f);
        clicks += t.Step(in).ratchet;
    }
    EXPECT_GT(clicks, 0);
    for (int i = 0; i < 20; ++i)
        EXPECT_FALSE(t.Step(in).ratchet);
    const auto back = Move(t, in, 0.2f);
    EXPECT_LT(back.strain, 0.3f);
    EXPECT_FALSE(Move(t, in, 0.5f).ratchet);
}

TEST(PhysicalLever, CancellationReasonsNeverCompleteOrLeakOwnedGrip) {
    for (int cause = 0; cause < 11; ++cause) {
        auto in = Frame();
        L::Tracker t;
        Grab(t, in);
        switch (cause) {
        case 0:
            in.enabled = false;
            break;
        case 1:
            in.gameplay = false;
            break;
        case 2:
            in.candidate.eligible = false;
            break;
        case 3:
            in.candidate.age_s = 1;
            break;
        case 4:
            ++in.candidate.id;
            break;
        case 5:
            ++in.candidate.generation;
            break;
        case 6:
            ++in.candidate.space_epoch;
            break;
        case 7:
            in.hands[0].tracked = false;
            break;
        case 8:
            in.hands[0].busy = true;
            break;
        case 9:
            in.fallback = true;
            break;
        case 10:
            in.dt = 1;
            break;
        }
        const auto o = t.Step(in);
        EXPECT_FALSE(o.holding) << cause;
        EXPECT_FALSE(o.completed) << cause;
        EXPECT_TRUE(o.owns_grip[0]) << cause;
        EXPECT_FLOAT_EQ(o.strain, 0);
    }
}

TEST(PhysicalLever, TrackingLossCannotManufactureAReleaseOrRegrab) {
    auto in = Frame();
    L::Tracker t;
    Grab(t, in);
    in.hands[0].grip_valid = false;
    in.hands[0].grip = 0;
    EXPECT_TRUE(t.Step(in).owns_grip[0]);
    in.hands[0].grip_valid = true;
    in.hands[0].grip = 1;
    EXPECT_FALSE(t.Step(in).grabbed);
    in.hands[0].grip = 0;
    EXPECT_FALSE(t.Step(in).owns_grip[0]);
}

TEST(PhysicalLever, IneligibleStaleBusyOrMalformedCandidatesCannotGrab) {
    for (int cause = 0; cause < 12; ++cause) {
        auto in = Frame();
        L::Tracker t;
        switch (cause) {
        case 0:
            in.candidate.eligible = false;
            break;
        case 1:
            in.candidate.age_s = 1;
            break;
        case 2:
            in.candidate.id = 0;
            break;
        case 3:
            in.candidate.rig.axis = {};
            break;
        case 4:
            in.candidate.rig.rest = {};
            break;
        case 5:
            in.candidate.rig.sweep = 0;
            break;
        case 6:
            in.candidate.rig.sweep = 4;
            break;
        case 7:
            in.hands[0].busy = true;
            break;
        case 8:
            in.hands[0].blocked = true;
            break;
        case 9:
            in.candidate.angle = 4;
            break;
        case 10:
            in.candidate.age_s = -1;
            break;
        case 11:
            in.candidate.rig.pivot.x = std::numeric_limits<float>::quiet_NaN();
            break;
        }
        EXPECT_FALSE(Grab(t, in).grabbed) << cause;
    }
}

TEST(PhysicalLever, GripHysteresisAvoidsChattering) {
    auto in = Frame();
    L::Tracker t;
    Grab(t, in);
    in.hands[0].grip = 0.6f;
    EXPECT_TRUE(t.Step(in).holding);
    in.hands[0].grip = 0.45f;
    EXPECT_EQ(t.Step(in).ended, L::End::Released);
    in.hands[0].grip = 0.7f;
    EXPECT_FALSE(t.Step(in).grabbed);
    in.hands[0].grip = 0.75f;
    EXPECT_TRUE(t.Step(in).grabbed);
}

TEST(PhysicalLever, SimultaneousSqueezesChooseClosestHandAndNeverSwitch) {
    auto in = Frame();
    L::Tracker t;
    in.hands[0].position.x = 0.08f;
    t.Step(in);
    in.hands[0].grip = in.hands[1].grip = 1;
    auto o = t.Step(in);
    EXPECT_EQ(o.hand, 1);
    EXPECT_FALSE(o.owns_grip[0]);
    in.hands[1].grip = 0;
    o = t.Step(in);
    EXPECT_FALSE(o.holding);
    EXPECT_FALSE(t.Step(in).grabbed);
}

TEST(PhysicalLever, RecenterJumpAndChangedRigCancelRatherThanPull) {
    for (int cause = 0; cause < 3; ++cause) {
        auto in = Frame();
        L::Tracker t;
        Grab(t, in);
        if (cause == 0)
            in.hands[0].position = {0, 0, 0.5f};
        if (cause == 1)
            in.candidate.rig.pivot.x = 1;
        if (cause == 2)
            in.candidate.rig.sweep = 0.7f;
        const auto o = t.Step(in);
        EXPECT_FALSE(o.holding);
        EXPECT_FALSE(o.completed);
        EXPECT_TRUE(o.owns_grip[0]);
    }
}

TEST(PhysicalLever, NonfiniteHandOrClockNeverProducesMotion) {
    for (int cause = 0; cause < 4; ++cause) {
        auto in = Frame();
        L::Tracker t;
        Grab(t, in);
        const float nan = std::numeric_limits<float>::quiet_NaN();
        if (cause == 0)
            in.hands[0].position.z = nan;
        if (cause == 1)
            in.hands[0].grip = nan;
        if (cause == 2)
            in.dt = nan;
        if (cause == 3)
            in.dt = -1;
        const auto o = t.Step(in);
        EXPECT_FALSE(o.holding);
        EXPECT_FALSE(o.completed);
        EXPECT_TRUE(std::isfinite(o.angle));
    }
}

TEST(PhysicalLever, NewOfferDoesNotUseSqueezeHeldThroughMenu) {
    auto in = Frame();
    L::Tracker t;
    t.Step(in);
    in.gameplay = false;
    in.hands[0].grip = 1;
    t.Step(in);
    in.gameplay = true;
    EXPECT_FALSE(t.Step(in).grabbed);
}

TEST(PhysicalLever, FrameRateDoesNotChangeHandDrivenProgress) {
    for (int hz : {30, 60, 72, 90, 120, 144}) {
        auto in = Frame();
        in.dt = 1.0f / hz;
        L::Tracker t;
        Grab(t, in);
        L::Output o{};
        for (int frame = 1; frame <= hz; ++frame) {
            in.hands[0].position = L::HandleAt(in.candidate.rig, float(frame) / hz);
            o = t.Step(in);
        }
        EXPECT_TRUE(o.holding) << hz;
        EXPECT_NEAR(o.angle, 1.0f, 1e-5f) << hz;
        EXPECT_NEAR(o.progress, 1.0f / 1.2f, 1e-5f) << hz;
    }
}

TEST(PhysicalLever, TranslatedRotatedHingeHasTheSameResponse) {
    auto in = Frame();
    in.candidate.rig.pivot = {10, -2, 5};
    in.candidate.rig.axis = {0, 1, 0};
    in.candidate.rig.rest = {0.4f, 0.1f, 0};
    in.hands[0].position = L::HandleAt(in.candidate.rig, 0);
    L::Tracker t;
    Grab(t, in);
    const auto o = Move(t, in, 0.6f);
    EXPECT_NEAR(o.progress, 0.5f, 1e-5f);
    EXPECT_NEAR(o.pin.y, -1.9f, 1e-5f);
    EXPECT_LT(o.pin.z, 5);
}

TEST(PhysicalLever, NearHingeSingularitySlipsWithoutAnAngleSpike) {
    auto in = Frame();
    in.candidate.rig.rest = {0, 0.1f, 0};
    in.hands[0].position = {0, 0.1f, 0};
    L::Tracker t;
    ASSERT_TRUE(Grab(t, in).grabbed);
    L::Output o{};
    for (int i = 1; i <= 9; ++i) {
        in.hands[0].position.y = 0.1f - i * 0.01f;
        o = t.Step(in);
        if (!o.holding)
            break;
    }
    EXPECT_EQ(o.ended, L::End::Slipped);
    EXPECT_FALSE(o.completed);
    EXPECT_FLOAT_EQ(o.angle, 0);
}

TEST(PhysicalLever, ChangingTargetsRequiresAFreshSqueeze) {
    auto in = Frame();
    L::Tracker t;
    Grab(t, in);
    ++in.candidate.id;
    EXPECT_EQ(t.Step(in).ended, L::End::CandidateLost);
    EXPECT_FALSE(t.Step(in).grabbed);
    in.hands[0].grip = 0;
    t.Step(in);
    in.hands[0].grip = 1;
    EXPECT_TRUE(t.Step(in).grabbed);
}

TEST(PhysicalLever, NonOwnerCannotCompleteOrStealTheHold) {
    auto in = Frame();
    L::Tracker t;
    Grab(t, in);
    in.hands[1].grip = 1;
    for (int i = 1; i <= 60; ++i) {
        in.hands[1].position = L::HandleAt(in.candidate.rig, i * 0.02f);
        const auto o = t.Step(in);
        EXPECT_EQ(o.hand, 0);
        EXPECT_FLOAT_EQ(o.angle, 0);
        EXPECT_FALSE(o.completed);
        EXPECT_FALSE(o.owns_grip[1]);
    }
}
} // namespace

TEST(PhysicalLever, BoneTargetAndPinnedHandleAgreeAcrossObjectAndParentRotations) {
    L::BoneBinding b;
    b.parent_world = {{2, -3, 4}, VR::Body::AxisAngle({0, 1, 0}, 0.7f)};
    b.rest_local = {{0.1f, 0.3f, -0.2f}, VR::Body::AxisAngle({0, 0, 1}, -0.4f)};
    b.axis_local = {1, 0, 0};
    b.handle_local = {0.03f, 0.5f, 0.1f};
    for (float sweep : {-1.2f, 1.2f}) {
        b.sweep = sweep;
        const auto rig = L::BindRig(b);
        ASSERT_TRUE(rig);
        for (float fraction : {0.0f, 0.7f, 0.3f, 1.0f, 0.0f}) {
            const float angle = fraction * sweep;
            const auto target = L::BoneTarget(b, angle);
            ASSERT_TRUE(target);
            const auto world = VR::Body::Compose(b.parent_world, *target);
            const auto actual = world.t + VR::Body::Rotate(world.q, b.handle_local);
            EXPECT_LT(VR::Body::Length(actual - L::HandleAt(*rig, angle)), 1e-5f);
            EXPECT_LT(VR::Body::Length(target->t - b.rest_local.t), 1e-7f);
        }
    }
}

TEST(PhysicalLever, BoneBindingRejectsInvalidGeometryAndClampsMechanicalStops) {
    L::BoneBinding b{{}, {}, {1, 0, 0}, {0, 0.5f, 0}, 1.2f};
    ASSERT_TRUE(L::BindRig(b));
    auto target = L::BoneTarget(b, 3.0f);
    ASSERT_TRUE(target);
    EXPECT_NEAR(std::abs(VR::Body::Dot(target->q, VR::Body::AxisAngle({1, 0, 0}, 1.2f))), 1.0f,
                1e-6f);
    EXPECT_FALSE(L::BoneTarget(b, std::numeric_limits<float>::quiet_NaN()));
    b.parent_world.s.y = 2;
    EXPECT_FALSE(L::BindRig(b));
    b.parent_world.s.y = 1;
    b.rest_local.q = {0, 0, 0, 0};
    EXPECT_FALSE(L::BindRig(b));
    b.rest_local.q = {};
    b.handle_local = {0.5f, 0, 0};
    EXPECT_FALSE(L::BindRig(b));
}

TEST(PhysicalLever, TrackingBridgePreservesPullDirectionAndMetreScale) {
    for (float scale : {0.5f, 1.0f, 2.0f}) {
        VR::Body::Anchor a{};
        a.valid = true;
        a.steady_ns = 1'000'000'000;
        a.eye_world = {12, -3, 4};
        a.head_k = {0.1f, 1.6f, -0.2f};
        a.base_yaw = 0.9f;
        a.world_scale = scale;
        const auto space = L::Space::From(a, a.steady_ns);
        ASSERT_TRUE(space);
        L::Rig world{a.eye_world, {1, 0, 0}, {0, 0.5f * scale, 0}, 1.2f};
        world.shaft = true;
        world.shaft_start = {0, 0.2f * scale, 0};
        const auto rig = space->RigInMetres(world);
        ASSERT_TRUE(rig);
        EXPECT_NEAR(VR::Body::Length(rig->rest), 0.5f, 1e-6f);
        EXPECT_NEAR(VR::Body::Length(rig->shaft_start), 0.2f, 1e-6f);
        L::Input in = Frame();
        in.candidate.rig = *rig;
        in.hands[0].position = L::HandleAt(*rig, 0);
        L::Tracker tracker;
        ASSERT_TRUE(Grab(tracker, in).grabbed);
        L::Output out{};
        for (int i = 1; i <= 60; ++i) {
            const float angle = 0.6f * i / 60;
            const Vec3 game_point = L::HandleAt(world, angle);
            const Vec3 tracking =
                a.head_k + VR::Body::WorldToTracking(game_point - a.eye_world, a.base_yaw, scale);
            in.hands[0].position = space->HandInMetres(tracking);
            out = tracker.Step(in);
            EXPECT_TRUE(out.holding);
            EXPECT_NEAR(out.angle, angle, 1e-4f);
            EXPECT_LT(VR::Body::Length(space->PinInTracking(out.pin) - tracking), 1e-4f);
        }
    }
}

TEST(PhysicalLever, TrackingBridgeRejectsStaleFutureOrInvalidAnchors) {
    VR::Body::Anchor a{};
    a.valid = true;
    a.steady_ns = 1'000'000'000;
    EXPECT_TRUE(L::Space::From(a, a.steady_ns));
    EXPECT_FALSE(L::Space::From(a, a.steady_ns - 1));
    EXPECT_FALSE(L::Space::From(a, a.steady_ns + 100'000'001));
    a.world_scale = 0;
    EXPECT_FALSE(L::Space::From(a, a.steady_ns));
    a.world_scale = 1;
    a.eye_world.x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(L::Space::From(a, a.steady_ns));
}

TEST(PhysicalLever, ModelRefreshDoesNotDuplicateOrDiscardOtherQueuedWork) {
    std::array<std::uint16_t, 4> indices{2, 0xEEEE, 0xEEEE, 0xEEEE};
    std::uint32_t count = 1;
    std::uint16_t pending = 0x8041, active = 0xA020;
    EXPECT_EQ(L::RequestModelRefresh(1, 4, indices, count, pending, active),
              L::RefreshResult::Queued);
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(indices, (std::array<std::uint16_t, 4>{2, 1, 0xEEEE, 0xEEEE}));
    EXPECT_EQ(pending, 0x8141);
    EXPECT_EQ(active, 0xB020);
    for (int frame = 0; frame < 120; ++frame) {
        EXPECT_EQ(L::RequestModelRefresh(1, 4, indices, count, pending, active),
                  L::RefreshResult::AlreadyQueued);
    }
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(pending, 0x8141);
    EXPECT_EQ(active, 0xB020);
}

TEST(PhysicalLever, InvalidRefreshQueueNeverPartiallyMutatesStorage) {
    const auto reject = [](std::array<std::uint16_t, 4> indices, std::uint32_t count,
                           std::uint16_t active, std::uint32_t capacity = 4,
                           std::uint16_t index = 1) {
        const auto old_indices = indices;
        const auto old_count = count;
        const auto old_active = active;
        std::uint16_t pending = 0x8041;
        EXPECT_EQ(L::RequestModelRefresh(index, capacity, indices, count, pending, active),
                  L::RefreshResult::Invalid);
        EXPECT_EQ(indices, old_indices);
        EXPECT_EQ(count, old_count);
        EXPECT_EQ(active, old_active);
        EXPECT_EQ(pending, 0x8041);
    };
    reject({0, 2, 3, 0}, 4, 0);       // full queue, no room for a new request
    reject({0, 2, 0, 0}, 2, 0x1000);  // stale membership flag
    reject({1, 2, 0, 0}, 2, 0);       // index already present without membership
    reject({1, 1, 0, 0}, 2, 0x1000);  // duplicate membership
    reject({0, 4, 0, 0}, 2, 0);       // corrupted unrelated index
    reject({0, 0, 0, 0}, 5, 0);       // count outside storage
    reject({0, 0, 0, 0}, 0, 0, 3);    // mismatched capacity/view
    reject({0, 0, 0, 0}, 0, 0, 4, 4); // invalid object index
}

TEST(PhysicalLever, FullQueueCanRefreshAnExistingMemberWithoutAppending) {
    std::array<std::uint16_t, 4> indices{0, 1, 2, 3};
    std::uint32_t count = 4;
    std::uint16_t pending = 0, active = 0x1000;
    EXPECT_EQ(L::RequestModelRefresh(1, 4, indices, count, pending, active),
              L::RefreshResult::AlreadyQueued);
    EXPECT_EQ(count, 4u);
    EXPECT_EQ(pending, 0x100);
    EXPECT_EQ(indices, (std::array<std::uint16_t, 4>{0, 1, 2, 3}));
}
namespace {
struct LeverPoseFixture {
    L::BoneBinding binding{{}, {{0, 0.214419f, 0.048485f}, {}}, {1, 0, 0}, {0, 1, -0.75f}, 1.4f};
    std::array<VR::Body::HkQsTransform, 4> locals{};
    std::array<std::uint32_t, 4> flags{};
    std::uint16_t sync = 0x0101, pending = 0x40, active = 0;
    std::array<std::uint16_t, 4> indices{};
    std::uint32_t count = 0;
    LeverPoseFixture() {
        for (auto &local : locals) {
            VR::Body::ToHavok({}, local);
            local.t[3] = 7; // padding must survive writing and restoring the bone
            local.s[3] = 9;
        }
        VR::Body::ToHavok(binding.rest_local, locals[1]);
    }
    L::PoseBuffers Pose() { return {locals, flags, sync}; }
    L::UpdateQueue Queue() { return {1, 4, indices, count, pending, active}; }
};
} // namespace

TEST(PhysicalLever, PoseEditFollowsAngleAndRestoresWithoutChangingOtherBones) {
    LeverPoseFixture f;
    const auto original = f.locals;
    L::PoseEdit edit;
    ASSERT_TRUE(edit.Begin(f.binding, f.Pose(), 1));
    for (float angle : {0.2f, 0.8f, 0.4f, 1.4f}) {
        ASSERT_TRUE(edit.Apply(angle, f.Pose(), f.Queue()));
        const auto expected = L::BoneTarget(f.binding, angle);
        EXPECT_LT(VR::Body::AngleBetween(VR::Body::FromHavok(f.locals[1]).q, expected->q), 1e-6f);
        for (int i : {0, 2, 3}) {
            EXPECT_EQ(std::memcmp(&f.locals[i], &original[i], sizeof(original[i])), 0);
        }
        EXPECT_EQ(f.locals[1].t[3], 7);
        EXPECT_EQ(f.locals[1].s[3], 9);
        EXPECT_EQ(f.sync, 0x0100);
        EXPECT_EQ(f.flags, (std::array<std::uint32_t, 4>{2, 2, 2, 2}));
        EXPECT_EQ(f.count, 1u);
        EXPECT_EQ(f.pending, 0x140);
        f.sync = 0x0101; // game model sync between input samples
    }
    ASSERT_TRUE(edit.Restore(f.Pose(), f.Queue()));
    EXPECT_EQ(std::memcmp(f.locals.data(), original.data(), sizeof(original)), 0);
    EXPECT_FALSE(edit.Owned());
}

TEST(PhysicalLever, PoseEditDoesNotOverwriteAReplacementPoseOrGameAnimation) {
    LeverPoseFixture f;
    L::PoseEdit edit;
    ASSERT_TRUE(edit.Begin(f.binding, f.Pose(), 1));
    ASSERT_TRUE(edit.Apply(0.7f, f.Pose(), f.Queue()));
    f.locals[1].t[1] += 0.1f; // another owner changed this bone
    const auto external = f.locals;
    const auto flags = f.flags;
    EXPECT_FALSE(edit.Restore(f.Pose(), f.Queue()));
    EXPECT_FALSE(edit.Owned());
    EXPECT_EQ(std::memcmp(f.locals.data(), external.data(), sizeof(external)), 0);
    EXPECT_EQ(f.flags, flags);

    LeverPoseFixture old_pose, replacement;
    ASSERT_TRUE(edit.Begin(old_pose.binding, old_pose.Pose(), 1));
    EXPECT_FALSE(edit.Apply(0.4f, replacement.Pose(), replacement.Queue()));
    EXPECT_EQ(replacement.count, 0u);
    EXPECT_EQ(replacement.sync, 0x0101);
    EXPECT_FALSE(edit.Owned());
}

TEST(PhysicalLever, PoseEditCanRecoverAfterQueueFailureWithoutPartialBoneWrites) {
    LeverPoseFixture f;
    L::PoseEdit edit;
    const auto original = f.locals;
    ASSERT_TRUE(edit.Begin(f.binding, f.Pose(), 1));
    f.count = 4;
    f.indices = {0, 2, 3, 0};
    EXPECT_FALSE(edit.Apply(0.7f, f.Pose(), f.Queue()));
    EXPECT_EQ(std::memcmp(f.locals.data(), original.data(), sizeof(original)), 0);
    EXPECT_EQ(f.sync, 0x0101);
    EXPECT_EQ(f.pending, 0x40);
    EXPECT_TRUE(edit.Owned());
    f.count = 0;
    ASSERT_TRUE(edit.Apply(0.7f, f.Pose(), f.Queue()));
    EXPECT_FALSE(edit.Apply(std::numeric_limits<float>::quiet_NaN(), f.Pose(), f.Queue()));
    EXPECT_TRUE(edit.Restore(f.Pose(), f.Queue()));
    EXPECT_EQ(std::memcmp(f.locals.data(), original.data(), sizeof(original)), 0);
}
TEST(PhysicalLeverChannel, NewerReleaseCannotBeLostWhileGameFinishesOldSample) {
    L::Transport channel;
    L::Command first{}, release{};
    EXPECT_FALSE(channel.Read(first));
    EXPECT_EQ(channel.work.load(), 0);
    ASSERT_TRUE(channel.Publish({0, 100, 42, 7, 0.3f, 0, L::CommandKind::Hold}));
    ASSERT_TRUE(channel.Read(first));
    ASSERT_TRUE(channel.Publish({0, 101, 42, 7, 0.3f, 0, L::CommandKind::Release}));
    EXPECT_FALSE(channel.Retire(first.serial));
    ASSERT_TRUE(channel.Read(release));
    EXPECT_EQ(release.kind, L::CommandKind::Release);
    EXPECT_GT(release.serial, first.serial);
    EXPECT_TRUE(channel.CleanupPending());
    EXPECT_FALSE(channel.Publish({0, 102, 43, 8, 0, 1, L::CommandKind::Hold}));
    EXPECT_FALSE(channel.Publish({0, 102, 42, 7, 0.4f, 0, L::CommandKind::Hold}));
    channel.RecordApplied({101, 42, 7, 0.3f});
    ASSERT_TRUE(channel.Retire(release.serial));
    L::Applied retired{};
    ASSERT_TRUE(channel.LatestApplied(retired));
    EXPECT_EQ(retired.id, 0);
    EXPECT_EQ(channel.work.load(), 0);
    EXPECT_FALSE(channel.Read(first));
    EXPECT_FALSE(channel.Retire(release.serial));
    EXPECT_TRUE(channel.Publish({0, 103, 43, 8, 0, 1, L::CommandKind::Hold}));
}

TEST(PhysicalLeverChannel, HoldCannotBeStolenOrReplacedByStaleOrInvalidCommands) {
    L::Transport channel;
    ASSERT_TRUE(channel.Publish({0, 100, 42, 7, 0.3f, 0, L::CommandKind::Hold}));
    EXPECT_FALSE(channel.Publish({0, 101, 42, 8, 0, 0, L::CommandKind::Hold}));
    EXPECT_FALSE(channel.Publish({0, 101, 42, 7, 0, 1, L::CommandKind::Hold}));
    EXPECT_FALSE(channel.Publish({0, 99, 42, 7, 0, 0, L::CommandKind::Release}));
    EXPECT_FALSE(channel.Publish(
        {0, 101, 42, 7, std::numeric_limits<float>::quiet_NaN(), 0, L::CommandKind::Hold}));
    L::Command command{};
    ASSERT_TRUE(channel.Read(command));
    EXPECT_FLOAT_EQ(command.angle, 0.3f);
    EXPECT_EQ(command.ns, 100);
    ASSERT_TRUE(channel.Publish({0, 102, 42, 7, 1.2f, 0, L::CommandKind::Complete}));
    EXPECT_FALSE(channel.Publish({0, 103, 42, 7, 1.2f, 0, L::CommandKind::Release}));
    ASSERT_TRUE(channel.Read(command));
    EXPECT_EQ(command.kind, L::CommandKind::Complete);
}

TEST(PhysicalLeverChannel, AcknowledgedPoseAndOfferKeepIdentityAndActualSampleTime) {
    L::Transport channel;
    L::Offer offer{1'000'000'000, Frame().candidate};
    L::Offer read{};
    EXPECT_FALSE(channel.LatestOffer(read));
    channel.PublishOffer(offer);
    ASSERT_TRUE(channel.LatestOffer(read));
    EXPECT_EQ(read.candidate.id, 42);
    EXPECT_EQ(read.candidate.generation, 7);
    EXPECT_EQ(read.ns, offer.ns);
    EXPECT_TRUE(L::Fresh(read.ns, read.ns + 150'000'000));
    EXPECT_FALSE(L::Fresh(read.ns, read.ns + 150'000'001));
    EXPECT_FALSE(L::Fresh(read.ns, read.ns - 1));
    L::Applied applied{};
    EXPECT_FALSE(channel.LatestApplied(applied));
    channel.RecordApplied({offer.ns, 42, 7, 0.4f});
    ASSERT_TRUE(channel.LatestApplied(applied));
    EXPECT_FLOAT_EQ(applied.angle, 0.4f);
    channel.PublishOffer({});
    EXPECT_FALSE(channel.LatestOffer(read));
}

TEST(PhysicalLeverChannel, ExpiredOfferSleepsWithoutHidingANewerGamePublication) {
    L::Transport channel;
    L::Offer old{1'000'000'000, Frame().candidate}, read{};
    channel.PublishOffer(old);
    channel.DismissOffer(old);
    EXPECT_FALSE(channel.LatestOffer(read));
    auto newer = old;
    newer.ns += 10'000'000;
    channel.PublishOffer(newer);
    channel.DismissOffer(old);
    ASSERT_TRUE(channel.LatestOffer(read));
    EXPECT_EQ(read.ns, newer.ns);
}

TEST(PhysicalLever, CandidateWorkOnlyForFreshSqueezeOrOwnedHold) {
    L::Tracker tracker;
    auto in = Frame();
    in.hands[0].grip = 1;
    EXPECT_FALSE(tracker.NeedsCandidate(in.hands)); // startup squeeze is not a grab
    in.hands[0].grip = 0;
    tracker.Step(in);
    EXPECT_FALSE(tracker.NeedsCandidate(in.hands));
    in.hands[0].grip = 1;
    EXPECT_TRUE(tracker.NeedsCandidate(in.hands));
    in.candidate.eligible = false;
    tracker.Step(in); // consume a squeeze elsewhere
    EXPECT_FALSE(tracker.NeedsCandidate(in.hands));
    in.candidate.eligible = true;
    EXPECT_FALSE(tracker.Step(in).grabbed);
    in.hands[0].grip = 0;
    tracker.Step(in);
    in.hands[1].grip = 1;
    EXPECT_TRUE(tracker.NeedsCandidate(in.hands));
    ASSERT_TRUE(tracker.Step(in).holding);
    EXPECT_TRUE(tracker.NeedsCandidate(in.hands));
    in.hands[1].grip = 0;
    EXPECT_TRUE(tracker.NeedsCandidate(in.hands)); // release needs one final active step
    EXPECT_FALSE(tracker.Step(in).holding);
    EXPECT_FALSE(tracker.NeedsCandidate(in.hands));
    in.hands[0].grip_valid = false;
    in.hands[0].grip = 1;
    EXPECT_FALSE(tracker.NeedsCandidate(in.hands));
    in.hands[0].grip_valid = true;
    in.hands[0].grip = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(tracker.NeedsCandidate(in.hands));
}

TEST(PhysicalLever, DeferredGrabUsesSqueezePositionAndMovesImmediatelyOnReply) {
    L::Tracker tracker;
    L::Acquisition request;
    auto in = Frame();
    tracker.Step(in);
    in.hands[0].grip = 1;
    ASSERT_TRUE(request.Begin(tracker, in, 7, 1'000'000'000));
    auto waiting = request.Poll(tracker, in, 7, 1'010'000'000, false);
    EXPECT_FALSE(waiting.holding);
    EXPECT_TRUE(waiting.owns_grip[0]);
    in.hands[0].position = L::HandleAt(in.candidate.rig, 0.1f);
    const auto out = request.Poll(tracker, in, 7, 1'030'000'000, true);
    EXPECT_TRUE(out.grabbed);
    EXPECT_TRUE(out.holding);
    EXPECT_NEAR(out.angle, 0.1f, 1e-5f);
    EXPECT_EQ(request.Stamp(), 0);
}

TEST(PhysicalLever, DeferredGrabNeverAcquiresAfterApproachOrWithdrawal) {
    for (float away : {0.3f, 1.0f}) {
        for (bool approach : {false, true}) {
            L::Tracker tracker;
            L::Acquisition request;
            auto in = Frame();
            tracker.Step(in);
            in.hands[0].grip = 1;
            if (approach)
                in.hands[0].position.x = away;
            ASSERT_TRUE(request.Begin(tracker, in, 7, 1'000'000'000));
            in.hands[0].position.x = approach ? 0 : away;
            const auto out = request.Poll(tracker, in, 7, 1'030'000'000, true);
            EXPECT_FALSE(out.grabbed);
            EXPECT_FALSE(out.holding);
            // A miss reaching for the lever (within NearMiss of its grip) keeps the squeeze
            // instead of handing it to the weapon; one well away from it passes through.
            EXPECT_EQ(out.owns_grip[0], (approach ? 0.0f : away) <= L::NearMiss) << away;
            in.hands[0].position.x = 0;
            EXPECT_FALSE(tracker.Step(in).grabbed);
        }
    }
}

TEST(PhysicalLever, DeferredGrabCancelsOnReleaseTrackingContextSpaceAndTimeout) {
    for (int failure = 0; failure < 9; ++failure) {
        L::Tracker tracker;
        L::Acquisition request;
        auto in = Frame();
        tracker.Step(in);
        in.hands[1].grip = 1;
        ASSERT_TRUE(request.Begin(tracker, in, 7, 1'000'000'000));
        auto epoch = 7u;
        auto now = 1'030'000'000LL;
        bool replied = true;
        if (failure == 0) in.hands[1].grip = 0;
        if (failure == 1) in.hands[1].tracked = false;
        if (failure == 2) in.gameplay = false;
        if (failure == 3) ++epoch;
        if (failure == 4) now += 100'000'000; // a late offer, the hand at the handle: kept
        if (failure == 5) in.hands[1].busy = true;
        if (failure == 6) in.candidate.eligible = false;
        if (failure == 7) { // never answered: the squeeze goes back to ordinary controls
            now += 100'000'000;
            replied = false;
        }
        if (failure == 8) { // a late offer with the hand well away from it: back as well
            now += 100'000'000;
            in.hands[1].position = L::HandleAt(in.candidate.rig, 0) + Vec3{0, 0, 1};
        }
        const auto out = request.Poll(tracker, in, epoch, now, replied);
        EXPECT_FALSE(out.grabbed) << failure;
        EXPECT_FALSE(out.holding) << failure;
        EXPECT_EQ(out.owns_grip[1], failure != 0 && failure != 6 && failure != 7 && failure != 8)
            << failure;
        EXPECT_EQ(request.Stamp(), 0);
    }
}

TEST(PhysicalLever, DeferredGrabDoesNotReviveAReleasedHandWhileOtherHandWaits) {
    L::Tracker tracker;
    L::Acquisition request;
    auto in = Frame();
    tracker.Step(in);
    in.hands[0].grip = in.hands[1].grip = 1;
    ASSERT_TRUE(request.Begin(tracker, in, 7, 1'000'000'000));
    in.hands[0].grip = 0;
    request.Poll(tracker, in, 7, 1'010'000'000, false);
    in.hands[0].grip = 1;
    auto out = request.Poll(tracker, in, 7, 1'030'000'000, true);
    EXPECT_TRUE(out.holding);
    EXPECT_EQ(out.hand, 1);
}

TEST(PhysicalLeverChannel, AcquisitionCancellationCannotEraseANewerSqueeze) {
    L::Transport channel;
    channel.RequestOffer(1'000'000'000);
    channel.RequestOffer(1'050'000'000);
    channel.CancelOffer(1'000'000'000);
    EXPECT_EQ(channel.acquire.load(), 1'050'000'000);
    // A denied request still answers with its identity, so XR need not wait for timeout.
    channel.PublishOffer({1'060'000'000, {}, 1'050'000'000});
    L::Offer answer{};
    ASSERT_TRUE(channel.LatestOffer(answer));
    EXPECT_FALSE(answer.candidate.eligible);
    EXPECT_EQ(answer.request_ns, 1'050'000'000);
    channel.CancelOffer(1'050'000'000);
    EXPECT_EQ(channel.acquire.load(), 0);
}

TEST(PhysicalLeverChannel, HandPresentationClearsWithoutLeavingTheOtherHandActive) {
    L::Transport channel;
    L::HandPresentation hand{};
    EXPECT_FALSE(channel.PresentedHand(hand));
    channel.PresentHand({1'000'000'000, 3, 0});
    ASSERT_TRUE(channel.PresentedHand(hand));
    EXPECT_EQ(hand.hand, 0);
    EXPECT_EQ(hand.space, 3);
    EXPECT_TRUE(L::Fresh(hand.ns, 1'020'000'000));
    EXPECT_FALSE(L::Fresh(hand.ns, 1'200'000'000));
    channel.PresentHand({1'030'000'000, 3, 1});
    ASSERT_TRUE(channel.PresentedHand(hand));
    EXPECT_EQ(hand.hand, 1);
    channel.PresentHand({});
    EXPECT_FALSE(channel.PresentedHand(hand));
    channel.PresentHand({1'040'000'000, 3, 2});
    EXPECT_FALSE(channel.PresentedHand(hand));
}

TEST(PhysicalLever, FingerShapeUsesTheCorrectHandsReferencePose) {
    namespace G = VR::Body::Grip;
    namespace C = VR::Body::C0000;
    C::Skeleton skeleton{};
    for (int f = 0; f < 5; ++f) {
        for (int j = 0; j < 3; ++j) {
            skeleton.local[G::LeftFingers[f][j]].q = {0, 0, 0, 1};
            skeleton.local[G::RightFingers[f][j]].q = {0, 0, 1, 0};
            const auto left = G::ShapeLocal({}, f, j, 1, 1, skeleton, C::Left);
            const auto right = G::ShapeLocal({}, f, j, 1, 1, skeleton, C::Right);
            EXPECT_FLOAT_EQ(left.w, 1);
            EXPECT_FLOAT_EQ(left.z, 0);
            EXPECT_FLOAT_EQ(right.w, 0);
            EXPECT_FLOAT_EQ(right.z, 1);
        }
    }
}


TEST(LeverGrip, ContactStaysOnHandleAcrossWristRotationAndWeaponOffsets) {
    namespace B = VR::Body;
    for (float side : {-1.0f, 1.0f}) {
        const Vec3 centre{0.08f, 0.02f, side * 0.015f};
        for (float turn : {0.0f, 0.7f, -1.2f}) {
            const auto wrist = B::AxisAngle({0, 1, 0}, turn);
            for (Vec3 offset : {Vec3{0.03f, -0.06f, 0}, Vec3{0.11f, 0.01f, -0.02f}}) {
                const B::QsTransform attachment{offset, B::AxisAngle({1, 0, 0}, 0.3f), {1,1,1}};
                const B::ModelTarget handle{{1, 2, 3}, B::Normalize(wrist * attachment.q)};
                const auto target = L::ContactTarget(handle, attachment, centre);
                const auto solved_wrist = target.p - B::Rotate(wrist, attachment.t);
                EXPECT_LT(B::Length(solved_wrist + B::Rotate(wrist, centre) - handle.p), 1e-5f);
            }
        }
    }
}

TEST(PhysicalLever, EitherHandLatchesAtShaftContactAndFollowsForwardAndReverse) {
    for (int h = 0; h < 2; ++h) {
        auto in = Frame();
        in.candidate.rig.rest = {0, 1, 0};
        in.candidate.rig.shaft_start = {0, .3f, 0};
        in.candidate.rig.shaft = true;
        in.candidate.angle = .2f;
        const Vec3 contact{0, .45f, 0};
        in.hands[h].position = L::Rotate(contact, in.candidate.rig.axis, .2f);
        L::Tracker t;
        auto out = Grab(t, in, h);
        ASSERT_TRUE(out.grabbed);
        EXPECT_NEAR(VR::Body::Length(out.pin - in.hands[h].position), 0, 1e-6f);
        EXPECT_NEAR(VR::Body::Length(out.contact_rest - contact), 0, 1e-6f);
        for (float target : {.8f, .3f}) {
            const float start = out.angle;
            for (int i = 1; i <= 30; ++i) {
                const float angle = start + (target - start) * i / 30;
                in.hands[h].position = L::Rotate(contact, in.candidate.rig.axis, angle);
                out = t.Step(in);
                ASSERT_TRUE(out.holding);
                EXPECT_NEAR(out.angle, angle, 1e-5f);
                EXPECT_NEAR(VR::Body::Length(out.pin - in.hands[h].position), 0, 1e-5f);
            }
        }
        // Small along-shaft motion must not slide the latched contact.
        in.hands[h].position = L::Rotate(Vec3{0, .50f, 0}, in.candidate.rig.axis, out.angle);
        out = t.Step(in);
        ASSERT_TRUE(out.holding);
        EXPECT_NEAR(VR::Body::Length(out.contact_rest - contact), 0, 1e-6f);
    }
}

TEST(PhysicalLever, ShaftReachIsBoundedAndDeferredGrabKeepsPressedContact) {
    auto in = Frame();
    in.candidate.rig.rest = {0, 1, 0};
    in.candidate.rig.shaft_start = {0, .3f, 0};
    in.candidate.rig.shaft = true;
    in.hands[0].position = {.23f, .5f, 0};
    L::Tracker outside;
    EXPECT_FALSE(Grab(outside, in).holding);
    in.hands[0].grip = 0;
    in.hands[0].position = {0, .4f, 0};
    L::Tracker deferred;
    deferred.Step(in);
    in.hands[0].grip = 1;
    auto now = in;
    now.hands[0].position = {0, .43f, 0};
    const auto out = deferred.ResolveGrab(in, now);
    ASSERT_TRUE(out.grabbed);
    EXPECT_NEAR(out.pin.y, .4f, 1e-6f);
    EXPECT_NEAR(out.contact_rest.y, .4f, 1e-6f);
}

TEST(PhysicalLever, PassiveSettleIsSmoothBoundedAndStops) {
    for (bool commit : {false, true}) {
        L::Settle settle;
        settle.Begin(.7f, 1.2f, commit);
        EXPECT_FLOAT_EQ(settle.Angle(), .7f);
        const float first = settle.Step(.01f);
        EXPECT_LT(std::abs(first - .7f), .002f);
        float previous = first;
        for (int i = 0; i < 50; ++i) {
            const float angle = settle.Step(.01f);
            EXPECT_GE(angle, commit ? .7f : 0.f);
            EXPECT_LE(angle, commit ? 1.2f : .7f);
            EXPECT_GE((angle - previous) * (commit ? 1.f : -1.f), -1e-6f);
            previous = angle;
        }
        EXPECT_TRUE(settle.Done());
        EXPECT_FLOAT_EQ(settle.Angle(), commit ? 1.2f : 0.f);
    }
    L::Settle paused;
    paused.Begin(.7f, 1.2f, false);
    EXPECT_GT(paused.Step(10.f), .6f); // a loading stall cannot teleport to rest
}

TEST(PhysicalLever, VerifiedMissReturnsRightGripToWeaponControls) {
    L::Tracker tracker;
    L::Acquisition acquisition;
    auto in = Frame();
    tracker.Step(in);
    in.hands[1].grip = 1;
    ASSERT_TRUE(acquisition.Begin(tracker, in, 7, 1'000'000'000));
    EXPECT_TRUE(acquisition.Poll(tracker, in, 7, 1'010'000'000, false).owns_grip[1]);
    in.candidate.eligible = false;
    const auto miss = acquisition.Poll(tracker, in, 7, 1'020'000'000, true);
    EXPECT_FALSE(miss.owns_grip[1]);
    EXPECT_FALSE(miss.holding);
    in.candidate.eligible = true;
    EXPECT_FALSE(tracker.Step(in).grabbed); // walking closer while squeezing cannot grab
}

// The headset failure (2026-10-14, Central Yharnam o240001_4100): the game offered the lever,
// the hand was out of the 22 cm reach, and every miss went to the right grip's transform. A
// miss within NearMiss of the offered grip is an attempt at the lever: kept, never a transform.
TEST(PhysicalLever, NearMissAtAnOfferedLeverKeepsTheSqueeze) {
    EXPECT_NEAR(L::NearMiss, L::GrabRadius + 0.10f, 1e-6f);
    for (float away : {0.25f, 0.30f, 0.40f, 0.65f}) {
        L::Tracker tracker;
        L::Acquisition acquisition;
        auto in = Frame();
        in.hands[1].position = L::HandleAt(in.candidate.rig, 0) + Vec3{0, 0, away};
        tracker.Step(in);
        in.hands[1].grip = 1;
        ASSERT_TRUE(acquisition.Begin(tracker, in, 7, 1'000'000'000));
        EXPECT_NEAR(acquisition.Distance(in), away, 1e-5f);
        const auto out = acquisition.Poll(tracker, in, 7, 1'020'000'000, true);
        EXPECT_FALSE(out.grabbed) << away;
        EXPECT_EQ(out.owns_grip[1], away <= L::NearMiss) << away;
        // Kept until released: no transform, and no grab by walking in while still squeezing.
        in.hands[1].position = L::HandleAt(in.candidate.rig, 0);
        EXPECT_FALSE(tracker.Step(in).grabbed);
        in.hands[1].grip = 0;
        EXPECT_FALSE(tracker.Step(in).owns_grip[1]);
        // A fresh squeeze at the handle grabs (with the next offer).
        in.hands[1].grip = 1;
        EXPECT_TRUE(tracker.Step(in).grabbed) << away;
    }
    // A refusal (no eligible offer) never keeps it.
    L::Tracker tracker;
    L::Acquisition acquisition;
    auto in = Frame();
    tracker.Step(in);
    in.hands[1].grip = 1;
    ASSERT_TRUE(acquisition.Begin(tracker, in, 7, 1'000'000'000));
    in.candidate = {};
    EXPECT_FALSE(acquisition.Poll(tracker, in, 7, 1'020'000'000, true).owns_grip[1]);
}

// Both grips squeezed together, only the right hand reaching for the offered lever: the right
// squeeze is kept, the left one (the quick-item arc, a guide grip) keeps its ordinary use.
TEST(PhysicalLever, OnlyTheHandReachingForTheLeverKeepsItsMissedSqueeze) {
    L::Tracker tracker;
    L::Acquisition acquisition;
    auto in = Frame();
    in.hands[0].position = L::HandleAt(in.candidate.rig, 0) + Vec3{0, 0, 1.0f};
    in.hands[1].position = L::HandleAt(in.candidate.rig, 0) + Vec3{0, 0, 0.27f};
    tracker.Step(in);
    in.hands[0].grip = in.hands[1].grip = 1;
    ASSERT_TRUE(acquisition.Begin(tracker, in, 7, 1'000'000'000));
    const auto waiting = acquisition.Poll(tracker, in, 7, 1'010'000'000, false);
    EXPECT_TRUE(waiting.owns_grip[0] && waiting.owns_grip[1]); // both wait for the reply
    const auto out = acquisition.Poll(tracker, in, 7, 1'020'000'000, true);
    EXPECT_FALSE(out.grabbed);
    EXPECT_FALSE(out.owns_grip[0]);
    EXPECT_TRUE(out.owns_grip[1]);
}


// ---- The measured lever models (core/vr/lever_models.h) ----------------------------------------

namespace {
namespace LM = VR::Lever;
namespace B = VR::Body;

// The skeleton the game holds for `m`: its Havok bones and its FLVER -> Havok mesh table.
LM::Skeleton SkeletonOf(const LM::Model& m) {
    LM::Skeleton s{};
    s.count = m.count;
    for (std::uint32_t i = 0; i < m.count; ++i) {
        std::strncpy(s.names[i].data(), m.bones[i], s.names[i].size());
        s.parents[i] = m.parents[i];
    }
    s.mesh_count = m.mesh_count;
    for (std::uint32_t f = 0; f < m.mesh_count; ++f)
        s.mesh_map[f] = m.mesh_map[f];
    return s;
}
// The lever bone at its measured rest, with its own scale.
B::QsTransform RestOf(const LM::Model& m) { return {m.rest_t, m.rest_q, m.rest_s}; }
// Every live local at rest: the lever's ancestors at their rests (the root unrotated at its
// reference offset for a lever stick; o353010's turned root and column), the lever bone at rest,
// the other bones somewhere else (they never matter).
std::array<B::QsTransform, LM::MaxBones> LocalsOf(const LM::Model& m) {
    std::array<B::QsTransform, LM::MaxBones> locals{};
    for (auto& l : locals)
        l = {{0.3f, -0.2f, 0.7f}, B::AxisAngle({0, 0, 1}, 1.1f), {1.3f, 1.3f, 1.3f}};
    const auto chain = LM::Chain(m);
    for (std::size_t i = 0; i < chain.n; ++i)
        locals[chain.idx[i]] = {m.chain_rest[i].t + (i == 0 ? m.root_t : Vec3{}),
                                m.chain_rest[i].q, {1, 1, 1}};
    locals[m.lever] = RestOf(m);
    return locals;
}
std::span<const B::QsTransform> Live(const std::array<B::QsTransform, LM::MaxBones>& locals,
                                     const LM::Model& m) {
    return {locals.data(), m.count};
}
// The lever sticks: the lever bone 1 under the root alone (Match's root-and-lever form fits them).
bool Stick(const LM::Model& m) { return m.lever == 1 && LM::Chain(m).n == 1; }
// A bone-local mesh point as the game draws it under `bone` (its own scale first).
Vec3 Drawn(const B::QsTransform& bone, Vec3 v) { return bone.t + B::Rotate(bone.q, B::Mul(bone.s, v)); }

const LM::Model& ModelNamed(const char* name) {
    for (const auto& m : LM::Models)
        if (std::strcmp(m.name, name) == 0)
            return m;
    throw std::runtime_error(name);
}

// The Chalice Dungeon levers, measured from the user's dump (2026-10-14) as the main-game ones
// were, and deliberately NOT listed (main game only): each must keep the game's own prompt.
struct ChaliceLever {
    const char* model;
    std::uint32_t count;
    std::array<const char*, 4> bones;
    std::array<std::int16_t, 4> parents;
    std::uint32_t mesh_count;
    std::array<std::int16_t, 4> mesh_map;
    B::Quat root_q;     // the root's rest (o290020's is turned -90 degrees about Y)
    B::QsTransform lever; // the lever bone (Havok 1) at rest, with its own scale
    std::array<std::int32_t, 3> rows;
};
const std::array<ChaliceLever, 3> ChaliceLevers{{
    // o290010 / 12 / 14: the lift-call and gate levers, row 9902 (one skeleton, o250010's
    // mechanism under its own names).
    {"o290010", 3, {"o9902", "o9902_00", "o9902_01", nullptr}, {-1, 0, 0, 0}, 3, {0, 1, 2, 0},
     {0, 0, 0, 1},
     {{0.0f, 0.2144193f, 0.0484846f}, {0.6432018f, -0.6439532f, -0.2929297f, -0.2929297f},
      {0.807792f, 0.456584f, 0.807792f}},
     {9902, 0, 0}},
    // o290020 / 22 / 24: the main-block door levers, rows 2902000, 9902 and 320. o250010's bone
    // names (lever, lever_01, lever_02), its lever bone's rotation and scale; four FLVER bones
    // (lever, lever_01 and two without a Havok bone), the root turned -90 degrees about Y and the
    // hinge 0.414 m up.
    {"o290020", 3, {"lever", "lever_01", "lever_02", nullptr}, {-1, 0, 0, 0}, 4, {0, 1, -1, -1},
     {0.0f, -0.7074513f, 0.0f, 0.7067621f},
     {{0.0f, 0.4144199f, 0.0484846f}, {-0.6439532f, 0.6432018f, 0.2929297f, 0.2929297f},
      {0.807792f, 0.456584f, 0.807792f}},
     {2902000, 9902, 320}},
    // o293110 / 12 / 14: the turning levers (a capstan bar, motion 101230), rows 2931100 and 9902:
    // two bones.
    {"o293110", 2, {"o293110", "_01", nullptr, nullptr}, {-1, 0, 0, 0}, 2, {0, 1, 0, 0},
     {0, 0, 0, 1}, {{0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1}}, {2931100, 9902, 0}},
}};
LM::Skeleton SkeletonOf(const ChaliceLever& c) {
    LM::Skeleton s{};
    s.count = c.count;
    for (std::uint32_t i = 0; i < c.count; ++i) {
        std::strncpy(s.names[i].data(), c.bones[i], s.names[i].size());
        s.parents[i] = c.parents[i];
    }
    s.mesh_count = c.mesh_count;
    s.mesh_map = c.mesh_map;
    return s;
}
} // namespace

TEST(LeverModels, EveryRowIsListedOnce) {
    // In the order the models first list them; row 100 is shared by six models, listed once. The
    // Research Hall capstan's row (o353010, 3500110) comes last.
    EXPECT_EQ(LM::Actions,
              (std::array<std::int32_t, 10>{2400000, 2410000, 2420000, 2700000, 2500010, 100,
                                            3400020, 2500000, 2600000, 3500110}));
    for (const auto a : {2400000, 2410000, 2420000, 2700000, 2500010, 100, 3400020, 2500000, 2600000,
                         3500110})
        EXPECT_TRUE(LM::SupportedAction(a)) << a;
    for (std::size_t i = 0; i < LM::Actions.size(); ++i)
        for (std::size_t j = i + 1; j < LM::Actions.size(); ++j)
            EXPECT_NE(LM::Actions[i], LM::Actions[j]);
    // The boss-key lever, cannons, Chalice levers (the call levers' 9902, the door levers' 320 and
    // 2902000, the turning levers' 2931100).
    for (const auto a : {2410001, 2410020, 9902, 320, 2902000, 2931100, 0})
        EXPECT_FALSE(LM::SupportedAction(a)) << a;
    // No model lists a Chalice Dungeon row (main game only).
    for (const auto& m : LM::Models)
        for (const auto a : {9902, 320, 2902000, 2931100})
            EXPECT_FALSE(LM::ListedWith(m, a)) << m.name << " " << a;
}

// Every entry is one the live checks can rely on (also a static_assert in lever_models.h).
TEST(LeverModels, EveryModelIsWellFormed) {
    for (const auto& m : LM::Models) {
        EXPECT_TRUE(LM::WellFormed(m)) << m.name;
        EXPECT_GE(m.mesh_count, 2u) << m.name;
        // Every lever stick turns under the root; only the Research Hall capstan under its column,
        // and it alone is a capstan.
        const auto chain = LM::Chain(m);
        const bool capstan = std::strcmp(m.name, "o353010") == 0;
        EXPECT_EQ(chain.n, capstan ? 2u : 1u) << m.name;
        EXPECT_EQ(chain.idx[0], 0) << m.name;
        if (capstan)
            EXPECT_EQ(chain.idx[1], 1);
        else
            EXPECT_EQ(m.parents[m.lever], 0) << m.name;
        EXPECT_EQ(m.motion == LM::Motion::Capstan, capstan) << m.name;
    }
    auto broken = ModelNamed("o321110");
    broken.mesh_map[1] = 2; // the lever bone would have no mesh
    EXPECT_FALSE(LM::WellFormed(broken));
    broken = ModelNamed("o321110");
    broken.mesh_map[3] = 3; // a FLVER bone drawn from a Havok bone the skeleton does not have
    EXPECT_FALSE(LM::WellFormed(broken));
    broken = ModelNamed("o321110");
    broken.end_deg = broken.sweep_deg + 1;
    EXPECT_FALSE(LM::WellFormed(broken));
    broken = ModelNamed("o260040");
    broken.mesh_count = 5;
    EXPECT_FALSE(LM::WellFormed(broken));
    broken = ModelNamed("o250010");
    broken.rest_s.y = 0;
    EXPECT_FALSE(LM::WellFormed(broken));
    // The capstan: it must end turned (its own clip stays at the stop); its chain must reach the
    // root within two ancestors, each a unit rotation; the entries past a chain stay untouched.
    broken = ModelNamed("o353010");
    broken.end_deg = 0;
    EXPECT_FALSE(LM::WellFormed(broken));
    broken = ModelNamed("o353010");
    broken.chain_rest[1].q = {0, 0, 0, 2};
    EXPECT_FALSE(LM::WellFormed(broken));
    broken = ModelNamed("o353010");
    broken.count = 4;
    broken.bones[3] = "o353010_02";
    broken.parents[3] = 2;
    broken.lever = 3; // three ancestors: too deep
    EXPECT_EQ(LM::Chain(broken).n, 0u);
    EXPECT_FALSE(LM::WellFormed(broken));
    broken = ModelNamed("o240001");
    broken.chain_rest[1].t = {0, 0.1f, 0}; // a second ancestor the stick does not have
    EXPECT_FALSE(LM::WellFormed(broken));
    broken = ModelNamed("o240001");
    broken.motion = LM::Motion::Capstan; // a stick returning to rest is no capstan
    EXPECT_FALSE(LM::WellFormed(broken));
    broken = ModelNamed("o240001");
    broken.motion = static_cast<LM::Motion>(7);
    EXPECT_FALSE(LM::WellFormed(broken));
}

// A row only binds the models measured and listed for it. Twins share everything but their rows:
// o240001's skeleton at its rest is o240001 on its Yharnam rows, o342202 on the Hunter's
// Nightmare row and o352001 on the generic row 100; o250010's is the library lever o253010 on
// row 2500000. o352000 has o243005's rest under its own bone names. o260040 shares o260041's row
// under its own names and mesh table.
TEST(LeverModels, ARowBindsOnlyTheModelsListedWithIt) {
    const B::QsTransform root{};
    const auto& yharnam = ModelNamed("o240001");
    const auto& lift = ModelNamed("o243005");
    const auto& cainhurst = ModelNamed("o250010");
    const auto at_rest = [](const LM::Model& m) { return RestOf(m); };
    EXPECT_EQ(LM::Match(SkeletonOf(lift), root, at_rest(lift), 100), &lift);
    EXPECT_EQ(LM::Match(SkeletonOf(yharnam), root, at_rest(yharnam), 2410000), &yharnam);
    EXPECT_EQ(LM::Match(SkeletonOf(yharnam), root, at_rest(yharnam), 100), &ModelNamed("o352001"));
    EXPECT_EQ(LM::Match(SkeletonOf(yharnam), root, at_rest(yharnam), 3400020),
              &ModelNamed("o342202"));
    EXPECT_EQ(LM::Match(SkeletonOf(cainhurst), root, at_rest(cainhurst), 2500010), &cainhurst);
    EXPECT_EQ(LM::Match(SkeletonOf(cainhurst), root, at_rest(cainhurst), 2500000),
              &ModelNamed("o253010"));
    EXPECT_EQ(LM::Match(SkeletonOf(lift), root, at_rest(lift), 2410000), nullptr);
    EXPECT_EQ(LM::Match(SkeletonOf(yharnam), root, at_rest(yharnam), 2600000), nullptr);
    EXPECT_EQ(LM::Match(SkeletonOf(cainhurst), root, at_rest(cainhurst), 100), nullptr);
    auto renamed = SkeletonOf(lift); // o352000: o243005's rest pose under its own names
    std::strncpy(renamed.names[0].data(), "o350142", renamed.names[0].size());
    std::strncpy(renamed.names[1].data(), "o350142_00", renamed.names[1].size());
    std::strncpy(renamed.names[2].data(), "o350142_01", renamed.names[2].size());
    std::strncpy(renamed.names[3].data(), "o350142_02", renamed.names[3].size());
    EXPECT_EQ(LM::Match(renamed, root, at_rest(lift), 100), &ModelNamed("o352000"));
    EXPECT_EQ(LM::Match(renamed, root, at_rest(lift), 2410000), nullptr);
    // The twelve o260040 levers share o260041's row under their own names and their own two-bone
    // mesh table: each binds its own rig, never the other's.
    const auto& mensis = ModelNamed("o260041");
    const auto& cages = ModelNamed("o260040");
    auto renamed_mensis = SkeletonOf(mensis); // o260041's four-entry table under o260040's names
    std::strncpy(renamed_mensis.names[0].data(), "o260040", renamed_mensis.names[0].size());
    std::strncpy(renamed_mensis.names[1].data(), "o260040_00", renamed_mensis.names[1].size());
    std::strncpy(renamed_mensis.names[2].data(), "o260040_01", renamed_mensis.names[2].size());
    std::strncpy(renamed_mensis.names[3].data(), "o260040_02", renamed_mensis.names[3].size());
    EXPECT_EQ(LM::Match(SkeletonOf(mensis), root, at_rest(mensis), 2600000), &mensis);
    EXPECT_EQ(LM::Match(SkeletonOf(cages), root, at_rest(cages), 2600000), &cages);
    EXPECT_EQ(LM::Match(renamed_mensis, root, at_rest(mensis), 2600000), nullptr);
    EXPECT_EQ(LM::Match(renamed_mensis, root, at_rest(cages), 2600000), nullptr);
    EXPECT_EQ(LM::Match(SkeletonOf(cages), root, at_rest(cages), 100), nullptr);
    // o321110 (Byrgenwerth) on the generic row 100 binds its own rig, on no other row.
    const auto& lecture = ModelNamed("o321110");
    EXPECT_EQ(LM::Match(SkeletonOf(lecture), root, at_rest(lecture), 100), &lecture);
    EXPECT_EQ(LM::Match(SkeletonOf(lecture), root, at_rest(lecture), 2600000), nullptr);
    // Every model binds with each of its own rows (every live local at rest), and no other model's.
    for (const auto& m : LM::Models) {
        const auto locals = LocalsOf(m);
        for (const auto a : m.actions)
            if (a != 0) {
                EXPECT_EQ(LM::Match(SkeletonOf(m), Live(locals, m), a), &m) << m.name << " " << a;
                if (Stick(m))
                    EXPECT_EQ(LM::Match(SkeletonOf(m), root, at_rest(m), a), &m) << m.name;
                EXPECT_TRUE(LM::ListedWith(m, a));
            }
    }
    // The capstan's row binds the capstan alone; its skeleton binds on no other listed row.
    const auto& capstan = ModelNamed("o353010");
    const auto capstan_rest = LocalsOf(capstan);
    for (const auto a : LM::Actions)
        EXPECT_EQ(LM::Match(SkeletonOf(capstan), Live(capstan_rest, capstan), a),
                  a == 3500110 ? &capstan : nullptr)
            << a;
    for (const auto& m : LM::Models)
        if (&m != &capstan)
            EXPECT_FALSE(LM::ListedWith(m, 3500110)) << m.name;
    EXPECT_FALSE(LM::ListedWith(lift, 0));
}

// o243005, the Central Yharnam lift levers (row 100): measured from the user's dump. Its lever
// bone rests tilted -12 degrees about X with the hinge 0.363 m up; the grip bulb is in front of
// the hinge, ~1.07 m up, and swings over the top to the back.
TEST(LeverModels, LiftLeverRigLiesWhereItsMeshIs) {
    const auto& lift = ModelNamed("o243005");
    EXPECT_EQ(lift.count, 4u);
    EXPECT_EQ(lift.mesh_map, (std::array<std::int16_t, 4>{0, 1, 2, 3}));
    const auto rig = LM::BindRig(LM::Binding(lift, {}, {lift.rest_t, lift.rest_q, {1, 1, 1}}));
    ASSERT_TRUE(rig.has_value());
    EXPECT_NEAR(rig->sweep, 92.5042f * B::Deg, 1e-4f);
    const auto rest = LM::HandleAt(*rig, 0), stop = LM::HandleAt(*rig, rig->sweep);
    EXPECT_NEAR(rest.y, 1.066f, 0.005f);
    EXPECT_NEAR(rest.z, -0.704f, 0.005f);
    EXPECT_NEAR(stop.y, 1.097f, 0.005f);
    EXPECT_NEAR(stop.z, 0.798f, 0.005f);
    // The shaft starts above the collar, on the moving stem, below the grip.
    const auto shaft = rig->pivot + rig->shaft_start;
    EXPECT_GT(shaft.y, rig->pivot.y + 0.3f);
    EXPECT_LT(shaft.y, rest.y);
}

// The per-instance prompt verdict (core/vr/lever_prompt.h).
TEST(LeverPrompt, OnlyABoundInstanceHidesItsPromptAndBRestoresIt) {
    LM::PromptState state;
    LM::PromptVerdict v{};
    const std::uint64_t row = 0x1000;
    const std::int64_t t0 = 1'000'000'000;
    EXPECT_TRUE(LM::NeedsClassify(v, row, 7, 0, 0));
    v = LM::Decide(v, state, row, 7, true, true, false, 0, 0, t0);
    EXPECT_EQ(v.bound, 1);
    EXPECT_EQ(v.recheck, LM::BoundRecheck);
    EXPECT_FALSE(LM::NeedsClassify(v, row, 7, 0, 0));
    EXPECT_TRUE(LM::NeedsClassify(v, row, 8, 0, 0));         // another object on the row
    EXPECT_TRUE(LM::NeedsClassify(v, row + 0x140, 7, 0, 0)); // another row
    EXPECT_TRUE(LM::NeedsClassify(v, row, 7, 1, 0));         // B fallback
    EXPECT_TRUE(LM::NeedsClassify(v, row, 7, 0, 1));         // a refusal, hold or readiness
    auto due = v;
    due.recheck = 0;
    EXPECT_TRUE(LM::NeedsClassify(due, row, 7, 0, 0));
    due.recheck = 0xFFFF; // a counter wrapped by the gate is due too
    EXPECT_TRUE(LM::NeedsClassify(due, row, 7, 0, 0));
    // No rig (or off): the prompt stays.
    EXPECT_EQ(LM::Decide(v, state, row, 8, true, false, false, 0, 0, t0).bound, 0);
    EXPECT_EQ(LM::Decide(v, state, row, 7, false, true, false, 0, 0, t0).bound, 0);
    EXPECT_EQ(LM::Decide(v, state, row, 8, true, false, false, 0, 0, t0).recheck,
              LM::UnboundRecheck);
    // Held: its pose is off rest by design, and it stays hidden.
    EXPECT_EQ(LM::Decide(v, state, row, 7, true, false, true, 0, 0, t0).bound, 1);
    EXPECT_EQ(LM::Decide(v, state, row, 7, true, false, true, 0, 3, t0).serial, 3u);
    // B at the lever: prompt back for FallbackNs, then hand pulls again.
    v = LM::Decide(v, state, row, 7, true, true, false, 1, 0, t0);
    EXPECT_EQ(v.bound, 0);
    EXPECT_EQ(v.fallback, 1u);
    v = LM::Decide(v, state, row, 7, true, true, false, 1, 0, t0 + LM::FallbackNs - 1);
    EXPECT_EQ(v.bound, 0);
    EXPECT_EQ(LM::Decide(v, state, row + 0x140, 7, true, true, false, 1, 0, t0).bound, 1); // others
    v = LM::Decide(v, state, row, 7, true, true, false, 1, 0, t0 + LM::FallbackNs);
    EXPECT_EQ(v.bound, 1);
    // A refused pull after that (the adapter demotes it): prompt back for RejectedNs.
    const std::int64_t t1 = t0 + LM::FallbackNs;
    state.Demote(row, 7, t1 + LM::RejectedNs);
    EXPECT_EQ(LM::Decide(v, state, row, 7, true, true, false, 1, 0, t1).bound, 0);
    EXPECT_EQ(LM::Decide(v, state, row, 7, true, true, false, 1, 0, t1 + LM::RejectedNs).bound, 1);
}

// The native gate compares the row, handle, B serial and prompt serial, then decrements
// `recheck` (sub word, 1) and asks the host on a borrow or zero (jbe). Modelled here in that
// order: a demotion reaches the host through the prompt serial on the very next pass, and the
// host never meets a pass it should classify but does not (2026-10-14 review: a demotion that
// zeroed the counter wrapped it to 0xFFFF and froze the verdict bound for 65535 passes).
TEST(LeverPrompt, TheGateAsksTheHostOnEveryChangeAndNeverSkipsAWrap) {
    LM::PromptState state;
    const std::uint64_t row = 0x2000;
    const std::uint32_t handle = 0x100001;
    std::uint64_t fallback = 0, serial = 0;
    std::int64_t now = 1'000'000'000;
    LM::PromptVerdict v{};
    int classified = 0;
    // One selection pass: the gate, then (when it asks) the host's classifier.
    const auto pass = [&](bool binds) {
        bool ask = v.row != row || v.handle != handle || v.fallback != fallback ||
                   v.serial != serial;
        if (!ask) {
            const std::uint16_t before = v.recheck;
            v.recheck = static_cast<std::uint16_t>(before - 1);
            ask = before <= 1;
        }
        if (ask && LM::NeedsClassify(v, row, handle, fallback, serial)) {
            v = LM::Decide(v, state, row, handle, true, binds, false, fallback, serial, now);
            ++classified;
        }
        EXPECT_FALSE(ask && LM::NeedsClassify(v, row, handle, fallback, serial));
        return v.bound != 0;
    };
    EXPECT_TRUE(pass(true));
    // A refused pull: the adapter demotes it and bumps the serial (never touching the counter).
    state.Demote(row, handle, now + LM::RejectedNs);
    ++serial;
    EXPECT_FALSE(pass(true)); // the very next pass shows the prompt
    for (int i = 0; i < 3 * LM::BoundRecheck; ++i)
        EXPECT_FALSE(pass(true));
    EXPECT_LE(v.recheck, LM::BoundRecheck);
    // The demotion ends: the next due recheck hides it again.
    now += LM::RejectedNs;
    bool hidden = false;
    for (int i = 0; i <= LM::UnboundRecheck && !hidden; ++i)
        hidden = pass(true);
    EXPECT_TRUE(hidden);
    // Even a counter someone left at zero is classified on the next pass, not 65535 later.
    v.recheck = 0;
    const int before = classified;
    pass(true);
    EXPECT_EQ(classified, before + 1);
    EXPECT_EQ(v.recheck, LM::BoundRecheck);
    // B: the B serial reaches the host at once.
    ++fallback;
    EXPECT_FALSE(pass(true));
}

// One demotion per lever: a refused squeeze at another lever never ends a B fallback, a short
// refusal never shortens a long B fallback of the same lever, a full table replaces the one that
// ends first.
TEST(LeverPrompt, DemotionsAreKeptPerLeverAndNeverShortened) {
    LM::PromptState state;
    const std::int64_t t0 = 1'000'000'000;
    state.Demote(0x1000, 1, t0 + LM::FallbackNs); // B at lever 1
    state.Demote(0x2000, 2, t0 + LM::RejectedNs); // a refusal at lever 2
    EXPECT_TRUE(state.Demoted(0x1000, 1, t0 + LM::RejectedNs + 1));
    EXPECT_FALSE(state.Demoted(0x2000, 2, t0 + LM::RejectedNs));
    state.Demote(0x1000, 1, t0 + LM::RejectedNs); // a later refusal at lever 1
    EXPECT_TRUE(state.Demoted(0x1000, 1, t0 + LM::FallbackNs - 1));
    EXPECT_FALSE(state.Demoted(0x1000, 1, t0 + LM::FallbackNs));
    EXPECT_FALSE(state.Demoted(0x1000, 2, t0)); // the same row, another object
    EXPECT_FALSE(state.Demoted(0, 0, t0));
    state.Demote(0, 0, t0 + LM::FallbackNs); // no row: nothing
    EXPECT_FALSE(state.Demoted(0, 0, t0));
    // Full: lever 1 (B, the latest end) survives three more.
    for (std::uint64_t r = 3; r <= 5; ++r)
        state.Demote(r * 0x1000, static_cast<std::uint32_t>(r), t0 + LM::RejectedNs + r);
    EXPECT_TRUE(state.Demoted(0x1000, 1, t0 + LM::RejectedNs + 10));
    for (std::uint64_t r = 3; r <= 5; ++r)
        EXPECT_TRUE(state.Demoted(r * 0x1000, static_cast<std::uint32_t>(r), t0));
}

// B counts only on its press, at a lever the game selected within FallbackHintNs, with nothing
// pending or held, while hand pulls are on.
TEST(LeverPrompt, BFallbackNeedsAFreshSelectionAndNothingInHand) {
    const std::int64_t hint = 5'000'000'000;
    const std::int64_t now = hint + 100'000'000;
    EXPECT_TRUE(LM::FallbackPress(true, false, true, false, false, hint, now));
    EXPECT_FALSE(LM::FallbackPress(true, true, true, false, false, hint, now));   // held B
    EXPECT_FALSE(LM::FallbackPress(false, false, true, false, false, hint, now)); // no B
    EXPECT_FALSE(LM::FallbackPress(true, false, false, false, false, hint, now)); // off, rehearsal
    EXPECT_FALSE(LM::FallbackPress(true, false, true, true, false, hint, now));   // pending
    EXPECT_FALSE(LM::FallbackPress(true, false, true, false, true, hint, now));   // holding
    EXPECT_FALSE(LM::FallbackPress(true, false, true, false, false, 0, now));     // never selected
    EXPECT_TRUE(LM::FallbackPress(true, false, true, false, false, hint, hint + LM::FallbackHintNs));
    EXPECT_FALSE(
        LM::FallbackPress(true, false, true, false, false, hint, hint + LM::FallbackHintNs + 1));
}

TEST(LeverPrompt, RecentWindow) {
    const std::int64_t t0 = 5'000'000'000;
    EXPECT_TRUE(LM::Recent(t0, t0, 300'000'000));
    EXPECT_TRUE(LM::Recent(t0, t0 + 300'000'000, 300'000'000));
    EXPECT_FALSE(LM::Recent(t0, t0 + 300'000'001, 300'000'000));
    EXPECT_FALSE(LM::Recent(0, t0, 300'000'000));
    EXPECT_FALSE(LM::Recent(t0, t0 - 1, 300'000'000)); // a clock that went back
}

// Arming has no timer: the armed lever stays grabbable while the head or a tracked hand is
// within its handle's reach plus ArmMargin of the hinge (about 3 m for o240001), from any side.
TEST(LeverArming, NearByDistanceFromAnySideNeverByTime) {
    const auto& yharnam = ModelNamed("o240001");
    const B::QsTransform object{{10, 0, -4}, B::AxisAngle({0, 1, 0}, 2.356f), {1, 1, 1}};
    const auto rig = LM::BindRig(LM::Binding(yharnam, object, {yharnam.rest_t, yharnam.rest_q, {1, 1, 1}}));
    ASSERT_TRUE(rig.has_value());
    const float radius = LM::ArmRadius(*rig);
    EXPECT_NEAR(radius, B::Length(yharnam.handle) + LM::ArmMargin, 1e-4f);
    EXPECT_GT(radius, 2.5f);
    EXPECT_LT(radius, 3.2f);
    const std::array<bool, 2> none{false, false}, both{true, true};
    const std::array<Vec3, 2> far_hands{Vec3{100, 0, 0}, Vec3{100, 0, 0}};
    // The head from the front, the side, behind: near within the radius, never beyond it.
    for (float yaw : {0.0f, 1.57f, 3.14f, 4.71f}) {
        const Vec3 dir{std::sin(yaw), 0, std::cos(yaw)};
        EXPECT_TRUE(LM::ArmedNear(*rig, rig->pivot + dir * (radius - 0.01f), true, far_hands, both));
        EXPECT_FALSE(LM::ArmedNear(*rig, rig->pivot + dir * (radius + 0.01f), true, far_hands, both));
    }
    // A hand alone keeps it near (head turned away or leaning back); an untracked one does not.
    const Vec3 far_head = rig->pivot + Vec3{0, 0, radius + 1};
    std::array<Vec3, 2> reaching{far_hands[0], rig->pivot + Vec3{0, 1, 0}};
    EXPECT_TRUE(LM::ArmedNear(*rig, far_head, true, reaching, both));
    EXPECT_FALSE(LM::ArmedNear(*rig, far_head, true, reaching, none));
    EXPECT_FALSE(LM::ArmedNear(*rig, rig->pivot, false, far_hands, both)); // no head sample
    EXPECT_FALSE(LM::ArmedNear(LM::Rig{}, rig->pivot, true, far_hands, both)); // no rig
}

TEST(LeverArming, ReachTickHasHysteresis) {
    EXPECT_TRUE(LM::InReach(false, LM::GrabRadius));
    EXPECT_FALSE(LM::InReach(false, LM::GrabRadius + 0.01f));
    EXPECT_TRUE(LM::InReach(true, LM::GrabRadius + 0.04f)); // stays in on the boundary
    EXPECT_FALSE(LM::InReach(true, LM::GrabRadius + LM::ReachHysteresis + 0.01f));
    EXPECT_FALSE(LM::InReach(true, std::numeric_limits<float>::infinity()));
    EXPECT_FALSE(LM::InReach(false, std::numeric_limits<float>::quiet_NaN()));
}

// The XR side's per-frame arming (lever_acquisition.h Arm), as the presenter runs it.
namespace {
LM::ArmInput ArmedAt(const LM::Rig& rig, std::int64_t now) {
    LM::ArmInput in{};
    in.now = now;
    in.published = true;
    in.published_ns = now;
    in.armed = true;
    in.rig = rig;
    in.gameplay = true;
    in.head = rig.pivot + Vec3{0, 1.6f, 1.0f};
    in.hands = {rig.pivot + Vec3{-0.4f, 1.0f, 1.0f}, rig.pivot + Vec3{0.4f, 1.0f, 1.0f}};
    in.tracked = {true, true};
    in.free = {true, true};
    return in;
}
LM::Rig YharnamRig() {
    const auto& yharnam = ModelNamed("o240001");
    return *LM::BindRig(LM::Binding(yharnam, {}, {yharnam.rest_t, yharnam.rest_q, {1, 1, 1}}));
}
} // namespace

TEST(LeverArming, ArmedNearOnlyOnTheGamesRecentWordInGameplay) {
    const auto rig = YharnamRig();
    const std::int64_t t0 = 10'000'000'000;
    LM::ArmState state;
    auto in = ArmedAt(rig, t0);
    auto out = LM::Arm(state, in);
    EXPECT_TRUE(out.near);
    EXPECT_TRUE(out.armed_near);
    EXPECT_FALSE(out.request_check); // the word is fresh
    // Nothing published (disarmed), or a rig that does not map: nothing.
    auto none = in;
    none.published = false;
    EXPECT_FALSE(LM::Arm(state, none).near);
    none = in;
    none.rig = {};
    EXPECT_FALSE(LM::Arm(state, none).armed_near);
    // The game's word grows stale: still near (it is asked again), not armed past ArmConfirmNs.
    in.now = t0 + LM::ArmConfirmNs;
    EXPECT_TRUE(LM::Arm(state, in).armed_near);
    in.now = t0 + LM::ArmConfirmNs + 1;
    out = LM::Arm(state, in);
    EXPECT_TRUE(out.near);
    EXPECT_FALSE(out.armed_near);
    // A word published just after the frame's clock was read still counts.
    in.now = t0 - 1000;
    EXPECT_TRUE(LM::Arm(state, in).armed_near);
    // Not grabbable now (used, animating, its prompt back): near, never armed.
    in = ArmedAt(rig, t0);
    in.armed = false;
    out = LM::Arm(state, in);
    EXPECT_TRUE(out.near);
    EXPECT_FALSE(out.armed_near);
    // Outside gameplay (a menu, the VR page, a cinematic): not armed.
    in = ArmedAt(rig, t0);
    in.gameplay = false;
    EXPECT_FALSE(LM::Arm(state, in).armed_near);
    // Walked away (head and hands beyond the arm radius): nothing, not even a check.
    LM::ArmState away;
    in = ArmedAt(rig, t0);
    in.published_ns = t0 - 10 * LM::ArmCheckNs;
    in.head = rig.pivot + Vec3{0, 0, LM::ArmRadius(rig) + 1};
    in.hands = {in.head, in.head};
    out = LM::Arm(away, in);
    EXPECT_FALSE(out.near);
    EXPECT_FALSE(out.request_check);
}

TEST(LeverArming, AsksTheGameAtMostTwiceASecondWhileNear) {
    const auto rig = YharnamRig();
    const std::int64_t t0 = 10'000'000'000;
    LM::ArmState state;
    auto in = ArmedAt(rig, t0);
    in.published_ns = t0 - LM::ArmCheckNs - 1; // older than a check period, and never answered
    int checks = 0;
    for (std::int64_t t = t0; t < t0 + 2'000'000'000; t += 11'000'000) { // 90 Hz for 2 s
        in.now = t;
        checks += LM::Arm(state, in).request_check;
    }
    EXPECT_GE(checks, 3);
    EXPECT_LE(checks, 4);
    // A fresh word needs no check.
    LM::ArmState fresh;
    in = ArmedAt(rig, t0);
    for (int i = 0; i < 40; ++i) {
        in.now = t0 + i * 11'000'000;
        EXPECT_FALSE(LM::Arm(fresh, in).request_check);
    }
}

TEST(LeverArming, ReachTicksOnlyAHandThatCouldGrabOnceOnTheWayIn) {
    const auto rig = YharnamRig();
    const auto grip = LM::HandleAt(rig, 0);
    const Vec3 across{1, 0, 0}; // along the hinge axis: off the stem, the grip stays closest
    const std::int64_t t0 = 10'000'000'000;
    LM::ArmState state;
    auto in = ArmedAt(rig, t0);
    auto out = LM::Arm(state, in);
    EXPECT_FALSE(out.tick[1]);
    EXPECT_FALSE(out.reaching[1]);
    // The right hand reaches the handle: one tick, and it is reaching (a squeeze is for it).
    in.hands[1] = grip + across * 0.1f;
    in.now += 11'000'000;
    out = LM::Arm(state, in);
    EXPECT_TRUE(out.tick[1]);
    EXPECT_TRUE(out.reaching[1]);
    EXPECT_FALSE(out.tick[0]);
    EXPECT_FALSE(out.reaching[0]);
    EXPECT_NEAR(out.distance[1], 0.1f, 1e-4f);
    in.now += 11'000'000;
    EXPECT_FALSE(LM::Arm(state, in).tick[1]); // staying there: no more
    // Within NearMiss but beyond the grab radius: reaching, no tick.
    LM::ArmState other;
    auto mid = ArmedAt(rig, t0);
    mid.hands[1] = grip + across * (LM::GrabRadius + 0.05f);
    out = LM::Arm(other, mid);
    EXPECT_TRUE(out.reaching[1]);
    EXPECT_FALSE(out.tick[1]);
    mid.hands[1] = grip + across * (LM::NearMiss + 0.01f);
    EXPECT_FALSE(LM::Arm(other, mid).reaching[1]);
    // A hand that could not grab (busy, blocked, its grip still squeezed) never ticks; once it is
    // free at the handle, it does.
    LM::ArmState busy;
    auto blocked = ArmedAt(rig, t0);
    blocked.hands[1] = grip;
    blocked.free[1] = false;
    out = LM::Arm(busy, blocked);
    EXPECT_FALSE(out.tick[1]);
    EXPECT_TRUE(out.reaching[1]);
    blocked.free[1] = true;
    blocked.now += 11'000'000;
    EXPECT_TRUE(LM::Arm(busy, blocked).tick[1]);
    // Out (past the hysteresis) and straight back in, as a swing through the handle does: at
    // most one tick per ReachTickGapNs.
    blocked.hands[1] = grip + across * (LM::GrabRadius + LM::ReachHysteresis + 0.05f);
    blocked.now += 50'000'000;
    EXPECT_FALSE(LM::Arm(busy, blocked).tick[1]);
    blocked.hands[1] = grip;
    blocked.now += 50'000'000;
    EXPECT_FALSE(LM::Arm(busy, blocked).tick[1]);
    blocked.hands[1] = grip + across;
    blocked.now += 50'000'000;
    LM::Arm(busy, blocked);
    blocked.hands[1] = grip;
    blocked.now += LM::ReachTickGapNs;
    EXPECT_TRUE(LM::Arm(busy, blocked).tick[1]);
    // An untracked hand neither ticks nor reaches.
    LM::ArmState lost;
    auto untracked = ArmedAt(rig, t0);
    untracked.hands[0] = grip;
    untracked.tracked[0] = false;
    untracked.head = grip; // still near by the head
    out = LM::Arm(lost, untracked);
    EXPECT_TRUE(out.armed_near);
    EXPECT_FALSE(out.tick[0]);
    EXPECT_FALSE(out.reaching[0]);
}

TEST(LeverModels, EachRestMatchesItsOwnModel) {
    const B::QsTransform root{};
    for (const auto& m : LM::Models) {
        // Every live local at once (the other bones posed anyhow), as the adapter reads them.
        const auto locals = LocalsOf(m);
        // With its row, itself. Without a row, the first model with this layout and rest (a twin
        // listed earlier carries the very same numbers).
        EXPECT_EQ(LM::Match(SkeletonOf(m), Live(locals, m), m.actions[0]), &m) << m.name;
        const auto* any = LM::Match(SkeletonOf(m), Live(locals, m));
        ASSERT_NE(any, nullptr) << m.name;
        EXPECT_LE(any, &m) << m.name;
        EXPECT_LT(B::Length(any->rest_t - m.rest_t), 0.001f) << m.name;
        // The root-and-lever form (the lever sticks).
        if (Stick(m))
            EXPECT_EQ(LM::Match(SkeletonOf(m), root, RestOf(m), m.actions[0]), &m) << m.name;
        // Fewer locals than the model's bones: nothing to compare, nothing binds.
        EXPECT_EQ(LM::Match(SkeletonOf(m), std::span<const B::QsTransform>{locals.data(), 1},
                            m.actions[0]),
                  nullptr)
            << m.name;
    }
    // o240001 and o270003 share a skeleton: the hinge height tells them apart.
    const auto& yharnam = ModelNamed("o240001");
    const auto& woods = ModelNamed("o270003");
    EXPECT_EQ(LM::Match(SkeletonOf(yharnam), root, {woods.rest_t, {}, {1, 1, 1}}), &woods);
    // Mid-clip (the bone turned 30 degrees), off its pivot, a moved, turned or scaled root (or
    // ancestor), another mapping, or the lever bone at another scale: nothing binds, so the lever
    // keeps its normal interaction.
    for (const auto& m : LM::Models) {
        const auto rest = LocalsOf(m);
        const auto without = [&](auto&& change) {
            auto locals = rest;
            change(locals);
            return LM::Match(SkeletonOf(m), Live(locals, m), m.actions[0]);
        };
        const auto turned = B::Normalize(m.rest_q * B::AxisAngle(m.axis, 30.0f * B::Deg));
        EXPECT_EQ(without([&](auto& l) { l[m.lever].q = turned; }), nullptr) << m.name;
        EXPECT_EQ(without([&](auto& l) { l[m.lever].t = l[m.lever].t + Vec3{0, 0.01f, 0}; }), nullptr)
            << m.name;
        const auto chain = LM::Chain(m);
        for (std::size_t i = 0; i < chain.n; ++i) {
            const auto b = chain.idx[i];
            EXPECT_EQ(without([&](auto& l) { l[b].t = l[b].t + Vec3{0, 0.1f, 0}; }), nullptr)
                << m.name << " ancestor " << i;
            EXPECT_EQ(without([&](auto& l) {
                          l[b].q = B::AxisAngle({0, 1, 0}, -90.0f * B::Deg) * l[b].q;
                      }),
                      nullptr)
                << m.name << " ancestor " << i;
            EXPECT_EQ(without([&](auto& l) { l[b].s = {1, 1.01f, 1}; }), nullptr)
                << m.name << " ancestor " << i;
        }
        auto remapped = SkeletonOf(m);
        std::swap(remapped.mesh_map[1], remapped.mesh_map[2]);
        EXPECT_EQ(LM::Match(remapped, Live(rest, m)), nullptr) << m.name;
        EXPECT_EQ(without([&](auto& l) { l[m.lever].s = B::Mul(l[m.lever].s, Vec3{1.0f, 1.01f, 1.0f}); }),
                  nullptr)
            << m.name;
        // A scaled lever bone at unit scale (or a unit one scaled like Cainhurst's): not this model.
        const bool scaled = B::Length(m.rest_s - Vec3{1, 1, 1}) > 0.01f;
        EXPECT_EQ(without([&](auto& l) {
                      l[m.lever].s = scaled ? Vec3{1, 1, 1} : Vec3{0.8077924f, 0.4565846f, 0.8077924f};
                  }),
                  nullptr)
            << m.name;
        // The same through the root-and-lever form (the lever sticks).
        if (!Stick(m))
            continue;
        EXPECT_EQ(LM::Match(SkeletonOf(m), root, {m.rest_t, turned, m.rest_s}), nullptr) << m.name;
        EXPECT_EQ(LM::Match(SkeletonOf(m), {{0, 0.1f, 0}, {}, {1, 1, 1}}, RestOf(m)), nullptr);
        EXPECT_EQ(LM::Match(SkeletonOf(m), {{}, B::AxisAngle({0, 1, 0}, -90.0f * B::Deg), {1, 1, 1}},
                            RestOf(m)),
                  nullptr)
            << m.name;
        EXPECT_EQ(LM::Match(SkeletonOf(m), {{}, {}, {1, 1.01f, 1}}, RestOf(m)), nullptr) << m.name;
    }
    // The bone-scaled models: Cainhurst's two and the Fishing Hamlet's.
    for (const char* name : {"o250010", "o253010", "o360010"})
        EXPECT_GT(B::Length(ModelNamed(name).rest_s - Vec3{1, 1, 1}), 0.2f) << name;
}

// The matrix provider's table is read FLVER -> Havok: one entry per FLVER bone, an entry the
// Havok bone it draws from, a FLVER bone with no Havok bone negative. o260040 has 2 FLVER bones over
// 4 Havok bones; o321110 4 over 3 (its fourth, the mechanism block, draws from no Havok bone).
TEST(LeverModels, MeshTableIsReadFlverToHavok) {
    const B::QsTransform root{};
    for (const char* name : {"o260040", "o321110"}) {
        const auto& m = ModelNamed(name);
        const auto rest = RestOf(m);
        const auto row = m.actions[0];
        ASSERT_NE(m.mesh_count, m.count) << name;
        EXPECT_EQ(LM::Match(SkeletonOf(m), root, rest, row), &m) << name;
        // One FLVER entry more or fewer.
        for (const int delta : {-1, 1}) {
            auto s = SkeletonOf(m);
            s.mesh_count = m.mesh_count + delta;
            EXPECT_EQ(LM::Match(s, root, rest, row), nullptr) << name << " count " << delta;
        }
        // The lever's FLVER entry drawing from another Havok bone; any matched entry changed.
        for (std::uint32_t f = 0; f < m.mesh_count; ++f) {
            if (m.mesh_map[f] < 0)
                continue;
            auto s = SkeletonOf(m);
            s.mesh_map[f] = static_cast<std::int16_t>((m.mesh_map[f] + 1) % m.count);
            EXPECT_EQ(LM::Match(s, root, rest, row), nullptr) << name << " entry " << f;
            s.mesh_map[f] = -1;
            EXPECT_EQ(LM::Match(s, root, rest, row), nullptr) << name << " entry " << f;
        }
        // The table indexed the other way (Havok -> FLVER: the skeleton's count of entries).
        auto havok = SkeletonOf(m);
        havok.mesh_count = m.count;
        for (std::uint32_t h = 0; h < m.count; ++h) {
            havok.mesh_map[h] = -1;
            for (std::uint32_t f = 0; f < m.mesh_count; ++f)
                if (m.mesh_map[f] == static_cast<std::int16_t>(h))
                    havok.mesh_map[h] = static_cast<std::int16_t>(f);
        }
        EXPECT_EQ(LM::Match(havok, root, rest, row), nullptr) << name;
    }
    // o321110's unmapped fourth entry: any negative value is the game's "no Havok bone"; one set to
    // a real bone (0, the root) is another mesh.
    const auto& lecture = ModelNamed("o321110");
    for (const std::int16_t unmapped : {std::int16_t{-1}, std::int16_t{-2}, std::int16_t{-32768}}) {
        auto s = SkeletonOf(lecture);
        s.mesh_map[3] = unmapped;
        EXPECT_EQ(LM::Match(s, root, RestOf(lecture), 100), &lecture) << unmapped;
    }
    auto drawn = SkeletonOf(lecture);
    drawn.mesh_map[3] = 0;
    EXPECT_EQ(LM::Match(drawn, root, RestOf(lecture), 100), nullptr);
    // Entries past the table's count are never read.
    auto cages = SkeletonOf(ModelNamed("o260040"));
    cages.mesh_map[2] = 3;
    cages.mesh_map[3] = -7;
    EXPECT_EQ(LM::Match(cages, root, RestOf(ModelNamed("o260040")), 2600000), &ModelNamed("o260040"));
}

// The wave-2 rigs against external game data, not against their own entries (re-extracted and
// re-read independently, 2026-10-14): each mesh table as the game's matrix provider holds it, each
// lever bone's own scale, and a bind at the dump's own poses (the Havok reference pose, the FLVER
// bind pose; or the idle clip's first frame), every bone as the dump has it. A transcription slip
// (o353010's table as {2, 1, 0}, a scale digit) fails here instead of keeping a prompt in the game.
TEST(LeverModels, NewRigsMatchTheDump) {
    using Table = std::array<std::int16_t, LM::MaxMeshBones>;
    const auto table = [](const char* name) {
        const auto& m = ModelNamed(name);
        Table t{};
        for (std::uint32_t f = 0; f < m.mesh_count; ++f)
            t[f] = m.mesh_map[f];
        return std::pair{m.mesh_count, t};
    };
    EXPECT_EQ(table("o353010"), (std::pair{3u, Table{1, 2, 0, 0}}));
    EXPECT_EQ(table("o321110"), (std::pair{4u, Table{0, 1, 2, -1}}));
    EXPECT_EQ(table("o260040"), (std::pair{2u, Table{0, 1, 0, 0}}));
    for (const char* name : {"o250010", "o253010", "o360010"})
        EXPECT_EQ(table(name), (std::pair{3u, Table{0, 1, 2, 0}})) << name;
    const auto near = [](Vec3 a, Vec3 b) { return B::Length(a - b) < 2e-6f; };
    for (const char* name : {"o250010", "o253010"})
        EXPECT_TRUE(near(ModelNamed(name).rest_s, {0.807792f, 0.456584f, 0.807792f})) << name;
    EXPECT_TRUE(near(ModelNamed("o360010").rest_s, {0.840041f, 0.797562f, 0.840041f}));
    for (const char* name : {"o260040", "o321110", "o353010"})
        EXPECT_TRUE(near(ModelNamed(name).rest_s, {1, 1, 1})) << name;

    // The dump's root (the reference pose's, a float off identity) and lever bone.
    struct Pose {
        const char* model;
        const char* source;
        B::QsTransform root, lever;
    };
    const B::QsTransform ref_root{{0, 0, -5.96046448e-08f}, {0, 0, 0, 0.99999994f},
                                  {0.99999994f, 0.99999994f, 0.99999994f}};
    const B::QsTransform cainhurst_ref{{7.5255393e-08f, 0.214419335f, 0.0484845713f},
                                       {0.643439829f, -0.643439651f, -0.293232381f, -0.293232471f},
                                       {0.807791948f, 0.456584007f, 0.807791829f}};
    const std::array<Pose, 7> poses{{
        {"o260040", "reference pose (t.x 0.29 mm)", ref_root,
         {{0.0002898573f, 0.214419335f, 0.0484845713f}, {0, 0, 0, 1}, {1, 1, 1}}},
        {"o260040", "idle clip", {}, {{0, 0.21441932f, 0.0484845676f}, {0, 0, 0, 1}, {1, 1, 1}}},
        {"o321110", "reference pose",
         ref_root,
         {{-2.31013502e-08f, 0.214419335f, 0.0484845713f}, {-0.043619372f, 0, 0, 0.999048233f},
          {1, 1, 1}}},
        {"o321110", "idle clip", {},
         {{0, 0.21441932f, 0.0484845676f}, {-0.043524936f, 0, 0, 0.999052341f}, {1, 1, 1}}},
        {"o250010", "reference pose",
         {{-7.52554215e-08f, 0, -5.96046448e-08f}, {0, 0, 0, 0.99999994f},
          {0.99999994f, 0.99999994f, 0.99999994f}},
         cainhurst_ref},
        {"o253010", "reference pose",
         {{-7.52554215e-08f, 0, -5.96046448e-08f}, {0, 0, 0, 0.99999994f},
          {0.99999994f, 0.99999994f, 0.99999994f}},
         cainhurst_ref},
        {"o360010", "idle clip", {},
         {{0, 0.214419916f, 0.0484799556f},
          {0.643953222f, -0.643201832f, -0.292929728f, -0.292929728f},
          {0.840040326f, 0.797561884f, 0.840040147f}}},
    }};
    for (const auto& p : poses) {
        const auto& m = ModelNamed(p.model);
        auto locals = LocalsOf(m);
        locals[0] = p.root;
        locals[m.lever] = p.lever;
        EXPECT_EQ(LM::Match(SkeletonOf(m), Live(locals, m), m.actions[0]), &m)
            << p.model << " " << p.source;
        const auto binding = LM::Binding(m, LM::ParentWorld(m, Live(locals, m), {}), p.lever);
        const auto rig = LM::BindRig(binding);
        ASSERT_TRUE(rig.has_value()) << p.model << " " << p.source;
        // The grip where the game draws the knob or bulb from that very pose (its scale first),
        // and the stop the same sweep away.
        EXPECT_LT(B::Length(LM::HandleAt(*rig, 0) - Drawn(B::Compose(locals[0], p.lever), m.handle)),
                  1e-5f)
            << p.model << " " << p.source;
        EXPECT_NEAR(rig->sweep, m.sweep_deg * B::Deg, 1e-6f) << p.model << " " << p.source;
    }
}

// The Mensis lift lever (o260041) idles unrotated, but its own pull clip starts and ends tilted
// -4.99 degrees (the dump's frames 0 and 105, hinge 0.29 mm off the reference pose's): after the
// game's own pull it stands there, and it binds there too, with the clip's stop. The twelve
// o260040 levers' clip starts and ends at 0 (no second rest); nothing between two rests binds.
TEST(LeverModels, MensisLiftLeverBindsWhereItsOwnPullLeavesIt) {
    const auto& lift = ModelNamed("o260041");
    const auto s = SkeletonOf(lift);
    EXPECT_NEAR(lift.rest_alt_deg, -4.9893f, 0.0005f);
    const B::QsTransform clip_end{{0, 0.214419052f, 0.0484846123f},
                                  {-0.043524936f, 0, 0, 0.999052341f},
                                  {1, 1, 1}};
    auto locals = LocalsOf(lift);
    locals[0] = {};
    locals[1] = clip_end;
    EXPECT_EQ(LM::Match(s, Live(locals, lift), 2600000), &lift);
    EXPECT_FALSE(LM::LeverAtRest(lift, clip_end));
    EXPECT_TRUE(LM::LeverAtAltRest(lift, clip_end));
    EXPECT_FALSE(LM::LeverAtAltRest(lift, RestOf(lift)));
    const auto from_end = LM::BindRig(LM::Binding(lift, {}, clip_end));
    const auto from_rest = LM::BindRig(LM::Binding(lift, {}, RestOf(lift)));
    ASSERT_TRUE(from_end.has_value());
    ASSERT_TRUE(from_rest.has_value());
    EXPECT_NEAR(from_rest->sweep, 75.7855f * B::Deg, 1e-6f);
    EXPECT_NEAR(from_end->sweep, (75.7855f + 4.9893f) * B::Deg, 1e-4f);
    // The same stop either way (the clip's frame 76), within the hinge's 0.29 mm.
    EXPECT_LT(B::Length(LM::HandleAt(*from_end, from_end->sweep) -
                        LM::HandleAt(*from_rest, from_rest->sweep)),
              0.0005f);
    const auto stop = LM::BoneTarget(LM::Binding(lift, {}, clip_end), from_end->sweep);
    ASSERT_TRUE(stop.has_value());
    EXPECT_GT(std::abs(B::Dot(B::Normalize(stop->q), B::AxisAngle({1, 0, 0}, 75.7855f * B::Deg))),
              0.999999f);
    // Between the two rests, or past either: nothing binds.
    for (const float deg : {-2.5f, -8.0f, 3.0f}) {
        auto l = locals;
        l[1].q = B::Normalize(lift.rest_q * B::AxisAngle(lift.axis, deg * B::Deg));
        EXPECT_EQ(LM::Match(s, Live(l, lift), 2600000), nullptr) << deg;
    }
    // o260040: its clip returns to 0; tilted -4.99 degrees it is mid-clip, not at rest.
    const auto& cages = ModelNamed("o260040");
    EXPECT_EQ(cages.rest_alt_deg, 0.0f);
    auto c = LocalsOf(cages);
    c[0] = {};
    c[1] = {cages.rest_t, clip_end.q, {1, 1, 1}};
    EXPECT_EQ(LM::Match(SkeletonOf(cages), Live(c, cages), 2600000), nullptr);
    // Only o260041 has one. A second rest within the match tolerance of the first, far off, or on
    // a capstan is not well formed.
    for (const auto& m : LM::Models)
        EXPECT_EQ(m.rest_alt_deg != 0, std::strcmp(m.name, "o260041") == 0) << m.name;
    EXPECT_TRUE(LM::WellFormed(lift));
    for (const float bad : {-1.0f, 1.5f, -12.0f}) {
        auto m = lift;
        m.rest_alt_deg = bad;
        EXPECT_FALSE(LM::WellFormed(m)) << bad;
    }
    auto capstan = ModelNamed("o353010");
    capstan.rest_alt_deg = -5.0f;
    EXPECT_FALSE(LM::WellFormed(capstan));
}

// Main game only (the user's decision, 2026-10-14): no Chalice Dungeon lever binds, on its own rows
// or on any listed one, although o290020's skeleton shares o250010's bone names and its lever
// bone's rotation and scale, and o290010's is o250010's mechanism under other names. Each guard
// that tells o290020 from o250010 / o253010 holds on its own.
TEST(LeverModels, ChaliceDungeonLeversStayUnbound) {
    const auto chalice_rows = {9902, 320, 2902000, 2931100};
    for (const auto& c : ChaliceLevers) {
        const auto s = SkeletonOf(c);
        const B::QsTransform root{{}, c.root_q, {1, 1, 1}};
        for (const auto row : c.rows)
            if (row != 0) {
                EXPECT_FALSE(LM::SupportedAction(row)) << c.model << " " << row;
                EXPECT_EQ(LM::Match(s, root, c.lever, row), nullptr) << c.model << " " << row;
            }
        for (const auto row : chalice_rows)
            EXPECT_EQ(LM::Match(s, root, c.lever, row), nullptr) << c.model << " " << row;
        for (const auto row : LM::Actions)
            EXPECT_EQ(LM::Match(s, root, c.lever, row), nullptr) << c.model << " " << row;
        EXPECT_EQ(LM::Match(s, root, c.lever), nullptr) << c.model;
        // Even with an unrotated root: still no model.
        EXPECT_EQ(LM::Match(s, B::QsTransform{}, c.lever), nullptr) << c.model;
    }
    // o290020 against Cainhurst's rows, one difference at a time.
    const auto& door = ChaliceLevers[1];
    ASSERT_STREQ(door.model, "o290020");
    const auto& cainhurst = ModelNamed("o250010");
    const B::QsTransform door_root{{}, door.root_q, {1, 1, 1}};
    for (const auto row : {2500010, 2500000}) {
        // Its own layout (four FLVER bones, two unmapped), root and hinge.
        EXPECT_EQ(LM::Match(SkeletonOf(door), door_root, door.lever, row), nullptr) << row;
        // Cainhurst's root and lever under its four-bone mesh table.
        EXPECT_EQ(LM::Match(SkeletonOf(door), {}, RestOf(cainhurst), row), nullptr) << row;
        // Its root turned -90 degrees under Cainhurst's mesh table and lever.
        EXPECT_EQ(LM::Match(SkeletonOf(cainhurst), door_root, RestOf(cainhurst), row), nullptr) << row;
        // Its hinge 0.414 m up (its lever otherwise Cainhurst's) under Cainhurst's table and root.
        EXPECT_EQ(LM::Match(SkeletonOf(cainhurst), {}, door.lever, row), nullptr) << row;
        // And the reverse: Cainhurst's very own pose binds Cainhurst's models (the guards above
        // are the only differences).
        EXPECT_NE(LM::Match(SkeletonOf(cainhurst), {}, RestOf(cainhurst), row), nullptr) << row;
    }
    // Its lever bone is Cainhurst's rotation (the opposite sign: the same rotation) and scale.
    EXPECT_GT(std::abs(B::Dot(B::Normalize(door.lever.q), B::Normalize(cainhurst.rest_q))), 0.9999f);
    EXPECT_LT(B::Length(door.lever.s - cainhurst.rest_s), 0.001f);
    // o290010: Cainhurst's mechanism (its rest within 0.3 degree) under its own names.
    const auto& call = ChaliceLevers[0];
    EXPECT_EQ(LM::Match(SkeletonOf(call), {}, RestOf(cainhurst), 2500010), nullptr);
    EXPECT_EQ(LM::Match(SkeletonOf(call), {}, call.lever, 9902), nullptr);
}

// The rig turns the handle exactly as the game's own pull clip turns the bone: at the clip's
// stop (its quaternion, sampled from the user's dump) the grip point is where the clip puts it.
TEST(LeverModels, PulledHandleFollowsTheGameClip) {
    struct Pulled {
        const char* model;
        B::Quat q; // the lever bone at the clip's stop
    };
    const std::array<Pulled, 15> clips{{
        {"o240001", {0.648038f, 0, 0, 0.761608f}},                       // frame 76
        {"o270003", {0.751456f, 0, 0, 0.659783f}},                       // frame 79
        {"o250010", {0.29293f, -0.29293f, -0.643202f, -0.643953f}},      // frame 72
        {"o243005", {0.646311f, 0, 0, 0.763074f}},                       // frame 76
        // The twins' clips are their originals' (every track, every frame, within 0.01 degree).
        {"o342202", {0.648038f, 0, 0, 0.761608f}},                       // frame 76
        {"o352001", {0.648038f, 0, 0, 0.761608f}},                       // frame 76
        {"o352000", {0.646311f, 0, 0, 0.763074f}},                       // frame 76
        {"o253010", {0.29293f, -0.29293f, -0.643202f, -0.643953f}},      // frame 72 (and held)
        {"o220005", {0.684654f, 0, 0, 0.728868f}},                       // frame 76
        {"o260041", {0.614185f, 0, 0, 0.789162f}},                       // frame 76
        {"o280001", {0.614185f, 0, 0, 0.789162f}},                       // frame 76
        {"o360010", {0.29293f, -0.29293f, -0.643202f, -0.643953f}},      // frame 72
        {"o260040", {0.6141852f, 0, 0, 0.7891619f}},                     // frame 76
        {"o321110", {0.6141852f, 0, 0, 0.7891619f}},                     // frame 76
        // The Research Hall capstan's turn (101230's object clip, frame 130, its last).
        {"o353010", {0, 0.705034876f, 0, -0.709172633f}},
    }};
    // An object turned and moved in the world, to catch frame-order mistakes.
    const B::QsTransform object{{1.25f, 2.5f, -3.75f}, B::AxisAngle({0, 1, 0}, 0.7f), {1, 1, 1}};
    for (const auto& c : clips) {
        const auto& m = ModelNamed(c.model);
        // The lever bone's parent: the object and every ancestor at rest (o353010's turned root
        // and column; a stick's root, raised for o280001).
        const auto parent = LM::ParentWorld(m, Live(LocalsOf(m), m), object);
        const auto binding = LM::Binding(m, parent, RestOf(m));
        const auto rig = LM::BindRig(binding);
        ASSERT_TRUE(rig.has_value()) << m.name;
        // The bone as the game draws it: its own scale (the clip's constant track) included.
        const auto bone_world = [&](B::Quat q) { return B::Compose(parent, {m.rest_t, q, m.rest_s}); };
        const Vec3 expect_rest = Drawn(bone_world(m.rest_q), m.handle);
        const Vec3 expect_pulled = Drawn(bone_world(c.q), m.handle);
        EXPECT_LT(B::Length(LM::HandleAt(*rig, 0) - expect_rest), 0.001f) << m.name;
        EXPECT_LT(B::Length(LM::HandleAt(*rig, rig->sweep) - expect_pulled), 0.005f) << m.name;
        // The bone pose written at the stop is the clip's, its scale kept.
        const auto target = LM::BoneTarget(binding, rig->sweep);
        ASSERT_TRUE(target.has_value());
        EXPECT_GT(std::abs(B::Dot(B::Normalize(target->q), B::Normalize(c.q))), 0.9999f) << m.name;
        EXPECT_EQ(B::Length(target->s - m.rest_s), 0.0f) << m.name;
    }
    // Cainhurst's knob swings over the top, back to front, about the object's +X: 1.15 m from the
    // hinge as drawn (1.43 m in the mesh, scaled by the bone's 0.81).
    const auto& cainhurst = ModelNamed("o250010");
    const auto rig = LM::BindRig(LM::Binding(cainhurst, {}, RestOf(cainhurst)));
    ASSERT_TRUE(rig.has_value());
    EXPECT_NEAR(rig->axis.x, 1.0f, 0.001f);
    const auto rest = LM::HandleAt(*rig, 0), stop = LM::HandleAt(*rig, rig->sweep);
    EXPECT_NEAR(rest.y, 1.084f, 0.005f);
    EXPECT_NEAR(rest.z, -0.709f, 0.005f);
    EXPECT_NEAR(stop.y, 1.084f, 0.005f);
    EXPECT_NEAR(stop.z, 0.806f, 0.005f);
    EXPECT_NEAR(B::Length(rig->rest), 1.153f, 0.002f);
    EXPECT_NEAR(LM::ArmRadius(*rig), 1.153f + LM::ArmMargin, 0.002f);
    // Bound at a unit scale (as before 2026-10-14, which the game never holds), the grip would be
    // 0.29 m off the drawn knob.
    const auto unscaled = LM::BindRig(LM::Binding(cainhurst, {}, {cainhurst.rest_t, cainhurst.rest_q, {1, 1, 1}}));
    ASSERT_TRUE(unscaled.has_value());
    EXPECT_GT(B::Length(LM::HandleAt(*unscaled, 0) - rest), 0.25f);
}

// A lever bone's own (non-uniform) scale is drawn before the bone turns: its rigid vertices land at
// t + R(q)(s * v), as the game's T * R(q) * diag(s) bone matrix puts them, at every angle, for any
// hinge axis and parent. The parent must stay unscaled; the bone's own scale must be sane.
TEST(PhysicalLever, ABoneOwnScaleIsDrawnBeforeItTurns) {
    L::BoneBinding b{};
    b.parent_world = {{2, -3, 4}, VR::Body::AxisAngle({0.3f, 1, 0.2f}, 0.7f), {1, 1, 1}};
    b.rest_local = {{0.1f, 0.3f, -0.2f}, VR::Body::AxisAngle({0, 0, 1}, -0.4f), {0.8f, 0.45f, 0.8f}};
    b.axis_local = VR::Body::Normalize(Vec3{0.2f, -1.0f, 0.1f}); // not along a scale axis
    b.handle_local = {0.03f, 0.1f, 1.4f};
    b.shaft_start_local = {0, 0, 0.6f};
    b.shaft = true;
    b.sweep = 1.4f;
    const auto rig = L::BindRig(b);
    ASSERT_TRUE(rig);
    for (const float fraction : {0.0f, 0.3f, 1.0f}) {
        const float angle = fraction * b.sweep;
        const auto target = L::BoneTarget(b, angle);
        ASSERT_TRUE(target);
        EXPECT_EQ(VR::Body::Length(target->s - b.rest_local.s), 0.0f); // the write keeps the scale
        const auto world = VR::Body::Compose(b.parent_world, *target);
        const auto drawn = [&](Vec3 v) { return world.t + VR::Body::Rotate(world.q, VR::Body::Mul(world.s, v)); };
        EXPECT_LT(VR::Body::Length(drawn(b.handle_local) - L::HandleAt(*rig, angle)), 1e-5f) << fraction;
        EXPECT_LT(VR::Body::Length(drawn(b.shaft_start_local) -
                                   (rig->pivot + L::Rotate(rig->shaft_start, rig->axis, angle))),
                  1e-5f)
            << fraction;
    }
    // The hinge stays a circle: the drawn handle keeps its distance from the pivot.
    EXPECT_NEAR(VR::Body::Length(L::HandleAt(*rig, b.sweep) - rig->pivot),
                VR::Body::Length(L::HandleAt(*rig, 0) - rig->pivot), 1e-5f);
    for (const Vec3 bad : {Vec3{0, 0.45f, 0.8f}, Vec3{-0.8f, 0.45f, 0.8f}, Vec3{0.8f, 40, 0.8f},
                           Vec3{0.8f, std::numeric_limits<float>::quiet_NaN(), 0.8f},
                           Vec3{0.8f, std::numeric_limits<float>::infinity(), 0.8f}}) {
        auto broken = b;
        broken.rest_local.s = bad;
        EXPECT_FALSE(L::BindRig(broken)) << bad.x << " " << bad.y;
        EXPECT_FALSE(L::BoneTarget(broken, 0.5f));
    }
    auto scaled_parent = b;
    scaled_parent.parent_world.s.y = 2; // an ellipse, not a hinge
    EXPECT_FALSE(L::BindRig(scaled_parent));
    scaled_parent.parent_world.s = {0.8f, 0.8f, 0.8f}; // even a uniform one: lengths would differ
    EXPECT_FALSE(L::BindRig(scaled_parent));
}

// The settle can end anywhere between rest and the stop (o321110's own clip leaves it partway).
TEST(PhysicalLever, SettleBeginToEndsAtItsTarget) {
    L::Settle s;
    s.BeginTo(1.4f, 1.07f);
    EXPECT_FLOAT_EQ(s.Angle(), 1.4f);
    float previous = s.Angle();
    for (int i = 0; i < 40 && !s.Done(); ++i) {
        const float a = s.Step(1.0f / 60.0f);
        EXPECT_LE(a, previous + 1e-6f);
        EXPECT_GE(a, 1.07f - 1e-6f);
        previous = a;
    }
    EXPECT_TRUE(s.Done());
    EXPECT_FLOAT_EQ(s.Angle(), 1.07f);
    s.Begin(0.9f, 1.4f, true); // the wrappers keep their meaning
    while (!s.Done())
        s.Step(0.05f);
    EXPECT_FLOAT_EQ(s.Angle(), 1.4f);
    s.Begin(0.5f, 1.4f, false);
    while (!s.Done())
        s.Step(0.05f);
    EXPECT_FLOAT_EQ(s.Angle(), 0.0f);
}

// The geometric twins carry their originals' rigs exactly, under their own rows (and, for
// o352000, its own bone names). Only the library lever stays pulled after a completed pull.
TEST(LeverModels, TwinsCarryTheirOriginalsRigUnderTheirOwnRows) {
    const auto same = [](Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
    const std::array<std::pair<const char*, const char*>, 4> twins{{
        {"o342202", "o240001"},
        {"o352001", "o240001"},
        {"o352000", "o243005"},
        {"o253010", "o250010"},
    }};
    for (const auto& [twin_name, original_name] : twins) {
        const auto& twin = ModelNamed(twin_name);
        const auto& original = ModelNamed(original_name);
        EXPECT_EQ(twin.count, original.count) << twin_name;
        EXPECT_EQ(twin.parents, original.parents) << twin_name;
        EXPECT_EQ(twin.mesh_count, original.mesh_count) << twin_name;
        EXPECT_EQ(twin.mesh_map, original.mesh_map) << twin_name;
        EXPECT_EQ(twin.lever, original.lever) << twin_name;
        EXPECT_TRUE(same(twin.rest_t, original.rest_t)) << twin_name;
        EXPECT_TRUE(same(twin.rest_s, original.rest_s)) << twin_name;
        EXPECT_TRUE(twin.rest_q.x == original.rest_q.x && twin.rest_q.y == original.rest_q.y &&
                    twin.rest_q.z == original.rest_q.z && twin.rest_q.w == original.rest_q.w)
            << twin_name;
        EXPECT_TRUE(same(twin.axis, original.axis)) << twin_name;
        EXPECT_EQ(twin.sweep_deg, original.sweep_deg) << twin_name;
        EXPECT_TRUE(same(twin.handle, original.handle)) << twin_name;
        EXPECT_TRUE(same(twin.shaft_start, original.shaft_start)) << twin_name;
        EXPECT_TRUE(same(twin.root_t, original.root_t)) << twin_name;
        // Under the same bone names, never one of its original's rows: the row alone tells them
        // apart. (o352000 shares the generic row 100 with o243005 under its own names.)
        if (std::strcmp(twin.bones[0], original.bones[0]) == 0)
            for (const auto a : twin.actions)
                if (a != 0)
                    EXPECT_FALSE(LM::ListedWith(original, a)) << twin_name << " " << a;
    }
    // o352000's own bone names; the others keep their originals'.
    EXPECT_STREQ(ModelNamed("o352000").bones[1], "o350142_00");
    EXPECT_STREQ(ModelNamed("o342202").bones[1], "o240001_00");
    EXPECT_STREQ(ModelNamed("o253010").bones[1], "lever_01");
    // Where each one's own clip leaves it after use: the library lever and the Research Hall
    // capstan at their stops, the Lecture Building's partway back (61.4 degrees), every other one
    // back at rest.
    for (const auto& m : LM::Models) {
        EXPECT_EQ(LM::StaysAtStop(m),
                  std::strcmp(m.name, "o253010") == 0 || std::strcmp(m.name, "o353010") == 0)
            << m.name;
        if (std::strcmp(m.name, "o321110") == 0)
            EXPECT_NEAR(LM::EndAngle(m), 61.39f * B::Deg, 1e-5f);
        else if (!LM::StaysAtStop(m))
            EXPECT_EQ(m.end_deg, 0.0f) << m.name;
    }
    EXPECT_EQ(LM::EndAngle(ModelNamed("o253010")), ModelNamed("o253010").sweep_deg * B::Deg);
}

// The rigs measured from the user's dump on 2026-10-14: each binds on its own row, its grip lies
// where its mesh's grip is (in the lever's parent frame, at rest and at the clip's stop), the
// grabbable shaft runs from above the hub to the grip, and the arming radius is about 3 m.
TEST(LeverModels, MeasuredRigsLieWhereTheirMeshIs) {
    struct Expect {
        const char* model;
        std::int32_t row;
        Vec3 rest, stop; // the grip, in the lever bone's parent frame
    };
    const std::array<Expect, 6> rigs{{
        // Hemwick: the bulb 1.27 m up behind the hinge, 1.00 m up in front at the stop.
        {"o220005", 100, {-0.0003f, 1.2683f, -0.6725f}, {-0.0003f, 0.9999f, 1.0553f}},
        // Mensis lift lever: rests unrotated (its clip starts 5 degrees back).
        {"o260041", 2600000, {0.0042f, 1.1458f, -0.5945f}, {0.0042f, 1.0664f, 0.7935f}},
        // Yahar'gul: the crook's middle, under its raised root.
        {"o280001", 100, {0.004f, 1.029f, -0.7192f}, {0.004f, 0.9637f, 0.8719f}},
        // Fishing Hamlet: the knob 1.41 m out on the rod in the mesh, drawn at the bone's own
        // scale (0.84) 1.18 m from the hinge, over the top about the object's +X.
        {"o360010", 100, {-0.0009f, 1.1045f, -0.7311f}, {-0.0009f, 1.1095f, 0.8223f}},
        // Mensis cage levers (wave 2): the bulb 1.10 m up behind the hinge, the same in front.
        {"o260040", 2600000, {0.0009f, 1.0994f, -0.6408f}, {0.0009f, 1.0999f, 0.7371f}},
        // Byrgenwerth's Lecture Building lever (wave 2): resting 5 degrees back.
        {"o321110", 100, {0.0025f, 1.0328f, -0.6496f}, {0.0025f, 1.0347f, 0.7444f}},
    }};
    const B::QsTransform root{};
    for (const auto& e : rigs) {
        const auto& m = ModelNamed(e.model);
        EXPECT_FALSE(LM::StaysAtStop(m)) << e.model;
        const auto rest_pose = RestOf(m);
        EXPECT_EQ(LM::Match(SkeletonOf(m), root, rest_pose, e.row), &m) << e.model;
        EXPECT_EQ(LM::Match(SkeletonOf(m), root, rest_pose, 2410000), nullptr) << e.model;
        const auto rig = LM::BindRig(LM::Binding(m, {}, rest_pose));
        ASSERT_TRUE(rig.has_value()) << e.model;
        EXPECT_NEAR(rig->sweep, m.sweep_deg * B::Deg, 1e-5f) << e.model;
        // Every one of them swings its grip over the top about the object's +X.
        EXPECT_NEAR(rig->axis.x, 1.0f, 0.001f) << e.model;
        const auto rest = LM::HandleAt(*rig, 0), stop = LM::HandleAt(*rig, rig->sweep);
        EXPECT_LT(B::Length(rest - e.rest), 0.002f) << e.model;
        EXPECT_LT(B::Length(stop - e.stop), 0.002f) << e.model;
        // The shaft starts on the moving stem above the hub, below and in front of the grip.
        const auto shaft = rig->pivot + rig->shaft_start;
        EXPECT_GT(shaft.y, rig->pivot.y + 0.3f) << e.model;
        EXPECT_LT(shaft.y, rest.y - 0.2f) << e.model;
        EXPECT_GT(shaft.z, rest.z) << e.model;
        EXPECT_GT(LM::ArmRadius(*rig), 2.8f) << e.model;
        EXPECT_LT(LM::ArmRadius(*rig), 3.2f) << e.model;
    }
}

// o321110's own clip pulls 80.8 degrees and leaves the lever 61.4 degrees from rest (its last
// frame): a completed hand pull settles there. The bone written there is the clip's last frame.
TEST(LeverModels, LectureLeverEndsWhereItsClipLeavesIt) {
    const auto& lecture = ModelNamed("o321110");
    const auto binding = LM::Binding(lecture, {}, RestOf(lecture));
    const auto rig = LM::BindRig(binding);
    ASSERT_TRUE(rig.has_value());
    const float end = LM::EndAngle(lecture);
    EXPECT_GT(end, 0.0f);
    EXPECT_LT(end, rig->sweep);
    const auto target = LM::BoneTarget(binding, end);
    ASSERT_TRUE(target.has_value());
    const B::Quat clip_end{0.4725564f, 0.0f, 0.0f, 0.8813004f}; // frame 90 (the last)
    EXPECT_GT(std::abs(B::Dot(B::Normalize(target->q), clip_end)), 0.99999f);
    const auto handle = LM::HandleAt(*rig, end);
    EXPECT_LT(B::Length(handle - Vec3{0.0025f, 1.2192f, 0.4327f}), 0.002f);
    EXPECT_LT(B::Length(handle - Drawn({lecture.rest_t, clip_end, lecture.rest_s}, lecture.handle)),
              0.001f);
}

// o280001's root sits 0.153 m up in its reference pose; its pull clip carries it at the origin.
// Either rest binds (the rig follows the live root), any other root offset or a turned root not.
TEST(LeverModels, RaisedRootBindsAtItsReferenceOrTheOrigin) {
    const auto& yahargul = ModelNamed("o280001");
    const B::QsTransform rest_pose{yahargul.rest_t, yahargul.rest_q, {1, 1, 1}};
    const auto skeleton = SkeletonOf(yahargul);
    const B::QsTransform raised{yahargul.root_t, {}, {1, 1, 1}};
    EXPECT_EQ(LM::Match(skeleton, raised, rest_pose, 100), &yahargul);
    EXPECT_EQ(LM::Match(skeleton, {}, rest_pose, 100), &yahargul);
    EXPECT_EQ(LM::Match(skeleton, {yahargul.root_t * 0.5f, {}, {1, 1, 1}}, rest_pose, 100), nullptr);
    EXPECT_EQ(LM::Match(skeleton, {yahargul.root_t, B::AxisAngle({0, 1, 0}, 0.1f), {1, 1, 1}},
                        rest_pose, 100),
              nullptr);
    // The game's own pull leaves the lever 0.9 degrees and 0.6 mm off its reference rest: it
    // still binds there (its clip's last frame).
    const B::QsTransform after_game_pull{{0.0f, 0.0617381f, 0.0384846f},
                                         {-0.03558f, 0.0f, 0.0f, 0.999367f}, {1, 1, 1}};
    EXPECT_EQ(LM::Match(skeleton, {}, after_game_pull, 100), &yahargul);
    // A raised root is the model's own: no other model accepts it.
    const auto& yharnam = ModelNamed("o240001");
    EXPECT_EQ(LM::Match(SkeletonOf(yharnam), raised, {yharnam.rest_t, yharnam.rest_q, {1, 1, 1}},
                        2410000),
              nullptr);
    // The rig hinges at the raised root plus the lever's rest: 0.214 m up, as on o240001.
    const auto rig = LM::BindRig(LM::Binding(yahargul, raised, rest_pose));
    ASSERT_TRUE(rig.has_value());
    EXPECT_NEAR(rig->pivot.y, 0.214f, 0.001f);
    EXPECT_NEAR(rig->pivot.z, 0.048f, 0.001f);
    EXPECT_NEAR(LM::HandleAt(*rig, 0).y, 1.182f, 0.002f); // the crook, 1.18 m up
}

// ---- The Research Hall capstan (o353010: Motion::Capstan) -----------------------------------------

// Its rig, measured from the user's dump: the bar along the capstan bone's local +X at 1.182 m up,
// from 1.80 m (outside the wheel's 1.66 m rim) to its end collar at 2.90 m, turns 90.3 degrees about
// the object's +Y, from the object's -X to its +Z, exactly as the game's own clip turns it, and
// stays there (its clip has no return).
TEST(LeverModels, CapstanRigLiesOnItsBar) {
    const auto& m = ModelNamed("o353010");
    EXPECT_EQ(m.motion, LM::Motion::Capstan);
    EXPECT_TRUE(LM::StaysAtStop(m));
    EXPECT_EQ(m.lever, 2);
    EXPECT_EQ(m.actions, (std::array<std::int32_t, 3>{3500110, 0, 0}));
    const auto locals = LocalsOf(m);
    // Its root and column compose to no rotation: the bar's parent is the object itself.
    const auto parent = LM::ParentWorld(m, Live(locals, m), {});
    EXPECT_LT(B::Length(parent.t), 1e-6f);
    EXPECT_GT(std::abs(parent.q.w), 0.999999f);
    const auto binding = LM::Binding(m, parent, RestOf(m));
    EXPECT_EQ(binding.motion, LM::Motion::Capstan);
    const auto rig = LM::BindRig(binding);
    ASSERT_TRUE(rig.has_value());
    EXPECT_EQ(rig->motion, LM::Motion::Capstan);
    EXPECT_LT(B::Length(rig->pivot), 1e-6f); // the hub's axis through the object's origin
    EXPECT_LT(B::Length(rig->axis - Vec3{0, 1, 0}), 1e-5f);
    EXPECT_NEAR(rig->sweep, 90.3353f * B::Deg, 1e-5f);
    const auto rest = LM::HandleAt(*rig, 0), stop = LM::HandleAt(*rig, rig->sweep);
    // The handle (2.90, 1.182, -0.0025) turned 180 degrees, then 90.3353 degrees round: the bar's
    // 2.5 mm offset from the hub's line turns with it.
    EXPECT_LT(B::Length(rest - Vec3{-2.90f, 1.182f, 0.0025f}), 0.0005f);
    EXPECT_LT(B::Length(stop - Vec3{0.0195f, 1.182f, 2.8999f}), 0.0005f);
    // The shaft: the bar's inner end, outside the wheel's rim, at the bar's height.
    const auto shaft = rig->pivot + rig->shaft_start;
    EXPECT_NEAR(shaft.y, 1.182f, 0.001f);
    EXPECT_NEAR(B::Length(LM::Radial(rig->shaft_start, rig->axis)), 1.80f, 0.001f);
    EXPECT_NEAR(B::Length(LM::Radial(rig->rest, rig->axis)), 2.90f, 0.001f);
    // The bone written at the stop is the clip's last frame (130), and halfway its frame 63.
    const auto target = LM::BoneTarget(binding, rig->sweep);
    ASSERT_TRUE(target.has_value());
    EXPECT_GT(std::abs(B::Dot(B::Normalize(target->q), B::Quat{0, 0.705034876f, 0, -0.709172633f})),
              0.99999f);
    const auto half = LM::BoneTarget(binding, 44.798f * B::Deg);
    ASSERT_TRUE(half.has_value());
    EXPECT_GT(std::abs(B::Dot(B::Normalize(half->q), B::Quat{0, 0.924551487f, 0, -0.381057145f})),
              0.99999f);
    // Armed within about 4.9 m of the hub: the whole swept bar (3 m out) and its stand point.
    EXPECT_NEAR(LM::ArmRadius(*rig), 3.132f + LM::ArmMargin, 0.005f);
    EXPECT_LT(B::Length(Vec3{-2.119f, 0.154f, -0.78f} + Vec3{0, 1.6f, 0}), LM::ArmRadius(*rig));
    // A lever stick's rig this long would not be valid; the capstan's is.
    auto as_stick = *rig;
    as_stick.motion = LM::Motion::Pull;
    EXPECT_FALSE(LM::Valid(as_stick));
    EXPECT_TRUE(LM::Valid(*rig));
}

// It binds only as the game shows it unused: its root and column at rest (the idle clip's 90.06
// degrees and the skeleton's reference pose both are), its bar at rest, on its own row. Turned by
// its clip, or its column lifted by the next one (the map script's, once it is turned), it keeps
// the game's own interaction.
TEST(LeverModels, CapstanBindsOnlyAtRestUnderItsColumn) {
    const auto& m = ModelNamed("o353010");
    const auto s = SkeletonOf(m);
    const auto rest = LocalsOf(m);
    EXPECT_EQ(LM::Match(s, Live(rest, m), 3500110), &m);
    const auto with = [&](auto&& change) {
        auto l = rest;
        change(l);
        return LM::Match(s, Live(l, m), 3500110);
    };
    EXPECT_EQ(with([](auto& l) {
                  l[0].q = {-0.707451338f, 0, 0, 0.706762056f};
                  l[1].q = {0.707451338f, 0, 0, 0.706762056f};
              }),
              &m);
    EXPECT_EQ(with([](auto& l) {
                  l[0] = {{0, 0, 1.31233704e-10f},
                          {-0.707106769f, 0, 0, 0.707106709f},
                          {0.99999994f, 0.999999821f, 0.999999821f}};
                  l[1] = {{0, 1.31233677e-10f, 2.34664125e-17f},
                          {0.707106829f, 0, 0, 0.70710665f},
                          {1, 1.00000012f, 1.00000012f}};
                  l[2] = {{0, -4.9630821e-24f, -2.77555624e-17f},
                          {3.38813219e-21f, 0.99999994f, -3.5527141e-15f, -4.01339292e-07f},
                          {0.999999702f, 0.99999994f, 0.999999702f}};
              }),
              &m);
    EXPECT_EQ(with([](auto& l) { l[2].q = {0, 0.705034876f, 0, -0.709172633f}; }), nullptr);
    EXPECT_EQ(with([](auto& l) { l[1].t = {0, 0, 6.2168889f}; }), nullptr);
    EXPECT_EQ(with([](auto& l) {
                  l[1].t = {0, 0, 10.1999969f};
                  l[2].q = {0, -0.705034876f, 0, 0.709172633f};
              }),
              nullptr);
    EXPECT_EQ(with([](auto& l) { l[0].q = {}; }), nullptr);
    // Both ancestors unturned (the same net rotation): another skeleton state, not this one.
    EXPECT_EQ(with([](auto& l) {
                  l[0].q = {};
                  l[1].q = {};
              }),
              nullptr);
    // Any other row (a stick's, the Chalice turning levers'), and the root-and-lever form: nothing.
    for (const auto row : {100, 2410000, 2600000, 2931100, 9902})
        EXPECT_EQ(LM::Match(s, Live(rest, m), row), nullptr) << row;
    EXPECT_EQ(LM::Match(s, rest[0], rest[2], 3500110), nullptr);
    EXPECT_EQ(LM::Match(s, B::QsTransform{}, rest[2], 3500110), nullptr);
    // The Chalice turning levers (o293110 / 12 / 14): two bones, never this model.
    for (const auto& c : ChaliceLevers) {
        if (std::strcmp(c.model, "o293110") != 0)
            continue;
        std::array<B::QsTransform, LM::MaxBones> chalice{};
        chalice[1] = c.lever;
        for (const auto row : {3500110, 2931100, 9902})
            EXPECT_EQ(LM::Match(SkeletonOf(c), std::span<const B::QsTransform>{chalice.data(), 2}, row),
                      nullptr)
                << row;
    }
}

namespace {
// The capstan's rig at the object's origin, in metres, as the game binds it.
LM::Rig CapstanRig() {
    const auto& m = ModelNamed("o353010");
    return *LM::BindRig(LM::Binding(m, LM::ParentWorld(m, Live(LocalsOf(m), m), {}), RestOf(m)));
}
// A point on its bar `r` metres out from the hub (on the bar's centreline), the bar at `angle`.
Vec3 OnBar(const LM::Rig& rig, float r, float angle) {
    const auto radial = LM::Radial(rig.rest, rig.axis);
    const auto out = radial * (1.0f / B::Length(radial));
    return rig.pivot + LM::Rotate(out * r + rig.axis * B::Dot(rig.rest, rig.axis), rig.axis, angle);
}
L::Input CapstanFrame(float angle = 0.0f) {
    auto in = Frame();
    in.candidate.rig = CapstanRig();
    in.candidate.angle = angle;
    for (auto& hand : in.hands)
        hand.position = {0, 5, 0}; // nowhere near
    return in;
}
} // namespace

// The hand may slide along the bar (toward or away from the hub) without slipping or turning it;
// the bar's angle follows the hand's angle about the hub; the pinned fist slides with the hand;
// only leaving the bar (0.3 m) slips. A lever stick keeps where the hand caught it instead.
TEST(PhysicalLever, CapstanContactSlidesAlongTheBar) {
    auto in = CapstanFrame();
    const auto rig = in.candidate.rig;
    L::Tracker t;
    in.hands[0].position = OnBar(rig, 2.0f, 0);
    auto out = Grab(t, in);
    ASSERT_TRUE(out.grabbed);
    // (The bar's centreline runs 2.5 mm beside the hub's: OnBar's radial line is within 1 mm.)
    EXPECT_LT(B::Length(out.pin - in.hands[0].position), 2e-3f);
    for (int i = 1; i <= 30; ++i) { // 0.6 m out along the bar
        in.hands[0].position = OnBar(rig, 2.0f + 0.6f * i / 30, 0);
        out = t.Step(in);
        ASSERT_TRUE(out.holding) << i;
        EXPECT_NEAR(out.angle, 0.0f, 1e-3f);
        EXPECT_LT(B::Length(out.pin - in.hands[0].position), 2e-3f) << i;
    }
    EXPECT_LT(B::Length(out.contact_rest - (OnBar(rig, 2.6f, 0) - rig.pivot)), 2e-3f);
    for (int i = 1; i <= 30; ++i) { // walked 30 degrees round, 2.6 m out
        in.hands[0].position = OnBar(rig, 2.6f, 30.0f * B::Deg * i / 30);
        out = t.Step(in);
        ASSERT_TRUE(out.holding) << i;
    }
    EXPECT_NEAR(out.angle, 30.0f * B::Deg, 0.5f * B::Deg);
    EXPECT_LT(B::Length(out.pin - in.hands[0].position), 2e-3f);
    EXPECT_LT(B::Length(t.Expected() - in.hands[0].position), 2e-3f);
    // And back in toward the hub while turned: still no slip, still no turn.
    const float turned = out.angle;
    for (int i = 1; i <= 30; ++i) {
        in.hands[0].position = OnBar(rig, 2.6f - 0.7f * i / 30, turned);
        out = t.Step(in);
        ASSERT_TRUE(out.holding) << i;
    }
    EXPECT_NEAR(out.angle, turned, 1e-3f);
    // Off the bar, 0.31 m above it: slipped.
    const auto on = in.hands[0].position;
    for (int i = 1; i <= 10 && out.holding; ++i) {
        in.hands[0].position = on + Vec3{0, 0.031f * i, 0};
        out = t.Step(in);
    }
    EXPECT_FALSE(out.holding);
    EXPECT_EQ(out.ended, L::End::Slipped);

    // A lever stick (its shaft from 0.4 to 1.0 m up): the same slide along it slips.
    auto stick = Frame();
    stick.candidate.rig = {{}, {1, 0, 0}, {0, 1.0f, 0}, 1.2f, {0, 0.4f, 0}, true};
    L::Tracker s;
    stick.hands[0].position = {0, 0.55f, 0};
    ASSERT_TRUE(Grab(s, stick).grabbed);
    L::Output slid{};
    for (int i = 1; i <= 20 && (i == 1 || slid.holding); ++i) {
        stick.hands[0].position = {0, 0.55f + 0.4f * i / 20, 0};
        slid = s.Step(stick);
    }
    EXPECT_FALSE(slid.holding);
    EXPECT_EQ(slid.ended, L::End::Slipped);
}

// A capstan's reach is its own: valid to 5 m, where a lever stick's rig stops at 2 m. It completes
// within its last 3% (a stick at 60%); its motion survives binding and the metre mapping, and a
// change of motion under a held hand loses the hold.
TEST(PhysicalLever, CapstanValidityCommitAndIdentity) {
    auto rig = CapstanRig();
    EXPECT_TRUE(L::Valid(rig));
    auto stick = rig;
    stick.motion = L::Motion::Pull;
    EXPECT_FALSE(L::Valid(stick));
    auto far = rig;
    far.rest = {-5.1f, 1.182f, 0};
    EXPECT_FALSE(L::Valid(far));
    far = rig;
    far.shaft_start = {-4.9f, 1.182f, 0}; // 5.04 m from the pivot
    EXPECT_FALSE(L::Valid(far));
    auto unknown = rig;
    unknown.motion = static_cast<L::Motion>(7);
    EXPECT_FALSE(L::Valid(unknown));
    EXPECT_FLOAT_EQ(L::CommitFor(L::Motion::Pull), L::CommitProgress);
    EXPECT_FLOAT_EQ(L::CommitFor(L::Motion::Capstan), 0.97f);
    L::BoneBinding b{{}, {}, {0, 1, 0}, {2.9f, 1.182f, 0}, 1.5766f, {1.8f, 1.182f, 0}, true};
    EXPECT_FALSE(L::BindRig(b)); // a stick 2.9 m long
    b.motion = L::Motion::Capstan;
    const auto bound = L::BindRig(b);
    ASSERT_TRUE(bound);
    EXPECT_EQ(bound->motion, L::Motion::Capstan);
    VR::Body::Anchor a{};
    a.valid = true;
    a.steady_ns = 1'000'000'000;
    a.world_scale = 1.0f;
    const auto space = L::Space::From(a, a.steady_ns);
    ASSERT_TRUE(space);
    const auto metres = space->RigInMetres(rig);
    ASSERT_TRUE(metres);
    EXPECT_EQ(metres->motion, L::Motion::Capstan);
    // Held as a capstan, offered as a stick of the same geometry (a 1 m bar): the hold ends.
    auto in = Frame();
    in.candidate.rig = {{}, {0, 1, 0}, {-1.0f, 1.0f, 0}, 1.5f, {-0.5f, 1.0f, 0}, true,
                        L::Motion::Capstan};
    in.hands[0].position = L::HandleAt(in.candidate.rig, 0);
    L::Tracker t;
    ASSERT_TRUE(Grab(t, in).grabbed);
    in.candidate.rig.motion = L::Motion::Pull;
    const auto out = t.Step(in);
    EXPECT_FALSE(out.holding);
    EXPECT_EQ(out.ended, L::End::CandidateLost);
}

// A parked capstan (the game offers it at the angle the hand left it) is taken there without a
// jump, ratchets only at detents past it, and completes at its stop; let go within its last 3% it
// is a completion too (the game's settle carries the rest).
TEST(PhysicalLever, ParkedCapstanResumesWithoutAJumpAndCompletesAtItsStop) {
    const float parked = 0.6f;
    auto in = CapstanFrame(parked);
    const auto rig = in.candidate.rig;
    L::Tracker t;
    in.hands[1].position = OnBar(rig, 2.4f, parked);
    auto out = Grab(t, in, 1);
    ASSERT_TRUE(out.grabbed);
    EXPECT_FLOAT_EQ(out.angle, parked);
    EXPECT_FALSE(out.ratchet);
    EXPECT_LT(B::Length(out.pin - in.hands[1].position), 2e-3f);
    // Nowhere near the bar's rest line: a hand there does not take it.
    L::Tracker other;
    auto at_rest = CapstanFrame(parked);
    at_rest.hands[0].position = OnBar(rig, 2.4f, 0);
    EXPECT_FALSE(Grab(other, at_rest).holding);
    int ratchets = 0;
    for (int i = 1; i <= 60 && out.holding; ++i) {
        in.hands[1].position = OnBar(rig, 2.4f, parked + (rig.sweep + 0.05f - parked) * i / 60);
        out = t.Step(in);
        ratchets += out.ratchet;
    }
    EXPECT_TRUE(out.completed);
    EXPECT_FLOAT_EQ(out.angle, rig.sweep);
    EXPECT_EQ(ratchets, 4); // detents 4..7 (parked within detent 3); the clunk replaces the 8th

    auto again = CapstanFrame(parked);
    L::Tracker u;
    again.hands[0].position = OnBar(rig, 2.4f, parked);
    ASSERT_TRUE(Grab(u, again).grabbed);
    for (int i = 1; i <= 40; ++i) {
        again.hands[0].position = OnBar(rig, 2.4f, parked + (0.975f * rig.sweep - parked) * i / 40);
        out = u.Step(again);
    }
    ASSERT_TRUE(out.holding);
    EXPECT_FALSE(out.completed);
    again.hands[0].grip = 0;
    out = u.Step(again);
    EXPECT_EQ(out.ended, L::End::Released);
    EXPECT_GE(out.progress, L::CommitFor(L::Motion::Capstan));
}

// The hunter walks a capstan round with the stick: the game moves the hand in its own frame once per
// game frame (30 Hz), in steps a headset frame (120 Hz) would take for a jump, and a snap turn
// swings the hand about the head. The hold measures the tracked hand's own motion instead, so
// neither drops it, and the bar follows the hand round to its stop. A real tracking glitch, or a
// warp of the hunter, still ends it; a lever stick keeps its check in the game's frame.
TEST(PhysicalLever, CapstanWalkedRoundWithTheStickKeepsItsHold) {
    const auto rig = CapstanRig();
    const Vec3 still{0.2f, 1.1f, -0.4f}; // the real hand, held still in front of the player
    for (const float speed : {1.6f, 2.5f, 4.0f, 6.0f}) { // m/s along the bar's arc, 2.4 m out
        auto in = CapstanFrame();
        in.dt = 1.0f / 120.0f;
        in.hands[0].tracking = still;
        in.hands[0].position = OnBar(rig, 2.4f, 0);
        L::Tracker t;
        ASSERT_TRUE(Grab(t, in).grabbed) << speed;
        const float game_step = speed / 30.0f / 2.4f; // radians per game frame
        if (speed >= 2.5f) // a step the game-frame check would have taken for a jump
            ASSERT_GT(B::Length(OnBar(rig, 2.4f, game_step) - OnBar(rig, 2.4f, 0)),
                      L::MaxHandSpeed * in.dt + 0.01f);
        L::Output out{};
        float body = 0.0f;
        for (int frame = 1; frame <= 4000; ++frame) {
            if (frame % 4 == 0) // the game frame: the hunter (and the hand with him) steps on
                body = std::min(body + game_step, rig.sweep + 0.02f);
            in.hands[0].position = OnBar(rig, 2.4f, body);
            out = t.Step(in);
            if (!out.holding)
                break;
        }
        EXPECT_TRUE(out.completed) << speed;
        EXPECT_EQ(out.ended, L::End::None) << speed;
    }

    // A snap turn (30 degrees about the head, 0.58 m from the hand) mid-push: the hand swings
    // 0.26 m in the game's frame in one headset frame; the bar follows it, the hold stays.
    auto in = CapstanFrame();
    in.dt = 1.0f / 120.0f;
    in.hands[0].tracking = still;
    in.hands[0].position = OnBar(rig, 2.4f, 0.3f);
    in.candidate.angle = 0.3f;
    L::Tracker t;
    ASSERT_TRUE(Grab(t, in).grabbed);
    const Vec3 head = in.hands[0].position + Vec3{0.3f, 0.3f, 0.4f};
    const Vec3 swung = head + LM::Rotate(in.hands[0].position - head, {0, 1, 0}, 30.0f * B::Deg);
    ASSERT_GT(B::Length(swung - in.hands[0].position), L::MaxHandSpeed * in.dt + 0.01f);
    in.hands[0].position = swung;
    auto out = t.Step(in);
    EXPECT_TRUE(out.holding);
    EXPECT_EQ(out.ended, L::End::None);
    EXPECT_GT(std::abs(out.angle - 0.3f), 0.01f); // the bar went with the hand
    // A tracking glitch (the tracked hand 0.3 m away in one frame): a jump.
    in.hands[0].tracking = still + Vec3{0.3f, 0, 0};
    out = t.Step(in);
    EXPECT_FALSE(out.holding);
    EXPECT_EQ(out.ended, L::End::Discontinuity);
    // A warp of the hunter (2 m in the game's frame, the tracked hand still): a jump too.
    L::Tracker w;
    in = CapstanFrame();
    in.hands[0].tracking = still;
    in.hands[0].position = OnBar(rig, 2.4f, 0);
    ASSERT_TRUE(Grab(w, in).grabbed);
    in.hands[0].position = OnBar(rig, 2.4f, 0) + Vec3{0, 0, 2.0f};
    out = w.Step(in);
    EXPECT_EQ(out.ended, L::End::Discontinuity);
    // An untracked (not finite) tracking position: no motion, a jump.
    L::Tracker n;
    in = CapstanFrame();
    in.hands[0].position = OnBar(rig, 2.4f, 0);
    ASSERT_TRUE(Grab(n, in).grabbed);
    in.hands[0].tracking = {std::numeric_limits<float>::quiet_NaN(), 0, 0};
    out = n.Step(in);
    EXPECT_EQ(out.ended, L::End::Discontinuity);

    // A lever stick: its hand stepping 8 cm along the pull in one 120 Hz frame jumped, whatever
    // the tracked hand did (its hunter stands still).
    auto stick = Frame();
    stick.dt = 1.0f / 120.0f;
    L::Tracker s;
    stick.hands[0].position = L::HandleAt(stick.candidate.rig, 0.1f);
    stick.candidate.angle = 0.1f;
    ASSERT_TRUE(Grab(s, stick).grabbed);
    stick.hands[0].position = L::HandleAt(stick.candidate.rig, 0.26f); // 8 cm round, 0.5 m out
    out = s.Step(stick);
    EXPECT_EQ(out.ended, L::End::Discontinuity);
}

// A capstan is pushed with both hands: the other hand's squeeze on the held bar is the lever's (it
// never reaches the game: no weapon transform, no item arc to end the hold), and it does not take
// the hold. Let go, it is free again. Away from the bar, blocked, or at a lever stick, a squeeze
// stays the game's.
TEST(PhysicalLever, CapstanSecondHandOnTheBarIsTheLevers) {
    auto in = CapstanFrame();
    const auto rig = in.candidate.rig;
    L::Tracker t;
    in.hands[0].position = OnBar(rig, 2.6f, 0);
    ASSERT_TRUE(Grab(t, in).grabbed);
    // The other hand on the bar, nearer the hub (5 cm above its line), squeezes.
    in.hands[1].position = OnBar(rig, 2.0f, 0) + Vec3{0, 0.05f, 0};
    auto out = t.Step(in);
    EXPECT_FALSE(out.owns_grip[1]);
    in.hands[1].grip = 1;
    out = t.Step(in);
    EXPECT_TRUE(out.holding);
    EXPECT_EQ(out.hand, 0);
    EXPECT_TRUE(out.owns_grip[0]);
    EXPECT_TRUE(out.owns_grip[1]);
    // Both walk it round; it turns with the holding hand, the second hand's squeeze stays owned.
    for (int i = 1; i <= 20; ++i) {
        in.hands[0].position = OnBar(rig, 2.6f, 0.4f * i / 20);
        in.hands[1].position = OnBar(rig, 2.0f, 0.4f * i / 20);
        out = t.Step(in);
        ASSERT_TRUE(out.holding) << i;
        EXPECT_EQ(out.hand, 0);
        EXPECT_TRUE(out.owns_grip[1]) << i;
    }
    EXPECT_NEAR(out.angle, 0.4f, 1e-3f);
    // Let go: that grip is its own again; the hold goes on.
    in.hands[1].grip = 0;
    out = t.Step(in);
    EXPECT_FALSE(out.owns_grip[1]);
    EXPECT_TRUE(out.holding);
    // A squeeze 0.5 m above the bar stays the game's; so does a blocked hand's on it.
    in.hands[1].position = OnBar(rig, 2.0f, 0.4f) + Vec3{0, 0.5f, 0};
    in.hands[1].grip = 1;
    EXPECT_FALSE(t.Step(in).owns_grip[1]);
    in.hands[1].grip = 0;
    t.Step(in);
    in.hands[1].position = OnBar(rig, 2.0f, 0.4f);
    in.hands[1].blocked = true;
    in.hands[1].grip = 1;
    EXPECT_FALSE(t.Step(in).owns_grip[1]);
    in.hands[1].blocked = false;
    in.hands[1].grip = 0;
    t.Step(in);
    // On the bar again; then the holding hand lets go: the hold ends, and the second hand's
    // squeeze stays owned until it lets go too.
    in.hands[1].grip = 1;
    EXPECT_TRUE(t.Step(in).owns_grip[1]);
    in.hands[0].grip = 0;
    out = t.Step(in);
    EXPECT_EQ(out.ended, L::End::Released);
    EXPECT_FALSE(out.holding);
    EXPECT_FALSE(out.owns_grip[0]);
    EXPECT_TRUE(out.owns_grip[1]);
    in.hands[1].grip = 0;
    EXPECT_FALSE(t.Step(in).owns_grip[1]);

    // A lever stick: the other hand's squeeze at its very handle stays the game's.
    auto stick = Frame();
    L::Tracker s;
    stick.hands[1].position = {0, 5, 0};
    ASSERT_TRUE(Grab(s, stick).grabbed);
    stick.hands[1].position = L::HandleAt(stick.candidate.rig, 0);
    stick.hands[1].grip = 1;
    out = s.Step(stick);
    EXPECT_TRUE(out.holding);
    EXPECT_FALSE(out.owns_grip[1]);
}

// The bone a capstan's hold left turned and forgot (parked) is taken again only while it is byte
// for byte as left, and only with its true rest, which a later restore puts back.
TEST(PhysicalLever, PoseEditResumesAParkedBoneAndRestoresItsTrueRest) {
    LeverPoseFixture f;
    const auto at_rest = f.locals;
    L::PoseEdit first;
    ASSERT_TRUE(first.Begin(f.binding, f.Pose(), 1));
    ASSERT_TRUE(first.Apply(0.7f, f.Pose(), f.Queue()));
    const auto original = first.Original();
    const auto last = first.Last();
    EXPECT_EQ(std::memcmp(&original, &at_rest[1], sizeof(original)), 0);
    EXPECT_EQ(std::memcmp(&last, &f.locals[1], sizeof(last)), 0);
    first.Forget(); // parked: nothing is restored
    EXPECT_EQ(std::memcmp(&f.locals[1], &last, sizeof(last)), 0);
    f.sync = 0x0101;
    L::PoseEdit edit;
    // Bound at the parked pose instead of its true rest: refused.
    auto at_parked = f.binding;
    at_parked.rest_local = VR::Body::FromHavok(last);
    EXPECT_FALSE(edit.Resume(at_parked, f.Pose(), 1, original, last));
    // The bone changed by one ulp since: refused (and nothing written).
    f.locals[1].q[0] = std::nextafter(f.locals[1].q[0], 2.0f);
    const auto touched = f.locals;
    EXPECT_FALSE(edit.Resume(f.binding, f.Pose(), 1, original, last));
    EXPECT_EQ(std::memcmp(f.locals.data(), touched.data(), sizeof(touched)), 0);
    f.locals[1] = last;
    // Another bone, a bone out of range, or buffers mid-update: refused.
    EXPECT_FALSE(edit.Resume(f.binding, f.Pose(), 0, original, last));
    EXPECT_FALSE(edit.Resume(f.binding, f.Pose(), 9, original, last));
    f.sync = 0;
    EXPECT_FALSE(edit.Resume(f.binding, f.Pose(), 1, original, last));
    f.sync = 0x0101;
    EXPECT_FALSE(edit.Owned());
    // Resumed: it carries on from the parked bone, and a restore puts back the true rest.
    ASSERT_TRUE(edit.Resume(f.binding, f.Pose(), 1, original, last));
    EXPECT_TRUE(edit.Owned());
    EXPECT_FALSE(edit.Resume(f.binding, f.Pose(), 1, original, last)); // already owned
    EXPECT_EQ(std::memcmp(&edit.Original(), &original, sizeof(original)), 0);
    ASSERT_TRUE(edit.Apply(1.1f, f.Pose(), f.Queue()));
    EXPECT_LT(VR::Body::AngleBetween(VR::Body::FromHavok(f.locals[1]).q,
                                     L::BoneTarget(f.binding, 1.1f)->q),
              1e-6f);
    ASSERT_TRUE(edit.Restore(f.Pose(), f.Queue()));
    EXPECT_EQ(std::memcmp(f.locals.data(), at_rest.data(), sizeof(at_rest)), 0);
    EXPECT_FALSE(edit.Owned());
}

// A parked capstan's record binds only its own object, untouched: the same address, entry, handle
// and row; its ancestors at rest; its bar byte for byte as the hand left it; an angle within its
// stops (its stop included: a refused completion parks it there); its true rest the model's own.
TEST(LeverModels, StillParkedOnlyForItsOwnUntouchedBar) {
    const auto& m = ModelNamed("o353010");
    const auto s = SkeletonOf(m);
    const auto rest = LocalsOf(m);
    std::array<B::HkQsTransform, LM::MaxBones> raw{};
    for (std::size_t i = 0; i < m.count; ++i)
        B::ToHavok(rest[i], raw[i]);
    const auto original = raw[m.lever];
    const auto binding = LM::Binding(m, LM::ParentWorld(m, Live(rest, m), {}), RestOf(m));
    const float angle = 0.6f;
    auto last = original;
    B::ToHavok(*LM::BoneTarget(binding, angle), last);
    raw[m.lever] = last;
    const LM::Parked p{0x1000, 0x2000, 0x100001, &m, original, last, angle};
    const auto live = [](const auto& r) { return std::span<const B::HkQsTransform>{r.data(), 3}; };
    EXPECT_EQ(LM::StillParked(p, s, live(raw), 0x1000, 0x2000, 0x100001, 3500110), &m);
    // Turned, it is no rest pose: only the record binds it.
    std::array<B::QsTransform, LM::MaxBones> turned{};
    for (std::size_t i = 0; i < m.count; ++i)
        turned[i] = B::FromHavok(raw[i]);
    EXPECT_EQ(LM::Match(s, Live(turned, m), 3500110), nullptr);
    // Another object, entry, handle or row.
    EXPECT_EQ(LM::StillParked(p, s, live(raw), 0x1001, 0x2000, 0x100001, 3500110), nullptr);
    EXPECT_EQ(LM::StillParked(p, s, live(raw), 0x1000, 0x2001, 0x100001, 3500110), nullptr);
    EXPECT_EQ(LM::StillParked(p, s, live(raw), 0x1000, 0x2000, 0x100002, 3500110), nullptr);
    for (const auto row : {0, 100, 2931100})
        EXPECT_EQ(LM::StillParked(p, s, live(raw), 0x1000, 0x2000, 0x100001, row), nullptr) << row;
    // Its bar touched (one ulp), its column lifted, its root turned, too few bones, another layout.
    auto changed = raw;
    changed[2].q[1] = std::nextafter(changed[2].q[1], 2.0f);
    EXPECT_EQ(LM::StillParked(p, s, live(changed), 0x1000, 0x2000, 0x100001, 3500110), nullptr);
    changed = raw;
    changed[1].t[2] = 10.2f;
    EXPECT_EQ(LM::StillParked(p, s, live(changed), 0x1000, 0x2000, 0x100001, 3500110), nullptr);
    changed = raw;
    B::ToHavok(B::QsTransform{}, changed[0]);
    EXPECT_EQ(LM::StillParked(p, s, live(changed), 0x1000, 0x2000, 0x100001, 3500110), nullptr);
    EXPECT_EQ(LM::StillParked(p, s, std::span<const B::HkQsTransform>{raw.data(), 2}, 0x1000, 0x2000,
                              0x100001, 3500110),
              nullptr);
    auto other = s;
    other.mesh_map[0] = 0;
    EXPECT_EQ(LM::StillParked(p, other, live(raw), 0x1000, 0x2000, 0x100001, 3500110), nullptr);
    // Its angle: at rest, back past it, past the stop or not a number: no; exactly at its stop: yes.
    const float sweep = m.sweep_deg * B::Deg;
    for (const float a : {0.0f, -0.1f, sweep + 0.01f, std::numeric_limits<float>::quiet_NaN()}) {
        auto q = p;
        q.angle = a;
        EXPECT_EQ(LM::StillParked(q, s, live(raw), 0x1000, 0x2000, 0x100001, 3500110), nullptr) << a;
    }
    auto at_stop = p;
    at_stop.angle = sweep;
    EXPECT_EQ(LM::StillParked(at_stop, s, live(raw), 0x1000, 0x2000, 0x100001, 3500110), &m);
    // No model, a lever stick's (sticks never park), or a rest that is not the model's own.
    auto q = p;
    q.model = nullptr;
    EXPECT_EQ(LM::StillParked(q, s, live(raw), 0x1000, 0x2000, 0x100001, 3500110), nullptr);
    q.model = &ModelNamed("o240001");
    EXPECT_EQ(LM::StillParked(q, s, live(raw), 0x1000, 0x2000, 0x100001, 3500110), nullptr);
    q = p;
    q.original = last;
    EXPECT_EQ(LM::StillParked(q, s, live(raw), 0x1000, 0x2000, 0x100001, 3500110), nullptr);
    // An empty record never matches (not even a null object).
    EXPECT_EQ(LM::StillParked(LM::Parked{}, s, live(raw), 0, 0, 0, 3500110), nullptr);
}

// The XR side measures reach to a parked capstan's bar where it stands (the game's word carries its
// angle), not at its rest line; its arm radius covers the whole swept bar.
TEST(LeverArming, ParkedCapstanArmsAndReachesAtItsBar) {
    const auto rig = CapstanRig();
    const float parked = 1.0f; // 57 degrees round
    const std::int64_t t0 = 10'000'000'000;
    auto in = ArmedAt(rig, t0);
    in.angle = parked;
    in.head = OnBar(rig, 2.6f, parked) + Vec3{0, 0.5f, 0};
    in.hands = {OnBar(rig, 2.4f, 0), OnBar(rig, 2.4f, parked)}; // the rest line; the bar now
    LM::ArmState state;
    auto out = LM::Arm(state, in);
    ASSERT_TRUE(out.armed_near);
    EXPECT_LT(out.distance[1], 0.01f);
    EXPECT_TRUE(out.reaching[1]);
    EXPECT_TRUE(out.tick[1]);
    EXPECT_GT(out.distance[0], 1.0f);
    EXPECT_FALSE(out.reaching[0]);
    EXPECT_FALSE(out.tick[0]);
    // At rest, the reverse.
    LM::ArmState at_rest;
    in.angle = 0;
    out = LM::Arm(at_rest, in);
    EXPECT_TRUE(out.reaching[0]);
    EXPECT_FALSE(out.reaching[1]);
    // Turned to its stop, a hunter at its far end is still near.
    LM::ArmState turned;
    in.angle = rig.sweep;
    in.head = OnBar(rig, 3.4f, rig.sweep) + Vec3{0, 0.5f, 0};
    in.hands = {OnBar(rig, 2.8f, rig.sweep), OnBar(rig, 2.8f, rig.sweep)};
    out = LM::Arm(turned, in);
    EXPECT_TRUE(out.near);
    EXPECT_TRUE(out.reaching[0]);
    EXPECT_NEAR(LM::ArmRadius(rig), 4.88f, 0.01f);
}

// Headset 2026-10-14: the hand slipped off the gate lever at 96% and the lever sprang back.
// A slip settles like a release: past 60% it completes.
TEST(PhysicalLever, SlippingOffPastCommitSettlesLikeARelease) {
    EXPECT_TRUE(VR::Lever::SettlesOnEnd(VR::Lever::End::Released));
    EXPECT_TRUE(VR::Lever::SettlesOnEnd(VR::Lever::End::Slipped));
    for (auto end : {VR::Lever::End::None, VR::Lever::End::Context, VR::Lever::End::CandidateLost,
                     VR::Lever::End::Tracking, VR::Lever::End::Busy,
                     VR::Lever::End::Discontinuity})
        EXPECT_FALSE(VR::Lever::SettlesOnEnd(end));
}
