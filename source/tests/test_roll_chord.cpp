// SPDX-License-Identifier: GPL-2.0-or-later

// The A+B chord (core/vr/roll_chord.h, roll_chord=1). Moving: the game's real roll toward the
// stick the moment the chord is complete. Standing still: a roll back on a release before 1.2 s,
// the roll toward the stick at once when it is pushed during the hold, the grip adjust's A+B at
// 1.2 s. Neither A nor B reaches the game during a chord; A pressed first waits at most 60 ms for
// B. While moving, B first waits at most 150 ms for A, including after B is released.
// Standing still, B is immediate. A's timing is unchanged; buffered presses last min_press_s.

#include <cmath>

#include <gtest/gtest.h>

#include "core/vr/roll_chord.h"

namespace {

namespace R = VR::Roll;

// (Steps are never called inside EXPECT_EQ: the harness's shim evaluates its arguments twice.)
constexpr float Dt = 1.0f / 120.0f; // the presenter's input runs per XR frame (Quest 3: 120 Hz)

int Frames(float seconds) {
    return static_cast<int>(std::lround(seconds / Dt));
}

struct Rig {
    R::Chord chord;
    R::Config cfg{};
    bool enabled = true;
    bool bypass = false;
    float x = 0.0f, y = 0.0f; // the left stick
    int frame = 0;            // steps so far
    // Counts over the steps since the last Clear.
    int a_frames = 0, b_frames = 0, a_taps = 0, b_taps = 0, chords = 0, ticks = 0, stills = 0,
        rolls = 0, released = 0, adjust_frames = 0;
    int roll_frame = -1; // the step of the last roll
    R::Output last{};
    R::Output roll{};

    R::Output Step(bool a, bool b, float dt = Dt) {
        R::Input in{};
        in.dt = dt;
        in.a = a;
        in.b = b;
        in.x = x;
        in.y = y;
        in.enabled = enabled;
        in.bypass = bypass;
        last = chord.Update(cfg, in);
        a_frames += last.a ? 1 : 0;
        b_frames += last.b ? 1 : 0;
        a_taps += last.a_tap ? 1 : 0;
        b_taps += last.b_tap ? 1 : 0;
        ticks += last.chord ? 1 : 0;
        adjust_frames += last.adjust ? 1 : 0;
        switch (last.event) {
        case R::Event::Chord:
            ++chords;
            break;
        case R::Event::Still:
            ++stills;
            break;
        case R::Event::Roll:
            ++rolls;
            roll = last;
            roll_frame = frame;
            break;
        case R::Event::Released:
            ++released;
            break;
        default:
            break;
        }
        ++frame;
        return last;
    }
    // Holds the buttons for `seconds`.
    void Hold(bool a, bool b, float seconds) {
        for (int i = 0, n = Frames(seconds); i < n; ++i) {
            Step(a, b);
        }
    }
    void Clear() {
        a_frames = b_frames = a_taps = b_taps = chords = ticks = stills = rolls = released = 0;
        adjust_frames = 0;
    }
    [[nodiscard]] int Leaks() const {
        return a_frames + b_frames + a_taps + b_taps;
    }
};

// One A (or B) press of `press_s` alone, then let go: the steps where the game's button goes
// down and up, today (roll_chord=0: straight to the game) or with the chord.
struct Edges {
    int down = -1, up = -1;
};
Edges AloneEdges(float press_s, bool chord_on, bool use_b = false, bool moving = false) {
    Rig r;
    r.enabled = chord_on;
    r.y = moving ? 1.0f : 0.0f;
    Edges e;
    const int n = Frames(press_s);
    bool was = false;
    for (int i = 0; i < n + 40; ++i) {
        const bool held = i < n;
        const R::Output o = r.Step(use_b ? false : held, use_b ? held : false);
        const bool now = use_b ? o.b : o.a;
        if (now && !was && e.down < 0) {
            e.down = i;
        }
        if (!now && was && e.up < 0) {
            e.up = i;
        }
        was = now;
    }
    return e;
}

} // namespace

// ---- A alone: the dodge and the sprint ---------------------------------------------------------

