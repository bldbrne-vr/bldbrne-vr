// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include <gtest/gtest.h>

#include "core/vr/anim_classes.h"
#include "core/vr/daze.h"
#include "core/vr/hit_stun.h"
#include "core/vr/hit_vignette.h"

namespace {

namespace B = VR::Body;
namespace D = VR::Daze;
namespace H = VR::HitStun;
namespace V = VR::HitVig;

constexpr std::int64_t kMs = 1'000'000;
constexpr std::uint64_t kIdleAllow = 0xC3FFEB; // what idle accepts (as read in the game)

struct Clip {
    std::int32_t id;
    float from;
};
B::GateSignals Frame(std::initializer_list<Clip> clips, std::uint64_t allow, bool move = false) {
    B::GateSignals s{};
    s.ring_valid = true;
    for (const Clip& c : clips) {
        s.new_ids[s.new_count] = c.id;
        s.new_from[s.new_count++] = c.from;
    }
    s.cancel_valid = true;
    s.allow = allow;
    s.move_ok = move;
    return s;
}

// The physical melee's refusal of a swing (bloodborne_body.cpp MeleePoseTick's attack_wait and
// the right arm: bloodborne_melee.cpp BlockReason "the game takes no attack yet" / "the game has
// the right arm").
bool MeleeRefuses(const B::GateOut& g) {
    const bool attack_wait = g.kept && g.ik[1] && !g.attack_ok[1];
    return attack_wait || (!g.ik[1] && !g.dodge_attack);
}

// Plays clip `id` from idle at 60 fps: its R1 / R2 window opens at `r_at` s, L2's at `l_at` s,
// a move cancels it at `move_at`. Returns the time the stun ended (ms after the clip's first
// frame, -1: never) and checks, every frame, that the stun is exactly the melee's refusal.
double StunEndMs(std::int32_t id, bool keep, float r_at, float l_at, float move_at = 9.0f,
                 int* frames_checked = nullptr) {
    const std::int64_t frame = 1'000'000'000 / 60;
    const float dt = 1.0f / 60.0f;
    B::AnimGate gate;
    std::int64_t now = 0;
    gate.Step(Frame({{0, 1.0f}}, kIdleAllow), now += frame);
    const std::int64_t start = now + frame;
    double end_ms = -1.0;
    int checked = 0;
    for (int i = 0; i * dt < 1.5f; ++i) {
        const float t = i * dt;
        std::uint64_t allow = 0;
        if (i == 0) {
            allow = kIdleAllow; // the first step still carries idle's requests
        } else {
            allow |= t >= r_at ? B::AllowRightAttack : 0;
            allow |= t >= l_at ? B::AllowLeftAttack : 0;
        }
        B::GateSignals s = Frame({{id, t}}, allow, i > 0 && t >= move_at);
        s.keep_hit = keep;
        const B::GateOut out = gate.Step(s, now += frame);
        const bool stun = H::StunClip(out) >= 0;
        if (stun) {
            EXPECT_EQ(H::StunClip(out), id) << "frame " << i;
            // A stun is always a refused swing.
            EXPECT_TRUE(MeleeRefuses(out)) << "frame " << i;
            ++checked;
        }
        // A Stagger of the black vignette is always a stun (never a view without either).
        if (D::LevelOf(out) == D::Level::Stagger) {
            EXPECT_TRUE(stun) << "frame " << i;
        }
        if (!stun && end_ms < 0.0) {
            end_ms = static_cast<double>(now - start) / static_cast<double>(kMs);
            // ... and from here the melee takes the swing again (a kept hit; the game's arm
            // came back with its window).
            EXPECT_FALSE(MeleeRefuses(out)) << "frame " << i;
        }
    }
    if (frames_checked != nullptr) {
        *frames_checked = checked;
    }
    return end_ms;
}

// The stun is the on-feet hit reaction before its R1 window: a light stagger 0.18 s, a heavy one
// 0.58 s (as in the user's playtest with stagger_shorten_ms), with the hands kept or not; every
// frame of it is a swing the melee refuses, and the first frame after it is not.
TEST(HitStun, TheStunIsTheMeleesRefusalUntilR1) {
    for (const bool keep : {true, false}) {
        int n = 0;
        EXPECT_NEAR(StunEndMs(3020110, keep, 0.18f, 0.18f, 0.25f, &n), 183.0, 17.0) << keep;
        EXPECT_GT(n, 8);
        EXPECT_NEAR(StunEndMs(20201, keep, 0.58f, 0.62f, 0.7f, &n), 583.0, 17.0) << keep;
        EXPECT_GT(n, 30);
        // L2's window first: swings still wait for R1 (the black would clear at L2).
        EXPECT_NEAR(StunEndMs(20103, keep, 0.40f, 0.20f, 0.6f), 400.0, 17.0) << keep;
        // A move cancel before R1 ends the clip's hold (the game frees the hunter).
        EXPECT_NEAR(StunEndMs(20104, keep, 0.50f, 0.50f, 0.30f), 300.0, 17.0) << keep;
    }
}

// Never for a knockdown, a launch, a get-up, a dodge, a guard reaction, an attack, a death clip,
// or death in a stagger.
TEST(HitStun, OnlyOnFeetHitReactions) {
    const std::int64_t frame = 1'000'000'000 / 60;
    for (const std::int32_t id : {20400, 20500, 22100, 23999, 7101, 7200, 10000, 30000, 24000}) {
        B::AnimGate gate;
        std::int64_t now = 0;
        B::GateSignals s = Frame({{id, 0.0f}}, 0);
        s.keep_hit = s.keep_dodge = true;
        const B::GateOut out = gate.Step(s, now += frame);
        EXPECT_EQ(H::StunClip(out), -1) << id;
    }
    for (const std::int32_t id : {20000, 20134, 20203, 20334, 3020120}) {
        B::AnimGate gate;
        std::int64_t now = 0;
        B::GateSignals s = Frame({{id, 0.0f}}, 0);
        s.keep_hit = true;
        const B::GateOut out = gate.Step(s, now += frame);
        EXPECT_EQ(H::StunClip(out), id) << id;
    }
    B::AnimGate gate;
    std::int64_t now = 0;
    B::GateSignals s = Frame({{20203, 0.0f}}, 0);
    s.keep_hit = true;
    s.hp_valid = true;
    s.hp = 0;
    B::GateOut out = gate.Step(s, now += frame);
    EXPECT_EQ(H::StunClip(out), -1);
    // The arm gate off (arm_anim_gate=0): no held clip, no stun.
    gate.Reset();
    s = Frame({{20203, 0.0f}}, 0);
    s.anim_gate = false;
    out = gate.Step(s, now += frame);
    EXPECT_EQ(H::StunClip(out), -1);
}

// Which stagger the black tunnel keeps: hitstun_vignette=1 a heavy one and knockdowns, 2 only
// knockdowns, 0 all of them.
TEST(HitStun, TheBlackStepsAsideForTheRed) {
    using L = D::Level;
    EXPECT_EQ(H::BlackLevel(L::Stagger, H::Mode::Light, 20103), L::None);
    EXPECT_EQ(H::BlackLevel(L::Stagger, H::Mode::Light, 3020120), L::None);
    EXPECT_EQ(H::BlackLevel(L::Stagger, H::Mode::Light, 20000), L::None);
    EXPECT_EQ(H::BlackLevel(L::Stagger, H::Mode::Light, 20201), L::Stagger);
    EXPECT_EQ(H::BlackLevel(L::Stagger, H::Mode::Light, 20334), L::Stagger);
    EXPECT_EQ(H::BlackLevel(L::Stagger, H::Mode::OnFeet, 20201), L::None);
    EXPECT_EQ(H::BlackLevel(L::Stagger, H::Mode::OnFeet, 20334), L::None);
    EXPECT_EQ(H::BlackLevel(L::Stagger, H::Mode::Off, 20103), L::Stagger);
    for (const H::Mode m : {H::Mode::Off, H::Mode::Light, H::Mode::OnFeet}) {
        EXPECT_EQ(H::BlackLevel(L::Knockdown, m, -1), L::Knockdown);
        EXPECT_EQ(H::BlackLevel(L::None, m, 20103), L::None);
    }
    EXPECT_EQ(H::ModeOf(0), H::Mode::Off);
    EXPECT_EQ(H::ModeOf(1), H::Mode::Light);
    EXPECT_EQ(H::ModeOf(2), H::Mode::OnFeet);
    EXPECT_EQ(H::ModeOf(7), H::Mode::Off);
}

// The hold: in over 60 ms, held as long as the stun, out over 120 ms; death ends it at once.
TEST(HitStun, TheHoldFollowsTheStunAndClearsQuickly) {
    const float dt = 1.0f / 120.0f;
    H::Hold h;
    float a = 0.0f;
    int frames_in = 0;
    while (a < 1.0f - 1e-5f && frames_in < 100) {
        a = h.Step(true, false, dt);
        ++frames_in;
    }
    EXPECT_NEAR(frames_in * dt, H::FadeInS, 1.5f * dt);
    EXPECT_TRUE(h.On());
    for (int i = 0; i < 60; ++i) {
        a = h.Step(true, false, dt);
    }
    EXPECT_NEAR(a, 1.0f, 1e-6f);
    EXPECT_NEAR(h.Since(), (frames_in + 60) * dt, 1e-4f);
    int frames_out = 0;
    while (a > 0.0f && frames_out < 100) {
        a = h.Step(false, false, dt);
        ++frames_out;
        EXPECT_FALSE(h.On());
        if (a > 0.0f) {
            EXPECT_TRUE(h.Shown());
        }
    }
    EXPECT_NEAR(frames_out * dt, H::FadeOutS, 1.5f * dt);
    EXPECT_FALSE(h.Shown());
    // Half way out after 60 ms (linear; the drawn level is its smoothstep).
    for (int i = 0; i < 20; ++i) {
        h.Step(true, false, dt);
    }
    for (int i = 0; i < 7; ++i) {
        a = h.Step(false, false, dt);
    }
    EXPECT_NEAR(h.Linear(), 1.0f - 7.0f * dt / H::FadeOutS, 1e-4f);
    EXPECT_NEAR(h.Linear(), 0.5f, 0.02f);
    // A new stun while it fades comes back up from there; a reset ends it at once.
    a = h.Step(true, false, dt);
    EXPECT_GT(h.Linear(), 0.5f);
    EXPECT_NEAR(h.Since(), dt, 1e-6f);
    a = h.Step(true, true, dt);
    EXPECT_EQ(a, 0.0f);
    EXPECT_FALSE(h.Shown());
}

// The red: the flash or the hold at the blow's peak, the larger; the flash's tail is cut to the
// hold's fade while the hold shows.
TEST(HitStun, TheRedIsTheFlashOrTheHold) {
    EXPECT_NEAR(H::Red(0.0f, 1.0f, 0.94f, 1.0f), 0.94f, 1e-6f);
    EXPECT_NEAR(H::Red(0.97f, 1.0f, 0.94f, 1.0f), 0.97f, 1e-6f);
    EXPECT_NEAR(H::Red(0.2f, 1.0f, 0.94f, 0.5f), 0.47f, 1e-6f);
    EXPECT_NEAR(H::Red(0.0f, 1.0f, 1.0f, 3.0f), 1.5f, 1e-6f);
    EXPECT_NEAR(H::Red(0.3f, 0.0f, 1.0f, 1.0f), 0.3f, 1e-6f);
    EXPECT_NEAR(H::FlashFadeS(0.5f, true), H::FadeOutS, 1e-6f);
    EXPECT_NEAR(H::FlashFadeS(0.5f, false), 0.5f, 1e-6f);
    EXPECT_NEAR(H::FlashFadeS(0.05f, true), 0.05f, 1e-6f);
}

// A timeline as the presenter runs it (120 Hz): a blow at 0 that takes 20% of max HP (flash peak
// 0.94), a light stun whose window opens at 183 ms. The flash alone (hitstun off) is still at
// 82% of its peak at the window and lingers to 600 ms; with the hold the red stays at the peak
// through the stun and is gone 120 ms after the window.
TEST(HitStun, ALightStunTimeline) {
    const float dt = 1.0f / 120.0f;
    const float peak = 0.94f;
    for (const bool on : {false, true}) {
        // The flash (openxr_presenter.cpp UpdateHitVignette): up 60 ms, held 40, out over 500
        // (the hold cuts the fade to FadeOutS).
        float level = 0.0f, hold_left = 0.04f;
        bool rising = true;
        H::Hold h;
        float at_window = -1.0f, gone_at = -1.0f, min_in_stun = 2.0f;
        for (int i = 0; i < 120; ++i) {
            const float t = i * dt;
            const bool stun = on && t < 0.183f;
            const float hold = h.Step(stun, false, dt);
            if (rising) {
                level = std::min(level + dt / 0.06f, 1.0f);
                rising = level < 1.0f;
            } else if (hold_left > 0.0f) {
                hold_left -= dt;
            } else {
                level = std::max(level - dt / H::FlashFadeS(0.5f, h.Shown()), 0.0f);
            }
            const float flash = peak * level * level * (3.0f - 2.0f * level);
            const float red = H::Red(flash, hold, peak, 1.0f);
            if (t >= 0.07f && t < 0.183f) {
                min_in_stun = std::min(min_in_stun, red);
            }
            if (at_window < 0.0f && t >= 0.183f) {
                at_window = red;
            }
            if (gone_at < 0.0f && t > 0.1f && red <= 0.0f) {
                gone_at = t;
            }
        }
        if (on) {
            EXPECT_NEAR(min_in_stun, peak, 0.01f);
            EXPECT_NEAR(gone_at, 0.183f + H::FadeOutS, 2.0f * dt);
        } else {
            EXPECT_LT(min_in_stun, peak - 0.04f);
            EXPECT_GT(gone_at, 0.55f);
        }
        EXPECT_GT(at_window, 0.5f);
    }
}

// ---- the red ring (core/vr/hit_vignette.h, hit_vignette_inner, 2026-10-09) ----

constexpr float kRed = 0.35f; // stagger_vignette_red's default

// The ring until 2026-10-09 (the presenter's old constants): fitted, clear inside 26 / 56, the
// powers 3 (plain) / 2 (wedge) doubled, the stagger merge from 26 / 56; not fitted, the mesh's own
// degree ring (no inner_deg).
float OldFitProfile(float r, bool wedge, float share) {
    const float inner = D::RedInner(share, kRed, 26.0f / 56.0f);
    const float t = std::clamp((r - inner) / (1.0f - inner), 0.0f, 1.0f);
    return std::pow(t * t * (3.0f - 2.0f * t),
                    (wedge ? 2.0f : 3.0f) * 2.0f * D::RedPower(share, kRed));
}

TEST(HitVignette, TodaysInnerReproducesTheOldRing) {
    for (const bool wedge : {false, true}) {
        for (const float share : {0.0f, 0.4f, 1.0f}) {
            const V::Ring fit = V::RingFor(V::OldInner, true, wedge, share, kRed);
            EXPECT_FLOAT_EQ(fit.fit_inner, D::RedInner(share, kRed, 26.0f / 56.0f));
            EXPECT_FLOAT_EQ(fit.fit_full, 1.0f);
            EXPECT_FLOAT_EQ(fit.power, (wedge ? 4.0f : 6.0f) * D::RedPower(share, kRed));
            EXPECT_EQ(fit.inner_deg, 0.0f);
            for (float r = 0.0f; r <= 1.4f; r += 0.01f) {
                EXPECT_NEAR(V::FitProfile(r, fit), OldFitProfile(r, wedge, share), 1e-5f);
            }
            const V::Ring deg = V::RingFor(V::OldInner, false, wedge, share, kRed);
            EXPECT_EQ(deg.fit_inner, 0.0f);   // the degree ring
            EXPECT_EQ(deg.inner_deg, 0.0f);   // the mesh's own 26 .. 56 degrees
            EXPECT_FLOAT_EQ(deg.power, (wedge ? 2.0f : 3.0f) * D::RedPower(share, kRed));
        }
    }
}

// The flash, its wedge and the hit-stun hold are one AppendVignette from RingFor: the inner edge
// is hit_vignette_inner for all three (the hold over a light stagger: the black steps aside, share
// 0); over a heavy stagger's black it moves out from that base toward RedFitInner, never inward.
TEST(HitVignette, TheInnerEdgeIsTheSettingForTheFlashTheWedgeAndTheHold) {
    for (const float inner : {V::OldInner, 0.6f, V::DefaultInner, 0.7f}) {
        const V::Ring flash = V::RingFor(inner, true, false, 0.0f, kRed);
        const V::Ring wedge = V::RingFor(inner, true, true, 0.0f, kRed);
        const V::Ring hold_light = V::RingFor(inner, true, false, 0.0f, kRed);
        EXPECT_FLOAT_EQ(flash.fit_inner, inner);
        EXPECT_FLOAT_EQ(wedge.fit_inner, inner);
        EXPECT_FLOAT_EQ(hold_light.fit_inner, inner);
        float last = inner;
        for (float share = 0.1f; share <= 1.001f; share += 0.1f) {
            const V::Ring heavy = V::RingFor(inner, true, false, share, kRed);
            EXPECT_GE(heavy.fit_inner, last); // out with the black's share
            EXPECT_FLOAT_EQ(heavy.fit_inner, inner + (D::RedFitInner - inner) * share);
            last = heavy.fit_inner;
            // stagger_vignette_red=1: the flash as is over the black.
            EXPECT_FLOAT_EQ(V::RingFor(inner, true, false, share, 1.0f).fit_inner, inner);
        }
        EXPECT_NEAR(last, D::RedFitInner, 1e-5f); // the old and the new rim meet at full black
    }
    // A setting beyond RedFitInner is never moved inward by the black.
    EXPECT_FLOAT_EQ(V::RingFor(0.9f, true, false, 1.0f, kRed).fit_inner, 0.9f);
    // Clamped like vr.ini's parse.
    EXPECT_FLOAT_EQ(V::RingFor(0.05f, true, false, 0.0f, kRed).fit_inner, V::MinInner);
    EXPECT_FLOAT_EQ(V::RingFor(3.0f, true, false, 0.0f, kRed).fit_inner, V::MaxInner);
    // Not fitted: the degree ring's clear inside moves out with it (inner x 56 degrees).
    const V::Ring deg = V::RingFor(V::DefaultInner, false, false, 0.0f, kRed);
    EXPECT_EQ(deg.fit_inner, 0.0f);
    EXPECT_NEAR(deg.inner_deg, V::DefaultInner * 56.0f, 1e-4f);
    EXPECT_FLOAT_EQ(deg.full_deg, 56.0f);
}

// "A little smaller" (the user, ~01:10: "do not make much softer, just a little bit": 0.53; the
// morning: "a tiny bit smaller": 0.58): never more red anywhere than before (than the old ring and
// than 0.53), the visible edge (r >= 1) as strong, clear inside the new inner edge, and a little
// less of the view above 3% opacity (at hit_vignette_strength 0.3: a profile of 0.1) for the plain
// ring and the wedge: 15 .. 25% less than the old ring, 5 .. 15% less than 0.53, not more.
TEST(HitVignette, TheDefaultIsSmallerAndAsStrongAtTheEdge) {
    constexpr float first_default = 0.53f; // the default from ~01:10 to the 2026-10-09 morning
    EXPECT_GT(V::DefaultInner, first_default + 0.03f);
    EXPECT_LT(V::DefaultInner, first_default + 0.07f);
    for (const bool wedge : {false, true}) {
        const V::Ring now = V::RingFor(V::DefaultInner, true, wedge, 0.0f, kRed);
        const V::Ring old = V::RingFor(V::OldInner, true, wedge, 0.0f, kRed);
        const V::Ring first = V::RingFor(first_default, true, wedge, 0.0f, kRed);
        for (float r = 0.0f; r <= 1.4f; r += 0.005f) {
            EXPECT_LE(V::FitProfile(r, now), V::FitProfile(r, old) + 1e-6f);
            EXPECT_LE(V::FitProfile(r, now), V::FitProfile(r, first) + 1e-6f);
            if (r <= V::DefaultInner) {
                EXPECT_EQ(V::FitProfile(r, now), 0.0f);
            }
            if (r >= 1.0f) {
                EXPECT_FLOAT_EQ(V::FitProfile(r, now), 1.0f);
            }
        }
        // The share of the displayed field (r <= 1 on a grid of the unit disc in per-axis
        // fractions) where the red passes 3% at 0.3 strength.
        int n = 0, a_now = 0, a_old = 0, a_first = 0;
        for (float x = -1.0f; x <= 1.0f; x += 0.01f) {
            for (float y = -1.0f; y <= 1.0f; y += 0.01f) {
                const float r = std::sqrt(x * x + y * y);
                if (r > 1.0f) {
                    continue;
                }
                ++n;
                a_now += V::FitProfile(r, now) * 0.3f > 0.03f ? 1 : 0;
                a_old += V::FitProfile(r, old) * 0.3f > 0.03f ? 1 : 0;
                a_first += V::FitProfile(r, first) * 0.3f > 0.03f ? 1 : 0;
            }
        }
        EXPECT_GT(a_now, 0);
        EXPECT_LT(static_cast<float>(a_now), 0.85f * static_cast<float>(a_old));
        EXPECT_GT(static_cast<float>(a_now), 0.75f * static_cast<float>(a_old));
        EXPECT_LT(static_cast<float>(a_now), 0.95f * static_cast<float>(a_first));
        EXPECT_GT(static_cast<float>(a_now), 0.85f * static_cast<float>(a_first));
        std::printf("  red above 3%% in the field (%s): old %.1f%%, 0.53 %.1f%%, now %.1f%%\n",
                    wedge ? "wedge ring" : "plain ring", 100.0f * a_old / n,
                    100.0f * a_first / n, 100.0f * a_now / n);
    }
}

} // namespace
