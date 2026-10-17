// SPDX-License-Identifier: GPL-2.0-or-later

// The Beast Cutter's flick recall (core/vr/whip_flick.h): a quick snap of the wrist with the hand
// nearly still reels the paid-out chain in, once; a lash (the arm moving), a slow turn, a wrist
// circle, the lash's own wrist and a second snap right after the first never do; disabled, never.
// Since 2026-10-15 (whip_flick_recall=1) only a snap lifting the weapon back recalls: a crack (the
// same snap thrown out: down, forward, sideways), a crack's wind-up and its rebound never do, and a
// recall's lift is never a crack (CrackGate). whip_flick_recall=any keeps the any-direction snap.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>

#include <gtest/gtest.h>

#include "core/vr/whip_chain.h"
#include "core/vr/whip_flick.h"
#include "core/vr/whip_spin.h"

namespace W = VR::Whip;
namespace B = VR::Body;

namespace {

constexpr float Deg = 3.14159265f / 180.0f;
// The grip is about this far in front of the wrist's pivot: a wrist snap moves it this x omega.
constexpr float WristToGripM = 0.08f;

// The turn's direction (FlickInput::lift = this x omega): a recall's lift, a crack's throw.
constexpr float Lift = 1.0f;     // the far end up / back
constexpr float Down = -1.0f;    // ... down / forward
constexpr float Sideways = 0.0f; // ... across

struct Rig {
    W::WristFlick flick{};
    W::FlickConfig cfg{};
    float dt{1.0f / 90.0f};
    float extension{3.0f};
    bool asked_out{true};
    bool enabled{true};
    bool valid{true}; // the hunter's frame known
    float dir{Lift}; // the turn's share lifting the far end
    int fired{0};
    int lifting{0};  // frames the crack gate was held off
    int dropped{0};  // lifts let go (a wind-up, a rebound)
    std::string why; // ... the last one's reason

    explicit Rig(bool any = false) : cfg(any ? W::AnyDirectionFlick() : W::FlickConfig{}) {}