TEST(RollChord, AHeldIsForwardedAtTheWindow) {
    // A held (a sprint): it reaches the game once B can no longer join (60 ms), not later.
    Rig r;
    int first = -1;
    for (int i = 0; i < 40; ++i) {
        if (r.Step(true, false).a && first < 0) {
            first = i;
        }
    }
    EXPECT_NEAR(static_cast<float>(first) * Dt, r.cfg.a_window_s, 1.01f * Dt);
    EXPECT_LE(static_cast<float>(first) * Dt, 0.06f + 1e-4f);
    EXPECT_EQ(r.chords + r.ticks + r.rolls, 0);
    EXPECT_EQ(r.b_frames, 0);
}

TEST(RollChord, AReleaseReachesTheGameAsToday) {
    // Every A tap: the game's Circle goes up on the same step as today once the press has been
    // shown min_press_s; a quicker tap is sent at its release and shown min_press_s (the game
    // must sample it), so it is never lost and is late by at most min_press_s (in whole XR frames).
    const R::Config cfg{};
    for (float press = 0.01f; press <= 0.40f; press += 0.005f) {
        const Edges today = AloneEdges(press, false);
        const Edges now = AloneEdges(press, true);
        ASSERT_GE(now.down, 0) << press;
        ASSERT_GT(now.up, now.down) << press;
        // The game sees the press for at least min_press_s (or as long as today).
        EXPECT_GE(static_cast<float>(now.up - now.down) * Dt + 1e-4f,
                  std::min(cfg.min_press_s, static_cast<float>(today.up - today.down) * Dt))
            << press;
        const int late = now.up - today.up;
        EXPECT_GE(late, 0) << press;
        if (press >= cfg.a_window_s + cfg.min_press_s + Dt) {
            EXPECT_EQ(late, 0) << press; // the release at exactly today's instant
        } else {
            // whole XR frames
            EXPECT_LE(static_cast<float>(late) * Dt, cfg.min_press_s + Dt) << press;
        }
        // The press is held back at most a_window_s.
        EXPECT_LE(static_cast<float>(now.down - today.down) * Dt, cfg.a_window_s + 1e-4f)
            << press;
    }
}

TEST(RollChord, QuickATapIsSentAtItsRelease) {
    Rig r;
    r.Hold(true, false, 0.04f); // let go inside the window
    EXPECT_EQ(r.a_frames, 0);
    const R::Output o = r.Step(false, false);
    EXPECT_TRUE(o.a_tap);
    EXPECT_TRUE(o.a); // the press goes down on the release step
    r.Hold(false, false, 0.2f);
    EXPECT_EQ(r.a_taps, 1);
    EXPECT_EQ(r.a_frames, Frames(r.cfg.min_press_s));
    EXPECT_EQ(r.rolls + r.chords + r.b_frames, 0);
}

TEST(RollChord, DisabledNeverHoldsAPressBack) {
    for (float press = 0.01f; press <= 0.3f; press += 0.01f) {
        const Edges today = AloneEdges(press, false);
        EXPECT_EQ(today.down, 0) << press;
        EXPECT_EQ(today.up, Frames(press)) << press;
    }
}

// ---- B alone: interact ------------------------------------------------------------------------

TEST(RollChord, MovingBAloneWaitsIts150msGraceAndInteractsOnce) {
    const R::Config cfg{};
    Edges e = AloneEdges(0.3f, true, true, true);
    EXPECT_NEAR(static_cast<float>(e.down) * Dt, cfg.b_window_s, 1.01f * Dt);
    EXPECT_EQ(e.up, Frames(0.3f)); // let go when it is let go
    // A quick B tap waits for grace to expire, then is shown min_press_s.
    e = AloneEdges(0.03f, true, true, true);
    EXPECT_EQ(e.down, static_cast<int>(std::ceil(cfg.b_window_s / Dt)));
    EXPECT_EQ(e.up - e.down, Frames(cfg.min_press_s));
}

// ---- The chord: order and windows -------------------------------------------------------------

