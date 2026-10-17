// SPDX-License-Identifier: GPL-2.0-or-later

// Quick items in VR (core/vr/quick_items.h): the goods table, the held effects, the launch speed
// clamps, the release velocity, the arc's layout and hover, and the arc's controller (Y, the left
// grip and trigger, throws, uses, the game's own use, the gun, the vial, contexts).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/vr/gun_settings.h"
#include "core/vr/quick_items.h"
#include "core/vr/quick_settings.h"
#ifdef PI_QUICK_DLL_TEST
#include "quick_dll_controller.h"
#endif
#ifdef PI_QUICK_BOUNDED_TEST
#include "quick_allocation_probe.h"
#endif

namespace {

#ifdef PI_QUICK_DLL_TEST
namespace Q {
using namespace VR::Quick;
using Controller = QuickDllTest::Controller;
using Output = PhysicalInteractions::QuickOutput;
} // namespace Q
#elif defined(PI_QUICK_BOUNDED_TEST)
namespace Q {
using namespace VR::Quick;
using Controller = QuickAllocationProbe::Controller;
using Output = PhysicalInteractions::QuickOutput;
} // namespace Q
#else
namespace Q = VR::Quick;
#endif

constexpr double Dt = 1.0 / 90.0;

Q::Snapshot Slots() {
    Q::Snapshot s{};
    s.valid = true;
    s.slot[0] = {80, 1200, 5, 24, 20000, true}; // Molotov
    s.slot[1] = {81, 1120, 3, 18, 20009, true}; // Blue Elixir
    s.slot[2] = {82, 1110, 3, 20, -1, true};    // Beast Blood Pellet
    s.slot[3] = {79, 1411, 1, 31, -1, true};    // Monocular (the game's own use)
    s.selected = 0;
    return s;
}

// Drives a Controller frame by frame.
struct Rig {
    Q::Controller c;
    Q::Config cfg{};
    Q::Input in{};
    Q::Output out{};
    Q::Request sent{}; // the last request any step sent
    double t = 100.0;

