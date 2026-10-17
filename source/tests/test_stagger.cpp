// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "core/vr/daze.h"
#include "core/vr/stagger_speed.h"

namespace {

namespace S = VR::StaggerSpeed;
namespace D = VR::Daze;

// Plays one clip through the shortener at `dt` (the game's frame time): the clip's time advances
// by the scaled frame time each update; its window opens at `window_at` (clip seconds). Returns
// the real time at which the window was first seen open, or -1.
float RealTimeToWindow(S::Shortener& sh, std::int32_t id, float window_at, float dt,
                       float shorten_s, float start_real = 0.0f) {
    float clip = 0.0f;
    float real = start_real;
    for (int i = 0; i < 1000; ++i) {
        const bool window = clip >= window_at;
        if (window) {
            return real;
        }
        S::In in{.anim_id = id, .clip_time = clip, .window = false, .dt = dt,
                 .shorten_s = shorten_s};
        const float rate = sh.Step(in);
        clip += dt * rate;
        real += dt;
    }
    return -1.0f;
}

TEST(StaggerSpeed, AHeavyStaggerEnds150msSooner) {
    S::Shortener sh;
    const float dt = 1.0f / 60.0f;
    const float real = RealTimeToWindow(sh, 20203, 0.733f, dt, 0.15f);
    // The window comes 150 ms sooner (to a frame).
    EXPECT_NEAR(real, 0.733f - 0.15f, dt + 1e-4f);
    EXPECT_NEAR(sh.Gained(), 0.15f, 1e-4f);
    EXPECT_FALSE(sh.Active());
    const auto end = sh.TakeEnded();
    EXPECT_EQ(end, S::Shortener::End::Gained);
}

TEST(StaggerSpeed, ALightStaggerReachesItsWindowAtMaxRate) {
    S::Shortener sh;
    const float dt = 1.0f / 60.0f;
    const float real = RealTimeToWindow(sh, 20104, 0.333f, dt, 0.15f);
    // 0.333 s at up to x1.75: about 0.19 s, never more than shorten sooner.
    EXPECT_GT(real, 0.333f - 0.15f - 1e-4f);
    EXPECT_LT(real, 0.333f - 0.12f);
}

TEST(StaggerSpeed, TheRateNeverExceedsMaxRateAndThe60fpsAnd30fpsAgree) {
    for (const float dt : {1.0f / 60.0f, 1.0f / 30.0f}) {
        S::Shortener sh;
        float clip = 0.0f;
        for (int i = 0; i < 60; ++i) {
            const float rate = sh.Step({.anim_id = 20203, .clip_time = clip, .dt = dt,
                                        .shorten_s = 0.15f});
            EXPECT_LE(rate, S::MaxRate + 1e-6f);
            EXPECT_GE(rate, 1.0f);
            clip += dt * rate;
        }
        EXPECT_NEAR(sh.Gained(), 0.15f, 1e-4f);
    }
}

TEST(StaggerSpeed, KnockdownsGrabsDeathAndOtherClipsAreTheGames) {
    for (const std::int32_t id : {20400, 20500, 21000, 23999, 9000, 24000, 7100, 30000, 0}) {
        S::Shortener sh;
        for (int i = 0; i < 10; ++i) {
            const float rate = sh.Step({.anim_id = id, .clip_time = 0.02f * i, .dt = 1.0f / 60.0f,
                                        .shorten_s = 0.15f});
            EXPECT_EQ(rate, 1.0f) << id;
        }
    }
    // A weapon group's id (a2xx) of a stagger is a stagger.
    EXPECT_TRUE(S::IsHitStagger(202020203));
    EXPECT_FALSE(S::IsHitStagger(-1));
}

TEST(StaggerSpeed, ZeroIsTheGames) {
    S::Shortener sh;
    const float dt = 1.0f / 60.0f;
    EXPECT_NEAR(RealTimeToWindow(sh, 20203, 0.733f, dt, 0.0f), 0.733f, dt + 1e-4f);
}

TEST(StaggerSpeed, AnOpenWindowEndsItButNotTheLastClipsStaleOne) {
    S::Shortener sh;
    const float dt = 1.0f / 60.0f;
    // The first update of the stagger still carries the idle's R1: ignored (clip time < 0.1 s).
    // (Each step in a local: the test shim evaluates its arguments more than once.)
    const float first = sh.Step({.anim_id = 20203, .clip_time = 0.0f, .window = true, .dt = dt,
                                 .shorten_s = 0.15f});
    EXPECT_GT(first, 1.0f);
    // A real window (past WindowMinS) ends the speed-up at once.
    const float at_window = sh.Step({.anim_id = 20203, .clip_time = 0.12f, .window = true,
                                     .dt = dt, .shorten_s = 0.15f});
    EXPECT_EQ(at_window, 1.0f);
    const auto end = sh.TakeEnded();
    EXPECT_EQ(end, S::Shortener::End::Window);
    const float after = sh.Step({.anim_id = 20203, .clip_time = 0.14f, .window = false, .dt = dt,
                                 .shorten_s = 0.15f});
    EXPECT_EQ(after, 1.0f);
}

TEST(StaggerSpeed, ANewBlowMidStaggerStartsOver) {
    S::Shortener sh;
    const float dt = 1.0f / 60.0f;
    float clip = 0.0f;
    for (int i = 0; i < 30; ++i) {
        clip += dt * sh.Step({.anim_id = 20203, .clip_time = clip, .dt = dt, .shorten_s = 0.15f});
    }
    EXPECT_FALSE(sh.Active());
    // The same id from its start again: a new blow.
    const float again = sh.Step({.anim_id = 20203, .clip_time = 0.017f, .dt = dt,
                                 .shorten_s = 0.15f});
    EXPECT_GT(again, 1.0f);
    const bool started = sh.TakeStarted();
    EXPECT_TRUE(started);
    // Another stagger id mid-speed-up: starts over too.
    const float other = sh.Step({.anim_id = 20204, .clip_time = 0.017f, .dt = dt,
                                 .shorten_s = 0.15f});
    EXPECT_GT(other, 1.0f);
    const auto end = sh.TakeEnded();
    EXPECT_EQ(end, S::Shortener::End::NewClip);
    EXPECT_NEAR(sh.Gained(), dt * (S::MaxRate - 1.0f), 1e-5f);
    // A knockdown mid-speed-up: the game's at once.
    const float down = sh.Step({.anim_id = 20500, .clip_time = 0.0f, .dt = dt, .shorten_s = 0.15f});
    EXPECT_EQ(down, 1.0f);
    EXPECT_FALSE(sh.Active());
}

// The fitted stagger vignette: 0.75 at the visible edge for a stagger, 0.9 for a knockdown, and a
// darker onset dip; without it the old opacities.
TEST(DazeFit, FittedOpacitiesAndTheOnsetDip) {
    const float dt = 1.0f / 90.0f;
    D::Fader fit;
    float at_100ms = 0.0f;
    float at_500ms = 0.0f;
    for (int i = 1; i <= 45; ++i) {
        const float a = fit.Step(D::Level::Stagger, false, dt, 1.0f, 0.4f, true, 0.12f);
        if (i == 9) {
            at_100ms = a;
        }
        at_500ms = a;
    }
    EXPECT_NEAR(at_500ms, D::FitStaggerAlpha, 1e-4f);
    EXPECT_GT(at_100ms, at_500ms * 1.15f); // the dip: darker in the first moment
    EXPECT_LE(at_100ms, D::FitMaxAlpha + 1e-6f);

    D::Fader down;
    float a = 0.0f;
    for (int i = 0; i < 60; ++i) {
        a = down.Step(D::Level::Knockdown, false, dt, 1.0f, 0.4f, true, 0.12f);
    }
    EXPECT_NEAR(a, D::FitKnockdownAlpha, 1e-4f);

    D::Fader old;
    for (int i = 0; i < 60; ++i) {
        a = old.Step(D::Level::Stagger, false, dt, 1.0f, 0.4f);
    }
    EXPECT_NEAR(a, D::StaggerAlpha, 1e-4f);
    // No dip without dip_ms.
    D::Fader nodip;
    float peak = 0.0f;
    for (int i = 0; i < 60; ++i) {
        peak = std::max(peak, nodip.Step(D::Level::Stagger, false, dt, 1.0f, 0.4f, true, 0.0f));
    }
    EXPECT_NEAR(peak, D::FitStaggerAlpha, 1e-4f);
}

TEST(DazeFit, ABlowInsideTheRestDipsAgain) {
    const float dt = 1.0f / 90.0f;
    D::Fader f;
    for (int i = 0; i < 60; ++i) {
        f.Step(D::Level::Stagger, false, dt, 1.0f, 0.4f, true, 0.12f);
    }
    for (int i = 0; i < 9; ++i) { // cleared 100 ms ago: in the faint rest
        f.Step(D::Level::None, false, dt, 1.0f, 0.4f, true, 0.12f);
    }
    f.Step(D::Level::Stagger, false, dt, 1.0f, 0.4f, true, 0.12f);
    EXPECT_TRUE(f.Merged());
    EXPECT_LT(f.SinceStart(), 0.02f);
    EXPECT_FLOAT_EQ(D::DipWeight(0.05f, 0.12f), 1.0f);
    EXPECT_NEAR(D::DipWeight(0.12f + D::DipEaseS, 0.12f), 0.0f, 1e-4f);
    EXPECT_FLOAT_EQ(D::DipWeight(0.05f, 0.0f), 0.0f);
}

// The red hit flash over the stagger's black (stagger_vignette_red 0.35, 2026-10-07): a blow's
// first moment keeps most of its red (the black is still fading in), at full darkness 35% of it,
// thinner; the black's edge then outweighs it about 7 : 1 (0.75 x 1.25 dip vs 0.3 x 0.35); no
// stagger = the flash as before; red 1 = as before over a stagger too.
TEST(DazeFit, TheRedFlashYieldsToTheBlack) {
    const float dt = 1.0f / 90.0f;
    const float red = 0.35f;
    D::Fader f;
    std::vector<float> share;
    for (int i = 1; i <= 27; ++i) { // 300 ms
        const float a = f.Step(D::Level::Stagger, false, dt, 1.0f, 0.4f, true, 0.12f);
        share.push_back(D::Share(a, f.Shown(), true, 1.0f));
    }
    // 22 ms in: the black barely in, the red still mostly there.
    EXPECT_GT(D::RedScale(share[1], red), 0.8f);
    // From 100 ms (the black fully in, the dip counts as full): 35%, the profile 1.5x.
    for (size_t i = 8; i < share.size(); ++i) {
        EXPECT_FLOAT_EQ(share[i], 1.0f) << i;
        EXPECT_NEAR(D::RedScale(share[i], red), 0.35f, 1e-6f);
        EXPECT_NEAR(D::RedPower(share[i], red), D::RedPowerScale, 1e-6f);
    }
    const float black = f.Step(D::Level::Stagger, false, dt, 1.0f, 0.4f, true, 0.12f);
    EXPECT_GT(black, 6.0f * 0.3f * D::RedScale(1.0f, red)); // hit_vignette_strength 0.3
    // No stagger: unchanged.
    EXPECT_FLOAT_EQ(D::Share(0.0f, D::Level::None, true, 1.0f), 0.0f);
    EXPECT_FLOAT_EQ(D::RedScale(0.0f, red), 1.0f);
    EXPECT_FLOAT_EQ(D::RedPower(0.0f, red), 1.0f);
    // red = 1: the flash as before over a stagger too.
    EXPECT_FLOAT_EQ(D::RedScale(1.0f, 1.0f), 1.0f);
    EXPECT_FLOAT_EQ(D::RedPower(1.0f, 1.0f), 1.0f);
    // Fitted, the red's clear inside moves out to RedFitInner over a full tunnel (a rim in the
    // outer ~5%), never inward; unfitted (0) and red 1 unchanged.
    const float hit_inner = 26.0f / 56.0f; // the presenter's HitFitInner
    EXPECT_FLOAT_EQ(D::RedInner(0.0f, red, hit_inner), hit_inner);
    EXPECT_FLOAT_EQ(D::RedInner(1.0f, red, hit_inner), D::RedFitInner);
    EXPECT_GT(D::RedInner(0.5f, red, hit_inner), hit_inner);
    EXPECT_LT(D::RedInner(0.5f, red, hit_inner), D::RedFitInner);
    EXPECT_FLOAT_EQ(D::RedInner(1.0f, red, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(D::RedInner(1.0f, 1.0f, hit_inner), hit_inner);
    EXPECT_FLOAT_EQ(D::RedInner(1.0f, red, 0.9f), 0.9f);
    // A knockdown's share against its own full darkness; strength 0 = no tunnel.
    EXPECT_NEAR(D::Share(0.45f, D::Level::Knockdown, true, 1.0f), 0.5f, 1e-6f);
    EXPECT_FLOAT_EQ(D::Share(0.45f, D::Level::Stagger, true, 0.0f), 0.0f);
}

} // namespace
