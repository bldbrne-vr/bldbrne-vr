// SPDX-License-Identifier: GPL-2.0-or-later

// The left-hand gun's math and decisions (core/vr/gun_aim.h): the guns by behaviour, the
// direct-fire table, the barrel in the grip (against the body's own route through DefaultGrip), the
// emitter look-at the game turns into the flight direction (spread fans included), the start moved
// onto the barrel line, the very gentle lock-on assist, the recoil kick and a pull's fate.

#include <array>
#include <cmath>
#include <random>

#include <gtest/gtest.h>

#include "c0000_test_skeleton.h"

#include "core/vr/gun_aim.h"

namespace {

namespace G = VR::Gun;
namespace B = VR::Body;
using B::Quat;
using B::Vec3;

constexpr float Deg = B::Deg;

float AngleDeg(Vec3 a, Vec3 b) {
    return B::Angle(a, b) / Deg;
}

// The game's composition of an emitter (0x19F2440's tail): out = M * Ry(pi + yaw) * Rx(pitch), the
// velocity = out's column 2.
Vec3 GameFlight(const std::array<float, 12>& rows, float yaw_spread, float pitch_spread) {
    const auto col = [&rows](int c) { return Vec3{rows[c], rows[4 + c], rows[8 + c]}; };
    const Vec3 x = col(0), y = col(1), z = col(2);
    // Ry(a) column 2 = (sin a, 0, cos a); then Rx(p) applied first: R = Ry * Rx, column 2 of Rx
    // = (0, -sin p, cos p).
    const float a = B::Pi + yaw_spread;
    const Vec3 rx2{0.0f, -std::sin(pitch_spread), std::cos(pitch_spread)};
    const Vec3 r2{std::cos(a) * rx2.x + std::sin(a) * rx2.z, rx2.y,
                  -std::sin(a) * rx2.x + std::cos(a) * rx2.z};
    return B::Normalize(x * r2.x + y * r2.y + z * r2.z);
}

Quat RandomQuat(std::mt19937& rng) {
    std::normal_distribution<float> n(0.0f, 1.0f);
    return B::Normalize(Quat{n(rng), n(rng), n(rng), n(rng)});
}

TEST(GunAim, GunsByBehaviour) {
    EXPECT_EQ(G::GunOfBehavior(101400800).family, G::Family::Pistol);
    EXPECT_EQ(G::GunOfBehavior(101400810).family, G::Family::Pistol); // Bone Marrow Ash
    EXPECT_EQ(G::GunOfBehavior(101400800).attach, 347);
    EXPECT_EQ(G::GunOfBehavior(101410800).family, G::Family::Repeating);
    EXPECT_EQ(G::GunOfBehavior(101410800).cost, 2);
    EXPECT_EQ(G::GunOfBehavior(100600800).family, G::Family::Blunderbuss);
    EXPECT_EQ(G::GunOfBehavior(100600800).attach, 345);
    EXPECT_EQ(G::GunOfBehavior(100610800).family, G::Family::Rifle);
    EXPECT_EQ(G::GunOfBehavior(103600800).family, G::Family::Piercing);
    EXPECT_EQ(G::GunOfBehavior(101500800).family, G::Family::Cannon);
    EXPECT_EQ(G::GunOfBehavior(101500800).cost, 12);
    EXPECT_EQ(G::GunOfBehavior(103300800).family, G::Family::Gatling);
    EXPECT_TRUE(G::GunOfBehavior(103300800).sustained);
    EXPECT_EQ(G::GunOfBehavior(103500800).family, G::Family::ChurchCannon);
    EXPECT_TRUE(G::GunOfBehavior(103500800).gravity);
    EXPECT_EQ(G::GunOfBehavior(101800800).family, G::Family::Sprayer);
    EXPECT_EQ(G::GunOfBehavior(101810800).family, G::Family::Sprayer);
    // Not guns: a melee behaviour, a negative id, a quick item's.
    EXPECT_EQ(G::GunOfBehavior(100700000).family, G::Family::None);
    EXPECT_EQ(G::GunOfBehavior(-1).family, G::Family::None);
    EXPECT_EQ(G::GunOfBehavior(1400800).family, G::Family::None);
    EXPECT_EQ(G::VariationOf(101400800), 1400);
}

TEST(GunAim, DirectFireTable) {
    const G::DirectGun* pistol = G::DirectGunOf(G::Family::Pistol);
    ASSERT_NE(pistol, nullptr);
    EXPECT_NEAR(pistol->cadence_s, 0.867f, 1e-4f);
    EXPECT_EQ(pistol->flash_ffx, 6102);
    EXPECT_EQ(pistol->flash_ffx_ash, 6103);
    EXPECT_EQ(pistol->flash_dummy, 20215);
    EXPECT_EQ(pistol->bang, 720);
    EXPECT_EQ(pistol->dry, 721);
    EXPECT_EQ(pistol->shot_dummy, 3);
    const G::DirectGun* blunderbuss = G::DirectGunOf(G::Family::Blunderbuss);
    ASSERT_NE(blunderbuss, nullptr);
    EXPECT_NEAR(blunderbuss->cadence_s, 1.167f, 1e-4f);
    EXPECT_EQ(blunderbuss->bang, 700);
    ASSERT_NE(G::DirectGunOf(G::Family::Rifle), nullptr);
    ASSERT_NE(G::DirectGunOf(G::Family::Repeating), nullptr);
    // The rest stay on the game's own L2.
    for (const G::Family f : {G::Family::None, G::Family::Piercing, G::Family::Cannon,
                              G::Family::ChurchCannon, G::Family::Gatling, G::Family::Sprayer}) {
        EXPECT_EQ(G::DirectGunOf(f), nullptr) << G::FamilyName(f);
    }
}

// On the tests' mannequin (tests/c0000_test_skeleton.h) the gun dummies' forward is tipped up
// from the weapon bone's +Y by 25 / 45 / 60 degrees in its XY plane: with the default grip the
// barrel points that far below grip -Z, straight down the grip's YZ plane. On the hunter (the
// game-data tier: the player model) the reference barrel directions: 24.6 / 43.8 / 61.6 degrees.
TEST(GunAim, BarrelInGripFollowsTheAttachDummy) {
    struct Case {
        int attach;
        Vec3 expected;
        float below_deg;
    };
    const auto tipped = [](int attach, float deg) {
        const float r = deg * B::Deg;
        return Case{attach, {0.0f, -std::sin(r), -std::cos(r)}, deg};
    };
    const bool hunter = B::C0000::Test::OnGameData();
    const std::array<Case, 3> cases =
        hunter ? std::array<Case, 3>{Case{347, {-0.0398f, -0.6906f, -0.7221f}, 43.8f},
                                     Case{345, {-0.0074f, -0.4169f, -0.9089f}, 24.6f},
                                     Case{348, {-0.1013f, -0.8740f, -0.4753f}, 61.6f}}
               : std::array<Case, 3>{tipped(347, 45.0f), tipped(345, 25.0f), tipped(348, 60.0f)};
    for (const Case& c : cases) {
        const Vec3 b = G::BarrelInGrip(c.attach, 0.0f);
        EXPECT_NEAR(b.x, c.expected.x, 2e-4f) << c.attach;
        EXPECT_NEAR(b.y, c.expected.y, 2e-4f) << c.attach;
        EXPECT_NEAR(b.z, c.expected.z, 2e-4f) << c.attach;
        EXPECT_NEAR(AngleDeg(b, {0.0f, 0.0f, -1.0f}), c.below_deg, 0.06f) << c.attach;
    }
    // No frame for the Church Cannon's attach: no barrel (its shot stays the game's).
    EXPECT_EQ(B::LengthSq(G::BarrelInGrip(350, 0.0f)), 0.0f);
}

// The ray from BarrelInGrip equals the world direction of the barrel on the L_Weapon bone the
// body's left IK target puts there: TrackingToWorldRotation(grip) * DefaultGrip(Left, ...).
TEST(GunAim, BarrelRayEqualsTheBodysWeaponBone) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> trim(-20.0f, 20.0f);
    std::uniform_real_distribution<float> yaw(-B::Pi, B::Pi);
    std::uniform_real_distribution<float> pitch(-10.0f, 10.0f);
    float worst = 0.0f;
    for (int i = 0; i < 200; ++i) {
        const Quat q = RandomQuat(rng);
        const float base_yaw = yaw(rng);
        const float t_yaw = trim(rng), t_pitch = trim(rng), t_roll = trim(rng);
        const float blade = pitch(rng);
        for (const int attach : {345, 347, 348}) {
            const Quat bone = B::TrackingToWorldRotation(q, base_yaw) *
                              B::DefaultGrip(B::C0000::Left, blade, t_yaw, t_pitch, t_roll);
            const Vec3 body = B::Rotate(B::Normalize(bone), G::BarrelInWeaponBone(attach));
            const Vec3 ours = G::BarrelRayWorld(
                q, G::BarrelInGrip(attach, blade, t_yaw, t_pitch, t_roll), base_yaw);
            worst = std::max(worst, AngleDeg(body, ours));
        }
    }
    EXPECT_LT(worst, 1e-2f);
}