    Rig() {
        in.gameplay = true;
        in.body = true;
        in.arm_weight = 1.0f;
        in.hand_valid = true;
        in.hand_p = {-0.2f, 1.1f, -0.3f};
        in.snapshot_fresh = true;
        in.snapshot = Slots();
    }
    const Q::Output& Step(int frames = 1) {
        for (int i = 0; i < frames; ++i) {
            t += Dt;
            in.t = t;
            in.dt = static_cast<float>(Dt);
            in.hover.steady_ns = static_cast<std::int64_t>(t * 1e9);
            c.Update(cfg, in, out);
            if (out.send) {
                sent = out.request;
            }
            in.x_edge = false;
        }
        return out;
    }
    void PressY() {
        in.y = true;
        Step();
        in.y = false;
    }
    void Hover(int slot) {
        in.hover_fresh = true;
        in.hover.open_serial = c.OpenSerial();
        in.hover.placed = true;
        in.hover.slot = static_cast<std::int8_t>(slot);
    }
    void Open() {
        PressY();
        Step();
    }
    void Grab(int slot) {
        Open();
        Hover(slot);
        Step(9); // hovers count from 80 ms after the open
        in.grip = 1.0f;
        Step();
    }
};

TEST(QuickItems, GoodsTable) {
    EXPECT_EQ(Q::Classify(1200).use, Q::UseKind::Throw);
    EXPECT_EQ(Q::Classify(1200).bullet, 100);
    EXPECT_EQ(Q::Classify(1250).bullet, 111); // the rope molotov's child, not its spawner
    EXPECT_EQ(Q::Classify(1240).throw_class, Q::ThrowClass::Knife);
    EXPECT_EQ(Q::Classify(1410).throw_class, Q::ThrowClass::Coin);
    for (const std::int32_t g : {1120, 1110, 1500, 1501, 1300, 1310, 1320, 1330, 701, 1510, 1519,
                                 1590, 1594}) {
        EXPECT_EQ(Q::Classify(g).use, Q::UseKind::Instant) << g;
    }
    for (const std::int32_t g : {1100, 1101, 2030}) {
        EXPECT_EQ(Q::Classify(g).use, Q::UseKind::Bullet) << g;
    }
    // Dialogs, multiplayer and toggles keep the game's own animation.
    for (const std::int32_t g : {100, 1400, 200, 205, 225, 111, 1411, 1412, 1700, 2000, 704}) {
        EXPECT_EQ(Q::Classify(g).use, Q::UseKind::Square) << g;
    }
    EXPECT_EQ(Q::Classify(-1).use, Q::UseKind::None);
}

TEST(QuickItems, HeldEffects) {
    EXPECT_EQ(Q::HeldFfx(3, 0), 20000);  // Molotov
    EXPECT_EQ(Q::HeldFfx(34, 20), 20020); // Delayed Molotov
    EXPECT_EQ(Q::HeldFfx(10, 9), 20009);  // Blue Elixir
    EXPECT_EQ(Q::HeldFfx(9, 95), 20095);  // Madman's Knowledge (a glow)
    EXPECT_EQ(Q::HeldFfx(0, 58), -1);     // Beast Blood Pellet: none (our mesh)
    EXPECT_EQ(Q::HeldFfx(7, 0), -1);      // coins: not the molotov's effect
    EXPECT_EQ(Q::HeldFfx(3, -1), -1);
    // None: our draw of the game's model (the effects flare where the right hand would be, or
    // cloud the view: the coldbloods' crush).
    EXPECT_FALSE(Q::UseHeldEffect(1200, 3));
    EXPECT_FALSE(Q::UseHeldEffect(1240, 2));
    EXPECT_FALSE(Q::UseHeldEffect(1120, 10));
    EXPECT_FALSE(Q::UseHeldEffect(1510, 9));
}

TEST(QuickItems, HeldLooks) {
    // The game's own models (the held effects' models in the sfx archive).
    EXPECT_EQ(Q::HeldLookOf(1200).model, 10010); // Molotov
    EXPECT_EQ(Q::HeldLookOf(1240).model, 10016); // Throwing Knife
    EXPECT_EQ(Q::HeldLookOf(1210).model, 10016); // Poison Knife
    EXPECT_EQ(Q::HeldLookOf(1220).model, 10014); // Oil Urn
    EXPECT_EQ(Q::HeldLookOf(1230).model, 10012); // Pebble
    EXPECT_EQ(Q::HeldLookOf(1270).model, 10009); // Numbing Mist
    EXPECT_EQ(Q::HeldLookOf(1101).model, 10022); // Sedative
    EXPECT_EQ(Q::HeldLookOf(1120).model, 10017); // Blue Elixir
    EXPECT_EQ(Q::HeldLookOf(2030).model, 10023); // Lead Elixir
    // No model in the game: our own (vr_goods_meshes.h) in its own grip, else the icon cut-out,
    // pinched.
    for (const std::int32_t g : {1110}) {
        EXPECT_EQ(Q::HeldLookOf(g).model, -1) << g;
        EXPECT_EQ(Q::HeldLookOf(g).grip, Q::GripPinch) << g;
    }
    EXPECT_EQ(Q::HeldLookOf(703).grip, Q::GripBottle);    // Iosefka's Blood Vial
    EXPECT_EQ(Q::HeldLookOf(701).grip, Q::GripVial);      // the blood tubes: the vial's own
    EXPECT_EQ(Q::HeldLookOf(1100).grip, Q::GripTray);     // Antidote
    EXPECT_EQ(Q::HeldLookOf(1510).grip, Q::GripCup);      // a coldblood
    EXPECT_EQ(Q::HeldLookOf(1500).grip, Q::GripCradle);   // Madman's Knowledge
    EXPECT_EQ(Q::HeldLookOf(120).grip, Q::GripBook);      // Notebook
    EXPECT_EQ(Q::HeldLookOf(205).grip, Q::GripHookPinch); // Small Resonant Bell
    EXPECT_EQ(Q::HeldLookOf(1300).grip, Q::GripPinch);  // Fire Paper, by its left end
    EXPECT_EQ(Q::HeldLookOf(1310).grip, Q::GripRubPinch);  // the Empty Phantasm Shell
    // Every look has a grip and a sane size.
    for (std::int32_t g = 0; g < 2200; ++g) {
        const Q::HeldLook look = Q::HeldLookOf(g);
        EXPECT_GE(look.grip, Q::GripNeck) << g;
        EXPECT_LE(look.grip, Q::GripRubPinch) << g;
        // Native-model fitting is baked into the approved absolute scale.
        EXPECT_GT(look.scale, 0.0f) << g;
        EXPECT_LE(look.scale, 2.0f) << g;
    }
}

TEST(QuickItems, LaunchSpeed) {
    // Real ballistics with a small boost: no minimum (a weak flick drops short), no cap yet.
    const Q::V3 slow{0.0f, 0.0f, -1.0f};
    EXPECT_NEAR(Q::Length(Q::LaunchVelocity(slow, Q::ThrowClass::Bottle, 1.2f)), 1.2f, 1e-4f);
    EXPECT_NEAR(Q::Length(Q::LaunchVelocity(slow, Q::ThrowClass::Knife, 1.2f)), 1.56f, 1e-3f);
    const Q::V3 v{0.0f, 3.0f, -4.0f}; // 5 m/s
    const Q::V3 bottle = Q::LaunchVelocity(v, Q::ThrowClass::Bottle, 1.2f);
    EXPECT_NEAR(Q::Length(bottle), 6.0f, 1e-4f);
    EXPECT_NEAR(bottle.y / bottle.z, v.y / v.z, 1e-5f); // the direction is kept
    EXPECT_NEAR(Q::Length(Q::LaunchVelocity(v, Q::ThrowClass::Knife, 1.2f)), 7.8f, 1e-3f);
    EXPECT_NEAR(Q::Length(Q::LaunchVelocity(v, Q::ThrowClass::Stone, 1.2f)), 7.2f, 1e-3f);
    EXPECT_NEAR(Q::Length(Q::LaunchVelocity(v, Q::ThrowClass::Coin, 1.2f)), 4.8f, 1e-3f);
    EXPECT_EQ(Q::Length(Q::LaunchVelocity({}, Q::ThrowClass::Bottle, 1.2f)), 0.0f);
    // The speed curve (throw_gain_curve): linear at 1; at 0.8 the same at 3 m/s (its reference),
    // a little faster below (a natural flick carries), a little slower above (room under the
    // cap), always rising, the direction kept.
    for (float s = 0.5f; s < 9.0f; s += 0.5f) {
        EXPECT_NEAR(Q::LaunchCurve(s, 1.0f), s, 1e-4f) << s;
    }
    EXPECT_NEAR(Q::LaunchCurve(3.0f, 0.8f), 3.0f, 1e-4f);
    EXPECT_NEAR(Q::LaunchCurve(2.0f, 0.8f) / 2.0f, 1.084f, 0.005f);
    EXPECT_NEAR(Q::LaunchCurve(5.0f, 0.8f) / 5.0f, 0.903f, 0.005f);
    float last = 0.0f;
    for (float s = 0.1f; s < 12.0f; s += 0.1f) {
        const float l = Q::LaunchCurve(s, 0.8f);
        EXPECT_GT(l, last) << s;
        last = l;
    }
    EXPECT_EQ(Q::LaunchCurve(0.0f, 0.8f), 0.0f);
    const Q::V3 curved = Q::LaunchVelocity(v, Q::ThrowClass::Bottle, 4.0f, 0.8f);
    EXPECT_NEAR(Q::Length(curved), 4.0f * Q::LaunchCurve(5.0f, 0.8f), 1e-3f);
    EXPECT_NEAR(curved.y / curved.z, v.y / v.z, 1e-5f);
    // The soft cap: untouched below its knee (half of it by default), then bent toward it, never
    // past it, never down.
    constexpr float Cap = 12.3f;
    EXPECT_NEAR(Q::Length(Q::SoftCap(v, Cap)), 5.0f, 1e-5f);
    const Q::V3 hard = Q::SoftCap(v * 3.0f, Cap);
    EXPECT_GT(Q::Length(hard), 0.75f * Cap);
    EXPECT_LT(Q::Length(hard), Cap);
    EXPECT_NEAR(hard.y / hard.z, v.y / v.z, 1e-5f);
    EXPECT_LT(Q::Length(Q::SoftCap(v * 40.0f, Cap)), Cap + 1e-3f);
    // The knee: 9 m/s against 12.3 is bent from 50%, not from 75% (the old default).
    EXPECT_LT(Q::Length(Q::SoftCap(slow * 9.0f, Cap)), 9.0f - 0.1f);
    EXPECT_NEAR(Q::Length(Q::SoftCap(slow * 9.0f, Cap, 0.75f)), 9.0f, 1e-5f);
    for (const float knee : {0.0f, 0.5f, 0.75f, 0.95f}) {
        float last = 0.0f;
        for (float k = 0.1f; k < 40.0f; k += 0.25f) {
            const float launch = Q::Length(Q::SoftCap(slow * k, Cap, knee));
            EXPECT_GE(launch, last - 1e-4f);
            EXPECT_LT(launch, Cap + 1e-3f);
            last = launch;
        }
    }
    // A harder throw always leaves faster up to the cap: 3, 5 and 7 m/s by the hand at the
    // default gain against a Molotov's 14 m/s stay apart (the cap does not flatten them alike).
    const float g = Q::Config{}.throw_gain;
    const float c5 = Q::Length(Q::SoftCap(slow * (5.0f * g), 14.0f));
    const float c7 = Q::Length(Q::SoftCap(slow * (7.0f * g), 14.0f));
    const float c3 = Q::Length(Q::SoftCap(slow * (3.0f * g), 14.0f));
    EXPECT_GT(c5, 1.02f * c3);
    EXPECT_GT(c7, 1.01f * c5);
}

// The game's own throws, from its BulletParam rows (Bloodborne 1.09, plan_throwables.md 2.1).
constexpr Q::BulletFlight Molotov{4.0f, 6.0f, 8.0f, 12.0f, 15.0f, 0.0f, -8.0f, 50.0f, 15.0f, 0.0f,
                                  10.0f};
// The throwing knife's row as the game has it at run time (accelInRange 10: it speeds up over its
// first 8 m).
constexpr Q::BulletFlight Knife{2.0f, 8.0f, 0.0f, 9.8f, 20.0f, 10.0f, 0.0f, 50.0f, 10.0f, 0.0f,
                                0.0f};
constexpr Q::BulletFlight Pebble{4.0f, 10.0f, 3.0f, 16.0f, 20.0f, 0.0f, -8.0f, 20.0f, 16.0f, 0.0f,
                                 2.0f};
constexpr Q::BulletFlight Coins{2.04f, 20.0f, 9.8f, 9.8f, 2.0f, 0.0f, 0.0f, 20.0f, 2.0f, 0.0f,
                                -20.0f};

TEST(QuickItems, VanillaRangeAndCap) {
    // The game's throw from 1.5 m: a Molotov flies 15 m/s (its minimum) under gravity 8, then
    // 12 after 6 m, and lands about 13.4 m away; a knife flies flat (and faster) for 8 m first.
    EXPECT_NEAR(Q::VanillaRange(Molotov, 1.5f), 13.42f, 0.15f);
    EXPECT_NEAR(Q::VanillaRange(Knife, 1.5f), 20.9f, 0.4f); // +-1 frame at the 8 m switch
    EXPECT_NEAR(Q::VanillaRange(Pebble, 1.5f), 17.33f, 0.2f);
    // The slowest real-gravity launch that reaches a range: 45 degrees on level ground.
    EXPECT_NEAR(Q::SpeedForRange(10.0f, 0.0f, 9.81f), std::sqrt(98.1f), 1e-3f);
    // From 1.5 m up, at that speed, the best arc reaches exactly that far.
    const float v = Q::SpeedForRange(12.0f, 1.5f, 9.81f);
    float best = 0.0f;
    for (float a = 0.0f; a < 80.0f; a += 0.1f) {
        const float vx = v * std::cos(a * 0.01745329f), vy = v * std::sin(a * 0.01745329f);
        const float t = (vy + std::sqrt(vy * vy + 2.0f * 9.81f * 1.5f)) / 9.81f;
        best = std::max(best, vx * t);
    }
    EXPECT_NEAR(best, 12.0f, 0.02f);
    // The caps: the best arc reaches 1.25 x the game's throw.
    float range = 0.0f;
    EXPECT_NEAR(Q::ThrowCapOf(Molotov, Q::ThrowClass::Bottle, 1.25f, 1.5f, 9.81f, &range), 12.27f,
                0.1f);
    EXPECT_NEAR(range, 13.42f, 0.15f);
    EXPECT_NEAR(Q::ThrowCapOf(Knife, Q::ThrowClass::Knife, 1.25f, 1.5f, 9.81f), 15.6f, 0.2f);
    EXPECT_NEAR(Q::ThrowCapOf(Pebble, Q::ThrowClass::Stone, 1.25f, 1.5f, 9.81f), 14.1f, 0.1f);
    // The class caps (the fallback when a row cannot be read) match the rows' at the default
    // range factor (1.6: the best arc 1.6 x the game's own throw).
    EXPECT_NEAR(Q::LimitsOf(Q::ThrowClass::Bottle).max_speed,
                Q::ThrowCapOf(Molotov, Q::ThrowClass::Bottle, 1.6f, 1.5f, 9.81f), 0.1f);
    EXPECT_NEAR(Q::LimitsOf(Q::ThrowClass::Knife).max_speed,
                Q::ThrowCapOf(Knife, Q::ThrowClass::Knife, 1.6f, 1.5f, 9.81f), 0.15f);
    EXPECT_NEAR(Q::LimitsOf(Q::ThrowClass::Stone).max_speed,
                Q::ThrowCapOf(Pebble, Q::ThrowClass::Stone, 1.6f, 1.5f, 9.81f), 0.1f);
    // Coins are dropped, not thrown, by the game (pitched down, 0.9 m): the class's cap.
    EXPECT_EQ(Q::ThrowCapOf(Coins, Q::ThrowClass::Coin, 1.25f, 1.5f, 9.81f),
              Q::LimitsOf(Q::ThrowClass::Coin).max_speed);
    // A longer reach for the same item: a higher cap.
    EXPECT_GT(Q::ThrowCapOf(Molotov, Q::ThrowClass::Bottle, 1.5f, 1.5f, 9.81f), 12.5f);
}

TEST(QuickItems, LockOnAssist) {
    // A throw straight ahead (-Z), 2 m/s up.
    const Q::V3 v{0.0f, 2.0f, -8.0f};
    const auto target_at = [](float yaw_deg) { // + = to the left (-X)
        const float a = yaw_deg * 0.01745329f;
        return Q::V3{-6.0f * std::sin(a), -1.0f, -6.0f * std::cos(a)};
    };
    float turn = 0.0f, off = 0.0f;
    // A near-hit 5 degrees off: 22% of the way, the pitch and the horizontal speed kept.
    Q::V3 a = Q::YawAssist(v, target_at(5.0f), 0.22f, 9.0f, turn, off);
    EXPECT_NEAR(off, 5.0f, 0.01f);
    EXPECT_NEAR(std::abs(turn), 1.1f, 0.01f);
    EXPECT_NEAR(a.y, v.y, 1e-5f);
    EXPECT_NEAR(std::hypot(a.x, a.z), 8.0f, 1e-4f);
    EXPECT_LT(a.x, 0.0f); // toward the target (left)
    // The other side.
    a = Q::YawAssist(v, target_at(-5.0f), 0.22f, 9.0f, turn, off);
    EXPECT_GT(a.x, 0.0f);
    // Fading out toward the cone's edge, nothing beyond: a clear miss is never pulled in.
    Q::YawAssist(v, target_at(8.0f), 0.22f, 9.0f, turn, off);
    EXPECT_GT(std::abs(turn), 0.0f);
    EXPECT_LT(std::abs(turn), 0.5f);
    a = Q::YawAssist(v, target_at(12.0f), 0.22f, 9.0f, turn, off);
    EXPECT_EQ(turn, 0.0f);
    EXPECT_EQ(a.x, v.x);
    EXPECT_EQ(a.z, v.z);
    // Knives: a wider cone.
    Q::YawAssist(v, target_at(12.0f), 0.22f, 15.0f, turn, off);
    EXPECT_GT(std::abs(turn), 0.5f);
    // Off: nothing.
    a = Q::YawAssist(v, target_at(3.0f), 0.0f, 9.0f, turn, off);
    EXPECT_EQ(turn, 0.0f);
    // The assisted throw never ends nearer the target's line than the unassisted one would be
    // further off, and the turn never exceeds strength x the error.
    for (float yaw = -20.0f; yaw <= 20.0f; yaw += 0.5f) {
        Q::YawAssist(v, target_at(yaw), 0.22f, 9.0f, turn, off);
        EXPECT_LE(std::abs(turn), 0.22f * std::abs(yaw) + 1e-3f) << yaw;
    }
    // The defaults: the user's 30% (2026-10-07 morning; 0.26 the night before, 0.27 on 10-06,
    // 0.22 before), the cones unchanged.
    const Q::QuickSettings d{};
    EXPECT_NEAR(d.throw_assist, 0.30f, 1e-6f);
    EXPECT_EQ(d.throw_assist_deg, 9.0f);
    EXPECT_EQ(d.throw_assist_knife_deg, 15.0f);
}

// The knife's real-gravity flight from `from` with velocity `v`: how far from `target` it passes
// (the closest point of the arc, stepped finely).
float MissDistance(Q::V3 from, Q::V3 v, Q::V3 target, float g) {
    float best = 1e9f;
    Q::V3 p = from;
    for (int i = 0; i < 4000; ++i) {
        best = std::min(best, Q::Length(p - target));
        const float dt = 0.0005f;
        p = p + v * dt;
        v.y -= g * dt;
    }
    return best;
}

Q::V3 Yawed(float speed, float yaw_deg, float pitch_deg) { // yaw + = left (-X), forward -Z
    const float y = yaw_deg * 0.01745329f, p = pitch_deg * 0.01745329f;
    return {-speed * std::cos(p) * std::sin(y), speed * std::sin(p), -speed * std::cos(p) * std::cos(y)};
}

TEST(QuickItems, BallisticDirHitsThePoint) {
    for (const Q::V3 to : {Q::V3{0.0f, -0.5f, -8.0f}, Q::V3{3.0f, 0.4f, -6.0f},
                           Q::V3{-1.0f, -1.2f, -12.0f}}) {
        Q::V3 d{};
        ASSERT_TRUE(Q::BallisticDir(to, 20.0f, 9.81f, d));
        EXPECT_NEAR(Q::Length(d), 1.0f, 1e-4f);
        EXPECT_LT(MissDistance({}, d * 20.0f, to, 9.81f), 0.02f);
        EXPECT_GT(d.y / Q::Length(d), to.y / Q::Length(to)); // above the line: the drop
    }
    // Out of reach at that speed (a flick at 4 m/s, 20 m): no arc.
    Q::V3 d{};
    EXPECT_FALSE(Q::BallisticDir({0.0f, 0.0f, -20.0f}, 4.0f, 9.81f, d));
}

TEST(QuickItems, KnifeAssist) {
    const float g = 9.81f;
    // knife_assist 0.85 (2026-10-07; 0.70 on 10-06).
    const float kKnife = Q::QuickSettings{}.knife_assist;
    const Q::V3 from{0.0f, 1.5f, 0.0f};
    // The beast's lock point 1.0 m up, 5-10 m ahead; knives thrown 10-15 degrees off it.
    for (const float dist : {5.0f, 7.5f, 10.0f}) {
        const Q::V3 target{0.0f, 1.0f, -dist};
        for (const float yaw : {-15.0f, -12.0f, -10.0f, 10.0f, 12.0f, 15.0f}) {
            // Aimed along the line of sight (no allowance for the drop), off by `yaw`.
            const Q::V3 los = Q::Normalize(target - from);
            const float pitch = std::asin(los.y) * 57.29578f;
            const Q::V3 v = Yawed(22.0f, yaw, pitch);
            Q::KnifeOut k{};
            const Q::V3 a = Q::KnifeAssist(v, from, &target, 1, -1, g, kKnife, 15.0f, 40.0f, k);
            ASSERT_EQ(k.target, 0);
            EXPECT_TRUE(k.reachable);
            EXPECT_NEAR(Q::Length(a), 22.0f, 1e-3f); // the speed is kept
            EXPECT_NEAR(k.weight, kKnife * Q::KnifeWeight(k.off_deg, 1.0f, 15.0f), 1e-4f);
            if (k.off_deg <= 15.0f) {
                EXPECT_NEAR(k.weight, kKnife, 1e-4f); // full strength within 15 degrees
            }
            EXPECT_NEAR(k.after_deg, (1.0f - k.weight) * k.off_deg, 0.05f);
            std::printf("knife %4.1f m %+5.1f deg: %.1f deg off the arc -> %.1f, passes %.2f m -> "
                        "%.2f m from the lock point\n",
                        dist, yaw, k.off_deg, k.after_deg, MissDistance(from, v, target, g),
                        MissDistance(from, a, target, g));
            const float miss0 = MissDistance(from, v, target, g);
            const float miss1 = MissDistance(from, a, target, g);
            EXPECT_LT(miss1, 0.3f * miss0) << dist << " m, " << yaw << " deg";
            // Well within the beast's half-width (~0.6 m) up to 15 degrees off at 7.5 m (0.85:
            // 0.21 / 0.25 / 0.38 m at 10 / 12 / 15 degrees; 0.70 left 0.43 / 0.50 / 0.67).
            if (std::abs(yaw) <= 15.0f && dist <= 7.5f) {
                EXPECT_LT(miss1, 0.4f) << dist << " m, " << yaw << " deg";
            }
        }
    }
    // The drop: a throw straight at a target 10 m away falls ~1 m short of it unassisted; most
    // of that is taken out.
    {
        const Q::V3 target{0.0f, 1.0f, -10.0f};
        const Q::V3 v = Q::Normalize(target - from) * 20.0f;
        Q::KnifeOut k{};
        const Q::V3 a = Q::KnifeAssist(v, from, &target, 1, -1, g, kKnife, 15.0f, 40.0f, k);
        EXPECT_GT(MissDistance(from, v, target, g), 0.9f);
        EXPECT_LT(MissDistance(from, a, target, g), 0.45f);
        EXPECT_GT(k.off_deg, 5.0f);
        EXPECT_GT(a.y, v.y); // aimed higher
    }
    // Fading from 15 to 20 degrees, nothing beyond: a clear miss stays a miss.
    {
        const Q::V3 target{0.0f, 1.5f, -8.0f};
        const auto weight_at = [&](float yaw) {
            // At the arc's own pitch, off by `yaw` only.
            Q::KnifeOut k{};
            Q::V3 d{};
            Q::BallisticDir(target - from, 22.0f, g, d);
            const Q::V3 aimed = Yawed(22.0f, yaw, std::asin(d.y) * 57.29578f);
            Q::KnifeAssist(aimed, from, &target, 1, -1, g, kKnife, 15.0f, 40.0f, k);
            return k.weight;
        };
        EXPECT_NEAR(weight_at(14.0f), kKnife, 1e-3f);
        const float w17 = weight_at(17.5f);
        EXPECT_GT(w17, 0.1f);
        EXPECT_LT(w17, 0.6f);
        EXPECT_EQ(weight_at(20.5f), 0.0f);
        EXPECT_EQ(weight_at(30.0f), 0.0f);
    }
    // Two enemies: the one nearest the throw's line; the lock-on target wins while in the cone.
    {
        const std::array<Q::V3, 2> two{Q::V3{-1.2f, 1.0f, -8.0f}, Q::V3{1.0f, 1.0f, -8.0f}};
        const Q::V3 v = Yawed(22.0f, -5.0f, 2.0f); // toward +X (the second)
        Q::KnifeOut k{};
        Q::KnifeAssist(v, from, two.data(), 2, -1, g, kKnife, 15.0f, 40.0f, k);
        EXPECT_EQ(k.target, 1);
        Q::KnifeAssist(v, from, two.data(), 2, 0, g, kKnife, 15.0f, 40.0f, k);
        EXPECT_EQ(k.target, 0);
        // A lock-on target far off the throw: the nearest one instead.
        const std::array<Q::V3, 2> wide{Q::V3{-8.0f, 1.0f, -4.0f}, Q::V3{1.0f, 1.0f, -8.0f}};
        Q::KnifeAssist(v, from, wide.data(), 2, 0, g, kKnife, 15.0f, 40.0f, k);
        EXPECT_EQ(k.target, 1);
    }
    // Out of reach (a weak flick): its heading only, the pitch kept.
    {
        const Q::V3 target{0.0f, 1.0f, -20.0f};
        const Q::V3 v = Yawed(6.0f, 8.0f, 10.0f);
        Q::KnifeOut k{};
        const Q::V3 a = Q::KnifeAssist(v, from, &target, 1, -1, g, kKnife, 15.0f, 40.0f, k);
        EXPECT_FALSE(k.reachable);
        EXPECT_NEAR(Q::PitchOf(a), Q::PitchOf(v), 0.05f);
        EXPECT_LT(std::abs(a.x), std::abs(v.x));
    }
    // Off, no target, too near or too far: unchanged.
    {
        const Q::V3 target{0.0f, 1.0f, -8.0f};
        const Q::V3 v = Yawed(22.0f, 5.0f, 3.0f);
        Q::KnifeOut k{};
        Q::V3 a = Q::KnifeAssist(v, from, &target, 1, -1, g, 0.0f, 15.0f, 40.0f, k);
        EXPECT_EQ(k.target, -1);
        EXPECT_EQ(a.x, v.x);
        a = Q::KnifeAssist(v, from, &target, 0, -1, g, kKnife, 15.0f, 40.0f, k);
        EXPECT_EQ(k.target, -1);
        const Q::V3 near{0.0f, 1.5f, -0.3f};
        Q::KnifeAssist(v, from, &near, 1, -1, g, kKnife, 15.0f, 40.0f, k);
        EXPECT_EQ(k.target, -1);
        Q::KnifeAssist(v, from, &target, 1, -1, g, kKnife, 15.0f, 5.0f, k);
        EXPECT_EQ(k.target, -1);
        const Q::V3 bad{NAN, 1.0f, -8.0f};
        a = Q::KnifeAssist(v, from, &bad, 1, -1, g, kKnife, 15.0f, 40.0f, k);
        EXPECT_EQ(k.target, -1);
        EXPECT_EQ(a.z, v.z);
    }
    const Q::QuickSettings d{};
    EXPECT_NEAR(d.knife_assist, 0.85f, 1e-6f);
    EXPECT_EQ(d.knife_assist_deg, 15.0f);
}

// The user's 2026-10-07 answer: regular throws 30% locked on and also helped unlocked (0.26, toward
// the lock-on candidate nearest the throw's heading); the knives keep their own assist; the gun
// stays lock-on only (gun_assist 0.27, gun_assist_unlocked 0).
TEST(QuickItems, UnlockedThrowAssist) {
    const Q::QuickSettings d{};
    EXPECT_NEAR(d.throw_assist, 0.30f, 1e-6f);
    EXPECT_NEAR(d.throw_assist_unlocked, 0.26f, 1e-6f);
    EXPECT_EQ(d.throw_assist_deg, 9.0f);
    EXPECT_NEAR(d.knife_assist, 0.85f, 1e-6f);
    EXPECT_EQ(d.knife_assist_deg, 15.0f);
    // The gun is unchanged: 27% locked on, nothing unlocked.
    const VR::Gun::GunSettings gun{};
    EXPECT_NEAR(gun.assist, 0.27f, 1e-6f);
    EXPECT_EQ(gun.assist_deg, 8.0f);
    EXPECT_FALSE(gun.assist_unlocked);

    // A Molotov lobbed straight ahead (-Z) from the hand; the beast's lock point 7 m away.
    const Q::V3 from{0.0f, 1.5f, 0.0f};
    const Q::V3 v = Yawed(9.0f, 0.0f, 30.0f);
    const auto at = [&](float yaw_deg, float dist) { // + = to the left (-X)
        const float a = yaw_deg * 0.01745329f;
        return Q::V3{-dist * std::sin(a), 1.0f, -dist * std::cos(a)};
    };
    // Full strength within 60% of the cone (5.4 degrees): 5 degrees off -> 0.30 x 5 = 1.50 locked
    // on, 0.26 x 5 = 1.30 unlocked; the mock's 6 degrees (the fade's 0.926) -> 1.67 / 1.44.
    for (const auto& [yaw, locked, unlocked] :
         {std::array<float, 3>{5.0f, 1.50f, 1.30f}, std::array<float, 3>{6.0f, 1.667f, 1.444f}}) {
        const Q::V3 target = at(yaw, 7.0f);
        float turn = 0.0f, off = 0.0f;
        Q::V3 a = Q::YawAssist(v, target - from, d.throw_assist, d.throw_assist_deg, turn, off);
        EXPECT_NEAR(off, yaw, 0.01f);
        EXPECT_NEAR(turn, locked, 0.01f) << yaw;
        a = Q::YawAssist(v, target - from, d.throw_assist_unlocked, d.throw_assist_deg, turn, off);
        EXPECT_NEAR(turn, unlocked, 0.01f) << yaw;
        // The speed and the pitch never change.
        EXPECT_NEAR(Q::Length(a), Q::Length(v), 1e-4f);
        EXPECT_NEAR(a.y, v.y, 1e-6f);
        EXPECT_LT(a.x, 0.0f); // toward it (left)
    }

    // The unlocked pick: the candidate whose heading is nearest the throw's, whatever its distance
    // or height (the yaw is all the assist turns).
    float off = 0.0f;
    {
        const std::array<Q::V3, 3> three{at(-7.0f, 6.0f), at(4.0f, 15.0f), at(20.0f, 4.0f)};
        EXPECT_EQ(Q::PickYawTarget(v, from, three.data(), 3, 9.0f, 40.0f, off), 1);
        EXPECT_NEAR(off, 4.0f, 0.01f);
        Q::V3 lower = at(-2.0f, 9.0f);
        lower.y = -4.0f; // on the street below
        const std::array<Q::V3, 2> two{at(3.0f, 6.0f), lower};
        EXPECT_EQ(Q::PickYawTarget(v, from, two.data(), 2, 9.0f, 40.0f, off), 1);
        EXPECT_NEAR(off, 2.0f, 0.01f);
    }
    // None outside the cone (either side), none with no candidates, none too near, too far,
    // straight below or not finite: the throw stays as it was.
    {
        const std::array<Q::V3, 2> wide{at(9.5f, 6.0f), at(-12.0f, 6.0f)};
        EXPECT_EQ(Q::PickYawTarget(v, from, wide.data(), 2, 9.0f, 40.0f, off), -1);
        EXPECT_EQ(off, 0.0f);
        EXPECT_EQ(Q::PickYawTarget(v, from, wide.data(), 0, 9.0f, 40.0f, off), -1);
        EXPECT_EQ(Q::PickYawTarget(v, from, nullptr, 0, 9.0f, 40.0f, off), -1);
        const std::array<Q::V3, 4> odd{at(2.0f, 0.3f), at(2.0f, 45.0f),
                                       Q::V3{0.1f, -6.0f, -0.2f}, Q::V3{NAN, 1.0f, -6.0f}};
        EXPECT_EQ(Q::PickYawTarget(v, from, odd.data(), 4, 9.0f, 40.0f, off), -1);
        // Off (cone 0) or a throw with no heading (straight down).
        const std::array<Q::V3, 1> one{at(2.0f, 6.0f)};
        EXPECT_EQ(Q::PickYawTarget(v, from, one.data(), 1, 0.0f, 40.0f, off), -1);
        EXPECT_EQ(Q::PickYawTarget({0.0f, -3.0f, 0.0f}, from, one.data(), 1, 9.0f, 40.0f, off), -1);
        EXPECT_EQ(Q::PickYawTarget(v, from, one.data(), 1, 9.0f, 40.0f, off), 0);
    }
    // Behind the throw: never picked.
    {
        const std::array<Q::V3, 1> behind{at(178.0f, 6.0f)};
        EXPECT_EQ(Q::PickYawTarget(v, from, behind.data(), 1, 9.0f, 40.0f, off), -1);
    }
    // Knives keep their own assist: the knife assist's strength and its choice of target are not
    // the unlocked yaw assist's (two enemies: the nearest to the knife's line, 85%).
    {
        const std::array<Q::V3, 2> two{Q::V3{-1.2f, 1.0f, -8.0f}, Q::V3{1.0f, 1.0f, -8.0f}};
        const Q::V3 knife = Yawed(22.0f, -5.0f, 2.0f);
        Q::KnifeOut k{};
        Q::KnifeAssist(knife, from, two.data(), 2, -1, 9.81f, d.knife_assist, d.knife_assist_deg,
                       40.0f, k);
        EXPECT_EQ(k.target, 1);
        EXPECT_NEAR(k.weight, 0.85f, 1e-4f);
    }
}

TEST(QuickItems, ReleaseAtTheItem) {
    // The angular velocity between two orientations: 0.1 rad about Y in 10 ms.
    const Q::Qt a{};
    const Q::Qt b{0.0f, std::sin(0.05f), 0.0f, std::cos(0.05f)};
    const Q::V3 w = Q::AngularVelocity(a, b, 0.01f);
    EXPECT_NEAR(w.y, 10.0f, 1e-3f);
    EXPECT_NEAR(w.x, 0.0f, 1e-5f);
    // The other way round, and through the quaternion's double cover.
    EXPECT_NEAR(Q::AngularVelocity(b, a, 0.01f).y, -10.0f, 1e-3f);
    EXPECT_NEAR(Q::AngularVelocity(a, {-b.x, -b.y, -b.z, -b.w}, 0.01f).y, 10.0f, 1e-3f);
    // The spin at a Molotov's bottle (12 cm below the fist, grip +Z) for a wrist snap about X.
    const Q::V3 r = Q::ReleaseOffsetOf(1200);
    EXPECT_NEAR(r.z, 0.12f, 1e-6f);
    const Q::V3 s = Q::SpinVelocity({-10.0f, 0.0f, 0.0f}, {}, r);
    EXPECT_NEAR(s.y, 1.2f, 1e-4f);
    EXPECT_NEAR(s.z, 0.0f, 1e-4f);
    // Turned with the grip: a controller yawed 90 degrees left puts the bottle on +X.
    const Q::Qt yawed{0.0f, std::sin(0.785398f), 0.0f, std::cos(0.785398f)};
    EXPECT_NEAR(Q::Rotate(yawed, {0.0f, 0.0f, 0.12f}).x, 0.12f, 1e-4f);
    // At most 2.5 m/s.
    EXPECT_NEAR(Q::Length(Q::SpinVelocity({-40.0f, 0.0f, 0.0f}, {}, r)), 2.5f, 1e-4f);
    // Items held in the fist: no offset.
    EXPECT_EQ(Q::Length(Q::ReleaseOffsetOf(1410)), 0.0f);
    EXPECT_LT(Q::ReleaseOffsetOf(1240).z, 0.0f); // the knife's blade out of the thumb's side
}

TEST(QuickItems, PathTurn) {
    // A straight push: no turn.
    Q::VelocityHistory push;
    for (int i = 0; i <= 20; ++i) {
        push.Push(i * Dt, Q::V3{0.0f, 0.5f, -1.0f} * (0.3f * static_cast<float>(i)));
    }
    EXPECT_LT(push.Turn(20 * Dt, push.Release(20 * Dt)), 1.0f);
    // An overhand arc: the hand first rises and comes forward over the shoulder.
    Q::VelocityHistory arc;
    for (int i = 0; i <= 20; ++i) {
        const float k = static_cast<float>(i) / 20.0f;
        const float a = (80.0f - 85.0f * k) * 0.01745329f; // from up-back to forward
        arc.Push(i * Dt, Q::V3{0.0f, std::sin(a), -std::cos(a)} * (1.0f + 6.0f * k));
    }
    EXPECT_GT(arc.Turn(20 * Dt, arc.Release(20 * Dt)), 60.0f);
    // Slow samples (under 35% of the release speed or 1 m/s) do not count (the release itself is
    // the peak averaged with its neighbours: 5 degrees up here).
    Q::VelocityHistory slow;
    slow.Push(0.0, {0.0f, 0.5f, 0.0f});
    slow.Push(Dt, {0.0f, 0.0f, -6.0f});
    EXPECT_LT(slow.Turn(Dt, slow.Release(Dt)), 6.0f);
}

TEST(QuickItems, ThrowPower) {
    // A wind-up from the shoulder, or a whip of the forearm and wrist (the user's real overhand
    // throws: 19-37 rad/s): the full gain.
    EXPECT_NEAR(Q::ThrowPower(-0.05f, 1.0f, 0.0f, 0.75f), 1.0f, 1e-4f);
    EXPECT_NEAR(Q::ThrowPower(0.30f, 19.0f, 0.0f, 0.75f), 1.0f, 1e-4f);
    // A push with some turn of the wrist (up to 8 rad/s) is still a push.
    EXPECT_NEAR(Q::ThrowPower(0.30f, 8.0f, 0.0f, 0.75f), 0.75f, 1e-4f);
    EXPECT_GT(Q::ThrowPower(0.30f, 13.0f, 0.0f, 0.75f), 0.80f);
    EXPECT_LT(Q::ThrowPower(0.30f, 13.0f, 0.0f, 0.75f), 0.95f);
    // ... or an arm swung round (the hand's path turned).
    EXPECT_NEAR(Q::ThrowPower(0.35f, 1.0f, 70.0f, 0.75f), 1.0f, 1e-4f);
    // A straight push from in front of the chest: a tad weaker.
    EXPECT_NEAR(Q::ThrowPower(0.35f, 1.5f, 10.0f, 0.75f), 0.75f, 1e-4f);
    // Continuous in between, never outside [push, 1].
    float last = 2.0f;
    for (float d = -0.1f; d < 0.45f; d += 0.01f) {
        const float p = Q::ThrowPower(d, 2.0f, 0.0f, 0.75f);
        EXPECT_LE(p, last + 1e-6f);
        EXPECT_GE(p, 0.75f - 1e-6f);
        last = p;
    }
    last = 0.0f;
    for (float turn = 0.0f; turn < 90.0f; turn += 1.0f) {
        const float p = Q::ThrowPower(0.35f, 1.0f, turn, 0.75f);
        EXPECT_GE(p, last - 1e-6f);
        last = p;
    }
    // The wind-up along the throw's horizontal direction; a straight-up throw counts as full.
    Q::WindupHistory w;
    for (int i = 0; i <= 30; ++i) {
        const float k = static_cast<float>(i) / 30.0f;
        w.Push(i * Dt, {0.0f, 0.0f, 0.10f - 0.45f * k}, 1.0f); // from 10 cm behind to 35 in front
    }
    float windup = 0.0f;
    float omega = 0.0f;
    w.Measure(30 * Dt, {0.0f, 1.0f, -5.0f}, 0.0f, windup, omega);
    EXPECT_NEAR(windup, -0.10f, 1e-4f);
    EXPECT_NEAR(omega, 1.0f, 1e-4f);
    w.Measure(30 * Dt, {0.0f, 6.0f, -0.5f}, 0.0f, windup, omega);
    EXPECT_EQ(windup, 0.0f);
}

TEST(QuickItems, ReleaseVelocity) {
    Q::VelocityHistory h;
    // A swing peaking at 6 m/s 40 ms before the release, slowing as the fingers open.
    for (int i = 0; i <= 20; ++i) {
        const double t = i * Dt;
        const float speed = 6.0f * std::exp(-std::pow(static_cast<float>(t - 0.18) / 0.06f, 2.0f));
        h.Push(t, {0.0f, 0.2f * speed, -speed});
    }
    const Q::V3 v = h.Release(20 * Dt);
    EXPECT_GT(Q::Length(v), 4.5f);
    EXPECT_LT(v.z, 0.0f);
    // Nothing recent (the hand was not tracked just before the release): no velocity at all.
    EXPECT_EQ(Q::Length(h.Release(5.0)), 0.0f);
}

TEST(QuickItems, ThrowPeakBeforeRelease) {
    // The user's observation: the hand peaks (6 m/s) ~180 ms before the fingers open and is down
    // to 2.5 m/s at the release. Release() alone sees the slow end; PeakBefore finds the peak.
    Q::VelocityHistory h;
    for (int i = 0; i <= 30; ++i) {
        const double t = i * Dt;
        const float speed =
            2.5f + 3.5f * std::exp(-std::pow(static_cast<float>(t - 0.15) / 0.035f, 2.0f));
        h.Push(t, {0.0f, 0.2f * speed, -speed});
    }
    const double now = 30 * Dt;
    const Q::V3 rel = h.Release(now);
    EXPECT_LT(Q::Length(rel), 3.2f);
    const Q::V3 pk = h.PeakBefore(now, 0.30, rel);
    EXPECT_GT(Q::Length(pk), 5.0f);
    EXPECT_LT(pk.z, 0.0f);
    // A window that ends before the peak finds only the slow tail.
    EXPECT_LT(Q::Length(h.PeakBefore(now, 0.10, rel)), 3.2f);
    // A wind-up away from the target (fast backwards, then a slow forward release) never counts.
    Q::VelocityHistory back;
    for (int i = 0; i <= 30; ++i) {
        const double t = i * Dt;
        back.Push(t, i < 20 ? Q::V3{0.0f, 0.0f, 6.0f} : Q::V3{0.0f, 0.0f, -2.0f});
    }
    const Q::V3 rel_b = back.Release(now);
    EXPECT_LT(Q::Length(back.PeakBefore(now, 0.30, rel_b)), 2.5f);
    // A hand that stopped (put back slowly) never throws from an earlier fast move.
    Q::VelocityHistory stop;
    for (int i = 0; i <= 30; ++i) {
        stop.Push(i * Dt, i < 12 ? Q::V3{0.0f, 0.0f, -5.0f} : Q::V3{0.0f, 0.0f, -0.2f});
    }
    EXPECT_EQ(Q::Length(stop.PeakBefore(now, 0.30, stop.Release(now))), 0.0f);
}

TEST(QuickItems, SpikeSamplesAreNeverTheRelease) {
    // A one-frame tracking jump (20 m/s, under throw_max_hand_speed) 230 ms before an ordinary
    // 3 m/s release, and one at the release itself: neither is read.
    Q::VelocityHistory h;
    const Q::V3 fwd = Q::Normalize(Q::V3{0.0f, 0.3f, -1.0f});
    for (int i = 0; i <= 30; ++i) {
        h.Push(i * Dt, i == 9 ? Q::Normalize(Q::V3{0.3f, -0.6f, -1.0f}) * 20.0f : fwd * 3.0f);
    }
    const double now = 30 * Dt;
    EXPECT_TRUE(h.Spike(9, now));
    EXPECT_FALSE(h.Spike(10, now));
    const Q::V3 rel = h.Release(now);
    EXPECT_NEAR(Q::Length(rel), 3.0f, 0.01f);
    EXPECT_EQ(Q::Length(h.PeakBefore(now, 0.30, rel)), 3.0f);
    // The newest sample has no later neighbour yet: not judged (throw_max_hand_speed guards it).
    Q::VelocityHistory last;
    for (int i = 0; i <= 30; ++i) {
        last.Push(i * Dt, i == 30 ? fwd * 14.0f : fwd * 3.0f);
    }
    EXPECT_FALSE(last.Spike(30, now));
    // A real hand speeding up (5 -> 9 m/s over a few frames) is never a spike.
    Q::VelocityHistory fast;
    for (int i = 0; i <= 10; ++i) {
        fast.Push(i * Dt, fwd * (5.0f + 0.4f * static_cast<float>(i)));
    }
    EXPECT_NEAR(Q::Length(fast.Release(10 * Dt)), 9.0f, 0.3f);
}

// The reviewer's case (2026-10-05): a hand lowered toward the hip at 2.2 m/s, slowing to a drift,
// then the grip opens. The earlier reach must not turn the put-back into a throw at the feet.
TEST(QuickItems, SlowDriftAfterAReachPutsBack) {
    for (const float drift : {0.5f, 0.7f, 1.0f, 1.4f}) {
        Rig r;
        r.Grab(0);
        r.in.hand_v = {};
        r.Step(40);
        const Q::V3 dir = Q::Normalize(Q::V3{0.2f, -1.0f, 0.6f});
        bool threw = false;
        const auto step = [&](int n) {
            for (int i = 0; i < n; ++i) {
                r.Step();
                threw |= r.out.send && r.out.request.action == Q::Action::Throw;
            }
        };
        r.in.hand_v = dir * 2.2f;
        step(9);
        for (int i = 0; i < 7; ++i) {
            const float k = static_cast<float>(i + 1) / 7.0f;
            r.in.hand_v = dir * (2.2f + (drift - 2.2f) * k);
            step(1);
        }
        r.in.hand_v = dir * drift;
        step(9);
        r.in.grip = 0.0f;
        step(4);
        EXPECT_FALSE(threw);
        EXPECT_EQ(r.out.held_goods, -1); // back in the pouch
    }
}

// A tracking jump (85 m/s, pointing 68 degrees down: the haptics sweep's Molotov) in a 7 m/s
// overhand: the launch is the overhand's own, never into the floor. Also one under the cap.
TEST(QuickItems, TrackingJumpNeverThrowsIntoTheFloor) {
    const Q::V3 fwd = Q::Normalize(Q::V3{0.0f, 0.3f, -1.0f});
    const auto launch = [&](int spike_at, Q::V3 spike, std::string* log) {
        Rig r;
        r.Grab(0);
        r.Step(40);
        Q::Request thrown{};
        for (int i = 0; i < 30; ++i) {
            r.in.hand_v = i == spike_at ? spike : fwd * 7.0f;
            r.Step();
            if (log != nullptr) {
                *log += r.out.log;
            }
        }
        r.in.hand_v = fwd * 7.0f; // the release frames: the overhand's own speed
        r.in.grip = 0.0f;
        for (int i = 0; i < 6; ++i) {
            r.Step();
            if (r.out.send && r.out.request.action == Q::Action::Throw) {
                thrown = r.out.request;
            }
        }
        return thrown;
    };
    const Q::Request plain = launch(-1, {}, nullptr);
    ASSERT_EQ(plain.action, Q::Action::Throw);
    const Q::V3 vp{plain.v_world[0], plain.v_world[1], plain.v_world[2]};
    std::string log;
    for (const int at : {8, 27, 29}) {
        const Q::Request j = launch(at, Q::V3{-13.6f, -79.0f, -28.7f}, at == 8 ? &log : nullptr);
        ASSERT_EQ(j.action, Q::Action::Throw);
        const Q::V3 vj{j.v_world[0], j.v_world[1], j.v_world[2]};
        EXPECT_GT(Q::PitchOf(vj), 0.0f);
        EXPECT_NEAR(Q::Length(vj), Q::Length(vp), 0.01f * Q::Length(vp));
        const Q::Request u = launch(at, Q::Normalize(Q::V3{0.3f, -0.6f, -1.0f}) * 20.0f, nullptr);
        ASSERT_EQ(u.action, Q::Action::Throw);
        const Q::V3 vu{u.v_world[0], u.v_world[1], u.v_world[2]};
        EXPECT_NEAR(Q::PitchOf(vu), Q::PitchOf(vp), 0.5f);
        EXPECT_NEAR(Q::Length(vu), Q::Length(vp), 0.01f * Q::Length(vp));
    }
    EXPECT_NE(log.find("implausible hand speed"), std::string::npos);
}

// The card centres as seen from the eye: unit directions.
Q::V3 Seen(Q::V3 card, Q::V3 eye) {
    return Q::Normalize(card - eye);
}

TEST(QuickItems, ArcLayout) {
    const Q::V3 eye{0.0f, 1.6f, 0.0f};
    const Q::V3 palm{-0.2f, 1.1f, -0.3f};
    for (int n = 1; n <= Q::SlotCount; ++n) {
        const Q::Layout l = Q::PlaceArc(n, palm, eye, {});
        ASSERT_EQ(l.n, n);
        for (int i = 0; i < n; ++i) {
            const Q::V3 c = l.card[static_cast<size_t>(i)];
            EXPECT_GE(Q::Length(c - eye), 0.32f) << n << " " << i;
            EXPECT_NEAR(Q::Length(c - l.pivot), 0.18f, 1e-4f);
            EXPECT_GT(c.y, l.pivot.y); // above the hand
            for (int j = i + 1; j < n; ++j) {
                EXPECT_GE(Q::Length(c - l.card[static_cast<size_t>(j)]), 0.063f) << n;
                // Seen from the eye (45-55 degrees down) the cards keep their spacing: at least
                // a card diameter and a bit (5.5 cm at the arc's distance) apart.
                const float apart = std::acos(std::clamp(
                    Q::Dot(Seen(c, eye), Seen(l.card[static_cast<size_t>(j)], eye)), -1.0f, 1.0f));
                EXPECT_GE(apart * Q::Length(l.pivot - eye), 0.060f) << n << " " << i << " " << j;
            }
        }
        // Centred at 12 o'clock: symmetric heights.
        EXPECT_NEAR(l.card[0].y, l.card[static_cast<size_t>(n - 1)].y, 1e-4f);
        // The plane leans back toward the line of sight (not past it).
        EXPECT_GT(l.toward.y, 0.5f);
        EXPECT_NEAR(Q::Dot(l.up, l.toward), 0.0f, 1e-5f);
        EXPECT_NEAR(Q::Dot(l.right, l.toward), 0.0f, 1e-5f);
        EXPECT_NEAR(l.right.y, 0.0f, 1e-5f); // no roll
    }
    // Too near the eyes: pushed away.
    const Q::Layout near = Q::PlaceArc(4, {0.0f, 1.45f, -0.12f}, eye, {});
    for (int i = 0; i < near.n; ++i) {
        EXPECT_GE(Q::Length(near.card[static_cast<size_t>(i)] - eye), 0.32f);
    }
}

TEST(QuickItems, HoverHysteresis) {
    const Q::V3 eye{0.0f, 1.6f, 0.0f};
    const Q::Layout l = Q::PlaceArc(4, {-0.2f, 1.1f, -0.3f}, eye, {});
    EXPECT_EQ(Q::PickHover(l, l.pivot, eye, -1, 1.0f), -1); // the hand where it opened
    EXPECT_EQ(Q::PickHover(l, l.card[1], eye, -1, 1.0f), 1);
    // 7 cm off in the plane (outward from the last card): out without a hover, kept with one
    // (exit at 1.35).
    const Q::V3 off = l.card[3] + l.right * 0.07f;
    EXPECT_EQ(Q::PickHover(l, off, eye, -1, 1.0f), -1);
    EXPECT_EQ(Q::PickHover(l, off, eye, 3, 1.0f), 3);
    // Clearly on the neighbour: it switches.
    EXPECT_EQ(Q::PickHover(l, l.card[2], eye, 1, 1.0f), 2);
    // The scale widens the volume.
    EXPECT_EQ(Q::PickHover(l, off, eye, -1, 1.5f), 3);
}

TEST(QuickItems, YIsSquareWhenOff) {
    Rig r;
    r.cfg.enabled = false;
    r.in.y = true;
    EXPECT_TRUE(r.Step().square_hold);
    EXPECT_TRUE(r.Step().square_hold);
    r.in.y = false;
    EXPECT_FALSE(r.Step().square_hold);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    EXPECT_FALSE(r.out.send);
}

TEST(QuickItems, L3PlusYIsSquare) {
    Rig r;
    r.in.l3 = true;
    r.in.y = true;
    EXPECT_TRUE(r.Step().square_hold);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
}

TEST(QuickItems, NoHandIsSquare) {
    Rig r;
    r.in.body = false;
    r.in.y = true;
    EXPECT_TRUE(r.Step().square_hold);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    Rig s;
    s.in.snapshot_fresh = false;
    s.in.y = true;
    EXPECT_TRUE(s.Step().square_hold);
}

TEST(QuickItems, InertOutsideGameplay) {
    Rig r;
    r.in.gameplay = false;
    r.in.y = true;
    const Q::Output& o = r.Step();
    EXPECT_FALSE(o.square_hold);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
}

TEST(QuickItems, OpenAndClose) {
    Rig r;
    r.PressY();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
    EXPECT_EQ(r.out.scene.state, Q::SceneState::Open);
    EXPECT_EQ(r.out.scene.cards, 0x0F);
    EXPECT_TRUE(r.out.block_l2);
    EXPECT_FALSE(r.out.square_hold);
    r.Step(5);
    r.PressY();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    EXPECT_FALSE(r.out.block_l2);
}

TEST(QuickItems, EmptySlotsOpenNothing) {
    Rig r;
    r.in.snapshot = {};
    r.in.snapshot.valid = true;
    r.PressY();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    EXPECT_FALSE(r.out.square_hold);
}

TEST(QuickItems, TriggerWithTheArcOpenFiresTheGun) {
    // The user's rule: the arc open, no item in the hand, the gun in it: the trigger fires it at
    // the pull (its own latch, as without the arc) and the arc closes in the same frame, for a
    // full pull and for one just past the latch alike.
    for (const float pull : {1.0f, 0.5f}) {
        Rig r;
        r.Open();
        r.Step(20);
        EXPECT_TRUE(r.out.block_l2);
        r.in.trigger_latch = true;
        r.in.trigger = pull;
        EXPECT_FALSE(r.Step().block_l2) << pull; // the frame of the pull
        EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle) << pull;
        EXPECT_NE(r.out.log.find("CLOSE (trigger (gun))"), std::string::npos) << r.out.log;
        for (int i = 0; i < 20; ++i) {
            EXPECT_FALSE(r.Step().block_l2) << pull << " " << i; // held: the gun's L2 stays down
        }
    }
    // Right after Y too (no fist_s wait with the grip at rest).
    Rig q;
    q.PressY();
    q.in.trigger_latch = true;
    q.in.trigger = 1.0f;
    EXPECT_FALSE(q.Step().block_l2);
    EXPECT_EQ(q.c.GetPhase(), Q::Phase::Idle);
}

