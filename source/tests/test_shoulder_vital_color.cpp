#include <limits>

#include "core/vr/shoulder_switch.h"
#include "core/vr/vital_color.h"
#include <gtest/gtest.h>
namespace {
namespace S = VR::ShoulderSwitch;
S::Input Ready() {
    S::Input i{};
    i.enabled = i.gameplay = i.tracked = i.available = true;
    i.head = {0, 1.6f, 0};
    i.hand = {.20f, 1.40f, .08f};
    return i;
}
void Settle(S::Gate &gate, S::Input &i, float hz = 90) {
    for (int n = 0; n < int(.2f * hz); ++n)
        EXPECT_FALSE(gate.Update(i, 1 / hz).request);
}
TEST(ShoulderSwitch, TapFiresOnReleaseOnly) {
    for (float hz : {30.f, 60.f, 90.f, 120.f}) {
        S::Gate g;
        auto i = Ready();
        Settle(g, i, hz);
        i.b = true;
        auto o = g.Update(i, 1 / hz);
        EXPECT_FALSE(o.request);
        EXPECT_TRUE(o.consume);
        for (int n = 0; n < int(hz); ++n) {
            o = g.Update(i, 1 / hz);
            EXPECT_FALSE(o.request);
            EXPECT_TRUE(o.consume);
        }
        i.b = false;
        o = g.Update(i, 1 / hz);
        EXPECT_TRUE(o.request);
        EXPECT_FALSE(o.unarm);
        EXPECT_FALSE(o.consume);
        Settle(g, i, hz);
    }
}
TEST(ShoulderSwitch, HeldButtonEnteringZoneOrRegainingTrackingNeverSwitches) {
    S::Gate g;
    auto i = Ready();
    i.hand = {0, 1, -.4f};
    Settle(g, i);
    i.b = true;
    EXPECT_FALSE(g.Update(i, .02f).request);
    i.hand = {.2f, 1.4f, .08f};
    Settle(g, i);
    i.tracked = false;
    g.Update(i, .02f);
    i.tracked = true;
    Settle(g, i);
    i.b = false;
    Settle(g, i);
    i.b = true;
    EXPECT_FALSE(g.Update(i, .02f).request);
    i.b = false;
    EXPECT_TRUE(g.Update(i, .02f).request);
}
TEST(ShoulderSwitch, StartupAndMenuHeldBNeedRelease) {
    S::Gate g;
    auto i = Ready();
    i.b = true;
    Settle(g, i);
    i.gameplay = false;
    g.Update(i, .02f);
    i.gameplay = true;
    Settle(g, i);
    i.b = false;
    Settle(g, i);
    i.b = true;
    EXPECT_FALSE(g.Update(i, .02f).request);
    i.b = false;
    EXPECT_TRUE(g.Update(i, .02f).request);
}
TEST(ShoulderSwitch, OverheadAttackConsumesButCannotQueueSwitch) {
    for (int guard = 0; guard < 3; ++guard) {
        S::Gate g;
        auto i = Ready();
        Settle(g, i);
        if (guard == 0)
            i.velocity = {0, 2, 0};
        if (guard == 1)
            i.angular_speed = 5;
        if (guard == 2)
            i.attacking = true;
        i.b = true;
        auto o = g.Update(i, .02f);
        EXPECT_FALSE(o.request);
        EXPECT_TRUE(o.consume);
        i.velocity = {};
        i.angular_speed = 0;
        i.attacking = false;
        Settle(g, i);
    }
}
TEST(ShoulderSwitch, DisabledAndRollChordDoNotStealB) {
    S::Gate g;
    auto i = Ready();
    Settle(g, i);
    i.chord = true;
    i.b = true;
    auto o = g.Update(i, .02f);
    EXPECT_FALSE(o.request);
    EXPECT_FALSE(o.consume);
    i.b = false;
    i.chord = false;
    i.enabled = false;
    Settle(g, i);
    i.b = true;
    o = g.Update(i, .02f);
    EXPECT_FALSE(o.request);
    EXPECT_FALSE(o.consume);
}
TEST(ShoulderSwitch, ReleaseOutsideZoneDoesNotLeakOwnedB) {
    S::Gate g;
    auto i = Ready();
    Settle(g, i);
    i.b = true;
    ASSERT_TRUE(g.Update(i, .02f).consume);
    i.hand = {0, 1, -.5f};
    EXPECT_TRUE(g.Update(i, .02f).consume);
    i.b = false;
    EXPECT_FALSE(g.Update(i, .02f).consume);
}
TEST(ShoulderSwitch, BothShouldersHaveLargerForwardReachButRejectOutside) {
    for (bool left : {false, true}) {
        for (int location = 0; location < 4; ++location) {
            S::Gate gate;
            auto i = Ready();
            i.left = left;
            const float side = left ? -1.f : 1.f;
            // Front and lateral points outside the old 17 cm zone; then outside
            // the 24 cm radius (22 until 2026-10-15), and the opposite shoulder.
            i.hand = location == 0   ? VR::Body::Vec3{side * .20f, 1.40f, -.24f}
                     : location == 1 ? VR::Body::Vec3{side * .39f, 1.40f, -.06f}
                     : location == 2 ? VR::Body::Vec3{side * .20f, 1.40f, -.31f}
                                     : VR::Body::Vec3{-side * .20f, 1.40f, 0};
            Settle(gate, i);
            i.b = true;
            const auto out = gate.Update(i, .01f);
            EXPECT_FALSE(out.request);
            i.b = false;
            EXPECT_EQ(gate.Update(i, .01f).request, location < 2);
            EXPECT_EQ(out.consume, location < 2);
        }
    }
}
TEST(ShoulderSwitch, ZoneExtendsBackwardsNotForwards) {
    // Centre z = -.06; old sphere reached back to z = .14. The capsule reaches to .63.
    for (bool left : {false, true}) {
        for (float z : {.60f, .66f}) {
            S::Gate gate;
            auto i = Ready();
            i.left = left;
            i.hand = {left ? -.20f : .20f, 1.40f, z};
            Settle(gate, i);
            i.b = true;
            EXPECT_FALSE(gate.Update(i, .01f).request);
            i.b = false;
            EXPECT_EQ(gate.Update(i, .01f).request, z < .63f);
        }
    }
}
TEST(ShoulderSwitch, LeftYRequiresFreshPressAndOwnsHoldUntilRelease) {
    for (float hz : {72.f, 80.f, 90.f, 120.f}) {
        S::Gate gate;
        auto i = Ready();
        i.left = true;
        i.hand = {-.20f, 1.40f, 0};
        i.b = true; // Y was already held when reaching the shoulder.
        Settle(gate, i, hz);
        i.b = false;
        Settle(gate, i, hz);
        i.b = true;
        ASSERT_TRUE(gate.Update(i, 1 / hz).consume);
        i.hand = {0, 1, -.5f};
        for (int frame = 0; frame < int(hz); ++frame) {
            const auto out = gate.Update(i, 1 / hz);
            EXPECT_FALSE(out.request);
            EXPECT_TRUE(out.consume); // Never leak the held Y into the quick-item arc.
        }
        i.b = false;
        EXPECT_FALSE(gate.Update(i, 1 / hz).consume);
    }
}
TEST(ShoulderSwitch, LeftArmKeepsAttackAndChordGuards) {
    for (bool chord : {false, true}) {
        S::Gate gate;
        auto i = Ready();
        i.left = true;
        i.hand = {-.20f, 1.40f, 0};
        Settle(gate, i);
        i.attacking = !chord;
        i.chord = chord;
        i.b = true;
        const auto out = gate.Update(i, .01f);
        EXPECT_FALSE(out.request);
        EXPECT_EQ(out.consume, !chord);
    }
}
TEST(ShoulderSwitch, HoldUnarmsOnceAtTheHoldTimeAndReleaseDoesNotDraw) {
    for (bool left : {false, true}) {
        for (float hz : {30.f, 60.f, 72.f, 90.f, 120.f}) {
            S::Gate g;
            auto i = Ready();
            i.left = left;
            i.hand.x = left ? -.2f : .2f;
            Settle(g, i, hz);
            i.b = true;
            ASSERT_FALSE(g.Update(i, 1 / hz).request);
            int count = 0;
            for (int frame = 1; frame <= int(4 * hz); ++frame) {
                const auto o = g.Update(i, 1 / hz);
                EXPECT_TRUE(o.consume);
                if (o.request) {
                    EXPECT_TRUE(o.unarm);
                    EXPECT_GE(frame / hz, S::UnarmHoldS - .00001f);
                    EXPECT_LE(frame / hz, S::UnarmHoldS + 1 / hz + .00001f);
                    ++count;
                }
            }
            EXPECT_EQ(count, 1);
            i.b = false;
            EXPECT_FALSE(g.Update(i, 1 / hz).request);
            Settle(g, i, hz);
            i.b = true;
            EXPECT_FALSE(g.Update(i, 1 / hz).request);
            i.b = false;
            auto o = g.Update(i, 1 / hz);
            EXPECT_TRUE(o.request);
            EXPECT_FALSE(o.unarm);
        }
    }
}
TEST(ShoulderSwitch, ReleaseFrameCrossingHoldThresholdUnarmsInsteadOfCycling) {
    for (bool left : {false, true}) {
        S::Gate gate;
        auto in = Ready();
        in.left = left;
        in.hand.x = left ? -.2f : .2f;
        Settle(gate, in);
        in.b = true;
        EXPECT_FALSE(gate.Update(in, .02f).request);
        for (int frame = 0; frame < 59; ++frame)
            EXPECT_FALSE(gate.Update(in, .02f).request);
        in.b = false; // released on the first sample after 1.2 seconds
        const auto out = gate.Update(in, .04f);
        EXPECT_TRUE(out.request);
        EXPECT_TRUE(out.unarm);
        EXPECT_FALSE(out.consume);
        EXPECT_FALSE(gate.Update(in, .02f).request);
    }
}
TEST(ShoulderSwitch, ShortTapStillCyclesWhenHandMovesAwayOnRelease) {
    for (bool left : {false, true}) {
        S::Gate gate;
        auto in = Ready();
        in.left = left;
        in.hand.x = left ? -.2f : .2f;
        Settle(gate, in);
        in.b = true;
        EXPECT_TRUE(gate.Update(in, .02f).consume);
        in.hand = {0, 1, -.5f};
        in.velocity = {0, -2, -2};
        gate.Update(in, .02f);
        in.b = false;
        const auto out = gate.Update(in, .02f);
        EXPECT_TRUE(out.request);
        EXPECT_FALSE(out.unarm);
    }
}
TEST(ShoulderSwitch, HoldSurvivesMotionAtTheShoulderButNotLeavingIt) {
    for (bool leave : {false, true}) {
        S::Gate g;
        auto i = Ready();
        Settle(g, i);
        i.b = true;
        g.Update(i, .02f);
        int unarms = 0;
        for (int n = 1; n <= 80; ++n) {
            // Moving and turning the hand at the shoulder (fast enough to have cancelled the
            // hold before), or leaving the zone with its margin (27 cm from the centre).
            i.velocity = {n % 2 ? 1.f : -1.f, 0, 0};
            i.angular_speed = 5.f;
            i.hand = leave && n == 20 ? VR::Body::Vec3{.20f + .28f, 1.40f, -.06f}
                                      : VR::Body::Vec3{.20f + (n % 2 ? .02f : -.02f), 1.40f, .08f};
            unarms += g.Update(i, .02f).unarm ? 1 : 0;
        }
        EXPECT_EQ(unarms, leave ? 0 : 1);
    }
}
TEST(ShoulderSwitch, InterruptedHoldNeverQueuesAnAction) {
    for (int guard = 0; guard < 7; ++guard) {
        S::Gate g;
        auto i = Ready();
        Settle(g, i);
        i.b = true;
        g.Update(i, .02f);
        for (int n = 0; n < 50; ++n)
            EXPECT_FALSE(g.Update(i, .02f).request);
        const auto valid = i;
        if (guard == 0)
            i.hand = {0, 0, 0};
        if (guard == 1)
            i.tracked = false;
        if (guard == 2)
            i.gameplay = false;
        if (guard == 3)
            i.available = false;
        if (guard == 4)
            i.attacking = true;
        if (guard == 5)
            i.chord = true;
        if (guard == 6)
            i.enabled = false;
        EXPECT_FALSE(g.Update(i, .02f).request);
        i = valid;
        for (int n = 0; n < 150; ++n) {
            const auto o = g.Update(i, .02f);
            EXPECT_FALSE(o.request);
            EXPECT_TRUE(o.consume);
        }
        i.b = false;
        EXPECT_FALSE(g.Update(i, .02f).request);
    }
}
TEST(ShoulderSwitch, TargetConfirmationRejectsUnrelatedSlotChanges) {
    S::Confirmation c;
    c.Begin(0, 100, 2);
    EXPECT_EQ(c.Update(true, true, 1, 200), S::Result::None);
    EXPECT_EQ(c.Update(true, false, 2, 300), S::Result::None);
    EXPECT_EQ(c.Update(true, true, 2, 400), S::Result::Changed);
    c.Begin(2, 500, 0);
    EXPECT_EQ(c.Update(true, true, 1, 600), S::Result::None);
    EXPECT_EQ(c.Update(true, true, 0, 700), S::Result::Changed);
}
VR::Vitals::Status Status(int hp = 100, int stamina = 100) {
    VR::Vitals::Status s{};
    s.ok = true;
    s.hp = hp;
    s.max_hp = 100;
    s.stamina = stamina;
    s.max_stamina = 100;
    s.steady_ns = 1'000'000'000;
    return s;
}
VR::VitalColor::Output Tick(VR::VitalColor::State &state, VR::Vitals::Status s,
                            VR::VitalColor::Config c, float seconds, float hz = 90) {
    VR::VitalColor::Output o{};
    for (int i = 0; i < int(seconds * hz); ++i) {
        s.steady_ns += std::int64_t(1e9 / hz);
        o = state.Update(s, c, true, s.steady_ns, 1 / hz);
    }
    return o;
}
TEST(VitalColor, DisabledIsNeutralAndOptionsAreIndependent) {
    VR::VitalColor::State state;
    auto o = Tick(state, Status(5, 0), {}, 1);
    EXPECT_FLOAT_EQ(o.health, 0);
    EXPECT_FLOAT_EQ(o.desaturation, 0);
    o = Tick(state, Status(5, 100), {true, false, 1, 1}, 1);
    EXPECT_GT(o.health, .2f);
    EXPECT_FLOAT_EQ(o.desaturation, 0);
    state = {};
    o = Tick(state, Status(5, 0), {false, true, 1, 1}, .2f);
    EXPECT_FLOAT_EQ(o.health, 0);
    EXPECT_GT(o.desaturation, .2f);
}
TEST(VitalColor, OutOfStaminaHoldsUntilRecoveredNeverGray) {
    for (float hz : {30.f, 60.f, 90.f, 120.f}) {
        VR::VitalColor::State state;
        const VR::VitalColor::Config c{false, true, 1, 1};
        // Low but never out: nothing.
        auto o = Tick(state, Status(100, 15), c, 2, hz);
        EXPECT_LT(o.desaturation, .01f);
        // Out: drained within a third of a second, never all grey.
        o = Tick(state, Status(100, 0), c, .3f, hz);
        EXPECT_GT(o.desaturation, .65f);
        EXPECT_LE(o.desaturation, VR::VitalColor::MaxDesaturation);
        // Still empty: held, not a blip.
        o = Tick(state, Status(100, 0), c, 4, hz);
        EXPECT_GT(o.desaturation, .7f);
        // Refilling (20%): lighter, still there.
        o = Tick(state, Status(100, 20), c, 1, hz);
        EXPECT_GT(o.desaturation, .4f);
        EXPECT_LT(o.desaturation, .6f);
        // Recovered past 25%: gone.
        o = Tick(state, Status(100, 30), c, 2, hz);
        EXPECT_LT(o.desaturation, .01f);
        // Low again without running out: nothing.
        o = Tick(state, Status(100, 10), c, 2, hz);
        EXPECT_LT(o.desaturation, .01f);
    }
}
TEST(VitalColor, StrengthScalesTheDrain) {
    VR::VitalColor::State a, b;
    const auto x = Tick(a, Status(100, 0), {false, true, 1, .6f}, 2);
    const auto y = Tick(b, Status(100, 0), {false, true, 1, 1}, 2);
    EXPECT_NEAR(x.desaturation, .6f * y.desaturation, .01f);
    EXPECT_GT(x.desaturation, .4f); // the user's 0.6: clearly visible
}
TEST(VitalColor, HealthWinsAndDeathMenusStalenessClearImmediately) {
    VR::VitalColor::State state;
    const VR::VitalColor::Config c{true, true, 1, 1};
    auto o = Tick(state, Status(5, 0), c, .3f);
    EXPECT_GT(o.health, .15f);
    EXPECT_LT(o.desaturation, .35f); // the red stays legible over the drain
    auto s = Status(0, 0);
    o = state.Update(s, c, true, s.steady_ns, .02f);
    EXPECT_FLOAT_EQ(o.health, 0);
    EXPECT_FLOAT_EQ(o.desaturation, 0);
    Tick(state, Status(5, 0), c, .3f);
    s = Status(5, 0);
    o = state.Update(s, c, false, s.steady_ns, .02f);
    EXPECT_FLOAT_EQ(o.health, 0);
    Tick(state, s, c, .3f);
    o = state.Update(s, c, true, s.steady_ns + 600'000'000, .02f);
    EXPECT_FLOAT_EQ(o.desaturation, 0);
}
TEST(VitalColor, FadeIsTimeBased) {
    VR::VitalColor::State a, b;
    const VR::VitalColor::Config c{true, true, .5f, .5f};
    auto x = Tick(a, Status(10, 0), c, .3f, 30), y = Tick(b, Status(10, 0), c, .3f, 120);
    EXPECT_NEAR(x.health, y.health, .001f);
    EXPECT_NEAR(x.desaturation, y.desaturation, .001f);
}
TEST(ShoulderSwitch, ConfirmationUsesSlotAndNeverConfirmsStaleOrBlockedSwitch) {
    S::Confirmation c;
    c.Begin(0, 100);
    EXPECT_EQ(c.Update(true, true, 0, 200), S::Result::None);
    EXPECT_EQ(c.Update(true, false, 1, 300), S::Result::None);
    EXPECT_EQ(c.Update(true, true, -1, 400), S::Result::None);
    // The item ID can be identical; the selected slot is the actual acknowledgement.
    EXPECT_EQ(c.Update(true, true, 1, 500), S::Result::Changed);
    EXPECT_FALSE(c.Pending());
    EXPECT_EQ(c.Update(true, true, 0, 600), S::Result::None);
    c.Begin(1, 100);
    EXPECT_EQ(c.Update(true, true, 1, 1'200'000'100), S::Result::TimedOut);
    EXPECT_FALSE(c.Pending());
    c.Begin(0, 100);
    EXPECT_EQ(c.Update(false, true, 1, 200), S::Result::None);
    EXPECT_FALSE(c.Pending());
}
TEST(ShoulderSwitch, TrackingLossAndBlockedActionsRequireANewSettledPress) {
    for (bool tracking_loss : {false, true}) {
        S::Gate g;
        auto i = Ready();
        Settle(g, i);
        if (tracking_loss)
            i.tracked = false;
        else
            i.available = false;
        i.b = true;
        EXPECT_FALSE(g.Update(i, .02f).request);
        i.tracked = i.available = true;
        Settle(g, i);
        i.b = false;
        Settle(g, i);
        i.b = true;
        EXPECT_FALSE(g.Update(i, .02f).request);
        i.b = false;
        EXPECT_TRUE(g.Update(i, .02f).request);
    }
}
TEST(VitalColor, DisableClearsImmediatelyAndInvalidGainCannotPoisonFutureFrames) {
    VR::VitalColor::State state;
    auto s = Status(5, 0);
    Tick(state, s, {true, true, 1, 1}, .3f);
    auto o = state.Update(s, {}, true, s.steady_ns, .02f);
    EXPECT_FLOAT_EQ(o.health, 0);
    EXPECT_FLOAT_EQ(o.desaturation, 0);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    o = Tick(state, s, {true, true, nan, nan}, .3f);
    EXPECT_FLOAT_EQ(o.health, 0);
    EXPECT_FLOAT_EQ(o.desaturation, 0);
    o = Tick(state, Status(5, 100), {true, true, 1, 1}, .3f);
    EXPECT_GT(o.health, .15f);
}
} // namespace
