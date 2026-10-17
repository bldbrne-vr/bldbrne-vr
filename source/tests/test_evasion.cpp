// SPDX-License-Identifier: GPL-2.0-or-later

// Physical evasion (core/vr/evasion.h): body profile, caps, gain, wall limit and its release,
// the lean budget (hold, recharge, drift settle), fades, glitches, the pose-shift subtraction and
// the neck model, at 60 Hz.

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "core/vr/evasion.h"

namespace {

namespace E = VR::Evade;

constexpr double Dt = 1.0 / 60.0;

E::Config Unity() {
    E::Config cfg;
    cfg.gain = 1.0f;
    return cfg;
}

// Holds `neck` from `t0` for `seconds` and returns the last output.
E::Output Hold(E::Model& model, const E::Config& cfg, E::Vec3 neck, double& t, double seconds,
               E::Input base = {}) {
    E::Output out{};
    const double end = t + seconds;
    for (; t < end; t += Dt) {
        E::Input in = base;
        in.t = t;
        in.neck = neck;
        out = model.Step(in, cfg);
    }
    return out;
}

// A lean ramped in over 0.15 s (a real body; a one-frame jump is a tracking glitch) and held for
// 0.4 s (fade complete, no settle).
E::Output Lean(const E::Config& cfg, E::Vec3 neck, E::Input base = {}) {
    E::Model model;
    double t = 0.0;
    Hold(model, cfg, {}, t, 0.3, base);
    constexpr int Ramp = 9;
    for (int i = 1; i <= Ramp; ++i) {
        const float f = static_cast<float>(i) / Ramp;
        Hold(model, cfg, {neck.x * f, neck.y * f, neck.z * f}, t, Dt * 0.5, base);
    }
    return Hold(model, cfg, neck, t, 0.4, base);
}

TEST(Evasion, AnklePivotProfile) {
    const E::Output out = Lean(Unity(), {0.2f, 0.0f, 0.0f});
    EXPECT_NEAR(out.offset[E::Head01].x, 0.20f, 1e-4f);
    EXPECT_NEAR(out.offset[E::Pelvis01].x, 0.128f, 2e-3f);
    EXPECT_NEAR(out.offset[E::L_Foot01].x, 0.010f, 2e-3f);
    EXPECT_NEAR(out.offset[E::Head01].z, 0.0f, 1e-6f);
    EXPECT_NEAR(out.offset[E::Head01].y, 0.0f, 1e-6f);
    // Spine between pelvis and head.
    EXPECT_GT(out.offset[E::Spine02].x, out.offset[E::Spine01].x);
    EXPECT_GT(out.offset[E::Spine01].x, out.offset[E::Pelvis01].x);
}

TEST(Evasion, StepBeyondLeanRangeMovesTheFeetAndIsCapped) {
    const E::Output out = Lean(Unity(), {0.0f, 0.0f, 0.5f});
    EXPECT_NEAR(out.lean_m, 0.40f, 1e-5f);
    EXPECT_NEAR(out.offset[E::Head01].z, 0.40f, 1e-4f);
    EXPECT_NEAR(out.offset[E::R_Foot01].z, 0.10f + 0.30f * 0.08f / 1.58f, 1e-3f);
}

TEST(Evasion, GainMovesTheHurtboxesFurther) {
    E::Config cfg; // default gain 1.3
    const E::Output out = Lean(cfg, {-0.2f, 0.0f, 0.0f});
    EXPECT_NEAR(out.offset[E::Head01].x, -0.26f, 1e-4f);
    EXPECT_NEAR(out.offset[E::Pelvis01].x, -0.166f, 2e-3f);
}

TEST(Evasion, DuckCountsOnlyDownwardsAndIsCapped) {
    // The cap: 0.75 m (0.45 until 2026-10-08: the user's ducks hit it; then 0.60; the user the
    // same evening: "15cm more"), the camera's down room (positional_limit_down_m 0.80) a little
    // deeper.
    const E::Output duck = Lean(Unity(), {0.0f, -0.9f, 0.0f});
    EXPECT_NEAR(duck.duck_m, -0.75f, 1e-5f);
    EXPECT_NEAR(duck.offset[E::Head01].y, -0.75f, 1e-4f);
    // Within the cap a duck counts one to one.
    EXPECT_NEAR(Lean(Unity(), {0.0f, -0.7f, 0.0f}).duck_m, -0.70f, 1e-5f);
    // With the default gain 1.3 the head capsule goes down 0.975 m at the cap: its centre (1.58 m
    // on the reference hunter) stays 0.6 m over his feet, every capsule over 0.5 m; the calves and
    // feet never move.
    const E::Output gained = Lean(E::Config{}, {0.0f, -0.9f, 0.0f});
    EXPECT_NEAR(gained.offset[E::Head01].y, -0.975f, 1e-4f);
    EXPECT_GT(E::RestHeight[E::Head01] + gained.offset[E::Head01].y, 0.6f);
    for (const size_t b : {E::L_Foot01, E::R_Foot01, E::L_Calf01, E::R_Calf01}) {
        EXPECT_NEAR(gained.offset[b].y, 0.0f, 1e-6f) << E::BodyNames[b];
    }
    for (size_t b = 0; b < E::BodyCount; ++b) {
        EXPECT_GT(E::RestHeight[b] + gained.offset[b].y, E::RestHeight[b] > 0.5f ? 0.5f : 0.0f)
            << E::BodyNames[b];
    }
    EXPECT_NEAR(duck.offset[E::L_Calf01].y, 0.0f, 1e-6f);
    EXPECT_NEAR(duck.offset[E::L_Foot01].y, 0.0f, 1e-6f);
    // The pelvis about half (0.47 of the drop).
    EXPECT_LT(duck.offset[E::Pelvis01].y, -0.30f);
    EXPECT_GT(duck.offset[E::Pelvis01].y, -0.40f);

    const E::Output rise = Lean(Unity(), {0.0f, 0.3f, 0.0f});
    for (const auto& o : rise.offset) {
        EXPECT_NEAR(o.y, 0.0f, 1e-6f);
    }
}

TEST(Evasion, WallLimitsTheGainedOffset) {
    E::Input base;
    base.wall_m = 0.25f; // margin 0.03 -> 0.22 of head offset
    const E::Output unity = Lean(Unity(), {0.3f, 0.0f, 0.0f}, base);
    EXPECT_TRUE(unity.wall_limited);
    EXPECT_NEAR(unity.offset[E::Head01].x, 0.22f, 1e-4f);
    EXPECT_NEAR(unity.wall_limit_m, 0.22f, 1e-4f);
    EXPECT_NEAR(unity.lean_want_m, 0.30f, 1e-4f);

    // The limit is on the gained offset: the head stops at the same place with gain 1.3.
    const E::Output gained = Lean(E::Config{}, {0.3f, 0.0f, 0.0f}, base);
    EXPECT_NEAR(gained.offset[E::Head01].x, 0.22f, 1e-4f);
    // Every other body moves less than the head.
    for (const auto& o : gained.offset) {
        EXPECT_LE(o.x, 0.22f + 1e-4f);
    }

    base.wall_m = 0.02f; // inside the margin: no lean at all
    const E::Output blocked = Lean(Unity(), {0.3f, 0.0f, 0.0f}, base);
    EXPECT_NEAR(blocked.offset[E::Head01].x, 0.0f, 1e-6f);

    // No wall within reach: the full lean.
    base.wall_m = -1.0f;
    const E::Output free = Lean(Unity(), {0.3f, 0.0f, 0.0f}, base);
    EXPECT_FALSE(free.wall_limited);
    EXPECT_NEAR(free.offset[E::Head01].x, 0.30f, 1e-4f);
    EXPECT_NEAR(free.wall_limit_m, -1.0f, 1e-6f);
}

TEST(Evasion, WallDropsAtOnceAndIsReleasedAtItsRate) {
    E::Model model;
    const E::Config cfg = Unity(); // rise 1.5 m/s
    double t = 0.0;
    Hold(model, cfg, {}, t, 0.3);
    const E::Output free = Hold(model, cfg, {0.3f, 0.0f, 0.0f}, t, 0.3);
    EXPECT_NEAR(free.offset[E::Head01].x, 0.30f, 1e-4f);
    // A wall appears (stepping next to it): the limit applies on the very next frame.
    E::Input wall;
    wall.wall_m = 0.13f;
    const E::Output hit = Hold(model, cfg, {0.3f, 0.0f, 0.0f}, t, Dt, wall);
    EXPECT_TRUE(hit.wall_limited);
    EXPECT_NEAR(hit.offset[E::Head01].x, 0.10f, 1e-4f);
    // It goes away along the same direction (rounding a door frame): released at <= 1.5 m/s.
    float last = hit.offset[E::Head01].x;
    for (int i = 0; i < 30; ++i) {
        const E::Output o = Hold(model, cfg, {0.3f, 0.0f, 0.0f}, t, Dt);
        EXPECT_LE(o.offset[E::Head01].x - last, 1.5f * static_cast<float>(Dt) + 1e-4f);
        last = o.offset[E::Head01].x;
    }
    EXPECT_NEAR(last, 0.30f, 1e-4f); // 0.2 m at 1.5 m/s: back within 0.14 s
}

TEST(Evasion, WallLimitDoesNotHoldBackALeanTheOtherWay) {
    E::Model model;
    const E::Config cfg = Unity();
    double t = 0.0;
    Hold(model, cfg, {}, t, 0.3);
    E::Input wall;
    wall.wall_m = 0.03f; // flush against a wall on the right
    const E::Output pinned = Hold(model, cfg, {0.2f, 0.0f, 0.0f}, t, 0.3, wall);
    EXPECT_NEAR(pinned.offset[E::Head01].x, 0.0f, 1e-4f);
    // A quick lean to the left (no wall that way): counts at once.
    const E::Output away = Hold(model, cfg, {-0.2f, 0.0f, 0.0f}, t, Dt);
    EXPECT_FALSE(away.wall_limited);
    EXPECT_NEAR(away.offset[E::Head01].x, -0.20f, 1e-4f);
}

TEST(Evasion, ProbeDoesNotChangeTheModel) {
    // The render tick steps a copy without the wall to aim its casts; the real step afterwards
    // must see the same history.
    E::Model a, b;
    const E::Config cfg = E::Config{};
    double ta = 0.0, tb = 0.0;
    Hold(a, cfg, {}, ta, 0.3);
    Hold(b, cfg, {}, tb, 0.3);
    E::Input in;
    in.neck = {0.25f, 0.0f, 0.1f};
    for (int i = 0; i < 120; ++i) {
        in.t = ta;
        E::Model probe = a;
        const E::Output want = probe.Step(in, cfg);
        E::Input with_wall = in;
        with_wall.wall_m = 0.2f;
        const E::Output real = a.Step(with_wall, cfg);
        const E::Output ref = b.Step(with_wall, cfg);
        EXPECT_NEAR(real.offset[E::Head01].x, ref.offset[E::Head01].x, 1e-6f);
        EXPECT_NEAR(want.counted.x, real.counted.x, 1e-6f);
        EXPECT_GE(want.lean_want_m + 1e-6f, real.lean_m);
        ta += Dt;
    }
}

TEST(Evasion, QuickLeansCountFullyAndHeldLeansSettle) {
    E::Model model;
    const E::Config cfg = Unity();
    double t = 0.0;
    Hold(model, cfg, {}, t, 0.3);
    const E::Output quick = Hold(model, cfg, {0.3f, 0.0f, 0.0f}, t, 0.5);
    EXPECT_NEAR(quick.offset[E::Head01].x, 0.30f, 1e-4f);
    // The budget (1.5 s) runs out: the lean fades with tau 0.5 s ...
    const E::Output fading = Hold(model, cfg, {0.3f, 0.0f, 0.0f}, t, 1.3);
    EXPECT_TRUE(fading.hold_settling);
    EXPECT_LT(fading.offset[E::Head01].x, 0.25f);
    EXPECT_GT(fading.offset[E::Head01].x, 0.10f);
    // ... to nearly nothing 3 s into the lean (and stops following once it has caught up).
    const E::Output held = Hold(model, cfg, {0.3f, 0.0f, 0.0f}, t, 1.2);
    EXPECT_LT(held.offset[E::Head01].x, 0.05f);
    EXPECT_GT(held.settled_m, 0.25f);

    // Back to the centre: no lean the other way.
    const E::Output back = Hold(model, cfg, {}, t, 0.3);
    EXPECT_NEAR(back.offset[E::Head01].x, 0.0f, 1e-4f);
    // A fresh lean the other way counts fully at once.
    const E::Output other = Hold(model, cfg, {-0.2f, 0.0f, 0.0f}, t, 0.1);
    EXPECT_NEAR(other.offset[E::Head01].x, -0.20f, 1e-4f);
}

// The exploit the old stillness-based settle allowed: hold a lean and keep the head jiggling
// 1-2 cm, so it never counts as still. The hold budget runs out whatever the head does.
TEST(Evasion, HoldBudgetDefeatsJiggle) {
    for (const auto [amp, hz] : {std::pair{0.02f, 3.0f}, std::pair{0.01f, 6.0f}}) {
        E::Model model;
        const E::Config cfg = Unity();
        double t = 0.0;
        Hold(model, cfg, {}, t, 0.5);
        const double start = t;
        double sum = 0.0;
        int n = 0;
        E::Output at_1_4{};
        for (; t < start + 8.0; t += Dt) {
            const float x =
                0.3f + amp * std::sin(static_cast<float>(2.0 * 3.14159265 * hz * (t - start)));
            E::Input in;
            in.t = t;
            in.neck = {x, 0.0f, 0.0f};
            const E::Output out = model.Step(in, cfg);
            if (t - start < 1.4) {
                at_1_4 = out;
            }
            if (t - start >= 3.0) {
                sum += std::abs(out.counted.x);
                ++n;
            }
        }
        EXPECT_GE(at_1_4.offset[E::Head01].x, 0.27f)
            << "amp " << amp; // full while the budget lasts
        EXPECT_LT(sum / n, 0.05) << "amp " << amp;
    }
}

TEST(Evasion, HoldBudgetRechargesAtTheCentre) {
    E::Model model;
    const E::Config cfg = Unity();
    double t = 0.0;
    Hold(model, cfg, {}, t, 0.3);
    const E::Output first = Hold(model, cfg, {0.25f, 0.0f, 0.0f}, t, 1.2);
    EXPECT_NEAR(first.offset[E::Head01].x, 0.25f, 1e-4f);
    EXPECT_GT(first.held_s, 1.1f);
    Hold(model, cfg, {}, t, 0.3);
    // The next lean gets the full budget again: still full after another 1.2 s.
    const E::Output second = Hold(model, cfg, {0.25f, 0.0f, 0.0f}, t, 1.2);
    EXPECT_NEAR(second.offset[E::Head01].x, 0.25f, 1e-4f);
    EXPECT_FALSE(second.hold_settling);

    // A flick through the centre shorter than release_s does not recharge it.
    E::Model flick;
    double tf = 0.0;
    Hold(flick, cfg, {}, tf, 0.3);
    Hold(flick, cfg, {0.25f, 0.0f, 0.0f}, tf, 1.2);
    Hold(flick, cfg, {}, tf, 0.1);
    const E::Output again = Hold(flick, cfg, {0.25f, 0.0f, 0.0f}, tf, 0.6);
    EXPECT_TRUE(again.hold_settling);
    EXPECT_LT(again.offset[E::Head01].x, 0.20f);
}

TEST(Evasion, SwayingLeftAndRightKeepsCounting) {
    // A continuous sway (period 2 s, 0.3 m) never rests at the centre, but every reversal is a
    // new dodge: the peaks stay full for the whole 12 s.
    E::Model model;
    const E::Config cfg = Unity();
    double t = 0.0;
    Hold(model, cfg, {}, t, 0.3);
    const double start = t;
    float min_peak = 1.0f;
    float peak = 0.0f;
    float last_x = 0.0f;
    for (; t < start + 12.0; t += Dt) {
        const float x = 0.3f * std::sin(static_cast<float>(3.14159265 * (t - start)));
        E::Input in;
        in.t = t;
        in.neck = {x, 0.0f, 0.0f};
        const E::Output out = model.Step(in, cfg);
        peak = std::max(peak, std::abs(out.offset[E::Head01].x));
        if ((x > 0.0f) != (last_x > 0.0f) && t - start > 0.6) {
            min_peak = std::min(min_peak, peak); // a half cycle ended
            peak = 0.0f;
        }
        last_x = x;
    }
    EXPECT_GT(min_peak, 0.29f);
}

TEST(Evasion, DriftStillSettlesSilently) {
    // Slowly drifting 15 cm off the recentre spot over 20 s ends close to zero counted lean.
    E::Model model;
    const E::Config cfg = Unity();
    double t = 0.0;
    E::Output out{};
    for (; t < 24.0; t += Dt) {
        E::Input in;
        in.t = t;
        in.neck = {0.15f * static_cast<float>(std::min(t / 20.0, 1.0)), 0.0f, 0.0f};
        out = model.Step(in, cfg);
    }
    EXPECT_LT(std::abs(out.counted.x), 0.02f);
    EXPECT_FALSE(out.hold_settling);
}

TEST(Evasion, DuckUsesTheHoldBudgetToo) {
    E::Model model;
    const E::Config cfg = Unity();
    double t = 0.0;
    Hold(model, cfg, {}, t, 0.3);
    const E::Output ducked = Hold(model, cfg, {0.0f, -0.3f, 0.0f}, t, 1.0);
    EXPECT_NEAR(ducked.offset[E::Head01].y, -0.30f, 1e-4f);
    const E::Output held = Hold(model, cfg, {0.0f, -0.3f, 0.0f}, t, 2.5);
    EXPECT_GT(held.offset[E::Head01].y, -0.05f);
}

TEST(Evasion, SettleCanBeDisabled) {
    E::Config cfg = Unity();
    cfg.settle_s = 0.0f; // also turns the hold budget off
    E::Model model;
    double t = 0.0;
    const E::Output out = Hold(model, cfg, {0.25f, 0.0f, 0.0f}, t, 5.0);
    EXPECT_NEAR(out.offset[E::Head01].x, 0.25f, 1e-4f);
    EXPECT_FALSE(out.hold_settling);

    // hold_s = 0: no hold limit, only the drift settle (a lean held still is beyond engage_m, so
    // it keeps counting).
    E::Config no_hold = Unity();
    no_hold.hold_s = 0.0f;
    E::Model m2;
    double t2 = 0.0;
    const E::Output kept = Hold(m2, no_hold, {0.25f, 0.0f, 0.0f}, t2, 5.0);
    EXPECT_NEAR(kept.offset[E::Head01].x, 0.25f, 1e-4f);
}

TEST(Evasion, GatesFadeOutFastAndInSlower) {
    E::Model model;
    const E::Config cfg = Unity();
    double t = 0.0;
    Hold(model, cfg, {0.2f, 0.0f, 0.0f}, t, 0.4);
    E::Input closed;
    closed.active = false;
    // 40 ms into a fade-out of 80 ms: about half.
    const E::Output half = Hold(model, cfg, {0.2f, 0.0f, 0.0f}, t, 0.040, closed);
    EXPECT_GT(half.weight, 0.3f);
    EXPECT_LT(half.weight, 0.7f);
    const E::Output off = Hold(model, cfg, {0.2f, 0.0f, 0.0f}, t, 0.1, closed);
    EXPECT_NEAR(off.weight, 0.0f, 1e-6f);
    EXPECT_NEAR(off.offset[E::Head01].x, 0.0f, 1e-6f);
    // Fade back in over 150 ms.
    const E::Output in1 = Hold(model, cfg, {0.2f, 0.0f, 0.0f}, t, 0.075);
    EXPECT_GT(in1.weight, 0.35f);
    EXPECT_LT(in1.weight, 0.65f);
    const E::Output in2 = Hold(model, cfg, {0.2f, 0.0f, 0.0f}, t, 0.2);
    EXPECT_NEAR(in2.weight, 1.0f, 1e-6f);
}

TEST(Evasion, DisabledOrRecentreClearsEverything) {
    E::Model model;
    const E::Config cfg = Unity();
    double t = 0.0;
    Hold(model, cfg, {0.2f, 0.0f, 0.0f}, t, 0.4);
    E::Input off;
    off.enabled = false;
    const E::Output out = Hold(model, cfg, {0.2f, 0.0f, 0.0f}, t, Dt, off);
    for (const auto& o : out.offset) {
        EXPECT_NEAR(o.x, 0.0f, 1e-6f);
    }
    EXPECT_NEAR(out.weight, 0.0f, 1e-6f);
}

TEST(Evasion, TrackingGlitchIsAbsorbed) {
    E::Model model;
    const E::Config cfg = Unity();
    double t = 0.0;
    Hold(model, cfg, {0.1f, 0.0f, 0.0f}, t, 0.4);
    const E::Output jumped = Hold(model, cfg, {0.8f, 0.0f, 0.0f}, t, Dt);
    EXPECT_TRUE(jumped.glitch);
    EXPECT_NEAR(jumped.offset[E::Head01].x, 0.10f, 1e-4f);
}

TEST(Evasion, TrackingLossHoldsThenFades) {
    E::Model model;
    const E::Config cfg = Unity();
    double t = 0.0;
    Hold(model, cfg, {0.2f, 0.0f, 0.0f}, t, 0.4);
    E::Input lost;
    lost.tracked = false;
    const E::Output early = Hold(model, cfg, {5.0f, 5.0f, 5.0f}, t, 0.05, lost);
    EXPECT_GT(early.weight, 0.6f);
    EXPECT_GT(early.offset[E::Head01].x, 0.10f);
    EXPECT_LT(early.offset[E::Head01].x, 0.20f + 1e-4f);
    const E::Output late = Hold(model, cfg, {5.0f, 5.0f, 5.0f}, t, 0.3, lost);
    EXPECT_NEAR(late.weight, 0.0f, 1e-6f);
}

TEST(Evasion, PoseShiftIsSubtractedFromUpperBodies) {
    E::Input base;
    base.pose_shift = {0.20f, 0.0f, 0.0f};
    E::Config full = Unity();
    full.shift_reach = 1.0f;
    const E::Output out = Lean(full, {0.35f, 0.0f, 0.0f}, base);
    // Head: 0.30 pivot + 0.05 step = 0.35, minus the 0.20 already in the pose.
    EXPECT_NEAR(out.offset[E::Head01].x, 0.15f, 1e-4f);
    // Pelvis is not under UpperPos: full profile.
    EXPECT_NEAR(out.offset[E::Pelvis01].x, 0.05f + 0.30f * 1.01f / 1.58f, 1e-3f);

    // The game's ragdoll mapping carries only about half of the shift to the hurtboxes (default
    // shift_reach 0.5): only that half is subtracted.
    const E::Output half = Lean(Unity(), {0.35f, 0.0f, 0.0f}, base);
    EXPECT_NEAR(half.offset[E::Head01].x, 0.35f - 0.10f, 1e-4f);
    EXPECT_NEAR(half.offset[E::Pelvis01].x, out.offset[E::Pelvis01].x, 1e-6f);
}

// A duck's downward UpperPos shift: the ragdoll follows it only about 0.36 m (measured, duck3), so
// the hurtboxes compensate at most ShiftDownFollowM (0.72 m) of it; shallower shifts (the 0.60 m
// the body's duck reached before) and every horizontal shift are compensated as before.
TEST(Evasion, ADeepDuckShiftIsCompensatedOnlyAsFarAsTheRagdollFollows) {
    const auto head_y = [](float shift_y) {
        E::Input base;
        base.pose_shift = {0.0f, shift_y, 0.0f};
        return Lean(Unity(), {0.0f, -0.5f, 0.0f}, base).offset[E::Head01].y;
    };
    // The old range as before: the counted 0.5 m duck less half the shift.
    EXPECT_NEAR(head_y(-0.60f), -0.5f + 0.30f, 1e-4f);
    EXPECT_NEAR(head_y(-0.72f), -0.5f + 0.36f, 1e-4f);
    // Deeper: the 0.36 m the ragdoll follows (0.40 lifted the head hurtbox 4 cm over the vanilla
    // body in the mock's held 0.80 m duck).
    EXPECT_NEAR(head_y(-0.80f), -0.5f + 0.36f, 1e-4f);
    EXPECT_NEAR(head_y(-1.20f), -0.5f + 0.36f, 1e-4f);
    // The pelvis is not under UpperPos: nothing subtracted.
    E::Input deep;
    deep.pose_shift = {0.0f, -0.80f, 0.0f};
    EXPECT_NEAR(Lean(Unity(), {0.0f, -0.5f, 0.0f}, deep).offset[E::Pelvis01].y,
                Lean(Unity(), {0.0f, -0.5f, 0.0f}).offset[E::Pelvis01].y, 1e-6f);
    // The helper, in metres and in game units (world_scale 2: the floor doubles); up and the old
    // range unchanged bit for bit.
    EXPECT_EQ(E::FollowedShiftY(-0.60f), -0.60f);
    EXPECT_EQ(E::FollowedShiftY(0.10f), 0.10f);
    EXPECT_EQ(E::FollowedShiftY(-0.80f), -E::ShiftDownFollowM);
    EXPECT_EQ(E::FollowedShiftY(-1.20f, 2.0f), -1.20f);
    EXPECT_EQ(E::FollowedShiftY(-1.60f, 2.0f), -2.0f * E::ShiftDownFollowM);
}

// body_posture=lean: the visual trunk lean moves the eye by `pose_lean`; each body under UpperPos
// already carries lean_reach times its LeanReach share of it (the head 0.85, the chest about a
// third, the pelvis none), and only that is subtracted.
TEST(Evasion, PoseLeanIsSubtractedByHeight) {
    E::Input base;
    base.pose_lean = {0.20f, 0.0f, 0.0f};
    const E::Config cfg = Unity();
    const E::Output out = Lean(cfg, {0.35f, 0.0f, 0.0f}, base);
    const E::Output none = Lean(cfg, {0.35f, 0.0f, 0.0f}, {});
    EXPECT_NEAR(out.offset[E::Head01].x, none.offset[E::Head01].x - 0.85f * 0.20f, 1e-4f);
    const float h = (1.28f - 1.0f) / (1.58f - 1.0f);
    EXPECT_NEAR(out.offset[E::Spine02].x,
                none.offset[E::Spine02].x - 0.85f * std::pow(h, 1.4f) * 0.20f, 1e-4f);
    EXPECT_NEAR(out.offset[E::Pelvis01].x, none.offset[E::Pelvis01].x, 1e-6f);
    EXPECT_NEAR(out.offset[E::L_Thigh01].x, none.offset[E::L_Thigh01].x, 1e-6f);
    EXPECT_NEAR(E::LeanReach(E::Head01), 1.0f, 1e-6f);
    EXPECT_LT(E::LeanReach(E::Pelvis01), 0.01f); // not under UpperPos anyway
}

TEST(Evasion, IkOwnedArmsDoNotMove) {
    E::Input base;
    base.ik_right = 1.0f;
    base.ik_left = 0.5f;
    const E::Output out = Lean(Unity(), {0.2f, 0.0f, 0.0f}, base);
    EXPECT_NEAR(out.offset[E::R_Hand01].x, 0.0f, 1e-6f);
    EXPECT_NEAR(out.offset[E::R_UpperArm01].x, 0.0f, 1e-6f);
    EXPECT_NEAR(out.offset[E::L_Hand01].x, 0.5f * 0.2f * 0.98f / 1.58f, 1e-3f);
}

TEST(Evasion, IkOwnedArmsGetTheGainsExtra) {
    // Your real hand already leaned 0.2 m with you; with gain 1.3 the body's hurtboxes go 30 %
    // further, and so does the arm (only the extra), so it does not trail the body.
    E::Input base;
    base.ik_right = 1.0f;
    const E::Output out = Lean(E::Config{}, {0.2f, 0.0f, 0.0f}, base);
    const float profile = 0.2f * 0.98f / 1.58f;
    EXPECT_NEAR(out.offset[E::R_Hand01].x, 0.3f * profile, 1e-3f);
    EXPECT_NEAR(out.offset[E::L_Hand01].x, 1.3f * profile, 1e-3f); // the game's arm: all of it
}

TEST(Evasion, PelvisCanStayPut) {
    E::Config cfg = Unity();
    cfg.pelvis = false;
    const E::Output out = Lean(cfg, {0.2f, 0.0f, 0.0f});
    EXPECT_NEAR(out.offset[E::Pelvis01].x, 0.0f, 1e-6f);
    EXPECT_NEAR(out.offset[E::Head01].x, 0.2f, 1e-4f);
}

TEST(Evasion, NeckModelIgnoresHeadRotation) {
    const E::Vec3 neck0{0.0f, 1.6f, 0.0f};
    // Eye for an identity head: 7.5 cm up, 8 cm forward (-z).
    const E::Vec3 eye0{0.0f, 1.675f, -0.08f};
    const E::Vec3 n0 = E::NeckFromEye(eye0, 0.0f, 0.0f, 0.0f, 1.0f);
    EXPECT_NEAR(n0.x, neck0.x, 1e-5f);
    EXPECT_NEAR(n0.y, neck0.y, 1e-5f);
    EXPECT_NEAR(n0.z, neck0.z, 1e-5f);

    // Yaw 90 degrees left about +y: forward (-z) becomes -x.
    const float s = std::sqrt(0.5f);
    const E::Vec3 eye_yaw{-0.08f, 1.675f, 0.0f};
    const E::Vec3 n1 = E::NeckFromEye(eye_yaw, 0.0f, s, 0.0f, s);
    EXPECT_LT(std::hypot(n1.x - neck0.x, n1.z - neck0.z), 0.01f);
    EXPECT_NEAR(n1.y, neck0.y, 1e-4f);

    // Pitch 45 degrees down about +x (negative angle): the eyes swing forward and down.
    const float a = -0.3926991f; // half of -45 degrees
    const float qx = std::sin(a), qw = std::cos(a);
    const float c = std::cos(-0.7853982f), sn = std::sin(-0.7853982f);
    const E::Vec3 eye_pitch{0.0f, 1.6f + 0.075f * c - (-0.08f) * sn,
                            0.0f + 0.075f * sn + (-0.08f) * c};
    const E::Vec3 n2 = E::NeckFromEye(eye_pitch, qx, 0.0f, 0.0f, qw);
    EXPECT_LT(std::hypot(n2.x - neck0.x, n2.y - neck0.y, n2.z - neck0.z), 0.01f);
}

TEST(Evasion, BodyNamesMatchTheEnum) {
    EXPECT_EQ(std::string(E::BodyNames[E::Head01]), std::string("Head01"));
    EXPECT_EQ(std::string(E::BodyNames[E::R_Hand01]), std::string("R_Hand01"));
    EXPECT_EQ(E::BodyNames.size(), static_cast<size_t>(16));
}

// A kept hit or dodge (look_only): the torso alignment's shift and lean move the drawn trunk
// toward your head; the trunk's hurtboxes cancel them fully although evasion is off there (the
// game's animation decides where it can be hit), the IK arms keep them (they are drawn at your
// controllers). After the clip the full cancel holds while the lean fades back in; without
// look_only it fades with evasion (the old behaviour).
TEST(Evasion, LookOnlyShiftStaysOffTheTrunkHurtboxes) {
    const E::Config cfg;
    E::Model model;
    double t = 0.0;
    E::Input base;
    base.active = false;
    base.look_only = true;
    base.pose_shift = {0.2f, 0.0f, 0.1f};
    base.pose_lean = {0.1f, 0.0f, 0.0f};
    base.ik_right = 1.0f;
    E::Output out = Hold(model, cfg, {}, t, 0.5, base);
    const auto want = [&](E::Body b) {
        const float kl = cfg.lean_reach * E::LeanReach(b);
        return E::Vec3{-(0.2f * cfg.shift_reach + 0.1f * kl), 0.0f, -0.1f * cfg.shift_reach};
    };
    for (const E::Body b : {E::Head01, E::Spine02, E::Spine01, E::L_Hand01}) {
        EXPECT_NEAR(out.offset[b].x, want(b).x, 1e-5f) << E::BodyNames[b];
        EXPECT_NEAR(out.offset[b].z, want(b).z, 1e-5f) << E::BodyNames[b];
    }
    EXPECT_NEAR(out.offset[E::Pelvis01].x, 0.0f, 1e-6f);
    EXPECT_NEAR(out.offset[E::R_Hand01].x, 0.0f, 1e-6f);
    EXPECT_NEAR(out.offset[E::R_UpperArm01].x, 0.0f, 1e-6f);
    EXPECT_FLOAT_EQ(out.comp_weight, 1.0f);
    EXPECT_FLOAT_EQ(out.weight, 0.0f);
    // The clip ends, evasion comes back: the cancel never dips while the lean fades in.
    base.look_only = false;
    base.active = true;
    float lowest = 1.0f;
    for (int i = 0; i < 30; ++i, t += Dt) {
        E::Input in = base;
        in.t = t;
        out = model.Step(in, cfg);
        lowest = std::min(lowest, out.comp_weight);
    }
    EXPECT_FLOAT_EQ(lowest, 1.0f);
    // Without look_only and evasion off (a game-armed clip): it fades out with the rest.
    E::Model plain;
    t = 0.0;
    base.active = false;
    out = Hold(plain, cfg, {}, t, 0.5, base);
    EXPECT_NEAR(out.offset[E::Head01].x, 0.0f, 1e-6f);
    EXPECT_FLOAT_EQ(out.comp_weight, 0.0f);
}

} // namespace