    // `s` seconds of the grip turning at `deg_s` with the hand at `hand` m/s.
    void Hold(float s, float deg_s, float hand) {
        const int frames = std::max(1, static_cast<int>(std::lround(s / dt)));
        for (int i = 0; i < frames; ++i) {
            Frame(deg_s, hand);
        }
    }
    bool Frame(float deg_s, float hand) {
        W::FlickInput in{};
        in.dt = dt;
        in.omega = deg_s * Deg;
        in.hand_speed = hand;
        in.extension = extension;
        in.asked_out = asked_out;
        in.valid = valid;
        in.lift = dir * in.omega;
        const bool r = flick.Step(in, cfg, enabled);
        if (flick.Dropped() != nullptr) {
            why = flick.Dropped();
        }
        fired += flick.Fired() ? 1 : 0;
        lifting += flick.Lifting() ? 1 : 0;
        dropped += flick.Dropped() != nullptr ? 1 : 0;
        return r;
    }
    // A flick back, the wrist just stopped: its recall waits (`confirm_s`), the crack held off.
    void Lifted() {
        Snap(750.0f, 0.08f, Lift, 0.0f);
        Frame(20.0f, 0.05f);
        EXPECT_EQ(fired, 0);
        EXPECT_TRUE(flick.Lifting());
    }
    // One long frame (a hitch): `s` seconds, the grip's turn and speed its average over them.
    bool Hitch(float s, float deg_s, float hand) {
        const float keep = dt;
        dt = s;
        const bool r = Frame(deg_s, hand);
        dt = keep;
        return r;
    }
    // A wrist snap: a frame at half the speed, `s` at `deg_s`, a frame at half again, then still
    // for `tail` s (< 0: 0.05 s and long enough for a lift's recall to be confirmed; 0: none).
    void Snap(float deg_s, float s, float d = Lift, float tail = -1.0f) {
        const float keep = dir;
        dir = d;
        const float hand = deg_s * Deg * WristToGripM;
        Frame(deg_s * 0.5f, hand * 0.5f);
        Hold(s, deg_s, hand);
        Frame(deg_s * 0.5f, hand * 0.5f);
        dir = keep;
        if (tail < 0.0f) {
            tail = 0.05f + cfg.confirm_s;
        }
        if (tail > 0.0f) {
            Hold(tail, 20.0f, 0.05f);
        }
    }
};

// The properties that hold whatever the direction rule: the lift's (=1) and any snap's (=any).
class WristFlick : public ::testing::TestWithParam<bool> {};

TEST_P(WristFlick, QuickSnapWithTheHandStillRecallsOnce) {
    for (const int hz : {30, 60, 90}) {
        for (const float deg_s : {600.0f, 750.0f, 900.0f}) {
            for (const float s : {0.06f, 0.08f, 0.10f}) {
                SCOPED_TRACE(::testing::Message() << hz << " Hz, " << deg_s << " deg/s for " << s
                                                  << " s");
                Rig rig{GetParam()};
                rig.dt = 1.0f / hz;
                rig.Hold(0.40f, 30.0f, 0.2f); // the chain out (the lash 0.4 s ago), held
                ASSERT_FALSE(rig.flick.Returning());
                rig.Snap(deg_s, s);
                EXPECT_EQ(rig.fired, 1);
                EXPECT_EQ(rig.flick.Flicks(), 1u);
                ASSERT_TRUE(rig.flick.Returning());
                EXPECT_GE(rig.flick.Last().angle, rig.cfg.min_angle);
                // Held while the chain reels in: the snap's tip speed cannot pay it out again.
                W::Payout payout;
                const W::PayoutConfig pc;
                payout.Step(8.0f, 4.0f, rig.dt, pc);
                ASSERT_GT(payout.Target(), 3.9f);
                EXPECT_FLOAT_EQ(payout.Step(8.0f, 4.0f, rig.dt, pc, rig.Frame(300.0f, 0.4f)), 1.0f);
                EXPECT_TRUE(payout.Reeled());
                rig.asked_out = false;
                for (int i = 0; i < hz / 5; ++i) {
                    rig.extension = std::max(1.0f, rig.extension - 0.5f);
                    EXPECT_TRUE(rig.Frame(300.0f, 0.4f) || rig.extension <= 1.05f);
                }
                // Back on the stick and the wrist still: the recall lets go.
                rig.extension = 1.0f;
                EXPECT_FALSE(rig.Frame(20.0f, 0.05f));
                EXPECT_EQ(rig.fired, 1);
            }
        }
    }
}

TEST_P(WristFlick, ALashMovingTheArmNeverRecalls) {
    for (const int hz : {30, 60, 90}) {
        SCOPED_TRACE(::testing::Message() << hz << " Hz");
        Rig rig{GetParam()};
        rig.dt = 1.0f / hz;
        rig.asked_out = false;
        rig.extension = 1.0f;
        rig.Hold(0.2f, 10.0f, 0.1f);
        // The lash: the arm at 3-4 m/s and the wrist turning with it; the chain pays out.
        rig.asked_out = true;
        for (int i = 0; i < static_cast<int>(0.35f * hz); ++i) {
            rig.extension = std::min(4.0f, rig.extension + 0.2f);
            rig.Frame(i % 2 ? 800.0f : 650.0f, 3.5f);
        }
        rig.Hold(0.10f, 100.0f, 0.5f); // its stop
        // Another stroke while it is out (well after the lash began): the arm again.
        rig.Hold(0.12f, 750.0f, 2.8f);
        rig.Hold(0.10f, 50.0f, 0.3f);
        // A shorter jab of the arm with a wrist snap in it.
        rig.Hold(0.08f, 900.0f, 1.9f);
        rig.Hold(0.10f, 50.0f, 0.3f);
        rig.Hold(0.20f, 20.0f, 0.05f);
        EXPECT_EQ(rig.fired, 0);
        EXPECT_EQ(rig.lifting, 0) << "a lash is never held off as a recall";
        EXPECT_FALSE(rig.flick.Returning());
    }
}

TEST_P(WristFlick, SlowTurnsAndWristCirclesNeverRecall) {
    for (const int hz : {30, 60, 90}) {
        SCOPED_TRACE(::testing::Message() << hz << " Hz");
        Rig rig{GetParam()};
        rig.dt = 1.0f / hz;
        rig.Hold(0.4f, 20.0f, 0.1f);
        for (const float deg_s : {150.0f, 300.0f, 450.0f, 520.0f}) {
            rig.Hold(0.30f, deg_s, deg_s * Deg * WristToGripM);
            rig.Hold(0.10f, 20.0f, 0.05f);
        }
        // Small wrist circles keeping the chain circling: fast, but they never stop.
        for (const float deg_s : {600.0f, 800.0f}) {
            rig.Hold(1.0f, deg_s, 0.6f);
            rig.Hold(0.10f, 20.0f, 0.05f);
        }
        rig.Hold(0.20f, 20.0f, 0.05f);
        EXPECT_EQ(rig.fired, 0);
    }
}

TEST_P(WristFlick, ASecondSnapRightAfterTheFirstDoesNotFireAgain) {
    for (const int hz : {30, 60, 90}) {
        SCOPED_TRACE(::testing::Message() << hz << " Hz");
        Rig rig{GetParam()};
        rig.dt = 1.0f / hz;
        rig.Hold(0.4f, 20.0f, 0.1f);
        rig.Snap(750.0f, 0.08f);
        ASSERT_EQ(rig.fired, 1);
        rig.Snap(750.0f, 0.08f, Lift, 0.05f); // still recalling: nothing new
        EXPECT_EQ(rig.fired, 1);
        rig.Frame(100.0f, 2.2f); // a stroke ends the recall ...
        ASSERT_FALSE(rig.flick.Returning());
        rig.Snap(750.0f, 0.08f); // ... but within the refractory a snap does not count
        EXPECT_EQ(rig.fired, 1);
        rig.Hold(0.6f, 20.0f, 0.1f);
        rig.Snap(750.0f, 0.08f); // later it does
        EXPECT_EQ(rig.fired, 2);
        EXPECT_EQ(rig.flick.Flicks(), 2u);
    }
}

TEST_P(WristFlick, NotWhileTheLashPaysOutNorWithTheChainIn) {
    for (const int hz : {30, 60, 90}) {
        SCOPED_TRACE(::testing::Message() << hz << " Hz");
        Rig rig{GetParam()};
        rig.dt = 1.0f / hz;
        rig.asked_out = false;
        rig.extension = 1.0f;
        rig.Hold(0.3f, 20.0f, 0.1f);
        rig.Snap(750.0f, 0.08f); // at rest: a crack's business, not a recall
        EXPECT_EQ(rig.fired, 0);
        rig.Hold(0.6f, 20.0f, 0.1f);
        rig.asked_out = true; // a fresh lash ...
        rig.extension = 2.0f;
        rig.Hold(0.05f, 20.0f, 0.1f);
        rig.Snap(750.0f, 0.08f); // ... its own wrist, inside the cooldown
        EXPECT_EQ(rig.fired, 0);
        rig.Hold(0.3f, 20.0f, 0.1f);
        rig.extension = 1.05f; // reeling in by itself, hardly out
        rig.Snap(750.0f, 0.08f);
        EXPECT_EQ(rig.fired, 0);
        rig.extension = 3.0f;
        rig.Snap(750.0f, 0.08f); // out: now it counts
        EXPECT_EQ(rig.fired, 1);
    }
}

TEST_P(WristFlick, DisabledNeverRecalls) {
    Rig rig{GetParam()};
    rig.enabled = false;
    rig.Hold(0.4f, 20.0f, 0.1f);
    for (int i = 0; i < 4; ++i) {
        rig.Snap(800.0f, 0.08f);
        rig.Hold(0.6f, 20.0f, 0.1f);
    }
    EXPECT_EQ(rig.fired, 0);
    EXPECT_EQ(rig.lifting, 0);
    EXPECT_FALSE(rig.flick.Returning());
}

TEST_P(WristFlick, AHitchOrALostHunterBreaksTheSnap) {
    Rig rig{GetParam()};
    rig.Hold(0.4f, 20.0f, 0.1f);
    rig.Hold(0.05f, 750.0f, 0.9f);
    W::FlickInput in{};
    in.dt = 0.12f; // a long frame in the middle
    in.omega = 750.0f * Deg;
    in.lift = in.omega;
    in.hand_speed = 0.9f;
    in.extension = 3.0f;
    in.asked_out = true;
    rig.flick.Step(in, rig.cfg);
    rig.Frame(100.0f, 0.1f);
    in.dt = 1.0f / 90.0f;
    in.valid = false; // the hunter's frame lost
    rig.Hold(0.05f, 750.0f, 0.9f);
    rig.flick.Step(in, rig.cfg);
    rig.Frame(100.0f, 0.1f);
    rig.Hold(0.2f, 20.0f, 0.1f);
    EXPECT_EQ(rig.fired, 0);
    EXPECT_EQ(rig.flick.Flicks(), 0u);
}

INSTANTIATE_TEST_SUITE_P(LiftOrAny, WristFlick, ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool>& p) {
                             return p.param ? "AnyDirection" : "Lift";
                         });

