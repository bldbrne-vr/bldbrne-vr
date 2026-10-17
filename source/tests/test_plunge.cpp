// SPDX-License-Identifier: GPL-2.0-or-later

// The physical plunging attack (core/vr/plunge.h) on synthetic updates at the game's 30 Hz: a fall
// off a 3 m ledge is airborne, descending and a plunge to its landing; a step, a flat leap and a
// shallow drop are not plunges; a swing just after a deep landing still is; the attack holds to
// the landing plus its grace and never past max_hold.

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "core/vr/plunge.h"

namespace {

namespace P = VR::Melee::Plunge;

constexpr std::int64_t Ms = P::Ms;
constexpr std::int64_t Step = 33 * Ms; // the game's 30 Hz updates

struct Faller {
    P::Tracker tracker;
    P::Config cfg;
    std::int64_t t{1000 * Ms};

    const P::State& Feed(bool grounded, float y) {
        t += Step;
        return tracker.Step({true, grounded, y, t}, cfg);
    }
};

// Free fall from `top` for `drop` metres; returns the time of the landing update.
std::int64_t Fall(Faller& r, float top, float drop, std::vector<bool>* opens = nullptr) {
    r.Feed(true, top);
    const std::int64_t start = r.t;
    for (;;) {
        const float s = static_cast<float>(r.t + Step - start) * 1e-9f;
        const float y = top - 0.5f * 9.8f * s * s;
        if (y <= top - drop) {
            r.Feed(true, top - drop);
            return r.t;
        }
        r.Feed(false, y);
        if (opens != nullptr) {
            opens->push_back(r.tracker.Opens(r.t, r.cfg));
        }
    }
}

TEST(Plunge, ADeepFallIsAPlungeUntilItsLanding) {
    Faller r;
    std::vector<bool> opens;
    const std::int64_t landed = Fall(r, 10.0f, 3.0f, &opens);
    const P::State& s = r.tracker.Current();
    EXPECT_TRUE(s.landed);
    EXPECT_FALSE(s.airborne);
    EXPECT_NEAR(s.landed_drop_m, 3.0f, 1e-4f);
    EXPECT_EQ(s.airtimes, 1u);
    // Not at the first update off the ledge (a step?), but for most of the fall.
    ASSERT_GE(opens.size(), 6u);
    EXPECT_FALSE(opens.front());
    EXPECT_TRUE(opens.back());
    // The landing's depth counts for a hit just after it, and a plunge hit is stronger.
    EXPECT_NEAR(r.tracker.Drop(landed + 50 * Ms, r.cfg), 3.0f, 1e-4f);
    EXPECT_FLOAT_EQ(P::DamageFactor(3.0f, r.cfg), r.cfg.damage);
}

TEST(Plunge, TheAttackHoldsToTheLandingAndItsGrace) {
    Faller r;
    r.Feed(true, 10.0f);
    const std::int64_t start = r.t;
    std::int64_t created = 0;
    for (int i = 1; i < 30; ++i) {
        const float s = static_cast<float>(i) * 0.033f;
        r.Feed(false, 10.0f - 0.5f * 9.8f * s * s);
        if (created == 0 && r.tracker.Opens(r.t, r.cfg)) {
            created = r.t; // the swing opens mid-fall
        }
        if (created != 0) {
            EXPECT_TRUE(r.tracker.Holds(r.t, created, r.cfg)); // in the air: always held
        }
    }
    ASSERT_NE(created, 0);
    EXPECT_LT(created - start, 300 * Ms);
    r.Feed(true, 5.0f);
    const std::int64_t landed = r.t;
    EXPECT_TRUE(r.tracker.Holds(landed + 100 * Ms, created, r.cfg));
    EXPECT_FALSE(r.tracker.Holds(landed + 200 * Ms, created, r.cfg));
}

TEST(Plunge, AStepIsNotAirborne) {
    Faller r;
    r.Feed(true, 10.0f);
    r.Feed(false, 9.9f); // 33 ms off the ground, a step down
    r.Feed(false, 9.8f); // 66 ms
    EXPECT_FALSE(r.tracker.Current().airborne);
    EXPECT_FALSE(r.tracker.Opens(r.t, r.cfg));
    r.Feed(true, 9.8f);
    EXPECT_FALSE(r.tracker.Current().landed);
    EXPECT_EQ(r.tracker.Current().airtimes, 0u);
}

TEST(Plunge, AFlatLeapIsNotAPlunge) {
    // The sprint leap: the physics capsule stays flat (the Master bone hops); the landed byte off.
    Faller r;
    r.Feed(true, 10.0f);
    for (int i = 0; i < 45; ++i) {
        r.Feed(false, 10.0f);
        EXPECT_FALSE(r.tracker.Opens(r.t, r.cfg));
    }
    EXPECT_TRUE(r.tracker.Current().airborne);
    r.Feed(true, 10.0f);
    EXPECT_TRUE(r.tracker.Current().landed);
    EXPECT_FLOAT_EQ(r.tracker.Current().landed_drop_m, 0.0f);
    EXPECT_FALSE(r.tracker.Opens(r.t + 50 * Ms, r.cfg)); // no deep landing to carry
    EXPECT_FLOAT_EQ(P::DamageFactor(r.tracker.Drop(r.t, r.cfg), r.cfg), 1.0f);
}

TEST(Plunge, AShallowDropHitsWithoutTheBonus) {
    Faller r;
    const std::int64_t landed = Fall(r, 10.0f, 0.6f);
    EXPECT_NEAR(r.tracker.Drop(landed, r.cfg), 0.6f, 1e-4f);
    EXPECT_FLOAT_EQ(P::DamageFactor(0.6f, r.cfg), 1.0f);
    EXPECT_FALSE(r.tracker.Opens(landed + 50 * Ms, r.cfg)); // too shallow to carry past landing
}

TEST(Plunge, ASwingJustAfterADeepLandingStillPlunges) {
    Faller r;
    const std::int64_t landed = Fall(r, 10.0f, 3.0f);
    EXPECT_TRUE(r.tracker.Opens(landed + 100 * Ms, r.cfg));
    EXPECT_FALSE(r.tracker.Opens(landed + 200 * Ms, r.cfg));
    // Opened just after the landing: held for the grace after it.
    EXPECT_TRUE(r.tracker.Holds(landed + 120 * Ms, landed + 100 * Ms, r.cfg));
}

TEST(Plunge, NeverHeldPastMaxHold) {
    Faller r;
    r.Feed(true, 100.0f);
    float y = 100.0f;
    std::int64_t created = 0;
    for (int i = 0; i < 120; ++i) { // a 4 s fall
        y -= 0.3f;
        r.Feed(false, y);
        if (created == 0 && r.tracker.Opens(r.t, r.cfg)) {
            created = r.t;
        }
    }
    ASSERT_NE(created, 0);
    EXPECT_FALSE(r.tracker.Holds(created + r.cfg.max_hold_ns + Ms, created, r.cfg));
    EXPECT_TRUE(r.tracker.Holds(created + r.cfg.max_hold_ns - Ms, created, r.cfg));
}

// A gap in the signals mid-air (a stall) restarts the fall at the next sample: never a depth
// from height 0 (Bloodborne's world goes far below 0).
TEST(Plunge, AGapMidAirMeasuresFromTheNextSample) {
    Faller r;
    r.Feed(true, -40.0f);
    for (int i = 0; i < 5; ++i) {
        r.Feed(false, -40.0f - 0.2f * i);
    }
    r.t += Step;
    r.tracker.Step({false, true, 0.0f, r.t}, r.cfg); // the signals stale
    for (int i = 0; i < 6; ++i) {
        r.Feed(false, -41.0f - 0.15f * i);
    }
    // 0.75 m since the gap (not 41 m from height 0).
    EXPECT_NEAR(r.tracker.Current().drop_m, 0.75f, 1e-3f);
    EXPECT_FLOAT_EQ(P::DamageFactor(r.tracker.Drop(r.t, r.cfg), r.cfg), 1.0f);
}

TEST(Plunge, OffOrNoSignalNeverPlunges) {
    Faller r;
    r.cfg.enabled = false;
    std::vector<bool> opens;
    Fall(r, 10.0f, 3.0f, &opens);
    for (const bool o : opens) {
        EXPECT_FALSE(o);
    }
    EXPECT_FLOAT_EQ(P::DamageFactor(3.0f, r.cfg), 1.0f);
    // An invalid sample (the motion signals off) is the ground.
    Faller q;
    q.Feed(true, 10.0f);
    for (int i = 0; i < 10; ++i) {
        q.Feed(false, 10.0f - 0.3f * i);
    }
    EXPECT_TRUE(q.tracker.Current().airborne);
    q.t += Step;
    q.tracker.Step({false, true, 0.0f, q.t}, q.cfg);
    EXPECT_FALSE(q.tracker.Current().airborne);
    EXPECT_FALSE(q.tracker.Opens(q.t, q.cfg));
}

} // namespace
