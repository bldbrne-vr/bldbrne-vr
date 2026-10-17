// SPDX-License-Identifier: GPL-2.0-or-later

// VR body feedback and the weapon grip (core/vr/grip_adjust.h, core/vr/body_feedback.h,
// core/vr/bb_weapons.h, the trim parts of core/vr/body_frame.h). Covers: the trim rotation and
// its yaw / pitch / roll decomposition (both hands), the adjust math (the weapon frozen for one
// grip pose stays exactly where it was for the released pose, through the whole tracking ->
// model mapping, any camera yaw and world scale), the A+B gesture (the chord window, the 1 s hold,
// release, the settled pose, the trigger reset, deny, tracking loss, the time-out, cancel), the
// detach hysteresis and its tick rate, the ghost hand opacity and fade, the time budget windows,
// and the weapon family table.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "c0000_test_skeleton.h"

#include "core/vr/arm_ik.h"
#include "core/vr/bb_weapons.h"
#include "core/vr/body_feedback.h"
#include "core/vr/body_frame.h"
#include "core/vr/c0000_skeleton.h"
#include "core/vr/grip_adjust.h"

namespace {

namespace B = VR::Body;
namespace C = VR::Body::C0000;
namespace W = VR::Body::Weapons;
using B::Quat;
using B::Vec3;

constexpr float kDeg = B::Deg;
constexpr float kDt = 1.0f / 90.0f; // an XR frame

Quat RandomRotation(std::mt19937& rng) {
    std::normal_distribution<float> n(0.0f, 1.0f);
    return B::Normalize(Quat{n(rng), n(rng), n(rng), n(rng)});
}

B::TrackedHand HandAt(Vec3 p, Quat q) {
    B::TrackedHand h{};
    h.position_valid = h.position_tracked = h.orientation_valid = h.orientation_tracked = true;
    h.p = p;
    h.q = q;
    return h;
}

// W rows for a character at `t` facing world yaw `yaw` (as in test_body_frame.cpp).
B::ChrFrame FrameFor(float yaw, Vec3 t) {
    const float s = std::sin(yaw), c = std::cos(yaw);
    B::ChrFrame frame{};
    B::ChrFrameFromRows({-c, 0.0f, -s, t.x, 0.0f, 1.0f, 0.0f, t.y, s, 0.0f, -c, t.z}, frame);
    return frame;
}

// The weapon bone's model-space target for a grip pose with a trim, the way the pose hook builds
// it (bloodborne_body.cpp SolveBody).
B::ModelTarget WeaponTarget(const B::Anchor& a, const B::ChrFrame& f, const B::TrackedHand& h,
                            const B::GripTrim& trim, float blade_pitch) {
    const Quat grip =
        B::DefaultGrip(C::Right, blade_pitch, trim.yaw_deg, trim.pitch_deg, trim.roll_deg);
    return B::HandToModel(a, f, B::OffsetGrip(h, B::TrimOffset(trim)), grip);
}

float AngleDeg(const Quat& a, const Quat& b) {
    return B::AngleBetween(a, b) / kDeg;
}

// One gesture step.
B::AdjustOutput Step(B::GripAdjust& g, bool a, bool b, float dt = kDt, bool trigger = false,
                     bool ready = true, bool tracked = true, Vec3 p = {}, Quat q = {}) {
    B::AdjustInput in{};
    in.dt = dt;
    in.a = a;
    in.b = b;
    in.trigger = trigger;
    in.ready = ready;
    in.tracked = tracked;
    in.p = p;
    in.q = q;
    return g.Update(in);
}

// One step's event is `expected` (a bool, so a check evaluates the step once).
bool StepIs(B::GripAdjust& g, B::AdjustEvent expected, bool a, bool b, float dt = kDt,
            bool trigger = false, bool ready = true, bool tracked = true, Vec3 p = {},
            Quat q = {}) {
    return Step(g, a, b, dt, trigger, ready, tracked, p, q).event == expected;
}

// Holds A+B (pressed together) until the adjust enters; returns the frames it took.
int HoldUntilEnter(B::GripAdjust& g, Vec3 p = {}, Quat q = {}) {
    for (int frame = 1; frame <= 200; ++frame) {
        if (Step(g, true, true, kDt, false, true, true, p, q).event == B::AdjustEvent::Enter) {
            return frame;
        }
    }
    return -1;
}

} // namespace