TEST(QuickItems, TriggerWithTheGripMovingWaitsForTheGrab) {
    // The grip already moving at the latch (a whole-hand squeeze on its way): the shot waits up
    // to fist_s; the grip arriving makes it the grab, the grip settling back lets the shot go.
    Rig r;
    r.Open();
    r.Hover(0);
    r.Step(9);
    r.in.grip = 0.3f;
    r.in.trigger = 0.6f;
    r.in.trigger_latch = true;
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(r.Step().block_l2) << i;
    }
    r.in.grip = 1.0f;
    for (int i = 0; i < 30; ++i) {
        EXPECT_TRUE(r.Step().block_l2) << i;
    }
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
    EXPECT_EQ(r.c.HeldSlot(), 0);
    // The grip stays half way (never a squeeze): the shot goes fist_s after the pull.
    Rig s;
    s.Open();
    s.Step(20);
    s.in.grip = 0.3f;
    s.in.trigger = 1.0f;
    s.in.trigger_latch = true;
    int blocked = 0;
    while (s.Step().block_l2 && blocked < 30) {
        ++blocked;
    }
    EXPECT_GE(blocked * Dt, s.cfg.fist_s - Dt);
    EXPECT_LE(blocked * Dt, s.cfg.fist_s + Dt);
    EXPECT_EQ(s.c.GetPhase(), Q::Phase::Idle);
    // The grip let go again: the shot at once.
    Rig u;
    u.Open();
    u.Step(20);
    u.in.grip = 0.3f;
    u.in.trigger = 1.0f;
    u.in.trigger_latch = true;
    EXPECT_TRUE(u.Step(2).block_l2);
    u.in.grip = 0.0f;
    EXPECT_FALSE(u.Step().block_l2);
    EXPECT_EQ(u.c.GetPhase(), Q::Phase::Idle);
}

TEST(QuickItems, WholeHandSqueezeGrabsWithoutFiring) {
    // Grip and trigger together right after Y (the reviewer's r1): the card, no shot. The grip
    // ramps up as the hand closes: at the trigger's latch it is already moving (or squeezed).
    for (const int lead : {0, 3, -3}) { // the trigger with, 33 ms before, 33 ms after the grip
        Rig r;
        r.Open();
        r.Hover(0);
        r.Step(9);
        const auto squeeze = [&](bool grip, bool trigger) {
            r.in.grip = grip ? 1.0f : 0.3f;
            if (trigger) {
                r.in.trigger = 0.6f;
                r.in.trigger_latch = true;
            }
        };
        squeeze(lead <= 0, lead >= 0);
        for (int i = 0; i < 3; ++i) {
            EXPECT_TRUE(r.Step().block_l2) << lead << " " << i;
        }
        squeeze(true, true);
        for (int i = 0; i < 40; ++i) {
            EXPECT_TRUE(r.Step().block_l2) << lead << " " << i;
        }
        EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding) << lead;
        EXPECT_EQ(r.c.HeldSlot(), 0) << lead;
        // The full squeeze too (trigger 1.0): still the fist.
        r.in.trigger = 1.0f;
        for (int i = 0; i < 20; ++i) {
            EXPECT_TRUE(r.Step().block_l2) << lead << " " << i;
        }
        EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding) << lead;
    }
}

TEST(QuickItems, TriggerAlreadyDownAtTheOpenIsNotAPull) {
    // A grip held through the open (it must be let go before it grabs) makes a pull the fist ...
    Rig r;
    r.in.grip = 1.0f;
    r.Step(30);
    r.Open();
    r.Step(20);
    r.in.trigger = 0.6f;
    r.in.trigger_latch = true;
    for (int i = 0; i < 40; ++i) {
        EXPECT_TRUE(r.Step().block_l2) << i;
    }
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
    // ... and closing the arc with it still down keeps the gun back until it is let go.
    r.PressY();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(r.Step().block_l2) << i;
    }
    r.in.trigger = 0.0f;
    r.in.trigger_latch = false;
    EXPECT_FALSE(r.Step().block_l2);
    // A trigger already down when the arc opens is not a pull of this open.
    Rig q;
    q.in.trigger = 1.0f;
    q.in.trigger_latch = true;
    q.Step(5);
    q.Open();
    for (int i = 0; i < 30; ++i) {
        EXPECT_TRUE(q.Step().block_l2) << i;
    }
    EXPECT_EQ(q.c.GetPhase(), Q::Phase::Open);
}