TEST(GunAim, LookRowsBecomeTheFlightDirection) {
    std::mt19937 rng(11);
    std::normal_distribution<float> n(0.0f, 1.0f);
    for (int i = 0; i < 500; ++i) {
        const Vec3 d = B::Normalize(Vec3{n(rng), 0.6f * n(rng), n(rng)});
        std::array<float, 12> rows{};
        rows[3] = 1.0f, rows[7] = 2.0f, rows[11] = 3.0f;
        ASSERT_TRUE(G::WriteLookRows(rows.data(), d, {0.0f, 0.0f, 1.0f}));
        // The translation is untouched.
        EXPECT_EQ(rows[3], 1.0f);
        EXPECT_EQ(rows[7], 2.0f);
        EXPECT_EQ(rows[11], 3.0f);
        const Vec3 x{rows[0], rows[4], rows[8]}, y{rows[1], rows[5], rows[9]},
            z{rows[2], rows[6], rows[10]};
        EXPECT_NEAR(B::Length(x), 1.0f, 1e-5f);
        EXPECT_NEAR(B::Length(y), 1.0f, 1e-5f);
        EXPECT_NEAR(B::Dot(x, y), 0.0f, 1e-5f);
        EXPECT_NEAR(B::Dot(y, z), 0.0f, 1e-5f);
        EXPECT_NEAR(B::Dot(B::Cross(x, y), z), 1.0f, 1e-5f); // det +1
        EXPECT_LT(AngleDeg(-z, d), 1e-3f);
        EXPECT_LT(AngleDeg(G::FlightOfRows(rows.data()), d), 1e-3f);
        EXPECT_LT(AngleDeg(GameFlight(rows, 0.0f, 0.0f), d), 1e-3f);
        EXPECT_GE(y.y, -1e-6f); // up is world up made perpendicular
    }
    // Straight up and down: the fallback up.
    for (const Vec3 d :
         {Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, Vec3{0.05f, 0.9987f, 0.0f}}) {
        std::array<float, 12> rows{};
        ASSERT_TRUE(G::WriteLookRows(rows.data(), d, {0.0f, 0.0f, 1.0f}));
        EXPECT_LT(AngleDeg(GameFlight(rows, 0.0f, 0.0f), B::Normalize(d)), 1e-3f);
    }
    // A zero or non-finite ray writes nothing.
    std::array<float, 12> rows{};
    EXPECT_FALSE(G::WriteLookRows(rows.data(), {}, {0.0f, 0.0f, 1.0f}));
    EXPECT_FALSE(G::WriteLookRows(rows.data(), {NAN, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}));
}