TEST(RollChord, AFirstThenBWithin60ms) {
    for (float gap : {0.0f, 0.02f, 0.05f, 0.058f}) {
        Rig r;
        r.Hold(true, false, gap);
        const R::Output o = r.Step(true, true);
        EXPECT_EQ(o.event, R::Event::Chord) << gap;
        EXPECT_TRUE(o.chord) << gap;
        EXPECT_NEAR(o.gap_s, gap, 1.01f * Dt) << gap;
        r.Hold(true, true, 0.3f);
        r.Step(false, false);
        r.Hold(false, false, 0.2f);
        EXPECT_EQ(r.rolls, 1) << gap;
        EXPECT_EQ(r.ticks, 1) << gap;
        EXPECT_EQ(r.Leaks(), 0) << gap; // nothing reached the game
    }
}

TEST(RollChord, MovingBFirstThenAWithin150ms) {
    for (float gap : {0.02f, 0.05f, 0.08f, 0.12f, 0.14f}) {
        Rig r;
        r.y = 1.0f;
        r.Hold(false, true, gap);
        const R::Output o = r.Step(true, true);
        EXPECT_EQ(o.event, R::Event::Roll) << gap;
        EXPECT_NEAR(o.gap_s, gap, 1.01f * Dt) << gap;
        r.Hold(true, true, 0.3f);
        r.Step(false, false);
        r.Hold(false, false, 0.2f);
        EXPECT_EQ(r.rolls, 1) << gap;
        EXPECT_EQ(r.Leaks(), 0) << gap;
    }
}

TEST(RollChord, LateSecondPressIsNoChord) {
    // B 70 ms after A: A reached the game at 60 ms (a dodge or the sprint), so B goes straight.
    {
        Rig r;
        r.Hold(true, false, 0.07f);
        EXPECT_TRUE(r.last.a);
        const R::Output o = r.Step(true, true);
        EXPECT_EQ(o.event, R::Event::None);
        EXPECT_TRUE(o.a);
        EXPECT_TRUE(o.b);
        r.Hold(true, true, 2.0f);
        r.Step(false, false);
        EXPECT_EQ(r.chords + r.rolls + r.stills + r.ticks, 0);
        EXPECT_EQ(r.adjust_frames, 0);
    }
    // A 170 ms after B: B already reached the game, A goes straight.
    {
        Rig r;
        r.y = 1.0f;
        r.Hold(false, true, 0.17f);
        EXPECT_TRUE(r.last.b);
        const R::Output o = r.Step(true, true);
        EXPECT_EQ(o.event, R::Event::None);
        EXPECT_TRUE(o.a);
        r.Hold(true, true, 0.5f);
        r.Step(false, false);
        EXPECT_EQ(r.chords + r.rolls, 0);
    }
}

TEST(RollChord, ReleasesAfterAChordAreSwallowed) {
    // A let go first: the roll fires; B still held never reaches the game; A again while B is
    // held: nothing; after both are up a new A works again.
    Rig r;
    r.Step(true, true);
    r.Hold(true, true, 0.2f);
    const R::Output o = r.Step(false, true);
    EXPECT_EQ(o.event, R::Event::Roll);
    r.Hold(false, true, 0.5f);
    r.Hold(true, true, 0.2f);
    EXPECT_EQ(r.Leaks(), 0);
    EXPECT_EQ(r.rolls, 1);
    EXPECT_EQ(r.chord.State(), R::Phase::Wait);
    r.Step(false, false);
    r.Hold(true, false, 0.1f);
    EXPECT_TRUE(r.last.a);
}

// ---- Moving: the roll at once -----------------------------------------------------------------