TEST(QuickItems, GrabUsableAndUse) {
    Rig r;
    r.Grab(1);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
    EXPECT_EQ(r.out.held_goods, 1120);
    EXPECT_EQ(r.out.held_ffx, 20009);
    EXPECT_TRUE(r.out.send); // the HUD selection
    EXPECT_EQ(r.out.request.select, 1);
    EXPECT_EQ(r.out.request.action, Q::Action::None);
    EXPECT_TRUE(r.out.block_l2);
    // The trigger: a 100 ms commit, then the use.
    r.Step(12); // past fist_s: the use pull is not the grab's squeeze
    r.in.trigger = 1.0f;
    r.in.trigger_latch = true;
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Committing);
    EXPECT_TRUE(r.out.block_l2);
    r.Step(12);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Waiting);
    EXPECT_EQ(r.sent.action, Q::Action::Use);
    EXPECT_EQ(r.sent.goods, 1120);
    EXPECT_EQ(r.out.held_goods, 1120); // in the hand until the game answers
    // The game answers.
    r.in.have_result = true;
    r.in.result = {r.sent.serial, Q::Outcome::Used, "", 1120, 3, 2};
    r.in.snapshot.slot[1].qty = 2;
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    EXPECT_EQ(r.out.held_goods, -1);
    EXPECT_TRUE(r.out.block_l2); // the trigger still held: no gun until released
    r.in.trigger = 0.0f;
    r.in.trigger_latch = false;
    EXPECT_FALSE(r.Step().block_l2);
}

TEST(QuickItems, UseRequestCarriesTheGoods) {
    Rig r;
    r.Grab(2);
    r.Step(12); // past fist_s: the use pull is not the grab's squeeze
    r.in.trigger = 1.0f;
    Q::Request last{};
    for (int i = 0; i < 20; ++i) {
        if (r.Step().send) {
            last = r.out.request;
        }
    }
    EXPECT_EQ(last.action, Q::Action::Use);
    EXPECT_EQ(last.goods, 1110);
    EXPECT_EQ(last.slot, 2);
}

TEST(QuickItems, SquarePath) {
    Rig r;
    r.Grab(3); // the Monocular
    r.Step(12); // past fist_s: the use pull is not the grab's squeeze
    r.in.trigger = 1.0f;
    bool square = false;
    for (int i = 0; i < 20 && !square; ++i) {
        square = r.Step().square_press;
        if (i == 15) {
            r.in.snapshot.selected = 3; // the game shows it selected
        }
    }
    EXPECT_TRUE(square);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
}

TEST(QuickItems, InstantOffUsesTheGame) {
    Rig r;
    r.cfg.instant = false;
    r.Grab(1);
    r.in.snapshot.selected = 1;
    r.Step(12); // past fist_s: the use pull is not the grab's squeeze
    r.in.trigger = 1.0f;
    bool square = false;
    for (int i = 0; i < 20 && !square; ++i) {
        square = r.Step().square_press;
    }
    EXPECT_TRUE(square);
}

TEST(QuickItems, ThrowOnFastRelease) {
    Rig r;
    r.Grab(0);
    EXPECT_EQ(r.out.held_ffx, 20000);
    // Swing at 5 m/s, then let go.
    r.in.hand_v = {0.0f, 1.0f, -5.0f};
    r.Step(6);
    r.in.grip = 0.0f;
    const Q::Output& o = r.Step();
    EXPECT_TRUE(o.send);
    EXPECT_EQ(o.request.action, Q::Action::Throw);
    EXPECT_EQ(o.request.goods, 1200);
    const float speed = std::hypot(o.request.v_world[0], o.request.v_world[1], o.request.v_world[2]);
    const Q::Config d{};
    // No wind-up: full power; the speed through the curve, 11.3 degrees up lifted 6 more.
    EXPECT_NEAR(speed, d.throw_gain * Q::LaunchCurve(std::hypot(1.0f, 5.0f), d.throw_gain_curve),
                0.05f * d.throw_gain);
    EXPECT_NEAR(Q::PitchOf({o.request.v_world[0], o.request.v_world[1], o.request.v_world[2]}),
                11.31f + d.throw_lift_deg, 0.05f);
    EXPECT_LT(o.request.v_world[2], 0.0f);
    EXPECT_EQ(o.held_goods, -1);   // it left the hand
    EXPECT_FALSE(o.block_l2);      // the gun is free at once
    EXPECT_GT(o.throw_strength, 0.9f); // a firm throw (18 m/s before the cap): a strong pulse
    r.in.have_result = true;
    r.in.result = {o.request.serial, Q::Outcome::Thrown, "", 1200, 5, 4};
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
}

TEST(QuickItems, SlowReleasePutsBack) {
    Rig r;
    r.Grab(0);
    r.in.hand_v = {0.0f, 0.0f, -0.4f};
    r.Step(6);
    r.in.grip = 0.0f;
    const Q::Output& o = r.Step();
    EXPECT_FALSE(o.send && o.request.action == Q::Action::Throw);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
}

TEST(QuickItems, ThrowTraceLogsTheLastSamples) {
    // VR_THROW_TRACE: the release logs a summary and one line per input frame of the last 0.4 s
    // (the display-time pose, the spin, both angular velocities, the shoulder, the grip, and the
    // grip located at the input time); without it nothing.
    for (const bool on : {false, true}) {
        Rig r;
        r.cfg.throw_trace = on;
        r.in.shoulder_valid = true;
        r.in.shoulder = {-0.17f, 1.38f, 0.05f};
        r.in.hand_now_valid = true;
        r.in.hand_wv_rt_valid = true;
        r.in.hand_wv_rt = {0.0f, 0.0f, 3.0f};
        r.in.pose_lead_s = 0.045f;
        r.Grab(0);
        r.Step(30); // held: fills the trace's window
        r.in.hand_v = {0.0f, 1.0f, -5.0f};
        r.in.hand_v_now = {0.0f, 1.0f, -6.0f}; // the freshest estimate: faster
        r.Step(6);
        r.in.grip = 0.0f;
        r.Step();
        EXPECT_EQ(r.out.request.action, Q::Action::Throw);
        EXPECT_TRUE(r.out.trace.empty()); // it waits for the follow-through
        // The follow-through: the hand slows; the samples go out 0.15 s after the release.
        r.in.hand_v = {0.0f, 0.5f, -2.0f};
        r.in.hand_v_now = {0.0f, 0.5f, -2.0f};
        std::string trace;
        int frames = 0;
        while (trace.empty() && frames < 40) {
            r.Step();
            ++frames;
            trace = r.out.trace;
        }
        if (!on) {
            EXPECT_TRUE(trace.empty());
            continue;
        }
        EXPECT_EQ(frames, 14); // 0.15 s at 90 frames/s (13.5 frames) after the release frame
        // The throw reads the grip located at the input time (|(0, 1, -6)|); the display-time
        // poses' 5.10 m/s are logged beside it.
        EXPECT_EQ(trace.rfind("THROWTRACE #1 goods 1200 thrown: release 6.08 m/s (fresh poses)", 0),
                  0u);
        EXPECT_NE(trace.find("used 6.08 (0.000,1.000,-6.000) 0 ms after the grip opened"),
                  std::string::npos);
        EXPECT_NE(trace.find("the display-time poses: 5.10 at the release"), std::string::npos);
        EXPECT_NE(trace.find("6.08 at the release, 6.08 the fastest"), std::string::npos);
        EXPECT_NE(trace.find("poses 45 ms ahead"), std::string::npos);
        int lines = 0;
        int after = 0;
        for (std::size_t at = trace.find("\nTT#1 f "); at != std::string::npos;
             at = trace.find("\nTT#1 f ", at + 1)) {
            ++lines;
            after += trace.compare(at + 8, 1, "+") == 0 && trace.compare(at + 8, 7, "+0.0000") != 0;
        }
        EXPECT_GE(lines, 35 + 13); // 0.4 s before and 0.15 s after at 90 frames/s
        EXPECT_LE(lines, 38 + 14);
        EXPECT_GE(after, 13);
        // The release frame's own line: f 0, its pose 45 ms ahead, the grip open.
        EXPECT_NE(trace.find("TT#1 f +0.0000 d +0.0450 "), std::string::npos);
        EXPECT_NE(trace.find("|5.10| s 0.00,0.00,0.00 w 0.0,0.0,0.0 |0.0| rt 0.0,0.0,3.0 r "
                             "-0.030,-0.280,-0.350 g 0.00 now v 0.000,1.000,-6.000 |6.08|"),
                  std::string::npos);
        // ... and the follow-through's.
        EXPECT_NE(trace.find("|2.06| s 0.00,0.00,0.00"), std::string::npos);
    }
}

TEST(QuickItems, TriggerNeverFiresWithAThrowableInTheHand) {
    // The user's rule: an item in the hand, the trigger is never the gun. Hard pulls of the
    // trigger alone (the hand still, past fist_s) with a Molotov held: no L2, no press, still in
    // the hand; then it is thrown as usual.
    Rig r;
    r.Grab(0);
    r.Step(12); // past fist_s: the use pull is not the grab's squeeze
    for (int pull = 0; pull < 3; ++pull) {
        r.in.trigger_latch = true;
        r.in.trigger = 1.0f;
        for (int i = 0; i < 30; ++i) {
            const Q::Output& o = r.Step();
            EXPECT_TRUE(o.block_l2) << pull << " " << i;
            EXPECT_FALSE(o.l2_press) << pull << " " << i;
            EXPECT_FALSE(o.send && o.request.action != Q::Action::None) << pull << " " << i;
        }
        EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding) << pull;
        EXPECT_EQ(r.out.held_goods, 1200) << pull;
        r.in.trigger_latch = false;
        r.in.trigger = 0.0f;
        EXPECT_TRUE(r.Step(5).block_l2) << pull;
    }
    r.in.hand_v = {0.0f, 1.0f, -5.0f};
    r.Step(6);
    r.in.grip = 0.0f;
    const Q::Output& thrown = r.Step();
    EXPECT_TRUE(thrown.send);
    EXPECT_EQ(thrown.request.action, Q::Action::Throw);
    // Opt-in quick_arc_trigger_parry: a deliberate pull puts it away and fires.
    Rig q;
    q.cfg.trigger_parry = true;
    q.Grab(0);
    q.Step(12);
    q.in.trigger_latch = true;
    q.in.trigger = 1.0f;
    const Q::Output& o = q.Step();
    EXPECT_EQ(q.c.GetPhase(), Q::Phase::Idle);
    EXPECT_FALSE(o.block_l2);
    EXPECT_FALSE(o.send && o.request.action != Q::Action::None);
}

TEST(QuickItems, TriggerNeverFiresWithAUsableInTheHand) {
    // A usable (Blue Elixir): taps of every length below the commit do nothing (no L2, no press,
    // no use), the item stays in the hand; a pull held use_commit_s uses it, and the gun stays
    // back through the use's answer and until that pull is let go.
    Rig r;
    r.Grab(1);
    r.Step(12); // past fist_s: the use pull is not the grab's squeeze
    for (const int frames : {1, 2, 4, 8}) { // 11 .. 89 ms
        r.in.trigger_latch = true;
        r.in.trigger = 1.0f;
        for (int i = 0; i < frames; ++i) {
            const Q::Output& o = r.Step();
            EXPECT_TRUE(o.block_l2) << frames << " " << i;
        }
        r.in.trigger_latch = false;
        r.in.trigger = 0.0f;
        const Q::Output& o = r.Step();
        EXPECT_TRUE(o.block_l2) << frames;
        EXPECT_FALSE(o.l2_press) << frames;
        EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding) << frames;
        EXPECT_EQ(o.held_goods, 1120) << frames;
        r.Step(5);
    }
    EXPECT_NE(r.sent.action, Q::Action::Use);
    r.in.trigger_latch = true;
    r.in.trigger = 1.0f;
    for (int i = 0; i < 14; ++i) {
        EXPECT_TRUE(r.Step().block_l2) << i;
    }
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Waiting);
    EXPECT_EQ(r.sent.action, Q::Action::Use);
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(r.Step().block_l2) << i; // the answer is pending: still in the hand
    }
}

TEST(QuickItems, BrushedTriggerDuringAThrowIsNotTheGun) {
    // The reviewer's r3: the trigger brushed during a Molotov's wind-up.
    Rig r;
    r.Grab(0);
    r.Step(20);
    r.in.hand_v = {0.0f, 1.0f, 4.0f}; // winding up
    r.in.trigger = 0.6f;
    r.in.trigger_latch = true;
    for (int i = 0; i < 6; ++i) {
        EXPECT_TRUE(r.Step().block_l2) << i;
    }
    r.in.trigger = 1.0f; // squeezed harder through the wind-up's turn ...
    r.in.hand_v = {0.0f, 0.1f, 0.2f};
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(r.Step().block_l2) << i;
    }
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding); // ... still in the hand
    // The throw itself goes out.
    r.in.hand_v = {0.0f, 1.0f, -6.0f};
    r.Step(6);
    r.in.grip = 0.0f;
    const Q::Output& o = r.Step();
    EXPECT_TRUE(o.send);
    EXPECT_EQ(o.request.action, Q::Action::Throw);
    // Afterwards (opt-in quick_arc_trigger_parry) a deliberate pull of the trigger alone, the hand
    // still: the gun.
    Rig q;
    q.cfg.trigger_parry = true;
    q.Grab(0);
    q.Step(20);
    q.in.trigger = 0.5f;
    q.in.trigger_latch = true;
    q.Step(2);
    EXPECT_EQ(q.c.GetPhase(), Q::Phase::Holding); // not deep enough yet
    q.in.trigger = 0.95f;
    EXPECT_FALSE(q.Step().block_l2);
    EXPECT_EQ(q.c.GetPhase(), Q::Phase::Idle);
}

TEST(QuickItems, ItemLeavingWithTheTriggerDownKeepsTheGunBack) {
    // The reviewer's r3: an Antidote held with the trigger resting at 0.6, then the grip let go:
    // the item goes back, no shot until the trigger is let go.
    Rig r;
    r.Grab(1);
    r.Step(20);
    r.in.trigger = 0.6f;
    r.in.trigger_latch = true;
    r.Step(10);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
    r.in.grip = 0.0f;
    EXPECT_TRUE(r.Step().block_l2);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    for (int i = 0; i < 20; ++i) {
        EXPECT_TRUE(r.Step().block_l2) << i;
    }
    r.in.trigger = 0.0f;
    r.in.trigger_latch = false;
    EXPECT_FALSE(r.Step().block_l2);
}

TEST(QuickItems, UsePullWithTheGrabsSqueezeIsTheFist) {
    // Grabbing a usable with the whole hand: the trigger that came with the squeeze does not use
    // it; a new pull does.
    Rig r;
    r.Grab(1);
    r.in.trigger = 1.0f;
    r.in.trigger_latch = true;
    for (int i = 0; i < 30; ++i) {
        EXPECT_FALSE(r.Step().send && r.out.request.action == Q::Action::Use) << i;
    }
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
    r.in.trigger = 0.0f;
    r.in.trigger_latch = false;
    r.Step(3);
    r.in.trigger = 1.0f;
    r.in.trigger_latch = true;
    r.Step(14);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Waiting);
    EXPECT_EQ(r.sent.action, Q::Action::Use);
}

TEST(QuickItems, GripHeldAtOpenMustBeReleased) {
    Rig r;
    r.in.grip = 1.0f;
    r.Step(30); // held well before Y (one squeezed with Y is a quick draw)
    r.Open();
    r.Hover(1);
    r.Step(5);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
    r.in.grip = 0.0f;
    r.Step();
    r.in.grip = 1.0f;
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
}

TEST(QuickItems, QuickDraw) {
    Rig r;
    r.PressY();
    r.in.grip = 1.0f; // at once, without reaching: the game's selected item
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
    EXPECT_EQ(r.c.HeldSlot(), 0);
}

TEST(QuickItems, GripOnNothingIsNoGrab) {
    Rig r;
    r.Open();
    r.Step(80); // past the quick draw
    r.in.grip = 1.0f;
    const Q::Output& o = r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
    EXPECT_FALSE(o.send);
}

TEST(QuickItems, VialInterplay) {
    Rig r;
    r.in.vial_armed = true;
    r.PressY();
    EXPECT_TRUE(r.out.disarm_vial);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
    r.in.vial_armed = false;
    r.Hover(1);
    r.Step(9);
    r.in.grip = 1.0f;
    r.Step();
    ASSERT_EQ(r.c.GetPhase(), Q::Phase::Holding);
    r.in.x_edge = true; // the vial: the item goes away first
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    EXPECT_EQ(r.out.held_goods, -1);
}

TEST(QuickItems, ContextResetsWithoutRequests) {
    Rig r;
    r.Grab(1);
    r.in.gameplay = false; // a menu, a cinematic
    const Q::Output& o = r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    EXPECT_FALSE(o.send);
    EXPECT_EQ(o.held_goods, -1);
    EXPECT_FALSE(o.block_l2);
}

TEST(QuickItems, PutAwayWhenTheSlotEmpties) {
    Rig r;
    r.Grab(1);
    r.in.snapshot.slot[1].qty = 0;
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
}

TEST(QuickItems, DeniedUseStaysInTheHand) {
    Rig r;
    r.Grab(1);
    r.Step(12); // past fist_s: the use pull is not the grab's squeeze
    r.in.trigger = 1.0f;
    r.Step(14);
    ASSERT_EQ(r.c.GetPhase(), Q::Phase::Waiting);
    r.in.have_result = true;
    r.in.result = {r.sent.serial, Q::Outcome::Denied, "test", 1120, 3, 3};
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding); // the grip still holds it
    EXPECT_EQ(r.out.held_goods, 1120);
    // An answer to an older request is ignored.
    Rig q;
    q.Grab(1);
    q.Step(12); // past fist_s: the use pull is not the grab's squeeze
    q.in.trigger = 1.0f;
    q.Step(14);
    q.in.have_result = true;
    q.in.result = {q.sent.serial - 1, Q::Outcome::Used, "", 1120, 3, 2};
    q.Step();
    EXPECT_EQ(q.c.GetPhase(), Q::Phase::Waiting);
}

TEST(QuickItems, NoAnswerGivesUp) {
    Rig r;
    r.Grab(1);
    r.Step(12); // past fist_s: the use pull is not the grab's squeeze
    r.in.trigger = 1.0f;
    r.Step(14);
    ASSERT_EQ(r.c.GetPhase(), Q::Phase::Waiting);
    r.Step(60); // 0.67 s
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
}

TEST(QuickItems, ArmTakenByTheGame) {
    Rig r;
    r.Grab(0);
    r.in.arm_weight = 0.2f; // a dodge
    r.in.hand_v = {0.0f, 0.0f, -6.0f};
    r.Step(4);
    r.in.grip = 0.0f;
    const Q::Output& o = r.Step();
    EXPECT_FALSE(o.send && o.request.action == Q::Action::Throw); // no throw from his animation
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    // Held through a long one: put away.
    Rig s;
    s.Grab(1);
    s.in.arm_weight = 0.0f;
    s.Step(240);
    EXPECT_EQ(s.c.GetPhase(), Q::Phase::Idle);
}

TEST(QuickItems, YPutsAway) {
    Rig r;
    r.Grab(1);
    r.PressY();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    EXPECT_EQ(r.out.held_goods, -1);
}

TEST(QuickItems, TriggerTapIsTheGunNotAUse) {
    // Opt-in quick_arc_trigger_parry: a reflex tap (22 ms) with a usable in hand: no use; the item
    // goes away and the gun gets a press of its own (the tap itself was held back).
    Rig r;
    r.cfg.trigger_parry = true;
    r.Grab(1);
    r.Step(12); // past fist_s: the use pull is not the grab's squeeze
    r.in.trigger = 1.0f;
    r.in.trigger_latch = true;
    r.Step(2);
    ASSERT_EQ(r.c.GetPhase(), Q::Phase::Committing);
    EXPECT_TRUE(r.out.block_l2);
    r.in.trigger = 0.0f;
    r.in.trigger_latch = false;
    const Q::Output& o = r.Step();
    EXPECT_TRUE(o.l2_press);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    EXPECT_EQ(o.held_goods, -1);
    for (int i = 0; i < 30; ++i) {
        EXPECT_FALSE(r.Step().send && r.out.request.action == Q::Action::Use);
    }
    // Without it (the default) the tap only cancels: the item stays in the hand.
    Rig q;
    q.Grab(1);
    q.Step(12); // past fist_s: the use pull is not the grab's squeeze
    q.in.trigger = 1.0f;
    q.Step(2);
    q.in.trigger = 0.0f;
    EXPECT_FALSE(q.Step().l2_press);
    EXPECT_EQ(q.c.GetPhase(), Q::Phase::Holding);
    EXPECT_EQ(q.out.held_goods, 1120);
}

TEST(QuickItems, SquareWaitHoldsTheGunBack) {
    // The game's own use: the trigger that asked for it is not the gun while the select is
    // pending, nor after a timeout until it is let go.
    Rig r;
    r.cfg.select = false;
    r.Grab(3); // the Monocular (not selected: the game's selection is slot 0)
    r.Step(12); // past fist_s: the use pull is not the grab's squeeze
    r.in.trigger = 1.0f;
    r.in.trigger_latch = true;
    r.Step(12);
    ASSERT_EQ(r.c.GetPhase(), Q::Phase::SquareWait);
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(r.Step().block_l2) << i;
    }
    // Let go and pulled again while the game's use is pending: still not the gun.
    r.in.trigger = 0.0f;
    r.in.trigger_latch = false;
    EXPECT_TRUE(r.Step(2).block_l2);
    r.in.trigger = 1.0f;
    r.in.trigger_latch = true;
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(r.Step().block_l2) << i;
    }
    ASSERT_EQ(r.c.GetPhase(), Q::Phase::SquareWait);
    r.Step(80); // past square_wait_s: given up
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    EXPECT_TRUE(r.Step().block_l2);
    r.in.trigger = 0.0f;
    r.in.trigger_latch = false;
    EXPECT_FALSE(r.Step().block_l2);
}