// ---- whip_flick_recall=1: the lift recalls, a crack never does ---------------------------------

TEST(CutterFlick, ACrackThrownOutNeverRecalls) {
    // The review's case: a wrist crack 0.25 s or more after the chain went out, the hand still.
    for (const int hz : {30, 60, 90}) {
        for (const float deg_s : {600.0f, 750.0f, 900.0f}) {
            for (const float s : {0.06f, 0.08f, 0.10f}) {
                for (const float d : {Down, Sideways, -0.4f, 0.3f}) {
                    SCOPED_TRACE(::testing::Message() << hz << " Hz, " << deg_s << " deg/s for "
                                                      << s << " s, lifting " << d);
                    Rig rig;
                    rig.dt = 1.0f / hz;
                    rig.Hold(0.40f, 30.0f, 0.2f);
                    rig.Snap(deg_s, s, d);
                    rig.Hold(0.3f, 20.0f, 0.05f);
                    EXPECT_EQ(rig.fired, 0);
                    EXPECT_EQ(rig.lifting, 0) << "the crack gate is never held off";
                    EXPECT_FALSE(rig.flick.Returning());
                }
            }
        }
    }
}

TEST(CutterFlick, ADeliberateLiftStillRecallsAfterItsConfirm) {
    for (const int hz : {30, 60, 90}) {
        SCOPED_TRACE(::testing::Message() << hz << " Hz");
        Rig rig;
        rig.dt = 1.0f / hz;
        rig.Hold(0.40f, 30.0f, 0.2f);
        // A diagonal lift (up and a little across): 70% of its turn lifting.
        rig.Snap(750.0f, 0.08f, 0.7f, 0.05f); // the wrist just stopped ...
        EXPECT_EQ(rig.fired, 0) << "... the recall waits for its confirm";
        EXPECT_GT(rig.lifting, 0) << "... holding the crack off meanwhile";
        rig.Hold(rig.cfg.confirm_s, 20.0f, 0.05f);
        EXPECT_EQ(rig.fired, 1);
        EXPECT_TRUE(rig.flick.Returning());
        EXPECT_NEAR(rig.flick.Last().lift / rig.flick.Last().angle, 0.7f, 0.01f);
        EXPECT_EQ(rig.dropped, 0);
    }
}

TEST(CutterFlick, TheBounceAtTheEndOfAFlickBackKeepsTheRecall) {
    // A sharp stop leaves the wrist bouncing a little forward: slower than a snap, no crack.
    for (const int hz : {30, 60, 90}) {
        for (const float dip_s : {0.0f, 1.0f / 30.0f}) {
            SCOPED_TRACE(::testing::Message() << hz << " Hz, the bounce after " << dip_s << " s");
            Rig rig;
            rig.dt = 1.0f / hz;
            rig.Hold(0.40f, 30.0f, 0.2f);
            rig.Snap(800.0f, 0.08f, Lift, 0.0f);
            if (dip_s > 0.0f) {
                rig.Hold(dip_s, 100.0f, 0.1f);
            }
            const int held = rig.lifting;
            rig.dir = Down;
            rig.Hold(0.04f, 300.0f, 0.3f);
            EXPECT_EQ(rig.lifting - held, std::max(1, static_cast<int>(std::lround(0.04f * hz))))
                << "the crack stays held off through the bounce";
            rig.dir = Lift;
            rig.Hold(0.15f, 20.0f, 0.05f);
            EXPECT_EQ(rig.fired, 1);
            EXPECT_EQ(rig.dropped, 0);
        }
    }
}