// ---- Trim rotation ---------------------------------------------------------------------------

TEST(GripTrim, DefaultGripIsTrimTimesBase) {
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> angle(-60.0f, 60.0f);
    float worst = 0.0f;
    for (int i = 0; i < 2000; ++i) {
        const int side = i % 2 == 0 ? C::Right : C::Left;
        const float pitch = angle(rng), yaw = angle(rng), roll = angle(rng), blade = angle(rng);
        const Quat base = B::DefaultGrip(side, blade);
        const Quat trimmed = B::DefaultGrip(side, blade, yaw, pitch, roll);
        const Quat expected = B::GripTrimRotation(side, yaw, pitch, roll) * base;
        worst = std::max(worst, AngleDeg(trimmed, expected));
    }
    EXPECT_LT(worst, 0.01f);
}

TEST(GripTrim, AxesAndSigns) {
    // In the controller frame +Z is the blade, +Y the controller's up, +X its side.
    const Vec3 blade{0.0f, 0.0f, 1.0f};
    // Pitch + raises the blade.
    EXPECT_GT(B::Rotate(B::GripTrimRotation(C::Right, 0.0f, 20.0f, 0.0f), blade).y, 0.3f);
    // Yaw turns the blade about up, mirrored between the hands.
    const float right_x = B::Rotate(B::GripTrimRotation(C::Right, 20.0f, 0.0f, 0.0f), blade).x;
    const float left_x = B::Rotate(B::GripTrimRotation(C::Left, 20.0f, 0.0f, 0.0f), blade).x;
    EXPECT_GT(std::abs(right_x), 0.3f);
    EXPECT_NEAR(right_x, -left_x, 1e-6f);
    // Roll keeps the blade where it is.
    const Vec3 rolled = B::Rotate(B::GripTrimRotation(C::Right, 0.0f, 0.0f, 35.0f), blade);
    EXPECT_LT(B::Length(rolled - blade), 1e-6f);
}

TEST(GripTrim, AnglesRoundTrip) {
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> pitch(-85.0f, 85.0f);
    std::uniform_real_distribution<float> other(-179.0f, 179.0f);
    float worst = 0.0f;
    for (int i = 0; i < 20000; ++i) {
        const int side = i % 2 == 0 ? C::Right : C::Left;
        const float p = pitch(rng), y = other(rng), r = other(rng);
        float y2 = 0.0f, p2 = 0.0f, r2 = 0.0f;
        B::TrimAngles(side, B::GripTrimRotation(side, y, p, r), y2, p2, r2);
        worst = std::max({worst, std::abs(B::WrapPi((y2 - y) * kDeg) / kDeg), std::abs(p2 - p),
                          std::abs(B::WrapPi((r2 - r) * kDeg) / kDeg)});
        // Any rotation decomposes into angles that rebuild it.
        const Quat any = RandomRotation(rng);
        B::TrimAngles(side, any, y2, p2, r2);
        if (std::abs(p2) < 89.0f) {
            worst = std::max(worst, AngleDeg(B::GripTrimRotation(side, y2, p2, r2), any) * 0.1f);
        }
    }
    EXPECT_LT(worst, 0.01f);
}

TEST(GripTrim, OffsetGripMovesAlongTheGrip) {
    const Quat q = B::AxisAngle({0.0f, 1.0f, 0.0f}, 90.0f * kDeg);
    const B::TrackedHand h = HandAt({0.1f, 1.2f, -0.3f}, q);
    const B::TrackedHand moved = B::OffsetGrip(h, {0.0f, 0.0f, -0.05f});
    // Grip -Z turned 90 degrees left (about +Y) is tracking -X.
    EXPECT_LT(B::Length(moved.p - Vec3{0.05f, 1.2f, -0.3f}), 1e-6f);
    EXPECT_LT(AngleDeg(moved.q, h.q), 1e-4f);
}