TEST(QuickItems, ReadingACardKeepsTheArcOpen) {
    Rig r;
    r.Open();
    r.Hover(1);
    r.Step(static_cast<int>(8.0 / Dt)); // 8 s on one card (idle_close_s 6)
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
    r.in.hover.slot = -1;
    r.Step(static_cast<int>(6.5 / Dt));
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle); // nobody reaches for it: closes
}

TEST(QuickItems, OnlyYourOwnArmHoversAndTakes) {
    // The game animates the arm (a dodge) through the open arc: no detents, no grab.
    Rig r;
    r.Open();
    r.Step(10);
    r.in.arm_weight = 0.3f;
    r.Hover(1);
    const Q::Output& o = r.Step(3);
    EXPECT_EQ(o.haptic_count, 0);
    r.in.grip = 1.0f;
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
    // Back to yours: release and squeeze takes the hovered card.
    r.in.arm_weight = 1.0f;
    r.in.grip = 0.0f;
    r.Step(2);
    r.in.grip = 1.0f;
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
    EXPECT_EQ(r.c.HeldSlot(), 1);
    // No hover in the first 80 ms after the open (the arc unfolds around the hand).
    Rig s;
    s.PressY();
    s.Hover(2);
    EXPECT_EQ(s.Step().haptic_count, 0);
    s.Step(8);
    EXPECT_EQ(s.c.GetPhase(), Q::Phase::Open);
}

TEST(QuickItems, StackSqueezeWithTheTriggerIsNotTheGun) {
    // After a throw the next one is a squeeze away: a whole-hand squeeze (the grip already moving
    // at the trigger's latch) takes it without a shot; a pull with the grip moving that never
    // becomes a squeeze fires fist_s late; the trigger alone, the grip at rest, fires at once.
    {
        Rig r;
        r.Grab(0);
        r.in.hand_v = {0.0f, 1.0f, -5.0f};
        r.Step(6);
        r.in.grip = 0.0f;
        r.Step();
        ASSERT_EQ(r.sent.action, Q::Action::Throw);
        r.in.hand_v = {};
        r.in.have_result = true;
        r.in.result = {r.sent.serial, Q::Outcome::Thrown, "", 1200, 5, 4};
        r.in.snapshot.slot[0].qty = 4;
        r.Step(30);
        ASSERT_EQ(r.c.GetPhase(), Q::Phase::Idle);
        r.in.have_result = false;
        r.in.trigger = 1.0f;
        r.in.trigger_latch = true;
        EXPECT_FALSE(r.Step().block_l2); // the frame of the pull
        EXPECT_FALSE(r.Step(10).block_l2);
        EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    }
    for (const bool with_grip : {true, false}) {
        Rig r;
        r.Grab(0); // Molotov x5
        r.in.hand_v = {0.0f, 1.0f, -5.0f};
        r.Step(6);
        r.in.grip = 0.0f;
        r.Step();
        ASSERT_EQ(r.sent.action, Q::Action::Throw);
        r.in.hand_v = {};
        r.in.have_result = true;
        r.in.result = {r.sent.serial, Q::Outcome::Thrown, "", 1200, 5, 4};
        r.in.snapshot.slot[0].qty = 4;
        r.Step(30);
        ASSERT_EQ(r.c.GetPhase(), Q::Phase::Idle);
        r.in.have_result = false;
        r.in.grip = 0.3f; // the hand closing
        r.in.trigger = 1.0f;
        r.in.trigger_latch = true;
        EXPECT_TRUE(r.Step().block_l2) << with_grip;
        EXPECT_TRUE(r.Step().block_l2) << with_grip;
        if (with_grip) {
            r.in.grip = 1.0f;
            for (int i = 0; i < 30; ++i) {
                EXPECT_TRUE(r.Step().block_l2) << i;
            }
            EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
            EXPECT_EQ(r.c.HeldSlot(), 0);
        } else {
            int blocked = 2;
            while (r.Step().block_l2 && blocked < 30) {
                ++blocked;
            }
            EXPECT_GE(blocked * Dt, r.cfg.fist_s - Dt);
            EXPECT_LE(blocked * Dt, r.cfg.fist_s + Dt);
            EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
        }
    }
}

TEST(QuickItems, StackAfterAThrow) {
    Rig r;
    r.Grab(0); // Molotov x5
    r.in.hand_v = {0.0f, 1.0f, -5.0f};
    r.Step(6);
    r.in.grip = 0.0f;
    r.Step();
    ASSERT_EQ(r.sent.action, Q::Action::Throw);
    r.in.hand_v = {};
    // A new squeeze before the game's answer is kept for it.
    r.in.grip = 1.0f;
    r.Step();
    r.in.have_result = true;
    r.in.result = {r.sent.serial, Q::Outcome::Thrown, "", 1200, 5, 4};
    r.in.snapshot.slot[0].qty = 4;
    r.Step(2);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
    EXPECT_EQ(r.c.HeldSlot(), 0);
    EXPECT_EQ(r.out.held_goods, 1200);
    // Throw it too, then squeeze 0.5 s later: the next one.
    r.in.have_result = false;
    r.in.hand_v = {0.0f, 1.0f, -5.0f};
    r.Step(6);
    r.in.grip = 0.0f;
    r.Step();
    r.in.hand_v = {};
    r.in.have_result = true;
    r.in.result = {r.sent.serial, Q::Outcome::Thrown, "", 1200, 4, 3};
    r.in.snapshot.slot[0].qty = 3;
    r.Step(static_cast<int>(0.5 / Dt));
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    r.in.grip = 1.0f;
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
    // Too late (1.2 s): nothing.
    Rig q;
    q.Grab(0);
    q.in.hand_v = {0.0f, 1.0f, -5.0f};
    q.Step(6);
    q.in.grip = 0.0f;
    q.Step();
    q.in.hand_v = {};
    q.in.have_result = true;
    q.in.result = {q.sent.serial, Q::Outcome::Thrown, "", 1200, 5, 4};
    q.in.snapshot.slot[0].qty = 4;
    q.Step(static_cast<int>(1.2 / Dt));
    q.in.grip = 1.0f;
    q.Step();
    EXPECT_EQ(q.c.GetPhase(), Q::Phase::Idle);
    // The last one thrown: nothing to take.
    Rig e;
    e.in.snapshot.slot[0].qty = 1;
    e.Grab(0);
    e.in.hand_v = {0.0f, 1.0f, -5.0f};
    e.Step(6);
    e.in.grip = 0.0f;
    e.Step();
    e.in.hand_v = {};
    e.in.have_result = true;
    e.in.result = {e.sent.serial, Q::Outcome::Thrown, "", 1200, 1, 0};
    e.in.snapshot.slot[0].qty = 0;
    e.Step(5);
    e.in.grip = 1.0f;
    e.Step();
    EXPECT_EQ(e.c.GetPhase(), Q::Phase::Idle);
}

TEST(QuickItems, StackAfterAUseNeedsANewSqueeze) {
    Rig r;
    r.Grab(2); // pellets x3
    r.Step(12); // past fist_s: the use pull is not the grab's squeeze
    r.in.trigger = 1.0f;
    r.in.trigger_latch = true;
    r.Step(14);
    ASSERT_EQ(r.c.GetPhase(), Q::Phase::Waiting);
    r.in.have_result = true;
    r.in.result = {r.sent.serial, Q::Outcome::Used, "", 1110, 3, 2};
    r.in.snapshot.slot[2].qty = 2;
    r.Step();
    r.in.trigger = 0.0f;
    r.in.trigger_latch = false;
    // The grip still held: the hand stays empty (the trigger is the gun again).
    r.Step(40);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    EXPECT_FALSE(r.out.block_l2);
    // Release and squeeze within the second: the next pellet.
    r.in.grip = 0.0f;
    r.Step(3);
    r.in.grip = 1.0f;
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
    EXPECT_EQ(r.c.HeldSlot(), 2);
    // quick_arc_stack=0: nothing.
    Rig q;
    q.cfg.stack = false;
    q.Grab(2);
    q.Step(12); // past fist_s: the use pull is not the grab's squeeze
    q.in.trigger = 1.0f;
    q.Step(14);
    q.in.have_result = true;
    q.in.result = {q.sent.serial, Q::Outcome::Used, "", 1110, 3, 2};
    q.in.snapshot.slot[2].qty = 2;
    q.Step();
    q.in.trigger = 0.0f;
    q.in.grip = 0.0f;
    q.Step(3);
    q.in.grip = 1.0f;
    q.Step();
    EXPECT_EQ(q.c.GetPhase(), Q::Phase::Idle);
}

TEST(QuickItems, PushIsWeakerThanAThrow) {
    // The same 5 m/s release: a push from 30 cm in front of the shoulder vs a wind-up from
    // beside the head with a wrist whip.
    const auto launch = [](float start_z, float omega) {
        Rig r;
        r.in.shoulder_valid = true;
        r.in.shoulder = {-0.18f, 1.40f, 0.0f};
        r.Grab(0);
        r.in.hand_w = omega;
        for (int i = 0; i < 20; ++i) {
            const float k = static_cast<float>(i) / 19.0f;
            r.in.hand_p = {-0.18f, 1.35f, start_z - 0.30f * k};
            r.in.hand_v = {0.0f, 1.0f, -5.0f};
            r.Step();
        }
        r.in.grip = 0.0f;
        const Q::Output& o = r.Step();
        return std::hypot(o.request.v_world[0], o.request.v_world[1], o.request.v_world[2]);
    };
    const float push = launch(-0.30f, 1.0f);
    const float full = launch(0.05f, 20.0f);
    const Q::Config defaults{};
    const float curved = Q::LaunchCurve(std::hypot(1.0f, 5.0f), defaults.throw_gain_curve);
    EXPECT_NEAR(full, defaults.throw_gain * curved, 0.05f * defaults.throw_gain);
    EXPECT_NEAR(push, defaults.throw_push_factor * full, 0.05f * defaults.throw_gain);
    // A push with a little turn of the wrist (6 rad/s) is still a push.
    EXPECT_NEAR(launch(-0.30f, 6.0f), push, 1e-3f);
    // From the same place in front of the chest, but the hand's path curving up and over (a
    // sidearm or overhand swing from the elbow): a full throw.
    Rig r;
    r.in.shoulder_valid = true;
    r.in.shoulder = {-0.18f, 1.40f, 0.0f};
    r.Grab(0);
    r.in.hand_w = 1.0f;
    for (int i = 0; i < 20; ++i) {
        const float k = static_cast<float>(i) / 19.0f;
        const float a = (75.0f - 85.0f * k) * 0.01745329f;
        r.in.hand_p = {-0.18f, 1.35f + 0.1f * k, -0.30f - 0.30f * k};
        r.in.hand_v = Q::V3{0.0f, std::sin(a), -std::cos(a)} * (1.0f + 4.1f * k);
        r.Step();
    }
    r.in.grip = 0.0f;
    const Q::Output& o = r.Step();
    const float swung =
        std::hypot(o.request.v_world[0], o.request.v_world[1], o.request.v_world[2]);
    EXPECT_GT(swung, 0.97f * defaults.throw_gain *
                         Q::LaunchCurve(Q::Length(r.in.hand_v), defaults.throw_gain_curve));
}

TEST(QuickItems, ReleaseIgnoresPosesPredictedPastIt) {
    // The runtime predicts each pose 30 ms ahead of the input: the frames read just before the
    // grip opens already show the follow-through (the hand swinging down, faster). The release
    // velocity is the throw's, not theirs.
    const auto release = [](float lead) {
        Rig r;
        r.cfg.throw_gain_curve = 1.0f; // linear, no lift: the release velocity as read
        r.cfg.throw_lift_deg = 0.0f;
        r.in.pose_lead_s = lead;
        r.Grab(0);
        r.in.hand_v = {0.0f, 0.0f, -5.0f};
        r.Step(10);
        r.in.hand_v = {0.0f, -8.0f, -1.0f};
        r.Step(2);
        r.in.grip = 0.0f;
        const Q::Output& o = r.Step();
        EXPECT_EQ(o.request.action, Q::Action::Throw);
        return Q::V3{o.request.v_world[0], o.request.v_world[1], o.request.v_world[2]};
    };
    const Q::V3 compensated = release(0.03f);
    EXPECT_NEAR(compensated.y, 0.0f, 1e-4f);
    const float gain = Q::Config{}.throw_gain;
    EXPECT_NEAR(compensated.z, -5.0f * gain, 0.01f * gain);
    const Q::V3 naive = release(0.0f); // what the poses alone would give
    EXPECT_LT(naive.y, -4.0f);
}

TEST(QuickItems, PeakAfterTheRelease) {
    // The poses predicted past the release are left out of it but reported (the log's "predicted
    // past the release"): the hand still speeding up shows there.
    Q::VelocityHistory h;
    for (int i = 0; i < 10; ++i) {
        const double t = 0.01 * i;
        h.Push(t, {0.0f, 0.0f, -(2.0f + 0.5f * static_cast<float>(i))});
    }
    EXPECT_NEAR(Q::Length(h.Release(0.065)), 5.0f, 0.3f); // up to the 7th sample (5.0 m/s)
    EXPECT_NEAR(h.PeakAfter(0.065), 6.5f, 1e-4f);
    EXPECT_EQ(h.PeakAfter(1.0), 0.0f);
}

// The whole launch at the defaults (the controller's curve and lift, the launch hook's soft cap
// for its pitch): a release of `hand` m/s at `pitch_deg`, a Molotov. Its speed and pitch.
struct Launched {
    float speed{0.0f};
    float pitch{0.0f};
};
Launched LaunchAtDefaults(float hand, float pitch_deg, float power = 1.0f,
                          Q::ThrowClass cls = Q::ThrowClass::Bottle) {
    const Q::Config c{};
    const Q::QuickSettings s{};
    const float p = pitch_deg * 0.01745329f;
    const Q::V3 v{0.0f, hand * std::sin(p), -hand * std::cos(p)};
    const Q::V3 lifted = Q::LiftPitch(
        Q::LaunchVelocity(v, cls, c.throw_gain * power, c.throw_gain_curve), c.throw_lift_deg);
    const Q::ThrowLimits limits = Q::LimitsOf(cls);
    const float cap = Q::ThrowCapAt(limits.range * s.throw_range_factor, 1.5f, 9.81f,
                                    Q::PitchOf(lifted), s.throw_cap_flat_deg, limits.max_speed);
    const Q::V3 out = Q::SoftCap(lifted, cap, s.throw_cap_knee);
    return {Q::Length(out), Q::PitchOf(out)};
}
// Where it lands on flat ground `h` m below the release under real gravity (m).
float LandsAt(float speed, float deg, float h) {
    const float vx = speed * std::cos(deg * 0.01745329f);
    const float vy = speed * std::sin(deg * 0.01745329f);
    return vx * (vy + std::sqrt(vy * vy + 2.0f * 9.81f * h)) / 9.81f;
}
float LandsAt(const Launched& l, float h) {
    return LandsAt(l.speed, l.pitch, h);
}

// The user's first real throw (2026-10-04 headset log): Numbing Mist (the Molotov's flight), the
// hand 2.47 m/s at release, a full wind-up and wrist snap (power 1), launched 7 degrees up from
// 1.5 m. At the old gain 1.2 it left at 3.0 m/s and fell 1.7 m away ("quite a weak throw"). Now:
// ~10 m/s and well past 7 m; a minimum-speed lob still short; a hard throw clearly further, a hard
// flat one fast, nothing past the item's reach (1.6 x the game's own throw).
TEST(QuickItems, ThrowStrength) {
    const Q::Config c{};
    const Launched user = LaunchAtDefaults(2.47f, 7.2f);
    EXPECT_GT(user.speed, 9.5f);
    EXPECT_NEAR(user.pitch, 7.2f + c.throw_lift_deg, 0.05f);
    EXPECT_GT(LandsAt(user, 1.5f), 7.5f);
    // The same release at a throwing angle reaches past the game's own throw of a Molotov (9.9 m
    // traced on the clinic floor).
    EXPECT_GT(LandsAt(LaunchAtDefaults(2.47f, 30.0f), 1.5f), 11.0f);
    // A lob at the slowest speed that throws, 40 degrees up: well short of that.
    EXPECT_LT(LandsAt(LaunchAtDefaults(c.throw_min_speed, 40.0f), 1.3f), 6.5f);
    // A hard overhand (7 m/s) clearly further than a firm one (3 m/s) at the same angle; a push
    // at the same speed shorter than the overhand.
    EXPECT_GT(LandsAt(LaunchAtDefaults(7.0f, 30.0f), 1.5f),
              1.3f * LandsAt(LaunchAtDefaults(3.0f, 30.0f), 1.5f));
    EXPECT_LT(LaunchAtDefaults(3.0f, 0.0f, c.throw_push_factor).speed,
              LaunchAtDefaults(3.0f, 0.0f).speed);
    // Hard and flat: fast (a lob is held to the best arc's 14 m/s, a flat throw may leave at 20).
    EXPECT_GT(LaunchAtDefaults(7.0f, 0.0f).speed, 19.0f);
    EXPECT_LT(LaunchAtDefaults(7.0f, 45.0f).speed, 14.05f);
    // Never past the reach (1.6 x the game's 13.42 m) at any angle, however hard.
    for (float pitch = -20.0f; pitch <= 70.0f; pitch += 5.0f) {
        EXPECT_LT(LandsAt(LaunchAtDefaults(12.0f, pitch), 1.5f), 1.6f * 13.42f + 0.1f) << pitch;
    }
}

// The lift (throw_lift_deg) and the soft cap at the throw's pitch (ThrowCapAt).
TEST(QuickItems, LiftAndCapAtPitch) {
    const auto at = [](float speed, float pitch_deg) {
        const float p = pitch_deg * 0.01745329f;
        return Q::V3{0.3f * speed * std::cos(p), speed * std::sin(p), -0.954f * speed * std::cos(p)};
    };
    float lifted = 0.0f;
    // Flat and moderately up or down: all of it; the speed and the yaw kept.
    for (const float pitch : {-25.0f, -10.0f, 0.0f, 5.0f, 15.0f}) {
        const Q::V3 v = at(10.0f, pitch);
        const Q::V3 l = Q::LiftPitch(v, 6.0f, &lifted);
        EXPECT_NEAR(Q::PitchOf(l), pitch + 6.0f, 0.02f) << pitch;
        EXPECT_NEAR(lifted, 6.0f, 0.02f) << pitch;
        EXPECT_NEAR(Q::Length(l), Q::Length(v), 1e-3f);
        EXPECT_NEAR(std::atan2(l.x, l.z), std::atan2(v.x, v.z), 1e-5f);
    }
    // A lob keeps its own arc, a throw at the feet stays one; in between it fades.
    EXPECT_NEAR(Q::PitchOf(Q::LiftPitch(at(10.0f, 45.0f), 6.0f)), 45.0f, 0.02f);
    EXPECT_NEAR(Q::PitchOf(Q::LiftPitch(at(10.0f, -70.0f), 6.0f)), -70.0f, 0.02f);
    Q::LiftPitch(at(10.0f, 28.0f), 6.0f, &lifted);
    EXPECT_GT(lifted, 1.0f);
    EXPECT_LT(lifted, 5.0f);
    // Straight up (no horizontal part) and nothing: unchanged.
    EXPECT_EQ(Q::LiftPitch({0.0f, 5.0f, 0.0f}, 6.0f).y, 5.0f);
    EXPECT_EQ(Q::Length(Q::LiftPitch({}, 6.0f)), 0.0f);
    // The launch at a pitch that lands exactly `range` away.
    for (const float pitch : {0.0f, 10.0f, 25.0f, 40.0f}) {
        const float v = Q::SpeedForRangeAt(20.0f, 1.5f, 9.81f, pitch);
        EXPECT_NEAR(LandsAt(v, pitch, 1.5f), 20.0f, 0.02f) << pitch;
    }
    EXPECT_EQ(Q::SpeedForRangeAt(20.0f, 1.5f, 9.81f, -10.0f), 0.0f); // below the line to it
    // The cap: the best arc's speed for a lob (45 degrees and steeper), rising toward flat
    // throws, held below flat_deg; every capped throw at most lands at the reach.
    constexpr float Reach = 1.6f * 13.42f;
    const float best = Q::SpeedForRange(Reach, 1.5f, 9.81f);
    EXPECT_NEAR(Q::ThrowCapAt(Reach, 1.5f, 9.81f, 60.0f, 10.0f, 14.0f), best, 1e-3f);
    EXPECT_NEAR(Q::ThrowCapAt(Reach, 1.5f, 9.81f, 45.0f, 10.0f, 14.0f), best, 1e-3f);
    float last = 0.0f;
    for (float pitch = 50.0f; pitch >= 10.0f; pitch -= 1.0f) {
        const float cap = Q::ThrowCapAt(Reach, 1.5f, 9.81f, pitch, 10.0f, 14.0f);
        EXPECT_GE(cap, last - 1e-4f) << pitch;
        EXPECT_LT(LandsAt(cap, pitch, 1.5f), Reach + 0.02f) << pitch;
        if (pitch <= 43.0f) {
            EXPECT_NEAR(LandsAt(cap, pitch, 1.5f), Reach, 0.02f) << pitch; // exactly there
        }
        last = cap;
    }
    EXPECT_NEAR(Q::ThrowCapAt(Reach, 1.5f, 9.81f, 0.0f, 10.0f, 14.0f), last, 1e-3f);
    EXPECT_NEAR(Q::ThrowCapAt(Reach, 1.5f, 9.81f, -30.0f, 10.0f, 14.0f), last, 1e-3f);
    EXPECT_NEAR(last, 21.0f, 0.2f);
    // flat_deg 45: the best arc's speed at every pitch (the old cap); no reach: the fixed one.
    EXPECT_NEAR(Q::ThrowCapAt(Reach, 1.5f, 9.81f, 0.0f, 45.0f, 14.0f), best, 1e-3f);
    EXPECT_EQ(Q::ThrowCapAt(0.0f, 1.5f, 9.81f, 0.0f, 10.0f, 6.0f), 6.0f);
    // The reach from the game's rows: 1.6 x its own throw; coins (dropped) none.
    EXPECT_NEAR(Q::ThrowReachOf(Molotov, 1.6f, 1.5f), 1.6f * 13.42f, 0.25f);
    EXPECT_EQ(Q::ThrowReachOf(Coins, 1.6f, 1.5f), 0.0f);
    // The class fallbacks' ranges are the rows' own throws.
    EXPECT_NEAR(Q::LimitsOf(Q::ThrowClass::Bottle).range, Q::VanillaRange(Molotov, 1.5f), 0.15f);
    EXPECT_NEAR(Q::LimitsOf(Q::ThrowClass::Knife).range, Q::VanillaRange(Knife, 1.5f), 0.4f);
    EXPECT_NEAR(Q::LimitsOf(Q::ThrowClass::Stone).range, Q::VanillaRange(Pebble, 1.5f), 0.2f);
    EXPECT_EQ(Q::LimitsOf(Q::ThrowClass::Coin).range, 0.0f);
    // Knives fly straighter and faster than a bottle from the same hand: at 3 m/s flat, a knife
    // drops under half a metre over 8 m.
    const Launched knife = LaunchAtDefaults(3.0f, 0.0f, 1.0f, Q::ThrowClass::Knife);
    const Launched bottle = LaunchAtDefaults(3.0f, 0.0f);
    EXPECT_GT(knife.speed, 1.25f * bottle.speed);
    const float t8 = 8.0f / (knife.speed * std::cos(knife.pitch * 0.01745329f));
    const float rise8 = knife.speed * std::sin(knife.pitch * 0.01745329f) * t8 - 4.905f * t8 * t8;
    EXPECT_GT(rise8, -0.5f);
}

