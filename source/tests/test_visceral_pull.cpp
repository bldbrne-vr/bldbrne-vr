// SPDX-License-Identifier: GPL-2.0-or-later

// The interactive visceral (core/vr/visceral_pull.h): the first throw of a clip plays through and
// teaches its rip time; later ones wait for the pull just before it (slowed, never stopped). The
// hand's pull from the catch on scrubs the throw toward the rip and the yank tears it, with a
// slow-motion beat on the rip; a pull before the catch never counts; the timeout softens it.
// Learned times survive vr.ini's text form; the pull detector takes a yank back, up or sideways
// (head-relative) and not a push further in or a slow drift.

#include <cstdint>

#include <gtest/gtest.h>

#include "core/vr/visceral_pull.h"

namespace {

namespace P = VR::Melee::Pull;

constexpr std::int64_t Ms = P::Ms;
constexpr std::int32_t Clip = 100033000;

struct Throw {
    P::Hold hold;
    P::Config cfg;
    std::int64_t now{1000 * Ms};
    float clip_time{0.0f};
    float progress{0.0f}; // the hand's pull, as the presenter reports it
    bool ripped{false};   // the rip's damage seen

    P::Out Update(float at_s, bool throwing = true, bool pulled = false,
                  const char* abort = nullptr, float dt = 1.0f / 30.0f) {
        now += 33 * Ms;
        P::In in{};
        in.now = now;
        in.throwing = throwing;
        in.clip = Clip;
        in.clip_time = clip_time;
        in.progress = progress;
        in.pulled = pulled;
        in.ripped = ripped;
        in.abort = abort;
        in.dt = dt;
        const P::Out out = hold.Step(in, at_s, cfg);
        clip_time += dt * out.rate; // the clip runs on the scaled frame time
        return out;
    }
};

TEST(VisceralPull, UnknownRipPlaysThrough) {
    Throw t;
    for (int i = 0; i < 90; ++i) {
        const P::Out o = t.Update(0.0f);
        EXPECT_FLOAT_EQ(o.rate, 1.0f);
        EXPECT_FALSE(o.held_now);
    }
    EXPECT_EQ(t.hold.State(), P::Phase::Playing);
    t.Update(0.0f, false);
    EXPECT_EQ(t.hold.State(), P::Phase::Idle);
}

TEST(VisceralPull, WaitsForThePullJustBeforeTheRip) {
    Throw t;
    const float rip = 1.20f;
    bool held = false;
    for (int i = 0; i < 60 && !held; ++i) {
        const P::Out o = t.Update(rip);
        held = o.held_now;
        if (!held) {
            EXPECT_GT(o.rate, 0.0f);
            EXPECT_LE(o.rate, t.cfg.entry_rate);
        }
    }
    ASSERT_TRUE(held);
    EXPECT_EQ(t.hold.Serial(), 1u);
    EXPECT_GE(t.clip_time, rip - t.cfg.lead_s);
    EXPECT_LT(t.clip_time, rip); // the hand is still inside: the rip has not played
    // Held: the clip creeps (never stops, never faster than the hold's speed), still short of
    // the rip after 2 s.
    for (int i = 0; i < 60; ++i) {
        const float rate = t.Update(rip).rate;
        EXPECT_LE(rate, t.cfg.rate);
        EXPECT_GT(rate, 0.0f);
    }
    EXPECT_LT(t.clip_time, rip);
    // The pull: the throw plays on at full speed (the game's own rip).
    const P::Out o = t.Update(rip, true, true);
    EXPECT_TRUE(o.released_now);
    EXPECT_STREQ(o.why, "pulled");
    EXPECT_LE(o.rate, t.cfg.rate); // extraction ramps out of the catch
    EXPECT_FALSE(t.hold.Failed());
    EXPECT_EQ(t.hold.State(), P::Phase::Released);
    for (int i = 0; i < 30; ++i) {
        EXPECT_FALSE(t.Update(rip).held_now); // once per throw
    }
    EXPECT_GT(t.clip_time, rip);
}

// Held without a pull, the clip creeps but never reaches the rip before the timeout (the rip
// is the game's own, accelerated and softened, when the hold times out).
TEST(VisceralPull, TheRipWaitsForThePullUntilTheTimeout) {
    Throw t;
    const float rip = 1.20f;
    while (!t.Update(rip).held_now) {
    }
    P::Out o{};
    while (!(o = t.Update(rip)).released_now) {
        EXPECT_LT(t.clip_time, rip - P::RipMarginS + 1e-3f);
        EXPECT_GT(o.rate, 0.0f); // never a hard stop
        ASSERT_LT(t.now, 10'000 * Ms);
    }
    EXPECT_STREQ(o.why, "too late");
    EXPECT_TRUE(t.hold.Failed());
}

TEST(VisceralPull, LetsGoByItselfAfterTheTimeout) {
    Throw t;
    while (!t.Update(1.0f).held_now) {
    }
    const std::int64_t held = t.now;
    P::Out o{};
    while (!(o = t.Update(1.0f)).released_now) {
        ASSERT_LT(t.now - held, 4000 * Ms);
    }
    EXPECT_STREQ(o.why, "too late");
    EXPECT_TRUE(t.hold.Failed());
    EXPECT_GE(t.now - held, static_cast<std::int64_t>(t.cfg.timeout_s * 1e9f));
}

TEST(VisceralPull, AbortsAndTheThrowsEndLetGoAtOnce) {
    Throw a;
    while (!a.Update(1.0f).held_now) {
    }
    const P::Out o = a.Update(1.0f, true, false, "the hunter was hit");
    EXPECT_TRUE(o.released_now);
    EXPECT_STREQ(o.why, "the hunter was hit");

    Throw b;
    while (!b.Update(1.0f).held_now) {
    }
    const P::Out e = b.Update(1.0f, false);
    EXPECT_TRUE(e.released_now);
    EXPECT_STREQ(e.why, "the throw ended");
    EXPECT_EQ(b.hold.State(), P::Phase::Idle);

    // An abort before the hold point: no hold at all.
    Throw c;
    for (int i = 0; i < 60; ++i) {
        EXPECT_FALSE(c.Update(1.0f, true, false, "a menu").held_now);
    }
}

TEST(VisceralPull, AFixedHoldPointNeedsNoLearning) {
    Throw t;
    t.cfg.at_s = 0.9f;
    bool held = false;
    for (int i = 0; i < 60 && !held; ++i) {
        held = t.Update(0.0f).held_now;
    }
    ASSERT_TRUE(held);
    EXPECT_NEAR(t.clip_time, 0.9f, 0.05f);
}

TEST(VisceralPull, AThrowSeenLatePlaysOn) {
    Throw t;
    t.clip_time = 2.0f; // the hold point (1.1 s) long past
    for (int i = 0; i < 30; ++i) {
        EXPECT_FALSE(t.Update(1.2f).held_now);
    }
}

// The throw that teaches its rip (its first damage, at the rip) never holds after it: the rip
// has played.
TEST(VisceralPull, TheThrowThatLearnsItsRipPlaysOn) {
    Throw t;
    const float rip = 1.20f;
    while (t.clip_time < rip) {
        EXPECT_FALSE(t.Update(0.0f).held_now); // not known yet
    }
    for (int i = 0; i < 30; ++i) {
        const P::Out o = t.Update(rip); // learned now, the rip behind it
        EXPECT_FALSE(o.held_now);
        EXPECT_FLOAT_EQ(o.rate, 1.0f);
    }
}

TEST(VisceralPull, OffNeverHolds) {
    Throw t;
    t.cfg.enabled = false;
    for (int i = 0; i < 90; ++i) {
        EXPECT_FLOAT_EQ(t.Update(1.0f).rate, 1.0f);
    }
}

// The thrust's recoil (any yank before the catch) never counts, not even when the game sees it
// after the catch: only a pull from the catch on tears (by its own time).
TEST(VisceralPull, APullBeforeTheCatchNeverTears) {
    P::Hold h;
    P::Config c;
    P::In in{};
    in.throwing = true;
    in.clip = Clip;
    in.clip_time = 0.9f;
    in.now = 1000 * Ms;
    in.pulled = true; // a yank during the entry
    in.pulled_ns = in.now;
    EXPECT_FALSE(h.Step(in, 1.2f, c).released_now);
    in.clip_time = 1.1f;
    in.now += 100 * Ms;
    EXPECT_TRUE(h.Step(in, 1.2f, c).held_now); // still caught
    in.now += 200 * Ms;
    EXPECT_FALSE(h.Step(in, 1.2f, c).released_now); // that old yank does not tear it
    EXPECT_EQ(h.State(), P::Phase::Held);
    EXPECT_FALSE(h.Failed());
    in.pulled_ns = in.now; // a yank from the catch on
    in.now += 33 * Ms;
    EXPECT_STREQ(h.Step(in, 1.2f, c).why, "pulled");
    EXPECT_FALSE(h.Failed());
}

TEST(VisceralPull, MissingTheWindowMarksTheAutomaticRipMistimed) {
    Throw t;
    while (!t.Update(1.2f).held_now) {
    }
    P::Out o{};
    while (!(o = t.Update(1.2f)).released_now) {
        ASSERT_LT(t.now, 7000 * Ms);
    }
    EXPECT_STREQ(o.why, "too late");
    EXPECT_TRUE(t.hold.Failed());
    // ... and it does not carry into the next throw.
    const auto serial = t.hold.Serial();
    EXPECT_FLOAT_EQ(t.Update(1.2f, false).rate, 1.0f);
    t.clip_time = 0.0f;
    t.Update(1.2f);
    EXPECT_GT(t.hold.Serial(), serial);
    EXPECT_FALSE(t.hold.Failed());
}

// A yank already under way at the catch tears as soon as the catch has lasted catch_s; then the
// extraction eases up to rip_rate.
TEST(VisceralPull, AYankAtTheCatchTearsAfterTheCatchTime) {
    Throw t;
    while (!t.Update(1.2f).held_now) {
        ASSERT_LT(t.now, 3000 * Ms);
    }
    const auto caught = t.now;
    t.progress = 1.0f;
    P::Out o{};
    while (!(o = t.Update(1.2f, true, true)).released_now) {
        ASSERT_LT(t.now - caught, 200 * Ms);
    }
    EXPECT_GE(t.now - caught, 100 * Ms);
    EXPECT_STREQ(o.why, "pulled");
    EXPECT_FALSE(t.hold.Failed());
    for (int i = 0; i < 4; ++i) {
        o = t.Update(1.2f);
    }
    EXPECT_NEAR(o.rate, 1.4f, 0.001f);
    EXPECT_FLOAT_EQ(t.Update(1.2f, false).rate, 1.0f);
}

TEST(VisceralPull, LearningDuringThisThrowDoesNotArmAHold) {
    Throw t;
    t.Update(0.0f);
    // Even a newly learned time ahead of the cached update is for the next throw only.
    for (int i = 0; i < 60; ++i) {
        EXPECT_FALSE(t.Update(1.2f).held_now);
    }
}

TEST(VisceralPull, TwelveCentimetresOfWithdrawalIsEnough) {
    P::PullDetector det;
    const P::PullConfig cfg{};
    det.Arm({0, 0, -0.3f}, {0, 0, -1});
    // The thrust continues after detection: measure the pull from its deepest point.
    EXPECT_FALSE(det.Step({0, 0, -0.5f}, {0, 0, -1.5f}, cfg));
    // A hand's natural recoil (7.5 cm) is no pull; a real yank (12.5 cm) is.
    EXPECT_FALSE(det.Step({0, 0, -0.425f}, {0, 0, 1.0f}, cfg));
    EXPECT_TRUE(det.Step({0, 0, -0.375f}, {0, 0, 1.0f}, cfg));
}

TEST(VisceralPull, LateGraceAcceptsTheActualPullEvenWhenTheUpdateIsLate) {
    for (const int pull_ms : {3100, 3300}) {
        P::Hold h;
        P::Config c;
        P::In in{};
        in.throwing = true;
        in.clip = Clip;
        in.clip_time = 1.1f;
        in.now = 1000 * Ms;
        ASSERT_TRUE(h.Step(in, 1.2f, c).held_now);
        in.now += 3350 * Ms;
        in.pulled = true;
        in.pulled_ns = (1000 + pull_ms) * Ms;
        const auto out = h.Step(in, 1.2f, c);
        EXPECT_STREQ(out.why, pull_ms < 3200 ? "pulled" : "too late");
        EXPECT_EQ(h.Failed(), pull_ms > 3200);
    }
}

TEST(VisceralPull, ActualRipPreventsACatchAndAbortRestoresNormalSpeed) {
    P::Hold h;
    P::Config c;
    P::In in{};
    in.throwing = true;
    in.clip = Clip;
    in.now = 1000 * Ms;
    h.Step(in, 0.0f, c); // learning throw
    in.clip_time = 1.1f;
    in.ripped = true;
    const P::Out rip = h.Step(in, 1.2f, c);
    EXPECT_STREQ(rip.why, "the rip");
    EXPECT_TRUE(rip.impact_now); // the learning throw's rip has its beat too
    EXPECT_FLOAT_EQ(rip.rate, P::ImpactRate);
    in.now += 250 * Ms; // past the beat and the ease out of it
    EXPECT_FLOAT_EQ(h.Step(in, 1.2f, c).rate, c.rip_rate);
    in.abort = "menu";
    EXPECT_FLOAT_EQ(h.Step(in, 1.2f, c).rate, 1.0f);
}

TEST(VisceralPull, EntryCannotStepPastTheCatchOnALongFrame) {
    Throw t;
    const auto out = t.Update(1.2f, true, false, nullptr, 2.0f);
    EXPECT_LE(t.clip_time, 1.1f);
    EXPECT_LE(out.rate, t.cfg.entry_rate);
    EXPECT_TRUE(t.Update(1.2f).held_now);
}

// The hand drives the available stretch toward the rip, reserving a little clip time for
// creep until the deadline. The whole pull without a yank waits short of the damage event;
// the yank tears it.
TEST(VisceralPull, TheHandScrubsTheThrowTowardTheRip) {
    Throw t;
    const float rip = 1.20f;
    while (!t.Update(rip).held_now) {
        ASSERT_LT(t.now, 3000 * Ms);
    }
    const float caught = rip - t.cfg.lead_s;
    // The last few clip milliseconds are reserved for creep until the deadline.
    const float end = rip - P::RipMarginS - P::MinRate * (t.cfg.timeout_s + P::LateGraceS);
    t.progress = 0.5f;
    t.Update(rip);
    EXPECT_NEAR(t.clip_time, caught + 0.5f * (end - caught), 0.002f);
    t.progress = 1.0f;
    for (int i = 0; i < 10; ++i) {
        EXPECT_FALSE(t.Update(rip).released_now);
    }
    EXPECT_NEAR(t.clip_time, end, 0.002f);
    EXPECT_EQ(t.hold.State(), P::Phase::Held);
    const P::Out o = t.Update(rip, true, true);
    EXPECT_TRUE(o.released_now);
    EXPECT_STREQ(o.why, "pulled");
    EXPECT_FALSE(t.hold.Failed());
}

// Pushing the hand back in never rewinds the throw (it creeps on as it waits); a yank faster than
// the game's update is capped, and never carries it past the margin.
TEST(VisceralPull, AHandGoingBackInNeverRewindsIt) {
    Throw t;
    const float rip = 1.20f;
    while (!t.Update(rip).held_now) {
    }
    t.progress = 0.8f;
    t.Update(rip);
    const float reached = t.clip_time;
    t.progress = 0.3f;
    for (int i = 0; i < 10; ++i) {
        const float rate = t.Update(rip).rate;
        EXPECT_GT(rate, 0.0f);
        EXPECT_LE(rate, t.cfg.rate);
    }
    EXPECT_GE(t.clip_time, reached);
    EXPECT_LT(t.clip_time, reached + 0.01f);

    Throw y;
    while (!y.Update(rip).held_now) {
    }
    y.progress = 1.0f;
    EXPECT_FLOAT_EQ(y.Update(rip, true, false, nullptr, 0.01f).rate, P::MaxTearRate);
    for (int i = 0; i < 5; ++i) {
        y.Update(rip);
        EXPECT_LT(y.clip_time, rip - P::RipMarginS + 0.002f);
    }
}

// The rip's damage (seen by the update after it): the hunter and his target all but still for
// impact_s, then easing into the extraction; once per throw.
TEST(VisceralPull, TheRipLandsInSlowMotion) {
    Throw t;
    const float rip = 1.20f;
    while (!t.Update(rip).held_now) {
    }
    t.progress = 1.0f;
    while (!t.Update(rip, true, true).released_now) {
        ASSERT_LT(t.now, 5000 * Ms);
    }
    while (t.clip_time < rip) {
        ASSERT_FALSE(t.Update(rip).impact_now); // no beat before the damage
    }
    t.ripped = true;
    const P::Out first = t.Update(rip);
    EXPECT_TRUE(first.impact_now);
    EXPECT_FLOAT_EQ(first.rate, P::ImpactRate);
    P::Out o{};
    for (int i = 0; i < 4; ++i) { // +33 .. +132 ms
        o = t.Update(rip);
        EXPECT_FALSE(o.impact_now);
        EXPECT_FLOAT_EQ(o.rate, P::ImpactRate);
    }
    o = t.Update(rip); // +165 ms: easing out of it
    EXPECT_GT(o.rate, P::ImpactRate);
    EXPECT_LT(o.rate, t.cfg.rip_rate);
    for (int i = 0; i < 2; ++i) { // +231 ms
        o = t.Update(rip);
    }
    EXPECT_FLOAT_EQ(o.rate, t.cfg.rip_rate);
}

TEST(VisceralPull, NoBeatAfterAnAbortOrWithItOff) {
    // Let go by an abort (the hunter hit, a menu): the game's own speed, no beat after it.
    Throw a;
    while (!a.Update(1.2f).held_now) {
    }
    a.Update(1.2f, true, false, "the hunter was hit");
    a.ripped = true;
    for (int i = 0; i < 5; ++i) {
        const P::Out o = a.Update(1.2f);
        EXPECT_FALSE(o.impact_now);
        EXPECT_FLOAT_EQ(o.rate, 1.0f);
    }
    // impact_s 0: straight into the extraction.
    Throw b;
    b.cfg.impact_s = 0.0f;
    while (!b.Update(1.2f).held_now) {
    }
    b.progress = 1.0f;
    while (!b.Update(1.2f, true, true).released_now) {
        ASSERT_LT(b.now, 5000 * Ms);
    }
    b.ripped = true;
    P::Out o = b.Update(1.2f);
    EXPECT_FALSE(o.impact_now);
    for (int i = 0; i < 3; ++i) {
        o = b.Update(1.2f);
    }
    EXPECT_FLOAT_EQ(o.rate, b.cfg.rip_rate);
}

TEST(VisceralPull, TheHeartFailsAsYouTearItOut) {
    const P::Beat still = P::HeartbeatAt(1.0f, 0.0f);
    const P::Beat half = P::HeartbeatAt(1.0f, 0.5f);
    const P::Beat out = P::HeartbeatAt(1.0f, 1.0f);
    EXPECT_FLOAT_EQ(still.interval_s, P::HeartbeatInterval(1.0f)); // held still: it races
    EXPECT_GT(half.interval_s, still.interval_s);
    EXPECT_GT(out.interval_s, half.interval_s);
    EXPECT_LT(half.strength, still.strength);
    EXPECT_LT(out.strength, 0.25f); // all but stopped
    EXPECT_GT(P::HeartbeatAt(1.5f, 0.0f).strength, P::HeartbeatAt(0.0f, 0.0f).strength);
    // The tension: nothing until the hand moves, then rising to the full pull.
    EXPECT_FLOAT_EQ(P::TearTension(0.0f), 0.0f);
    EXPECT_GE(P::TearTension(0.05f), 0.12f);
    EXPECT_GT(P::TearTension(0.5f), P::TearTension(0.05f));
    EXPECT_GT(P::TearTension(1.0f), P::TearTension(0.5f));
    EXPECT_NEAR(P::TearTension(1.0f), 0.55f, 1e-4f);
    EXPECT_FLOAT_EQ(P::TearTension(2.0f), P::TearTension(1.0f));
    // The blade lodged from the catch: felt (a Touch motor shows 0.12 and up), and the hand's
    // level (the lodged one, then the tear's) never dips as the pull begins.
    EXPECT_GE(P::LodgedTension, 0.12f);
    float level = 0.0f;
    for (int i = 0; i <= 100; ++i) {
        const float now = std::max(P::LodgedTension, P::TearTension(i / 100.0f));
        EXPECT_GE(now, level);
        level = now;
    }
}

TEST(VisceralPull, HeartbeatAcceleratesAndVisualRecoveryWaitsForTheHandToClear) {
    EXPECT_GT(P::HeartbeatInterval(0.0f), P::HeartbeatInterval(0.75f));
    EXPECT_GT(P::HeartbeatInterval(0.75f), P::HeartbeatInterval(1.5f));
    EXPECT_NEAR(P::HeartbeatInterval(10.0f), 0.24f, 0.001f);
    const P::Config c{};
    EXPECT_FALSE(P::RecoveryReady(false, Clip, Clip, 1.6f, 1.2f, c));
    EXPECT_FALSE(P::RecoveryReady(true, Clip + 1, Clip, 1.6f, 1.2f, c));
    EXPECT_FALSE(P::RecoveryReady(true, Clip, Clip, 1.4f, 1.2f, c));
    EXPECT_TRUE(P::RecoveryReady(true, Clip, Clip, 1.5f, 1.2f, c));
}

TEST(VisceralPull, LearnedTimesRoundTrip) {
    P::Learned l;
    EXPECT_TRUE(l.Note(100033000, 1.12f));
    EXPECT_FALSE(l.Note(100033000, 1.5f)); // the first one stays
    EXPECT_TRUE(l.Note(100033100, 0.953f));
    EXPECT_FALSE(l.Note(-1, 1.0f));
    EXPECT_FALSE(l.Note(100033200, 0.0f));
    EXPECT_FLOAT_EQ(l.At(100033000), 1.12f);
    EXPECT_FLOAT_EQ(l.At(100033200), 0.0f);
    P::Learned m;
    m.Parse(l.Text() + ",junk,5:x,100033200:0.70");
    EXPECT_EQ(m.Count(), 3u);
    EXPECT_NEAR(m.At(100033000), 1.12f, 1e-3f);
    EXPECT_NEAR(m.At(100033100), 0.953f, 1e-3f);
    EXPECT_NEAR(m.At(100033200), 0.70f, 1e-3f);
    // Full: the oldest goes.
    P::Learned f;
    for (int i = 0; i < 20; ++i) {
        f.Note(1000 + i, 1.0f);
    }
    EXPECT_EQ(f.Count(), P::Learned::Max);
    EXPECT_FLOAT_EQ(f.At(1000), 0.0f);
    EXPECT_FLOAT_EQ(f.At(1019), 1.0f);
}

TEST(VisceralFreehand, TheThrustsRecoilIsNoPullTheCatchHoldsTheWorld) {
    // You stab and your hand comes back at once: the enemy's entry still plays, and the throw
    // waits just before the rip with the world held still until a pull from the catch on.
    Throw t;
    t.cfg.freehand = true;
    t.Update(1.2f);
    t.progress = 1.0f;
    EXPECT_FLOAT_EQ(t.Update(1.2f, true, true).world, 1.0f); // the recoil, before the catch
    t.progress = 0.0f;
    P::Out out{};
    while (!(out = t.Update(1.2f)).held_now) {
        EXPECT_FLOAT_EQ(out.world, 1.0f); // the world goes on during the entry
        ASSERT_LT(t.now, 3000 * Ms);
    }
    EXPECT_GE(t.clip_time, 1.2f - t.cfg.lead_s); // the enemy took the stab: just before the rip
    EXPECT_LT(t.clip_time, 1.2f);
    EXPECT_FLOAT_EQ(out.world, P::MinRate);
    out = t.Update(1.2f);
    EXPECT_EQ(t.hold.State(), P::Phase::Held);
    EXPECT_FLOAT_EQ(out.world, P::MinRate);
    // The pull: at once (no catch dwell), full damage, the world back to its speed.
    out = t.Update(1.2f, true, true);
    EXPECT_TRUE(out.released_now);
    EXPECT_STREQ(out.why, "pulled");
    EXPECT_FLOAT_EQ(out.world, 1.0f);
    EXPECT_FALSE(t.hold.Failed());
}

TEST(VisceralFreehand, PullWheneverYouLikeTheTimeoutIsASafety) {
    Throw t;
    t.cfg.freehand = true;
    t.cfg.timeout_s = 3.0f; // the legacy deadline does not apply
    while (!t.Update(1.2f).held_now) {
        ASSERT_LT(t.now, 3000 * Ms);
    }
    const std::int64_t held = t.now;
    while (t.now - held < 10'000 * Ms) {
        const P::Out o = t.Update(1.2f);
        ASSERT_EQ(t.hold.State(), P::Phase::Held);
        EXPECT_FLOAT_EQ(o.world, P::MinRate);
        EXPECT_LE(o.rate, t.cfg.rate);
    }
    EXPECT_LT(t.clip_time, 1.2f - P::RipMarginS + 1e-4f); // 10 s on: still short of the rip
    EXPECT_STREQ(t.Update(1.2f, true, true).why, "pulled");
    EXPECT_FALSE(t.hold.Failed());

    Throw timeout;
    timeout.cfg.freehand = true;
    while (!timeout.Update(1.2f).held_now) {
        ASSERT_LT(timeout.now, 3000 * Ms);
    }
    P::Out out{};
    while (!(out = timeout.Update(1.2f)).released_now) {
        ASSERT_LT(timeout.now, 30'000 * Ms);
    }
    EXPECT_STREQ(out.why, "timeout");
    EXPECT_GE(timeout.now - 1000 * Ms, static_cast<std::int64_t>(P::FreehandTimeoutS * 1e9));
    EXPECT_FALSE(timeout.hold.Failed());
    EXPECT_FLOAT_EQ(out.world, 1.0f);
}

TEST(VisceralPull, TheLegacyCatchNeverHoldsTheWorld) {
    Throw t;
    P::Out out{};
    while (!(out = t.Update(1.2f)).held_now) {
        ASSERT_LT(t.now, 3000 * Ms);
    }
    EXPECT_FLOAT_EQ(out.world, 1.0f);
    EXPECT_FLOAT_EQ(t.Update(1.2f).world, 1.0f);
}

TEST(VisceralFreehand, AbortAndUnknownAnimationRemainSafe) {
    Throw t;
    t.cfg.freehand = true;
    for (int i = 0; i < 50; ++i) {
        EXPECT_FLOAT_EQ(t.Update(0.0f).rate, 1.0f);
        EXPECT_EQ(t.hold.State(), P::Phase::Playing);
    }
    t.Update(0.0f, false);
    t.clip_time = 0;
    while (!t.Update(1.2f).held_now) { ASSERT_LT(t.now, 4000 * Ms); }
    const P::Out menu = t.Update(1.2f, true, true, "menu");
    EXPECT_FLOAT_EQ(menu.rate, 1.0f);
    EXPECT_FLOAT_EQ(menu.world, 1.0f); // a menu lets the world go at once
    t.ripped = true;
    EXPECT_FALSE(t.Update(1.2f).impact_now);
    EXPECT_FALSE(t.hold.Failed());
}

TEST(VisceralFreehand, SlowDeliberateWithdrawalCompletesWithoutAYank) {
    P::PullDetector detector;
    const P::PullConfig cfg{12.0f, 0.0f, 0.35f};
    detector.Arm({0, 0, -0.5f}, {0, 0, -1});
    EXPECT_FALSE(detector.Step({0, 0, -0.55f}, {0, 0, -0.2f}, cfg));
    EXPECT_FALSE(detector.Step({0, 0, -0.50f}, {0, 0, 0.2f}, cfg));
    EXPECT_TRUE(detector.Step({0, 0, -0.42f}, {0, 0, 0.2f}, cfg));
}

TEST(VisceralFreehand, LeaningAndBodyTranslationCannotPullAStationaryRelativeHand) {
    const P::V3 hand{0.2f, -0.25f, -0.45f};
    const P::V3 forward{0, 0, -1};
    const P::PullConfig cfg{12, 0, .35f};
    for (P::V3 motion : {P::V3{0, 0, -.13f}, P::V3{.13f, 0, 0}, P::V3{0, .13f, 0}}) {
        P::PullDetector lean, body;
        lean.Arm(hand, forward, hand);
        body.Arm(hand, forward, hand);
        for (int i = 1; i <= 60; ++i) {
            const P::V3 delta{motion.x * i / 60, motion.y * i / 60, motion.z * i / 60};
            EXPECT_FALSE(lean.Step(P::Sub(hand, delta), {}, cfg, hand));
            EXPECT_FLOAT_EQ(lean.Progress(cfg), 0);
            EXPECT_FALSE(body.Step(hand, {}, cfg, P::Sub(hand, delta)));
            EXPECT_FLOAT_EQ(body.Progress(cfg), 0);
        }
    }
}

TEST(VisceralFreehand, ALeanCannotTurnASmallHandMovementIntoAFullPull) {
    P::PullDetector detector;
    const P::PullConfig cfg{12, 0, .35f};
    detector.Arm({0, 0, -.45f}, {0, 0, -1}, {0, 0, -.45f});
    // Hand withdraws 2 cm while the head leans in 13 cm: only the 2 cm counts.
    EXPECT_FALSE(detector.Step({0, 0, -.30f}, {}, cfg, {0, 0, -.43f}));
    EXPECT_NEAR(detector.Travel(), .02f, 1e-6f);
    EXPECT_NEAR(detector.Progress(cfg), 1.f / 6, 1e-5f);
    EXPECT_TRUE(detector.Step({0, 0, -.19f}, {}, cfg, {0, 0, -.32f}));
}

TEST(VisceralFreehand, SlowTrackedPullsWorkInAllOutwardDirectionsWithBodyMovement) {
    const P::V3 start{0.2f, -.25f, -.45f};
    const P::PullConfig cfg{12, 0, .35f};
    for (P::V3 withdrawal : {P::V3{0, 0, .13f}, P::V3{.13f, 0, 0}, P::V3{0, .13f, 0}}) {
        for (float body_share : {0.f, .5f}) {
            P::PullDetector detector;
            detector.Arm(start, {0, 0, -1}, start);
            bool pulled = false;
            for (int i = 1; i <= 180; ++i) {
                const float t = i / 180.f;
                const P::V3 relative{start.x + withdrawal.x*t, start.y + withdrawal.y*t,
                                     start.z + withdrawal.z*t};
                const P::V3 tracked{relative.x + body_share*withdrawal.x*t,
                                    relative.y + body_share*withdrawal.y*t,
                                    relative.z + body_share*withdrawal.z*t};
                pulled |= detector.Step(relative, {}, cfg, tracked);
            }
            EXPECT_TRUE(pulled);
        }
    }
}

TEST(VisceralPull, PartialAndUnacknowledgedPullsReserveTimeUntilRelease) {
    for (bool freehand : {false, true}) {
        for (float hz : {10.f, 30.f, 90.f}) {
            for (float progress : {0.f, .5f, 11.9f/12.f, 1.f}) {
                for (float lead : {.04f, .10f}) {
                    SCOPED_TRACE(::testing::Message() << freehand << ", " << hz << ", "
                                                      << progress << ", " << lead);
                    P::Hold hold;
                    P::Config cfg;
                    cfg.freehand = freehand;
                    cfg.lead_s = lead;
                    P::In in;
                    in.now = 1'000'000'000;
                    in.throwing = true;
                    in.clip = Clip;
                    in.dt = 1.f/hz;
                    const float rip = 1.2f;
                    const float deadline = (freehand ? P::FreehandTimeoutS : cfg.timeout_s) +
                                           P::LateGraceS;
                    bool caught = false, released = false;
                    for (int i = 0; i < static_cast<int>((deadline + 3)*hz); ++i) {
                        in.progress = caught ? progress : 0;
                        const auto out = hold.Step(in, rip, cfg);
                        caught |= out.held_now;
                        if (out.released_now) {
                            ASSERT_TRUE(caught);
                            EXPECT_GE((in.now-hold.HeldNs())*1e-9, deadline - 1e-5);
                            EXPECT_EQ(hold.Failed(), !freehand);
                            EXPECT_FLOAT_EQ(out.world, 1);
                            released = true;
                            break;
                        }
                        in.clip_time += in.dt*out.rate;
                        if (caught) {
                            ASSERT_EQ(hold.State(), P::Phase::Held);
                            ASSERT_GT(out.rate, 0); // keep the game's timestep positive
                            ASSERT_LE(in.clip_time, rip-P::RipMarginS+1e-6f);
                        }
                        in.now += static_cast<int64_t>(1e9/hz);
                    }
                    ASSERT_TRUE(released);
                }
            }
        }
    }
}

TEST(VisceralFreehand, LongUpdateDuringPartialPullStaysBeforeDamageThenYankReleases) {
    Throw t;
    t.cfg.freehand = true;
    while (!t.Update(1.2f).held_now) { ASSERT_LT(t.now, 3000*Ms); }
    t.progress = 11.9f/12.f;
    t.Update(1.2f, true, false, nullptr, 2.f);
    EXPECT_LT(t.clip_time, 1.2f-P::RipMarginS);
    const auto release = t.Update(1.2f, true, true);
    EXPECT_TRUE(release.released_now);
    EXPECT_STREQ(release.why, "pulled");
    EXPECT_FLOAT_EQ(release.world, 1);
    EXPECT_FALSE(t.hold.Failed());
}

// A minimum-jerk move of the head-relative hand from `a` by `d` over `secs` at 90 Hz.
bool Move(P::PullDetector& det, P::V3 a, P::V3 d, float secs, const P::PullConfig& c) {
    const int n = static_cast<int>(secs * 90.0f);
    bool pulled = false;
    for (int i = 1; i <= n; ++i) {
        const float s = static_cast<float>(i) / n;
        const float k = 10 * s * s * s - 15 * s * s * s * s + 6 * s * s * s * s * s;
        const float dk = (30 * s * s - 60 * s * s * s + 30 * s * s * s * s) / secs;
        const P::V3 p{a.x + d.x * k, a.y + d.y * k, a.z + d.z * k};
        const P::V3 v{d.x * dk, d.y * dk, d.z * dk};
        pulled = det.Step(p, v, c) || pulled;
    }
    return pulled;
}

TEST(VisceralPull, AYankOutIsAPull) {
    const P::PullConfig c{};
    const P::V3 hand{0.15f, -0.25f, -0.45f}; // in front of the chest, relative to the head
    const P::V3 fwd{0.0f, 0.0f, -1.0f};
    for (const P::V3 d : {P::V3{0.0f, 0.0f, 0.20f},     // back toward you
                          P::V3{0.0f, 0.22f, 0.05f},    // up and out
                          P::V3{0.20f, 0.0f, 0.04f}}) { // aside
        P::PullDetector det;
        det.Arm(hand, fwd);
        EXPECT_TRUE(Move(det, hand, d, 0.25f, c)) << d.x << " " << d.y << " " << d.z;
    }
}

TEST(VisceralPull, PushingInOrDriftingIsNot) {
    const P::PullConfig c{};
    const P::V3 hand{0.15f, -0.25f, -0.45f};
    const P::V3 fwd{0.0f, 0.0f, -1.0f};
    P::PullDetector in;
    in.Arm(hand, fwd);
    EXPECT_FALSE(Move(in, hand, {0.0f, 0.0f, -0.20f}, 0.25f, c)); // further in
    P::PullDetector slow;
    slow.Arm(hand, fwd);
    EXPECT_FALSE(Move(slow, hand, {0.0f, 0.0f, 0.15f}, 1.2f, c)); // a slow drift back
    P::PullDetector small;
    small.Arm(hand, fwd);
    EXPECT_FALSE(Move(small, hand, {0.0f, 0.0f, 0.04f}, 0.08f, c)); // a twitch
    P::PullDetector off;
    EXPECT_FALSE(Move(off, hand, {0.0f, 0.0f, 0.20f}, 0.25f, c)); // never armed
}

// The progress the throw follows: the hand out of its deepest point (measured afresh when it goes
// in deeper), full at min_cm whatever the speed; the yank still completes it.
TEST(VisceralPull, TheTearFollowsTheHandOut) {
    const P::PullConfig c{};
    P::PullDetector det;
    det.Arm({0.0f, 0.0f, -0.40f}, {0.0f, 0.0f, -1.0f});
    EXPECT_FALSE(det.Step({0.0f, 0.0f, -0.34f}, {0.0f, 0.0f, 0.3f}, c)); // 6 cm back, slowly
    EXPECT_NEAR(det.Progress(c), 0.5f, 1e-3f);
    EXPECT_FALSE(det.Step({0.0f, 0.0f, -0.45f}, {0.0f, 0.0f, -1.0f}, c)); // in, deeper than before
    EXPECT_FLOAT_EQ(det.Progress(c), 0.0f);
    EXPECT_FALSE(det.Step({0.13f, 0.0f, -0.45f}, {0.4f, 0.0f, 0.0f}, c)); // 13 cm aside, slowly
    EXPECT_FLOAT_EQ(det.Progress(c), 1.0f);
    EXPECT_TRUE(det.Step({0.16f, 0.0f, -0.44f}, {1.2f, 0.0f, 0.3f}, c)); // the yank
}

} // namespace
