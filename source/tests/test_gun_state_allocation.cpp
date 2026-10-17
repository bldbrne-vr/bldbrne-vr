// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "physical_interactions/gun_state.h"
#include "quick_allocation_probe.h"
static __declspec(noinline) uint32_t RunGunFrame(PhysicalInteractions::GunState& state,
                                                 const PiGunInput& input, PiGunOutput& output) {
    return state.Step(input, output);
}
TEST(GunFrameAllocation, BoundedFramesAndResetAllocateNothing) {
    PhysicalInteractions::GunState state;
    PiGunInput in{};
    PiGunOutput out{};
    in.operation = PI_GUN_FRAME;
    in.facts.bow_hand = 1;
    in.facts.grip.q.w = 1;
    in.facts.fresh = in.facts.wrist_reload = in.facts.fire_direct = in.facts.direct_ok = 1;
    in.facts.family = 1;
    in.facts.player_epoch = 1;
    uint64_t checksum = 0;
    for (int i = 1; i <= 32000; ++i) {
        in.sequence = i;
        in.facts.now_ns = 1'000'000'000 + i * 11'111'111LL;
        in.facts.trigger = i % 7 == 0;
        out = {};
        uint32_t status;
        {
            QuickAllocationProbe::Scope probe("gun frame");
            status = RunGunFrame(state, in, out);
        }
        ASSERT_EQ(status, PI_OK);
        checksum += out.flags + out.reload_pending + out.request_serial;
    }
    EXPECT_GT(checksum, 0u);
    {
        QuickAllocationProbe::Scope probe("gun reset");
        state.Reset();
    }
}

#include "physical_interactions/saw_state.h"
static __declspec(noinline) uint32_t RunSawFrame(PhysicalInteractions::SawState& state,
                                                 const PiSawInput& in, PiSawOutput& out) {
    return state.Step(in, out);
}
TEST(SawFrameAllocation, SpinAndRumbleAllocateNothing) {
    PhysicalInteractions::SawState state;
    PiSawInput in{};
    PiSawOutput out{};
    in.operation = PI_SAW_FRAME;
    in.dt = 1.f / 90;
    in.enabled = in.status_ok = in.status_saw = in.form_b = in.right_saw = in.engaged = in.allowed =
        in.haptics = 1;
    uint64_t checksum = 0;
    for (int i = 1; i <= 32000; ++i) {
        in.sequence = i;
        in.now_ns = 1'000'000'000 + i * 11'111'111LL;
        in.trigger_left = (i % 500) < 200;
        in.trigger_right = (i % 500) > 100;
        in.seated = (i % 1200) < 900;
        out = {};
        uint32_t status;
        {
            QuickAllocationProbe::Scope probe("saw frame");
            status = RunSawFrame(state, in, out);
        }
        ASSERT_EQ(status, PI_OK);
        checksum += out.flags + out.spinning;
        in.left_level = out.left_level;
        in.right_level = out.right_level;
    }
    EXPECT_GT(checksum, 0u);
    {
        QuickAllocationProbe::Scope probe("saw reset");
        state.Reset();
    }
}

#include "physical_interactions/blade_state.h"
static __declspec(noinline) uint32_t RunBladeFrame(PhysicalInteractions::BladeState& state,
                                                   const PiBladeInput& in, PiBladeOutput& out) {
    return state.Step(in, out);
}
TEST(BladeFrameAllocation, ChargesWavesAndResetAllocateNothing) {
    PhysicalInteractions::BladeState state;
    PiBladeInput in{};
    PiBladeOutput out{};
    in.operation = PI_BLADE_FRAME;
    in.dt = 1.f / 90;
    in.charge_s = .3f;
    in.cadence_s = 1;
    in.swings = in.haptics = in.shot_weapon = 1;
    in.forward[2] = in.gaze[2] = -1;
    in.head[1] = 1.6f;
    in.right[1] = 1.3f;
    in.right[2] = -.3f;
    in.axis[2] = -1;
    uint64_t checksum = 0;
    for (int i = 1; i <= 32000; ++i) {
        in.sequence = i;
        in.now_ns = 4'000'000'000 + i * 11'111'111LL;
        in.trigger = (i % 200) < 130;
        in.shot = (i % 1000) < 700 ? PI_BLADE_STAKE : PI_BLADE_MOON;
        in.window = 1 + i / 100;
        in.open = in.committed = in.armed = in.charged = 1;
        in.tip_peak = 4;
        in.tip_speed = (i % 100) < 50 ? 4 : 2;
        out = {};
        uint32_t status;
        {
            QuickAllocationProbe::Scope probe("blade frame");
            status = RunBladeFrame(state, in, out);
        }
        ASSERT_EQ(status, PI_OK);
        checksum += out.flags + out.charging;
        if (out.flags & PI_BLADE_WRITE_SUSTAIN)
            in.sustain = out.sustain;
    }
    EXPECT_GT(checksum, 0u);
    {
        QuickAllocationProbe::Scope probe("blade reset");
        state.Reset();
    }
}

