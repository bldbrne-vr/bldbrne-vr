// SPDX-License-Identifier: GPL-2.0-or-later

// The left hand's locomotion in left_scheme=2 (core/vr/left_locomotion.h): the sprint latch on the
// stick click, A's leap / dodge blend with its grace, the second click's leap, and the Circle
// edges the game's jump needs (the Sequencer); and the Y modifier (core/vr/stick_click.h's
// LeftClick driven by Y, as the presenter does in left_scheme=2).

#include <cmath>
#include <cstdint>

#include <gtest/gtest.h>

#include "core/vr/left_locomotion.h"
#include "core/vr/stick_click.h"

namespace {

namespace L = VR::Loco;

constexpr float Dt = 1.0f / 120.0f; // the presenter's input runs per XR frame (Quest 3: 120 Hz)

struct Rig {
    L::Controller c;
    L::Config cfg{};
    L::Out out{};
    int started = 0, ended = 0, leaps = 0, dodges = 0, armed = 0, no_dash = 0;
    float off = 0.0f, dx = 0.0f, dy = 0.0f;
    L::Cause cause = L::Cause::None;
    int dash = -1, window = -1; // the game's SpEffects 391 / 390 passed with each Step
    float facing = -1.0f;       // the stick off the hunter's facing (deg; -1 unknown)
    int jump = -1;              // the game's jump (7800) playing
    float rate = 1.0f;          // 1/60 over the game frame time (Frame::game_rate)
    bool ladder = false;
    int ahead_again = 0, leap_ends = 0;
    L::LeapEnd leap_end = L::LeapEnd::None;
    float leap_held = 0.0f, t = 0.0f, ended_at = -1.0f, leap_end_at = -1.0f;

