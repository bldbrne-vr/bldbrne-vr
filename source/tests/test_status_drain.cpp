// SPDX-License-Identifier: GPL-2.0-or-later

// Status build-ups drain a little faster (src/core/vr/status_drain.h, vr.ini status_drain_pct):
// +pct % of the game's own recovery while a build-up is purely falling, never above max, never on
// the way up, never beasthood, a lost compare-exchange writes nothing.

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <gtest/gtest.h>

#include "core/vr/status_drain.h"

namespace {

using namespace VR::StatusDrain;
using Arr = std::array<s32, Types>;

constexpr s64 Ms = 1'000'000;

// (The test shim evaluates an EXPECT's arguments twice: every Run / Step is stored first.)
// A resist module: cur / max per type, the game's own recovery (RATE * dt with the fraction kept,
// as Update 0x1A5C440), and the step's compare-exchange on it.
struct Module {
    Arr cur{};
    Arr max{};
    std::array<float, Types> game_rem{};
    bool fail_cas{false};
    u32 cas_calls{0};

    void GameRecovery(s32 rate, float dt) {
        for (u32 i = 0; i < Types; ++i) {
            game_rem[i] += static_cast<float>(rate) * dt;
            const float n = std::trunc(game_rem[i]);
            game_rem[i] -= n;
            cur[i] = std::clamp(cur[i] + static_cast<s32>(n), 0, max[i]);
        }
    }
    u32 Step(State& st, const void* owner, s32 rate, float dt, s32 pct, s64 now) {
        return VR::StatusDrain::Step(st, owner, cur, max, rate, dt, pct, now,
                                     [&](u32 type, s32 expected, s32 value) {
                                         ++cas_calls;
                                         if (fail_cas || cur[type] != expected) {
                                             return false;
                                         }
                                         cur[type] = value;
                                         return true;
                                     });
    }
};

const int Owner = 0;

// `seconds` of updates at `hz`: the game's recovery, then the step (site A comes after it).
s32 Run(Module& m, State& st, float seconds, float hz, s32 pct, s64& now, s32 rate = 5,
        const void* owner = &Owner) {
    const float dt = 1.0f / hz;
    const int steps = static_cast<int>(std::lround(seconds * hz));
    s32 extra = 0;
    for (int i = 0; i < steps; ++i) {
        now += static_cast<s64>(1e9 / hz);
        m.GameRecovery(rate, dt);
        extra += static_cast<s32>(m.Step(st, owner, rate, dt, pct, now));
    }
    return extra;
}

TEST(StatusDrain, TenPercentIsHalfAPointPerSecondAtAnyRate) {
    for (const float hz : {30.0f, 60.0f, 120.0f}) {
        Module m{};
        m.max = {95, 95, 86, 165};
        m.cur = {10, 10, 10, 10}; // big build-ups
        State st{};
        s64 now = 10'000 * Ms;
        // 10 s and a little (the float sums land just under a whole point at exactly 10 s, in the
        // game's own recovery too).
        const s32 extra = Run(m, st, 10.1f, hz, 10, now);
        // +10% of 5 points/s over 10 s on each of the 4 types: 4 x 5 points.
        EXPECT_EQ(extra, 20) << hz;
        EXPECT_EQ(m.cur[0], 10 + 50 + 5) << hz;
        EXPECT_EQ(m.cur[3], 10 + 50 + 5) << hz;
        EXPECT_EQ(m.cur[2], 10 + 50 + 5) << hz;
    }
}

TEST(StatusDrain, PctZeroIsTheGamesRate) {
    Module m{};
    m.max = {95, 30, 86, 165};
    m.cur = {10, 10, 10, 10};
    State st{};
    s64 now = 0;
    const s32 extra = Run(m, st, 10.1f, 60.0f, 0, now);
    EXPECT_EQ(extra, 0);
    EXPECT_EQ(m.cur[3], 60);
    EXPECT_EQ(m.cas_calls, 0u);
}

TEST(StatusDrain, NeverAboveMaxAndNoWriteAtMax) {
    Module m{};
    m.max = {95, 30, 86, 165};
    m.cur = m.max; // nothing built up
    State st{};
    s64 now = 0;
    const s32 extra = Run(m, st, 5.0f, 60.0f, 100, now);
    EXPECT_EQ(extra, 0);
    EXPECT_EQ(m.cas_calls, 0u);
    EXPECT_TRUE(m.cur == m.max);
    // One point short of max at +100%: never past it.
    m.cur = {94, 29, 85, 164};
    st = {};
    Run(m, st, 3.0f, 60.0f, 100, now);
    EXPECT_TRUE(m.cur == m.max);
}

TEST(StatusDrain, BeasthoodIsNotAType) {
    // Types covers slow poison, rapid poison, bleed and frenzy only (the module's +0x10..+0x1C);
    // beasthood (+0x20) is never read or written.
    EXPECT_EQ(Types, 4u);
    EXPECT_STREQ(TypeName(3), "frenzy");
}

TEST(StatusDrain, ARiseHoldsTheExtraForOneSecond) {
    Module m{};
    m.max = {95, 30, 86, 165};
    m.cur = {95, 30, 86, 100}; // frenzy built up
    State st{};
    s64 now = 0;
    Run(m, st, 0.5f, 60.0f, 10, now); // seen, falling
    const s32 before = m.cur[3];
    // A hit: frenzy rises by 20.
    m.cur[3] -= 20;
    const s32 after_hit = m.cur[3];
    // For the next second only the game's 5 points / s.
    const s32 extra = Run(m, st, 0.95f, 60.0f, 10, now);
    EXPECT_EQ(extra, 0);
    EXPECT_GE(m.cur[3], after_hit + 4);
    EXPECT_LE(m.cur[3], after_hit + 5);
    // Then the extra resumes: 10 more seconds give +5.
    const s32 resumed = Run(m, st, 10.0f, 60.0f, 10, now);
    EXPECT_GE(resumed, 4);
    EXPECT_LE(resumed, 5);
    (void)before;
}

TEST(StatusDrain, ASteadySourceNeverGetsTheExtra) {
    // A source building every 0.5 s (a swamp's ticks, a brain's gaze): only the game's rate.
    Module m{};
    m.max = {95, 30, 86, 165};
    m.cur = {80, 30, 86, 165};
    State st{};
    s64 now = 0;
    s32 extra = 0;
    for (int tick = 0; tick < 20; ++tick) {
        m.cur[0] = std::max(m.cur[0] - 4, 1);
        extra += Run(m, st, 0.5f, 60.0f, 10, now);
    }
    EXPECT_EQ(extra, 0);
}

TEST(StatusDrain, LostCompareExchangeWritesNothing) {
    Module m{};
    m.max = {95, 30, 86, 165};
    m.cur = {10, 10, 10, 10};
    m.fail_cas = true;
    State st{};
    s64 now = 0;
    const s32 extra = Run(m, st, 10.1f, 60.0f, 10, now);
    EXPECT_EQ(extra, 0);
    EXPECT_GT(st.lost, 0u);
    EXPECT_EQ(m.cur[0], 60); // only the game's recovery
}

TEST(StatusDrain, DtIsClamped) {
    Module m{};
    m.max = {95, 30, 86, 165};
    m.cur = {10, 10, 10, 10};
    State st{};
    // A 3-second hitch counts as MaxDt; NaN and negative as nothing.
    m.Step(st, &Owner, 5, 0.0f, 100, 0);
    const u32 hitch = m.Step(st, &Owner, 5, 3.0f, 100, 5'000 * Ms);
    EXPECT_EQ(hitch, 4u); // 4 types x floor(5 * 0.25)
    EXPECT_EQ(m.cur[0], 11);
    const u32 nan = m.Step(st, &Owner, 5, std::numeric_limits<float>::quiet_NaN(), 100, 6'000 * Ms);
    EXPECT_EQ(nan, 0u);
    const u32 negative = m.Step(st, &Owner, 5, -1.0f, 100, 7'000 * Ms);
    EXPECT_EQ(negative, 0u);
    EXPECT_EQ(m.cur[0], 11);
}

TEST(StatusDrain, ANewOwnerStartsOver) {
    Module m{};
    m.max = {95, 30, 86, 165};
    m.cur = {50, 10, 10, 10};
    State st{};
    s64 now = 0;
    Run(m, st, 0.5f, 60.0f, 10, now);
    // Another PlayerIns (a load): its first value lower than the old one's is not a rise.
    const int other = 0;
    m.cur = {20, 10, 10, 10};
    m.Step(st, &other, 5, 1.0f / 60.0f, 10, now + 16 * Ms);
    EXPECT_EQ(st.owner, &other);
    EXPECT_FALSE(st.type[0].rose);
    EXPECT_EQ(st.points, 0u);
}

TEST(StatusDrain, ExtraPerSecond) {
    EXPECT_FLOAT_EQ(ExtraPerSecond(5, 10), 0.5f);
    EXPECT_FLOAT_EQ(ExtraPerSecond(5, 0), 0.0f);
    EXPECT_FLOAT_EQ(ExtraPerSecond(5, 500), 5.0f);
    EXPECT_FLOAT_EQ(ExtraPerSecond(-5, 10), 0.0f);
}

} // namespace