// TurnToward: a direction turned at most so far toward another.
TEST(QuickItems, TurnToward) {
    const Q::V3 fwd{0.0f, 0.0f, -2.0f};
    const Q::V3 up50{0.0f, std::sin(0.8727f), -std::cos(0.8727f)};
    const Q::V3 t = Q::TurnToward(fwd, up50, 20.0f);
    EXPECT_NEAR(Q::Length(t), 1.0f, 1e-4f);
    EXPECT_NEAR(Q::PitchOf(t), 20.0f, 0.05f);
    EXPECT_NEAR(Q::PitchOf(Q::TurnToward(fwd, up50, 60.0f)), 50.0f, 0.05f);
    EXPECT_NEAR(Q::PitchOf(Q::TurnToward(fwd, up50, 0.0f)), 0.0f, 0.05f);
    EXPECT_NEAR(Q::TurnToward(fwd, {}, 20.0f).z, -1.0f, 1e-5f);
}

TEST(QuickItems, ThrowAddsTheItemsSpin) {
    // A Molotov (its bottle 12 cm below the fist) snapped forward about the grip's X: the bottle
    // gets the spin's 1.2 m/s on top of the hand's 5 m/s.
    const auto release = [](float spin) {
        Rig r;
        r.cfg.throw_spin = spin;
        r.cfg.throw_gain_curve = 1.0f; // linear, no lift: the release velocity as read
        r.cfg.throw_lift_deg = 0.0f;
        r.Grab(0);
        r.in.hand_v = {0.0f, 0.0f, -5.0f};
        r.in.hand_wv = {-10.0f, 0.0f, 0.0f};
        r.in.hand_w = 10.0f;
        r.Step(6);
        r.in.grip = 0.0f;
        const Q::Output& o = r.Step();
        EXPECT_EQ(o.request.action, Q::Action::Throw);
        return Q::V3{o.request.v_world[0], o.request.v_world[1], o.request.v_world[2]};
    };
    const Q::V3 with = release(1.0f);
    const Q::V3 without = release(0.0f);
    EXPECT_NEAR(without.y, 0.0f, 1e-4f);
    const float gain = Q::Config{}.throw_gain;
    EXPECT_NEAR(with.y, 1.2f * gain, 0.02f * gain); // the spin's 1.2 m/s x the gain
    EXPECT_NEAR(with.z, without.z, 1e-4f);
}

TEST(QuickItems, StaleReleasePutsBack) {
    // Fast, then the hand not tracked for a second, then the release: back in the slot.
    Rig r;
    r.Grab(0);
    r.in.hand_v = {0.0f, 1.0f, -6.0f};
    r.Step(6);
    r.in.hand_valid = false;
    r.Step(90);
    r.in.grip = 0.0f;
    const Q::Output& o = r.Step();
    EXPECT_FALSE(o.send && o.request.action == Q::Action::Throw);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
}

// ---- Throw feel (2026-10-05: "much better now but still not perfect") ---------------------------

// The user's Quest over Link: 124 input frames a second, every pose predicted for its display
// time 45 ms ahead (41-50 ms in the log).
constexpr double LinkDt = 1.0 / 124.0;
constexpr float LinkLead = 0.045f;

// How the runtime predicts a pose for the display time f + LinkLead from the input frame f, for a
// hand whose true speed is s(t): "hold" its velocity as of f (no extrapolation of the speed),
// "accel" extrapolated with its acceleration, "ideal" the true future (the mock, which knows its
// scripted hand in advance). The grip located at f itself (the fresh pose) reads s(f).
enum class Predict { Hold, Accel, Ideal };
float Predicted(const std::function<float(double)>& s, double f, Predict p) {
    switch (p) {
    case Predict::Hold:
        return s(f);
    case Predict::Accel:
        return std::max(0.0f, s(f) + (s(f + 1e-4) - s(f - 1e-4)) / 2e-4f * LinkLead);
    case Predict::Ideal:
        return s(f + LinkLead);
    }
    return 0.0f;
}

struct Replay {
    Q::V3 launch{};          // the request's velocity (zero: none went out)
    double open_t{0.0};      // the frame the grip opened
    double sent_t{-1.0};     // the frame the throw went out
    bool held_at_open{false}; // the item still in the hand in the grip's opening frame
    bool put_back{false};
};

// One throw into a Rig at the Link's input rate: the hand moves along `dir` at the true speed
// `s(t)` (t: s from the throw's own zero), the grip opens at the first frame at or after
// `open_at`; 0.15 s more frames follow. Power 1 (no shoulder: no wind-up measured).
Replay ReplayThrow(Q::Config cfg, const std::function<float(double)>& s, double open_at, Q::V3 dir,
                   Predict p, const std::function<void(Rig&, double)>& during = {}) {
    Rig r;
    r.cfg = cfg;
    r.Grab(0);
    const double t0 = r.t + 0.6; // the throw's own zero
    Replay out{};
    bool open = false;
    for (double t = r.t + LinkDt; t < t0 + open_at + 0.15; t += LinkDt) {
        const double f = t - t0;
        r.t = t;
        r.in.t = t;
        r.in.dt = static_cast<float>(LinkDt);
        r.in.hover.steady_ns = static_cast<std::int64_t>(t * 1e9);
        r.in.pose_lead_s = LinkLead;
        r.in.hand_valid = true;
        r.in.hand_v = dir * Predicted(s, f, p);
        r.in.hand_now_valid = true;
        r.in.hand_v_now = dir * s(f);
        const bool opening = !open && f >= open_at - 1e-6;
        if (opening) {
            open = true;
            r.in.grip = 0.0f;
            out.open_t = t;
        }
        if (during) {
            during(r, f);
        }
        r.c.Update(r.cfg, r.in, r.out);
        if (opening) {
            out.held_at_open = r.out.held_goods == 1200;
            out.put_back = r.c.GetPhase() == Q::Phase::Idle && !r.out.send;
        }
        if (r.out.send && r.out.request.action == Q::Action::Throw && out.sent_t < 0.0) {
            out.sent_t = t;
            out.launch = {r.out.request.v_world[0], r.out.request.v_world[1],
                          r.out.request.v_world[2]};
        }
    }
    return out;
}

// predsim.py's profile: the speed rises (sin^2) over `rise_s` to V at 0, then falls (cos^2) over
// `fall_s`.
std::function<float(double)> Swing(float V, double rise_s = 0.15, double fall_s = 0.10) {
    return [=](double t) {
        if (t < -rise_s || t >= fall_s) {
            return 0.0f;
        }
        if (t < 0.0) {
            const double u = (t + rise_s) / rise_s;
            return V * static_cast<float>(std::pow(std::sin(0.5 * 3.14159265 * u), 2.0));
        }
        return V * static_cast<float>(std::pow(std::cos(0.5 * 3.14159265 * t / fall_s), 2.0));
    };
}

// The hand speed a throw read: gain 1, linear, no lift (the launch is the release velocity).
Q::Config ReadSpeed(bool fresh, float wait_s) {
    Q::Config c{};
    c.throw_gain = 1.0f;
    c.throw_gain_curve = 1.0f;
    c.throw_lift_deg = 0.0f;
    c.throw_fresh = fresh;
    c.throw_rise_wait_s = wait_s;
    return c;
}

// The release speed from the grip located at the input time (and the wait for a hand still
// speeding up) against the display-time poses, under each way a runtime may predict them. The
// user's real throws read 1.04-2.02x faster just after the release than at it (median 1.41): with
// a "hold" runtime the grip opened ~5 ms before the hand's peak, and the display-time poses up to
// the release (predicted 45 ms earlier) read ~0.7 of it.
TEST(QuickItems, FreshPosesReadTheHandsPeak) {
    const Q::V3 fwd{0.0f, 0.0f, -1.0f};
    const Q::Config d{};
    constexpr float V = 4.0f;
    const auto read = [&](Predict p, double open_at, bool fresh, float wait) {
        return Q::Length(ReplayThrow(ReadSpeed(fresh, wait), Swing(V), open_at, fwd, p).launch) / V;
    };
    std::string table;
    for (const Predict p : {Predict::Hold, Predict::Accel, Predict::Ideal}) {
        for (const double open : {-0.037, -0.020, -0.005, 0.0, 0.020, 0.060}) {
            const float before = read(p, open, false, 0.0f);
            const float now = read(p, open, true, d.throw_rise_wait_s);
            char line[96];
            std::snprintf(line, sizeof(line), " %s %+.0f ms: %.2f -> %.2f V;",
                          p == Predict::Hold ? "hold" : p == Predict::Accel ? "accel" : "ideal",
                          open * 1000.0, before, now);
            table += line;
            // Never weaker than before (the stronger of the two readings counts; within the
            // averaging's few percent), never past the hand's own peak unless the display-time
            // poses already were (an "accel" runtime at or after the peak: as before).
            EXPECT_GT(now, before - 0.04f) << table;
            EXPECT_LT(now, std::max(before, 1.0f) + 0.01f) << table;
        }
    }
    std::printf("[ QuickItems.FreshPosesReadTheHandsPeak ] the release speed / the hand's peak "
                "(display-time poses -> fresh + wait):%s\n",
                table.c_str());
    // A "hold" runtime (the user's ratio: the grip ~5 ms before the peak): ~0.7 V before, the peak
    // now.
    EXPECT_LT(read(Predict::Hold, -0.005, false, 0.0f), 0.80f);
    EXPECT_GT(read(Predict::Hold, -0.005, true, d.throw_rise_wait_s), 0.95f);
    // An "accel" runtime with the grip 37 ms before the peak (its reading of the user's ratio):
    // the wait reaches most of the rest of the way.
    EXPECT_GT(read(Predict::Accel, -0.037, true, d.throw_rise_wait_s),
              read(Predict::Accel, -0.037, true, 0.0f) + 0.05f);
    EXPECT_GT(read(Predict::Accel, -0.037, true, d.throw_rise_wait_s), 0.93f);
    // After the peak: the peak (both, within the averaging).
    EXPECT_GT(read(Predict::Hold, 0.020, true, d.throw_rise_wait_s), 0.95f);
    // The mock (ideal prediction, released at the peak): unchanged.
    EXPECT_NEAR(read(Predict::Ideal, 0.0, true, d.throw_rise_wait_s),
                read(Predict::Ideal, 0.0, false, 0.0f), 0.03f);
    // No fresh poses at all (a runtime that cannot locate the past): the display-time ones, as
    // with throw_fresh off.
    Q::Config none = ReadSpeed(true, d.throw_rise_wait_s);
    const Replay r =
        ReplayThrow(none, Swing(V), 0.0, fwd, Predict::Hold,
                    [](Rig& rig, double) { rig.in.hand_now_valid = false; });
    EXPECT_NEAR(Q::Length(r.launch) / V, read(Predict::Hold, 0.0, false, d.throw_rise_wait_s),
                1e-3f);
}

// The grip opened while the hand still speeds up: the item stays in the hand a frame or two more
// and leaves with the hand's peak (at most throw_rise_wait_s later); a slow release is never held
// back into a throw; a context switch in the wait puts it away (nothing thrown, nothing lost).
TEST(QuickItems, RisingWaitTakesThePeak) {
    const Q::V3 fwd{0.0f, 0.1f, -1.0f};
    const Q::Config d{};
    constexpr float V = 4.0f;
    const Replay early = ReplayThrow(ReadSpeed(true, d.throw_rise_wait_s), Swing(V), -0.016,
                                     Q::Normalize(fwd), Predict::Hold);
    EXPECT_TRUE(early.held_at_open); // still in the hand in the grip's opening frame
    EXPECT_GT(early.sent_t, early.open_t);
    EXPECT_LE(early.sent_t - early.open_t, d.throw_rise_wait_s + 1e-4);
    const float at_open = Swing(V)(-0.016); // 0.97 V
    EXPECT_GT(Q::Length(early.launch), at_open + 0.05f);
    EXPECT_GT(Q::Length(early.launch), 0.98f * V);
    // Without the wait: at once, the speed as the grip opened.
    const Replay now =
        ReplayThrow(ReadSpeed(true, 0.0f), Swing(V), -0.016, Q::Normalize(fwd), Predict::Hold);
    EXPECT_FALSE(now.held_at_open);
    EXPECT_NEAR(now.sent_t, now.open_t, 1e-9);
    EXPECT_NEAR(Q::Length(now.launch), at_open, 0.05f);
    // Earlier still (40 ms before the peak, 0.83 V): the wait ends at its 25 ms with what the hand
    // has then.
    const Replay earlier = ReplayThrow(ReadSpeed(true, d.throw_rise_wait_s), Swing(V), -0.040,
                                       Q::Normalize(fwd), Predict::Hold);
    EXPECT_NEAR(earlier.sent_t - earlier.open_t, d.throw_rise_wait_s, LinkDt + 1e-4);
    EXPECT_GT(Q::Length(earlier.launch), Swing(V)(-0.040) + 0.3f);
    // After the peak (the hand slowing): at once.
    const Replay late = ReplayThrow(ReadSpeed(true, d.throw_rise_wait_s), Swing(V), 0.02,
                                    Q::Normalize(fwd), Predict::Hold);
    EXPECT_NEAR(late.sent_t, late.open_t, 1e-9);
    // A slow hand speeding up (1 m/s at the opening, under throw_min_speed): back at once, never
    // a throw.
    const Replay slow = ReplayThrow(ReadSpeed(true, d.throw_rise_wait_s), Swing(1.6f), -0.05,
                                    Q::Normalize(fwd), Predict::Hold);
    EXPECT_TRUE(slow.put_back);
    EXPECT_LT(slow.sent_t, 0.0);
    // A context switch (a menu) during the wait: put away, nothing sent.
    const Replay menu = ReplayThrow(
        ReadSpeed(true, d.throw_rise_wait_s), Swing(V), -0.016, Q::Normalize(fwd), Predict::Hold,
        [](Rig& r, double f) { r.in.gameplay = f < -0.010; });
    EXPECT_LT(menu.sent_t, 0.0);
}

// The 300 ms look-back's earlier peak gives its speed, the release keeps most of its direction
// (the user's two such throws took the earlier sample's 46-56 degrees up and launched steeply).
TEST(QuickItems, EarlierPeakKeepsTheReleaseDirection) {
    Rig r;
    r.cfg = ReadSpeed(false, 0.0f);
    r.Grab(0);
    const float up = 50.0f * 0.01745329f;
    r.in.hand_v = Q::V3{0.0f, std::sin(up), -std::cos(up)} * 5.0f; // the peak, 50 degrees up
    r.Step(4);
    // Slowing, flat, still a throw's speed at the release (throw_min_speed 1.5: a slower release
    // is a put-back, SlowDriftAfterAReachPutsBack).
    r.in.hand_v = {0.0f, 0.0f, -1.6f};
    r.Step(16);
    r.in.grip = 0.0f;
    r.Step();
    const Q::V3 v{r.sent.v_world[0], r.sent.v_world[1], r.sent.v_world[2]};
    EXPECT_EQ(r.sent.action, Q::Action::Throw);
    EXPECT_NEAR(Q::Length(v), 5.0f, 0.05f);
    EXPECT_NEAR(Q::PitchOf(v), r.cfg.throw_peak_turn_deg, 0.1f);
}

// The user's 9 real Molotov throws (2026-10-05 00:56 headset log, "much better now but still not
// perfect"), replayed: each one's hand speed as the throw read it then, the runtime's fastest pose
// just past the release (what a pose located at the input time reads under a "hold" runtime), the
// launch's pitch and the release's height (no running carry). Before: median 6 m, the flat ones
// 3.5-6 m (the game's own Molotov: 9.9-13.4 m). Now: further every time, the median past the
// game's own throw, the weak one still short, nothing past 1.6 x the game's range.
TEST(QuickItems, UserThrowsReplay) {
    struct UserThrow {
        float hand, after, pitch, height;
    };
    constexpr std::array<UserThrow, 9> user{{
        {2.08f, 2.23f, -6.7f, 1.69f},
        {2.66f, 4.11f, 19.9f, 1.70f},
        {2.31f, 3.26f, 47.2f, 1.39f},
        {1.80f, 2.33f, 1.1f, 1.46f},
        {4.52f, 6.16f, 17.0f, 1.62f},
        {3.66f, 3.82f, 35.5f, 1.26f},
        {2.54f, 4.75f, 0.6f, 1.63f},
        {2.00f, 3.16f, -12.3f, 1.52f},
        {2.61f, 5.27f, -1.9f, 1.43f},
    }};
    const Q::Config d{};
    const Q::QuickSettings s{};
    const float reach = 1.6f * 13.42f;
    std::vector<float> before, now;
    std::string table;
    for (const UserThrow& u : user) {
        // A hand speeding up to `after` at the grip's opening (`hand` 45 ms before it, the
        // display-time poses' newest), then slowing over 100 ms.
        const float hand = u.hand, after = u.after;
        const auto speed = [hand, after](double t) {
            if (t < -0.30) {
                return 0.0f;
            }
            if (t < -0.045) {
                return hand * static_cast<float>((t + 0.30) / 0.255);
            }
            if (t < 0.0) {
                return hand + (after - hand) * static_cast<float>((t + 0.045) / 0.045);
            }
            return std::max(0.0f, after * static_cast<float>(1.0 - t / 0.10));
        };
        const float p = u.pitch * 0.01745329f;
        const Q::V3 dir{0.0f, std::sin(p), -std::cos(p)};
        // What the throw read then (the display-time poses, linear): `hand`.
        const float old_read =
            Q::Length(ReplayThrow(ReadSpeed(false, 0.0f), speed, 0.0, dir, Predict::Hold).launch);
        EXPECT_NEAR(old_read, hand, 0.03f * hand);
        // Before: x4 then the old cap (the best arc's 14 m/s, bending from 7).
        const float old_speed = Q::Length(Q::SoftCap(dir * (4.0f * old_read), 14.0f, 0.5f));
        before.push_back(LandsAt(old_speed, u.pitch, u.height));
        // Now: the defaults end to end (the controller's request, the hook's cap at its pitch).
        const Replay r = ReplayThrow(d, speed, 0.0, dir, Predict::Hold);
        const Q::V3 launch = r.launch;
        const float cap = Q::ThrowCapAt(reach, 1.5f, 9.81f, Q::PitchOf(launch),
                                        s.throw_cap_flat_deg, 14.0f);
        const Q::V3 capped = Q::SoftCap(launch, cap, s.throw_cap_knee);
        now.push_back(LandsAt(Q::Length(capped), Q::PitchOf(capped), u.height));
        const float new_read = Q::Length(ReplayThrow(ReadSpeed(true, d.throw_rise_wait_s), speed,
                                                     0.0, dir, Predict::Hold)
                                             .launch);
        EXPECT_GT(new_read, 0.9f * after);
        EXPECT_GT(now.back(), before.back() + 0.5f);
        EXPECT_LT(now.back(), reach + 0.1f);
        char line[160];
        std::snprintf(line, sizeof(line),
                      " %.2f->%.2f m/s %+.0f deg: %.1f m/s %.1f m -> %.1f m/s %+.0f deg %.1f m;",
                      old_read, new_read, u.pitch, old_speed, before.back(), Q::Length(capped),
                      Q::PitchOf(capped), now.back());
        table += line;
    }
    std::vector<float> b = before, n = now;
    std::sort(b.begin(), b.end());
    std::sort(n.begin(), n.end());
    std::printf("[ QuickItems.UserThrowsReplay ] the user's 9 throws (hand read before -> now, "
                "launch and landing before -> now):%s median %.1f -> %.1f m\n",
                table.c_str(), b[4], n[4]);
    EXPECT_LT(b[4], 6.5f);
    EXPECT_GT(n[4], 9.9f); // past the game's own Molotov on the clinic floor
    EXPECT_LT(now[3], 9.0f); // the weakest throw (1.8 m/s then) still drops short
}