// The Blunderbuss's five pellets (-12 + 6 i degrees in the emitter frame) fan around the ray.
TEST(GunAim, SpreadFansAroundTheBarrel) {
    const Vec3 d = B::Normalize(Vec3{0.5f, 0.2f, -0.8f});
    std::array<float, 12> rows{};
    ASSERT_TRUE(G::WriteLookRows(rows.data(), d, {0.0f, 0.0f, 1.0f}));
    for (int i = 0; i < 5; ++i) {
        const float spread = (-12.0f + 6.0f * static_cast<float>(i)) * Deg;
        EXPECT_NEAR(AngleDeg(GameFlight(rows, spread, 0.0f), d), std::abs(spread / Deg), 1e-3f);
    }
    // A vertical child spread of 3 degrees, too.
    EXPECT_NEAR(AngleDeg(GameFlight(rows, 0.0f, 3.0f * Deg), d), 3.0f, 1e-3f);
}

TEST(GunAim, StartMovesSidewaysOntoTheBarrel) {
    const Vec3 d = B::Normalize(Vec3{0.3f, 0.1f, -1.0f});
    const Vec3 muzzle{0.2f, 1.4f, -0.6f};
    Vec3 p{0.17f, 1.36f, -0.25f}; // the palm, about 5 cm off the barrel's axis
    Vec3 shift{};
    const Vec3 before = p;
    ASSERT_TRUE(G::OriginOnBarrel(p, d, muzzle, 0.08f, &shift));
    EXPECT_NEAR(B::Dot(shift, d), 0.0f, 1e-6f);          // never along the ray
    EXPECT_LT(B::Length(B::Perp(p - muzzle, d)), 1e-5f); // on the barrel's line
    EXPECT_NEAR(B::Length(p - before), B::Length(shift), 1e-6f);
    // A muzzle 10 cm off the line is not this gun's: nothing moves.
    Vec3 q = before;
    EXPECT_FALSE(G::OriginOnBarrel(q, d, before + Vec3{0.1f, 0.0f, 0.0f}, 0.08f));
    EXPECT_EQ(q.x, before.x);
    EXPECT_FALSE(G::OriginOnBarrel(q, {}, muzzle, 0.08f));
}