TEST(CutterFlick, ALiftTheWristTurnsStraightOutOfIsACracksWindUp) {
    // Cock the wrist back, then snap it out at once: the crack, not a recall.
    for (const int hz : {30, 60, 90}) {
        for (const float dip_s : {0.0f, 1.0f / 30.0f, 0.06f}) {
            SCOPED_TRACE(::testing::Message() << hz << " Hz, the turn-round " << dip_s << " s");
            Rig rig;
            rig.dt = 1.0f / hz;
            rig.Hold(0.40f, 30.0f, 0.2f);
            rig.Snap(750.0f, 0.08f, Lift, 0.0f);
            rig.Hold(dip_s, 100.0f, 0.1f); // the wrist turning round (below the snap's speed)
            rig.Snap(900.0f, 0.08f, Down);
            rig.Hold(0.3f, 20.0f, 0.05f);
            EXPECT_EQ(rig.fired, 0);
            EXPECT_EQ(rig.dropped, 1) << "the lift let go as a wind-up";
        }
    }
}

TEST(CutterFlick, TheWristEasingBackAfterAFlickBackKeepsTheRecall) {
    // The review's feel risk: a flick back and then the wrist back to where it was within the
    // wait, quick (a snap's speed) but slower than the flick itself, was let go as a wind-up: no
    // recall, no crack. A turn out at the flick's own speed or faster is still the crack.
    for (const int hz : {30, 60, 90}) {
        for (const float out_deg_s : {560.0f, 650.0f, 740.0f, 760.0f, 900.0f}) {
            SCOPED_TRACE(::testing::Message() << hz << " Hz, back out at " << out_deg_s
                                              << " deg/s after a flick back at 750");
            Rig rig;
            rig.dt = 1.0f / hz;
            rig.Hold(0.40f, 30.0f, 0.2f);
            rig.Snap(750.0f, 0.08f, Lift, 0.0f);
            rig.Frame(100.0f, 0.1f); // the turn-round: the recall waits
            ASSERT_TRUE(rig.flick.Lifting());
            const int held = rig.lifting;
            rig.dir = Down;
            rig.Hold(0.06f, out_deg_s, out_deg_s * Deg * WristToGripM);
            const int frames = std::max(1, static_cast<int>(std::lround(0.06f * hz)));
            rig.dir = Lift;
            rig.Hold(0.2f, 20.0f, 0.05f);
            if (out_deg_s < 750.0f) {
                EXPECT_GE(rig.lifting - held, frames) << "the crack held off throughout";
                EXPECT_EQ(rig.fired, 1);
                EXPECT_EQ(rig.dropped, 0);
            } else {
                EXPECT_EQ(rig.fired, 0);
                EXPECT_EQ(rig.dropped, 1) << "the lift let go as a wind-up";
                EXPECT_NE(rig.why.find("wind-up"), std::string::npos) << rig.why;
            }
        }
    }
    // whip_flick_recall=1 with windup 0: any snap out lets the lift go (the stricter rule).
    Rig rig;
    rig.cfg.windup = 0.0f;
    rig.Hold(0.40f, 30.0f, 0.2f);
    rig.Snap(750.0f, 0.08f, Lift, 0.0f);
    rig.Frame(100.0f, 0.1f);
    rig.dir = Down;
    rig.Hold(0.06f, 560.0f, 560.0f * Deg * WristToGripM);
    rig.dir = Lift;
    rig.Hold(0.2f, 20.0f, 0.05f);
    EXPECT_EQ(rig.fired, 0);
    EXPECT_EQ(rig.dropped, 1);
}

TEST(CutterFlick, ACracksReboundIsNoRecallButALaterLiftIs) {
    for (const int hz : {30, 60, 90}) {
        SCOPED_TRACE(::testing::Message() << hz << " Hz");
        Rig rig;
        rig.dt = 1.0f / hz;
        rig.Hold(0.40f, 30.0f, 0.2f);
        rig.Snap(900.0f, 0.08f, Down, 0.05f); // the crack ...
        rig.Snap(700.0f, 0.08f, Lift);        // ... and the wrist coming back up from it
        rig.Hold(0.3f, 20.0f, 0.05f);
        EXPECT_EQ(rig.fired, 0);
        EXPECT_EQ(rig.lifting, 0);
        EXPECT_EQ(rig.dropped, 1) << "the lift let go as a rebound";
        rig.Snap(750.0f, 0.08f, Lift); // a deliberate flick back once the crack is over
        EXPECT_EQ(rig.fired, 1);
    }
}

TEST(CutterFlick, AHitchWhileTheRecallWaitsKeepsIt) {
    // The review's case: at 30 fps one dropped frame (66 ms) in the 0.1 s wait cleared a
    // deliberate recall without a word. The long frame's average turn is judged instead.
    for (const int hz : {30, 60, 90}) {
        SCOPED_TRACE(::testing::Message() << hz << " Hz");
        Rig rig;
        rig.dt = 1.0f / hz;
        rig.Hold(0.40f, 30.0f, 0.2f);
        rig.Lifted();
        rig.Hitch(2.0f / 30.0f, 20.0f, 0.05f); // a dropped frame, the wrist still
        EXPECT_TRUE(rig.flick.Lifting() || rig.fired == 1) << "still waiting, the crack held off";
        rig.Hold(0.15f, 20.0f, 0.05f);
        EXPECT_EQ(rig.fired, 1);
        EXPECT_EQ(rig.dropped, 0);
        EXPECT_TRUE(rig.flick.Returning());
    }
    // A run of long frames (15 fps) right after the lift: the wait ends, no endless hold-off.
    Rig rig;
    rig.dt = 1.0f / 90.0f;
    rig.Hold(0.40f, 30.0f, 0.2f);
    rig.Lifted();
    for (int i = 0; i < 4; ++i) {
        rig.Hitch(1.0f / 15.0f, 20.0f, 0.05f);
    }
    EXPECT_EQ(rig.fired, 1);
    EXPECT_FALSE(rig.flick.Lifting());
}

