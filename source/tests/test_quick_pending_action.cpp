// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "core/vr/quick_interaction_mailbox.h"
#include "physical_interactions/quick_pending_action.h"
#include "quick_allocation_probe.h"
namespace Q = VR::Quick;
namespace P = PhysicalInteractions;
namespace {
template <class Controller>
struct Rig {
    Controller controller;
    Q::Config config{};
    Q::Input input{};
    P::QuickOutput output{};
    Q::Request sent{};
    double t = 100;
    Rig(int goods = 1120) {
        config.select = false;
        input.gameplay = input.body = input.hand_valid = input.snapshot_fresh = true;
        input.arm_weight = 1;
        input.hand_p = {-.2f, 1.1f, -.3f};
        input.snapshot.valid = true;
        input.snapshot.selected = 0;
        input.snapshot.slot[0] = {80, goods, 5, 24, -1, true};
    }
    void Step(int n = 1) {
        for (int i = 0; i < n; ++i) {
            input.t = t += 1.0 / 90;
            input.dt = 1.f / 90;
            input.hover.steady_ns = static_cast<int64_t>(t * 1e9);
            {
                QuickAllocationProbe::Scope scope{"definitive controller"};
                controller.Update(config, input, output);
            }
            if (output.send)
                sent = output.request;
        }
    }
    void Grab() {
        input.y = true;
        Step();
        input.y = false;
        Step();
        input.hover_fresh = true;
        input.hover.open_serial = controller.OpenSerial();
        input.hover.placed = true;
        input.hover.slot = 0;
        Step(9);
        input.grip = 1;
        Step();
    }
    void Use() {
        Grab();
        Step(12);
        input.trigger = 1;
        Step(12);
    }
};
PiQuickRequest Envelope(uint64_t id, uint64_t session = 1) {
    Q::Request request{};
    request.serial = 4;
    request.action = Q::Action::Use;
    request.goods = 1120;
    request.slot = 0;
    return P::QuickActions::Pack(request, session, id, 9);
}
} // namespace
TEST(QuickDefinitiveResults, DelayedUseRetainsWaitingUntilMatchingResult) {
    Rig<P::ConfirmedQuickController> rig;
    rig.Use();
    ASSERT_EQ(rig.controller.GetPhase(), Q::Phase::Waiting);
    ASSERT_EQ(rig.sent.action, Q::Action::Use);
    const auto serial = rig.sent.serial;
    for (int i = 0; i < 900; ++i) {
        rig.Step();
        ASSERT_EQ(rig.controller.GetPhase(), Q::Phase::Waiting);
        ASSERT_FALSE(rig.output.send);
        ASSERT_EQ(rig.output.haptic_count, 0);
    }
    rig.input.have_result = true;
    rig.input.result = {serial + 1, Q::Outcome::Used, "", 1120, 5, 4};
    rig.Step();
    EXPECT_EQ(rig.controller.GetPhase(), Q::Phase::Waiting);
    rig.input.result.serial = serial;
    rig.Step();
    EXPECT_EQ(rig.controller.GetPhase(), Q::Phase::Idle);
    rig.Step();
    EXPECT_FALSE(rig.output.send);
    EXPECT_EQ(rig.output.haptic_count, 0);
    Rig<P::QuickController> legacy;
    legacy.Use();
    legacy.Step(900);
    EXPECT_EQ(legacy.controller.GetPhase(), Q::Phase::Idle);
}
TEST(QuickDefinitiveResults, RepeatedCranksCannotReplaceUnresolvedPlay) {
    Rig<P::ConfirmedQuickController> rig(700);
    rig.config.crank = true;
    rig.Grab();
    ASSERT_EQ(rig.controller.GetPhase(), Q::Phase::Holding);
    ASSERT_TRUE(rig.output.crank_box);
    rig.input.crank_play = true;
    rig.Step();
    ASSERT_TRUE(rig.output.send);
    const auto first = rig.sent;
    EXPECT_TRUE(first.crank);
    for (int i = 0; i < 900; ++i) {
        rig.Step();
        ASSERT_FALSE(rig.output.send);
        ASSERT_EQ(rig.output.crank_answer, 0);
    }
    rig.input.have_result = true;
    rig.input.result = {first.serial + 1, Q::Outcome::Used, "", 700, 1, 1};
    rig.Step();
    EXPECT_FALSE(rig.output.send);
    rig.input.result.serial = first.serial;
    rig.input.crank_play = false;
    rig.Step();
    EXPECT_EQ(rig.output.crank_answer, 1);
    rig.Step();
    EXPECT_EQ(rig.output.crank_answer, 0);
    rig.input.crank_play = true;
    rig.Step();
    ASSERT_TRUE(rig.output.send);
    EXPECT_GT(rig.sent.serial, first.serial);
}
TEST(QuickPendingAction, ResetRetainsIdentityAndSuppressesLateFeedbackOwnership) {
    P::QuickPendingAction pending;
    Q::InteractionMailbox mailbox;
    const auto request = Envelope(1);
    ASSERT_TRUE(pending.Begin(request, 1));
    auto ticket = mailbox.Submit(request);
    ASSERT_NE(ticket, 0);
    Q::Request native{};
    ASSERT_TRUE(mailbox.Claim(9, native));
    pending.Cancel(); // the controller/session may now reset independently
    EXPECT_TRUE(pending.Cancelled());
    EXPECT_FALSE(mailbox.Cancel(ticket));
    EXPECT_FALSE(pending.Begin(Envelope(2, 2), 2));
    ASSERT_TRUE(mailbox.Finish({native.serial, Q::Outcome::Used, "used", 1120, 5, 4}));
    PiQuickResult result{};
    ASSERT_TRUE(mailbox.Poll(result));
    auto stale = result;
    ++stale.character_generation;
    EXPECT_EQ(pending.Resolve(stale), P::QuickPendingAction::Resolution::Invalid);
    EXPECT_TRUE(pending.Pending());
    auto wrong_kind = result;
    wrong_kind.outcome = PI_QUICK_SELECTED;
    EXPECT_EQ(pending.Resolve(wrong_kind), P::QuickPendingAction::Resolution::Invalid);
    auto wrong_goods = result;
    ++wrong_goods.goods;
    EXPECT_EQ(pending.Resolve(wrong_goods), P::QuickPendingAction::Resolution::Invalid);
    EXPECT_EQ(pending.Resolve(result), P::QuickPendingAction::Resolution::Cancelled);
    EXPECT_EQ(pending.Resolve(result), P::QuickPendingAction::Resolution::Invalid);
    EXPECT_FALSE(pending.Pending());
    EXPECT_FALSE(pending.Begin(request, 1)); // completed watermark retained
    EXPECT_TRUE(pending.Begin(Envelope(2, 2), 2));
}
TEST(QuickPendingAction, RejectionAndUnclaimedCancellationRequireExactIdentity) {
    P::QuickPendingAction pending;
    EXPECT_FALSE(pending.Begin(Envelope(1, 1), 2));
    ASSERT_TRUE(pending.Begin(Envelope(1), 1));
    EXPECT_EQ(pending.NotExecuted(2, 1), P::QuickPendingAction::Resolution::Invalid);
    EXPECT_EQ(pending.NotExecuted(1, 2), P::QuickPendingAction::Resolution::Invalid);
    EXPECT_TRUE(pending.Pending());
    EXPECT_EQ(pending.NotExecuted(1, 1), P::QuickPendingAction::Resolution::Current);
    ASSERT_TRUE(pending.Begin(Envelope(2), 1));
    pending.Cancel();
    pending.Cancel();
    EXPECT_EQ(pending.NotExecuted(1, 2), P::QuickPendingAction::Resolution::Cancelled);
    EXPECT_FALSE(pending.Begin(Envelope(2), 1));
    const auto next = Envelope(3);
    ASSERT_TRUE(pending.Begin(next, 1));
    PiQuickResult result{};
    ASSERT_TRUE(
        P::QuickActions::PackResult(next, {4, Q::Outcome::Denied, "denied", 1120, 5, 5}, result));
    EXPECT_EQ(pending.Resolve(result), P::QuickPendingAction::Resolution::Current);
    EXPECT_EQ(pending.Resolve(result), P::QuickPendingAction::Resolution::Invalid);
}