// ---- Arc selection (2026-10-03: "sometimes it's hard to select an item from the Y menu") -------

// The hover volume is round the visible fist: 6 cm in the card's plane (a 3 cm card half and a
// 3-4 cm fist half), 10 cm along the line of sight; kept to 1.35 x that.
TEST(ArcSelect, HoverReachesTheFist) {
    const Q::V3 eye{0.0f, 1.6f, 0.0f};
    const Q::Layout l = Q::PlaceArc(6, {-0.2f, 1.1f, -0.3f}, eye, {});
    for (int k = 0; k < l.n; ++k) {
        const Q::V3 c = l.card[static_cast<size_t>(k)];
        const Q::V3 n = Q::Normalize(eye - c);
        const Q::V3 out = Q::Normalize(c - l.pivot); // radially out, in the plane
        EXPECT_EQ(Q::PickHover(l, c + out * 0.055f, eye, -1, 1.0f), k) << k;
        EXPECT_EQ(Q::PickHover(l, c - out * 0.055f, eye, -1, 1.0f), k) << k; // short of it
        EXPECT_EQ(Q::PickHover(l, c + n * 0.09f, eye, -1, 1.0f), k) << k;   // in front of it
        EXPECT_EQ(Q::PickHover(l, c - n * 0.09f, eye, -1, 1.0f), k) << k;   // pushed through
        EXPECT_EQ(Q::PickHover(l, c + out * 0.065f, eye, -1, 1.0f), -1) << k;
        EXPECT_EQ(Q::PickHover(l, c + out * 0.075f, eye, k, 1.0f), k) << k; // kept
        EXPECT_EQ(Q::PickHover(l, c + out * 0.085f, eye, k, 1.0f), -1) << k;
    }
}

// Six cards 9.3 cm apart (radius 0.18, 150 degrees): the halfway line between two cards is
// 4.6 cm from each, so a 2 cm aiming error picks the neighbour 2% of the time (was 7% at 7.2 cm).
TEST(ArcSelect, Spacing) {
    const Q::V3 eye{0.0f, 1.6f, 0.0f};
    for (int n = 2; n <= Q::SlotCount; ++n) {
        const Q::Layout l = Q::PlaceArc(n, {-0.2f, 1.1f, -0.3f}, eye, {});
        for (int i = 0; i + 1 < n; ++i) {
            EXPECT_GE(Q::Length(l.card[static_cast<size_t>(i)] - l.card[static_cast<size_t>(i + 1)]),
                      0.092f)
                << n << " " << i;
        }
        // The end cards stay above the hand.
        EXPECT_GT(l.card[0].y, l.pivot.y + 0.02f) << n;
    }
}

// Sweeping the hand along the arc's line hovers every card in turn with no gap and no
// back-and-forth (each card entered once): the nearest card, with hysteresis.
TEST(ArcSelect, SweepHasNoGapsNoFlicker) {
    const Q::V3 eye{0.0f, 1.6f, 0.0f};
    const Q::Layout l = Q::PlaceArc(6, {-0.2f, 1.1f, -0.3f}, eye, {});
    for (const int direction : {1, -1}) {
        int current = -1;
        int changes = 0;
        int last_seen = direction > 0 ? -1 : l.n;
        const float a0 = direction * (-0.5f * 5.0f * l.step_rad - 0.3f * l.step_rad);
        for (int s = 0; s <= 400; ++s) {
            const float a = a0 - 2.0f * a0 * static_cast<float>(s) / 400.0f;
            const Q::V3 p =
                l.pivot + l.right * (std::sin(a) * 0.18f) + l.up * (std::cos(a) * 0.18f);
            const int h = Q::PickHover(l, p, eye, current, 1.0f);
            if (s > 10 && s < 390) {
                EXPECT_GE(h, 0) << s; // never a gap between two cards
            }
            if (h != current) {
                ++changes;
                if (h >= 0) {
                    EXPECT_TRUE(direction > 0 ? h > last_seen : h < last_seen) << s; // never back
                    last_seen = h;
                }
            }
            current = h;
        }
        EXPECT_LE(changes, 7);
    }
    // A hand trembling 5 mm round the halfway point between two cards never switches back and
    // forth (the switch needs the other card 1.5 cm nearer: a band of +-7.5 mm).
    const Q::V3 mid = (l.card[2] + l.card[3]) * 0.5f;
    const Q::V3 along = Q::Normalize(l.card[3] - l.card[2]);
    int h = Q::PickHover(l, mid - along * 0.012f, eye, -1, 1.0f);
    EXPECT_EQ(h, 2);
    int switches = 0;
    for (int s = 0; s < 200; ++s) {
        const float wobble = 0.005f * std::sin(static_cast<float>(s) * 0.7f);
        const int next = Q::PickHover(l, mid + along * wobble, eye, h, 1.0f);
        switches += next != h ? 1 : 0;
        h = next;
    }
    EXPECT_EQ(switches, 0);
}

// 2026-10-08, the user: "a tiny bit bigger area of selection (but not to the point where it gets
// hard to switch/move between them) ... a tiny tiny bit bigger icons". The cards 6 -> 6.4 cm, the
// hover volume x1.12 (6.7 cm in the card's plane, 11.2 cm along the line of sight): a hand 10%
// past today's edge (out, short of it, above, in front, behind) now lights the card, and is kept
// to 1.35 x the wider volume; moving from card to card switches exactly where it did.
TEST(ArcSelect, EasierPick20261008) {
    const Q::QuickSettings d{};
    EXPECT_NEAR(d.card_m, 0.064f, 1e-6f);
    EXPECT_NEAR(d.hover_scale, 1.12f, 1e-6f);
    const float now = d.hover_scale;
    const Q::V3 eye{0.0f, 1.6f, 0.0f};
    const Q::Layout l = Q::PlaceArc(6, {-0.2f, 1.1f, -0.3f}, eye, {});
    // The cards keep a clear gap at the 9.3 cm spacing (2.9 cm; 2.6 cm next to a lit one, 8%
    // larger).
    EXPECT_GT(Q::Length(l.card[2] - l.card[3]) - d.card_m, 0.028f);
    EXPECT_GT(Q::Length(l.card[2] - l.card[3]) - 0.5f * d.card_m * (1.0f + 1.08f), 0.023f);
    for (int k = 0; k < l.n; ++k) {
        const Q::V3 c = l.card[static_cast<size_t>(k)];
        const Q::V3 n = Q::Normalize(eye - c);
        const Q::V3 out = Q::Normalize(c - l.pivot); // radially out, in the plane
        std::vector<Q::V3> past{c + out * 0.066f, c - out * 0.066f, c + n * 0.11f,
                                c - n * 0.11f};
        if (k == 2 || k == 3) {
            past.push_back(c + l.up * 0.066f); // straight up from a middle card
        }
        for (size_t i = 0; i < past.size(); ++i) {
            EXPECT_EQ(Q::PickHover(l, past[i], eye, -1, 1.0f), -1) << k << " " << i; // today
            EXPECT_EQ(Q::PickHover(l, past[i], eye, -1, now), k) << k << " " << i;   // lights
        }
        // Today's edge itself (6 cm out, 10 cm in front) lights, well inside the new volume.
        EXPECT_LT(Q::HoverMetric(c + out * 0.06f, c, n, now), 0.92f) << k;
        EXPECT_LT(Q::HoverMetric(c + n * 0.10f, c, n, now), 0.92f) << k;
        // Kept to 1.35 x the wider volume: 8.5 cm out (dropped today) stays lit, 10 cm does not.
        EXPECT_EQ(Q::PickHover(l, c + out * 0.085f, eye, k, 1.0f), -1) << k;
        EXPECT_EQ(Q::PickHover(l, c + out * 0.085f, eye, k, now), k) << k;
        EXPECT_EQ(Q::PickHover(l, c + out * 0.10f, eye, k, now), -1) << k;
    }
    // Sweeping along the arc both ways (0.25 degree steps): every switch from a card to its
    // neighbour happens at the same step as with today's volume, never back, no gap.
    for (const int direction : {1, -1}) {
        const float a0 = direction * (-0.5f * 5.0f * l.step_rad - 0.2f * l.step_rad);
        const int steps = 2400;
        int was = -1, then = -1;
        std::vector<std::pair<int, int>> switches_was, switches_now; // (step, new card)
        int last_seen = direction > 0 ? -1 : l.n;
        for (int s = 0; s <= steps; ++s) {
            const float a = a0 - 2.0f * a0 * static_cast<float>(s) / static_cast<float>(steps);
            const Q::V3 p =
                l.pivot + l.right * (std::sin(a) * 0.18f) + l.up * (std::cos(a) * 0.18f);
            const int h1 = Q::PickHover(l, p, eye, was, 1.0f);
            const int h2 = Q::PickHover(l, p, eye, then, now);
            if (h1 != was && h1 >= 0 && was >= 0) {
                switches_was.push_back({s, h1});
            }
            if (h2 != then && h2 >= 0 && then >= 0) {
                switches_now.push_back({s, h2});
            }
            if (h2 != then && h2 >= 0) {
                EXPECT_TRUE(direction > 0 ? h2 > last_seen : h2 < last_seen) << s;
                last_seen = h2;
            }
            if (s > steps / 20 && s < steps - steps / 20) {
                EXPECT_GE(h2, 0) << s;
            }
            was = h1;
            then = h2;
        }
        ASSERT_EQ(switches_was.size(), 5u) << direction;
        ASSERT_EQ(switches_now.size(), switches_was.size()) << direction;
        for (size_t i = 0; i < switches_was.size(); ++i) {
            EXPECT_NEAR(switches_now[i].first, switches_was[i].first, 1) << direction << " " << i;
            EXPECT_EQ(switches_now[i].second, switches_was[i].second) << direction << " " << i;
        }
    }
    // The switch between two neighbours along their line: 1.5 cm nearer (7.5 mm past halfway)
    // at both scales, to the half millimetre.
    const Q::V3 from = l.card[2];
    const Q::V3 along = Q::Normalize(l.card[3] - l.card[2]);
    const float gap = Q::Length(l.card[3] - l.card[2]);
    float at[2] = {-1.0f, -1.0f};
    const float scales[2] = {1.0f, now};
    for (int j = 0; j < 2; ++j) {
        int h = 2;
        for (int s = 0; s <= 2000 && at[j] < 0.0f; ++s) {
            const float x = gap * static_cast<float>(s) / 2000.0f;
            h = Q::PickHover(l, from + along * x, eye, h, scales[j]);
            if (h == 3) {
                at[j] = x;
            }
        }
    }
    EXPECT_NEAR(at[0], 0.5f * gap + 0.0075f, 0.0005f);
    EXPECT_NEAR(at[1], at[0], 0.0001f);
    // A hand trembling 5 mm round the halfway point never switches back and forth at 1.12 either.
    const Q::V3 mid = (l.card[2] + l.card[3]) * 0.5f;
    int h = Q::PickHover(l, mid - along * 0.012f, eye, -1, now);
    EXPECT_EQ(h, 2);
    int flips = 0;
    for (int s = 0; s < 200; ++s) {
        const float wobble = 0.005f * std::sin(static_cast<float>(s) * 0.7f);
        const int next = Q::PickHover(l, mid + along * wobble, eye, h, now);
        flips += next != h ? 1 : 0;
        h = next;
    }
    EXPECT_EQ(flips, 0);
}

// A squeeze just after the hand left a card (its own pull) takes that card within 150 ms.
TEST(ArcSelect, LateHover150ms) {
    for (const double after : {0.10, 0.20}) {
        Rig r;
        r.Open();
        r.Step(80); // past the quick draw
        r.Hover(1);
        r.Step(12);
        r.in.hover.slot = -1; // left the card
        r.Step(static_cast<int>(after / Dt));
        r.in.grip = 1.0f;
        r.Step();
        if (after < 0.15) {
            EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
            EXPECT_EQ(r.c.HeldSlot(), 1);
            EXPECT_NE(r.out.log.find("via=late hover"), std::string::npos) << r.out.log;
        } else {
            EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
        }
    }
}

// The Hover record the render-camera hook publishes for a layout (every slot on the arc).
void Publish(Rig& r, const Q::Layout& l, Q::V3 hand, Q::V3 eye, int current = -1) {
    r.in.hover_fresh = true;
    r.in.hover.open_serial = r.c.OpenSerial();
    r.in.hover.placed = true;
    const int h = Q::PickHover(l, hand, eye, current, 1.0f);
    r.in.hover.slot = static_cast<std::int8_t>(h);
    r.in.hover.hand = {hand.x, hand.y, hand.z};
    r.in.hover.centre = {l.pivot.x, l.pivot.y, l.pivot.z};
    r.in.hover.eye = {eye.x, eye.y, eye.z};
    r.in.hover.scale = 1.0f;
    r.in.hover.card_count = static_cast<std::uint8_t>(l.n);
    for (int i = 0; i < l.n; ++i) {
        const Q::V3 c = l.card[static_cast<size_t>(i)];
        r.in.hover.cards[static_cast<size_t>(i)] = {c.x, c.y, c.z};
        r.in.hover.card_slot[static_cast<size_t>(i)] = static_cast<std::int8_t>(i);
    }
    r.in.hand_p = hand;
}

// A squeeze with no card lit takes the card clearly nearest the hand (metric <= 1.5: 9 cm in the
// card's plane); further, or between two cards, it is still "grip on no card".
TEST(ArcSelect, NearestOnMiss) {
    const Q::V3 eye{0.0f, 1.6f, 0.0f};
    const Q::Layout l = Q::PlaceArc(4, {-0.2f, 1.1f, -0.3f}, eye, {});
    const Q::V3 c2 = l.card[2];
    const Q::V3 out = Q::Normalize(c2 - l.pivot);
    struct Case {
        Q::V3 hand;
        int slot; // -1: no grab
    };
    const Q::V3 mid = (l.card[1] + l.card[2]) * 0.5f;
    const Case cases[] = {
        {c2 + out * 0.075f, 2},                           // 7.5 cm out: metric 1.25
        {c2 + out * 0.10f, -1},                           // 10 cm out: 1.67, too far
        {mid + Q::Normalize(mid - l.pivot) * 0.075f, -1}, // between two: no coin toss
    };
    for (const Case& k : cases) {
        Rig r;
        r.Open();
        r.Step(80); // past the quick draw
        Publish(r, l, k.hand, eye);
        ASSERT_EQ(r.in.hover.slot, -1); // not a hover
        r.Step(3);
        r.in.grip = 1.0f;
        const Q::Output& o = r.Step();
        if (k.slot >= 0) {
            EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
            EXPECT_EQ(r.c.HeldSlot(), k.slot);
            EXPECT_NE(o.log.find("via=nearest"), std::string::npos) << o.log;
        } else {
            EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
            EXPECT_EQ(o.haptics[0], Q::Haptic::Empty);
            EXPECT_NE(o.log.find("grip on no card"), std::string::npos) << o.log;
        }
    }
    // quick_arc_nearest=0: off.
    Rig r;
    r.cfg.nearest_max = 0.0f;
    r.Open();
    r.Step(80);
    Publish(r, l, c2 + out * 0.075f, eye);
    r.Step(3);
    r.in.grip = 1.0f;
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
}

// Never the nearest card in the first 120 ms of an open (the hand is where it opened, and a
// squeeze there is the quick draw's).
TEST(ArcSelect, NearestNeverRightAfterTheOpen) {
    const Q::V3 eye{0.0f, 1.6f, 0.0f};
    const Q::Layout l = Q::PlaceArc(4, {-0.2f, 1.1f, -0.3f}, eye, {});
    const Q::V3 out = Q::Normalize(l.card[2] - l.pivot);
    Rig r;
    r.cfg.quick_draw = false;
    r.Open();
    Publish(r, l, l.card[2] + out * 0.075f, eye);
    r.Step(8); // 89 ms after the open (the hover volume opens at 80, the nearest at 120)
    r.in.grip = 1.0f;
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
}

// Y while the game has the arm (a roll, a knockdown): the arc waits for it and is placed round
// your hand afterwards; its unfolding (hover 80 ms, nearest 120 ms) counts from that placement.
TEST(ArcSelect, UnfoldCountsFromAPlacementThatWaitedForTheArm) {
    const Q::V3 eye{0.0f, 1.6f, 0.0f};
    const Q::Layout l = Q::PlaceArc(4, {-0.2f, 1.1f, -0.3f}, eye, {});
    const Q::V3 out = Q::Normalize(l.card[2] - l.pivot);
    Rig r;
    r.cfg.quick_draw = false;
    r.in.arm_weight = 0.1f;
    r.Open();
    r.Step(45); // 0.5 s in a roll: not placed
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
    r.in.arm_weight = 1.0f;
    Publish(r, l, l.card[2] + out * 0.075f, eye);
    r.in.hover.placed_ns = static_cast<std::int64_t>((r.t + Dt) * 1e9); // placed now
    r.Step(8); // 89 ms after the placement: no nearest yet
    r.in.grip = 1.0f;
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
    // A new squeeze once it has unfolded: the nearest card.
    r.in.grip = 0.0f;
    r.Step(3);
    r.in.grip = 1.0f;
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
    EXPECT_EQ(r.c.HeldSlot(), 2);
}

// A squeeze the gates hold up waits while the grip stays squeezed: the game had the arm for a
// moment (the end of a dodge), the hover was not fresh, the arc was still unfolding. It takes the
// card once the gate opens; let go first, or wait too long, and it is no grab.
TEST(ArcSelect, GatesNeverEatAGrab) {
    const Q::V3 eye{0.0f, 1.6f, 0.0f};
    const Q::Layout l = Q::PlaceArc(4, {-0.2f, 1.1f, -0.3f}, eye, {});
    {
        // The arm comes back 0.2 s after the squeeze: the grab.
        Rig r;
        r.Open();
        r.Step(80);
        Publish(r, l, l.card[1], eye);
        r.in.arm_weight = 0.4f;
        r.Step(2);
        r.in.grip = 1.0f;
        const Q::Output& o = r.Step();
        EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
        EXPECT_EQ(o.haptic_count, 0); // no "empty" buzz while it waits
        EXPECT_NE(o.log.find("grip waits (the game has the arm)"), std::string::npos) << o.log;
        r.Step(17);
        r.in.arm_weight = 1.0f;
        r.Step();
        EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
        EXPECT_EQ(r.c.HeldSlot(), 1);
    }
    {
        // Let go while it waits: nothing.
        Rig r;
        r.Open();
        r.Step(80);
        Publish(r, l, l.card[1], eye);
        r.in.arm_weight = 0.4f;
        r.in.grip = 1.0f;
        r.Step(5);
        r.in.grip = 0.0f;
        r.Step();
        r.in.arm_weight = 1.0f;
        r.Step(3);
        EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
    }
    {
        // The game keeps the arm: no grab after grab_wait_s, the "empty" buzz then.
        Rig r;
        r.Open();
        r.Step(80);
        Publish(r, l, l.card[1], eye);
        r.in.arm_weight = 0.4f;
        r.in.grip = 1.0f;
        r.Step();
        int empties = 0;
        for (int i = 0; i < 40; ++i) {
            const Q::Output& o = r.Step();
            for (int h = 0; h < o.haptic_count; ++h) {
                empties += o.haptics[static_cast<size_t>(h)] == Q::Haptic::Empty ? 1 : 0;
            }
        }
        EXPECT_EQ(empties, 1);
        r.in.arm_weight = 1.0f;
        r.Step(3);
        EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open); // the squeeze was spent
    }
    {
        // No fresh hover for a moment (the render tick stalled): taken when it is back.
        Rig r;
        r.Open();
        r.Step(80);
        Publish(r, l, l.card[3], eye);
        r.in.hover_fresh = false;
        r.in.grip = 1.0f;
        r.Step(6);
        EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
        r.in.hover_fresh = true;
        r.Step();
        EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
        EXPECT_EQ(r.c.HeldSlot(), 3);
    }
}