TEST(CutterFlick, AHitchHidingTheSnapOutIsStillTheWindUp) {
    for (const int hz : {30, 60, 90}) {
        SCOPED_TRACE(::testing::Message() << hz << " Hz");
        Rig rig;
        rig.dt = 1.0f / hz;
        rig.Hold(0.40f, 30.0f, 0.2f);
        rig.Lifted();
        rig.dir = Down;
        rig.Hitch(2.0f / 30.0f, 900.0f, 0.6f); // the crack thrown out inside the long frame
        EXPECT_FALSE(rig.flick.Lifting()) << "the crack free at once";
        rig.dir = Lift;
        rig.Hold(0.3f, 20.0f, 0.05f);
        EXPECT_EQ(rig.fired, 0);
        EXPECT_EQ(rig.dropped, 1);
        EXPECT_NE(rig.why.find("wind-up"), std::string::npos) << rig.why;
    }
}

TEST(CutterFlick, LosingTheHunterOrAStrokeWhileTheRecallWaitsSaysWhy) {
    for (const int hz : {30, 60, 90}) {
        SCOPED_TRACE(::testing::Message() << hz << " Hz");
        {
            Rig rig;
            rig.dt = 1.0f / hz;
            rig.Hold(0.40f, 30.0f, 0.2f);
            rig.Lifted();
            rig.valid = false; // the hunter's frame lost (or his turn too quick to take out)
            rig.Frame(20.0f, 0.05f);
            rig.valid = true;
            rig.Hold(0.3f, 20.0f, 0.05f);
            EXPECT_EQ(rig.fired, 0);
            EXPECT_EQ(rig.dropped, 1);
            EXPECT_NE(rig.why.find("hunter"), std::string::npos) << rig.why;
        }
        {
            // The arm starting a new stroke, the wrist hardly turning: no wind-up, a stroke.
            Rig rig;
            rig.dt = 1.0f / hz;
            rig.Hold(0.40f, 30.0f, 0.2f);
            rig.Lifted();
            rig.Frame(100.0f, 2.5f);
            rig.Hold(0.3f, 20.0f, 0.05f);
            EXPECT_EQ(rig.fired, 0);
            EXPECT_EQ(rig.dropped, 1);
            EXPECT_NE(rig.why.find("stroke"), std::string::npos) << rig.why;
        }
    }
}

TEST(CutterFlick, AnyDirectionKeepsTheEarlierSnap) {
    // whip_flick_recall=any: a snap thrown down recalls at once (the crack reels in too), and the
    // crack gate is never held off.
    for (const float d : {Down, Sideways, Lift}) {
        Rig rig{true};
        rig.Hold(0.40f, 30.0f, 0.2f);
        rig.Snap(750.0f, 0.08f, d, 0.05f);
        EXPECT_EQ(rig.fired, 1) << d;
        EXPECT_EQ(rig.lifting, 0) << d;
    }
}

// The crack gate and the flick fed the same wrist snap (the hook publishes Lifting with the tip's
// speed; the presenter's crack reads both): a crack cracks and never recalls, a recall recalls and
// never cracks, a wind-up then its crack cracks once.
TEST(CutterFlick, ACrackIsNoRecallAndARecallNoCrack) {
    struct Case {
        const char* what;
        float first, second; // the snaps' directions (NaN: none)
        int cracks, recalls;
    };
    const float none = std::nanf("");
    for (const Case& k : {Case{"a crack thrown down", Down, none, 1, 0},
                          Case{"a crack across", Sideways, none, 1, 0},
                          Case{"a flick back", Lift, none, 0, 1},
                          Case{"a wind-up and its crack", Lift, Down, 1, 0}}) {
        for (const int hz : {30, 60, 90}) {
            SCOPED_TRACE(::testing::Message() << k.what << ", " << hz << " Hz");
            const float dt = 1.0f / hz;
            W::WristFlick flick;
            const W::FlickConfig fc{};
            W::CrackGate gate;
            const W::CrackConfig cc{};
            W::Payout payout;
            W::PayoutConfig pc{};
            pc.hold_s = W::HoldFor(2400, 0.6f);
            payout.Step(8.0f, 4.0f, dt, pc); // the chain out
            float tip = 2.0f;                // m/s, the chain's tip
            int cracks = 0, recalls = 0;
            auto frame = [&](float deg_s, float d, float hand) {
                W::FlickInput in{};
                in.dt = dt;
                in.omega = deg_s * Deg;
                in.lift = d * in.omega;
                in.hand_speed = hand;
                in.extension = payout.Target();
                in.asked_out = payout.Out();
                const bool recall = flick.Step(in, fc);
                recalls += flick.Fired() ? 1 : 0;
                // The far end 0.9 m out: a fast snap drives; its tip surges a frame later.
                const float far = deg_s * Deg * 0.9f;
                payout.Step(far, 4.0f, dt, pc, recall);
                W::CrackInput ci{};
                ci.dt = dt;
                ci.swings = true;
                ci.lash = 1;
                ci.asked_out = payout.Out();
                ci.out = (payout.Target() - 1.0f) / 3.0f;
                ci.tip = tip;
                ci.swing_speed = far;
                ci.v_drive = 5.5f;
                ci.lifting = payout.Out() && flick.Lifting();
                cracks += gate.Step(ci, cc) ? 1 : 0;
                tip = deg_s >= 500.0f ? 22.0f : std::max(2.0f, tip * 0.6f);
            };
            auto hold = [&](float s, float deg_s, float d, float hand) {
                for (int i = 0; i < std::max(1, static_cast<int>(std::lround(s / dt))); ++i) {
                    frame(deg_s, d, hand);
                }
            };
            auto snap = [&](float d) {
                frame(375.0f, d, 0.5f);
                hold(0.08f, 750.0f, d, 1.0f);
                frame(375.0f, d, 0.5f);
            };
            hold(0.6f, 30.0f, Lift, 0.1f); // out, the tip slow: the gate free to crack
            snap(k.first);
            if (!std::isnan(k.second)) {
                snap(k.second);
            }
            hold(0.4f, 20.0f, Lift, 0.05f);
            EXPECT_EQ(cracks, k.cracks);
            EXPECT_EQ(recalls, k.recalls);
            EXPECT_EQ(payout.Out(), k.recalls == 0) << "a recall reels it in, a crack leaves it out";
        }
    }
}