TEST(GunAim, GentleAssistTable) {
    const G::AssistCfg cfg{};
    const Vec3 p{0.0f, 1.3f, 0.0f};
    struct Case {
        float distance, off_deg, moved_deg;
    };
    // gun_assist 0.27, the user's 2026-10-07 value (0.30 on 10-06: 1 deg -> 0.30, 4 -> 1.20,
    // 5 -> 1.27, 6 -> 0.90; 0.25 before 2026-10-06: 0.25, 1.00, 1.05, 0.75).
    EXPECT_NEAR(cfg.strength, 0.27f, 1e-6f);
    for (const Case& c : {Case{3.0f, 1.0f, 0.27f}, Case{3.0f, 4.0f, 1.08f}, Case{3.0f, 5.0f, 1.14f},
                          Case{3.0f, 6.0f, 0.81f}, Case{3.0f, 8.0f, 0.0f}, Case{3.0f, 10.0f, 0.0f},
                          Case{10.0f, 5.0f, 1.14f}, Case{10.0f, 4.0f, 1.08f}}) {
        const Vec3 t{0.0f, 1.3f, -c.distance};
        const Vec3 d{std::sin(c.off_deg * Deg), 0.0f, -std::cos(c.off_deg * Deg)};
        const G::AssistOut a = G::GentleAssist(d, p, t, true, true, false, cfg);
        EXPECT_NEAR(a.moved_deg, c.moved_deg, 0.02f) << c.distance << " m " << c.off_deg;
        EXPECT_NEAR(AngleDeg(a.dir, d), c.moved_deg, 0.02f);
        EXPECT_NEAR(a.angle_deg, c.off_deg, 0.01f);
        // Toward the target, never past it.
        EXPECT_LE(AngleDeg(a.dir, B::Normalize(t - p)), c.off_deg + 1e-3f);
    }
    // Never more than about 1.2 degrees anywhere in the cone (1.17 at 4.6 degrees off).
    float most = 0.0f;
    for (float off = 0.1f; off < 9.0f; off += 0.1f) {
        const Vec3 d{std::sin(off * Deg), 0.0f, -std::cos(off * Deg)};
        most = std::max(
            most, G::GentleAssist(d, p, {0.0f, 1.3f, -5.0f}, true, true, false, cfg).moved_deg);
    }
    EXPECT_LT(most, 1.19f);
    EXPECT_GT(most, 1.14f);
    const Vec3 d4{std::sin(4.0f * Deg), 0.0f, -std::cos(4.0f * Deg)};
    const Vec3 t5{0.0f, 1.3f, -5.0f};
    // Not locked on: nothing; unlocked help only when asked for.
    EXPECT_EQ(G::GentleAssist(d4, p, t5, true, false, false, cfg).weight, 0.0f);
    G::AssistCfg unlocked = cfg;
    unlocked.unlocked = true;
    EXPECT_GT(G::GentleAssist(d4, p, t5, true, false, false, unlocked).weight, 0.0f);
    // No target, behind, too near, too far, not finite: unchanged.
    EXPECT_EQ(G::GentleAssist(d4, p, t5, false, true, false, cfg).weight, 0.0f);
    EXPECT_EQ(G::GentleAssist(d4, p, {0.0f, 1.3f, 5.0f}, true, true, false, cfg).weight, 0.0f);
    EXPECT_EQ(G::GentleAssist(d4, p, {0.0f, 1.3f, -0.3f}, true, true, false, cfg).weight, 0.0f);
    EXPECT_EQ(G::GentleAssist(d4, p, {0.0f, 1.3f, -30.0f}, true, true, false, cfg).weight, 0.0f);
    const G::AssistOut nan = G::GentleAssist(d4, p, {NAN, 1.3f, -5.0f}, true, true, false, cfg);
    EXPECT_EQ(nan.weight, 0.0f);
    EXPECT_LT(AngleDeg(nan.dir, d4), 1e-4f);
    // Gravity guns: the yaw turns, the pitch stays.
    const Vec3 up_left = B::Normalize(Vec3{std::sin(4.0f * Deg), 0.2f, -std::cos(4.0f * Deg)});
    const G::AssistOut g = G::GentleAssist(up_left, p, t5, true, true, true, cfg);
    EXPECT_GT(g.weight, 0.0f);
    EXPECT_NEAR(G::PitchOf(g.dir), G::PitchOf(up_left), 1e-4f);
    EXPECT_LT(std::abs(G::YawOf(g.dir) - G::YawOf(t5 - p)),
              std::abs(G::YawOf(up_left) - G::YawOf(t5 - p)));
    // Off: strength 0.
    G::AssistCfg off = cfg;
    off.strength = 0.0f;
    EXPECT_EQ(G::GentleAssist(d4, p, t5, true, true, false, off).weight, 0.0f);
}

