// SPDX-License-Identifier: GPL-2.0-or-later

// The injection's drain (src/core/vr/vial_drain.h): the fill curve (full to empty, a plunger),
// its timing from the start (and from the published word, as the renderer reads it), how long the
// vial must stay in the hand, the resets; and the held decision's drained vial (held_item.h
// FlagDrain: ours only when shown).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <gtest/gtest.h>
#include "core/vr/held_item.h"
#include "core/vr/vial_drain.h"

namespace {

using namespace VR::Held;

constexpr std::int64_t Ms = 1'000'000;

TEST(VialDrain, FillCurveIsAPlungerFromFullToEmpty) {
    EXPECT_FLOAT_EQ(DrainFill(0.0f), 1.0f);
    EXPECT_FLOAT_EQ(DrainFill(1.0f), 0.0f);
    EXPECT_FLOAT_EQ(DrainFill(-0.5f), 1.0f); // before the start: full
    EXPECT_FLOAT_EQ(DrainFill(2.0f), 0.0f);  // after the end: empty
    EXPECT_NEAR(DrainFill(180.0f / 420.0f), 0.5f, 1e-6f);
    EXPECT_NEAR(DrainFill(220.0f / 420.0f), 0.3742138f, 1e-5f); // pause ends: about 63% gone
    // Monotonic: accelerate, pour steadily, then slow down smoothly to empty.
    constexpr int N = 100;
    float last = 1.0f;
    float last_drop = 0.0f;
    float pour = -1.0f;
    for (int i = 1; i <= N; ++i) {
        const float t = static_cast<float>(i) / N;
        const float f = DrainFill(t);
        EXPECT_LT(f, last) << t;
        const float drop = last - f;
        if (t <= DrainRampIn + 1e-4f) {
            EXPECT_GT(drop, last_drop - 1e-6f) << t;
        } else if (t > DrainRampIn + 1.0f / N + 1e-4f && t <= 1.0f - DrainRampOut + 1e-4f) {
            if (pour < 0.0f) {
                pour = drop;
            }
            EXPECT_NEAR(drop, pour, 1e-4f) << t; // the steady pour
        } else if (t > 1.0f - DrainRampOut + 1.0f / N + 1e-4f) {
            EXPECT_LT(drop, last_drop + 1e-6f) << t;
        }
        last_drop = drop;
        last = f;
    }
    EXPECT_NEAR(pour, 1.0f / (1.0f - 0.5f * (DrainRampIn + DrainRampOut)) / N, 1e-4f);
    // The first frame at 60 fps (16.7 ms of 420) barely moves it: no jump at the THUMP; it is
    // seen falling within the first 4 frames (67 ms) and never stops before the end.
    EXPECT_GT(DrainFill(16.7f / 420.0f), 0.985f);
    EXPECT_LT(DrainFill(67.0f / 420.0f), 0.93f);
    // The last frame before the end is almost empty (it settles on empty, no flash).
    EXPECT_LT(DrainFill(1.0f - 16.7f / 420.0f), 0.01f);
    EXPECT_GT(DrainFill(1.0f - 16.7f / 420.0f), 0.0f);
}

TEST(VialDrain, TimingFromTheStart) {
    VialDrain d;
    EXPECT_FALSE(d.On());
    EXPECT_FLOAT_EQ(d.Fill(123 * Ms), 1.0f); // none: full
    EXPECT_FLOAT_EQ(d.VisibleS(0), 0.0f);
    const std::int64_t t0 = 5'000 * Ms;
    ASSERT_TRUE(d.Start(t0, 420)); // the default (vial_inject_drain_ms)
    EXPECT_TRUE(d.On());
    EXPECT_EQ(d.DrainMs(), 420);
    EXPECT_FLOAT_EQ(d.Fill(t0), 1.0f);
    EXPECT_NEAR(d.Fill(t0 + 180 * Ms), 0.5f, 1e-5f); // half at 180 ms
    EXPECT_NEAR(d.Fill(t0 + 42 * Ms), 1.0f - 7.0f / 106.0f, 1e-4f); // at speed
    EXPECT_FALSE(d.Empty(t0 + 419 * Ms));
    EXPECT_TRUE(d.Empty(t0 + 420 * Ms));
    EXPECT_FLOAT_EQ(d.Fill(t0 + 420 * Ms), 0.0f);
    EXPECT_FLOAT_EQ(d.Fill(t0 + 900 * Ms), 0.0f); // stays empty until put away
    // Shown for the drain and the empty vial's hold (the default, 200 ms) from the start: gone at
    // 620 ms.
    EXPECT_EQ(DefaultEmptyHoldMs, 200);
    EXPECT_EQ(d.EmptyHoldMs(), 200);
    EXPECT_NEAR(d.VisibleS(t0), 0.620f, 1e-6f);
    EXPECT_NEAR(d.VisibleS(t0 + 400 * Ms), 0.220f, 1e-6f);
    EXPECT_FLOAT_EQ(d.VisibleS(t0 + 2'000 * Ms), 0.0f);
}

// vial_inject_empty_ms: the empty vial stays its hold after the drain reaches empty, then the
// vial is put away (VisibleS reaches 0): 200 ms (the default since 2026-10-09: the user, "yes do
// it for 200ms") and 80 ms (before); the total lifetime from the THUMP.
TEST(VialDrain, TheEmptyVialStaysItsHoldThenGoes) {
    constexpr std::int64_t t0 = 3'000 * Ms;
    constexpr float Eps = 1e-6f;
    for (const std::int32_t hold : {200, 80}) {
        VialDrain d;
        ASSERT_TRUE(d.Start(t0, 420, hold));
        EXPECT_EQ(d.EmptyHoldMs(), hold);
        EXPECT_EQ(d.EmptyHoldNs(), hold * Ms);
        const std::int64_t empty_at = t0 + 420 * Ms; // the drain reaches empty
        const std::int64_t gone_at = empty_at + hold * Ms;
        // The total lifetime from the THUMP: 620 ms (200), 500 ms (80, as before).
        EXPECT_EQ(VisibleMs(420, hold), 420 + hold);
        EXPECT_NEAR(d.VisibleS(t0), static_cast<float>(420 + hold) * 1e-3f, Eps) << hold;
        // Just before empty: still draining, the whole hold still to come.
        EXPECT_FALSE(d.Empty(empty_at - 1 * Ms));
        EXPECT_GT(d.Fill(empty_at - 1 * Ms), 0.0f);
        EXPECT_NEAR(d.VisibleS(empty_at - 1 * Ms), static_cast<float>(hold + 1) * 1e-3f, Eps);
        // Empty from the drain's end, and shown for exactly the hold: the level stays 0.
        EXPECT_TRUE(d.Empty(empty_at));
        EXPECT_NEAR(d.VisibleS(empty_at), static_cast<float>(hold) * 1e-3f, Eps) << hold;
        for (std::int64_t t = empty_at; t < gone_at; t += 10 * Ms) {
            EXPECT_FLOAT_EQ(d.Fill(t), 0.0f) << (t - t0) / Ms;
            EXPECT_GT(d.VisibleS(t), 0.0f) << (t - t0) / Ms; // still in the hand
        }
        // Gone exactly at the end of the hold (and after).
        EXPECT_NEAR(d.VisibleS(gone_at - 1 * Ms), 0.001f, Eps);
        EXPECT_FLOAT_EQ(d.VisibleS(gone_at), 0.0f);
        EXPECT_FLOAT_EQ(d.VisibleS(gone_at + 100 * Ms), 0.0f);
    }
    // 200 ms keeps the empty vial 120 ms longer than before (put away at 620 ms, not 500).
    EXPECT_EQ(VisibleMs(420, 200) - VisibleMs(420, 80), 120);
    // A hold of 0: put away as the drain ends; clamped to 0 .. MaxEmptyHoldMs.
    VialDrain d;
    ASSERT_TRUE(d.Start(t0, 420, 0));
    EXPECT_FLOAT_EQ(d.VisibleS(t0 + 420 * Ms), 0.0f);
    ASSERT_TRUE(d.Start(t0, 420, -50));
    EXPECT_EQ(d.EmptyHoldMs(), 0);
    ASSERT_TRUE(d.Start(t0, 420, 99'999));
    EXPECT_EQ(d.EmptyHoldMs(), MaxEmptyHoldMs);
    EXPECT_EQ(VisibleMs(420, 99'999), 420 + MaxEmptyHoldMs);
    // No drain: nothing shown for it, whatever the hold.
    EXPECT_EQ(VisibleMs(0, 200), 0);
    EXPECT_FALSE(d.Start(t0, 0, 200));
    // vial_inject_hold_s (0.3 s) is stretched to the lifetime (the presenter's max): 0.62 s.
    EXPECT_NEAR(std::max(0.3f, static_cast<float>(VisibleMs(420, 200)) * 1e-3f), 0.62f, Eps);
    // A cancel (a new vial armed, a menu, death, a load) ends it at once, hold or not.
    ASSERT_TRUE(d.Start(t0, 420, 200));
    d.Cancel();
    EXPECT_FLOAT_EQ(d.VisibleS(t0 + 430 * Ms), 0.0f);
    EXPECT_FLOAT_EQ(d.Fill(t0 + 430 * Ms), 1.0f); // the next vial is full
}

TEST(VialDrain, OffAndResets) {
    VialDrain d;
    EXPECT_FALSE(d.Start(0, 0)); // vial_inject_drain_ms 0: nothing
    EXPECT_FALSE(d.Start(0, -5));
    EXPECT_FALSE(d.On());
    ASSERT_TRUE(d.Start(10 * Ms, 240));
    d.Cancel(); // menu, death, load, put away, re-armed
    EXPECT_FALSE(d.On());
    EXPECT_FLOAT_EQ(d.Fill(130 * Ms), 1.0f); // the next vial is full
    EXPECT_FALSE(d.Empty(1'000 * Ms));
    EXPECT_FLOAT_EQ(d.VisibleS(10 * Ms), 0.0f);
    // A new start replaces a running one (its own clock).
    ASSERT_TRUE(d.Start(100 * Ms, 240));
    ASSERT_TRUE(d.Start(200 * Ms, 300));
    EXPECT_EQ(d.DrainMs(), 300);
    EXPECT_FLOAT_EQ(d.Fill(200 * Ms), 1.0f);
    // Clamped.
    ASSERT_TRUE(d.Start(0, 999'999));
    EXPECT_EQ(d.DrainMs(), MaxDrainMs);
}

TEST(VialDrain, WordRoundTripsAndGivesTheSameLevel) {
    EXPECT_EQ(PackDrain({1234, 0}), 0u); // no drain
    EXPECT_FLOAT_EQ(DrainFillAt(0, 999 * Ms), 1.0f);
    const std::int64_t start_ms = 7'654'321;
    const std::uint64_t word = PackDrain({start_ms, 420});
    const DrainWord d = UnpackDrain(word);
    EXPECT_EQ(d.start_ms, start_ms);
    EXPECT_EQ(d.drain_ms, 420);
    // The renderer's level against the presenter's (to the millisecond).
    VialDrain p;
    ASSERT_TRUE(p.Start(start_ms * Ms, 420));
    for (std::int64_t t = 0; t <= 520; t += 7) {
        const std::int64_t now = (start_ms + t) * Ms + 400'000; // mid-millisecond
        EXPECT_NEAR(DrainFillAt(word, now), p.Fill(now), 0.01f) << t;
    }
    // Read just before the stamp (another thread): full, never a wrapped huge age.
    EXPECT_FLOAT_EQ(DrainFillAt(word, (start_ms - 1) * Ms), 1.0f);
    // Past the end: empty (until the vial is put away and the word cleared).
    EXPECT_FLOAT_EQ(DrainFillAt(word, (start_ms + 5'000) * Ms), 0.0f);
    // The empty vial's word: level 0 at any time of a running clock.
    EXPECT_NE(EmptyDrainWord, 0u);
    EXPECT_FLOAT_EQ(DrainFillAt(EmptyDrainWord, 5 * Ms), 0.0f);
    EXPECT_FLOAT_EQ(DrainFillAt(EmptyDrainWord, 123'456'789 * Ms + 400'000), 0.0f);
}

TEST(VialDrain, AVialArmedWithNoneLeftIsEmptyUntilPutAway) {
    ArmedLook look;
    bool fresh = false;
    std::uint64_t w = 0;
    const std::uint64_t drain = PackDrain({1000, 240});
    // Armed with vials: full; the stab's drain passes through.
    w = look.Word(0, 3, fresh);
    EXPECT_EQ(w, 0u);
    EXPECT_TRUE(fresh);
    w = look.Word(0, 3, fresh);
    EXPECT_EQ(w, 0u);
    EXPECT_FALSE(fresh);
    // The last vial: the count drops to 0 with the injection, its drain still plays out.
    w = look.Word(drain, 0, fresh);
    EXPECT_EQ(w, drain);
    EXPECT_FALSE(fresh);
    // A new vial armed over the drain (the word back to 0): none left, empty.
    w = look.Word(0, 0, fresh);
    EXPECT_EQ(w, EmptyDrainWord);
    EXPECT_TRUE(fresh);
    EXPECT_TRUE(look.Empty());
    // A refused stab's hold (no drain): still empty; a vial picked up meanwhile changes nothing
    // until the next one is armed.
    w = look.Word(0, 1, fresh);
    EXPECT_EQ(w, EmptyDrainWord);
    EXPECT_FALSE(fresh);
    // Put away, armed again with one carried: full.
    look.Reset();
    w = look.Word(0, 1, fresh);
    EXPECT_EQ(w, 0u);
    EXPECT_TRUE(fresh);
    // Unknown count (no heal hook): full.
    look.Reset();
    w = look.Word(0, -1, fresh);
    EXPECT_EQ(w, 0u);
    EXPECT_FALSE(look.Empty());
}

TEST(VialDrain, TheDecisionMakesTheVialOursOnlyWhenShown) {
    Inputs in{};
    in.request = {Kind::Vial, FlagDrain, 1000};
    in.age_ms = 5;
    const Policy p{};
    Decision d = Decide(in, p);
    EXPECT_TRUE(d.show);
    EXPECT_TRUE(d.ours);
    EXPECT_EQ(d.hide_left_mask, 0x01); // the gun stays hidden through the drain
    // Without the flag: the game's vial.
    in.request.flags = 0;
    d = Decide(in, p);
    EXPECT_TRUE(d.show);
    EXPECT_FALSE(d.ours);
    // Suppressed (menu, death, the gun): not ours either (nothing drawn).
    in.request.flags = FlagDrain;
    in.menu = true;
    d = Decide(in, p);
    EXPECT_FALSE(d.show);
    EXPECT_FALSE(d.ours);
    in.menu = false;
    in.hp = 0;
    EXPECT_FALSE(Decide(in, p).ours);
    in.hp = 1;
    in.request.flags = FlagDrain | FlagGun;
    EXPECT_FALSE(Decide(in, p).ours);
    // A stale request (the presenter stopped): not ours.
    in.request.flags = FlagDrain;
    in.age_ms = 1000;
    EXPECT_FALSE(Decide(in, p).ours);
    // A quick item never is.
    in.age_ms = 5;
    in.request.kind = Kind::Item;
    EXPECT_FALSE(Decide(in, p).ours);
}

} // namespace

// Each drawn vial looks a little fuller or emptier (-10% .. +5%), steady while it is held.
TEST(VialDrain, FillVariationPerVialStaysInRangeAndSteady) {
    VR::Held::FillVariation v;
    std::int64_t now = 5'000'000'000;
    float lowest = 2.0f, highest = 0.0f;
    for (int vial = 0; vial < 200; ++vial) {
        now += 1'000'000'000 + vial * 7'919'000LL; // put away and drawn again
        const float scale = v.Scale(now, 1.0f, -0.10f, 0.05f);
        EXPECT_GE(scale, 0.90f);
        EXPECT_LE(scale, 1.05f);
        lowest = std::min(lowest, scale);
        highest = std::max(highest, scale);
        for (int f = 1; f < 30; ++f) // held: the same vial keeps its look while it drains
            EXPECT_EQ(v.Scale(now + f * 11'000'000LL, 1.0f - f / 30.0f, -0.10f, 0.05f), scale);
        now += 30 * 11'000'000LL;
    }
    EXPECT_LT(lowest, 0.93f); // it really varies
    EXPECT_GT(highest, 1.02f);
}
TEST(VialDrain, FillVariationRerollsForAFreshVialAndCanBeOff) {
    VR::Held::FillVariation v;
    const std::int64_t t = 9'000'000'000;
    const float first = v.Scale(t, 1.0f, -0.10f, 0.05f);
    v.Scale(t + 10'000'000, 0.0f, -0.10f, 0.05f); // drained
    const float next = v.Scale(t + 20'000'000, 1.0f, -0.10f, 0.05f); // a fresh vial, no gap
    EXPECT_NE(first, next);
    VR::Held::FillVariation off;
    EXPECT_EQ(off.Scale(t, 1.0f, 0.0f, 0.0f), 1.0f);
}