TEST(CutterFlick, TheLiftOfTheWeaponsFarEnd) {
    const W::FlickDir fwd{0.0f, 0.0f, -1.0f}; // the hunter faces -Z, +X his right
    const float dt = 1.0f / 90.0f;
    const float a = 10.0f * Deg;
    const float rate = a / dt;
    auto share = [&](W::FlickDir from, W::FlickDir to) { return W::FlickLift(from, to, fwd, dt) / rate; };
    // Held forward: up lifts, down and across do not.
    const W::FlickDir ahead{0.0f, 0.0f, -1.0f};
    EXPECT_NEAR(share(ahead, {0.0f, std::sin(a), -std::cos(a)}), 1.0f, 0.02f);
    EXPECT_NEAR(share(ahead, {0.0f, -std::sin(a), -std::cos(a)}), -1.0f, 0.02f);
    EXPECT_NEAR(share(ahead, {std::sin(a), 0.0f, -std::cos(a)}), 0.0f, 0.01f);
    // Held forward and down (the chain lying ahead): raising it lifts.
    const float p = -30.0f * Deg;
    const W::FlickDir low{0.0f, std::sin(p), -std::cos(p)};
    EXPECT_NEAR(share(low, {0.0f, std::sin(p + a), -std::cos(p + a)}), 1.0f, 0.02f);
    EXPECT_NEAR(share(low, {0.0f, std::sin(p - a), -std::cos(p - a)}), -1.0f, 0.02f);
    // Held upright: back over the shoulder lifts, forward does not.
    const W::FlickDir up{0.0f, 1.0f, 0.0f};
    EXPECT_NEAR(share(up, {0.0f, std::cos(a), std::sin(a)}), 1.0f, 0.02f);
    EXPECT_NEAR(share(up, {0.0f, std::cos(a), -std::sin(a)}), -1.0f, 0.02f);
    // Held out to the right: raising it is mostly a lift, swinging it back only partly, forward
    // not.
    const W::FlickDir right{1.0f, 0.0f, 0.0f};
    EXPECT_GT(share(right, {std::cos(a), std::sin(a), 0.0f}), 0.85f);
    EXPECT_LT(share(right, {std::cos(a), 0.0f, std::sin(a)}), 0.5f);
    EXPECT_LT(share(right, {std::cos(a), 0.0f, -std::sin(a)}), 0.0f);
    // Pointing up and back already: nothing left to lift toward; no time: nothing.
    const float n = std::sqrt(1.25f);
    EXPECT_EQ(W::FlickLift({0.0f, 1.0f / n, 0.5f / n}, up, fwd, dt), 0.0f);
    EXPECT_EQ(W::FlickLift(ahead, up, fwd, 0.0f), 0.0f);
}

TEST(CutterFlick, RaisingTheWeaponLiftsAtEveryPitchAndDippingNever) {
    // The review's case: aimed steeply down (after an overhead slam, at the chain on the floor),
    // a fixed "up and back" stood straight against the weapon about 63 degrees down and ahead:
    // nothing read as a lift from 55 to 70 degrees down, and a dip read as one below that.
    const W::FlickDir fwd{0.0f, 0.0f, -1.0f};
    const float dt = 1.0f / 90.0f;
    const float a = 5.0f * Deg;
    const float rate = a / dt;
    for (const float yaw_deg : {0.0f, 60.0f, -60.0f}) {
        const float cy = std::cos(yaw_deg * Deg), sy = std::sin(yaw_deg * Deg);
        // The weapon at `p` (rad) above horizontal, ahead of the hunter turned `yaw_deg` right.
        auto at = [&](float p) {
            return W::FlickDir{sy * std::cos(p), std::sin(p), -cy * std::cos(p)};
        };
        for (int p = -75; p <= 80; p += 5) {
            SCOPED_TRACE(::testing::Message() << "yaw " << yaw_deg << ", pitch " << p);
            const float t = static_cast<float>(p) * Deg;
            const float raise = W::FlickLift(at(t), at(t + a), fwd, dt) / rate;
            const float dip = W::FlickLift(at(t), at(t - a), fwd, dt) / rate;
            EXPECT_GT(raise, 0.5f);
            EXPECT_LT(dip, 0.0f);
            if (yaw_deg == 0.0f) {
                EXPECT_NEAR(raise, 1.0f, 0.02f);
                EXPECT_NEAR(dip, -1.0f, 0.02f);
            }
        }
        // Within about 11 degrees of straight down every way lifts the far end: no direction.
        EXPECT_EQ(W::FlickLift(at(-85.0f * Deg), at(-80.0f * Deg), fwd, dt), 0.0f);
        EXPECT_EQ(W::FlickLift(at(-85.0f * Deg), at(-90.0f * Deg), fwd, dt), 0.0f);
    }
}

// The chain on the hand (core/vr/whip_chain.h, the Beast Cutter's config) just thrown out ahead
// and in the air (`aim` a little up), or lying on the floor (aimed steeply down), and a wrist snap
// read the way the hook reads it (the grip's turn and speed, the far end's lift from the weapon's
// axis), with the presenter's crack fed the chain's own tip.
struct ChainSnap {
    int cracks{0};      // the crack gate as wired (held off while the flick lifts)
    int bare_cracks{0}; // ... never held off
    std::uint32_t recalls{0};
    bool out{false}; // the payout still asks the chain out at the end
    float peak_tip{0.0f};
};