// ---- Adjust math -----------------------------------------------------------------------------

// Freeze at grip pose 0, release at grip pose 1: the weapon the new grip puts on pose 1 is the
// weapon the old grip put on pose 0, exactly, through the full tracking -> model mapping.
TEST(GripAdjust, KeepsTheFrozenWeapon) {
    std::mt19937 rng(9);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    std::uniform_real_distribution<float> small(-15.0f, 15.0f);
    float worst_p = 0.0f, worst_q = 0.0f, worst_turn = 0.0f;
    for (int i = 0; i < 5000; ++i) {
        const B::ChrFrame frame = FrameFor(u(rng) * 3.1f, {u(rng) * 50.0f, u(rng), u(rng) * 50.0f});
        B::Anchor anchor{};
        anchor.valid = true;
        anchor.eye_rel = {0.1f * u(rng), 1.6f, -0.1f};
        anchor.head_k = {0.0f, 1.6f, 0.0f};
        anchor.base_yaw = u(rng) * 3.1f;
        anchor.world_scale = 1.0f + 0.5f * std::abs(u(rng));
        const float blade = small(rng);
        B::GripTrim old{};
        old.pitch_deg = small(rng);
        old.yaw_deg = small(rng);
        old.roll_deg = small(rng);
        old.offset_m = {0.02f * u(rng), 0.02f * u(rng), 0.02f * u(rng)};
        // Grip poses a hand's width apart, turned by up to about 40 degrees.
        const Quat q0 = RandomRotation(rng);
        const Vec3 p0{0.2f * u(rng), 1.1f + 0.2f * u(rng), -0.3f + 0.1f * u(rng)};
        const Quat q1 =
            B::Normalize(B::AxisAngle({u(rng), u(rng), u(rng)}, 0.7f * std::abs(u(rng))) * q0);
        const Vec3 p1 = p0 + Vec3{0.03f * u(rng), 0.03f * u(rng), 0.03f * u(rng)};
        const B::AdjustResult r = B::AdjustTrim(C::Right, old, p0, q0, p1, q1);
        const B::ModelTarget before = WeaponTarget(anchor, frame, HandAt(p0, q0), old, blade);
        const B::ModelTarget after = WeaponTarget(anchor, frame, HandAt(p1, q1), r.trim, blade);
        worst_p = std::max(worst_p, B::Length(after.p - before.p));
        worst_q = std::max(worst_q, AngleDeg(after.q, before.q));
        worst_turn = std::max(worst_turn, std::abs(r.turn_deg - AngleDeg(q0, q1)));
    }
    EXPECT_LT(worst_p, 1e-4f); // game units
    EXPECT_LT(worst_q, 0.02f);
    EXPECT_LT(worst_turn, 0.01f);
}

TEST(GripAdjust, NoMovementKeepsTheGrip) {
    std::mt19937 rng(13);
    for (int i = 0; i < 100; ++i) {
        const Quat q = RandomRotation(rng);
        const Vec3 p{0.1f, 1.2f, -0.2f};
        B::GripTrim old{};
        old.pitch_deg = 10.0f;
        old.yaw_deg = -5.0f;
        old.roll_deg = 20.0f;
        old.offset_m = {0.01f, -0.02f, 0.005f};
        const B::AdjustResult r = B::AdjustTrim(C::Right, old, p, q, p, q);
        EXPECT_NEAR(r.trim.pitch_deg, old.pitch_deg, 0.01f);
        EXPECT_NEAR(r.trim.yaw_deg, old.yaw_deg, 0.01f);
        EXPECT_NEAR(r.trim.roll_deg, old.roll_deg, 0.01f);
        for (size_t k = 0; k < 3; ++k) {
            EXPECT_NEAR(r.trim.offset_m[k], old.offset_m[k], 1e-5f);
        }
        EXPECT_LT(r.turn_deg, 0.05f);
        EXPECT_LT(r.move_m, 1e-6f);
    }
}