TEST(GunAim, KickEnvelope) {
    const G::Kick k = G::KickOf(G::Family::Pistol);
    EXPECT_NEAR(G::KickEnvelope(k, k.attack_ms), 1.0f, 1e-5f);
    EXPECT_NEAR(G::KickEnvelope(k, 0.5f * k.attack_ms), 0.5f, 1e-5f);
    EXPECT_EQ(G::KickEnvelope(k, -1.0f), 0.0f);
    EXPECT_LT(G::KickEnvelope(k, k.attack_ms + 5.0f * k.tau_ms), 0.05f);
    float last = 2.0f;
    for (float t = k.attack_ms; t < 600.0f; t += 5.0f) {
        const float e = G::KickEnvelope(k, t);
        EXPECT_LE(e, last + 1e-6f); // never kicks twice
        EXPECT_GE(e, 0.0f);
        last = e;
    }
    // Heavier guns kick harder; the sprayers not at all.
    EXPECT_GT(G::KickOf(G::Family::Cannon).pitch_deg, G::KickOf(G::Family::Blunderbuss).pitch_deg);
    EXPECT_GT(G::KickOf(G::Family::Blunderbuss).pitch_deg, k.pitch_deg);
    EXPECT_EQ(G::KickOf(G::Family::Sprayer).pitch_deg, 0.0f);
}

// The kick turns the muzzle toward the gun's top and pulls the hand back along the barrel.
TEST(GunAim, KickRaisesTheMuzzle) {
    for (const int attach : {345, 347, 348}) {
        const G::Kick k = G::KickOf(G::Family::Pistol);
        B::ModelTarget t{{0.1f, 1.2f, -0.4f}, B::Normalize(Quat{0.1f, -0.3f, 0.2f, 0.9f})};
        const B::ModelTarget kicked = G::ApplyKick(t, attach, k, 1.0f);
        const Vec3 b0 = B::Rotate(t.q, G::BarrelInWeaponBone(attach));
        const Vec3 b1 = B::Rotate(kicked.q, G::BarrelInWeaponBone(attach));
        const Vec3 up0 = B::Rotate(t.q, G::UpInWeaponBone(attach));
        EXPECT_NEAR(AngleDeg(b0, b1), k.pitch_deg, 0.05f) << attach;
        EXPECT_GT(B::Dot(b1 - b0, up0), 0.0f) << attach; // toward the gun's top
        EXPECT_NEAR(B::Length(kicked.p - t.p), k.back_m, 1e-5f);
        EXPECT_LT(B::Dot(kicked.p - t.p, b1), 0.0f); // back, against the barrel
        // No kick: unchanged.
        const B::ModelTarget same = G::ApplyKick(t, attach, k, 0.0f);
        EXPECT_EQ(same.p.x, t.p.x);
        EXPECT_EQ(same.q.w, t.q.w);
    }
}