// `back`: right after the turn the wrist turns back this share of it, over `back_T` s.
ChainSnap SnapOnTheChain(float hz, B::Vec3 axis, float angle, float t0, float T,
                         const W::FlickConfig& fc, B::Vec3 aim = {0.0f, 0.3f, -1.0f},
                         float back = 0.0f, float back_T = 0.1f) {
    constexpr int n = 26;
    // A plain 26-point line, 3.4 cm links, along the part's -Y (the weapon's axis).
    std::array<B::Vec3, n> rest{};
    for (int i = 0; i < n; ++i) {
        rest[i] = {0.0f, -0.2f - 0.034f * static_cast<float>(i), 0.0f};
    }
    const B::Vec3 stick = B::Normalize(rest[n - 1] - rest[0]);
    const B::Vec3 fwd{0.0f, 0.0f, -1.0f}; // the hunter faces -Z, his feet at y 0
    aim = B::Normalize(aim);
    const float dt = 1.0f / hz;
    W::Chain chain;
    EXPECT_TRUE(chain.Init(rest.data(), n));
    auto cfg = W::ConfigFor(2400);
    cfg.floor_y = 0.03f;
    const W::Frame hand0{{0.2f, 1.3f, -0.35f}, B::FromTo(stick, aim)};
    std::array<B::Vec3, n> out{};
    for (int i = 0; i < n; ++i) {
        out[i] = W::Apply(hand0, rest[0] + (rest[i] - rest[0]) * 4.0f);
        out[i].y = std::max(out[i].y, cfg.floor_y);
    }
    chain.ResetTo(out.data(), 4.0f); // thrown out to x4, falling (or lying on the floor)
    W::Payout payout;
    W::PayoutConfig pc{};
    pc.hold_s = W::HoldFor(2400, 0.6f);
    payout.Step(8.0f, 4.0f, dt, pc);
    W::WristFlick flick;
    W::CrackGate gate, bare;
    const W::CrackConfig cc{};
    // The wrist's pivot 8 cm behind the grip; a minimum-jerk turn of `angle` over `T` from `t0`.
    const B::Vec3 pivot = hand0.t - aim * WristToGripM;
    auto ease = [](float u) {
        u = std::clamp(u, 0.0f, 1.0f);
        return u * u * u * (10.0f + u * (-15.0f + 6.0f * u));
    };
    auto at = [&](float t) {
        const float e = ease((t - t0) / T) - back * ease((t - t0 - T) / back_T);
        const B::Quat r = B::AxisAngle(B::Normalize(axis), angle * e);
        return W::Frame{pivot + B::Rotate(r, hand0.t - pivot), B::Normalize(r * hand0.q)};
    };
    ChainSnap res;
    W::Frame prev = hand0;
    for (float t = dt; t < t0 + 0.8f; t += dt) {
        const W::Frame f = at(t);
        W::FlickInput in{};
        in.dt = dt;
        in.omega = B::AngleBetween(prev.q, f.q) / dt;
        in.hand_speed = B::Length(f.t - prev.t) / dt;
        const B::Vec3 a0 = B::Rotate(prev.q, stick), a1 = B::Rotate(f.q, stick);
        in.lift =
            W::FlickLift({a0.x, a0.y, a0.z}, {a1.x, a1.y, a1.z}, {fwd.x, fwd.y, fwd.z}, dt);
        in.extension = chain.Extension();
        in.asked_out = payout.Out();
        const bool recall = flick.Step(in, fc);
        // The presenter's swing speed: the weapon's far end (0.8 m out).
        const float speed =
            B::Length(W::Apply(f, stick * 0.8f) - W::Apply(prev, stick * 0.8f)) / dt;
        const float target = payout.Step(speed, 4.0f, dt, pc, recall);
        auto now = cfg;
        now.release = W::ReleaseForSpeed(speed, pc.v_keep, cfg);
        now.side_damp = W::SideDampForSpeed(speed, pc.v_keep, cfg);
        EXPECT_GT(chain.Step(prev, f, dt, target, now), 0);
        W::CrackInput ci{};
        ci.dt = dt;
        ci.swings = true;
        ci.lash = 1;
        ci.asked_out = payout.Out();
        ci.out = (chain.Extension() - 1.0f) / 3.0f;
        ci.tip = chain.TipSpeed();
        ci.swing_speed = speed;
        ci.v_drive = 5.5f;
        const W::CrackInput bi = ci;
        ci.lifting = payout.Out() && flick.Lifting(); // as the hook publishes it
        if (t < t0 - 0.05f) {
            gate = bare = {}; // the throw itself is a stroke's window, not this snap's
        } else {
            res.cracks += gate.Step(ci, cc) ? 1 : 0;
            res.bare_cracks += bare.Step(bi, cc) ? 1 : 0;
            res.peak_tip = std::max(res.peak_tip, chain.TipSpeed());
        }
        prev = f;
    }
    res.recalls = flick.Flicks();
    res.out = payout.Out();
    return res;
}