// Tilting the controller's tip down 10 degrees under a frozen weapon raises the blade 10
// degrees relative to the controller: pitch + 10.
TEST(GripAdjust, PitchFromATiltedController) {
    const Quat q0{};
    // Grip -Z forward; a rotation about grip +X by -10 degrees tips -Z down.
    const Quat q1 = B::AxisAngle({1.0f, 0.0f, 0.0f}, -10.0f * kDeg);
    const B::AdjustResult r = B::AdjustTrim(C::Right, {}, {}, q0, {}, q1);
    EXPECT_NEAR(r.trim.pitch_deg, 10.0f, 0.01f);
    EXPECT_NEAR(r.trim.yaw_deg, 0.0f, 0.01f);
    EXPECT_NEAR(r.trim.roll_deg, 0.0f, 0.01f);
    // Sliding the controller 2 cm back along its tube leaves the weapon 2 cm in front of the
    // grip (grip -Z).
    const B::AdjustResult s = B::AdjustTrim(C::Right, {}, {}, q0, {0.0f, 0.0f, 0.02f}, q0);
    EXPECT_NEAR(s.trim.offset_m[2], -0.02f, 1e-6f);
    EXPECT_NEAR(s.trim.offset_m[0], 0.0f, 1e-6f);
}

// ---- The A+B gesture -------------------------------------------------------------------------

TEST(GripAdjust, ChordEntersAfterTheHold) {
    B::GripAdjust g;
    const int frames = HoldUntilEnter(g, {0.1f, 1.0f, -0.2f});
    EXPECT_NEAR(B::GripAdjust::HoldS, 1.2f, 1e-6f);
    EXPECT_NEAR(static_cast<float>(frames) * kDt, B::GripAdjust::HoldS + kDt, 1.5f * kDt);
    EXPECT_TRUE(g.Frozen());
    EXPECT_LT(B::Length(g.p0 - Vec3{0.1f, 1.0f, -0.2f}), 1e-6f);
}

TEST(GripAdjust, ChordWindow) {
    // B 0.2 s after A: a chord.
    {
        B::GripAdjust g;
        for (int i = 0; i < 18; ++i) {
            Step(g, true, false);
        }
        EXPECT_EQ(g.Phase(), B::AdjustPhase::Idle);
        Step(g, true, true);
        EXPECT_EQ(g.Phase(), B::AdjustPhase::Chord);
    }
    // B 0.5 s after A (A held to sprint): never a chord, however long.
    {
        B::GripAdjust g;
        for (int i = 0; i < 45; ++i) {
            Step(g, true, false);
        }
        bool entered = false;
        for (int i = 0; i < 300; ++i) {
            entered |= Step(g, true, true).event != B::AdjustEvent::None;
        }
        EXPECT_FALSE(entered);
        EXPECT_EQ(g.Phase(), B::AdjustPhase::Idle);
    }
    // A after B within the window works the other way round too.
    {
        B::GripAdjust g;
        Step(g, false, true);
        Step(g, false, true);
        Step(g, true, true);
        EXPECT_EQ(g.Phase(), B::AdjustPhase::Chord);
    }
}

TEST(GripAdjust, ShortChordDoesNothing) {
    B::GripAdjust g;
    for (int i = 0; i < 60; ++i) {
        EXPECT_TRUE(StepIs(g, B::AdjustEvent::None, true, true));
    }
    EXPECT_TRUE(StepIs(g, B::AdjustEvent::None, false, true));
    EXPECT_EQ(g.Phase(), B::AdjustPhase::Idle);
}

TEST(GripAdjust, SaveUsesTheSettledPose) {
    B::GripAdjust g;
    const int entered = HoldUntilEnter(g);
    ASSERT_GT(entered, 0);
    // Turn the controller slowly, then a jolt on the release frame.
    Vec3 p{};
    for (int i = 0; i < 90; ++i) {
        p = {0.001f * static_cast<float>(i), 0.0f, 0.0f};
        EXPECT_TRUE(StepIs(g, B::AdjustEvent::None, true, true, kDt, false, true, true, p, {}));
    }
    const Vec3 jolt{0.5f, 0.5f, 0.5f};
    const B::AdjustOutput out = Step(g, true, false, kDt, false, true, true, jolt, {});
    EXPECT_EQ(out.event, B::AdjustEvent::Save);
    // The newest pose at least 60 ms old: 6 frames before the release frame at 90 Hz.
    EXPECT_GT(B::Length(out.p1 - jolt), 0.5f);
    EXPECT_NEAR(out.p1.x, 0.084f, 1e-6f);
    EXPECT_EQ(g.Phase(), B::AdjustPhase::Wait); // A still held
    EXPECT_TRUE(StepIs(g, B::AdjustEvent::None, true, false));
    Step(g, false, false);
    EXPECT_EQ(g.Phase(), B::AdjustPhase::Idle);
}

