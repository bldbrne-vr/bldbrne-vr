// SPDX-License-Identifier: GPL-2.0-or-later

// The view recenter's tracking-space shift, its log and fade (src/core/vr/space_shift.h), and
// the body's held hand across it (src/core/vr/body_frame.h HandHold::MoveSpace).

#include <cmath>
#include <cstdint>
#include <gtest/gtest.h>

#include "c0000_test_skeleton.h"
#include "core/vr/body_frame.h"
#include "core/vr/space_shift.h"

namespace {

using namespace VR::Space;

struct V3 {
    float x, y, z;
};
struct Q4 {
    float x, y, z, w;
};

constexpr float Pi = 3.14159265358979f;
constexpr float Deg = Pi / 180.0f;

Q4 Mul(const Q4& a, const Q4& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Q4 Axis(float x, float y, float z, float angle) {
    const float s = std::sin(0.5f * angle);
    return {x * s, y * s, z * s, std::cos(0.5f * angle)};
}
// Yaw (about +Y, + = left), then pitch (+ = up), then roll about the view axis: the head's pose.
Q4 Head(float yaw, float pitch, float roll = 0.0f) {
    return Mul(Mul(Axis(0, 1, 0, yaw), Axis(1, 0, 0, pitch)), Axis(0, 0, 1, roll));
}
V3 Rotate(const Q4& q, const V3& v) {
    const Q4 p{v.x, v.y, v.z, 0.0f};
    const Q4 r = Mul(Mul(q, p), Q4{-q.x, -q.y, -q.z, q.w});
    return {r.x, r.y, r.z};
}
// vr_session.cpp YawPitchFromQuat: the yaw of the forward vector.
float Yaw(const Q4& q) {
    const V3 f = Rotate(q, {0.0f, 0.0f, -1.0f});
    return std::atan2(-f.x, -f.z);
}
float Wrap(float a) {
    return std::remainder(a, 2.0f * Pi);
}
// body_frame.h TrackingToWorld (scale 1): a tracking-space offset in the game world.
V3 TrackingToWorld(const V3& d, float base_yaw) {
    const float cb = std::cos(base_yaw), sb = std::sin(base_yaw);
    return {cb * d.x - sb * d.z, d.y, -sb * d.x - cb * d.z};
}
void ExpectNear(const V3& a, const V3& b, float tolerance = 1e-5f) {
    EXPECT_NEAR(a.x, b.x, tolerance);
    EXPECT_NEAR(a.y, b.y, tolerance);
    EXPECT_NEAR(a.z, b.z, tolerance);
}

TEST(SpaceShift, RecenterOnHeadPutsHeadAtOriginFacingForward) {
    // The task's case: head turned 40 degrees left, 0.2 m to the side.
    const Q4 head = Head(40.0f * Deg, -10.0f * Deg);
    const V3 position{0.2f, 1.6f, -0.05f};
    const Shift s = FromHead(head, position);
    EXPECT_NEAR(s.yaw, 40.0f * Deg, 1e-5f);
    ExpectNear(PointToNew(s, position), {0.0f, 0.0f, 0.0f});
    const Q4 now = RotationToNew(s, head);
    EXPECT_NEAR(Yaw(now), 0.0f, 1e-5f);
    // The pitch is kept: the forward's height is the same.
    EXPECT_NEAR(Rotate(now, {0, 0, -1}).y, Rotate(head, {0, 0, -1}).y, 1e-5f);
}

TEST(SpaceShift, PointsRoundTrip) {
    const Shift s{1.1f, 0.3f, -0.2f, 0.7f, true};
    const V3 p{-0.4f, 1.2f, 2.5f};
    ExpectNear(PointToOld(s, PointToNew(s, p)), p);
    ExpectNear(PointToNew(s, PointToOld(s, p)), p);
}

TEST(SpaceShift, ViewYawAndGameMappingUnchanged) {
    // For many heads, shifts and base yaws: view yaw = base - head yaw stays, and every tracking
    // offset (hand - head, 6DoF movement) lands at the same place in the game world.
    for (int i = 0; i < 200; ++i) {
        const float a = 0.37f * i;
        const Q4 head = Head(std::sin(a) * 3.0f, std::sin(1.7f * a) * 1.2f, std::cos(a) * 0.4f);
        const Shift s{std::cos(2.3f * a) * 3.0f, std::sin(a), 1.5f, std::cos(a), true};
        const float base = std::sin(0.9f * a) * 3.0f;
        const float view_before = Wrap(base - Yaw(head));
        const float base_after = BaseYawAfter(base, s);
        const float view_after = Wrap(base_after - Yaw(RotationToNew(s, head)));
        EXPECT_NEAR(Wrap(view_after - view_before), 0.0f, 2e-4f) << "case " << i;
        const V3 hand{0.3f * std::cos(a), 1.2f, -0.4f + 0.1f * std::sin(a)};
        const V3 eye{0.0f, 1.6f, 0.05f};
        const V3 d{hand.x - eye.x, hand.y - eye.y, hand.z - eye.z};
        const V3 hn = PointToNew(s, hand), en = PointToNew(s, eye);
        ExpectNear(TrackingToWorld({hn.x - en.x, hn.y - en.y, hn.z - en.z}, base_after),
                   TrackingToWorld(d, base), 1e-4f);
    }
}

TEST(SpaceShift, ComposeAndInverse) {
    const Shift a{0.8f, 0.2f, 0.1f, -0.3f, true};
    const Shift b{-1.9f, -0.5f, 0.0f, 0.4f, true};
    const V3 p{1.0f, 2.0f, 3.0f};
    ExpectNear(PointToNew(Compose(a, b), p), PointToNew(b, PointToNew(a, p)));
    EXPECT_TRUE(IsIdentity(Compose(a, Inverse(a))));
    EXPECT_TRUE(IsIdentity(Compose(Inverse(b), b)));
    EXPECT_FALSE(IsIdentity(a));
    const Q4 q = Head(0.5f, 0.2f, 0.1f);
    const Q4 two = RotationToNew(b, RotationToNew(a, q));
    const Q4 one = RotationToNew(Compose(a, b), q);
    EXPECT_NEAR(std::abs(two.x * one.x + two.y * one.y + two.z * one.z + two.w * one.w), 1.0f,
                1e-5f);
}

TEST(SpaceShift, UnknownShiftCarriesNothing) {
    const Shift unknown{0.0f, 0.0f, 0.0f, 0.0f, false};
    EXPECT_NEAR(BaseYawAfter(1.25f, unknown), 1.25f, 1e-6f);
    EXPECT_FALSE(Compose(unknown, Shift{}).known);
    EXPECT_FALSE(Compose(Shift{}, unknown).known);
    EXPECT_FALSE(IsIdentity(unknown));
    EXPECT_TRUE(IsIdentity(Shift{}));
}

TEST(SpaceShift, HeadingAtThePoles) {
    // Looking straight down or up: the heading is still where the face turns (no flip).
    EXPECT_NEAR(Heading(Head(30.0f * Deg, -90.0f * Deg)), 30.0f * Deg, 1e-4f);
    EXPECT_NEAR(Heading(Head(30.0f * Deg, 90.0f * Deg)), 30.0f * Deg, 1e-4f);
    EXPECT_NEAR(Heading(Head(-120.0f * Deg, -89.0f * Deg)), -120.0f * Deg, 1e-4f);
    EXPECT_NEAR(Heading(Head(170.0f * Deg, 20.0f * Deg)), 170.0f * Deg, 1e-4f);
    // A tilted head (roll) faces where its nose points.
    EXPECT_NEAR(Heading(Head(45.0f * Deg, 0.0f, 25.0f * Deg)), 45.0f * Deg, 1e-4f);
    EXPECT_NEAR(Heading(Head(45.0f * Deg, -30.0f * Deg, 20.0f * Deg)), 45.0f * Deg, 1e-4f);
    EXPECT_NEAR(Heading(Head(-60.0f * Deg, 60.0f * Deg, -30.0f * Deg)), -60.0f * Deg, 1e-4f);
    // Near a pole with roll: no flip, within a few degrees of the nose.
    EXPECT_NEAR(Heading(Head(45.0f * Deg, -80.0f * Deg, 10.0f * Deg)), 45.0f * Deg, 12.0f * Deg);
}

TEST(SpaceShiftFade, OutBlackIn) {
    Fade fade;
    EXPECT_NEAR(fade.Brightness(), 1.0f, 1e-6f);
    EXPECT_FALSE(fade.Step(0.1f, true)); // idle
    EXPECT_TRUE(fade.Start());
    int swaps = 0;
    float t = 0.0f;
    const float dt = 1.0f / 120.0f;
    float last = 1.0f;
    while (fade.phase == Fade::Phase::Out) {
        swaps += fade.Step(dt, false) ? 1 : 0;
        t += dt;
        EXPECT_LE(fade.Brightness(), last);
        last = fade.Brightness();
    }
    EXPECT_EQ(swaps, 1);
    EXPECT_NEAR(t, Fade::OutS, dt);
    EXPECT_EQ(fade.phase, Fade::Phase::Black);
    EXPECT_NEAR(fade.Brightness(), 0.0f, 1e-6f);
    EXPECT_FALSE(fade.Start()); // a second hold during the black is the same recenter
    for (int i = 0; i < 10; ++i) {
        EXPECT_FALSE(fade.Step(dt, false)); // old frames still shown: stays black
    }
    EXPECT_EQ(fade.phase, Fade::Phase::Black);
    fade.Step(dt, true);
    EXPECT_EQ(fade.phase, Fade::Phase::In);
    float in = 0.0f;
    while (fade.phase == Fade::Phase::In) {
        EXPECT_FALSE(fade.Step(dt, true));
        in += dt;
    }
    EXPECT_NEAR(in, Fade::InS, dt);
    EXPECT_NEAR(fade.Brightness(), 1.0f, 1e-6f);
}

TEST(SpaceShiftFade, BlackIsCapped) {
    Fade fade;
    fade.Start();
    EXPECT_TRUE(fade.Step(1.0f, false));
    float t = 0.0f;
    while (fade.phase == Fade::Phase::Black && t < 5.0f) {
        fade.Step(0.01f, false); // the new space's frames never come (a stalled game)
        t += 0.01f;
    }
    EXPECT_NEAR(t, Fade::MaxBlackS, 0.011f);
}

TEST(SpaceShiftFade, RestartDuringFadeInKeepsBrightness) {
    Fade fade;
    fade.Start();
    fade.Step(1.0f, false);
    fade.Step(0.01f, true);
    fade.Step(0.05f, true); // fading in
    const float before = fade.Brightness();
    EXPECT_GT(before, 0.0f);
    EXPECT_LT(before, 1.0f);
    EXPECT_TRUE(fade.Start());
    EXPECT_EQ(fade.phase, Fade::Phase::Out);
    EXPECT_NEAR(fade.Brightness(), before, 1e-5f);
}

TEST(SpaceShiftFade, CutToBlackHasNoSwap) {
    Fade fade;
    fade.Start();
    fade.Step(0.01f, false);
    fade.CutToBlack(); // the runtime recentred meanwhile
    EXPECT_EQ(fade.phase, Fade::Phase::Black);
    for (int i = 0; i < 20; ++i) {
        EXPECT_FALSE(fade.Step(0.01f, false)); // never a change of our own
    }
    fade.Step(0.01f, true);
    EXPECT_EQ(fade.phase, Fade::Phase::In);
    // From idle (no recenter of ours): black at once, then back in.
    Fade idle;
    idle.CutToBlack();
    EXPECT_NEAR(idle.Brightness(), 0.0f, 1e-6f);
    EXPECT_FALSE(idle.Step(0.01f, true));
    EXPECT_EQ(idle.phase, Fade::Phase::In);
}

TEST(SpaceShiftLog, BetweenComposesTheChangesInOrder) {
    ShiftLog<4> log;
    const Shift a{0.7f, 0.2f, 1.6f, -0.1f, true};
    const Shift b{-0.3f, 0.0f, 0.1f, 0.4f, true};
    const Shift c{1.4f, -0.5f, 0.0f, 0.2f, true};
    log.Add(1, a);
    log.Add(2, b);
    log.Add(3, c);
    const V3 p{0.3f, 1.1f, -0.6f};
    Shift s{};
    ASSERT_TRUE(log.Between(0, 3, s));
    ExpectNear(PointToNew(s, p), PointToNew(c, PointToNew(b, PointToNew(a, p))));
    // A camera tick that saw space 1 and now has a sample of space 2 only takes b (a change noted
    // after its sample is carried with the next one).
    ASSERT_TRUE(log.Between(1, 2, s));
    ExpectNear(PointToNew(s, p), PointToNew(b, p));
    ASSERT_TRUE(log.Between(2, 2, s));
    EXPECT_TRUE(IsIdentity(s));
    // Not later, or beyond what the log holds: unknown.
    EXPECT_FALSE(log.Between(3, 2, s));
    EXPECT_FALSE(s.known);
    log.Add(4, a);
    log.Add(5, b); // overwrites space 1's entry
    EXPECT_FALSE(log.Between(0, 5, s));
    EXPECT_FALSE(s.known);
    EXPECT_TRUE(log.Between(1, 5, s));
    // A change without its pose (the runtime's recenter): everything across it is unknown.
    log.Add(6, Shift{0.0f, 0.0f, 0.0f, 0.0f, false});
    log.Add(7, c);
    EXPECT_FALSE(log.Between(5, 7, s));
    EXPECT_TRUE(log.Between(6, 7, s));
    ExpectNear(PointToNew(s, p), PointToNew(c, p));
    // A serial never added (the session's first space has none).
    ShiftLog<4> empty;
    EXPECT_FALSE(empty.Between(0, 1, s));
    EXPECT_TRUE(empty.Between(0, 0, s));
}

TEST(SpaceShiftLog, SerialsWrap) {
    ShiftLog<8> log;
    const Shift a{0.5f, 0.1f, 0.0f, 0.0f, true};
    const std::uint32_t last = 0xFFFFFFFFu;
    log.Add(last, a);
    log.Add(0, a); // the serial after 0xFFFFFFFF
    Shift s{};
    ASSERT_TRUE(log.Between(last - 1, 0, s));
    EXPECT_NEAR(s.yaw, 1.0f, 1e-5f);
}

// The body's held hand (core/vr/body_frame.h HandHold) across a recenter: held, it stays where it
// was in the room, and a regain eases from where it was shown.
TEST(SpaceShiftHold, HeldHandStaysInTheRoom) {
    namespace B = VR::Body;
    const Shift s = FromHead(Head(50.0f * Deg, 0.0f), V3{0.3f, 1.6f, -0.2f});
    B::TrackedHand hand{};
    hand.position_valid = hand.position_tracked = true;
    hand.orientation_valid = hand.orientation_tracked = true;
    hand.velocity_valid = true;
    hand.p = {0.25f, 1.2f, -0.45f};
    const Q4 hq = Head(0.3f, -0.2f, 0.1f);
    hand.q = {hq.x, hq.y, hq.z, hq.w};
    B::HandHold hold;
    B::TrackedHand out{};
    const std::int64_t ms = 1'000'000;
    const B::HeadPose head{true, {0.0f, 1.6f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
    EXPECT_EQ(hold.Update(hand, 0, 300 * ms, out, -1, head), B::HandUse::Tracked);
    // Lost: held at its last pose.
    B::TrackedHand lost = hand;
    lost.position_valid = lost.position_tracked = false;
    lost.orientation_valid = false;
    EXPECT_EQ(hold.Update(lost, 10 * ms, 300 * ms, out), B::HandUse::Held);
    // The recenter: the held pose moves into the new space with it.
    hold.MoveSpace(s);
    EXPECT_EQ(hold.Update(lost, 20 * ms, 300 * ms, out), B::HandUse::Held);
    const V3 moved = PointToNew(s, V3{hand.p.x, hand.p.y, hand.p.z});
    ExpectNear({out.p.x, out.p.y, out.p.z}, moved);
    const Q4 turned = RotationToNew(s, hq);
    EXPECT_NEAR(std::abs(out.q.x * turned.x + out.q.y * turned.y + out.q.z * turned.z +
                         out.q.w * turned.w),
                1.0f, 1e-5f);
    // Regained in the new space where it was: no ease jump (shown == controller).
    B::TrackedHand back = hand;
    back.p = {moved.x, moved.y, moved.z};
    back.q = {turned.x, turned.y, turned.z, turned.w};
    const V3 head_new = PointToNew(s, V3{0.0f, 1.6f, 0.0f});
    const Q4 head_q = RotationToNew(s, Q4{0.0f, 0.0f, 0.0f, 1.0f});
    EXPECT_EQ(hold.Update(back, 30 * ms, 300 * ms, out, -1,
                          B::HeadPose{true, {head_new.x, head_new.y, head_new.z},
                                      {head_q.x, head_q.y, head_q.z, head_q.w}}),
              B::HandUse::Tracked);
    ExpectNear({out.p.x, out.p.y, out.p.z}, moved, 1e-4f);
    // An unknown shift drops the hold.
    hold.MoveSpace(Shift{0.0f, 0.0f, 0.0f, 0.0f, false});
    EXPECT_EQ(hold.Update(lost, 40 * ms, 300 * ms, out), B::HandUse::Lost);
}

} // namespace
