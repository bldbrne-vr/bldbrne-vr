// SPDX-License-Identifier: GPL-2.0-or-later

// The injection's hands freeze (src/core/vr/inject_hold.h): the hunter's hands stay where they
// were at the stab for vial_inject_hold_frames of the game's own frames (FreezeFrames counts the
// body's pose copies), then ease back to the controllers with no snap (InjectHold). And X's rule
// (VialButton): a press arms or puts back the vial, holding X never does anything more (the
// game's own animated vial is never used from gameplay).

#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include "core/vr/inject_hold.h"

namespace {

using namespace VR::Body;
using Phase = InjectHold::Phase;

constexpr std::int64_t Ms = 1'000'000;
constexpr std::int64_t Frame = 8'333'333; // 120 Hz

TrackedHand Hand(Vec3 p, Quat q = {}) {
    TrackedHand h{};
    h.position_valid = h.position_tracked = true;
    h.orientation_valid = h.orientation_tracked = true;
    h.velocity_valid = h.angular_valid = true;
    h.p = p;
    h.q = q;
    h.v = {1.0f, 0.0f, 0.0f};
    h.w = {0.0f, 2.0f, 0.0f};
    return h;
}

float Dist(Vec3 a, Vec3 b) {
    return Length(a - b);
}

TEST(InjectHold, StartNeedsATrackedGripAndAHoldTime) {
    InjectHold hold;
    TrackedHand lost = Hand({0.1f, 1.0f, -0.3f});
    lost.position_tracked = false;
    EXPECT_FALSE(hold.Start(lost, 1, 0, 50 * Ms, 100 * Ms));
    EXPECT_FALSE(hold.On());
    EXPECT_FALSE(hold.Start(Hand({0.1f, 1.0f, -0.3f}), 1, 0, 0, 100 * Ms));
    EXPECT_FALSE(hold.On());
    TrackedHand live = Hand({0.2f, 1.1f, -0.4f});
    EXPECT_TRUE(hold.Apply(live, 1, 10 * Ms) == Phase::Off); // nothing held: untouched
    EXPECT_NEAR(live.p.x, 0.2f, 1e-6f);
    EXPECT_NEAR(live.v.x, 1.0f, 1e-6f);
    EXPECT_TRUE(hold.Start(Hand({0.1f, 1.0f, -0.3f}), 1, 0, 50 * Ms, 100 * Ms));
    EXPECT_TRUE(hold.On());
}

TEST(InjectHold, HeldStillThenEasedBackWithNoSnap) {
    // The hand stabs in, the game takes the injection with the grip at `at`, and the hand pulls
    // straight back out at 1.5 m/s while the hunter's hand should stay there for 50 ms.
    const Vec3 at{0.05f, 1.05f, -0.35f};
    const Quat q0 = AxisAngle({0.0f, 1.0f, 0.0f}, 0.4f);
    InjectHold hold;
    const std::int64_t t0 = 1000 * Ms;
    ASSERT_TRUE(hold.Start(Hand(at, q0), 7, t0, 50 * Ms, 100 * Ms));

    Vec3 last = at;
    float max_step = 0.0f;
    int held_frames = 0;
    int release_frames = 0;
    bool ended = false;
    for (int f = 1; f <= 40; ++f) {
        const std::int64_t now = t0 + f * Frame;
        const float s = static_cast<float>(now - t0) * 1e-9f;
        const Vec3 live_p = at + Vec3{-1.5f * s, 0.2f * s, 0.6f * s};
        const Quat live_q = AxisAngle({0.0f, 1.0f, 0.0f}, 0.4f + 3.0f * s);
        TrackedHand live = Hand(live_p, live_q);
        const Phase phase = hold.Apply(live, 7, now);
        if (now - t0 < 50 * Ms) {
            EXPECT_TRUE(phase == Phase::Held);
            ++held_frames;
            EXPECT_LT(Dist(live.p, at), 1e-6f); // exactly where the injection went in
            EXPECT_LT(AngleBetween(live.q, q0), 1e-3f);
            EXPECT_NEAR(Length(live.v), 0.0f, 1e-6f); // at rest
            EXPECT_NEAR(Length(live.w), 0.0f, 1e-6f);
        } else if (now - t0 < 150 * Ms) {
            EXPECT_TRUE(phase == Phase::Release);
            ++release_frames;
            // Between the held pose and the controller, nearer the controller as time goes on.
            const float to_held = Dist(live.p, at);
            const float to_live = Dist(live.p, live_p);
            EXPECT_LE(to_held + to_live, Dist(at, live_p) + 1e-4f);
        } else {
            EXPECT_TRUE(phase == Phase::Off);
            EXPECT_LT(Dist(live.p, live_p), 1e-6f); // the controller again, untouched
            EXPECT_LT(AngleBetween(live.q, live_q), 1e-4f);
            EXPECT_NEAR(live.v.x, 1.0f, 1e-6f);
            ended = true;
        }
        max_step = std::max(max_step, Dist(live.p, last));
        last = live.p;
    }
    EXPECT_TRUE(ended);
    EXPECT_FALSE(hold.On());
    EXPECT_EQ(held_frames, 6);     // 50 ms at 120 Hz (frame 6 is at 49.99998 ms)
    EXPECT_EQ(release_frames, 12); // 100 ms
    // No snap: the hunter's hand never moves more than 3.5 cm in one 120 Hz frame (about
    // 3.7 m/s at the steepest point of the ease, catching up the 16 cm the controller, at
    // 1.6 m/s, got ahead).
    EXPECT_LE(max_step, 0.035f);
}

TEST(InjectHold, ContinuousAtBothEnds) {
    InjectHold hold;
    const Vec3 at{0.0f, 1.0f, -0.3f};
    ASSERT_TRUE(hold.Start(Hand(at), 1, 0, 50 * Ms, 100 * Ms));
    // The weight: 1 to the end of the stillness, then smoothstep to 0.
    EXPECT_NEAR(hold.Weight(49 * Ms), 1.0f, 1e-6f);
    EXPECT_NEAR(hold.Weight(50 * Ms), 1.0f, 1e-6f);
    EXPECT_NEAR(hold.Weight(51 * Ms), 1.0f, 1e-3f);
    EXPECT_NEAR(hold.Weight(100 * Ms), 0.5f, 1e-5f);
    EXPECT_NEAR(hold.Weight(149 * Ms), 0.0f, 1e-3f);
    EXPECT_NEAR(hold.Weight(150 * Ms), 0.0f, 1e-6f);
    EXPECT_TRUE(hold.PhaseAt(150 * Ms) == Phase::Off);
    // Halfway, the rotation is halfway too (slerp).
    InjectHold turn;
    ASSERT_TRUE(turn.Start(Hand(at, {}), 1, 0, 50 * Ms, 100 * Ms));
    TrackedHand live = Hand(at + Vec3{0.1f, 0.0f, 0.0f}, AxisAngle({0.0f, 0.0f, 1.0f}, 1.0f));
    EXPECT_TRUE(turn.Apply(live, 1, 100 * Ms) == Phase::Release);
    EXPECT_NEAR(AngleBetween(live.q, {}), 0.5f, 1e-3f);
    EXPECT_NEAR(live.p.x, 0.05f, 1e-5f);
    EXPECT_NEAR(live.v.x, 0.5f, 1e-5f); // the controller's velocity, eased in as well
}

TEST(InjectHold, NoReleaseTimeGoesBackAtOnce) {
    InjectHold hold;
    ASSERT_TRUE(hold.Start(Hand({0.0f, 1.0f, -0.3f}), 1, 0, 50 * Ms, 0));
    TrackedHand live = Hand({0.2f, 1.0f, -0.3f});
    EXPECT_TRUE(hold.Apply(live, 1, 49 * Ms) == Phase::Held);
    live = Hand({0.2f, 1.0f, -0.3f});
    EXPECT_TRUE(hold.Apply(live, 1, 50 * Ms) == Phase::Off);
    EXPECT_NEAR(live.p.x, 0.2f, 1e-6f);
}

TEST(InjectHold, TheGunReleasesItEarly) {
    InjectHold hold;
    const Vec3 at{0.0f, 1.0f, -0.3f};
    ASSERT_TRUE(hold.Start(Hand(at), 1, 0, 50 * Ms, 100 * Ms));
    hold.Release(20 * Ms); // the left trigger
    EXPECT_EQ(hold.HoldEndNs(), 20 * Ms);
    EXPECT_TRUE(hold.PhaseAt(20 * Ms) == Phase::Release);
    EXPECT_NEAR(hold.Weight(70 * Ms), 0.5f, 1e-5f);
    EXPECT_TRUE(hold.PhaseAt(120 * Ms) == Phase::Off);
    // A second release while easing back changes nothing.
    hold.Release(60 * Ms);
    EXPECT_EQ(hold.HoldEndNs(), 20 * Ms);
    // A release before the start (clock order) never moves the end before the start.
    InjectHold early;
    ASSERT_TRUE(early.Start(Hand(at), 1, 100 * Ms, 50 * Ms, 100 * Ms));
    early.Release(90 * Ms);
    EXPECT_EQ(early.HoldEndNs(), 100 * Ms);
}

TEST(InjectHold, RecenterAndLostTracking) {
    const Vec3 at{0.0f, 1.0f, -0.3f};
    // A recenter (another tracking space): the held pose is in the old one, the live grip passes.
    InjectHold hold;
    ASSERT_TRUE(hold.Start(Hand(at), 3, 0, 50 * Ms, 100 * Ms));
    TrackedHand live = Hand({0.3f, 1.2f, -0.1f});
    EXPECT_TRUE(hold.Apply(live, 4, 10 * Ms) == Phase::Off);
    EXPECT_NEAR(live.p.x, 0.3f, 1e-6f);
    EXPECT_FALSE(hold.On());
    Vec3 p{0.3f, 1.2f, -0.1f};
    Quat q{};
    EXPECT_FALSE(hold.Blend(p, q, 3, 10 * Ms)); // ended: nothing to blend

    // Lost while still: the held pose, tracked (the hand stays where it went in).
    InjectHold still;
    ASSERT_TRUE(still.Start(Hand(at), 3, 0, 50 * Ms, 100 * Ms));
    TrackedHand lost = Hand({0.3f, 1.2f, -0.1f});
    lost.position_tracked = false;
    EXPECT_TRUE(still.Apply(lost, 3, 10 * Ms) == Phase::Held);
    EXPECT_TRUE(IsTracked(lost));
    EXPECT_LT(Dist(lost.p, at), 1e-6f);
    // Lost while easing back: the live grip as it is, and the hold is over.
    TrackedHand lost2 = Hand({0.3f, 1.2f, -0.1f});
    lost2.position_tracked = false;
    EXPECT_TRUE(still.Apply(lost2, 3, 80 * Ms) == Phase::Off);
    EXPECT_FALSE(IsTracked(lost2));
    EXPECT_NEAR(lost2.p.x, 0.3f, 1e-6f);
    EXPECT_FALSE(still.On());
}

TEST(InjectHold, BlendMatchesApplyWithoutChangingState) {
    const Vec3 at{0.0f, 1.0f, -0.3f};
    InjectHold hold;
    ASSERT_TRUE(hold.Start(Hand(at, AxisAngle({1.0f, 0.0f, 0.0f}, 0.3f)), 2, 0, 50 * Ms,
                           100 * Ms));
    for (const std::int64_t now : {10 * Ms, 60 * Ms, 100 * Ms, 140 * Ms}) {
        const Vec3 live_p{0.2f, 1.1f, -0.2f};
        const Quat live_q = AxisAngle({0.0f, 1.0f, 0.0f}, -0.5f);
        Vec3 p = live_p;
        Quat q = live_q;
        EXPECT_TRUE(hold.Blend(p, q, 2, now));
        TrackedHand live = Hand(live_p, live_q);
        hold.Apply(live, 2, now);
        EXPECT_LT(Dist(p, live.p), 1e-6f);
        EXPECT_LT(AngleBetween(q, live.q), 1e-4f);
        Vec3 other = live_p;
        Quat other_q = live_q;
        EXPECT_FALSE(hold.Blend(other, other_q, 9, now)); // another space: untouched
        EXPECT_NEAR(other.x, 0.2f, 1e-6f);
    }
    // A new injection while one still runs starts over from the new grip.
    ASSERT_TRUE(hold.Start(Hand({0.5f, 1.0f, -0.3f}), 2, 145 * Ms, 50 * Ms, 100 * Ms));
    TrackedHand live = Hand({0.0f, 0.0f, 0.0f});
    EXPECT_TRUE(hold.Apply(live, 2, 150 * Ms) == Phase::Held);
    EXPECT_NEAR(live.p.x, 0.5f, 1e-6f);
}

// A second injection while the first hold is still or easing back (the presenter's
// StartInjectHold): the new hold starts from the pose the game was last given (Blend of the live
// grip at that sample's time), so the arms never jump to the controller and back.
TEST(InjectHold, ASecondInjectionStartsWhereTheArmsAre) {
    const Vec3 at{0.0f, 1.0f, -0.3f};
    const Vec3 live_p{0.06f, 1.0f, -0.3f}; // the controller, 6 cm away
    for (const std::int64_t t : {30 * Ms, 80 * Ms, 95 * Ms}) { // held, mid-release, nearly back
        InjectHold hold;
        ASSERT_TRUE(hold.Start(Hand(at), 1, 0, 67 * Ms, 50 * Ms));
        TrackedHand published = Hand(live_p);
        hold.Apply(published, 1, t); // what the game was given at t
        TrackedHand start = Hand(live_p);
        EXPECT_TRUE(hold.Blend(start.p, start.q, 1, t));
        EXPECT_LT(Dist(start.p, published.p), 1e-6f) << t;
        hold.Release(t + Ms); // the old freeze ended ("a new injection")
        ASSERT_TRUE(hold.Start(start, 1, t + Ms, 67 * Ms, 50 * Ms));
        TrackedHand next = Hand(live_p);
        EXPECT_TRUE(hold.Apply(next, 1, t + 2 * Ms) == Phase::Held);
        EXPECT_LT(Dist(next.p, published.p), 1e-6f) << t; // no jump
    }
}

// FreezeFrames: the game's own frames, one per main-player pose copy (BodyStatus::seq and the
// copy's start), counted from the time the held pose was published.
TEST(FreezeFrames, CountsTheGamesOwnFrames) {
    constexpr std::int64_t G = 16'666'667; // 60 fps
    FreezeFrames f;
    const std::int64_t from = 1000 * Ms;
    // The newest status (seq 10) began before the held pose was published: not counted.
    ASSERT_TRUE(f.Start(from, 4, G, true, 10, 990 * Ms));
    EXPECT_TRUE(f.On());
    EXPECT_EQ(f.CountedFrames(), 0);
    EXPECT_EQ(f.CapNs(), from + 6 * G); // 1.5 x 4 frames: a stall never holds them longer
    f.Note(10, 990 * Ms); // the same status again
    EXPECT_EQ(f.CountedFrames(), 0);
    f.Note(11, 1005 * Ms);
    EXPECT_EQ(f.CountedFrames(), 1);
    EXPECT_EQ(f.FirstNs(), 1005 * Ms);
    f.Note(11, 1005 * Ms);
    EXPECT_EQ(f.CountedFrames(), 1);
    f.Note(13, 1038 * Ms); // two copies between two looks (a catch-up): both count
    EXPECT_EQ(f.CountedFrames(), 3);
    EXPECT_FALSE(f.Done(1050 * Ms));
    f.Note(14, 1055 * Ms);
    EXPECT_EQ(f.CountedFrames(), 4);
    EXPECT_TRUE(f.Done(1056 * Ms));
    EXPECT_TRUE(f.Counted());
    EXPECT_EQ(f.EndNs(), 1055 * Ms); // the stillness ends at the 4th frame's start
    // Four frames on screen: 1005 .. 1055 ms and the last one's 16.7 ms.
    EXPECT_NEAR(static_cast<double>(f.StillNs()) * 1e-6, 66.67, 0.01);
    f.Note(15, 1072 * Ms); // later frames change nothing
    EXPECT_EQ(f.CountedFrames(), 4);
    EXPECT_EQ(f.EndNs(), 1055 * Ms);
    f.Stop();
    EXPECT_FALSE(f.On());
    EXPECT_FALSE(f.Done(2000 * Ms));
}

TEST(FreezeFrames, ACopyThatAlreadyReadTheHeldPoseCounts) {
    constexpr std::int64_t G = 33'333'333; // 30 fps
    FreezeFrames f;
    // The pose copy began 0.8 ms after the stab frame's grips were published (before the
    // injection's answer came): it drew the held pose.
    ASSERT_TRUE(f.Start(1000 * Ms, 4, G, true, 20, 1000 * Ms + 800'000));
    EXPECT_EQ(f.CountedFrames(), 1);
    EXPECT_EQ(f.FirstNs(), 1000 * Ms + 800'000);
    // A status that began before `from` (an older copy seen late) is never counted.
    FreezeFrames g;
    ASSERT_TRUE(g.Start(1000 * Ms, 4, G, true, 20, 990 * Ms));
    g.Note(21, 999 * Ms);
    EXPECT_EQ(g.CountedFrames(), 0);
    g.Note(22, 1020 * Ms);
    EXPECT_EQ(g.CountedFrames(), 1);
    for (std::uint64_t s = 23; s <= 25; ++s) {
        g.Note(s, 1020 * Ms + static_cast<std::int64_t>(s - 22) * G);
    }
    EXPECT_TRUE(g.Done(1121 * Ms));
    EXPECT_EQ(g.EndNs(), 1020 * Ms + 3 * G); // 133 ms of still frames at 30 fps
    EXPECT_NEAR(static_cast<double>(g.StillNs()) * 1e-6, 133.33, 0.01);
}

TEST(FreezeFrames, TimedWithoutTheBodyAndCappedInAStall) {
    constexpr std::int64_t G = 16'666'667;
    // No fresh status (loading, third person, the body off): the frames at the frame time.
    FreezeFrames t;
    ASSERT_TRUE(t.Start(0, 4, G, false));
    t.Note(5, 10 * Ms); // never counts
    EXPECT_EQ(t.CountedFrames(), 0);
    EXPECT_FALSE(t.Done(4 * G - 1));
    EXPECT_TRUE(t.Done(4 * G));
    EXPECT_FALSE(t.Counted());
    EXPECT_EQ(t.EndNs(), 4 * G);
    EXPECT_EQ(t.StillNs(), 4 * G);
    // Counting, but the game stalls after two frames: over at the cap (1.5 x 4 frames).
    FreezeFrames c;
    ASSERT_TRUE(c.Start(0, 4, G, true, 1, -5 * Ms));
    c.Note(2, 3 * Ms);
    c.Note(3, 20 * Ms);
    EXPECT_FALSE(c.Done(99 * Ms));
    EXPECT_TRUE(c.Done(6 * G));
    EXPECT_FALSE(c.Counted());
    EXPECT_EQ(c.EndNs(), 6 * G);
    // None, and the clamp.
    FreezeFrames n;
    EXPECT_FALSE(n.Start(0, 0, G, true));
    EXPECT_FALSE(n.On());
    EXPECT_FALSE(n.Done(Ms * 1000));
    ASSERT_TRUE(n.Start(0, 99, G, false));
    EXPECT_EQ(n.Frames(), FreezeFrames::MaxFrames);
    // One frame: the first counted is also the last.
    FreezeFrames one;
    ASSERT_TRUE(one.Start(0, 1, G, true, 7, -1 * Ms));
    one.Note(8, 4 * Ms);
    EXPECT_TRUE(one.Done(5 * Ms));
    EXPECT_EQ(one.EndNs(), 4 * Ms);
    EXPECT_EQ(one.StillNs(), G);
}

// The presenter's loop as it runs (vial_inject_hold_frames=10, the default since 2026-10-08, and
// the 4 before it; vial_inject_release_ms=50): XR frames publish the hands (the freeze counted
// from the newest body status first, released at the last still frame's start), the game's pose
// copies read the
// newest hands as they begin and publish their status about 1 ms later. The hand at the stab
// pulls back at 1.5 m/s. For the headset's rates and the game at 60 and 30 fps, whatever the
// phase between them: exactly that many game frames draw the held pose, the next already moves
// toward the controller, and the hand is the controller's again a few frames later, never a jump.
TEST(FreezeFrames, GameFramesStillAtAnyRate) {
    struct Case {
        double xr_hz;
        double fps;
        int frames;
    };
    for (const Case c :
         {Case{120.0, 60.0, 10}, Case{90.0, 60.0, 10}, Case{72.0, 60.0, 10}, Case{120.0, 30.0, 10},
          Case{90.0, 30.0, 10}, Case{72.0, 30.0, 10}, Case{120.0, 60.0, 4}, Case{90.0, 60.0, 4},
          Case{72.0, 60.0, 4}, Case{120.0, 30.0, 4}, Case{90.0, 30.0, 4}, Case{72.0, 30.0, 4}}) {
        const int frames = c.frames;
        const auto xr = static_cast<std::int64_t>(1e9 / c.xr_hz);
        const auto game = static_cast<std::int64_t>(1e9 / c.fps);
        for (int phase = 0; phase < 8; ++phase) {
            const std::int64_t copy0 = 1 * Ms + phase * game / 8; // the first copy's start
            const std::int64_t stab_due = 30 * xr; // the stab frame: the first XR frame from here
            const Vec3 origin{0.05f, 1.05f, -0.35f};
            const auto live_at = [&](std::int64_t t) {
                const float s = static_cast<float>(t - stab_due) * 1e-9f;
                return origin + Vec3{-1.5f * s, 0.0f, 0.0f};
            };
            InjectHold hold;
            FreezeFrames ff;
            std::int64_t t_stab = -1;  // the stab frame's publish
            std::int64_t t_start = -1; // its input step (1.5 ms later): healed
            Vec3 at{};                 // the pose published at the stab frame: the held pose
            Vec3 published = live_at(0);
            std::uint64_t visible_seq = 0; // the newest status the presenter can read
            std::int64_t visible_ns = 0;
            std::int64_t next_xr = 0;
            std::int64_t next_copy = copy0;
            std::vector<std::pair<std::int64_t, Vec3>> reads; // (copy start, pose read)
            std::vector<std::int64_t> pending;                // status stamps not yet visible
            for (std::int64_t t = 0; t <= stab_due + 700 * Ms; t += 100'000) {
                while (!pending.empty() && pending.front() + 1 * Ms <= t) {
                    ++visible_seq;
                    visible_ns = pending.front();
                    pending.erase(pending.begin());
                }
                if (t >= next_xr) {
                    next_xr += xr;
                    if (ff.On()) {
                        ff.Note(visible_seq, visible_ns);
                        if (ff.Done(t)) {
                            hold.Release(ff.EndNs());
                            ff.Stop();
                        }
                    }
                    TrackedHand h = Hand(live_at(t));
                    hold.Apply(h, 1, t);
                    published = h.p;
                    if (t_stab < 0 && t >= stab_due) {
                        t_stab = t;
                        t_start = t + 1'500'000;
                        at = published;
                    }
                }
                if (t_start >= 0 && t == t_start) {
                    ASSERT_TRUE(ff.Start(t_stab, frames, game, true, visible_seq, visible_ns));
                    ASSERT_TRUE(hold.Start(Hand(at), 1, t, ff.CapNs() - t, 50 * Ms));
                }
                if (t >= next_copy) {
                    next_copy += game;
                    reads.emplace_back(t, published);
                    pending.push_back(t);
                }
            }
            // The copies from the stab on: the first `frames` draw exactly the held pose.
            int still = 0;
            size_t first = 0;
            while (first < reads.size() && reads[first].first < t_stab) {
                ++first;
            }
            size_t i = first;
            while (i < reads.size() && Dist(reads[i].second, at) < 1e-6f) {
                ++still;
                ++i;
            }
            // frames, Hz, fps, phase
            const double label = frames * 1e6 + c.xr_hz * 1000.0 + c.fps * 10.0 + phase;
            EXPECT_EQ(still, frames) << label;
            ASSERT_LT(i + 4, reads.size());
            // The next has started back toward the controller (at least 1% of the gap)...
            const Vec3 p5 = reads[i].second;
            const Vec3 live5 = live_at(reads[i].first);
            EXPECT_GT(Dist(p5, at), 0.01f * Dist(live5, at)) << label;
            EXPECT_LT(Dist(p5, live5), Dist(at, live5)) << label;
            // ... never past it (no overshoot: at most the gap and the controller's own motion
            // in one game frame), and it is the controller's again within 50 ms + 2 frames.
            float max_step = 0.0f;
            for (size_t k = i; k < reads.size(); ++k) {
                const float step = Dist(reads[k].second, reads[k - 1].second);
                max_step = std::max(max_step, step);
            }
            const float gap = Dist(at, live_at(reads[i].first));
            EXPECT_LE(max_step, gap + 1.5f * static_cast<float>(game) * 1e-9f + 0.01f) << label;
            const std::int64_t back_by = reads[i - 1].first + 50 * Ms + 2 * game;
            for (const auto& [t, p] : reads) {
                if (t >= back_by) {
                    EXPECT_LT(Dist(p, live_at(t)), 0.03f) << label << " " << t;
                }
            }
            // The stillness measured: about `frames` frames on screen.
            EXPECT_FALSE(hold.On());
        }
    }
}

// The weapon hand's way out: a stroke under way is faster than SwingBreakSpeed, below the swing
// detector's fire speed (2 m/s), so the blade is back on the controller before a swing commits.
TEST(FreezeFrames, TheWeaponHandLetsGoForASwing) {
    EXPECT_GT(SwingBreakSpeed, 1.0f); // the swing gate's still hand (1 m/s) never breaks it
    EXPECT_LT(SwingBreakSpeed, 2.0f); // a swing's fire speed always does, before it commits
    InjectHold right;
    const Vec3 at{0.3f, 1.0f, -0.3f};
    ASSERT_TRUE(right.Start(Hand(at), 1, 0, 100 * Ms, 50 * Ms));
    right.Release(20 * Ms); // a swing at 20 ms
    EXPECT_TRUE(right.PhaseAt(20 * Ms) == Phase::Release);
    EXPECT_TRUE(right.PhaseAt(70 * Ms) == Phase::Off);
}

// X held from idle for 2 s at 120 Hz: one arm on the press, a single Held note at 0.6 s (the log;
// it does nothing), never a disarm nor a second arm - and nothing that could be the game's vial.
TEST(VialButton, HoldingXOnlyArmsOnce) {
    VialButton x;
    bool armed = false;
    int arms = 0, disarms = 0, helds = 0;
    float held_at = -1.0f;
    const float dt = 1.0f / 120.0f;
    EXPECT_TRUE(x.Update(false, armed, false, dt) == VialKey::None);
    for (int f = 1; f <= 240; ++f) {
        const VialKey k = x.Update(true, armed, false, dt);
        if (k == VialKey::Arm) {
            ++arms;
            armed = true;
            EXPECT_EQ(f, 1);
        } else if (k == VialKey::Disarm) {
            ++disarms;
        } else if (k == VialKey::Held) {
            ++helds;
            held_at = static_cast<float>(f - 1) * dt;
        }
    }
    EXPECT_EQ(arms, 1);
    EXPECT_EQ(disarms, 0);
    EXPECT_EQ(helds, 1);
    EXPECT_NEAR(held_at, VialButton::LongHoldS, dt + 1e-4f);
    EXPECT_TRUE(armed); // the vial stays armed: stab to inject
    EXPECT_TRUE(x.Down());
    // Let go: nothing; pressed again: the vial goes back (a hold of that press: still nothing).
    EXPECT_TRUE(x.Update(false, armed, false, dt) == VialKey::None);
    EXPECT_FALSE(x.Down());
    EXPECT_NEAR(x.HeldS(), 0.0f, 1e-6f);
    EXPECT_TRUE(x.Update(true, armed, false, dt) == VialKey::Disarm);
    armed = false;
    int others = 0;
    for (int f = 0; f < 120; ++f) {
        const VialKey k = x.Update(true, armed, false, dt);
        others += k == VialKey::Arm || k == VialKey::Disarm ? 1 : 0;
    }
    EXPECT_EQ(others, 0);
}

TEST(VialButton, TapsArmAndDisarm) {
    VialButton x;
    const float dt = 1.0f / 120.0f;
    EXPECT_TRUE(x.Update(true, false, false, dt) == VialKey::Arm);
    EXPECT_TRUE(x.Update(true, true, false, dt) == VialKey::None);
    EXPECT_TRUE(x.Update(false, true, false, dt) == VialKey::None);
    EXPECT_TRUE(x.Update(true, true, false, dt) == VialKey::Disarm);
    EXPECT_TRUE(x.Update(false, false, false, dt) == VialKey::None);
    // A quick second press (a double tap) arms again: never a vial used.
    EXPECT_TRUE(x.Update(true, false, false, dt) == VialKey::Arm);
}

// X held through the injection of the vial it armed: no second vial armed until X is let go and
// pressed again; a press while the injection is with the game arms nothing.
TEST(VialButton, HeldThroughAnInjection) {
    VialButton x;
    const float dt = 1.0f / 120.0f;
    ASSERT_TRUE(x.Update(true, false, false, dt) == VialKey::Arm);
    // Stabbed (the presenter disarms and waits for the game), X still held.
    for (int f = 0; f < 10; ++f) {
        EXPECT_TRUE(x.Update(true, false, true, dt) != VialKey::Arm);
    }
    for (int f = 0; f < 200; ++f) {
        const VialKey k = x.Update(true, false, false, dt);
        EXPECT_TRUE(k == VialKey::None || k == VialKey::Held);
    }
    EXPECT_TRUE(x.Update(false, false, true, dt) == VialKey::None);
    EXPECT_TRUE(x.Update(true, false, true, dt) == VialKey::None); // the game still has one
    EXPECT_TRUE(x.Update(false, false, false, dt) == VialKey::None);
    EXPECT_TRUE(x.Update(true, false, false, dt) == VialKey::Arm);
}

TEST(VialButton, ResetForgetsThePress) {
    VialButton x;
    const float dt = 1.0f / 120.0f;
    ASSERT_TRUE(x.Update(true, false, false, dt) == VialKey::Arm);
    x.Reset(); // a context switch (the presenter ignores X until it is let go)
    EXPECT_FALSE(x.Down());
    EXPECT_TRUE(x.Update(false, false, false, dt) == VialKey::None);
    EXPECT_TRUE(x.Update(true, false, false, dt) == VialKey::Arm);
}

TEST(FreezeTime, FixedDeadlineAtDifferentSamplingRates) {
    for (const int hz : {15, 30, 60, 72, 90, 120, 144}) {
        FreezeTime timer;
        InjectHold hold;
        const auto start = 1000 * Ms;
        ASSERT_TRUE(timer.Start(start, 220));
        ASSERT_TRUE(hold.Start(Hand({0, 0, 0}), 1, start,
                               timer.EndNs() - start, 80 * Ms));
        EXPECT_EQ(timer.EndNs(), start + 220 * Ms);
        for (int frame = 0; frame <= hz; ++frame) {
            const auto elapsed = std::int64_t{frame} * 1'000'000'000 / hz;
            const auto now = start + elapsed;
            EXPECT_EQ(timer.Done(now), elapsed >= 220 * Ms);
            auto live = Hand({0.3f, 0, 0}); // pulling away must not shorten the hold
            const auto phase = hold.Apply(live, 1, now);
            if (elapsed < 220 * Ms) {
                EXPECT_EQ(phase, Phase::Held);
                EXPECT_FLOAT_EQ(live.p.x, 0.0f);
            } else if (elapsed < 300 * Ms) {
                EXPECT_EQ(phase, Phase::Release);
            } else {
                EXPECT_EQ(phase, Phase::Off);
                EXPECT_FLOAT_EQ(live.p.x, 0.3f);
            }
        }
    }
}

TEST(FreezeTime, StallsDoNotExtendHoldOrRestartRelease) {
    FreezeTime timer;
    InjectHold hold;
    ASSERT_TRUE(timer.Start(1000 * Ms, 220));
    ASSERT_TRUE(hold.Start(Hand({0, 0, 0}), 1, 1000 * Ms, 220 * Ms, 80 * Ms));
    EXPECT_FALSE(timer.Done(1220 * Ms - 1));
    EXPECT_TRUE(timer.Done(1220 * Ms));
    auto live = Hand({0.3f, 0, 0});
    EXPECT_EQ(hold.Apply(live, 1, 1500 * Ms), Phase::Off);
    EXPECT_FLOAT_EQ(live.p.x, 0.3f);
    EXPECT_EQ(timer.EndNs(), 1220 * Ms);
    timer.Stop();
    EXPECT_FALSE(timer.On());
    EXPECT_FALSE(timer.Start(2000 * Ms, 0));
}

} // namespace

TEST(VialGripHold, KeepsHoldUntilReleaseAndReturnsOnlyOnce) {
    VR::Body::VialGripHold hold;
    hold.Start(.45f);
    for (int i = 0; i < 300; ++i)
        EXPECT_FALSE(hold.Return(.46f, true));
    EXPECT_TRUE(hold.Return(.45f, true));
    EXPECT_FALSE(hold.Return(0, true));
    EXPECT_FALSE(hold.Return(1, true)); // regripping cannot resurrect a returned vial
}
TEST(VialGripHold, XDrawAndFinishedInjectionHaveNoGripOwnership) {
    VR::Body::VialGripHold hold;
    EXPECT_FALSE(hold.Return(0, true)); // X does not Start ownership
    hold.Start(.45f);
    hold.Reset(); // DisarmVial, including before submitting an injection
    EXPECT_FALSE(hold.Return(0, true)); // cannot cancel/refund the injection
}
TEST(VialGripHold, TrackingLossSafelyReturnsAndCannotRearm) {
    VR::Body::VialGripHold hold;
    hold.Start(.45f);
    EXPECT_TRUE(hold.Return(1, false));
    EXPECT_FALSE(hold.Return(1, true));
    hold.Start(.45f);
    EXPECT_TRUE(hold.Return(std::numeric_limits<float>::quiet_NaN(), true));
}
TEST(VialGripHold, UsesReleaseThresholdFromTheDraw) {
    VR::Body::VialGripHold hold;
    hold.Start(.25f);
    EXPECT_FALSE(hold.Return(.35f, true));
    EXPECT_TRUE(hold.Return(.25f, true));
}