TEST(GripAdjust, TriggerResets) {
    B::GripAdjust g;
    // A trigger already held when the adjust starts does not reset it.
    for (int i = 0; i < 200 && !g.Frozen(); ++i) {
        Step(g, true, true, kDt, true);
    }
    ASSERT_TRUE(g.Frozen());
    EXPECT_TRUE(StepIs(g, B::AdjustEvent::None, true, true, kDt, true));
    EXPECT_TRUE(StepIs(g, B::AdjustEvent::None, true, true, kDt, false));
    EXPECT_TRUE(StepIs(g, B::AdjustEvent::Reset, true, true, kDt, true));
    EXPECT_FALSE(g.Frozen());
    // Releasing afterwards saves nothing.
    EXPECT_TRUE(StepIs(g, B::AdjustEvent::None, false, false));
}

TEST(GripAdjust, DenyWhenNotReady) {
    B::GripAdjust g;
    B::AdjustEvent last = B::AdjustEvent::None;
    for (int i = 0; i < 120 && last == B::AdjustEvent::None; ++i) {
        last = Step(g, true, true, kDt, false, false).event;
    }
    EXPECT_EQ(last, B::AdjustEvent::Deny);
    EXPECT_EQ(g.Phase(), B::AdjustPhase::Wait);
    for (int i = 0; i < 200; ++i) {
        EXPECT_TRUE(StepIs(g, B::AdjustEvent::None, true, true)); // held on: no retry
    }
    Step(g, false, false);
    const int again = HoldUntilEnter(g); // a new chord works
    EXPECT_GT(again, 0);
    // Untracked at the end of the hold: denied too.
    B::GripAdjust h;
    last = B::AdjustEvent::None;
    for (int i = 0; i < 120 && last == B::AdjustEvent::None; ++i) {
        last = Step(h, true, true, kDt, false, true, false).event;
    }
    EXPECT_EQ(last, B::AdjustEvent::Deny);
}

TEST(GripAdjust, CancelOnLossTimeoutAndRequest) {
    {
        B::GripAdjust g;
        const int entered = HoldUntilEnter(g);
        ASSERT_GT(entered, 0);
        EXPECT_TRUE(StepIs(g, B::AdjustEvent::Cancel, true, true, kDt, false, true, false));
        EXPECT_TRUE(StepIs(g, B::AdjustEvent::None, false, false));
    }
    {
        B::GripAdjust g;
        const int entered = HoldUntilEnter(g);
        ASSERT_GT(entered, 0);
        B::AdjustEvent last = B::AdjustEvent::None;
        int frames = 0;
        while (last == B::AdjustEvent::None && frames < 10000) {
            last = Step(g, true, true, 0.1f).event;
            ++frames;
        }
        EXPECT_EQ(last, B::AdjustEvent::Cancel);
        EXPECT_NEAR(static_cast<float>(frames) * 0.1f, B::GripAdjust::TimeoutS, 0.15f);
    }
    {
        B::GripAdjust g;
        const int entered = HoldUntilEnter(g);
        ASSERT_GT(entered, 0);
        EXPECT_TRUE(g.Cancel());
        EXPECT_FALSE(g.Frozen());
        EXPECT_EQ(g.Phase(), B::AdjustPhase::Wait); // until A and B are both released
        EXPECT_FALSE(g.Cancel());
        EXPECT_TRUE(StepIs(g, B::AdjustEvent::None, false, true)); // no save on release
    }
}

// ---- Detached hand ---------------------------------------------------------------------------