TEST(CutterFlick, OnTheChainACrackCracksAndAFlickBackRecalls) {
    const B::Vec3 pitch{1.0f, 0.0f, 0.0f}, yaw{0.0f, 1.0f, 0.0f};
    for (const float hz : {30.0f, 60.0f, 90.0f}) {
        SCOPED_TRACE(::testing::Message() << hz << " Hz");
        // 0.3 s after the throw, a 50 degree snap in 0.1 s: down (the crack) ...
        const ChainSnap down = SnapOnTheChain(hz, pitch, -50.0f * Deg, 0.3f, 0.1f, {});
        ASSERT_GE(down.peak_tip, W::CrackConfig{}.tip) << "the snap whips the tip to a crack";
        EXPECT_EQ(down.cracks, 1);
        EXPECT_EQ(down.recalls, 0u);
        EXPECT_TRUE(down.out) << "the crack leaves the chain out";
        // ... which the any-direction snap (whip_flick_recall=any) reeled in.
        const ChainSnap before =
            SnapOnTheChain(hz, pitch, -50.0f * Deg, 0.3f, 0.1f, W::AnyDirectionFlick());
        EXPECT_EQ(before.recalls, 1u);
        // Across: no recall either.
        const ChainSnap across = SnapOnTheChain(hz, yaw, 50.0f * Deg, 0.3f, 0.1f, {});
        EXPECT_EQ(across.recalls, 0u);
        EXPECT_TRUE(across.out);
        // Back (the far end lifting): the recall, no crack.
        const ChainSnap back = SnapOnTheChain(hz, pitch, 50.0f * Deg, 0.3f, 0.1f, {});
        EXPECT_EQ(back.recalls, 1u);
        EXPECT_EQ(back.cracks, 0);
        EXPECT_FALSE(back.out);
        // A wider, slower flick back later on: its tip surges while the recall confirms, and the
        // crack is held off (never held off, it cracks too: the scenario's evidence, printed).
        const ChainSnap late = SnapOnTheChain(hz, pitch, 60.0f * Deg, 0.6f, 0.14f, {});
        EXPECT_EQ(late.recalls, 1u);
        EXPECT_EQ(late.cracks, 0);
        EXPECT_FALSE(late.out);
        std::printf("  cutter wrist snaps %.0f Hz: down tip %.1f m/s, %d crack(s), %u recall(s) "
                    "(any direction: %d, %u); across %u recall(s); back %u recall(s), %d "
                    "crack(s); late back tip %.1f m/s, %u recall(s), %d crack(s) (not held off: "
                    "%d)\n",
                    hz, down.peak_tip, down.cracks, down.recalls, before.cracks, before.recalls,
                    across.recalls, back.recalls, back.cracks, late.peak_tip, late.recalls,
                    late.cracks, late.bare_cracks);
    }
}

TEST(CutterFlick, OnTheChainTheWristEasingBackKeepsTheRecall) {
    // A 45 degree flick back in 0.1 s, then the wrist straight back 40 degrees over 0.10 / 0.13 s
    // (a snap's speed, slower than the flick): the recall, no crack. Any snap out letting the
    // lift go (windup 0) lost these.
    const B::Vec3 pitch{1.0f, 0.0f, 0.0f};
    W::FlickConfig strict{};
    strict.windup = 0.0f;
    for (const float hz : {30.0f, 60.0f, 90.0f}) {
        for (const float down : {16.7f, -30.0f}) {
            for (const float back_T : {0.10f, 0.13f}) {
                SCOPED_TRACE(::testing::Message() << hz << " Hz, aimed " << down
                                                  << " deg, back over " << back_T << " s");
                const B::Vec3 aim{0.0f, std::sin(down * Deg), -std::cos(down * Deg)};
                const ChainSnap r = SnapOnTheChain(hz, pitch, 45.0f * Deg, 0.4f, 0.1f, {}, aim,
                                                   40.0f / 45.0f, back_T);
                EXPECT_EQ(r.recalls, 1u);
                EXPECT_EQ(r.cracks, 0);
                EXPECT_FALSE(r.out);
                const ChainSnap s = SnapOnTheChain(hz, pitch, 45.0f * Deg, 0.4f, 0.1f, strict,
                                                   aim, 40.0f / 45.0f, back_T);
                std::printf("  flick back + ease back %.2f s, %.0f Hz, aimed %.0f: %u recall(s), "
                            "%d crack(s); any snap out a wind-up: %u recall(s), %d crack(s)\n",
                            back_T, hz, down, r.recalls, r.cracks, s.recalls, s.cracks);
            }
        }
    }
}

TEST(CutterFlick, AimedSteeplyDownAFlickBackRecallsAndADipNever) {
    // The weapon aimed at the chain lying on the floor ahead (after an overhead slam, or the arm
    // relaxed), 0.4 s after the throw, a 45 degree snap in 0.1 s: raising it is the flick back,
    // dipping it is a crack thrown down. A fixed "up and back" missed the first and took the
    // second for a lift from about 55 degrees down.
    const B::Vec3 pitch{1.0f, 0.0f, 0.0f};
    for (const float hz : {30.0f, 60.0f, 90.0f}) {
        for (const float down : {-45.0f, -55.0f, -65.0f, -70.0f, -75.0f}) {
            SCOPED_TRACE(::testing::Message() << hz << " Hz, aimed " << down << " deg");
            const B::Vec3 aim{0.0f, std::sin(down * Deg), -std::cos(down * Deg)};
            const ChainSnap back =
                SnapOnTheChain(hz, pitch, 45.0f * Deg, 0.4f, 0.1f, {}, aim);
            EXPECT_EQ(back.recalls, 1u);
            EXPECT_EQ(back.cracks, 0);
            EXPECT_FALSE(back.out);
            const ChainSnap dip = SnapOnTheChain(hz, pitch, -45.0f * Deg, 0.4f, 0.1f, {}, aim);
            EXPECT_EQ(dip.recalls, 0u);
            EXPECT_TRUE(dip.out) << "a crack thrown down leaves the chain out";
            EXPECT_EQ(dip.cracks, dip.bare_cracks) << "and is never held off";
            if (down == -55.0f) {
                EXPECT_EQ(dip.cracks, 1) << "the dip off the floor whips the tip to a crack";
            }
        }
    }
}

} // namespace