TEST(RollChord, MovingRollsTheMomentTheChordIsComplete) {
    const float dirs[4][2] = {{-1.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {0.6f, -0.6f}};
    for (int order = 0; order < 3; ++order) {
        for (const auto& d : dirs) {
            Rig r;
            r.x = d[0];
            r.y = d[1];
            r.Hold(order == 0, order == 1, order == 2 ? 0.0f : 0.03f);
            const int complete = r.frame;
            const R::Output o = r.Step(true, true);
            ASSERT_EQ(o.event, R::Event::Roll) << order;
            EXPECT_EQ(r.roll_frame, complete);
            EXPECT_TRUE(o.chord);
            EXPECT_TRUE(o.moved);
            EXPECT_FALSE(o.centred);
            EXPECT_FALSE(o.adjust);
            const float len = std::hypot(d[0], d[1]);
            EXPECT_NEAR(o.roll_x, d[0] / len, 1e-5f);
            EXPECT_NEAR(o.roll_y, d[1] / len, 1e-5f);
            // Held on for 2 s while moving: nothing more (no adjust, no second roll), then let go.
            r.Hold(true, true, 2.0f);
            r.Step(false, false);
            EXPECT_EQ(r.rolls, 1);
            EXPECT_EQ(r.stills + r.adjust_frames + r.released, 0);
            EXPECT_EQ(r.Leaks(), 0);
        }
    }
}

TEST(RollChord, StickJustLetGoStillCountsAsMoving) {
    Rig r;
    r.x = 1.0f;
    r.Hold(false, false, 0.2f);
    r.x = 0.0f; // the thumb lifts off 50 ms before the chord
    r.Hold(false, false, 0.05f);
    const R::Output o = r.Step(true, true);
    EXPECT_EQ(o.event, R::Event::Roll);
    EXPECT_NEAR(o.roll_x, 1.0f, 1e-5f);
    EXPECT_FALSE(o.centred);
    // 0.2 s ago is standing still.
    Rig s;
    s.x = 1.0f;
    s.Hold(false, false, 0.2f);
    s.x = 0.0f;
    s.Hold(false, false, 0.2f);
    const R::Output so = s.Step(true, true);
    EXPECT_EQ(so.event, R::Event::Chord);
}

// ---- Standing still: tap, push, hold ----------------------------------------------------------

TEST(RollChord, StillTapRollsBackOnTheRelease) {
    Rig r;
    const R::Output first = r.Step(true, true);
    EXPECT_EQ(first.event, R::Event::Chord);
    EXPECT_EQ(r.ticks, 1);
    r.Hold(true, true, 0.15f);
    EXPECT_TRUE(r.last.adjust); // the grip adjust counts meanwhile
    EXPECT_EQ(r.rolls, 0);
    const R::Output o = r.Step(false, false);
    EXPECT_EQ(o.event, R::Event::Roll);
    EXPECT_TRUE(o.centred);
    EXPECT_NEAR(o.roll_x, 0.0f, 1e-6f);
    EXPECT_NEAR(o.roll_y, -1.0f, 1e-6f);
    EXPECT_FALSE(o.adjust);
    EXPECT_EQ(r.chord.State(), R::Phase::Idle);
    EXPECT_EQ(r.Leaks(), 0);
}

TEST(RollChord, StickPushedDuringTheHoldRollsAtOnce) {
    Rig r;
    r.Step(true, true);
    r.Hold(true, true, 0.8f);
    EXPECT_TRUE(r.last.adjust);
    r.x = -1.0f;
    const int push = r.frame;
    const R::Output o = r.Step(true, true);
    EXPECT_EQ(o.event, R::Event::Roll);
    EXPECT_EQ(r.roll_frame, push);
    EXPECT_NEAR(o.roll_x, -1.0f, 1e-5f);
    EXPECT_TRUE(o.moved);
    EXPECT_FALSE(o.adjust); // the hold is cancelled
    r.x = 0.0f;
    r.Hold(true, true, 1.0f);
    r.Step(false, false);
    EXPECT_EQ(r.rolls, 1);
    EXPECT_EQ(r.stills, 0);
    EXPECT_EQ(r.ticks, 1);
    EXPECT_EQ(r.adjust_frames, 1 + Frames(0.8f)); // up to the push only
    EXPECT_EQ(r.Leaks(), 0);
}

TEST(RollChord, HeldStillIsTheGripAdjust) {
    Rig r;
    r.Step(true, true);
    int still_at = -1;
    for (int i = 1; i <= 200; ++i) {
        if (r.Step(true, true).event == R::Event::Still) {
            still_at = i;
        }
    }
    EXPECT_NEAR(static_cast<float>(still_at) * Dt, r.cfg.hold_s, 1.01f * Dt);
    EXPECT_TRUE(r.last.adjust); // on through the adjust; a stick pushed then is ignored
    r.x = 1.0f;
    r.Hold(true, true, 0.3f);
    EXPECT_TRUE(r.last.adjust);
    const R::Output o = r.Step(false, false);
    EXPECT_EQ(o.event, R::Event::Released);
    EXPECT_FALSE(o.adjust);
    EXPECT_EQ(r.rolls, 0);
    EXPECT_EQ(r.stills, 1);
    EXPECT_EQ(r.Leaks(), 0);
}

TEST(RollChord, LetGoJustBeforeTheHoldIsARoll) {
    Rig r;
    r.Step(true, true);
    r.Hold(true, true, r.cfg.hold_s - 2.0f * Dt);
    EXPECT_EQ(r.stills, 0);
    const R::Output o = r.Step(false, false);
    EXPECT_EQ(o.event, R::Event::Roll);
}

// ---- Bypass, roll_chord=0, reset --------------------------------------------------------------

TEST(RollChord, BypassGoesStraightThrough) {
    // The sprint latch (A leaps / dodges) or the modifier (B's gestures): no wait, no chord.
    Rig r;
    r.bypass = true;
    EXPECT_TRUE(r.Step(true, false).a);
    EXPECT_TRUE(r.Step(true, true).b);
    r.Hold(true, true, 0.5f);
    EXPECT_FALSE(r.Step(false, false).a); // let go at once (no minimum on a straight press)
    EXPECT_EQ(r.chords + r.rolls + r.ticks, 0);
    // A tap of one step while bypassed: one step, as today.
    r.Clear();
    EXPECT_TRUE(r.Step(true, false).a);
    EXPECT_FALSE(r.Step(false, false).a);
}

TEST(RollChord, DisabledPassesThroughAndStillFeedsTheAdjust) {
    Rig r;
    r.enabled = false;
    EXPECT_TRUE(r.Step(true, false).a); // at once
    r.Hold(true, false, 0.1f);
    const R::Output o = r.Step(true, true); // B 0.1 s later: within 0.3 s, a chord
    EXPECT_TRUE(o.a);
    EXPECT_TRUE(o.b);
    EXPECT_TRUE(o.adjust);
    EXPECT_FALSE(o.chord); // no tick: roll_chord=0 has no roll
    r.Hold(true, true, 1.3f);
    EXPECT_EQ(r.stills, 1);
    EXPECT_TRUE(r.last.a && r.last.b && r.last.adjust);
    r.Step(false, false);
    EXPECT_EQ(r.rolls, 0);
    EXPECT_EQ(r.released, 1);
    // A short chord: no roll either, A and B went through.
    r.Clear();
    r.Step(true, true);
    r.Hold(true, true, 0.2f);
    r.Step(false, false);
    EXPECT_EQ(r.rolls, 0);
    EXPECT_GT(r.a_frames, 0);
    EXPECT_GT(r.b_frames, 0);
    // A held 0.5 s, then B: never a chord (A held to sprint), as the grip adjust had it.
    r.Clear();
    r.Hold(true, false, 0.5f);
    r.Hold(true, true, 1.5f);
    r.Step(false, false);
    EXPECT_EQ(r.chords + r.stills, 0);
    EXPECT_EQ(r.adjust_frames, 0);
}

TEST(RollChord, ResetForgetsTheChord) {
    Rig r;
    r.Step(true, true);
    r.Hold(true, true, 0.3f);
    r.chord.Reset();
    // The presenter blocks held buttons through a context switch: they come back up.
    r.Step(false, false);
    EXPECT_EQ(r.rolls, 0);
    EXPECT_EQ(r.chord.State(), R::Phase::Idle);
    r.Hold(true, false, 0.1f);
    EXPECT_TRUE(r.last.a);
}

TEST(RollChord, ReleasedBThenARollsImmediatelyWithoutInteract) {
    Rig r;
    r.y = 1.0f;
    r.Hold(false, true, 0.025f);
    r.Hold(false, false, 0.083f);
    EXPECT_EQ(r.Leaks(), 0);
    const auto o = r.Step(true, false);
    EXPECT_EQ(o.event, R::Event::Roll);
    EXPECT_TRUE(o.chord);
    EXPECT_TRUE(o.moved);
    EXPECT_FALSE(o.centred);
    EXPECT_FALSE(o.adjust);
    r.Hold(true, false, 0.3f);
    r.Hold(false, false, 0.3f);
    EXPECT_EQ(r.rolls, 1);
    EXPECT_EQ(r.Leaks(), 0);
}

TEST(RollChord, ReleasedBExpiresAndCannotTurnLaterDodgeIntoRoll) {
    Rig r;
    r.y = 1.0f;
    r.Hold(false, true, 0.025f);
    r.Hold(false, false, 0.25f);
    EXPECT_EQ(r.b_taps, 1);
    r.Hold(true, false, 0.2f);
    EXPECT_TRUE(r.last.a);
    EXPECT_EQ(r.rolls + r.chords + r.ticks, 0);
}

TEST(RollChord, BGraceUsesElapsedTimeAcrossRefreshRates) {
    for (int hz : {30, 60, 72, 90, 120, 144}) {
        const float dt = 1.0f / hz;
        Rig r;
        r.y = 1.0f;
        r.Step(false, true, dt);
        const int frames = static_cast<int>(std::floor(0.14f / dt));
        for (int i = 1; i < frames; ++i) r.Step(false, false, dt);
        auto o = r.Step(true, false, dt);
        EXPECT_EQ(o.event, R::Event::Roll) << hz;
        EXPECT_EQ(r.Leaks(), 0) << hz;
        Rig expired;
        expired.y = 1.0f;
        expired.Step(false, true, dt);
        for (int i = 0; i < static_cast<int>(std::ceil(0.17f / dt)); ++i)
            expired.Step(false, false, dt);
        o = expired.Step(true, false, dt);
        EXPECT_EQ(expired.rolls, 0) << hz;
        EXPECT_EQ(expired.b_taps, 1) << hz;
    }
}

TEST(RollChord, ResetAndBypassCannotReuseReleasedB) {
    for (bool reset : {false, true}) {
        Rig r;
        r.y = 1.0f;
        r.Step(false, true);
        r.Step(false, false);
        if (reset) r.chord.Reset();
        else r.bypass = true;
        r.Step(true, false);
        r.Hold(true, false, 0.2f);
        EXPECT_EQ(r.rolls + r.chords + r.ticks, 0);
    }
}

TEST(RollChord, StationaryBIsImmediateEvenWithRecentDirectionMemory) {
    for (bool recently_moving : {false, true}) {
        Rig r;
        if (recently_moving) {
            r.y = 1.0f;
            r.Step(false, false);
        }
        r.y = 0.0f;
        auto o = r.Step(false, true);
        EXPECT_TRUE(o.b);
        EXPECT_FALSE(r.chord.Pending());
        o = r.Step(false, false);
        EXPECT_FALSE(o.b);
        r.y = 1.0f;
        o = r.Step(true, false);
        EXPECT_EQ(o.event, R::Event::None);
        EXPECT_EQ(r.rolls + r.chords, 0);
        EXPECT_EQ(r.b_frames, 1);
    }
}

TEST(RollChord, StoppingFlushesBGraceWithoutRollingOrRepeatingInteract) {
    for (bool held : {false, true}) {
        Rig r;
        r.y = 1.0f;
        r.Step(false, true);
        r.Step(false, held);
        EXPECT_EQ(r.Leaks(), 0);
        r.y = 0.0f;
        auto o = r.Step(true, held); // A arrives on the same frame movement stops.
        EXPECT_TRUE(o.b);
        EXPECT_EQ(o.event, R::Event::None);
        EXPECT_EQ(o.b_tap, !held);
        r.y = 1.0f;
        r.Hold(true, held, 0.2f);
        r.Hold(false, false, 0.2f);
        EXPECT_EQ(r.rolls + r.chords, 0);
        EXPECT_EQ(r.b_taps, held ? 0 : 1);
    }
}

TEST(RollChord, StationaryBHeldThenMovingCannotBecomeARoll) {
    Rig r;
    auto o = r.Step(false, true);
    EXPECT_TRUE(o.b);
    r.y = 1.0f;
    o = r.Step(true, true);
    EXPECT_TRUE(o.a);
    EXPECT_TRUE(o.b);
    EXPECT_EQ(o.event, R::Event::None);
}
