// SPDX-License-Identifier: GPL-2.0-or-later

// Immersive vitals (src/core/vr/vitals.h): the Blood Gauge's levels and colours, the change
// edges, its animation, the low-HP heartbeat's timing, the dark edges, the fit to the arm's real
// surface from the game's depth (a ray-cast sleeve), the glance and the counter's reading
// direction.

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "core/vr/vitals.h"
#include "core/vr/vr_session.h"

namespace {

using namespace VR::Vitals;

Status Make(int hp, int max_hp, int stamina = 90, int max_stamina = 90, int qs = 20, int blood = 0,
            int rally = -1) {
    Status s{};
    s.ok = true;
    s.hp = hp;
    s.max_hp = max_hp;
    s.rally_hp = rally < 0 ? hp : rally;
    s.stamina = stamina;
    s.max_stamina = max_stamina;
    s.qs_bullets = qs;
    s.blood_bullets = blood;
    s.bullet_cap = 20;
    return s;
}

TEST(Vitals, RallyEndIsHpPlusThePool) {
    // The HUD's orange end (0x1A0D3F0): HP + (int)pool, never past max HP.
    EXPECT_EQ(RallyEnd(499, 712, 213.0f), 712);
    EXPECT_EQ(RallyEnd(224, 712, 275.6f), 499);
    EXPECT_EQ(RallyEnd(600, 712, 500.0f), 712);
    // No pool, a fraction of one, nonsense, or dead: none.
    EXPECT_EQ(RallyEnd(300, 712, 0.0f), 300);
    EXPECT_EQ(RallyEnd(300, 712, 0.6f), 300);
    EXPECT_EQ(RallyEnd(300, 712, -50.0f), 300);
    EXPECT_EQ(RallyEnd(300, 712, std::nanf("")), 300);
    EXPECT_EQ(RallyEnd(300, 712, 1.0e9f), 300);
    EXPECT_EQ(RallyEnd(0, 712, 100.0f), 0);
}

TEST(Vitals, FillsAreFractionsWithGuards) {
    const Fill f = FillOf(Make(356, 712, 45, 90, 10, 0, 500));
    EXPECT_NEAR(f.hp, 0.5f, 1e-6f);
    EXPECT_NEAR(f.rally, 500.0f / 712.0f, 1e-6f);
    EXPECT_NEAR(f.stamina, 0.5f, 1e-6f);
    EXPECT_NEAR(f.bullets, 0.5f, 1e-6f);
    // Guards: max 0, negatives, over the max, the rally below HP.
    const Fill z = FillOf(Make(10, 0, -5, 0, -3, 0, 2));
    EXPECT_EQ(z.hp, 0.0f);
    EXPECT_EQ(z.stamina, 0.0f);
    EXPECT_EQ(z.bullets, 0.0f);
    const Fill o = FillOf(Make(900, 712, 200, 90, 40, 0, 100));
    EXPECT_EQ(o.hp, 1.0f);
    EXPECT_EQ(o.stamina, 1.0f);
    EXPECT_EQ(o.bullets, 1.0f);
    EXPECT_GE(o.rally, o.hp);
    EXPECT_EQ(BulletCap(12), 20);
    EXPECT_EQ(BulletCap(25), 25); // the Formless Oedon rune: the most ever seen
}

TEST(Vitals, CounterNumberAndInk) {
    EXPECT_EQ(BulletsShown(Make(1, 1, 1, 1, 20, 0)), 20);
    EXPECT_EQ(BulletsShown(Make(1, 1, 1, 1, 20, 4)), 4); // Blood Bullets first, as the game
    EXPECT_EQ(ToneOf(Make(1, 1, 1, 1, 20, 0)), Tone::Bone);
    EXPECT_EQ(ToneOf(Make(1, 1, 1, 1, 3, 0)), Tone::Amber);
    EXPECT_EQ(ToneOf(Make(1, 1, 1, 1, 0, 0)), Tone::DarkRed);
    EXPECT_EQ(ToneOf(Make(1, 1, 1, 1, 0, 2)), Tone::FreshRed);
    // The palette in linear light (the shaders' constants).
    const auto blood = Linear(ColorBlood);
    EXPECT_NEAR(blood[0], 0.3278f, 2e-4f);
    EXPECT_NEAR(blood[1], 0.0110f, 2e-4f);
    const auto amber = Linear(ColorRally);
    EXPECT_GT(amber[0], amber[1]);
    EXPECT_GT(amber[1], amber[2]);
}

TEST(Vitals, ChangeEdges) {
    ChangeTracker t{};
    // (Each step once: the checks' messages evaluate their arguments again.)
    const auto step = [&](const Status& s) { return t.Step(s); };
    u32 bits = step(Make(712, 712));
    EXPECT_EQ(bits, 0u); // the first: none
    bits = step(Status{});
    EXPECT_EQ(bits, 0u); // a failed read: none, nothing forgotten
    bits = step(Make(499, 712, 90, 90, 20, 0, 712));
    EXPECT_EQ(bits, static_cast<u32>(ChangeHit));
    // HP back within the orange segment: rally; past it: a heal.
    bits = step(Make(600, 712, 90, 90, 20, 0, 712));
    EXPECT_EQ(bits, static_cast<u32>(ChangeRally));
    bits = step(Make(712, 712));
    EXPECT_EQ(bits, static_cast<u32>(ChangeRally));
    bits = step(Make(600, 712));
    EXPECT_EQ(bits, static_cast<u32>(ChangeHit));
    bits = step(Make(700, 712));
    EXPECT_EQ(bits, static_cast<u32>(ChangeHeal));
    bits = step(Make(700, 712, 50));
    EXPECT_EQ(bits, static_cast<u32>(ChangeStaminaSpent));
    bits = step(Make(700, 712, 0));
    EXPECT_EQ(bits, static_cast<u32>(ChangeStaminaSpent | ChangeStaminaEmpty));
    bits = step(Make(700, 712, 30));
    EXPECT_EQ(bits, 0u); // refilling
    bits = step(Make(700, 712, 30, 90, 19));
    EXPECT_EQ(bits, static_cast<u32>(ChangeShot));
    bits = step(Make(700, 712, 30, 90, 20));
    EXPECT_EQ(bits, static_cast<u32>(ChangeRestock));
    bits = step(Make(700, 712, 30, 90, 20, 3));
    EXPECT_EQ(bits, static_cast<u32>(ChangeShot)); // 20 -> 3 red ones
    // A level up: max HP changes, no hit or heal.
    bits = step(Make(760, 760, 30, 90, 20, 3));
    EXPECT_EQ(bits, static_cast<u32>(ChangeMaxHp));
}

TEST(Vitals, HeartbeatRate) {
    EXPECT_EQ(BeatsPerMinute(0.31f), 0.0f);
    EXPECT_EQ(BeatsPerMinute(1.0f), 0.0f);
    EXPECT_EQ(BeatsPerMinute(0.0f), 0.0f); // dead
    EXPECT_NEAR(BeatsPerMinute(0.2999f), 60.0f, 0.1f);
    EXPECT_NEAR(BeatsPerMinute(0.10f), 110.0f, 0.01f);
    EXPECT_NEAR(BeatsPerMinute(0.05f), 130.0f, 0.01f);
    EXPECT_NEAR(BeatsPerMinute(0.01f), 130.0f, 0.01f);
    float last = 0.0f;
    for (float hp = 0.299f; hp > 0.0f; hp -= 0.005f) {
        const float bpm = BeatsPerMinute(hp);
        EXPECT_GE(bpm, last - 1e-3f); // faster as HP falls
        last = bpm;
    }
    // vitals_low_hp moves the threshold.
    EXPECT_EQ(BeatsPerMinute(0.35f, 0.40f) > 0.0f, true);
    EXPECT_EQ(BeatsPerMinute(0.25f, 0.20f), 0.0f);
}

TEST(Vitals, HeartbeatTiming) {
    // At 120 Hz for 10 s at 30% HP minus a hair (60 bpm): 10 beats, one per second, the first at
    // once; then at 10% (110 bpm) the period shortens to 0.545 s.
    Heartbeat h{};
    constexpr float Dt = 1.0f / 120.0f;
    std::vector<float> beats;
    for (int i = 0; i < 1200; ++i) {
        if (h.Step(Dt, 0.2999f, true)) {
            beats.push_back(static_cast<float>(i) * Dt);
        }
    }
    ASSERT_EQ(beats.size(), 10u);
    EXPECT_NEAR(beats[0], 0.0f, 1e-6f);
    for (size_t i = 1; i < beats.size(); ++i) {
        EXPECT_NEAR(beats[i] - beats[i - 1], 1.0f, Dt + 1e-4f);
    }
    beats.clear();
    for (int i = 0; i < 1200; ++i) {
        if (h.Step(Dt, 0.10f, true)) {
            beats.push_back(static_cast<float>(i) * Dt);
        }
    }
    ASSERT_GE(beats.size(), 17u);
    EXPECT_NEAR(beats[3] - beats[2], 60.0f / 110.0f, Dt + 1e-4f);
    // Above the line, inactive (a menu, death): it stops; it starts again with a beat at once.
    EXPECT_FALSE(h.Step(Dt, 0.5f, true));
    EXPECT_FALSE(h.beating);
    EXPECT_EQ(h.Pulse(), 0.0f);
    EXPECT_TRUE(h.Step(Dt, 0.2f, true));
    EXPECT_FALSE(h.Step(Dt, 0.2f, false));
    // The envelope: the lub's peak, the dub's (0.7) 150 ms later, quiet between beats.
    EXPECT_NEAR(BeatEnvelope(0.025f), 1.0f, 1e-3f);
    EXPECT_NEAR(BeatEnvelope(0.175f), 0.7f, 0.02f);
    EXPECT_LT(BeatEnvelope(0.60f), 0.02f);
    EXPECT_EQ(BeatEnvelope(-0.1f), 0.0f);
    // The haptic only within 20 s of the last HP lost.
    constexpr std::int64_t S = 1'000'000'000;
    EXPECT_TRUE(HeartbeatFelt(100 * S, 90 * S));
    EXPECT_FALSE(HeartbeatFelt(111 * S, 90 * S));
    EXPECT_FALSE(HeartbeatFelt(100 * S, 0));
    EXPECT_TRUE(HeartbeatFelt(100 * S, 70 * S, 60.0f));
}

TEST(Vitals, DarkEdges) {
    EXPECT_EQ(EdgeStrength(1.0f), 0.0f);
    EXPECT_EQ(EdgeStrength(0.6f), 0.0f);
    EXPECT_NEAR(EdgeStrength(0.45f), 0.09f, 1e-6f);
    EXPECT_NEAR(EdgeStrength(0.30f), 0.18f, 1e-6f);
    EXPECT_NEAR(EdgeStrength(0.05f), 0.42f, 1e-6f);
    EXPECT_NEAR(EdgeStrength(0.0f), 0.42f, 1e-6f);
    float last = 0.0f;
    for (float hp = 1.0f; hp >= 0.0f; hp -= 0.01f) {
        EXPECT_GE(EdgeStrength(hp), last - 1e-6f);
        last = EdgeStrength(hp);
    }
    // Below the line the heartbeat swells it (+25%) and draws it 3 degrees in; above, never.
    const Edge calm = EdgeOf(0.2f, 0.0f, 0.0f, 0.0f, 0.0f);
    const Edge lub = EdgeOf(0.2f, 1.0f, 0.0f, 0.0f, 0.0f);
    EXPECT_NEAR(lub.alpha, calm.alpha * 1.25f, 1e-5f);
    EXPECT_NEAR(lub.inner_deg, EdgeInnerDeg - EdgeSwellDeg, 1e-5f);
    EXPECT_NEAR(calm.inner_deg, 30.0f, 1e-5f);
    EXPECT_NEAR(calm.full_deg, 58.0f, 1e-5f);
    EXPECT_NEAR(EdgeOf(0.5f, 1.0f, 0.0f, 0.0f, 0.0f).alpha, EdgeStrength(0.5f), 1e-6f);
    // A hit flash over it: max, not sum (the dark recedes under the flash).
    const Edge flash = EdgeOf(0.2f, 0.0f, 0.0f, 0.0f, 0.3f);
    EXPECT_NEAR(flash.alpha, std::max(calm.alpha - 0.3f, 0.0f), 1e-6f);
    EXPECT_LE(1.0f - (1.0f - flash.alpha) * (1.0f - 0.3f), std::max(calm.alpha, 0.3f) + 0.05f);
    // The rally's rim: 0.05 when it appears (half the first 0.10: too strong in the dark),
    // fading as the game drains it; the dark edges unchanged by it.
    EXPECT_NEAR(EdgeOf(0.5f, 0.0f, 0.2f, 0.2f, 0.0f).rim, 0.05f, 1e-6f);
    EXPECT_NEAR(EdgeOf(0.5f, 0.0f, 0.05f, 0.2f, 0.0f).rim, 0.0125f, 1e-6f);
    EXPECT_EQ(EdgeOf(0.5f, 0.0f, 0.0f, 0.2f, 0.0f).rim, 0.0f);
    EXPECT_NEAR(EdgeOf(0.2f, 0.0f, 0.2f, 0.2f, 0.0f).alpha, calm.alpha, 1e-6f);
    EXPECT_NEAR(EdgeStrength(0.30f), 0.18f, 1e-6f);
    EXPECT_NEAR(EdgeStrength(0.05f), 0.42f, 1e-6f);
}

TEST(Vitals, GaugeAnimation) {
    GaugeAnim a{};
    constexpr float Dt = 1.0f / 60.0f;
    GaugeLook l = a.Step(Make(712, 712), 0, Dt, false, true, 0.0f);
    EXPECT_NEAR(l.hp, 1.0f, 1e-6f);
    // Fades in over 0.15 s.
    for (int i = 0; i < 12; ++i) {
        l = a.Step(Make(712, 712), 0, Dt, false, true, 0.0f);
    }
    EXPECT_NEAR(l.alpha, 1.0f, 1e-6f);
    // A hit: the level drops at once, the orange segment stays at the old level, the tubes shake.
    l = a.Step(Make(356, 712, 90, 90, 20, 0, 712), ChangeHit, Dt, false, true, 0.0f);
    EXPECT_NEAR(l.hp, 0.5f, 1e-6f);
    EXPECT_NEAR(l.rally, 1.0f, 1e-6f);
    bool shook = false;
    for (int i = 0; i < 10; ++i) {
        l = a.Step(Make(356, 712, 90, 90, 20, 0, 712), 0, Dt, false, true, 0.0f);
        shook = shook || std::abs(l.shake_mm) > 0.1f;
    }
    EXPECT_TRUE(shook);
    EXPECT_GT(l.glow, 0.0f); // a change glows
    for (int i = 0; i < 20; ++i) {
        l = a.Step(Make(356, 712), 0, Dt, false, true, 0.0f);
    }
    EXPECT_EQ(l.shake_mm, 0.0f);
    // A heal: it rises over 0.3 s, with bubbles.
    l = a.Step(Make(712, 712), ChangeHeal, Dt, false, true, 0.0f);
    EXPECT_LT(l.hp, 0.6f);
    EXPECT_GT(l.bubbles, 0.0f);
    for (int i = 0; i < 9; ++i) {
        l = a.Step(Make(712, 712), 0, Dt, false, true, 0.0f);
    }
    EXPECT_GT(l.hp, 0.7f); // a third of a second: more than half way
    EXPECT_LT(l.hp, 1.0f);
    for (int i = 0; i < 10; ++i) {
        l = a.Step(Make(712, 712), 0, Dt, false, true, 0.0f);
    }
    EXPECT_NEAR(l.hp, 1.0f, 1e-6f);
    // Stamina spent to nothing: the glass fogs, then clears within 0.65 s.
    l = a.Step(Make(712, 712, 0), ChangeStaminaSpent | ChangeStaminaEmpty, Dt, false, true, 0.0f);
    EXPECT_NEAR(l.fog, 1.0f, 1e-6f);
    for (int i = 0; i < 40; ++i) {
        l = a.Step(Make(712, 712, 0), 0, Dt, false, true, 0.0f);
    }
    EXPECT_EQ(l.fog, 0.0f);
    // A shot: the counter rolls from 20 to 19 over 120 ms.
    l = a.Step(Make(712, 712, 90, 90, 19), ChangeShot, Dt, false, true, 0.0f);
    EXPECT_EQ(l.digits, 19);
    EXPECT_EQ(l.digits_old, 20);
    EXPECT_LT(l.roll, 1.0f);
    for (int i = 0; i < 8; ++i) {
        l = a.Step(Make(712, 712, 90, 90, 19), 0, Dt, false, true, 0.0f);
    }
    EXPECT_EQ(l.roll, 1.0f);
    // A glance glows (0.3 at most); death fades it out within 0.3 s.
    for (int i = 0; i < 200; ++i) {
        l = a.Step(Make(712, 712, 90, 90, 19), 0, Dt, i < 100, true, 0.0f);
        EXPECT_LE(l.glow, 0.3f + 1e-6f);
    }
    EXPECT_EQ(l.glow, 0.0f);
    for (int i = 0; i < 19; ++i) {
        l = a.Step(Make(0, 712, 90, 90, 19), 0, Dt, false, false, 0.0f);
    }
    EXPECT_EQ(l.alpha, 0.0f);
}

// ---- The fit: a sleeve ray-cast like the game's depth --------------------------------------------
// An elliptic sleeve round the x axis (semi-axes ry up = the back of the wrist, rz sideways), the
// camera at `eye` looking along `fwd` (unit). The probe's points: lines out of the axis at x =
// along, angle `around` from +y. A point's gap = its depth minus the scene's at its pixel (the
// first hit along the ray from the eye, or 100 m of background).
struct Sleeve {
    float ry{0.035f};
    float rz{0.045f};
};

float RayHit(const Sleeve& s, V3 o, V3 d) {
    // (o.y + t d.y)^2 / ry^2 + (o.z + t d.z)^2 / rz^2 = 1, the nearest t > 0.
    const float a = d.y * d.y / (s.ry * s.ry) + d.z * d.z / (s.rz * s.rz);
    const float b = 2.0f * (o.y * d.y / (s.ry * s.ry) + o.z * d.z / (s.rz * s.rz));
    const float c = o.y * o.y / (s.ry * s.ry) + o.z * o.z / (s.rz * s.rz) - 1.0f;
    const float disc = b * b - 4.0f * a * c;
    if (a < 1e-12f || disc < 0.0f) {
        return -1.0f;
    }
    const float t0 = (-b - std::sqrt(disc)) / (2.0f * a);
    const float t1 = (-b + std::sqrt(disc)) / (2.0f * a);
    return t0 > 1e-4f ? t0 : (t1 > 1e-4f ? t1 : -1.0f);
}

float MeasureLine(const Sleeve& s, V3 eye, V3 fwd, float around, float along,
                  const FitConfig& c = {}, float occluder_t = -1.0f) {
    std::array<float, FitSteps> gap{};
    std::array<bool, FitSteps> ok{};
    for (int i = 0; i < FitSteps; ++i) {
        const float r = FitRadius(i);
        const V3 p{along, r * std::cos(around), r * std::sin(around)};
        const V3 d0{p.x - eye.x, p.y - eye.y, p.z - eye.z};
        const float len = Length(d0);
        const V3 d{d0.x / len, d0.y / len, d0.z / len};
        float t = RayHit(s, eye, d);
        if (occluder_t > 0.0f && (t < 0.0f || occluder_t < t)) {
            t = occluder_t; // a hand in front of it all
        }
        const float scene = t > 0.0f ? t * Dot(d, fwd) : 100.0f;
        gap[static_cast<size_t>(i)] = Dot(d0, fwd) - scene;
        ok[static_cast<size_t>(i)] = true;
    }
    return LineExit(gap.data(), ok.data(), FitSteps, c);
}

TEST(Vitals, FitFindsTheSleeveFromAnyAngle) {
    const Sleeve s{};
    // Looking at the back of the wrist from 0.45 m: head-on, 40 and 70 degrees off.
    for (const float tilt_deg : {0.0f, 40.0f, 70.0f}) {
        const float t = tilt_deg * 0.0174532925f;
        const V3 eye{0.05f, 0.45f * std::cos(t), 0.45f * std::sin(t)};
        const V3 fwd{-eye.x / Length(eye), -eye.y / Length(eye), -eye.z / Length(eye)};
        const float exit = MeasureLine(s, eye, fwd, 0.0f, 0.0f);
        const float side_a = 23.0f * 0.0174532925f;
        (void)side_a;
        ASSERT_GT(exit, 0.0f);
        EXPECT_NEAR(exit, s.ry, tilt_deg < 60.0f ? 0.0015f : 0.0035f);
        // Round the arm (the plate's sides, 23 degrees): the ellipse's radius there.
        const float a = 23.0f * 0.0174532925f;
        const float expect = 1.0f / std::sqrt(std::pow(std::cos(a) / s.ry, 2.0f) +
                                              std::pow(std::sin(a) / s.rz, 2.0f));
        const float side = MeasureLine(s, eye, fwd, a, 0.0f);
        ASSERT_GT(side, 0.0f);
        EXPECT_NEAR(side, expect, tilt_deg < 60.0f ? 0.002f : 0.004f);
    }
    // A big gauntlet (65 mm) and a bare wrist (24 mm).
    const V3 eye{0.0f, 0.5f, 0.1f};
    const V3 fwd{0.0f, -0.98058f, -0.19612f};
    const float gauntlet = MeasureLine({0.065f, 0.07f}, eye, fwd, 0.0f, 0.0f);
    EXPECT_NEAR(gauntlet, 0.065f, 0.0015f);
    const float bare = MeasureLine({0.024f, 0.03f}, eye, fwd, 0.0f, 0.0f);
    EXPECT_NEAR(bare, 0.024f, 0.0015f);
    // Something in front (the right hand 0.25 m from the eye): no measurement.
    const float hidden = MeasureLine(s, eye, fwd, 0.0f, 0.0f, {}, 0.25f);
    EXPECT_LT(hidden, 0.0f);
    // Seen from the palm's side (the back of the wrist faces away): no exit toward the eye.
    const V3 under{0.0f, -0.5f, 0.0f};
    const float far_side = MeasureLine(s, under, {0.0f, 1.0f, 0.0f}, 0.0f, 0.0f);
    EXPECT_LT(far_side, 0.0f);
    // The axis off the arm (the first point already outside): none.
    std::array<float, FitSteps> gap{};
    std::array<bool, FitSteps> ok{};
    gap.fill(-0.01f);
    ok.fill(true);
    EXPECT_LT(LineExit(gap.data(), ok.data(), FitSteps), 0.0f);
    // A point off the frame on the way out: none.
    gap.fill(0.01f);
    gap[5] = -0.01f;
    ok[3] = false;
    EXPECT_LT(LineExit(gap.data(), ok.data(), FitSteps), 0.0f);
    // The first point without depth (a hole at the arm's middle): from the next one.
    ok.fill(true);
    ok[0] = false;
    const float skipped = LineExit(gap.data(), ok.data(), FitSteps);
    EXPECT_NEAR(skipped, FitRadius(4) + 0.5f * FitStepM, 1e-5f);
    ok[1] = ok[2] = false;
    EXPECT_LT(LineExit(gap.data(), ok.data(), FitSteps), 0.0f);
}

TEST(Vitals, FitCombinesAndFilters) {
    const std::array<float, FitLines> exits{0.035f, 0.036f, -1.0f, 0.032f, 0.033f};
    EXPECT_NEAR(CombineExits(exits.data(), FitLines), 0.036f, 1e-6f); // the plate clears all
    const std::array<float, FitLines> one{0.035f, -1.0f, -1.0f, -1.0f, -1.0f};
    EXPECT_LT(CombineExits(one.data(), FitLines), 0.0f); // one line is not enough
    // Two lines on the gun's grip beside the wrist (58 mm), three on the sleeve: the sleeve's.
    const std::array<float, FitLines> grip{0.030f, 0.058f, 0.031f, 0.057f, 0.029f};
    EXPECT_NEAR(CombineExits(grip.data(), FitLines), 0.031f, 1e-6f);
    // A cuff higher on one side (8 mm) is the sleeve: it counts.
    const std::array<float, FitLines> cuff{0.030f, 0.038f, 0.031f, 0.030f, 0.029f};
    EXPECT_NEAR(CombineExits(cuff.data(), FitLines), 0.038f, 1e-6f);
    FitConfig c{};
    FitFilter f{};
    // Before any measurement: the default; the first one at once (+ the stand-off).
    const float first = f.Step(-1.0f, 0.01f, c);
    EXPECT_NEAR(first, c.default_m, 1e-6f);
    const float second = f.Step(0.035f, 0.01f, c);
    EXPECT_NEAR(second, 0.035f + c.standoff_m, 1e-6f);
    // A thicker sleeve (a gauntlet put on): the plate lifts within a few frames (fast rise) ...
    float r = 0.0f;
    for (int i = 0; i < 30; ++i) {
        r = f.Step(0.060f, 1.0f / 60.0f, c);
    }
    EXPECT_NEAR(r, 0.060f + c.standoff_m, 0.0015f);
    // ... and settles slowly back down (no jitter), never below the surface plus the stand-off.
    for (int i = 0; i < 6; ++i) {
        r = f.Step(0.035f, 1.0f / 60.0f, c);
        EXPECT_GE(r, 0.035f + c.standoff_m - 1e-6f);
    }
    EXPECT_GT(r, 0.050f);
    for (int i = 0; i < 300; ++i) {
        r = f.Step(0.035f, 1.0f / 60.0f, c);
    }
    EXPECT_NEAR(r, 0.035f + c.standoff_m, 0.001f);
    // One wild measurement (a strap's edge, a stray texel) is outvoted by the median.
    const float before = r;
    r = f.Step(0.090f, 1.0f / 60.0f, c);
    EXPECT_NEAR(r, before, 1e-4f);
    // Nothing measured (hidden, off the frame): it holds.
    for (int i = 0; i < 100; ++i) {
        r = f.Step(-1.0f, 1.0f / 60.0f, c);
    }
    EXPECT_NEAR(r, before, 1e-4f);
    // Clamped to a sane range.
    FitFilter g{};
    const float big = g.Step(0.5f, 0.01f, c);
    EXPECT_NEAR(big, c.max_m, 1e-6f);
    FitFilter h{};
    const float small = h.Step(0.005f, 0.01f, c);
    EXPECT_NEAR(small, c.min_m, 1e-6f);
}

// The straps' sides: a strap at the plate's level on top comes down onto the sleeve where the
// side line came out of it, instead of a round ring at the plate's radius standing off the sleeve.
TEST(Vitals, StrapsHugTheSleeve) {
    // A slim sleeve under a plate lifted by a high cuff (the plate's strap height 45 mm, the sleeve
    // 30 mm up and 34 mm round): seen from 0.45 m, 30 degrees off the back of the wrist.
    const Sleeve s{0.030f, 0.034f};
    const float t = 30.0f * 0.0174532925f;
    const V3 eye{0.02f, 0.45f * std::cos(t), 0.45f * std::sin(t)};
    const V3 fwd{-eye.x / Length(eye), -eye.y / Length(eye), -eye.z / Length(eye)};
    const float a = 0.045f;
    const auto surface = [&](float polar) {
        return 1.0f / std::sqrt(std::pow(std::cos(polar) / s.ry, 2.0f) +
                                std::pow(std::sin(polar) / s.rz, 2.0f));
    };
    for (const float sign : {1.0f, -1.0f}) {
        const float exit = MeasureLine(s, eye, fwd, sign * StrapPolarRad, 0.0f);
        ASSERT_GT(exit, 0.0f);
        const StrapSide side = FitStrapSide(a, exit);
        ASSERT_TRUE(side.ok);
        // Where it fades out it is on the sleeve (the leather's thickness above it) ...
        const float y = a * std::cos(side.end);
        const float z = side.b * std::sin(side.end);
        const float r_end = std::sqrt(y * y + z * z);
        const float polar_end = std::atan2(z, y);
        // (the near side measures the sleeve itself; the far side its silhouette from the eye)
        const float tol = sign > 0.0f ? 0.003f : 0.010f;
        EXPECT_NEAR(r_end, surface(polar_end) + 0.0015f, tol);
        // ... and where it shows at the arm's side (the outer part, before it fades) it stands off
        // the sleeve less than half as far as a round strap's ends did (13 - 14 mm here).
        float worst_fit = 0.0f, worst_round = 0.0f;
        for (float ang = side.end - 0.30f; ang <= side.end - 0.15f; ang += 0.025f) {
            const float fy = a * std::cos(ang), fz = side.b * std::sin(ang);
            const float p = std::atan2(fz, fy);
            worst_fit = std::max(worst_fit, std::sqrt(fy * fy + fz * fz) - surface(p));
        }
        for (float ang = 0.65f; ang <= 0.80f; ang += 0.025f) {
            worst_round = std::max(worst_round, a - surface(ang));
        }
        EXPECT_LT(worst_fit, 0.5f * worst_round);
        EXPECT_GT(side.end, 0.60f);
        EXPECT_LE(side.end, 1.25f);
    }
    // Nothing seen: no fit (the strap stays as it was, round before any fit).
    EXPECT_FALSE(FitStrapSide(a, -1.0f).ok);
    // A side as high as the top (not a sleeve: the gun's grip beside the wrist), or far out: none.
    EXPECT_FALSE(FitStrapSide(a, 0.090f).ok);
    EXPECT_FALSE(FitStrapSide(a, 0.060f).ok);
    // The filter: the first fit at once, then eased (no flicker); a failed line holds it.
    StrapFilter f{};
    EXPECT_FALSE(f.valid);
    const StrapSide one = FitStrapSide(a, 0.032f);
    f.Step(one, a, 0.01f);
    EXPECT_TRUE(f.valid);
    EXPECT_NEAR(f.b_over_a * a, one.b, 1e-6f);
    const StrapSide two = FitStrapSide(a, 0.040f);
    f.Step(two, a, 1.0f / 60.0f);
    EXPECT_GT(f.b_over_a * a, one.b);
    EXPECT_LT(f.b_over_a * a, two.b);
    const float held = f.b_over_a;
    f.Step(FitStrapSide(a, -1.0f), a, 1.0f / 60.0f);
    EXPECT_EQ(f.b_over_a, held);
    // A side that stays unseen (the gun's grip in front of it) narrows: no strap held out wide in
    // the air from a fit made from another view.
    StrapFilter w{};
    StrapSide wide{};
    wide.ok = true;
    wide.b = 1.05f * a;
    wide.end = 0.92f;
    w.Step(wide, a, 0.01f);
    for (int i = 0; i < 12; ++i) {
        w.Step(FitStrapSide(a, -1.0f), a, 1.0f / 60.0f); // 0.2 s: held
    }
    EXPECT_NEAR(w.b_over_a, 1.05f, 1e-6f);
    for (int i = 0; i < 90; ++i) {
        w.Step(FitStrapSide(a, -1.0f), a, 1.0f / 60.0f); // 1.5 s more
    }
    EXPECT_NEAR(w.b_over_a, StrapStaleRatio, 0.01f);
    EXPECT_LE(w.end, StrapStaleEnd + 1e-6f);
    // ... and a narrow one stays as it was.
    StrapFilter nrw{};
    nrw.Step(one, a, 0.01f);
    for (int i = 0; i < 120; ++i) {
        nrw.Step(FitStrapSide(a, -1.0f), a, 1.0f / 60.0f);
    }
    EXPECT_NEAR(nrw.b_over_a * a, one.b, 1e-6f);
}

TEST(Vitals, Glance) {
    GlanceState g{};
    const V3 eye{0.0f, 0.0f, 0.0f};
    const V3 fwd{0.0f, 0.0f, 1.0f};
    // The gauge 0.4 m ahead, facing the eye: shown after 0.12 s, not before.
    const V3 p{0.05f, -0.05f, 0.4f};
    const V3 n{0.0f, 0.0f, -1.0f};
    EXPECT_FALSE(StepGlance(g, p, n, eye, fwd, 0.1f));
    EXPECT_TRUE(StepGlance(g, p, n, eye, fwd, 0.05f));
    // Looking away (40 degrees): hidden after 0.25 s.
    const V3 away{0.0f, -0.34f, 0.4f};
    EXPECT_TRUE(StepGlance(g, away, n, eye, fwd, 0.2f));
    EXPECT_FALSE(StepGlance(g, away, n, eye, fwd, 0.1f));
    // Facing away (the palm toward you) or too far: never.
    GlanceState h{};
    for (int i = 0; i < 10; ++i) {
        EXPECT_FALSE(StepGlance(h, p, V3{0.0f, 0.0f, 1.0f}, eye, fwd, 0.1f));
        EXPECT_FALSE(StepGlance(h, V3{0.0f, 0.0f, 0.9f}, n, eye, fwd, 0.1f));
    }
}

TEST(Vitals, DigitsStandUpright) {
    // +u up on the screen: 0; +u down: 1; +v up: 2; -v up: 3.
    EXPECT_EQ(DigitUp(0.0f, 1.0f, 1.0f, 0.0f, -1), 0);
    EXPECT_EQ(DigitUp(0.0f, -1.0f, 1.0f, 0.0f, -1), 1);
    EXPECT_EQ(DigitUp(1.0f, 0.0f, 0.0f, 1.0f, -1), 2);
    EXPECT_EQ(DigitUp(1.0f, 0.0f, 0.0f, -1.0f, -1), 3);
    // Hysteresis: at 45 degrees between +u and +v it keeps the current one.
    EXPECT_EQ(DigitUp(0.7071f, 0.7071f, -0.7071f, 0.7071f, 0), 0);
    EXPECT_EQ(DigitUp(0.7071f, 0.7071f, -0.7071f, 0.7071f, 2), 2);
    // Well past it: turns.
    EXPECT_EQ(DigitUp(0.94f, 0.34f, -0.34f, 0.94f, 0), 2);
}

TEST(Vitals, SettingsDefaults) {
    // The user's pick (2026-10-05): the wrist gauge, the heartbeat and the dark edges on; the
    // gauge 15% larger than the design (digits at least 10 mm).
    const VR::Settings d{};
    EXPECT_EQ(static_cast<Mode>(d.hud_vitals), Mode::Wrist);
    EXPECT_TRUE(d.vitals_heartbeat);
    EXPECT_TRUE(d.vitals_edge);
    EXPECT_NEAR(d.vitals_low_hp, 0.30f, 1e-6f);
    EXPECT_NEAR(d.vitals_heartbeat_mute_s, 20.0f, 1e-6f);
    EXPECT_NEAR(GaugeLengthMm, 52.9f, 0.01f);
    EXPECT_NEAR(GaugeWidthMm, 34.5f, 0.01f);
    EXPECT_GE(GaugeDigitMm, 10.0f);
    // The heartbeat's haptic is gentle: under the gun's recoil (1.0) after haptic_floor.
    const BeatShape shape{};
    EXPECT_LT(d.haptic_floor + (1.0f - d.haptic_floor) * shape.lub, 0.6f);
}

} // namespace

