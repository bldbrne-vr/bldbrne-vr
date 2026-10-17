// SPDX-License-Identifier: GPL-2.0-or-later

// The weapon against the walls (core/vr/weapon_wall.h) at 60 updates a second: a blade driven into
// a wall strikes it once; one sliding along it scrapes, a cinder at once and then every 80 ms, its
// feel by the speed; resting or creeping against it is silent; a short gap in the contact is the
// same contact, its scrape going on; a blade rattling on a wall strikes once, one bouncing off it
// at most every 0.25 s; a jump is no contact.

#include <gtest/gtest.h>

#include "core/vr/weapon_wall.h"

namespace {

namespace W = VR::WeaponWall;
using V = VR::Body::Vec3;

constexpr float Dt = 1.0f / 60.0f;
constexpr V Wall{0.0f, 0.0f, 1.0f}; // the surface's normal: the wall faces +z

W::Sample Touch(V v, bool contact = true) {
    return {Dt, contact, Wall, v};
}

TEST(WeaponWall, ABladeDrivenIntoAWallStrikesItOnce) {
    W::Contact c;
    EXPECT_FALSE(c.Step(Touch({}, false)).strike);
    const W::Out hit = c.Step(Touch({0.0f, 0.0f, -2.5f})); // 2.5 m/s into it
    EXPECT_TRUE(hit.strike);
    EXPECT_NEAR(hit.strike_speed, 2.5f, 1e-4f);
    EXPECT_FALSE(hit.scraping);
    for (int i = 0; i < 10; ++i) { // pushed on through it: still the one strike
        EXPECT_FALSE(c.Step(Touch({0.0f, 0.0f, -2.0f})).strike);
    }
    // A slow touch: no strike.
    W::Contact slow;
    EXPECT_FALSE(slow.Step(Touch({0.0f, 0.0f, -0.8f})).strike);
}

TEST(WeaponWall, ASlideAlongTheWallScrapesWithCinders) {
    W::Contact c;
    int cinders = 0;
    float speed = 0.0f;
    for (int i = 0; i < 60; ++i) { // 1 s along the wall at 1.5 m/s
        const W::Out o = c.Step(Touch({1.5f, 0.0f, 0.0f}));
        EXPECT_TRUE(o.scraping);
        EXPECT_FALSE(o.strike); // along it, not into it
        speed = o.scrape_speed;
        cinders += o.cinder ? 1 : 0;
        if (i == 0) {
            EXPECT_TRUE(o.cinder); // the first at once
        }
    }
    EXPECT_NEAR(speed, 1.5f, 1e-4f);
    EXPECT_GE(cinders, 12); // every 80 ms
    EXPECT_LE(cinders, 14);
    EXPECT_EQ(W::ScrapeLevel(W::ScrapeSpeed), 0.0f);
    EXPECT_EQ(W::ScrapeLevel(W::ScrapeFullSpeed + 1.0f), 1.0f);
    EXPECT_GT(W::ScrapeLevel(1.5f), 0.4f);
}

TEST(WeaponWall, RestingOrCreepingAgainstAWallIsSilent) {
    W::Contact c;
    for (int i = 0; i < 60; ++i) {
        const W::Out o = c.Step(Touch({0.1f, 0.05f, -0.1f}));
        EXPECT_FALSE(o.strike);
        EXPECT_FALSE(o.scraping);
        EXPECT_FALSE(o.cinder);
    }
}

TEST(WeaponWall, AShortGapIsTheSameContact) {
    W::Contact c;
    EXPECT_TRUE(c.Step(Touch({0.0f, 0.0f, -2.0f})).strike);
    for (int i = 0; i < 20; ++i) { // slid along it for 0.33 s: past the strike's repeat time
        c.Step(Touch({1.0f, 0.0f, 0.0f}));
    }
    for (int i = 0; i < 3; ++i) { // 50 ms of no contact (the cast grazing an edge)
        c.Step(Touch({}, false));
    }
    EXPECT_FALSE(c.Step(Touch({0.0f, 0.0f, -2.0f})).strike); // the same contact: no second strike
    for (int i = 0; i < 8; ++i) { // 130 ms away: a new contact
        c.Step(Touch({}, false));
    }
    EXPECT_TRUE(c.Step(Touch({0.0f, 0.0f, -2.0f})).strike);
}

TEST(WeaponWall, AShortGapKeepsTheScrapeGoing) {
    W::Contact c;
    for (int i = 0; i < 10; ++i) {
        c.Step(Touch({1.5f, 0.0f, 0.0f}));
    }
    for (int i = 0; i < 3; ++i) { // 50 ms over a seam: still scraping, at its speed, no cinder
        const W::Out o = c.Step(Touch({}, false));
        EXPECT_TRUE(o.scraping);
        EXPECT_NEAR(o.scrape_speed, 1.5f, 1e-4f);
        EXPECT_FALSE(o.cinder);
    }
    EXPECT_TRUE(c.Step(Touch({1.5f, 0.0f, 0.0f})).scraping); // and on along the wall
    for (int i = 0; i < 4; ++i) { // 67 ms off it: over
        c.Step(Touch({}, false));
    }
    const W::Out off = c.Step(Touch({}, false));
    EXPECT_FALSE(off.scraping);
    EXPECT_EQ(off.scrape_speed, 0.0f);
}

TEST(WeaponWall, ABladeRattlingOnAWallStrikesOnce) {
    W::Contact c;
    int strikes = 0;
    for (int i = 0; i < 12; ++i) { // in and out every other update for 0.2 s: one contact
        strikes += c.Step(Touch({0.0f, 0.0f, -2.0f}, i % 2 == 0)).strike ? 1 : 0;
    }
    EXPECT_EQ(strikes, 1);
    W::Contact bounce;
    strikes = 0;
    for (int i = 0; i < 60; ++i) { // bouncing off it every 133 ms for 1 s: new contacts
        strikes += bounce.Step(Touch({0.0f, 0.0f, -2.0f}, i % 8 == 0)).strike ? 1 : 0;
    }
    EXPECT_EQ(strikes, 4); // at 0, 0.27, 0.53 and 0.8 s: never two within 0.25 s
}

TEST(WeaponWall, AJumpIsNoContact) {
    W::Contact c;
    const W::Out o = c.Step(Touch({0.0f, 0.0f, -60.0f})); // a recenter's 1 m in one update
    EXPECT_FALSE(o.strike);
    EXPECT_FALSE(o.scraping);
    EXPECT_TRUE(c.Step(Touch({0.0f, 0.0f, -2.0f})).strike); // the blade's own touch right after
    W::Contact hard;
    EXPECT_TRUE(hard.Step(Touch({0.0f, 0.0f, -20.0f})).strike); // the hardest swing: a strike
}

TEST(WeaponWall, GentleBladeRubbingScrapesButRestingDoesNot) {
    W::Contact c;
    for (int i = 0; i < 90; ++i) {
        const W::Out o = c.Step(Touch({0.2f, 0.0f, 0.0f}));
        EXPECT_TRUE(o.scraping);
        EXPECT_FALSE(o.strike);
    }
    EXPECT_FALSE(c.Step(Touch({})).scraping);
}

} // namespace