#include "physical_interactions/melee_state.h"
static __declspec(noinline) uint32_t RunMeleeFrame(PhysicalInteractions::MeleeState& state,
                                                   const PiMeleeInput& in, PiMeleeOutput& out) {
    return state.Step(in, out);
}
TEST(MeleeFrameAllocation, ClassifiersLinkedWindowsAndTelegraphsAllocateNothing) {
    namespace MP = PhysicalInteractions::MeleePackets;
    PhysicalInteractions::MeleeState state;
    PiMeleeInput in{};
    PiMeleeOutput out{};
    in.operation = PI_MELEE_WINDOW_POOL;
    in.target = 1;
    in.serial = 1;
    in.sequence = 1;
    ASSERT_EQ(state.Step(in, out), PI_OK);
    VR::Melee::Config cfg{};
    cfg.arc_enabled = true;
    in.config = MP::Pack(cfg);
    VR::Melee::TrackerConfig tc{};
    in.tracker_config = MP::Pack(tc);
    VR::Melee::TelegraphConfig gc{};
    in.telegraph_config = MP::Pack(gc);
    uint64_t checksum = 0;
    for (int i = 2; i <= 32001; ++i) {
        in.sequence = i;
        in.serial = 0;
        in.target = i % 2;
        VR::Melee::Input motion{};
        motion.t = i / 90.;
        motion.active = motion.ori_valid = motion.pos_valid = motion.head_ok = true;
        const float angle = std::sin(i * .08f);
        motion.q = {std::sin(angle / 2), 0, 0, std::cos(angle / 2)};
        motion.p = {.2f, 1.3f, -.4f};
        motion.head_p = {0, 1.6f, 0};
        motion.v = {0, 3 * std::cos(i * .08f), -3};
        in.motion = MP::Pack(motion);
        VR::Melee::TrackerInput ti{};
        ti.dt = 1.f / 90;
        ti.swings = ti.stroke_live = true;
        ti.stroke = 1 + i / 100;
        ti.blade_rate = ti.stroke_peak = 8;
        ti.hand_speed = 3;
        ti.tip_speed = 6;
        ti.hand_v = {0, 0, -3};
        ti.fired = i % 100 == 30;
        ti.fired_stroke = ti.kind_stroke = ti.stroke;
        ti.kind = ti.fired ? VR::Melee::Kind::Light : VR::Melee::Kind::None;
        in.tracker_motion = MP::Pack(ti);
        VR::Melee::TelegraphInput gi{};
        gi.dt = 1.f / 90;
        gi.swings = gi.stroke_live = true;
        gi.stroke = ti.stroke;
        gi.windup = "raising";
        gi.stroke_arc = 2;
        in.telegraph_motion = MP::Pack(gi);
        in.operation = i % 3 == 0   ? PI_MELEE_CLASS_FRAME
                       : i % 3 == 1 ? PI_MELEE_WINDOW_FRAME
                                    : PI_MELEE_TELEGRAPH_FRAME;
        out = {};
        uint32_t status;
        {
            QuickAllocationProbe::Scope probe("melee frame");
            status = RunMeleeFrame(state, in, out);
        }
        ASSERT_EQ(status, PI_OK);
        checksum += out.decision.flags + out.tracker.flags + out.telegraph.fire;
    }
    EXPECT_GT(checksum, 0u);
    {
        QuickAllocationProbe::Scope probe("melee reset");
        state.Reset();
    }
}

TEST(WhipFrameAllocation, RevolutionsCracksFeedbackAndResetAllocateNothing) {
    namespace MP = PhysicalInteractions::MeleePackets;
    PhysicalInteractions::MeleeState state;
    PiMeleeInput in{};
    PiMeleeOutput out{};
    VR::Melee::TrackerConfig cfg{};
    in.tracker_config = MP::Pack(cfg);
    uint64_t checksum = 0;
    for (int i = 1; i <= 32000; ++i) {
        VR::Melee::TrackerInput ti{};
        ti.dt = 1.f / 90;
        ti.swings = true;
        ti.hand_v = {3 * std::cos(i * .15f), 0, 3 * std::sin(i * .15f)};
        ti.hand_speed = 3;
        ti.tip_speed = 8;
        in.tracker_motion = MP::Pack(ti);
        in.whip.now_ns = in.whip.feel_ns = 4'000'000'000 + i * 11'111'111LL;
        in.whip.flags =
            PI_MWF_FORM | PI_MWF_DRIVING | PI_MWF_SWINGS | PI_MWF_HAPTICS | PI_MWF_ASKED_OUT;
        in.whip.lash = in.whip.locks = i / 100;
        in.whip.reels = i / 150;
        in.whip.out = 1;
        in.whip.tip_speed = 18;
        in.whip.payout_speed = 3;
        in.sequence = i;
        in.operation = i % 2 ? PI_MELEE_PHYSICAL_FRAME : PI_MELEE_WHIP_FEEDBACK;
        out = {};
        uint32_t status;
        {
            QuickAllocationProbe::Scope probe("whip frame");
            status = RunMeleeFrame(state, in, out);
        }
        ASSERT_EQ(status, PI_OK);
        checksum += out.whip.flags + out.whip.revolutions + out.whip.cracks;
    }
    EXPECT_GT(checksum, 0u);
    {
        QuickAllocationProbe::Scope probe("whip reset");
        state.Reset();
    }
}