// The sword hand's kick at a blade's shot: the tip thrown up about the grip, the hand pushed back
// along the blade; the Rifle Spear's the strongest flip, the stake's the deepest shove.
TEST(GunAim, BladeKickThrowsTheTipUp) {
    const Vec3 down{0.0f, -1.0f, 0.0f}; // the blade in R_Weapon
    for (const int variation : {1010, 1000, 1100}) {
        const G::Kick k = G::BladeKickOf(variation);
        // Held forward and a little down and to the side.
        const Vec3 aim = B::Normalize(Vec3{0.2f, -0.2f, -1.0f});
        const B::ModelTarget t{{-0.25f, 1.2f, -0.4f}, B::Normalize(B::FromTo(down, aim))};
        const B::ModelTarget kicked = G::ApplyBladeKick(t, k, 1.0f);
        const Vec3 b0 = B::Rotate(t.q, down);
        const Vec3 b1 = B::Rotate(kicked.q, down);
        EXPECT_NEAR(AngleDeg(b0, b1), k.pitch_deg, 0.05f) << variation;
        EXPECT_GT(b1.y, b0.y) << variation;                          // the tip up ...
        EXPECT_NEAR(b1.x / b1.z, b0.x / b0.z, 1e-3f) << variation;  // ... not sideways
        EXPECT_NEAR(B::Length(kicked.p - t.p), k.back_m, 1e-5f);
        EXPECT_LT(B::Dot(kicked.p - t.p, b0), 0.0f); // back, against the blade
        EXPECT_EQ(G::ApplyBladeKick(t, k, 0.0f).p.x, t.p.x); // none: unchanged
    }
    // Near straight down (a downed enemy), the hand trembling across vertical: a small throw that
    // fades to none, never a flip from one side to the other.
    for (const float off_deg : {-0.6f, 0.6f, 3.0f}) {
        const Vec3 aim_down = B::Normalize(Vec3{std::sin(off_deg * B::Deg), -std::cos(off_deg * B::Deg),
                                                0.0f});
        const B::ModelTarget t{{0.0f, 1.0f, -0.3f}, B::Normalize(B::FromTo(down, aim_down))};
        const B::ModelTarget kicked = G::ApplyBladeKick(t, G::BladeKickOf(1000), 1.0f);
        EXPECT_LT(AngleDeg(B::Rotate(t.q, down), B::Rotate(kicked.q, down)), 3.0f) << off_deg;
    }
    // Raised steeply (80 degrees up): thrown up to vertical at most, never back over the head.
    {
        const Vec3 steep{0.0f, std::sin(80.0f * B::Deg), -std::cos(80.0f * B::Deg)};
        const B::ModelTarget t{{0.0f, 1.5f, -0.3f}, B::Normalize(B::FromTo(down, steep))};
        const Vec3 b1 = B::Rotate(G::ApplyBladeKick(t, G::BladeKickOf(1000), 1.0f).q, down);
        EXPECT_LE(b1.z, 1e-4f); // still in front
        EXPECT_GE(b1.y, steep.y);
    }
    // Held straight up: no tip to throw, only the shove (down the arm).
    const G::Kick k = G::BladeKickOf(1000);
    const B::ModelTarget up{{0.0f, 1.5f, -0.3f}, B::AxisAngle({1.0f, 0.0f, 0.0f}, B::Pi)};
    const B::ModelTarget kicked = G::ApplyBladeKick(up, k, 1.0f);
    EXPECT_NEAR(AngleDeg(B::Rotate(up.q, down), B::Rotate(kicked.q, down)), 0.0f, 0.05f);
    EXPECT_NEAR(kicked.p.y, up.p.y - k.back_m, 1e-4f);
    EXPECT_GT(G::BladeKickOf(1000).pitch_deg, G::BladeKickOf(1010).pitch_deg);
    EXPECT_GT(G::BladeKickOf(1100).back_m, G::BladeKickOf(1000).back_m);
    EXPECT_GE(G::BladeKickOf(1100).back_m, 0.15f); // the stake: a big visible jolt (2026-10-13)
    EXPECT_GT(G::KickOf(G::Family::ChurchCannon).pitch_deg, G::KickOf(G::Family::Blunderbuss).pitch_deg);
    EXPECT_EQ(G::BladeKickOf(2600).back_m, 0.0f); // the moonlight wave: no kick

    // Two-handed: the hand on the shaft rides the kick (its offset from the sword hand, in the
    // sword hand's frame, unchanged); one-handed (w 0) it stays.
    const Vec3 aim = B::Normalize(Vec3{0.0f, -0.1f, -1.0f});
    const B::ModelTarget right{{0.2f, 1.2f, -0.4f}, B::Normalize(B::FromTo(down, aim))};
    const B::ModelTarget left{{0.18f, 1.22f, -0.15f}, B::Normalize(Quat{0.2f, 0.1f, 0.0f, 0.97f})};
    const B::ModelTarget after = G::ApplyBladeKick(right, k, 1.0f);
    const B::ModelTarget carried = G::CarryKick(right, after, left, 1.0f);
    const Vec3 rel0 = B::Rotate(B::Conjugate(right.q), left.p - right.p);
    const Vec3 rel1 = B::Rotate(B::Conjugate(after.q), carried.p - after.p);
    EXPECT_NEAR(B::Length(rel1 - rel0), 0.0f, 1e-4f);
    EXPECT_GT(B::Length(carried.p - left.p), 0.02f); // it moved
    const B::ModelTarget free_hand = G::CarryKick(right, after, left, 0.0f);
    EXPECT_EQ(free_hand.p.z, left.p.z);
}