// Y and the grip pressed together: the grip a few frames before Y is a quick draw, not a grip
// held through the open (which must be let go first).
TEST(ArcSelect, GripWithYIsAQuickDraw) {
    Rig r;
    r.in.grip = 1.0f;
    r.Step(4); // 44 ms before Y
    r.PressY();
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
    EXPECT_EQ(r.c.HeldSlot(), 0);
    EXPECT_NE(r.out.log.find("via=quick draw"), std::string::npos) << r.out.log;
    // Held for a while before: no grab until it is squeezed again.
    Rig s;
    s.in.grip = 1.0f;
    s.Step(30);
    s.PressY();
    s.Step(3);
    EXPECT_EQ(s.c.GetPhase(), Q::Phase::Open);
}

// The detents: the first card lit after an open a little stronger than the next ones.
TEST(ArcSelect, Detents) {
    Rig r;
    r.Open();
    r.Step(10);
    r.Hover(1);
    const Q::Output& first = r.Step();
    ASSERT_EQ(first.haptic_count, 1);
    EXPECT_EQ(first.haptics[0], Q::Haptic::FirstDetent);
    r.Hover(2);
    const Q::Output& next = r.Step();
    ASSERT_EQ(next.haptic_count, 1);
    EXPECT_EQ(next.haptics[0], Q::Haptic::Detent);
    r.Hover(-1);
    EXPECT_EQ(r.Step().haptic_count, 0); // leaving a card is silent
}

// The squeeze's log line for the next headset session: ms since the open, the hover, the nearest
// card and its metric, the hand's distance from the pivot.
TEST(ArcSelect, SqueezeLog) {
    const Q::V3 eye{0.0f, 1.6f, 0.0f};
    const Q::Layout l = Q::PlaceArc(4, {-0.2f, 1.1f, -0.3f}, eye, {});
    Rig r;
    r.Open();
    r.Step(90);
    Publish(r, l, l.card[2], eye);
    r.Step(2);
    r.in.grip = 1.0f;
    const Q::Output& o = r.Step();
    EXPECT_NE(o.log.find("GRAB k=2"), std::string::npos) << o.log;
    EXPECT_NE(o.log.find("via=hover +1044 ms | hover 2, nearest 2 m 0.00 (2nd 1."),
              std::string::npos)
        << o.log;
    EXPECT_NE(o.log.find("hand 18.0 cm from the pivot"), std::string::npos) << o.log;
}

// The user's own arcs (shad_log.txt, 2026-10-03 16:46 session, VR_QUICK_LOG=1 + VR_BODY_LOG=1):
// the arc as it was placed then (radius 0.16, 130 degrees), and the controller's grip point (the
// body log's left "trk", where the hunter's L_Weapon was: IK error 0.0 mm) at the two squeezes
// that found no card. Hovered from the grip point, both are the card the user then took.
TEST(ArcSelect, UserMissesReplay) {
    Q::LayoutConfig then{};
    then.radius_m = 0.16f;
    then.card_m = 0.055f;
    then.span_deg = 130.0f;
    struct Miss {
        Q::V3 palm, eye;
        std::array<Q::V3, 3> grip; // the body log's samples (~0.5 s apart), the squeeze's last
        int samples;
        int took;
    };
    const Miss misses[] = {
        // arc 1: "grip on no card" at open + 3.0..4.1 s, then GRAB k=4 (Numbing Mist) at +6 s
        {{-0.34f, -0.37f, -0.11f},
         {-0.09f, -0.01f, 0.16f},
         {{{-0.324f, -0.300f, -0.218f}, {-0.318f, -0.298f, -0.221f}, {}}},
         2,
         4},
        // arc 2: "grip on no card" at open + 2.2..3.2 s, then GRAB k=5 (Fire Paper) at +16 s; the
        // hand had been on card 5 since open + 1.2 s (5.2, 6.2, 6.5 cm: entered, then kept)
        {{-0.27f, -0.39f, -0.18f},
         {-0.06f, -0.02f, 0.14f},
         {{{-0.175f, -0.377f, -0.261f}, {-0.183f, -0.380f, -0.251f}, {-0.182f, -0.382f, -0.249f}}},
         3,
         5},
    };
    for (const Miss& m : misses) {
        const Q::Layout l = Q::PlaceArc(6, m.palm, m.eye, then);
        int h = -1;
        for (int i = 0; i < m.samples; ++i) {
            h = Q::PickHover(l, m.grip[static_cast<size_t>(i)], m.eye, h, 1.0f);
        }
        EXPECT_EQ(h, m.took);
    }
}

// The user's three arcs in full: every grip sample (~2 Hz, linear in between at 90 Hz) from the
// open to the grab, the squeezes, and the card taken.
struct UserSample {
    float t; // s after the open
    Q::V3 grip;
};
struct UserArc {
    Q::V3 palm, eye; // logged at the open (cm rounding): the old arc's pivot was the palm
    int took;
    std::array<float, 2> squeeze; // s after the open (between the two body-log lines round it)
    std::array<int, 2> logged;    // what the old build did: the card, -1 "grip on no card"
    int squeezes;
    std::array<UserSample, 32> sample;
    int samples;
};
const UserArc kUserArcs[] = {
    {{-0.34f, -0.37f, -0.11f}, {-0.09f, -0.01f, 0.16f}, 4, {3.55f, 6.10f}, {-1, 4}, 2,
     {{
         {0.25f, {-0.366f, -0.367f, -0.129f}}, {0.76f, {-0.375f, -0.268f, -0.158f}},
         {1.27f, {-0.333f, -0.313f, -0.190f}}, {1.77f, {-0.335f, -0.320f, -0.214f}},
         {2.29f, {-0.337f, -0.304f, -0.215f}}, {2.80f, {-0.324f, -0.300f, -0.218f}},
         {3.29f, {-0.318f, -0.298f, -0.221f}}, {3.81f, {-0.327f, -0.298f, -0.226f}},
         {4.31f, {-0.327f, -0.297f, -0.223f}}, {4.82f, {-0.368f, -0.272f, -0.201f}},
         {5.33f, {-0.450f, -0.314f, -0.139f}}, {5.85f, {-0.330f, -0.284f, -0.224f}},
         {6.36f, {-0.328f, -0.293f, -0.224f}},
     }},
     13},
    {{-0.27f, -0.39f, -0.18f}, {-0.06f, -0.02f, 0.14f}, 5, {2.68f, 15.89f}, {-1, 5}, 2,
     {{
         {0.25f, {-0.296f, -0.409f, -0.199f}},  {0.78f, {-0.283f, -0.422f, -0.186f}},
         {1.42f, {-0.175f, -0.377f, -0.261f}},  {1.91f, {-0.183f, -0.380f, -0.251f}},
         {2.42f, {-0.182f, -0.382f, -0.249f}},  {2.93f, {-0.173f, -0.366f, -0.265f}},
         {3.44f, {-0.165f, -0.362f, -0.259f}},  {3.96f, {-0.128f, -0.331f, -0.247f}},
         {4.47f, {-0.162f, -0.349f, -0.265f}},  {4.98f, {-0.050f, -0.292f, -0.185f}},
         {5.48f, {-0.131f, -0.356f, -0.220f}},  {5.97f, {-0.177f, -0.381f, -0.249f}},
         {6.47f, {-0.178f, -0.372f, -0.251f}},  {6.98f, {-0.117f, -0.380f, -0.221f}},
         {7.50f, {-0.295f, -0.301f, -0.260f}},  {8.01f, {-0.390f, -0.344f, -0.209f}},
         {8.51f, {-0.213f, -0.335f, -0.253f}},  {9.02f, {-0.175f, -0.393f, -0.250f}},
         {9.53f, {-0.237f, -0.315f, -0.279f}},  {10.04f, {-0.314f, -0.309f, -0.266f}},
         {10.56f, {-0.364f, -0.305f, -0.240f}}, {11.07f, {-0.276f, -0.307f, -0.264f}},
         {11.58f, {-0.199f, -0.351f, -0.268f}}, {12.09f, {-0.182f, -0.370f, -0.266f}},
         {12.60f, {-0.158f, -0.474f, -0.251f}}, {13.11f, {-0.180f, -0.363f, -0.289f}},
         {13.62f, {-0.175f, -0.378f, -0.284f}}, {14.12f, {-0.137f, -0.546f, -0.197f}},
         {14.63f, {-0.175f, -0.381f, -0.271f}}, {15.13f, {-0.174f, -0.332f, -0.271f}},
         {15.64f, {-0.170f, -0.348f, -0.269f}}, {16.15f, {-0.176f, -0.349f, -0.270f}},
     }},
     32},
    {{-0.31f, -0.45f, -0.08f}, {-0.12f, -0.03f, 0.14f}, 0, {3.10f, 0.0f}, {0, 0}, 1,
     {{
         {0.25f, {-0.342f, -0.445f, -0.098f}}, {0.79f, {-0.341f, -0.456f, -0.099f}},
         {1.31f, {-0.458f, -0.412f, -0.065f}}, {1.82f, {-0.481f, -0.437f, -0.050f}},
         {2.33f, {-0.476f, -0.431f, -0.052f}}, {2.84f, {-0.481f, -0.429f, -0.050f}},
         {3.36f, {-0.469f, -0.409f, -0.056f}},
     }},
     7},
};

// The grip at `t` s after the open: the first sample's from the open on (the hand was there),
// linear between the samples.
Q::V3 UserGrip(const UserArc& a, float t) {
    if (t <= a.sample[0].t) {
        return a.sample[0].grip;
    }
    for (int i = 1; i < a.samples; ++i) {
        const UserSample& p = a.sample[static_cast<size_t>(i - 1)];
        const UserSample& q = a.sample[static_cast<size_t>(i)];
        if (t <= q.t) {
            return p.grip + (q.grip - p.grip) * ((t - p.t) / (q.t - p.t));
        }
    }
    return a.sample[static_cast<size_t>(a.samples - 1)].grip;
}

// The selection before this change, as it ran in that session: the palm dummy (the user's own
// offset from the grip, measured at the open: 3.2-3.7 cm toward the right and the eye), a
// 4.5 cm / 8 cm volume, late hover 60 ms, nothing else. The slot a squeeze at `t` took.
int OldSqueeze(const UserArc& a, float t_squeeze) {
    Q::LayoutConfig then{};
    then.radius_m = 0.16f;
    then.card_m = 0.055f;
    then.span_deg = 130.0f;
    const Q::Layout l = Q::PlaceArc(6, a.palm, a.eye, then);
    const Q::V3 offset = a.palm - a.sample[0].grip;
    const auto metric = [&a](Q::V3 p, Q::V3 c) {
        const Q::V3 n = Q::Normalize(a.eye - c);
        const Q::V3 d = p - c;
        const float along = Q::Dot(d, n);
        return std::hypot(Q::Length(d - n * along) / 0.045f, along / 0.08f);
    };
    int current = -1;
    int last = -1;
    float lost = -1e9f;
    for (float t = 0.0f; t < t_squeeze; t += static_cast<float>(Dt)) {
        const Q::V3 palm = UserGrip(a, t) + offset;
        int best = -1;
        float best_m = 1e9f;
        float current_m = 1e9f;
        for (int i = 0; i < l.n; ++i) {
            const float m = metric(palm, l.card[static_cast<size_t>(i)]);
            current_m = i == current ? m : current_m;
            if (m < best_m) {
                best_m = m;
                best = i;
            }
        }
        int h = best_m <= 1.0f ? best : -1;
        if (current >= 0 && current_m <= 1.35f) {
            h = best != current && best_m + 0.25f < current_m && best_m <= 1.0f ? best : current;
        }
        h = t >= 0.08f ? h : -1;
        if (h != current) {
            if (current >= 0) {
                last = current;
                lost = t;
            }
            current = h;
        }
    }
    return current >= 0 ? current : (last >= 0 && t_squeeze - lost <= 0.06f ? last : -1);
}

// The hand next to the nearest card of arc `from` put the same way next to that card of arc `to`
// (out from the pivot, along the arc, toward the eye): the user reaching for the card they see.
Q::V3 CardRelative(Q::V3 p, const Q::Layout& from, const Q::Layout& to) {
    int j = 0;
    for (int i = 1; i < from.n; ++i) {
        if (Q::Length(p - from.card[static_cast<size_t>(i)]) <
            Q::Length(p - from.card[static_cast<size_t>(j)])) {
            j = i;
        }
    }
    const auto frame = [j](const Q::Layout& l, Q::V3& u, Q::V3& v) {
        u = Q::Normalize(l.card[static_cast<size_t>(j)] - l.pivot);
        v = Q::Cross(l.toward, u);
    };
    Q::V3 u, v, u2, v2;
    frame(from, u, v);
    frame(to, u2, v2);
    const Q::V3 e = p - from.card[static_cast<size_t>(j)];
    return to.card[static_cast<size_t>(j)] + u2 * Q::Dot(e, u) + v2 * Q::Dot(e, v) +
           to.toward * Q::Dot(e, from.toward);
}

// The same squeeze through this build's controller (the render tick's hover from the grip point
// published every frame): `new_arc` = the arc as placed now (radius 0.18, 150 degrees, round
// the grip) with the path card-relative, else the arc as it was placed then.
int NewSqueeze(const UserArc& a, float t_squeeze, bool new_arc, std::string* via = nullptr) {
    Q::LayoutConfig then{};
    then.radius_m = 0.16f;
    then.card_m = 0.055f;
    then.span_deg = 130.0f;
    const Q::Layout old = Q::PlaceArc(6, a.palm, a.eye, then);
    const Q::Layout now = Q::PlaceArc(6, a.sample[0].grip, a.eye, {});
    const Q::Layout& l = new_arc ? now : old;
    Rig r;
    for (int k = 0; k < Q::SlotCount; ++k) {
        r.in.snapshot.slot[static_cast<size_t>(k)] = {80 + k, 1200 + 10 * k, 3, 24, -1, true};
    }
    r.in.hand_p = new_arc ? CardRelative(a.sample[0].grip, old, now) : a.sample[0].grip;
    r.PressY();
    const double open_t = r.t;
    int current = -1;
    while (true) {
        const float t = static_cast<float>(r.t + Dt - open_t);
        const Q::V3 g = UserGrip(a, t);
        const Q::V3 hand = new_arc ? CardRelative(g, old, now) : g;
        Publish(r, l, hand, a.eye, current);
        if (t < 0.08f) {
            r.in.hover.slot = -1; // the render tick's 80 ms
        }
        current = r.in.hover.slot;
        if (t >= t_squeeze) {
            r.in.grip = 1.0f;
            const Q::Output& o = r.Step();
            if (via != nullptr) {
                const std::size_t at = o.log.find("via=");
                *via = at == std::string::npos ? "none" : o.log.substr(at + 4, 6);
            }
            return r.c.GetPhase() == Q::Phase::Holding ? r.c.HeldSlot() : -1;
        }
        r.Step();
    }
}

TEST(ArcSelect, UserPathsReplay) {
    // The real squeezes: the old selection reproduces the session (3 of 5 took the card, the two
    // "grip on no card"); this build takes the card at every one, on either arc.
    int old_real = 0;
    int new_real = 0;
    int same_real = 0;
    int real = 0;
    for (const UserArc& a : kUserArcs) {
        for (int i = 0; i < a.squeezes; ++i) {
            const float t = a.squeeze[static_cast<size_t>(i)];
            const int before = OldSqueeze(a, t);
            if (a.logged[static_cast<size_t>(i)] < 0) {
                EXPECT_NE(before, a.took) << t; // the miss, as logged
            } else {
                EXPECT_EQ(before, a.logged[static_cast<size_t>(i)]) << t;
            }
            old_real += before == a.took ? 1 : 0;
            same_real += NewSqueeze(a, t, false) == a.took ? 1 : 0;
            new_real += NewSqueeze(a, t, true) == a.took ? 1 : 0;
            ++real;
        }
    }
    EXPECT_EQ(real, 5);
    EXPECT_EQ(old_real, 3);
    EXPECT_EQ(same_real, 5);
    EXPECT_EQ(new_real, 5);
    // A squeeze at every sample with the grip within 7 cm of the card the user took (from the
    // open to the grab): how often it takes that card.
    int samples = 0;
    std::array<int, 3> ok{};
    std::array<int, 3> none{};
    for (const UserArc& a : kUserArcs) {
        Q::LayoutConfig then{};
        then.radius_m = 0.16f;
        then.span_deg = 130.0f;
        const Q::Layout old = Q::PlaceArc(6, a.palm, a.eye, then);
        const float grab = a.squeeze[static_cast<size_t>(a.squeezes - 1)];
        for (int i = 0; i < a.samples; ++i) {
            const UserSample& s = a.sample[static_cast<size_t>(i)];
            if (s.t <= 0.2f || s.t > grab + 0.3f ||
                Q::Length(s.grip - old.card[static_cast<size_t>(a.took)]) > 0.07f) {
                continue;
            }
            ++samples;
            const std::array<int, 3> took{OldSqueeze(a, s.t), NewSqueeze(a, s.t, false),
                                          NewSqueeze(a, s.t, true)};
            for (std::size_t v = 0; v < 3; ++v) {
                ok[v] += took[v] == a.took ? 1 : 0;
                none[v] += took[v] < 0 ? 1 : 0;
            }
        }
    }
    std::printf("[ ArcSelect.UserPathsReplay ] real squeezes: before %d/5, after (the arc as "
                "then) %d/5, after (the new arc) %d/5 | a squeeze at each of %d grip samples "
                "within 7 cm of the card: before %d (%d no card), after (the arc as then) %d (%d), "
                "after (the new arc) %d (%d)\n",
                old_real, same_real, new_real, samples, ok[0], none[0], ok[1], none[1], ok[2],
                none[2]);
    EXPECT_EQ(samples, 32);
    EXPECT_LE(ok[0], 16);
    EXPECT_GE(ok[1], 30);
    EXPECT_GE(ok[2], 31);
    EXPECT_EQ(none[1], 0);
    EXPECT_EQ(none[2], 0);
}

// The pie option: the hand's direction from where the arc opened picks the card, from 7 cm out.
TEST(ArcSelect, Sector) {
    const Q::V3 eye{0.0f, 1.6f, 0.0f};
    for (int n = 1; n <= Q::SlotCount; ++n) {
        const Q::Layout l = Q::PlaceArc(n, {-0.2f, 1.1f, -0.3f}, eye, {});
        for (int k = 0; k < n; ++k) {
            const Q::V3 dir = Q::Normalize(l.card[static_cast<size_t>(k)] - l.pivot);
            EXPECT_EQ(Q::PickSector(l, l.pivot + dir * 0.05f, -1), -1) << n << k; // dead zone
            EXPECT_EQ(Q::PickSector(l, l.pivot + dir * 0.09f, -1), k) << n << k;  // a flick
            EXPECT_EQ(Q::PickSector(l, l.card[static_cast<size_t>(k)], -1), k) << n << k;
            EXPECT_EQ(Q::PickSector(l, l.pivot + dir * 0.27f, -1), k) << n << k;  // overshoot
        }
        if (n < 2) {
            continue;
        }
        // Hysteresis: 0.5 cm into the neighbour's sector keeps the card, 2 cm takes the neighbour.
        const float r = 0.16f;
        const auto at = [&l, r](float a) {
            return l.pivot + l.right * (std::sin(a) * r) + l.up * (std::cos(a) * r);
        };
        const float b = -0.5f * static_cast<float>(n - 1) * l.step_rad + 0.5f * l.step_rad;
        EXPECT_EQ(Q::PickSector(l, at(b + 0.005f / r), 0), 0) << n;
        EXPECT_EQ(Q::PickSector(l, at(b + 0.02f / r), 0), 1) << n;
        // Below the end card: still it, down to 30 degrees past its sector; then nothing.
        const float end = 0.5f * static_cast<float>(n - 1) * l.step_rad;
        EXPECT_EQ(Q::PickSector(l, at(end + 0.5f * l.step_rad + 0.4f), -1), n - 1) << n;
        EXPECT_EQ(Q::PickSector(l, at(end + 0.5f * l.step_rad + 0.6f), -1), -1) << n;
    }
}

} // namespace

TEST(QuickItems, ExplicitCancellationKeepsControlsIdle) {
    const std::string reason(8000, 'x'); // bounded diagnostics must not alter cancellation
    for (int state = 0; state < 4; ++state) {
        Rig r;
        if (state == 1)
            r.Open();
        if (state >= 2)
            r.Grab(1);
        if (state == 3) {
            r.Step(12);
            r.in.trigger = 1;
            r.Step(12);
            ASSERT_EQ(r.c.GetPhase(), Q::Phase::Waiting);
        }
        r.out = {};
        r.c.Cancel(r.out, reason.c_str());
        EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
        EXPECT_FALSE(r.out.send);
        r.c.Cancel(r.out, "again");
        EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    }
}
