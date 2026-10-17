// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "core/vr/interaction_quick.h"
#include "core/vr/quick_interaction_mailbox.h"
#include "quick_dll_controller.h"
namespace Q = VR::Quick;
namespace P = PhysicalInteractions;
namespace {
struct Rig {
    VR::Interactions::Module module;
    Q::Config config{};
    Q::Input input{};
    PiQuickManagedOutput output{};
    uint64_t sequence{}, candidate{1}, character{9};
    double t{100};
    Rig() {
        if (!module.Load(QuickDllTest::path))
            std::abort();
        config.select = false;
        input.gameplay = input.body = input.hand_valid = input.snapshot_fresh = true;
        input.arm_weight = 1;
        input.hand_p = {-.2f, 1.1f, -.3f};
        input.snapshot.valid = true;
        input.snapshot.selected = 0;
        input.snapshot.slot[0] = {80, 1120, 5, 24, -1, true};
    }
    uint32_t Call(PiQuickManagedInput packet) {
        packet.size = sizeof(packet);
        packet.session = module.Session();
        packet.sequence = ++sequence;
        output.size = sizeof(output);
        const auto status = module.QuickManaged(packet, output);
        if (status == PI_OK && output.action_pending)
            candidate = std::max(candidate, output.action.request_id + 1);
        return status;
    }
    void Step(int count = 1) {
        for (int i = 0; i < count; ++i) {
            input.t = t += 1. / 90;
            input.dt = 1.f / 90;
            input.hover.steady_ns = static_cast<int64_t>(t * 1e9);
            PiQuickManagedInput packet{};
            packet.operation = PI_QUICK_MANAGED_STEP;
            packet.candidate_id = candidate;
            packet.character_generation = character;
            packet.config = P::QuickPackets::ToPacket(config);
            packet.input = P::QuickPackets::ToPacket(input);
            ASSERT_EQ(Call(packet), PI_OK);
        }
    }
    void Grab() {
        input.y = true;
        Step();
        input.y = false;
        Step();
        input.hover_fresh = true;
        input.hover.open_serial = output.open_serial;
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
    uint32_t Result(const PiQuickResult& result) {
        PiQuickManagedInput p{};
        p.operation = PI_QUICK_MANAGED_RESULT;
        p.result = result;
        return Call(p);
    }
    uint32_t Reject(const PiQuickRequest& request) {
        PiQuickManagedInput p{};
        p.operation = PI_QUICK_MANAGED_NOT_EXECUTED;
        p.rejected_id = request.request_id;
        p.rejected_session = request.session;
        return Call(p);
    }
    void Cancel() {
        PiQuickManagedInput p{};
        p.operation = PI_QUICK_MANAGED_CANCEL;
        P::QuickPackets::Copy(p.cancel_reason, "cancel");
        ASSERT_EQ(Call(p), PI_OK);
    }
};
PiQuickResult Complete(Q::InteractionMailbox& box, uint32_t ticket,
                       Q::Outcome outcome = Q::Outcome::Used) {
    if (!box.Finish({ticket, outcome, "done", 1120, 5, 4}))
        std::abort();
    PiQuickResult result{};
    if (!box.Poll(result))
        std::abort();
    return result;
}
} // namespace
TEST(QuickManaged, DelayedNativeResultSettlesOnceThroughDll) {
    Rig rig;
    rig.Use();
    ASSERT_TRUE(rig.output.action_pending);
    EXPECT_EQ(rig.output.phase, static_cast<unsigned>(Q::Phase::Waiting));
    const auto action = rig.output.action;
    Q::InteractionMailbox box;
    const auto ticket = box.Submit(action);
    Q::Request native{};
    ASSERT_TRUE(box.Claim(9, native));
    for (int i = 0; i < 900; ++i) {
        rig.Step();
        ASSERT_TRUE(rig.output.action_pending);
        ASSERT_EQ(rig.output.action.request_id, action.request_id);
        ASSERT_FALSE(rig.output.decision.send);
        ASSERT_EQ(rig.output.decision.haptic_count, 0);
    }
    auto result = Complete(box, ticket);
    auto bad = result;
    ++bad.request_id;
    EXPECT_EQ(rig.Result(bad), PI_BAD_PACKET);
    ASSERT_EQ(rig.Result(result), PI_OK);
    EXPECT_FALSE(rig.output.action_pending);
    EXPECT_EQ(rig.Result(result), PI_BAD_PACKET);
    rig.Step();
    EXPECT_EQ(rig.output.phase, static_cast<unsigned>(Q::Phase::Idle));
    EXPECT_GT(rig.output.decision.haptic_count, 0);
    rig.Step();
    EXPECT_EQ(rig.output.decision.haptic_count, 0);
}
TEST(QuickManaged, ResetRetainsClaimedIdentityButNotFeedbackOrHeldEdges) {
    Rig rig;
    rig.Use();
    const auto action = rig.output.action;
    Q::InteractionMailbox box;
    const auto ticket = box.Submit(action);
    Q::Request native{};
    ASSERT_TRUE(box.Claim(9, native));
    rig.module.Reset();
    rig.sequence = 0;
    PiQuickManagedInput poll{};
    poll.operation = PI_QUICK_MANAGED_SNAPSHOT;
    ASSERT_EQ(rig.Call(poll), PI_OK);
    ASSERT_TRUE(rig.output.cancel_requested);
    EXPECT_EQ(rig.output.action.session, action.session);
    EXPECT_FALSE(box.Cancel(ticket));
    rig.input.y = true;
    rig.Step(3);
    EXPECT_FALSE(rig.output.decision.send);
    EXPECT_FALSE(rig.output.decision.square_press);
    ASSERT_EQ(rig.Result(Complete(box, ticket)), PI_OK);
    rig.Step();
    EXPECT_EQ(rig.output.decision.haptic_count, 0);
    EXPECT_EQ(rig.output.phase, static_cast<unsigned>(Q::Phase::Idle));
    rig.input.y = false;
    rig.Step();
    rig.input.y = true;
    rig.Step();
    EXPECT_EQ(rig.output.phase, static_cast<unsigned>(Q::Phase::Open));
}
TEST(QuickManaged, UnclaimedCancellationAndQueueRejectionAreExact) {
    for (bool cancel : {false, true}) {
        Rig rig;
        rig.Use();
        const auto action = rig.output.action;
        Q::InteractionMailbox box;
        const auto ticket = box.Submit(action);
        if (cancel)
            rig.Cancel();
        ASSERT_TRUE(box.Cancel(ticket));
        auto wrong = action;
        ++wrong.request_id;
        EXPECT_EQ(rig.Reject(wrong), PI_BAD_PACKET);
        ASSERT_EQ(rig.Reject(action), PI_OK);
        EXPECT_EQ(rig.Reject(action), PI_BAD_PACKET);
        rig.Step();
        EXPECT_EQ(rig.output.decision.haptic_count, cancel ? 0 : 1);
        EXPECT_FALSE(rig.output.decision.send);
    }
}
TEST(QuickManaged, BusySelectionCannotBeOverwrittenByUse) {
    Rig rig;
    rig.config.select = true;
    rig.input.snapshot.selected = 1;
    rig.Grab();
    ASSERT_TRUE(rig.output.action_pending);
    const auto selection = rig.output.action;
    ASSERT_EQ(selection.action, PI_QUICK_SELECT);
    Q::InteractionMailbox box;
    const auto ticket = box.Submit(selection);
    Q::Request native{};
    ASSERT_TRUE(box.Claim(9, native));
    rig.Step(12);
    rig.input.trigger = 1;
    rig.Step(12);
    EXPECT_TRUE(rig.output.cancel_requested);
    EXPECT_EQ(rig.output.action.request_id, selection.request_id);
    EXPECT_FALSE(rig.output.decision.send);
    ASSERT_EQ(rig.Result(Complete(box, ticket, Q::Outcome::Selected)), PI_OK);
    rig.Step();
    EXPECT_FALSE(rig.output.decision.send);
    EXPECT_EQ(rig.output.decision.haptic_count, 0);
}
TEST(QuickManaged, CharacterChangeAfterCompletionDropsOldFeedback) {
    Rig rig;
    rig.Use();
    Q::InteractionMailbox box;
    const auto ticket = box.Submit(rig.output.action);
    Q::Request native{};
    ASSERT_TRUE(box.Claim(9, native));
    ASSERT_EQ(rig.Result(Complete(box, ticket)), PI_OK);
    ++rig.character;
    rig.Step();
    EXPECT_EQ(rig.output.decision.haptic_count, 0);
    EXPECT_FALSE(rig.output.decision.send);
}

TEST(QuickRuntime, ProductionChannelDelaysAndResetKeepOneNativeUse) {
    Rig rig;
    bool fault = false;
    VR::Interactions::QuickRuntime runtime;
    runtime.Bind(rig.module, fault);
    Q::ObserveInteractionActor(0x1234, 77);
    Q::PublishSnapshot(rig.input.snapshot);
    ASSERT_TRUE(Q::LatestSnapshot(rig.input.snapshot));
    P::QuickOutput output{};
    auto step = [&](int frames = 1) {
        for (int i = 0; i < frames; ++i) {
            rig.input.t = rig.t += 1. / 90;
            rig.input.dt = 1.f / 90;
            rig.input.hover.steady_ns = static_cast<int64_t>(rig.t * 1e9);
            runtime.Update(rig.config, rig.input, output);
            EXPECT_FALSE(fault);
            EXPECT_FALSE(output.send);
        }
    };
    // A reset seeds the held-button state; the next real edge opens the menu.
    rig.module.Reset();
    step();
    rig.input.y = true;
    step();
    rig.input.y = false;
    step();
    rig.input.hover_fresh = true;
    rig.input.hover.open_serial = output.scene.open_serial;
    rig.input.hover.placed = true;
    rig.input.hover.slot = 0;
    step(9);
    rig.input.grip = 1;
    step(13);
    rig.input.trigger = 1;
    step(12);
    ASSERT_EQ(runtime.GetPhase(), Q::Phase::Waiting);
    ASSERT_TRUE(Q::InteractionRequestBusy());
    Q::Request native{};
    ASSERT_TRUE(Q::LatestRequest(native));
    Q::Request duplicate{};
    EXPECT_FALSE(Q::LatestRequest(duplicate));
    step(900);
    EXPECT_EQ(runtime.GetPhase(), Q::Phase::Waiting);
    rig.module.Reset();
    runtime.Service();
    EXPECT_TRUE(Q::InteractionRequestBusy());
    Q::PublishResult({native.serial, Q::Outcome::Used, "done", 1120, 5, 4});
    step();
    EXPECT_FALSE(Q::InteractionRequestBusy());
    EXPECT_EQ(runtime.GetPhase(), Q::Phase::Idle);
    EXPECT_EQ(output.haptic_count, 0);
}
