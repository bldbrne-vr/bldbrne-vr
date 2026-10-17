// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "core/vr/sparks.h"

namespace {

namespace K = VR::Sparks;
using V = VR::Body::Vec3;

constexpr float Dt = 1.0f / 90.0f;
constexpr V Point{0.0f, 1.0f, -0.5f};
constexpr V Up{0.0f, 1.0f, 0.0f}; // a floor's surface

int Thrown(float slide_mps, float secs) {
    K::Field f;
    float carry = 0.0f;
    int thrown = 0;
    for (int i = 0; i < static_cast<int>(secs / Dt); ++i) {
        const std::size_t before = f.Count();
        f.Scrape(carry, Point, Up, {slide_mps, 0.0f, 0.0f}, Dt);
        thrown += static_cast<int>(f.Count() - before);
        f.Step(Dt);
    }
    return thrown;
}

TEST(Sparks, ASlowScrapeThrowsNoneAFasterOneMore) {
    EXPECT_EQ(Thrown(0.1f, 2.0f), 0);
    const int one = Thrown(1.0f, 2.0f);
    const int two = Thrown(1.5f, 2.0f);
    const int three = Thrown(3.0f, 2.0f);
    EXPECT_GE(one, 226);
    EXPECT_LE(one, 228);
    EXPECT_GT(two, one);
    EXPECT_GT(three, two);
    EXPECT_GE(three, 479);
    EXPECT_LE(three, 481);
    EXPECT_GT(Thrown(0.2f, 2.0f), 0);
}

TEST(Sparks, ASlowScrapeNeverHoldsBackAnother) {
    K::Field alone;
    K::Field both;
    float a = 0.0f;
    float b = 0.0f;
    float c = 0.0f;
    int thrown_alone = 0;
    int thrown_both = 0;
    for (int i = 0; i < static_cast<int>(2.0f / Dt); ++i) {
        std::size_t before = alone.Count();
        alone.Scrape(a, Point, Up, {2.0f, 0.0f, 0.0f}, Dt);
        thrown_alone += static_cast<int>(alone.Count() - before);
        before = both.Count();
        both.Scrape(b, Point, Up, {2.0f, 0.0f, 0.0f}, Dt);
        both.Scrape(c, Point, Up, {0.05f, 0.0f, 0.0f}, Dt);
        thrown_both += static_cast<int>(both.Count() - before);
        alone.Step(Dt);
        both.Step(Dt);
    }
    EXPECT_GT(thrown_alone, 10);
    EXPECT_EQ(thrown_both, thrown_alone);
}

TEST(Sparks, TheyFlyAlongTheScrapeAndOutThenFallAndDie) {
    K::Field f;
    float carry = 0.0f;
    for (int i = 0; i < 30; ++i) {
        f.Scrape(carry, Point, Up, {3.0f, 0.0f, 0.0f}, Dt);
    }
    ASSERT_GT(f.Count(), 0u);
    float along = 0.0f;
    float out = 0.0f;
    for (std::size_t i = 0; i < f.Count(); ++i) {
        along += f[i].v.x;
        out += f[i].v.y;
        EXPECT_GT(f[i].v.y, 0.0f); // out of the surface
        EXPECT_GE(f[i].life, 0.225f);
        EXPECT_LE(f[i].life, 0.675f);
    }
    EXPECT_GT(along, 0.0f); // with the scrape
    EXPECT_GT(out, 0.0f);
    const float y0 = f[0].p.y;
    for (int i = 0; i < 9; ++i) {
        f.Step(Dt);
    }
    if (f.Count() > 0) {
        EXPECT_LT(f[0].v.y, 3.0f); // gravity
    }
    for (int i = 0; i < 63; ++i) {
        f.Step(Dt);
    }
    EXPECT_EQ(f.Count(), 0u);
    (void)y0;
}

TEST(Sparks, AStrikeSpraysByItsSpeedAndThePoolIsBounded) {
    K::Field soft;
    soft.Strike(Point, Up, {}, 0.5f);
    EXPECT_EQ(soft.Count(), 2u);
    K::Field hard;
    hard.Strike(Point, Up, {}, 6.0f);
    EXPECT_EQ(hard.Count(), 8u);
    K::Field full;
    for (std::size_t i = 0; i < K::MaxSparks / 8 + 2; ++i) {
        full.Strike(Point, Up, {}, 6.0f);
    }
    EXPECT_EQ(full.Count(), K::MaxSparks);
}

TEST(Sparks, ASparkCoolsFromWhiteToRedAndFades) {
    const K::Color hot = K::Heat(1.0f);
    const K::Color warm = K::Heat(0.5f);
    const K::Color cool = K::Heat(0.1f);
    EXPECT_GT(hot.b, 0.7f); // white-yellow
    EXPECT_GT(hot.g, 0.9f);
    EXPECT_LT(warm.b, 0.05f); // orange
    EXPECT_GT(warm.g, 0.5f);
    EXPECT_LT(cool.g, 0.4f); // dull red
    EXPECT_GT(hot.a, cool.a);
    EXPECT_EQ(K::Heat(0.0f).a, 0.0f);
}

TEST(Sparks, ScrapingStopsEmittingWhenTheWeaponsStop) {
    K::Field f;
    float carry = 0.9f;
    f.Scrape(carry, Point, Up, {}, Dt);
    EXPECT_EQ(f.Count(), 0u);
    EXPECT_EQ(carry, 0.0f);
    for (int i = 0; i < 180; ++i) {
        f.Scrape(carry, Point, Up, {2.0f, 0.0f, 0.0f}, Dt);
        f.Step(Dt);
        EXPECT_LE(f.Count(), K::MaxSparks);
    }
    EXPECT_GT(f.Count(), 15u);
    f.Step(0.7f);
    EXPECT_EQ(f.Count(), 0u);
}

} // namespace