    const L::Out& Step(bool click, bool a, float x, float y, int frames = 1, bool enabled = true) {
        for (int i = 0; i < frames; ++i) {
            L::Frame f{enabled, click, a, x, y, Dt};
            f.dash = dash;
            f.window = window;
            f.facing_off_deg = facing;
            f.jump = jump;
            f.game_rate = rate;
            f.ladder = ladder;
            out = c.Update(cfg, f);
            t += Dt;
            started += out.started ? 1 : 0;
            ended += out.ended ? 1 : 0;
            ended_at = out.ended ? t : ended_at;
            ahead_again += out.ahead_again ? 1 : 0;
            if (out.leap_end != L::LeapEnd::None) {
                ++leap_ends;
                leap_end = out.leap_end;
                leap_held = out.leap_held_s;
                leap_end_at = t;
            }
            no_dash += out.no_dash ? 1 : 0;
            armed += out.armed ? 1 : 0;
            if (out.action == L::Action::Leap) {
                ++leaps;
                cause = out.cause;
                off = out.off_deg;
            } else if (out.action == L::Action::Dodge) {
                ++dodges;
                cause = out.cause;
                off = out.off_deg;
                dx = out.dir_x;
                dy = out.dir_y;
            }
        }
        return out;
    }
    // A click (100 ms) with the stick at (x, y).
    void Click(float x, float y) {
        Step(true, false, x, y, 12);
        Step(false, false, x, y);
    }
};

TEST(LeftLoco, ClickLatchesTheSprintUntilTheStickCentres) {
    Rig r;
    r.Step(false, false, 0.0f, 0.9f, 10); // walking: no sprint
    EXPECT_FALSE(r.out.sprint);
    r.Click(0.0f, 0.9f);
    EXPECT_EQ(r.started, 1);
    EXPECT_TRUE(r.out.sprint);
    // Steering never ends it: hard right, then back-left, for 2 s.
    r.Step(false, false, 0.9f, 0.1f, 120);
    EXPECT_TRUE(r.out.sprint);
    r.Step(false, false, -0.5f, -0.6f, 120);
    EXPECT_TRUE(r.out.sprint);
    // A short pass through the centre (a flick across, 80 ms) keeps it.
    r.Step(false, false, 0.05f, 0.0f, 10);
    EXPECT_TRUE(r.out.sprint);
    r.Step(false, false, 0.0f, 0.9f, 10);
    EXPECT_TRUE(r.out.sprint);
    // Let go: it ends after centre_s (120 ms).
    r.Step(false, false, 0.0f, 0.1f, 13);
    EXPECT_TRUE(r.out.sprint);
    r.Step(false, false, 0.0f, 0.1f, 2);
    EXPECT_FALSE(r.out.sprint);
    EXPECT_EQ(r.ended, 1);
    EXPECT_EQ(r.leaps + r.dodges, 0);
}

TEST(LeftLoco, ClickWithTheStickCentredArmsTheLatch) {
    Rig r;
    r.Click(0.0f, 0.0f);
    EXPECT_EQ(r.armed, 1);
    EXPECT_FALSE(r.out.sprint);
    r.Step(false, false, 0.0f, 0.0f, 24); // 0.2 s later ...
    r.Step(false, false, 0.0f, 0.8f, 1);  // ... pushed: it starts
    EXPECT_TRUE(r.out.sprint);
    EXPECT_EQ(r.started, 1);
    // An armed click that is never followed by a push expires.
    Rig s;
    s.Click(0.0f, 0.0f);
    s.Step(false, false, 0.0f, 0.0f, 60); // 0.5 s
    s.Step(false, false, 0.0f, 0.8f, 10);
    EXPECT_FALSE(s.out.sprint);
    EXPECT_EQ(s.started, 0);
}

TEST(LeftLoco, AWithoutSprintIsTheGamesCircle) {
    Rig r;
    r.Step(false, false, 0.0f, 0.6f, 10);
    r.Step(false, true, 0.0f, 0.6f, 12); // a tap: the game's dodge
    EXPECT_TRUE(r.out.circle_a);
    r.Step(false, false, 0.0f, 0.6f);
    EXPECT_FALSE(r.out.circle_a);
    r.Step(false, true, 0.0f, 0.9f, 120); // a hold: the game's own sprint
    EXPECT_TRUE(r.out.circle_a);
    EXPECT_EQ(r.leaps + r.dodges, 0);
    EXPECT_FALSE(r.out.sprint);
}

TEST(LeftLoco, ASprintingForwardLeapsAndKeepsTheLatch) {
    Rig r;
    r.Click(0.0f, 1.0f);
    r.Step(false, false, 0.0f, 1.0f, 60);
    r.Step(false, true, 0.3f, 0.9f); // 18 deg right of the run: a leap
    EXPECT_EQ(r.leaps, 1);
    EXPECT_EQ(r.cause, L::Cause::A);
    EXPECT_NEAR(r.off, 18.4f, 0.5f);
    EXPECT_FALSE(r.out.circle_a); // swallowed ...
    r.Step(false, true, 0.3f, 0.9f, 30);
    EXPECT_FALSE(r.out.circle_a); // ... while held
    EXPECT_EQ(r.leaps, 1);
    EXPECT_TRUE(r.out.sprint); // the run goes on after the leap
    r.Step(false, false, 0.3f, 0.9f);
    r.Step(false, true, 0.3f, 0.9f); // another A: another leap
    EXPECT_EQ(r.leaps, 2);
}

TEST(LeftLoco, ASteeringHardDodgesOutOfTheSprint) {
    Rig r;
    r.Click(0.0f, 1.0f);
    r.Step(false, false, 0.0f, 1.0f, 60);
    r.Step(false, true, 1.0f, 0.0f); // 90 deg right: a dodge that way
    EXPECT_EQ(r.dodges, 1);
    EXPECT_EQ(r.leaps, 0);
    EXPECT_NEAR(r.off, 90.0f, 2.0f); // the run direction lags a sudden turn (run_delay_s)
    EXPECT_NEAR(r.dx, 1.0f, 1e-3f);
    EXPECT_NEAR(r.dy, 0.0f, 1e-3f);
    EXPECT_FALSE(r.out.sprint); // the latch ends with the dodge
    EXPECT_EQ(r.ended, 1);
    // Straight back: a dodge too.
    Rig b;
    b.Click(0.0f, 1.0f);
    b.Step(false, false, 0.0f, 1.0f, 60);
    b.Step(false, true, 0.0f, -1.0f);
    EXPECT_EQ(b.dodges, 1);
    EXPECT_NEAR(b.off, 180.0f, 1.0f);
}

TEST(LeftLoco, TheRunDirectionFollowsSteering) {
    Rig r;
    r.Click(0.0f, 1.0f);
    // A slow curve to the right over 1.5 s: the run turns with it, so an A then still leaps.
    for (int i = 0; i <= 180; ++i) {
        const float a = 1.5707963f * static_cast<float>(i) / 180.0f;
        r.Step(false, false, std::sin(a), std::cos(a));
    }
    r.Step(false, true, 1.0f, 0.0f);
    EXPECT_EQ(r.leaps, 1);
    EXPECT_EQ(r.dodges, 0);
    EXPECT_LT(r.off, 20.0f);
}

TEST(LeftLoco, ConeEdge) {
    for (const float deg : {55.0f, 65.0f}) {
        Rig r;
        r.Click(0.0f, 1.0f);
        r.Step(false, false, 0.0f, 1.0f, 60);
        const float rad = deg / 57.2957795f;
        r.Step(false, true, std::sin(rad), std::cos(rad));
        EXPECT_EQ(r.leaps, deg < 60.0f ? 1 : 0);
        EXPECT_EQ(r.dodges, deg < 60.0f ? 0 : 1);
    }
}

TEST(LeftLoco, GraceAfterTheLatchEnds) {
    Rig r;
    r.Click(0.0f, 1.0f);
    r.Step(false, false, 0.0f, 1.0f, 60);
    r.Step(false, false, 0.0f, 0.0f, 15); // 125 ms centred: ended
    EXPECT_FALSE(r.out.sprint);
    EXPECT_EQ(r.ended, 1);
    r.Step(false, false, 0.0f, 0.0f, 8); // 67 ms into the 80 ms grace
    r.Step(false, true, 0.0f, 0.0f);
    EXPECT_EQ(r.leaps, 1); // still a leap
    EXPECT_FALSE(r.out.circle_a);
    // Past the grace: the game's Circle.
    Rig s;
    s.Click(0.0f, 1.0f);
    s.Step(false, false, 0.0f, 1.0f, 60);
    s.Step(false, false, 0.0f, 0.0f, 15);
    s.Step(false, false, 0.0f, 0.0f, 11); // 92 ms
    s.Step(false, true, 0.0f, 0.0f);
    EXPECT_EQ(s.leaps, 0);
    EXPECT_TRUE(s.out.circle_a);
}

TEST(LeftLoco, SecondClickLeaps) {
    Rig r;
    r.Click(0.0f, 1.0f);
    r.Step(false, false, 0.0f, 1.0f, 60);
    r.Click(0.2f, 1.0f);
    EXPECT_EQ(r.leaps, 1);
    EXPECT_EQ(r.cause, L::Cause::Click);
    EXPECT_TRUE(r.out.sprint); // the latch goes on
    EXPECT_EQ(r.started, 1);
    // The game's own sprint (A held, stick past half way): a click leaps too.
    Rig a;
    a.Step(false, true, 0.0f, 0.9f, 120);
    a.Click(0.0f, 0.9f); // (A still held through Click: Step's a=false would release it)
    EXPECT_EQ(a.leaps, 0);
    Rig b;
    b.Step(false, true, 0.0f, 0.9f, 120);
    b.Step(true, true, 0.0f, 0.9f, 12);
    EXPECT_EQ(b.leaps, 1);
    EXPECT_EQ(b.cause, L::Cause::Click);
    EXPECT_FALSE(b.out.sprint); // no latch started by it
}

// A light push never latches (review 2026-10-05: an accidental click with the thumb resting on the
// stick, then a gentle step at 0.35, made the game backstep: Circle held while it still stands).
TEST(LeftLoco, ALightPushNeverLatches) {
    Rig r;
    r.Click(0.0f, 0.0f); // an accidental click, the stick centred: armed
    EXPECT_EQ(r.armed, 1);
    r.Step(false, false, 0.2f, 0.28f, 20); // a gentle step (0.34): no latch
    EXPECT_FALSE(r.out.sprint);
    EXPECT_EQ(r.started, 0);
    r.Step(false, false, 0.0f, 0.0f, 40); // the arm expires
    r.Step(false, false, 0.0f, 0.9f, 10);
    EXPECT_EQ(r.started, 0);
    // A click while walking slowly (0.4) arms; pushed past start_push soon after: the latch.
    Rig s;
    s.Step(false, false, 0.0f, 0.4f, 10);
    s.Click(0.0f, 0.4f);
    EXPECT_FALSE(s.out.sprint);
    EXPECT_EQ(s.armed, 1);
    s.Step(false, false, 0.0f, 0.7f, 2);
    EXPECT_TRUE(s.out.sprint);
    EXPECT_EQ(s.started, 1);
    // Once latched the stick may ease down to a walk: the latch holds until it is centred.
    s.Step(false, false, 0.0f, 0.3f, 60);
    EXPECT_TRUE(s.out.sprint);
}

// A second click right after the latching one (a bounce, a mash) is no leap (review 2026-10-05:
// click 44 ms, up 44 ms, down again leapt).
TEST(LeftLoco, SecondClickNeedsARealGap) {
    Rig r;
    r.Step(true, false, 0.0f, 1.0f, 5);  // 42 ms: the latch
    r.Step(false, false, 0.0f, 1.0f, 5); // 42 ms up
    r.Step(true, false, 0.0f, 1.0f, 4);  // down again
    EXPECT_EQ(r.started, 1);
    EXPECT_EQ(r.leaps, 0);
    EXPECT_TRUE(r.out.sprint);
    // The latch 0.3 s old but the click let go only 50 ms before: still ignored.
    r.Step(true, false, 0.0f, 1.0f, 30);
    r.Step(false, false, 0.0f, 1.0f, 6);
    r.Step(true, false, 0.0f, 1.0f, 2);
    EXPECT_EQ(r.leaps, 0);
    // Let go 0.15 s, the latch well past 0.25 s: a leap.
    r.Step(false, false, 0.0f, 1.0f, 18);
    r.Step(true, false, 0.0f, 1.0f, 2);
    EXPECT_EQ(r.leaps, 1);
    EXPECT_EQ(r.cause, L::Cause::Click);
}

// The run direction is where the stick pointed run_delay_s ago (review 2026-10-05: smoothed over
// 0.25 s, A while steering a curve at 240 deg/s or faster dodged).
TEST(LeftLoco, CurvesLeapSuddenPushesDodge) {
    for (const float w : {120.0f, 180.0f, 240.0f, 300.0f}) {
        Rig r;
        r.Click(0.0f, 1.0f);
        float ang = 0.0f;
        for (int i = 0; i < 120; ++i) { // 1 s of a curve at w deg/s, A on the last frame
            ang += w * Dt;
            const float rad = ang / 57.2957795f;
            r.Step(false, i == 119, std::sin(rad), std::cos(rad));
        }
        EXPECT_EQ(r.leaps, 1) << "curve at " << w << " deg/s";
        EXPECT_EQ(r.dodges, 0) << "curve at " << w << " deg/s";
        EXPECT_LT(r.off, 40.0f);
    }
    // A sudden push sideways (or back), A within run_delay_s: a dodge that way; later the run has
    // turned with it (the game runs where the stick points): a leap.
    for (const int ms : {0, 50, 100, 150, 250}) {
        Rig r;
        r.Click(0.0f, 1.0f);
        r.Step(false, false, 0.0f, 1.0f, 60);
        const int n = ms * 120 / 1000;
        r.Step(false, false, 1.0f, 0.0f, n);
        r.Step(false, true, 1.0f, 0.0f);
        EXPECT_EQ(r.dodges, ms <= 100 ? 1 : 0) << "A " << ms << " ms after the push";
        EXPECT_EQ(r.leaps, ms <= 100 ? 0 : 1) << "A " << ms << " ms after the push";
    }
}

TEST(LeftLoco, DisabledResets) {
    Rig r;
    r.Click(0.0f, 1.0f);
    EXPECT_TRUE(r.out.sprint);
    r.Step(false, false, 0.0f, 1.0f, 1, false);
    EXPECT_FALSE(r.out.sprint);
    EXPECT_EQ(r.ended, 1);
    r.Step(false, false, 0.0f, 1.0f, 30);
    EXPECT_FALSE(r.out.sprint); // nothing comes back by itself
    // A click held through the context switch is no new click.
    Rig s;
    s.Step(true, false, 0.0f, 1.0f, 5, false);
    s.Step(true, false, 0.0f, 1.0f, 5);
    EXPECT_EQ(s.started, 0);
}

// The latch ends when the game stops dashing with the stick pushed (review #9, 2026-10-05: out of
// stamina, after a swing or a shot, lock-on strafing the latch kept Circle held, which keeps the
// stamina from recovering): 391 off for no_dash_s (0.4 s), 390 off. No grace: A is the game's own.
TEST(LeftLocoNoDash, OutOfStaminaEndsTheLatch) {
    Rig r;
    r.dash = r.window = 0;
    r.Click(0.0f, 1.0f);
    EXPECT_TRUE(r.out.sprint);
    r.Step(false, false, 0.0f, 1.0f, 50); // the game breaks into its dash 0.5 s later
    r.dash = 1;
    r.Step(false, false, 0.3f, 0.95f, 360); // 3 s of dashing, steered
    EXPECT_TRUE(r.out.sprint);
    EXPECT_EQ(r.no_dash, 0);
    // Out of stamina: the dash stops (391 off), its jump window (390) for 0.2 s, then neither.
    r.dash = 0;
    r.window = 1;
    r.Step(false, false, 0.3f, 0.95f, 24);
    r.window = 0;
    r.Step(false, false, 0.3f, 0.95f, 23); // 0.39 s without the dash
    EXPECT_TRUE(r.out.sprint);
    r.Step(false, false, 0.3f, 0.95f, 1); // 0.4 s: Circle let go
    EXPECT_FALSE(r.out.sprint);
    EXPECT_TRUE(r.out.no_dash);
    EXPECT_TRUE(r.out.ended);
    EXPECT_NEAR(r.out.no_dash_s, 0.4f, Dt);
    EXPECT_EQ(r.no_dash, 1);
    EXPECT_EQ(r.ended, 1);
    // Walking on, the stick still pushed: nothing comes back by itself (the stamina recovers).
    r.Step(false, false, 0.3f, 0.95f, 240);
    EXPECT_FALSE(r.out.sprint);
    EXPECT_EQ(r.started, 1);
    // A click sprints again.
    r.Click(0.0f, 1.0f);
    EXPECT_TRUE(r.out.sprint);
    EXPECT_EQ(r.started, 2);
    EXPECT_EQ(r.leaps, 0);
    // No grace: an A right after it is the game's Circle (a roll), never a leap.
    Rig g;
    g.dash = 1;
    g.window = 0;
    g.Click(0.0f, 1.0f);
    g.Step(false, false, 0.0f, 1.0f, 60);
    g.dash = 0;
    g.Step(false, false, 0.0f, 1.0f, 48);
    EXPECT_EQ(g.no_dash, 1);
    g.Step(false, true, 0.0f, 1.0f);
    EXPECT_EQ(g.leaps + g.dodges, 0);
    EXPECT_TRUE(g.out.circle_a);
}

// The window (390) on: never ended inside it (the dash just stopped; a leap may still come).
TEST(LeftLocoNoDash, NeverInsideTheJumpWindow) {
    Rig r;
    r.dash = 1;
    r.window = 0;
    r.Click(0.0f, 1.0f);
    r.Step(false, false, 0.0f, 1.0f, 60);
    r.dash = 0;
    r.window = 1; // a window stuck on (as if): the latch holds
    r.Step(false, false, 0.0f, 1.0f, 120);
    EXPECT_TRUE(r.out.sprint);
    r.window = 0; // gone: the time already off counts
    r.Step(false, false, 0.0f, 1.0f);
    EXPECT_FALSE(r.out.sprint);
    EXPECT_EQ(r.no_dash, 1);
}

// A normal sprint is untouched: the dash flickering off for a few frames (a turn, a clip change)
// or for 0.3 s keeps the latch, and so does any time with no reading.
TEST(LeftLocoNoDash, ANormalSprintIsUnaffected) {
    Rig r;
    r.window = 0;
    r.dash = 0;
    r.Click(0.0f, 1.0f);
    r.Step(false, false, 0.0f, 1.0f, 40); // the dash starts 0.44 s after the latch
    for (int k = 0; k < 10; ++k) {        // 10 x (0.5 s dashing, 2 frames off)
        r.dash = 1;
        r.Step(false, false, 0.0f, 1.0f, 60);
        r.dash = 0;
        r.Step(false, false, 0.0f, 1.0f, 2);
    }
    r.dash = 1;
    r.Step(false, false, 0.0f, 1.0f, 60);
    r.dash = 0;
    r.Step(false, false, 0.0f, 1.0f, 36); // 0.3 s off
    r.dash = 1;
    r.Step(false, false, 0.0f, 1.0f, 60);
    EXPECT_TRUE(r.out.sprint);
    EXPECT_EQ(r.no_dash, 0);
    // No reading (third person, dodge_camera=0, a paused game): never judged.
    Rig n;
    n.Click(0.0f, 1.0f);
    n.Step(false, false, 0.0f, 1.0f, 600);
    EXPECT_TRUE(n.out.sprint);
    EXPECT_EQ(n.no_dash, 0);
    // The stick centred is the centre rule's (the dash stops as the hunter does): no no-dash end.
    Rig c;
    c.dash = 1;
    c.window = 0;
    c.Click(0.0f, 1.0f);
    c.Step(false, false, 0.0f, 1.0f, 60);
    c.dash = 0;
    c.Step(false, false, 0.05f, 0.0f, 10); // a flick across the centre (83 ms) ...
    c.Step(false, false, 0.0f, 1.0f, 40);  // ... then 0.33 s pushed without the dash
    EXPECT_TRUE(c.out.sprint);
    c.Step(false, false, 0.0f, 0.0f, 15);
    EXPECT_FALSE(c.out.sprint);
    EXPECT_EQ(c.no_dash, 0);
    EXPECT_EQ(c.ended, 1);
}

TEST(LeftLocoNoDash, LadderClimbAndSlideKeepTheLatch) {
    // Descending may face any direction against the camera, or have no facing reading
    // while the cinematic screen is up. None of those cases is a ground dash.
    for (float direction : {-1.0f, 1.0f}) {
        for (float facing : {-1.0f, 0.0f, 90.0f, 180.0f}) {
            Rig r;
            r.dash = r.window = 0;
            r.ladder = true;
            r.facing = facing;
            r.Click(0.0f, direction);
            r.Step(false, false, 0.0f, direction, 1200); // a long ladder, ten seconds
            EXPECT_TRUE(r.out.sprint);
            EXPECT_EQ(r.ended, 0);
            EXPECT_EQ(r.no_dash, 0);
        }
    }
}

TEST(LeftLocoNoDash, MountingAfterASprintKeepsTheLatch) {
    Rig r;
    r.dash = 1;
    r.window = 0;
    r.Click(0.0f, 1.0f);
    r.Step(false, false, 0.0f, 1.0f, 120);
    r.dash = 0;
    r.Step(false, false, 0.0f, 1.0f, 30); // some no-dash time before the ladder is detected
    r.ladder = true;
    r.Step(false, false, 0.0f, 1.0f, 1200);
    EXPECT_TRUE(r.out.sprint);
    EXPECT_EQ(r.no_dash, 0);
}

TEST(LeftLocoNoDash, LadderStillEndsWhenTheStickIsCentred) {
    Rig r;
    r.dash = r.window = 0;
    r.ladder = true;
    r.Click(0.0f, -1.0f);
    r.Step(false, false, 0.0f, -1.0f, 240);
    r.Step(false, false, 0.0f, 0.0f, 12); // 100 ms, inside the 120 ms centre threshold
    EXPECT_TRUE(r.out.sprint);
    r.Step(false, false, 0.0f, 0.0f, 4);
    EXPECT_FALSE(r.out.sprint);
    EXPECT_EQ(r.ended, 1);
    EXPECT_EQ(r.no_dash, 0);
}

TEST(LeftLocoNoDash, DismountAllowsDashStartupThenResumesCancellation) {
    Rig r;
    r.dash = 1;
    r.window = 0;
    r.Click(0.0f, 1.0f);
    r.Step(false, false, 0.0f, 1.0f, 120);
    r.ladder = true;
    r.dash = 0;
    r.Step(false, false, 0.0f, 1.0f, 1200);
    r.ladder = false;
    r.Step(false, false, 0.0f, 1.0f, 108); // 0.9 s: a fresh startup allowance
    ASSERT_TRUE(r.out.sprint);
    EXPECT_EQ(r.no_dash, 0);
    r.dash = 1;
    r.Step(false, false, 0.0f, 1.0f, 120);
    ASSERT_TRUE(r.out.sprint);
    r.dash = 0; // exhausted on the ground: the normal 0.4 s cutoff still applies
    r.Step(false, false, 0.0f, 1.0f, 47);
    EXPECT_TRUE(r.out.sprint);
    r.Step(false, false, 0.0f, 1.0f, 1);
    EXPECT_FALSE(r.out.sprint);
    EXPECT_EQ(r.no_dash, 1);
}

TEST(LeftLocoNoDash, DismountWithoutADashStillEndsTheLatch) {
    Rig r;
    r.dash = r.window = 0;
    r.ladder = true;
    r.Click(0.0f, -1.0f);
    r.Step(false, false, 0.0f, -1.0f, 1200);
    r.ladder = false;
    r.Step(false, false, 0.0f, 1.0f, 108);
    EXPECT_TRUE(r.out.sprint);
    r.Step(false, false, 0.0f, 1.0f, 24);
    EXPECT_FALSE(r.out.sprint);
    EXPECT_EQ(r.no_dash, 1);
}

// The game takes ~0.3-0.6 s to break into its dash: judged once it was seen, or dash_start_s
// (1 s) after the latch began (no dash at all: a click with no stamina left).
TEST(LeftLocoNoDash, TheDashMayStartLate) {
    Rig r;
    r.dash = r.window = 0;
    r.Click(0.0f, 1.0f);                  // latched at the click's first frame
    r.Step(false, false, 0.0f, 1.0f, 95); // 0.9 s: no dash yet
    EXPECT_TRUE(r.out.sprint);
    r.dash = 1;
    r.Step(false, false, 0.0f, 1.0f, 120);
    EXPECT_TRUE(r.out.sprint);
    EXPECT_EQ(r.no_dash, 0);
    Rig s;
    s.dash = s.window = 0;
    s.Click(0.0f, 1.0f);
    s.Step(false, false, 0.0f, 1.0f, 106); // 0.98 s
    EXPECT_TRUE(s.out.sprint);
    s.Step(false, false, 0.0f, 1.0f, 2);
    EXPECT_FALSE(s.out.sprint);
    EXPECT_EQ(s.no_dash, 1);
}

// A leap: its sequence lets go of the sprint (the presenter passes no reading meanwhile), then the
// jump, whose start turns 391 on for a moment (mock run nodash/nd1 N3: on 67 ms after the tap, 390
// 0.23 s, then off; judged at once, that blip ended the latch 0.6 s after the leap, mid-jump), and
// its clip runs ~1.6 s before the game dashes again with Circle held (loco3: 96 game frames of
// 7800, then 5200; nodash/nd3 with a 1.5 s allowance: the latch ended 0.12 s before the landing and
// he ran): without a reading of the game's jump (here) nothing is read for leap_hold_s (1.3 s,
// game time), then judged afresh, as after the latch's start (with one: ALeapHoldsUntil...).
TEST(LeftLocoNoDash, ALeapIsJudgedAfresh) {
    // The game's readings after a leap's frame, as measured (120 Hz frames).
    const auto jump = [](Rig& r) {
        r.dash = r.window = -1; // the Circle sequence (67 ms)
        r.Step(false, false, 0.0f, 1.0f, 8);
        r.dash = 1; // the jump's start: 391 for a moment
        r.window = 0;
        r.Step(false, false, 0.0f, 1.0f, 8);
        r.dash = 0; // then its window
        r.window = 1;
        r.Step(false, false, 0.0f, 1.0f, 27);
        r.window = 0; // 43 frames (0.36 s) after the leap: neither
    };
    for (const bool dash_back : {true, false}) {
        Rig r;
        r.dash = 1;
        r.window = 0;
        r.Click(0.0f, 1.0f);
        r.Step(false, false, 0.0f, 1.0f, 60);
        r.Step(false, true, 0.0f, 1.0f); // A: a leap (the game still dashing this frame)
        EXPECT_EQ(r.leaps, 1);
        jump(r);
        if (dash_back) {
            r.Step(false, false, 0.0f, 1.0f, 161); // 1.7 s after the leap: still latched ...
            EXPECT_TRUE(r.out.sprint);
            r.dash = 1; // ... and the run goes on after the landing: the latch with it
            r.Step(false, false, 0.0f, 1.0f, 120);
            EXPECT_TRUE(r.out.sprint);
            EXPECT_EQ(r.no_dash, 0);
            r.dash = 0; // judged as any run again: out of stamina, 0.4 s
            r.Step(false, false, 0.0f, 1.0f, 47);
            EXPECT_TRUE(r.out.sprint);
            r.Step(false, false, 0.0f, 1.0f, 1);
            EXPECT_FALSE(r.out.sprint);
            EXPECT_EQ(r.no_dash, 1);
        } else {
            r.Step(false, false, 0.0f, 1.0f, 232); // 2.29 s after the leap
            EXPECT_TRUE(r.out.sprint);
            r.Step(false, false, 0.0f, 1.0f, 2); // 2.3 s (the hold + dash_start_s): ended
            EXPECT_FALSE(r.out.sprint);
            EXPECT_EQ(r.no_dash, 1);
        }
    }
    // A second click's leap too (its own frame is not judged; the click held through the hold).
    Rig k;
    k.dash = 1;
    k.window = 0;
    k.Click(0.0f, 1.0f);
    k.Step(false, false, 0.0f, 1.0f, 60);
    k.Step(true, false, 0.0f, 1.0f); // the second click: a leap
    EXPECT_EQ(k.leaps, 1);
    EXPECT_EQ(k.cause, L::Cause::Click);
    k.dash = k.window = -1;
    k.Step(true, false, 0.0f, 1.0f, 11);
    jump(k); // 43 frames more
    k.Step(false, false, 0.0f, 1.0f, 221); // 2.29 s after the leap
    EXPECT_TRUE(k.out.sprint);
    EXPECT_EQ(k.no_dash, 0);
    k.Step(false, false, 0.0f, 1.0f, 2);
    EXPECT_FALSE(k.out.sprint);
    EXPECT_EQ(k.no_dash, 1);
}

// Sideways or back of the hunter (body_facing=1, a lock-on) the game strafes, which never dashes
// (nd4 R3: the latch ended 0.25 s into a strafe): the stick beyond dash_cone_deg (50) of his
// facing is never judged and the latch holds; back ahead it is judged afresh (the game dashes
// again by itself: nd2 Q1). Unknown facing (no reading): judged as ahead, as before.
TEST(LeftLocoNoDash, AStrafeHoldsTheLatch) {
    Rig r;
    r.dash = 1;
    r.window = 0;
    r.facing = 0.0f;
    r.Click(0.0f, 1.0f);
    r.Step(false, false, 0.0f, 1.0f, 120); // 1 s dashing ahead
    r.dash = 0;                            // hard right: the strafe run (4003), no dash
    r.facing = 90.0f;
    r.Step(false, false, 1.0f, 0.0f, 180); // 1.5 s
    EXPECT_TRUE(r.out.sprint);
    EXPECT_FALSE(r.out.ahead);
    EXPECT_EQ(r.no_dash, 0);
    r.facing = 0.0f; // ahead again: judged afresh, the game breaks into its dash 0.6 s later
    r.Step(false, false, 0.0f, 1.0f);
    EXPECT_TRUE(r.out.ahead_again);
    r.Step(false, false, 0.0f, 1.0f, 71);
    r.dash = 1;
    r.Step(false, false, 0.0f, 1.0f, 120);
    EXPECT_TRUE(r.out.sprint);
    EXPECT_EQ(r.no_dash, 0);
    EXPECT_EQ(r.ahead_again, 1);
    // Back (180 deg) for 3 s with the dash off: still held.
    r.dash = 0;
    r.facing = 180.0f;
    r.Step(false, false, 0.0f, -1.0f, 360);
    EXPECT_TRUE(r.out.sprint);
    EXPECT_EQ(r.no_dash, 0);
    // Ahead with no dash at all (out of stamina): judged afresh, so dash_start_s (1 s) after the
    // stick came back.
    r.facing = 10.0f;
    r.Step(false, false, 0.0f, 1.0f, 119);
    EXPECT_TRUE(r.out.sprint);
    r.Step(false, false, 0.0f, 1.0f, 2);
    EXPECT_FALSE(r.out.sprint);
    EXPECT_EQ(r.no_dash, 1);
    EXPECT_EQ(r.ahead_again, 2);
    // No facing reading (third person, the virtual screen): judged as ahead (sideways ends it).
    Rig n;
    n.dash = 1;
    n.window = 0;
    n.Click(0.0f, 1.0f);
    n.Step(false, false, 0.0f, 1.0f, 60);
    n.dash = 0;
    n.Step(false, false, 1.0f, 0.0f, 47);
    EXPECT_TRUE(n.out.sprint);
    n.Step(false, false, 1.0f, 0.0f, 1);
    EXPECT_FALSE(n.out.sprint);
    EXPECT_EQ(n.no_dash, 1);
}

// The cone's edge (50 deg) and a stick sweeping through it: judged inside, held outside.
TEST(LeftLocoNoDash, TheConeEdge) {
    for (const float off : {49.0f, 51.0f}) {
        Rig r;
        r.dash = 1;
        r.window = 0;
        r.facing = 0.0f;
        r.Click(0.0f, 1.0f);
        r.Step(false, false, 0.0f, 1.0f, 60);
        r.dash = 0;
        r.facing = off;
        r.Step(false, false, 0.75f, 0.66f, 60);
        EXPECT_EQ(r.out.sprint, off > 50.0f);
        EXPECT_EQ(r.no_dash, off > 50.0f ? 0 : 1);
    }
    // Out of stamina while steering a curve that leaves the cone for a moment (0.2 s at 60 deg):
    // the count starts again when it comes back, and the end comes dash_start_s after that.
    Rig s;
    s.dash = 1;
    s.window = 0;
    s.facing = 0.0f;
    s.Click(0.0f, 1.0f);
    s.Step(false, false, 0.0f, 1.0f, 60);
    s.dash = 0;
    s.Step(false, false, 0.0f, 1.0f, 36); // 0.3 s without the dash ...
    s.facing = 60.0f;
    s.Step(false, false, 0.8f, 0.6f, 24); // ... a strafe for 0.2 s ...
    s.facing = 20.0f;
    s.Step(false, false, 0.3f, 0.9f, 119); // ... ahead again: 0.99 s
    EXPECT_TRUE(s.out.sprint);
    s.Step(false, false, 0.3f, 0.9f, 2);
    EXPECT_FALSE(s.out.sprint);
    EXPECT_EQ(s.no_dash, 1);
}

// A leap with the game's jump read (its clip 7800, bloodborne_motion's publication): held until
// the jump has started and ended, then judged afresh; whatever the jump's length (a slow game,
// a fall after it).
TEST(LeftLocoNoDash, ALeapHoldsUntilTheGamesJumpEnds) {
    // The leap's frame, then the jump: off for `before` frames, on for `on` frames, then off.
    const auto leap = [](Rig& r, int before, int on) {
        r.dash = 1;
        r.window = 0;
        r.jump = 0;
        r.facing = 0.0f;
        r.Click(0.0f, 1.0f);
        r.Step(false, false, 0.0f, 1.0f, 60);
        r.Step(false, true, 0.0f, 1.0f); // A: a leap
        EXPECT_EQ(r.leaps, 1);
        r.dash = r.window = -1; // its Circle sequence
        r.Step(false, false, 0.0f, 1.0f, 8);
        r.dash = r.window = 0;
        r.Step(false, false, 0.0f, 1.0f, before - 8);
        r.jump = 1;
        r.Step(false, false, 0.0f, 1.0f, on);
        EXPECT_TRUE(r.out.sprint);
        EXPECT_EQ(r.leap_ends, 0);
        r.jump = 0;
    };
    {
        // As measured: the jump 0.12 s after A, 1.6 s long (+ 0.25 s until its pushes are seen
        // to stop); the dash back 0.1 s after that.
        Rig r;
        leap(r, 14, 222);
        r.Step(false, false, 0.0f, 1.0f);
        EXPECT_EQ(r.leap_ends, 1);
        EXPECT_EQ(r.leap_end, L::LeapEnd::Jump);
        EXPECT_NEAR(r.leap_held, 237.0f * Dt, 1e-3f);
        r.Step(false, false, 0.0f, 1.0f, 11);
        r.dash = 1;
        r.Step(false, false, 0.0f, 1.0f, 120);
        EXPECT_TRUE(r.out.sprint);
        EXPECT_EQ(r.no_dash, 0);
    }
    {
        // A long jump (a slow game, a fall off a ledge: 2.5 s), no dash after it (out of
        // stamina): held throughout, ended dash_start_s after the jump.
        Rig r;
        leap(r, 14, 300);
        r.Step(false, false, 0.0f, 1.0f, 120);
        EXPECT_TRUE(r.out.sprint);
        EXPECT_EQ(r.leap_end, L::LeapEnd::Jump);
        r.Step(false, false, 0.0f, 1.0f, 1);
        EXPECT_FALSE(r.out.sprint);
        EXPECT_EQ(r.no_dash, 1);
    }
    {
        // No jump at all (a reading that never shows it: the tap rolled): leap_start_s (0.5 s).
        Rig r;
        r.dash = 1;
        r.window = 0;
        r.jump = 0;
        r.Click(0.0f, 1.0f);
        r.Step(false, false, 0.0f, 1.0f, 60);
        r.Step(true, false, 0.0f, 1.0f); // a second click
        EXPECT_EQ(r.leaps, 1);
        r.dash = 0;
        r.Step(false, false, 0.0f, 1.0f, 59);
        EXPECT_EQ(r.leap_ends, 0);
        r.Step(false, false, 0.0f, 1.0f, 1);
        EXPECT_EQ(r.leap_ends, 1);
        EXPECT_EQ(r.leap_end, L::LeapEnd::NoJump);
        r.Step(false, false, 0.0f, 1.0f, 119);
        EXPECT_TRUE(r.out.sprint);
        r.Step(false, false, 0.0f, 1.0f, 1);
        EXPECT_FALSE(r.out.sprint);
    }
    {
        // A reading stuck on: leap_max_s (3 s) at the most.
        Rig r;
        leap(r, 14, 100);
        r.jump = 1;
        r.Step(false, false, 0.0f, 1.0f, 600);
        EXPECT_EQ(r.leap_ends, 1);
        EXPECT_EQ(r.leap_end, L::LeapEnd::Timeout);
        EXPECT_NEAR(r.leap_held, 3.0f, 2.0f * Dt);
    }
}

// A slow game (40 fps: game_rate 2/3). The game keeps real time (fwd2c: capped at 40 fps its
// jump took 64 frames over 1575 ms), so the no-dash end stays 0.4 s; only the hold without the
// jump reading (1.3 s) is stretched, to 1.95 s (the safe side should a slower game run slow).
TEST(LeftLocoNoDash, ASlowGameStretchesTheFallbackHold) {
    Rig r;
    r.rate = 40.0f / 60.0f;
    r.dash = 1;
    r.window = 0;
    r.Click(0.0f, 1.0f);
    r.Step(false, false, 0.0f, 1.0f, 120);
    r.Step(false, true, 0.0f, 1.0f); // A: a leap, no jump reading
    const float leap_t = r.t;
    r.dash = 0;
    r.Step(false, false, 0.0f, 1.0f, 300);
    EXPECT_EQ(r.leap_end, L::LeapEnd::Time);
    EXPECT_NEAR(r.leap_end_at - leap_t, 1.95f, 2.0f * Dt);
    EXPECT_TRUE(r.out.sprint); // 2.5 s after the leap (1.95 s + 1 s for dash_start_s)
    r.dash = 1;
    r.Step(false, false, 0.0f, 1.0f, 60);
    r.dash = 0;
    const float off_t = r.t;
    r.Step(false, false, 0.0f, 1.0f, 120);
    EXPECT_FALSE(r.out.sprint);
    EXPECT_EQ(r.no_dash, 1);
    EXPECT_NEAR(r.ended_at - off_t, 0.4f, 2.0f * Dt);
    // With the jump reading the rate changes nothing: the hold ends with the jump.
    Rig j;
    j.rate = 40.0f / 60.0f;
    j.dash = 1;
    j.window = 0;
    j.jump = 0;
    j.Click(0.0f, 1.0f);
    j.Step(false, false, 0.0f, 1.0f, 60);
    j.Step(false, true, 0.0f, 1.0f);
    j.dash = 0;
    j.Step(false, false, 0.0f, 1.0f, 15);
    j.jump = 1;
    j.Step(false, false, 0.0f, 1.0f, 220);
    j.jump = 0;
    j.Step(false, false, 0.0f, 1.0f);
    EXPECT_EQ(j.leap_end, L::LeapEnd::Jump);
    EXPECT_NEAR(j.leap_held, 236.0f * Dt, 1e-3f);
    EXPECT_TRUE(j.out.sprint);
}

// The Sequencer at 60 game fps, 120 Hz input. `window` / `dash`: the frames (from the start) the
// game reports SpEffect 390 / 391 on; known=false: no reading.
struct Seq {
    L::Sequencer s;
    L::SeqConfig cfg{};
    L::SeqOut last_release{};
    float t = 0.0f, press_at = -1.0f, release_at = -1.0f, done_at = -1.0f;
    int downs = 0; // frames with Circle forced down
    int ups = 0;   // frames with Circle forced up