TEST(DetachWatch, HysteresisAndRate) {
    B::DetachWatch w;
    const std::int64_t ms = 1'000'000;
    std::int64_t t = 0;
    // Not owned or not solved: never.
    EXPECT_FALSE(w.Update(0.5f, true, false, 0.2f, t));
    EXPECT_FALSE(w.Update(1.0f, false, false, 0.2f, t));
    EXPECT_FALSE(w.detached);
    // 2.5 cm: not yet; 3.5 cm: detached with a tick.
    EXPECT_FALSE(w.Update(1.0f, true, false, 0.025f, t += 11 * ms));
    EXPECT_TRUE(w.Update(1.0f, true, false, 0.035f, t += 11 * ms));
    EXPECT_TRUE(w.detached);
    // Stays detached down to 2 cm (no new tick), clears below.
    EXPECT_FALSE(w.Update(1.0f, true, false, 0.025f, t += 11 * ms));
    EXPECT_TRUE(w.detached);
    EXPECT_FALSE(w.Update(1.0f, true, false, 0.015f, t += 11 * ms));
    EXPECT_FALSE(w.detached);
    // Back out within 0.5 s of the tick: detached, counted, no tick.
    EXPECT_FALSE(w.Update(1.0f, true, false, 0.05f, t += 11 * ms));
    EXPECT_TRUE(w.detached);
    EXPECT_EQ(w.count, 2u);
    // Reached clears it; 0.5 s after the first tick a new detach ticks again.
    EXPECT_FALSE(w.Update(1.0f, true, true, 0.05f, t += 11 * ms));
    EXPECT_FALSE(w.detached);
    EXPECT_TRUE(w.Update(1.0f, true, false, 0.05f, t += 500 * ms));
    // The game taking the arm clears it.
    EXPECT_FALSE(w.Update(0.89f, true, false, 0.05f, t += 11 * ms));
    EXPECT_FALSE(w.detached);
}

// ---- Ghost hands -----------------------------------------------------------------------------

TEST(Ghost, TargetAndFade) {
    const float a = 0.35f;
    EXPECT_EQ(B::GhostTarget(true, true, 1.0f, false, a), 0.0f);
    EXPECT_EQ(B::GhostTarget(true, true, 0.9f, false, a), 0.0f);
    EXPECT_NEAR(B::GhostTarget(true, true, 0.45f, false, a), a * 0.5f, 1e-6f);
    EXPECT_NEAR(B::GhostTarget(true, true, 0.0f, false, a), a, 1e-6f);
    EXPECT_NEAR(B::GhostTarget(true, true, 1.0f, true, a), a, 1e-6f); // detached
    EXPECT_EQ(B::GhostTarget(false, true, 0.0f, true, a), 0.0f);      // not driven (flat)
    EXPECT_EQ(B::GhostTarget(true, false, 0.0f, true, a), 0.0f);      // untracked
    EXPECT_EQ(B::GhostTarget(true, true, 0.0f, true, 0.0f), 0.0f);    // ghost_alpha 0
    // A full fade takes 80 ms either way, without overshoot.
    float alpha = 0.0f;
    int frames = 0;
    while (alpha < a && frames < 100) {
        alpha = B::GhostFade(alpha, a, a, 0.001f);
        ++frames;
    }
    EXPECT_NEAR(static_cast<float>(frames), 80.0f, 1.0f);
    EXPECT_EQ(alpha, a);
    EXPECT_EQ(B::GhostFade(a, 0.0f, a, 1.0f), 0.0f);
}

// ---- Time budget -----------------------------------------------------------------------------