TEST(GunAim, PullFate) {
    G::PullInputs in{};
    EXPECT_EQ(G::DecidePull(in).action, G::PullAction::Fire);
    // Anything that ends a shot outright.
    in.hard = "menu";
    EXPECT_EQ(G::DecidePull(in).action, G::PullAction::Deny);
    in.hard = nullptr;
    // The cooldown: early pulls are dropped, the last 120 ms are buffered.
    in.cooldown_left_s = 0.3f;
    EXPECT_EQ(G::DecidePull(in).action, G::PullAction::Deny);
    in.cooldown_left_s = 0.1f;
    EXPECT_EQ(G::DecidePull(in).action, G::PullAction::Wait);
    in.first = false;
    in.age_ms = 60.0f;
    in.cooldown_left_s = 0.04f;
    EXPECT_EQ(G::DecidePull(in).action, G::PullAction::Wait);
    in.cooldown_left_s = 0.0f;
    EXPECT_EQ(G::DecidePull(in).action, G::PullAction::Fire);
    // The game has the arm (a dodge): wait for it, up to the buffer.
    in = {};
    in.busy = true;
    EXPECT_EQ(G::DecidePull(in).action, G::PullAction::Wait);
    in.first = false;
    in.age_ms = 390.0f;
    EXPECT_EQ(G::DecidePull(in).action, G::PullAction::Wait);
    in.age_ms = 410.0f;
    EXPECT_EQ(G::DecidePull(in).action, G::PullAction::Deny);
    in.busy_mode = G::BusyMode::Vanilla;
    EXPECT_EQ(G::DecidePull(in).action, G::PullAction::Vanilla);
    in.busy = false;
    in.age_ms = 200.0f;
    EXPECT_EQ(G::DecidePull(in).action, G::PullAction::Fire); // the arm came back in time
    // No stamina: wait, then deny (never the game's L2: it would refuse too).
    in = {};
    in.stamina = false;
    EXPECT_EQ(G::DecidePull(in).action, G::PullAction::Wait);
    in.age_ms = 500.0f;
    in.busy_mode = G::BusyMode::Vanilla;
    EXPECT_EQ(G::DecidePull(in).action, G::PullAction::Deny);
    // The vial stepping aside: a short wait for the gun.
    in = {};
    in.hidden = true;
    EXPECT_EQ(G::DecidePull(in).action, G::PullAction::Wait);
    in.age_ms = 250.0f;
    EXPECT_EQ(G::DecidePull(in).action, G::PullAction::Deny);
}

} // namespace