    void Run(int frames, int w_from, int w_to, int d_to, float frame_s = 1.0f / 60.0f,
             bool known = true) {
        for (int i = 0; i < frames; ++i) {
            int w = -1, d = -1;
            if (known) {
                w = (i >= w_from && i < w_to) ? 1 : 0;
                d = i < d_to ? 1 : 0;
            }
            const L::SeqOut o = s.Update(cfg, {Dt, frame_s, w, d});
            if (o.pressed) {
                press_at = t;
            }
            if (o.released) {
                release_at = t;
                last_release = o;
            }
            if (o.done) {
                done_at = t;
            }
            downs += o.circle == 1 ? 1 : 0;
            ups += o.circle == 0 ? 1 : 0;
            t += Dt;
        }
    }
};

// SpEffect 390 (the jump window) at 120 Hz: on from the start (the dash) until frame 36 (0.3 s
// after the release); 391 (the dash) until frame 2.
constexpr int WFrom = 0, WTo = 36, DashTo = 2;

TEST(LeftLocoSequencer, LeapLetsGoAsSoonAsTheGameSeesTheTap) {
    Seq q;
    q.s.Start(L::Action::Leap, true);
    q.Run(80, WFrom, WTo, DashTo);
    EXPECT_NEAR(q.press_at, 3 * Dt, 1e-4f);         // 1.5 game frames up (25 ms)
    EXPECT_NEAR(q.release_at, (3 + 4) * Dt, 1e-4f); // 2 game frames down: 58 ms after the release
    EXPECT_TRUE(q.last_release.by_window);
    // Measured: let go 0.06-0.22 s after the sprint's release jumped.
    EXPECT_GT(q.release_at, 0.05f);
    EXPECT_GT(q.done_at, q.release_at);
    EXPECT_FALSE(q.s.Active());
}

TEST(LeftLocoSequencer, LeapWaitsForALateWindow) {
    Seq q;
    q.s.Start(L::Action::Leap, true);
    q.Run(80, 12, 36, 12); // 390 only from 100 ms
    EXPECT_NEAR(q.release_at, 12 * Dt, 1e-4f);
    EXPECT_TRUE(q.last_release.by_window);
}

TEST(LeftLocoSequencer, LeapWithoutAReadingUsesTheDelay) {
    Seq q;
    q.s.Start(L::Action::Leap, true);
    q.Run(80, 0, 0, 0, 1.0f / 60.0f, false);
    EXPECT_NEAR(q.release_at, q.cfg.leap_release_s, Dt + 1e-4f);
    EXPECT_GT(q.release_at, 0.06f); // inside the measured 0.06-0.22 s
    EXPECT_LT(q.release_at, 0.22f);
    EXPECT_FALSE(q.last_release.by_window);
}

TEST(LeftLocoSequencer, LeapGivesUpWaitingForTheWindow) {
    Seq q;
    q.s.Start(L::Action::Leap, true);
    q.Run(120, 1000, 1000, 1000); // dashing, but 390 never reported
    EXPECT_NEAR(q.release_at, q.cfg.leap_max_s, Dt + 1e-4f);
    EXPECT_LT(q.release_at, 0.22f);
}

TEST(LeftLocoSequencer, LeapWithoutADashRollsAtOnce) {
    Seq q;
    q.s.Start(L::Action::Leap, true);
    q.Run(60, 1000, 1000, 0); // the reading shows no dash (out of stamina: a run)
    EXPECT_TRUE(q.last_release.no_dash);
    EXPECT_NEAR(q.release_at, (3 + 4) * Dt, 1e-4f);
}

TEST(LeftLocoSequencer, LeapInTheGraceCountsTheTimeAlreadyUp) {
    Seq q;
    q.s.Start(L::Action::Leap, false, 0.06f); // the latch ended 60 ms ago
    q.Run(80, 0, 0, 0, 1.0f / 60.0f, false);
    EXPECT_NEAR(q.press_at, 0.0f, 1e-4f);         // Circle is up already: pressed at once
    EXPECT_NEAR(q.release_at, 0.04f, Dt + 1e-4f); // 2 game frames, 100 ms after the latch ended
}

TEST(LeftLocoSequencer, DodgeTapsAfterTheWindow) {
    Seq q;
    q.s.Start(L::Action::Dodge, true);
    q.Run(100, WFrom, WTo, DashTo);
    EXPECT_NEAR(q.press_at, WTo * Dt, 1e-4f);              // the first frame 390 is gone
    EXPECT_NEAR(q.release_at - q.press_at, 4 * Dt, 1e-4f); // 2 game frames
    EXPECT_TRUE(q.last_release.by_window);
}

TEST(LeftLocoSequencer, DodgeWithoutAReadingUsesTheMeasuredDelay) {
    Seq q;
    q.s.Start(L::Action::Dodge, true);
    q.Run(100, 0, 0, 0, 1.0f / 60.0f, false);
    // Measured: pressed 0.30 s after the sprint's release and let go at 0.40 s: a roll.
    EXPECT_NEAR(q.press_at, q.cfg.dodge_release_s - q.cfg.tap_s, Dt + 1e-4f);
    EXPECT_NEAR(q.release_at, q.cfg.dodge_release_s, Dt + 1e-4f);
}

TEST(LeftLocoSequencer, DodgeNeverPressedInTheWindow) {
    Seq q;
    q.s.Start(L::Action::Dodge, true);
    q.Run(120, 0, 60, 2, 1.0f / 30.0f); // a long window (a slow game): 0.5 s
    EXPECT_GE(q.press_at, 59.5f * Dt);
    EXPECT_GE(q.release_at - q.press_at, 2.0f / 30.0f - 1e-4f);
}

TEST(LeftLocoSequencer, DodgeGivesUpOnAStuckWindow) {
    Seq q;
    q.s.Start(L::Action::Dodge, true);
    q.Run(160, 0, 1000, 2);
    EXPECT_NEAR(q.press_at, q.cfg.dodge_max_s, Dt + 1e-4f);
}

TEST(LeftLocoSequencer, DodgeNotSprintingIsAPlainTap) {
    Seq q;
    q.s.Start(L::Action::Dodge, false, 10.0f);
    q.Run(40, 1000, 1000, 0);
    EXPECT_NEAR(q.press_at, 0.0f, 1e-4f);
    EXPECT_NEAR(q.release_at, 4 * Dt, 1e-4f);
}

TEST(LeftLocoSequencer, SlowGameStretchesTheTap) {
    Seq q;
    q.s.Start(L::Action::Leap, true);
    q.Run(120, 0, 60, 2, 1.0f / 30.0f);
    EXPECT_NEAR(q.press_at, 6 * Dt, 1e-4f);                     // 1.5 frames up (50 ms)
    EXPECT_GE(q.release_at - q.press_at, 2.0f / 30.0f - 1e-4f); // seen for 2 game frames
}

// The Y modifier (left_scheme=2): stick_click.h's LeftClick driven by Y, never sprinting.
TEST(LeftLocoY, TapIsTheArcHoldIsTheModifier) {
    VR::StickClick::LeftClick y;
    const VR::StickClick::Config cfg{true, 0.5f};
    int arcs = 0;
    for (int i = 0; i < 12; ++i) {
        const auto o = y.Update(cfg, {true, false, Dt});
        EXPECT_TRUE(o.modifier);
        EXPECT_FALSE(o.jump);
    }
    auto o = y.Update(cfg, {false, false, Dt});
    arcs += o.arc ? 1 : 0;
    EXPECT_EQ(arcs, 1);
    // Held with a combo (Y + right stick / B / X): no arc on the release.
    for (int i = 0; i < 30; ++i) {
        y.Update(cfg, {true, false, Dt});
        if (i == 10) {
            y.NoteCombo();
        }
    }
    o = y.Update(cfg, {false, false, Dt});
    EXPECT_FALSE(o.arc);
    EXPECT_FALSE(o.l3); // the caller ignores l3 for Y anyway
}

} // namespace