TEST(Budget, Windows) {
    const std::int64_t ms = 1'000'000;
    const std::int64_t frame = 16 * ms;
    // 40 us a call against a 1000 us budget, with a 5 ms spike: never over.
    {
        B::BudgetGuard g;
        std::int64_t t = 0;
        int closed = 0;
        for (int i = 0; i < 2000; ++i) {
            const auto w = g.Add(i == 100 ? 5 * ms : 40'000, t += frame, 1000.0f);
            if (w.closed) {
                ++closed;
                EXPECT_FALSE(w.over);
                EXPECT_FALSE(w.exceeded);
                EXPECT_LT(w.avg_us, 60.0);
            }
            if (w.closed && closed == 1) {
                EXPECT_EQ(w.over_calls, 1u);
                EXPECT_NEAR(w.max_us, 5000.0, 0.1);
            }
        }
        EXPECT_GE(closed, 5);
    }
    // 1.5 ms a call: the first window is over, the second one exceeds.
    {
        B::BudgetGuard g;
        std::int64_t t = 0;
        std::vector<B::BudgetWindow> windows;
        for (int i = 0; i < 700; ++i) {
            const auto w = g.Add(1500'000, t += frame, 1000.0f);
            if (w.closed) {
                windows.push_back(w);
            }
        }
        ASSERT_TRUE(windows.size() >= 2);
        EXPECT_TRUE(windows[0].over);
        EXPECT_FALSE(windows[0].exceeded);
        EXPECT_TRUE(windows[1].exceeded);
        EXPECT_NEAR(windows[0].avg_us, 1500.0, 0.1);
    }
    // Over, then a good window: the count starts again.
    {
        B::BudgetGuard g;
        std::int64_t t = 0;
        std::vector<B::BudgetWindow> windows;
        for (int i = 0; i < 1300; ++i) {
            const bool slow = i < 330 || (i >= 650 && i < 980);
            const auto w = g.Add(slow ? 1500'000 : 40'000, t += frame, 1000.0f);
            if (w.closed) {
                windows.push_back(w);
            }
        }
        ASSERT_TRUE(windows.size() >= 3);
        for (const auto& w : windows) {
            EXPECT_FALSE(w.exceeded);
        }
    }
    // Too few calls (loading) or no budget: never judged.
    {
        B::BudgetGuard g;
        auto w = g.Add(9 * ms, 0, 1000.0f);
        w = g.Add(9 * ms, 6000 * ms, 1000.0f);
        EXPECT_TRUE(w.closed);
        EXPECT_FALSE(w.over);
        B::BudgetGuard h;
        std::int64_t t = 0;
        bool over = false;
        for (int i = 0; i < 700; ++i) {
            over |= h.Add(9 * ms, t += frame, 0.0f).over;
        }
        EXPECT_FALSE(over);
    }
}

// ---- Weapon families -------------------------------------------------------------------------

TEST(Weapons, Table) {
    std::set<std::string_view> keys;
    std::set<std::int32_t> families;
    for (const W::Family& f : W::Families) {
        EXPECT_TRUE(keys.insert(f.key).second);
        EXPECT_TRUE(families.insert(f.family).second);
        for (const char c : f.key) {
            EXPECT_TRUE((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_');
        }
        EXPECT_EQ(W::FindKey(f.key), &f);
        EXPECT_EQ(W::FindFamily(f.family), &f);
        // Never the global trim keys' suffixes.
        EXPECT_TRUE(f.key != "pitch_deg" && f.key != "yaw_deg" && f.key != "roll_deg");
    }
    EXPECT_EQ(W::FamilyOf(7000000), 70);
    EXPECT_EQ(W::FamilyOf(7021000), 70); // Lost Saw Cleaver +10
    EXPECT_EQ(W::FamilyOf(-1), -1);
    const W::Family* cleaver = W::FindFamily(70);
    ASSERT_TRUE(cleaver != nullptr);
    EXPECT_EQ(cleaver->key, std::string_view{"saw_cleaver"});
    EXPECT_EQ(cleaver->hold, W::Hold::Blade);
    EXPECT_EQ(cleaver->dmy_a, 300);
    EXPECT_EQ(W::FindFamily(140)->hold, W::Hold::Gun); // Hunter Pistol
    EXPECT_EQ(W::FindFamily(140)->dmy_left, 347);
    EXPECT_EQ(W::FindFamily(50)->hold, W::Hold::Haft); // Hunter Axe
    EXPECT_EQ(W::FindFamily(90)->hold, W::Hold::Hand); // Beast Claw
    EXPECT_TRUE(W::FindFamily(12345) == nullptr);
}
