// SPDX-License-Identifier: GPL-2.0-or-later

// Blade sounds (core/vr/blade_sounds.h) at 90 updates a second: two weapons meeting tap; a hit
// glancing along the blade scrapes at once; a blade drawn along the other scrapes once it has slid
// 2.5 cm; one long draw is one stroke, back and forth a stroke each way, a draw again after a
// pause another, a draw right after a tap still sounds; a blade resting or jiggling on the other
// is silent; a short gap in the contact is the same contact; no sound twice in a row from a pool;
// the vr.ini pools.

#include <vector>

#include <gtest/gtest.h>

#include "core/vr/blade_sounds.h"

namespace {

namespace S = VR::BladeSound;
using V = VR::Body::Vec3;

constexpr float Dt = 1.0f / 90.0f;
constexpr V Up{0.0f, 1.0f, 0.0f}; // the normal between them: the right blade above the left

S::Sample Touch(V v, bool contact = true) {
    return {Dt, contact, Up, v};
}

struct Blades {
    S::Director d{};
    S::Settings set{};
    bool grinding{false}; // the grind loop plays: strokes ring once each
    std::vector<S::Out> played{};
    float grind{0.0f}; // the last update's grind level

    // `n` updates of `s`; the sounds they made.
    int Steps(const S::Sample& s, int n) {
        int count = 0;
        for (int i = 0; i < n; ++i) {
            const S::Out o = d.Step(s, set, grinding);
            grind = o.grind;
            if (o.id >= 0) {
                played.push_back(o);
                ++count;
            }
        }
        return count;
    }
};

TEST(BladeSounds, ATouchTaps) {
    Blades r;
    r.Steps(Touch({}, false), 5);
    EXPECT_EQ(r.Steps(Touch({0.0f, -0.8f, 0.0f}), 1), 1); // 0.8 m/s into it
    EXPECT_EQ(r.played.back().kind, S::Kind::Tap);
    EXPECT_NEAR(r.played.back().speed, 0.8f, 1e-4f);
    EXPECT_EQ(r.Steps(Touch({}), 30), 0); // resting on it: nothing more
    // Too slow to sound: laid on it, or brought together gently (0.45 m/s, under TapSpeed).
    Blades soft;
    EXPECT_EQ(soft.Steps(Touch({0.0f, -0.2f, 0.0f}), 20), 0);
    Blades gentle;
    gentle.Steps(Touch({}, false), 5);
    EXPECT_EQ(gentle.Steps(Touch({0.0f, -0.45f, 0.0f}), 1), 0);
}

TEST(BladeSounds, ABrushAlongTheBladeDoesNotClang) {
    // Into it fast enough for a tap, but more along it than into it (a brush at about 35 degrees):
    // no tap; the slide may scrape once it has slid.
    Blades r;
    r.Steps(Touch({}, false), 5);
    r.Steps(Touch({0.9f, -0.65f, 0.0f}), 1);
    r.Steps(Touch({0.9f, 0.0f, 0.0f}), 10);
    for (const auto& o : r.played) {
        EXPECT_NE(o.kind, S::Kind::Tap);
    }
    // The same speed into it, head-on: a tap.
    Blades hit;
    hit.Steps(Touch({}, false), 5);
    EXPECT_EQ(hit.Steps(Touch({0.5f, -0.65f, 0.0f}), 1), 1);
    EXPECT_EQ(hit.played.back().kind, S::Kind::Tap);
}

TEST(BladeSounds, AGlancingHitScrapesAtOnce) {
    Blades r;
    EXPECT_EQ(r.Steps(Touch({2.0f, -0.8f, 0.0f}), 1), 1); // fast along, a little into
    EXPECT_EQ(r.played.back().kind, S::Kind::Scrape);
    // Sliding on the same way: the same stroke.
    EXPECT_EQ(r.Steps(Touch({2.0f, 0.0f, 0.0f}), 10), 0);
}

TEST(BladeSounds, ADrawSoundsOnceItHasSlid) {
    Blades r;
    r.Steps(Touch({0.0f, -0.1f, 0.0f}), 1); // laid on it, silent
    // 0.5 m/s along: 2.5 cm takes 50 ms (4.5 updates).
    int at = -1;
    for (int i = 0; i < 20 && at < 0; ++i) {
        if (r.Steps(Touch({0.5f, 0.0f, 0.0f}), 1) == 1) {
            at = i;
        }
    }
    ASSERT_GE(at, 3);
    EXPECT_LE(at, 6);
    EXPECT_EQ(r.played.back().kind, S::Kind::Scrape);
    // One draw on and on (turning a little as the blade turns): one stroke, ringing again once
    // ScrapeRepeatS (and 10 cm) after its first ring: 0.67 s more -> one more ring.
    EXPECT_EQ(r.Steps(Touch({0.5f, 0.0f, 0.2f}), 60), 1);
}

TEST(BladeSounds, ALongScrapeRingsOnWhileItSlides) {
    Blades r;
    r.Steps(Touch({}, true), 1);
    // 1 s at 0.6 m/s one way: the first ring at ~2.5 cm, then every ScrapeRepeatS.
    const int rings = r.Steps(Touch({0.6f, 0.0f, 0.0f}), 90);
    EXPECT_GE(rings, 3);
    EXPECT_LE(rings, 4);
    for (const auto& o : r.played) {
        EXPECT_EQ(o.kind, S::Kind::Scrape);
    }
    // A creep (5 cm in 0.5 s) is no new ring.
    Blades slow;
    slow.Steps(Touch({}, true), 1);
    slow.Steps(Touch({0.3f, 0.0f, 0.0f}), 20); // its first ring
    EXPECT_EQ(slow.Steps(Touch({0.21f, 0.0f, 0.0f}), 20), 0);
}

TEST(BladeSounds, StruckAgainWhileStillTouchingTaps) {
    Blades r;
    EXPECT_EQ(r.Steps(Touch({0.0f, -1.0f, 0.0f}), 1), 1); // the tap
    r.Steps(Touch({}), 20); // resting against it (solid weapons stay in contact)
    EXPECT_EQ(r.Steps(Touch({0.0f, -1.0f, 0.0f}), 1), 1); // struck again: a tap
    EXPECT_EQ(r.played.back().kind, S::Kind::Tap);
    // Pressing on into it (no stop in between) is no new tap.
    EXPECT_EQ(r.Steps(Touch({0.0f, -1.0f, 0.0f}), 20), 0);
}

TEST(BladeSounds, ADrawRightAfterATapStillScrapes) {
    Blades r;
    EXPECT_EQ(r.Steps(Touch({0.0f, -1.0f, 0.0f}), 1), 1); // the tap
    // At once 1 m/s along: 2.5 cm in 25 ms, before MinGapS; it sounds as that ends.
    EXPECT_EQ(r.Steps(Touch({1.0f, 0.0f, 0.0f}), 20), 1);
    EXPECT_EQ(r.played.back().kind, S::Kind::Scrape);
}

TEST(BladeSounds, BackAndForthIsAStrokeEachWay) {
    Blades r;
    r.Steps(Touch({}, true), 1);
    // 0.6 m/s, 0.3 s each way (18 cm), six times.
    for (int k = 0; k < 6; ++k) {
        const float way = k % 2 == 0 ? 0.6f : -0.6f;
        EXPECT_EQ(r.Steps(Touch({way, 0.0f, 0.0f}), 27), 1) << "stroke " << k;
    }
    // Never the same sound twice in a row.
    for (size_t i = 1; i < r.played.size(); ++i) {
        EXPECT_NE(r.played[i].id, r.played[i - 1].id);
    }
}

TEST(BladeSounds, AFastShuffleDoesNotRattle) {
    // Reversing every 60 ms: no stroke sooner than StrokeRepeatS after the last.
    Blades r;
    r.Steps(Touch({}, true), 1);
    for (int k = 0; k < 20; ++k) {
        r.Steps(Touch({k % 2 == 0 ? 1.0f : -1.0f, 0.0f, 0.0f}), 5);
    }
    EXPECT_GE(r.played.size(), 2u);
    EXPECT_LE(r.played.size(), static_cast<size_t>(20 * 5 * Dt / S::StrokeRepeatS) + 1);
}

TEST(BladeSounds, JigglingOnTheBladeIsSilent) {
    Blades r;
    r.Steps(Touch({}, true), 1);
    // 0.4 m/s back and forth every 30 ms: never 2.5 cm one way.
    for (int k = 0; k < 60; ++k) {
        r.Steps(Touch({k % 2 == 0 ? 0.4f : -0.4f, 0.0f, 0.0f}), 3);
    }
    EXPECT_TRUE(r.played.empty());
}

TEST(BladeSounds, ADrawAgainAfterAPauseIsANewStroke) {
    Blades r;
    r.Steps(Touch({}, true), 1);
    EXPECT_EQ(r.Steps(Touch({0.6f, 0.0f, 0.0f}), 20), 1);
    EXPECT_EQ(r.Steps(Touch({}), 20), 0); // held still 0.22 s
    EXPECT_EQ(r.Steps(Touch({0.6f, 0.0f, 0.0f}), 20), 1);
}

TEST(BladeSounds, AShortGapIsTheSameContact) {
    Blades r;
    EXPECT_EQ(r.Steps(Touch({0.0f, -1.0f, 0.0f}), 1), 1); // the tap
    r.Steps(Touch({0.0f, -1.0f, 0.0f}), 20); // pressing on into it
    r.Steps(Touch({}, false), 4);             // the contact flickers 44 ms
    EXPECT_EQ(r.Steps(Touch({0.0f, -1.0f, 0.0f}), 1), 0); // the same press: no new tap
    r.Steps(Touch({}, false), 12); // 133 ms: apart
    EXPECT_EQ(r.Steps(Touch({0.0f, -1.0f, 0.0f}), 1), 1); // met again
}

TEST(BladeSounds, AJumpIsNothing) {
    Blades r;
    EXPECT_EQ(r.Steps(Touch({0.0f, -80.0f, 0.0f}), 1), 0);
    EXPECT_EQ(r.Steps(Touch({60.0f, 0.0f, 0.0f}), 5), 0);
}

TEST(BladeSounds, TheGrindFollowsTheSlide) {
    Blades r;
    r.grinding = true;
    r.Steps(Touch({}, true), 1);
    EXPECT_EQ(r.grind, 0.0f); // resting: silent
    r.Steps(Touch({0.3f, 0.0f, 0.0f}), 3);
    const float slow = r.grind;
    EXPECT_GT(slow, 0.0f);
    r.Steps(Touch({0.9f, 0.0f, 0.0f}), 3);
    EXPECT_GT(r.grind, slow); // faster: louder
    r.Steps(Touch({2.0f, 0.0f, 0.0f}), 3);
    EXPECT_FLOAT_EQ(r.grind, 1.0f);
    r.Steps(Touch({0.05f, 0.0f, 0.0f}), 1);
    EXPECT_EQ(r.grind, 0.0f); // stopped: silent at once (the mixer eases it out)
    // A slide through a 44 ms flicker grinds on; apart, it stops.
    r.Steps(Touch({0.6f, 0.0f, 0.0f}), 3);
    const float on = r.grind;
    r.Steps(Touch({}, false), 4);
    EXPECT_EQ(r.grind, on);
    r.Steps(Touch({}, false), 12);
    EXPECT_EQ(r.grind, 0.0f);
    // Struck into (a tap), pressed on into it: no grind.
    Blades t;
    t.grinding = true;
    t.Steps(Touch({0.0f, -1.0f, 0.0f}), 5);
    EXPECT_EQ(t.grind, 0.0f);
    EXPECT_EQ(S::GrindLevel(0.1f), 0.0f);
    EXPECT_FLOAT_EQ(S::GrindLevel(5.0f), 1.0f);
}

TEST(BladeSounds, WithTheGrindALongDrawRingsOnce) {
    // The grind carries a long draw: its stroke rings once (without it: on and on).
    Blades r;
    r.grinding = true;
    r.Steps(Touch({}, true), 1);
    EXPECT_EQ(r.Steps(Touch({0.6f, 0.0f, 0.0f}), 90), 1);
    EXPECT_GT(r.grind, 0.0f);
    // Back the other way: a new stroke, its shing.
    EXPECT_EQ(r.Steps(Touch({-0.6f, 0.0f, 0.0f}), 30), 1);
    EXPECT_EQ(r.played.back().kind, S::Kind::Scrape);
}

TEST(BladeSounds, TheGrindFromVrIni) {
    auto& set = S::GetSettings();
    const S::Settings keep = set;
    EXPECT_EQ(set.grind, 1);
    EXPECT_TRUE(S::ParseBladeSoundSetting("blade_grind", "2"));
    EXPECT_EQ(set.grind, 2);
    EXPECT_TRUE(S::ParseBladeSoundSetting("blade_grind", "x"));
    EXPECT_EQ(set.grind, 2); // unchanged
    EXPECT_TRUE(S::ParseBladeSoundSetting("blade_grind", "0"));
    EXPECT_EQ(set.grind, 0);
    EXPECT_TRUE(S::ParseBladeSoundSetting("blade_grind_volume", "3.5"));
    EXPECT_FLOAT_EQ(set.grind_volume, 2.0f);
    std::ostringstream out;
    S::WriteBladeSoundDefaults(out);
    EXPECT_NE(out.str().find("blade_grind=1\n"), std::string::npos);
    EXPECT_NE(out.str().find("blade_grind_volume=1\n"), std::string::npos);
    set = keep;
}

TEST(BladeSounds, PoolsFromVrIni) {
    S::Pool p = S::Pool::Of({1, 2});
    EXPECT_TRUE(S::ParsePool("935, c9044441 ,2018", p));
    ASSERT_EQ(p.n, 3);
    EXPECT_EQ(p.id[0], 935);
    EXPECT_EQ(p.id[1], 9044441);
    EXPECT_EQ(p.id[2], 2018);
    EXPECT_EQ(S::PoolText(p), "935,9044441,2018");
    // Nothing usable: the pool stays.
    EXPECT_FALSE(S::ParsePool(" , x", p));
    EXPECT_EQ(p.n, 3);
    // At most Max.
    EXPECT_TRUE(S::ParsePool("1,2,3,4,5,6,7,8", p));
    EXPECT_EQ(p.n, S::Pool::Max);
}

TEST(BladeSounds, APickerNeverRepeatsAndUsesThemAll) {
    S::Picker pick;
    const S::Pool p = S::Pool::Of({10, 20, 30});
    std::array<int, 3> seen{};
    std::int32_t last = -1;
    for (int i = 0; i < 300; ++i) {
        const std::int32_t id = pick.Pick(p, S::Kind::Scrape);
        EXPECT_NE(id, last);
        last = id;
        ++seen[static_cast<size_t>(id / 10 - 1)];
    }
    for (const int n : seen) {
        EXPECT_GT(n, 60);
    }
    // One sound: that one every time.
    const S::Pool one = S::Pool::Of({7});
    EXPECT_EQ(pick.Pick(one, S::Kind::Tap), 7);
    EXPECT_EQ(pick.Pick(one, S::Kind::Tap), 7);
    EXPECT_EQ(pick.Pick(S::Pool{}, S::Kind::Tap), -1);
}

} // namespace
