// SPDX-License-Identifier: GPL-2.0-or-later

// Directional hit feedback (core/vr/hit_direction.h): the source of a blow (hit position, then
// attacker), its direction relative to the view, the flash's wedge on the ring and the
// controllers' share.

#include <cmath>

#include <gtest/gtest.h>

#include "core/vr/hit_direction.h"

namespace {

namespace H = VR::HitDir;
constexpr float Deg = H::Deg;

H::Sample At(H::V3 center) {
    H::Sample s;
    s.center_ok = true;
    s.center = center;
    return s;
}

// The game's convention: forward (sin yaw, 0, cos yaw), right (cos yaw, 0, -sin yaw).
TEST(HitDirection, WorldYawFollowsTheGamesHeading) {
    EXPECT_NEAR(H::WorldYaw(0.0f, 1.0f), 0.0f, 1e-6f);
    EXPECT_NEAR(H::WorldYaw(1.0f, 0.0f), 90.0f * Deg, 1e-5f);
    EXPECT_NEAR(H::WorldYaw(-1.0f, 0.0f), -90.0f * Deg, 1e-5f);
    EXPECT_NEAR(std::abs(H::WorldYaw(0.0f, -1.0f)), 180.0f * Deg, 1e-5f);
}

TEST(HitDirection, AzimuthIsRelativeToTheView) {
    // Looking along +Z (yaw 0): +X is on the right.
    EXPECT_NEAR(H::Azimuth(H::WorldYaw(1.0f, 0.0f), 0.0f), 90.0f * Deg, 1e-5f);
    // Looking along +X (yaw 90): -Z is on the right (right = (cos 90, 0, -sin 90)), +Z on the left,
    // -X behind.
    EXPECT_NEAR(H::Azimuth(H::WorldYaw(0.0f, -1.0f), 90.0f * Deg), 90.0f * Deg, 1e-5f);
    EXPECT_NEAR(H::Azimuth(H::WorldYaw(0.0f, 1.0f), 90.0f * Deg), -90.0f * Deg, 1e-5f);
    EXPECT_NEAR(std::abs(H::Azimuth(H::WorldYaw(-1.0f, 0.0f), 90.0f * Deg)), 180.0f * Deg, 1e-5f);
    // Wrapped into (-pi, pi].
    EXPECT_NEAR(H::Azimuth(170.0f * Deg, -170.0f * Deg), -20.0f * Deg, 1e-5f);
    EXPECT_NEAR(std::abs(H::Wrap(3.0f * H::Pi)), H::Pi, 1e-5f);
    EXPECT_NEAR(H::Wrap(-7.0f * H::Pi / 4.0f), H::Pi / 4.0f, 1e-5f);
    EXPECT_EQ(H::Wrap(NAN), 0.0f);
}

TEST(HitDirection, AttackerFirstThenHitPoint) {
    // The mock's blow #4 (2026-10-06): the beast 1.21 m away on one side, the hit position 0.19 m
    // off the hunter's centre on the other: the attacker wins.
    H::Sample s = At({10.0f, 2.0f, -5.0f});
    s.hit_ok = true;
    s.hit = {10.4f, 3.2f, -5.0f}; // 0.4 m to +X, at chest height
    s.attacker_ok = true;
    s.attacker = {10.0f, 2.0f, -3.0f}; // 2 m to +Z
    H::Resolved r = H::Resolve(s);
    EXPECT_EQ(r.source, H::Source::Attacker);
    EXPECT_NEAR(r.world_yaw, 0.0f, 1e-4f);
    EXPECT_NEAR(r.distance, 2.0f, 1e-4f);

    // No attacker (his own Molotov, a trap): by default nothing (the hit position is only
    // logged: UseHitPoint false); with it, the hit position when far enough off his centre.
    s.attacker_ok = false;
    EXPECT_FALSE(H::UseHitPoint);
    EXPECT_EQ(H::Resolve(s).source, H::Source::None);
    r = H::Resolve(s, true);
    EXPECT_EQ(r.source, H::Source::HitPoint);
    EXPECT_NEAR(r.world_yaw, 90.0f * Deg, 1e-4f);
    EXPECT_NEAR(r.distance, 0.4f, 1e-4f);

    // An attacker on top of him (closer than 0.3 m, a grab): none by default, else the hit point.
    s.attacker_ok = true;
    s.attacker = {10.1f, 2.0f, -4.9f};
    EXPECT_EQ(H::Resolve(s).source, H::Source::None);
    EXPECT_EQ(H::Resolve(s, true).source, H::Source::HitPoint);

    // Implausible hit positions (on the centre line, far away, far below, not finite): none.
    s.attacker_ok = false;
    for (const H::V3 p : {H::V3{10.1f, 3.0f, -5.03f}, H::V3{14.0f, 2.0f, -5.0f},
                          H::V3{10.5f, -8.0f, -5.0f}, H::V3{NAN, 2.0f, -5.0f}}) {
        s.hit = p;
        EXPECT_EQ(H::Resolve(s, true).source, H::Source::None);
    }
}

TEST(HitDirection, RumbleOfTheBlowIsWeighted) {
    constexpr std::int64_t Ms = 1'000'000;
    H::RumbleWeighting w;
    EXPECT_FALSE(w.Step(0, 0.8f)); // no hit: as it is
    const H::Hands right = H::HandGains(90.0f * Deg);
    w.Hit(1000 * Ms, right);
    EXPECT_FALSE(w.Step(1010 * Ms, 0.0f)); // nothing to weight yet
    // The game's rumble starts 0.55 s after the blow: weighted until it stops, past the window.
    EXPECT_TRUE(w.Step(1550 * Ms, 0.8f));
    EXPECT_TRUE(w.Step(2200 * Ms, 0.8f));
    const H::Hands r = H::WeightRumble(0.0f, 0.8f, true, w.gain);
    EXPECT_NEAR(r.right, 0.8f, 1e-6f);
    EXPECT_NEAR(r.left, 0.8f * H::FarHandMin, 1e-5f);
    // It stops; a new rumble after the start window is another one: as it is.
    EXPECT_FALSE(w.Step(2300 * Ms, 0.0f));
    EXPECT_FALSE(w.Step(2400 * Ms, 0.8f));
    // A blow from the left whose rumble was already on: weighted at once, at most RumbleMaxNs.
    w.Hit(3000 * Ms, H::HandGains(-90.0f * Deg));
    EXPECT_TRUE(w.Step(3001 * Ms, 0.5f));
    const H::Hands l = H::WeightRumble(0.0f, 0.5f, true, w.gain);
    EXPECT_NEAR(l.left, 0.5f, 1e-6f);
    EXPECT_NEAR(l.right, 0.5f * H::FarHandMin, 1e-5f);
    EXPECT_TRUE(w.Step(5400 * Ms, 0.5f));
    EXPECT_FALSE(w.Step(5600 * Ms, 0.5f));
    EXPECT_EQ(w.hit_ns, 0);
    // Unweighted: the motors as they are.
    const H::Hands same = H::WeightRumble(0.1f, 0.8f, false, right);
    EXPECT_NEAR(same.left, 0.1f, 1e-6f);
    EXPECT_NEAR(same.right, 0.8f, 1e-6f);
}

TEST(HitDirection, NoDirectionWithoutASource) {
    H::Sample s = At({0.0f, 0.0f, 0.0f});
    EXPECT_EQ(H::Resolve(s).source, H::Source::None);
    // His own Molotov at his feet, no attacker.
    s.hit_ok = true;
    s.hit = {0.03f, 0.1f, 0.0f};
    EXPECT_EQ(H::Resolve(s).source, H::Source::None);
    // An attacker on top of him (a grab) has no side.
    s.attacker_ok = true;
    s.attacker = {0.1f, 0.0f, 0.1f};
    EXPECT_EQ(H::Resolve(s).source, H::Source::None);
    // No centre: nothing.
    H::Sample n;
    n.attacker_ok = true;
    n.attacker = {3.0f, 0.0f, 0.0f};
    EXPECT_EQ(H::Resolve(n).source, H::Source::None);
}

// The flash: the wedge's centre (x right, y up) and its weight around the ring.
TEST(HitDirection, WedgeMapsLikeACompass) {
    const H::Wedge right = H::WedgeFor(90.0f * Deg);
    EXPECT_NEAR(right.x, 1.0f, 1e-5f);
    EXPECT_NEAR(right.y, 0.0f, 1e-5f);
    EXPECT_NEAR(right.focus, 1.0f, 1e-5f);
    const H::Wedge left = H::WedgeFor(-90.0f * Deg);
    EXPECT_NEAR(left.x, -1.0f, 1e-5f);
    const H::Wedge behind = H::WedgeFor(180.0f * Deg);
    EXPECT_NEAR(behind.y, -1.0f, 1e-5f);
    EXPECT_NEAR(behind.focus, 1.0f, 1e-5f);
    const H::Wedge ahead = H::WedgeFor(0.0f);
    EXPECT_NEAR(ahead.y, 1.0f, 1e-5f);
    EXPECT_NEAR(ahead.focus, H::FrontFocus, 1e-5f);
    // The focus narrows smoothly from 20 to 60 degrees off ahead.
    EXPECT_NEAR(H::WedgeFor(20.0f * Deg).focus, H::FrontFocus, 1e-5f);
    EXPECT_GT(H::WedgeFor(40.0f * Deg).focus, H::FrontFocus);
    EXPECT_LT(H::WedgeFor(40.0f * Deg).focus, 1.0f);
    EXPECT_NEAR(H::WedgeFor(-60.0f * Deg).focus, 1.0f, 1e-5f);
}

TEST(HitDirection, RingWeightIsAWedge) {
    const H::Wedge right = H::WedgeFor(90.0f * Deg);
    EXPECT_NEAR(H::RingWeight(right, 1.0f, 0.0f), 1.0f, 1e-5f);   // its centre
    EXPECT_NEAR(H::RingWeight(right, 5.0f, 0.0f), 1.0f, 1e-5f);   // any radius
    EXPECT_NEAR(H::RingWeight(right, 0.0f, 1.0f), 0.0625f, 1e-4f); // the top: 90 deg off
    EXPECT_NEAR(H::RingWeight(right, -1.0f, 0.0f), 0.0f, 1e-5f);  // the far side
    // Half at about 47 degrees from its centre.
    const float c = std::cos(47.0f * Deg), s = std::sin(47.0f * Deg);
    EXPECT_NEAR(H::RingWeight(right, c, s), 0.5f, 0.02f);
    // Behind: the bottom and both lower corners, nothing at the top.
    const H::Wedge behind = H::WedgeFor(H::Pi);
    EXPECT_NEAR(H::RingWeight(behind, 0.0f, -1.0f), 1.0f, 1e-5f);
    EXPECT_GT(H::RingWeight(behind, -0.7071f, -0.7071f), 0.5f);
    EXPECT_GT(H::RingWeight(behind, 0.7071f, -0.7071f), 0.5f);
    EXPECT_NEAR(H::RingWeight(behind, -0.7071f, -0.7071f),
                H::RingWeight(behind, 0.7071f, -0.7071f), 1e-5f);
    EXPECT_LT(H::RingWeight(behind, 0.0f, 1.0f), 1e-4f);
    // Ahead: the top in full, the rest of the ring at half.
    const H::Wedge ahead = H::WedgeFor(0.0f);
    EXPECT_NEAR(H::RingWeight(ahead, 0.0f, 1.0f), 1.0f, 1e-5f);
    EXPECT_NEAR(H::RingWeight(ahead, 0.0f, -1.0f), 0.5f, 1e-5f);
    EXPECT_NEAR(H::RingWeight(ahead, 1.0f, 0.0f), 0.5f + 0.5f * 0.0625f, 1e-4f);
    // The plain ring (no focus) is 1 everywhere.
    H::Wedge plain;
    EXPECT_NEAR(H::RingWeight(plain, -0.3f, 0.2f), 1.0f, 1e-6f);
    EXPECT_NEAR(H::RingWeight(right, 0.0f, 0.0f), 0.0f + 1.0f * std::pow(0.5f, 4.0f), 1e-5f);
}

TEST(HitDirection, HitSideHandIsStronger) {
    H::Hands h = H::HandGains(90.0f * Deg); // from the right
    EXPECT_NEAR(h.right, 1.0f, 1e-6f);
    EXPECT_NEAR(h.left, H::FarHandMin, 1e-5f);
    h = H::HandGains(-90.0f * Deg); // from the left
    EXPECT_NEAR(h.left, 1.0f, 1e-6f);
    EXPECT_NEAR(h.right, H::FarHandMin, 1e-5f);
    h = H::HandGains(0.0f); // ahead: both
    EXPECT_NEAR(h.left, 1.0f, 1e-6f);
    EXPECT_NEAR(h.right, 1.0f, 1e-6f);
    h = H::HandGains(H::Pi); // behind: both
    EXPECT_NEAR(h.left, 1.0f, 1e-5f);
    EXPECT_NEAR(h.right, 1.0f, 1e-5f);
    h = H::HandGains(-135.0f * Deg); // behind-left: the left full, the right between
    EXPECT_NEAR(h.left, 1.0f, 1e-6f);
    EXPECT_GT(h.right, H::FarHandMin);
    EXPECT_LT(h.right, 1.0f);
    EXPECT_GE(h.right, H::ThumpMinGain); // still thumped
    // Straight from the side the far hand gets no thump, only the scaled rumble.
    EXPECT_LT(H::HandGains(80.0f * Deg).left, H::ThumpMinGain);
}

TEST(HitDirection, ThumpGrowsWithTheHit) {
    EXPECT_NEAR(H::ThumpAmplitude(0.0f), 0.55f, 1e-6f);
    EXPECT_NEAR(H::ThumpAmplitude(0.125f), 0.725f, 1e-5f);
    EXPECT_NEAR(H::ThumpAmplitude(0.25f), 0.9f, 1e-6f);
    EXPECT_NEAR(H::ThumpAmplitude(0.8f), 0.9f, 1e-6f);
}

TEST(HitDirection, CurrentAzimuthFollowsTheHead) {
    H::Event e;
    e.world_yaw = 90.0f * Deg; // +X
    e.view_yaw = 0.0f;
    e.azimuth = 90.0f * Deg;
    // The head turned toward it: now ahead.
    EXPECT_NEAR(H::CurrentAzimuth(e, true, 90.0f * Deg), 0.0f, 1e-5f);
    // No view now: the event's own.
    EXPECT_NEAR(H::CurrentAzimuth(e, false, 0.0f), 90.0f * Deg, 1e-5f);
}

TEST(HitDirection, SideNames) {
    EXPECT_STREQ(H::SideName(0.0f), "ahead");
    EXPECT_STREQ(H::SideName(90.0f * Deg), "right");
    EXPECT_STREQ(H::SideName(-90.0f * Deg), "left");
    EXPECT_STREQ(H::SideName(H::Pi), "behind");
    EXPECT_STREQ(H::SideName(-135.0f * Deg), "behind-left");
    EXPECT_STREQ(H::SideName(45.0f * Deg), "ahead-right");
}

TEST(HitDirection, ChannelCarriesTheNewestEvent) {
    H::Event e;
    e.serial = 7;
    e.source = static_cast<std::uint8_t>(H::Source::Attacker);
    e.view_ok = true;
    e.azimuth = -1.0f;
    H::Channel().Store(e);
    H::Event out;
    ASSERT_TRUE(H::Channel().Load(out));
    EXPECT_EQ(out.serial, 7u);
    EXPECT_EQ(out.source, static_cast<std::uint8_t>(H::Source::Attacker));
    EXPECT_TRUE(out.view_ok);
    EXPECT_NEAR(out.azimuth, -1.0f, 1e-6f);
}

} // namespace