TEST(VitalsFitMotion, HoldsFitDuringMotionAndRejectsDelayedMovingProbe) {
    FitMotionGate gate;
    FitMotionGate::Pose pose{0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 1, 0};
    gate.Step(pose, .01f);
    for (int i = 0; i < 20; ++i)
        gate.Step(pose, .01f);
    ASSERT_TRUE(gate.Ready());
    gate.Submitted(1);
    EXPECT_TRUE(gate.Accept(1));
    pose[0] += .02f; // running in world space, even if unchanged relative to the eye
    EXPECT_FALSE(gate.Step(pose, .01f));
    gate.Submitted(2);
    for (int i = 0; i < 20; ++i)
        gate.Step(pose, .01f);
    EXPECT_FALSE(gate.Accept(1)); // pre-motion epoch
    EXPECT_FALSE(gate.Accept(2)); // moving at submission
    gate.Submitted(3);
    EXPECT_TRUE(gate.Accept(3));
    EXPECT_FALSE(gate.Accept(19)); // same ring slot, different id
}
TEST(VitalsFitMotion, RotationViewMotionAndHitchesHoldOnlyTheFit) {
    for (int component : {0, 3, 6, 9}) {
        FitMotionGate gate;
        FitMotionGate::Pose pose{0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 1, 0};
        for (int i = 0; i < 20; ++i)
            gate.Step(pose, .01f);
        ASSERT_TRUE(gate.Ready());
        pose[component] += .1f;
        EXPECT_FALSE(gate.Step(pose, .01f));
        EXPECT_EQ(gate.previous, pose); // current pose retained, never eased or delayed
        for (int i = 0; i < 20; ++i)
            gate.Step(pose, .01f);
        EXPECT_FALSE(gate.Step(pose, .2f));
    }
}
TEST(VitalsFitMotion, StableAtAllHeadsetRates) {
    for (float hz : {30.f, 60.f, 72.f, 80.f, 90.f, 120.f}) {
        FitMotionGate gate;
        FitMotionGate::Pose pose{};
        for (int i = 0; i < int(hz); ++i)
            gate.Step(pose, 1.f / hz);
        EXPECT_TRUE(gate.Ready());
        gate.Submitted(1);
        EXPECT_TRUE(gate.Accept(1));
    }
}