TEST(QuickDefinitiveResults, SquareWaitNeedsIdentifiedSelectionBeforeSnapshot) {
    for (bool accepted : {false, true}) {
        Rig<P::ConfirmedQuickController> rig(1411);
        rig.input.snapshot.selected = 1;
        rig.Use();
        ASSERT_EQ(rig.controller.GetPhase(), Q::Phase::SquareWait);
        ASSERT_EQ(rig.sent.action, Q::Action::None);
        const auto serial = rig.sent.serial;
        rig.input.snapshot.selected = 0; // observation alone does not acknowledge our selection
        for (int i = 0; i < 900; ++i) {
            rig.Step();
            ASSERT_EQ(rig.controller.GetPhase(), Q::Phase::SquareWait);
            ASSERT_FALSE(rig.output.square_press);
            ASSERT_FALSE(rig.output.send);
        }
        rig.input.have_result = true;
        rig.input.result = {
            serial, accepted ? Q::Outcome::Selected : Q::Outcome::Denied, "", 1411, 1, 1};
        rig.Step();
        EXPECT_EQ(rig.output.square_press, accepted);
        EXPECT_EQ(rig.controller.GetPhase(), Q::Phase::Idle);
        rig.Step();
        EXPECT_FALSE(rig.output.square_press);
    }
}
