// SPDX-License-Identifier: GPL-2.0-or-later

// The two-handed grip (src/core/vr/two_hand_grip.h): the handle zone, the squeeze-edge rule, the
// weapon's axis from both hands, the left hand on the handle, the release rules and the
// two-handed swing's bonus.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <gtest/gtest.h>

#include "c0000_test_skeleton.h"
#include "core/vr/body_frame.h"
#include "core/vr/two_hand_grip.h"
#include "core/vr/two_hand_settings.h"
#include "core/vr/two_hand_motion.h"
#include "core/vr/swing_detector.h"
#include "core/vr/melee_window.h"
#include "core/vr/trick_weapons.h"

namespace {

using namespace VR::TwoHand;
using VR::Body::Deg;

constexpr float Dt = 1.0f / 90.0f;

BoneOnGrip Grip(int side, float pitch_deg = 0.0f, Vec3 offset = {}) {
    return {VR::Body::DefaultGrip(side, pitch_deg), offset};
}

const BoneOnGrip R0 = Grip(VR::Body::C0000::Right);
const BoneOnGrip L0 = Grip(VR::Body::C0000::Left);

float Dist(Vec3 a, Vec3 b) {
    return VR::Body::Length(a - b);
}

// The absolute steering model (the weapon along the hands' line, or turned from `ref_local`, the
// grab's line): the reference the tests check Place's frames with. The game steers through
// SupportGrip.
Applied Apply(const Pose& right, const Pose& left, const BoneOnGrip& r, const BoneOnGrip& l,
              const GameGrip& g, float scale, float w, const Vec3* ref_local = nullptr) {
    const GameGrip& use = g.valid ? g : DefaultGrip;
    const Handle h = HandleAt(right, r, use, scale);
    const Vec3 v = BoneOrigin(left, l) - h.right;
    const float hands = VR::Body::Length(v);
    if (!h.valid || !VR::Body::IsFinite(v)) {
        Applied out{};
        out.right = {right.p, VR::Body::Normalize(right.q)};
        out.left = {left.p, VR::Body::Normalize(left.q)};
        out.hands_m = hands;
        return out;
    }
    const float axis_w = VR::Body::Smoothstep(AxisFadeM, AxisFullM, hands);
    const Quat q = VR::Body::Normalize(right.q);
    Vec3 from = h.dir;
    if (ref_local != nullptr && VR::Body::IsFinite(*ref_local) &&
        VR::Body::Length(*ref_local) > 0.5f) {
        from = VR::Body::Rotate(q, VR::Body::Normalize(*ref_local));
    }
    const Quat world = VR::Body::ScaleRotation(VR::Body::FromTo(from, v), axis_w);
    const Quat local = VR::Body::Normalize(VR::Body::Conjugate(q) * world * q);
    return Place(right, left, r, l, use, scale, w, local);
}

// The right controller at the origin, unturned: the blade along -Z, the game's left grip 14 cm
// behind the right fist (+Z, the pommel side).
Handle H0() {
    return HandleAt({{0.0f, 0.0f, 0.0f}, {}}, R0, DefaultGrip, 1.0f);
}

Input In(Vec3 left, float grip, const Handle& h = H0()) {
    Input in{};
    in.dt = Dt;
    in.enabled = true;
    in.gameplay = true;
    in.two_handed = true;
    in.grip = grip;
    in.right_tracked = in.left_tracked = true;
    in.handle = h;
    in.left_bone = left;
    return in;
}

TEST(TwoHandFrames, TheBladeAndThePommelOnTheController) {
    // The weapon bone's blade (-Y) lands on the grip's -Z, pitched toward +Y by blade_pitch_deg.
    const Vec3 blade = BoneToGripLocal(R0, {0.0f, -1.0f, 0.0f}, 1.0f);
    EXPECT_NEAR(blade.x, 0.0f, 1e-5f);
    EXPECT_NEAR(blade.y, 0.0f, 1e-5f);
    EXPECT_NEAR(blade.z, -1.0f, 1e-5f);
    const BoneOnGrip pitched = Grip(VR::Body::C0000::Right, 20.0f);
    const Vec3 b20 = BoneToGripLocal(pitched, {0.0f, -1.0f, 0.0f}, 1.0f);
    EXPECT_NEAR(b20.y, std::sin(20.0f * Deg), 1e-5f);
    EXPECT_NEAR(b20.z, -std::cos(20.0f * Deg), 1e-5f);
    // The pommel side (+Y) behind the hand; game units over world_scale.
    const Vec3 pommel = BoneToGripLocal(R0, {0.0f, 0.28f, 0.0f}, 2.0f);
    EXPECT_NEAR(pommel.z, 0.14f, 1e-5f);
    // The trim offset is in the grip frame as it is.
    const Vec3 off = BoneToGripLocal(Grip(1, 0.0f, {0.01f, 0.02f, 0.03f}), {}, 1.0f);
    EXPECT_NEAR(off.x, 0.01f, 1e-6f);
    EXPECT_NEAR(off.z, 0.03f, 1e-6f);
}

TEST(TwoHandHandle, FollowsTheRightController) {
    const Handle h = H0();
    ASSERT_TRUE(h.valid);
    EXPECT_NEAR(h.length, 0.14f, 1e-5f);
    EXPECT_NEAR(h.dir.z, 1.0f, 1e-5f);
    EXPECT_NEAR(h.left.z, 0.14f, 1e-5f);
    // Turned 90 degrees about up: the handle turns with it.
    const Pose turned{{0.1f, 1.0f, -0.3f}, VR::Body::AxisAngle({0.0f, 1.0f, 0.0f}, 90.0f * Deg)};
    const Handle t = HandleAt(turned, R0, DefaultGrip, 1.0f);
    EXPECT_NEAR(t.dir.x, 1.0f, 1e-4f);
    EXPECT_NEAR(t.left.x, 0.24f, 1e-4f);
    EXPECT_NEAR(t.left.y, 1.0f, 1e-4f);
    // An invalid grip falls back to the default one.
    const Handle d = HandleAt({{}, {}}, R0, GameGrip{}, 1.0f);
    EXPECT_NEAR(d.length, 0.14f, 1e-5f);
}

TEST(TwoHandZone, ContinuousRailOnBothSidesOfTheDominantHand) {
    Handle h = H0();
    EXPECT_TRUE(ZoneOf(h, {0.0f, 0.0f, 0.14f}, 0.09f).inside);
    EXPECT_TRUE(ZoneOf(h, {0.08f, 0.0f, 0.14f}, 0.09f).inside);
    EXPECT_FALSE(ZoneOf(h, {0.10f, 0.0f, 0.14f}, 0.09f).inside);   // 10 cm off the handle
    EXPECT_FALSE(ZoneOf(h, {0.0f, -0.10f, 0.20f}, 0.09f).inside);  // below it
    // A sword (no bare shaft toward the blade): the blade side never takes a hand ...
    EXPECT_FALSE(ZoneOf(h, {0.0f, 0.0f, -0.12f}, 0.09f).inside);
    // ... and the rail ends a fist past the game's grip (14 + 4 cm), not 12 cm out in the air.
    EXPECT_TRUE(ZoneOf(h, {0.0f, 0.0f, 0.25f}, 0.09f).inside);
    EXPECT_FALSE(ZoneOf(h, {0.0f, 0.0f, 0.30f}, 0.09f).inside);
    // Not on the right fist itself (no steering there; the quick items keep that squeeze).
    EXPECT_FALSE(ZoneOf(h, {0.0f, 0.03f, 0.03f}, 0.09f).inside);
    EXPECT_FALSE(ZoneOf(h, {0.0f, 0.0f, -0.03f}, 0.09f).inside);
    // A haft with bare shaft past the fist: either hand can lead, up to the head.
    h.blade_clear = 0.22f;
    EXPECT_TRUE(ZoneOf(h, {0.0f, 0.0f, -0.12f}, 0.09f).inside);
    EXPECT_TRUE(ZoneOf(h, {0.0f, 0.0f, -0.30f}, 0.09f).inside);
    EXPECT_FALSE(ZoneOf(h, {0.0f, 0.0f, -0.33f}, 0.09f).inside);
    h.blade_clear = 0.0f;
    const Zone z = ZoneOf(h, {0.05f, 0.0f, 0.16f}, 0.09f);
    EXPECT_NEAR(z.along, 0.16f, 1e-5f);
    EXPECT_NEAR(z.off, 0.05f, 1e-5f);
    // A wider zone setting reaches further.
    EXPECT_TRUE(ZoneOf(h, {0.10f, 0.0f, 0.14f}, 0.12f).inside);
    // No handle, no zone.
    EXPECT_FALSE(ZoneOf(Handle{}, {0.0f, 0.0f, 0.14f}, 0.09f).inside);
}

TEST(TwoHandEdge, OnlyASqueezeOnTheHandleGrabs) {
    const Settings s{};
    Tracker t;
    Output o;
    const Vec3 on{0.02f, -0.01f, 0.15f};
    t.Update(s, In(on, 0.5f), o); // squeezing, not yet past the press
    EXPECT_FALSE(o.edge);
    EXPECT_FALSE(o.engaged);
    t.Update(s, In(on, 0.8f), o);
    EXPECT_TRUE(o.edge);
    EXPECT_TRUE(o.grabbed);
    EXPECT_TRUE(o.engaged);
    EXPECT_TRUE(o.owns_grip);
    EXPECT_TRUE(o.passed == nullptr);
    t.Update(s, In(on, 0.6f), o); // still held (above the let-go)
    EXPECT_TRUE(o.engaged);
    EXPECT_FALSE(o.grabbed);
    t.Update(s, In(on, 0.3f), o);
    EXPECT_FALSE(o.engaged);
    EXPECT_FALSE(o.owns_grip);
    EXPECT_TRUE(o.released == Why::Grip);
    EXPECT_FALSE(SoftTick(o.released));
}

TEST(TwoHandEdge, AHeldGripSlidingOntoTheHandleNeverGrabs) {
    const Settings s{};
    Tracker t;
    Output o;
    t.Update(s, In({0.30f, 0.0f, 0.10f}, 0.9f), o); // squeezed away from the handle
    EXPECT_TRUE(o.edge);
    EXPECT_FALSE(o.engaged);
    EXPECT_FALSE(o.owns_grip); // the quick items get it (the arc)
    ASSERT_NE(o.passed, nullptr);
    EXPECT_STREQ(o.passed, "not on the handle");
    for (int i = 0; i < 30; ++i) { // slides onto the handle, still squeezed
        const float k = static_cast<float>(i) / 29.0f;
        t.Update(s, In({0.30f * (1.0f - k), 0.0f, 0.10f + 0.04f * k}, 0.9f), o);
        EXPECT_FALSE(o.engaged);
        EXPECT_FALSE(o.owns_grip);
    }
    t.Update(s, In({0.0f, 0.0f, 0.14f}, 0.1f), o); // let go on the handle
    t.Update(s, In({0.0f, 0.0f, 0.14f}, 0.9f), o); // a new squeeze there grabs
    EXPECT_TRUE(o.grabbed);
}

TEST(TwoHandEdge, NotWithTheArcAVialAOneHandedFormOrAHeldOverGrip) {
    const Settings s{};
    const Vec3 on{0.0f, 0.0f, 0.14f};
    struct Case {
        const char* why;
        Input in;
    };
    Input busy = In(on, 0.9f);
    busy.busy = true;
    Input one = In(on, 0.9f);
    one.two_handed = false;
    Input blocked = In(on, 0.9f);
    blocked.blocked = true;
    Input menu = In(on, 0.9f);
    menu.gameplay = false;
    Input off = In(on, 0.9f);
    off.enabled = false;
    Input lost = In(on, 0.9f);
    lost.left_tracked = false;
    for (const Case& c : {Case{"an item or a vial", busy}, Case{"a one-handed form", one},
                          Case{"held through a context switch", blocked},
                          Case{"not in gameplay", menu}, Case{"two_hand_grip off", off},
                          Case{"a hand not tracked", lost}}) {
        Tracker t;
        Output o;
        t.Update(s, In(on, 0.0f), o);
        t.Update(s, c.in, o);
        EXPECT_TRUE(o.edge);
        EXPECT_FALSE(o.engaged);
        EXPECT_FALSE(o.owns_grip);
        ASSERT_NE(o.passed, nullptr);
        EXPECT_STREQ(o.passed, c.why);
    }
}

// A one-handed form whose grab transforms it (the grab group, the left hand freed): grabbed, and
// held through the transform until it is in its two-handed form; not freed, no grab.
TEST(TwoHandEdge, AFreedHandGrabsAOneHandedFormToTransformIt) {
    const Settings s{};
    const Vec3 on{0.0f, 0.0f, 0.14f};
    Input freed = In(on, 0.9f);
    freed.two_handed = false;
    freed.grab_transforms = true;
    Tracker t;
    Output o;
    Input idle = freed;
    idle.grip = 0.0f;
    t.Update(s, idle, o);
    t.Update(s, freed, o);
    EXPECT_TRUE(o.grabbed);
    EXPECT_TRUE(o.owns_grip);
    for (int i = 0; i < 20; ++i) { // the transform running, still form A
        t.Update(s, freed, o);
        EXPECT_TRUE(o.engaged);
    }
    Input formed = In(on, 0.9f); // in form B now: an ordinary two-handed hold
    t.Update(s, formed, o);
    EXPECT_TRUE(o.engaged);
    Input back = freed; // form A again without the freedom: let go (the form)
    back.grab_transforms = false;
    t.Update(s, back, o);
    EXPECT_EQ(o.released, Why::Form);
}

// A squeeze near the handle that cannot grab yet is kept from the quick items, and grabs without a
// new squeeze once allowed (the right grip a moment later; the hand back on the handle after a slip).
TEST(TwoHandEdge, AKeptSqueezeGrabsWhenAllowed) {
    const Settings s{};
    const Vec3 on{0.0f, 0.0f, 0.14f};
    const Vec3 near{0.0f, 0.12f, 0.14f}; // 12 cm off the handle: outside the 9 cm zone
    Input early = In(on, 0.9f);
    early.two_handed = false; // form A, not freed yet
    early.hold_near = true;
    Tracker t;
    Output o;
    Input idle = early;
    idle.grip = 0.0f;
    t.Update(s, idle, o);
    t.Update(s, early, o);
    EXPECT_FALSE(o.engaged);
    EXPECT_TRUE(o.held_near);
    EXPECT_TRUE(o.owns_grip); // no arc
    Input freed = early; // the right grip a frame later
    freed.grab_transforms = true;
    freed.grab_held = true;
    t.Update(s, freed, o);
    EXPECT_TRUE(o.grabbed);

    // A miss well away from the handle stays the quick items'.
    Tracker far;
    Input away = In({0.0f, 0.40f, 0.14f}, 0.9f);
    away.two_handed = false;
    away.hold_near = true;
    Input open = away;
    open.grip = 0.0f;
    far.Update(s, open, o);
    far.Update(s, away, o);
    EXPECT_FALSE(o.held_near);
    EXPECT_FALSE(o.owns_grip);

    // A near miss kept: not grabbed while still off the handle, then grabbed on it.
    Tracker slip;
    Input n = In(near, 0.9f);
    n.hold_near = true;
    Input n0 = n;
    n0.grip = 0.0f;
    slip.Update(s, n0, o);
    slip.Update(s, n, o);
    EXPECT_TRUE(o.held_near);
    n.grab_held = true;
    slip.Update(s, n, o);
    EXPECT_FALSE(o.engaged);
    Input back = In(on, 0.9f);
    back.grab_held = true;
    slip.Update(s, back, o);
    EXPECT_TRUE(o.grabbed);
    // Without grab_held a grip still held never grabs.
    Tracker plain;
    Input p0 = In(near, 0.0f);
    plain.Update(s, p0, o);
    plain.Update(s, In(near, 0.9f), o);
    plain.Update(s, In(on, 0.9f), o);
    EXPECT_FALSE(o.engaged);
}

TEST(TwoHandAxis, TheHandleRunsFromTheRightHandToTheLeft) {
    const Pose right{{0.1f, 1.1f, -0.4f}, VR::Body::AxisAngle({1.0f, 0.0f, 0.0f}, 0.3f)};
    const Handle before = HandleAt(right, R0, DefaultGrip, 1.0f);
    // The left hand 6 cm to the side of where the handle points (in the zone).
    const Vec3 left_p = before.left + Vec3{0.06f, 0.0f, 0.0f};
    const Pose left{left_p, VR::Body::AxisAngle({0.0f, 1.0f, 0.0f}, 1.0f)};
    const Applied a = Apply(right, left, R0, L0, DefaultGrip, 1.0f, 1.0f);
    const Handle after = HandleAt(a.right, R0, DefaultGrip, 1.0f);
    // The right fist stays; the handle now points at the real left hand.
    EXPECT_LT(Dist(after.right, before.right), 1e-5f);
    const Vec3 want = VR::Body::Normalize(left_p - before.right);
    EXPECT_GT(VR::Body::Dot(after.dir, want), 0.99999f);
    EXPECT_NEAR(a.turn_deg, std::atan2(0.06f, 0.14f) / Deg, 0.05f);
    // The roll is the right hand's: the shortest turn (its axis normal to both directions).
    const Quat d = a.right.q * VR::Body::Conjugate(VR::Body::Normalize(right.q));
    const Vec3 axis = VR::Body::Normalize(Vec3{d.x, d.y, d.z});
    EXPECT_NEAR(VR::Body::Dot(axis, before.dir), 0.0f, 1e-4f);
    EXPECT_NEAR(VR::Body::Dot(axis, want), 0.0f, 1e-4f);
    // The hunter's left fist on the handle at the game's grip point, turned as the right one (the
    // default grip: the same bone rotation, the two hands mirror each other round the handle).
    EXPECT_LT(Dist(BoneOrigin(a.left, L0), after.left), 1e-5f);
    EXPECT_GT(std::abs(VR::Body::Dot(VR::Body::Normalize(a.left.q),
                                     VR::Body::Normalize(a.right.q))),
              0.99999f);
}

TEST(TwoHandAxis, BlendsAndKeepsTheBoneOrigins) {
    const Vec3 off{0.01f, -0.02f, 0.03f};
    const BoneOnGrip r = Grip(VR::Body::C0000::Right, 15.0f, off);
    const BoneOnGrip l = Grip(VR::Body::C0000::Left, 15.0f, {-0.01f, 0.0f, 0.02f});
    const Pose right{{0.0f, 1.0f, -0.3f}, VR::Body::AxisAngle({0.0f, 0.0f, 1.0f}, 0.4f)};
    const Handle before = HandleAt(right, r, DefaultGrip, 1.0f);
    const Pose left{before.left + Vec3{0.0f, 0.07f, 0.0f}, {}};
    // w = 0: the controllers as they are.
    const Applied none = Apply(right, left, r, l, DefaultGrip, 1.0f, 0.0f);
    EXPECT_LT(Dist(none.left.p, left.p), 1e-6f);
    EXPECT_GT(std::abs(VR::Body::Dot(none.right.q, right.q)), 0.99999f);
    // Half way: half the turn.
    const Applied full = Apply(right, left, r, l, DefaultGrip, 1.0f, 1.0f);
    const Applied half = Apply(right, left, r, l, DefaultGrip, 1.0f, 0.5f);
    EXPECT_NEAR(VR::Body::AngleBetween(right.q, half.right.q),
                0.5f * VR::Body::AngleBetween(right.q, full.right.q), 1e-3f);
    // The right bone origin never moves (the trim offset is turned round it), and the left bone
    // lands on the handle with its own offset.
    EXPECT_LT(Dist(BoneOrigin(full.right, r), before.right), 1e-5f);
    EXPECT_LT(Dist(BoneOrigin(half.right, r), before.right), 1e-5f);
    const Handle after = HandleAt(full.right, r, DefaultGrip, 1.0f);
    EXPECT_LT(Dist(BoneOrigin(full.left, l), after.left), 1e-5f);
}

TEST(TwoHandAxis, HandsTooCloseKeepTheRightHandsDirection) {
    const Pose right{{0.0f, 1.0f, -0.3f}, {}};
    const Handle h = HandleAt(right, R0, DefaultGrip, 1.0f);
    const Pose left{h.right + Vec3{0.02f, 0.0f, 0.0f}, {}}; // 2 cm: the axis is undefined
    const Applied a = Apply(right, left, R0, L0, DefaultGrip, 1.0f, 1.0f);
    EXPECT_GT(std::abs(VR::Body::Dot(a.right.q, right.q)), 0.99999f);
    EXPECT_NEAR(a.turn_deg, 0.0f, 1e-4f);
    // The left hand still goes onto the (unturned) handle.
    EXPECT_LT(Dist(BoneOrigin(a.left, L0), h.left), 1e-5f);
}

TEST(TwoHandAxis, ALostLeftHandKeepsTheTurnAndStaysOnTheHandle) {
    // Both tracked: the turn in the right controller's own frame.
    const Pose right{{0.0f, 1.0f, -0.3f}, VR::Body::AxisAngle({0.0f, 1.0f, 0.0f}, 0.2f)};
    const Handle h = HandleAt(right, R0, DefaultGrip, 1.0f);
    const Pose left{h.left + Vec3{0.0f, 0.05f, 0.0f}, {}};
    const Applied a = Apply(right, left, R0, L0, DefaultGrip, 1.0f, 1.0f);
    // The right hand then turns 30 degrees more (a swing) while the left controller is occluded:
    // the weapon keeps its turn against the right hand, the left hand stays on the handle.
    const Quat swing = VR::Body::AxisAngle({1.0f, 0.0f, 0.0f}, 30.0f * Deg);
    const Pose right2{right.p + Vec3{0.1f, 0.0f, 0.0f}, VR::Body::Normalize(swing * right.q)};
    const Pose stale_left{{0.5f, 0.0f, 0.5f}, {}};
    const Applied k = Place(right2, stale_left, R0, L0, DefaultGrip, 1.0f, 1.0f, a.turn_local);
    const Quat want = VR::Body::Normalize(swing * a.right.q);
    EXPECT_GT(std::abs(VR::Body::Dot(k.right.q, want)), 0.99999f);
    const Handle h2 = HandleAt(k.right, R0, DefaultGrip, 1.0f);
    EXPECT_LT(Dist(BoneOrigin(k.left, L0), h2.left), 1e-5f);
    EXPECT_LT(Dist(h2.right, HandleAt(right2, R0, DefaultGrip, 1.0f).right), 1e-5f);
    EXPECT_NEAR(k.turn_deg, a.turn_deg, 1e-3f);
}

TEST(TwoHandAxis, TheGamesOwnGripPlacesAndTurnsTheLeftHand) {
    // A measured grip: 22 cm down the handle, 1 cm off it, the left bone turned 30 deg about Y.
    const GameGrip g{true, {0.01f, 0.22f, 0.0f}, VR::Body::AxisAngle({0.0f, 1.0f, 0.0f}, 0.52f)};
    ASSERT_TRUE(Plausible(g, 1.0f));
    const Pose right{{0.0f, 1.0f, -0.3f}, {}};
    const Handle h = HandleAt(right, R0, g, 1.0f);
    EXPECT_NEAR(h.length, std::sqrt(0.01f * 0.01f + 0.22f * 0.22f), 1e-5f);
    const Pose left{h.left, {}};
    const Applied a = Apply(right, left, R0, L0, g, 1.0f, 1.0f);
    EXPECT_NEAR(a.turn_deg, 0.0f, 1e-3f); // already on the line
    // The left bone = the right bone * the game's grip (in the game's controller frame).
    const Quat bone_r = PConj(a.right.q) * R0.grip;
    const Quat bone_l = PConj(a.left.q) * L0.grip;
    const Quat want = VR::Body::Normalize(bone_r * g.q);
    EXPECT_GT(std::abs(VR::Body::Dot(VR::Body::Normalize(bone_l), want)), 0.9999f);
}

TEST(TwoHandRelease, WideSwingsAndTwistsStayAttachedUntilBothGripsOpen) {
    Settings s{};
    Tracker t;
    Output o;
    auto in = In({0.0f, 0.0f, 0.14f}, 0.0f);
    t.Update(s, in, o);
    in.grip = 0.9f;
    in.right_grip = 0.9f;
    t.Update(s, in, o);
    ASSERT_TRUE(o.grabbed);
    // A long swing followed by crossed controllers is no reason to let go.
    for (Vec3 p : {Vec3{0.0f, 0.0f, 0.9f}, Vec3{0.0f, 0.0f, -0.3f}}) {
        in.left_bone = p;
        t.Update(s, in, o);
        EXPECT_TRUE(o.engaged);
        EXPECT_EQ(o.released, Why::None);
    }
    in.grip = 0.0f; // relaxing the left grip alone keeps the attachment and blocks items
    t.Update(s, in, o);
    EXPECT_TRUE(o.engaged);
    EXPECT_TRUE(o.owns_grip);
    in.right_grip = 0.5f; // hysteresis, not a fresh press
    t.Update(s, in, o);
    EXPECT_TRUE(o.engaged);
    in.grip = 0.9f;
    in.right_grip = 0.0f; // transfer retention back to the left hand
    t.Update(s, in, o);
    EXPECT_TRUE(o.engaged);
    in.grip = 0.0f;
    t.Update(s, in, o);
    EXPECT_FALSE(o.engaged);
    EXPECT_FALSE(o.owns_grip);
    EXPECT_EQ(o.released, Why::Grip);
}

TEST(TwoHandRelease, RightGripAloneNeverAcquiresAndWeaponSwitchClearsHold) {
    Tracker t;
    Output o;
    auto in = In({0.0f, 0.0f, 0.14f}, 0.0f);
    in.right_grip = 1.0f;
    in.family = 50;
    t.Update({}, in, o);
    EXPECT_FALSE(o.engaged);
    in.grip = 1.0f;
    t.Update({}, in, o);
    ASSERT_TRUE(o.engaged);
    in.family = 81;
    t.Update({}, in, o);
    EXPECT_EQ(o.released, Why::Form);
}

TEST(TwoHandRelease, FormArcVialMenuAndTracking) {
    const Settings s{};
    const Vec3 on{0.0f, 0.0f, 0.14f};
    const auto grabbed = [&](Tracker& t) {
        Output o;
        t.Update(s, In(on, 0.0f), o);
        t.Update(s, In(on, 0.9f), o);
        return o.engaged;
    };
    {
        Tracker t;
        ASSERT_TRUE(grabbed(t));
        Input in = In(on, 0.9f);
        in.two_handed = false; // a transform back to one hand, a weapon switch
        Output o;
        t.Update(s, in, o);
        EXPECT_TRUE(o.released == Why::Form);
        EXPECT_TRUE(o.owns_grip);
    }
    {
        Tracker t;
        ASSERT_TRUE(grabbed(t));
        Input in = In(on, 0.9f);
        in.busy = true; // X armed a vial
        Output o;
        t.Update(s, in, o);
        EXPECT_TRUE(o.released == Why::Busy);
    }
    {
        Tracker t;
        ASSERT_TRUE(grabbed(t));
        Input in = In(on, 0.9f);
        in.gameplay = false;
        Output o;
        t.Update(s, in, o);
        EXPECT_TRUE(o.released == Why::Context);
    }
    {
        // A hand occluded for 0.3 s keeps the grip (and no apart / twist check on inferred
        // positions); the support hand stays attached while the weapon hand is tracked.
        Tracker t;
        ASSERT_TRUE(grabbed(t));
        Output o;
        Input in = In({0.5f, 0.0f, 0.5f}, 0.9f); // an inferred position far away
        in.left_tracked = false;
        for (int i = 0; i < 27; ++i) { // 0.3 s
            t.Update(s, in, o);
        }
        EXPECT_TRUE(o.engaged);
        t.Update(s, In(on, 0.9f), o);
        EXPECT_TRUE(o.engaged);
        for (int i = 0; i < 90; ++i) {
            t.Update(s, in, o);
        }
        EXPECT_TRUE(o.engaged);
        in.right_tracked = false;
        for (int i = 0; i < 50; ++i) {
            t.Update(s, in, o);
        }
        EXPECT_FALSE(o.engaged);
    }
}

TEST(TwoHandRelease, ADropKeepsTheGripAndNeverRegrabs) {
    // A gap in the gameplay input (a menu, a long hitch) lets go at once; the grip still held is
    // no new squeeze: no grab again until it is let go and squeezed anew, and no arc meanwhile.
    const Settings s{};
    const Vec3 on{0.0f, 0.0f, 0.14f};
    Tracker t;
    Output o;
    t.Update(s, In(on, 0.0f), o);
    t.Update(s, In(on, 0.9f), o);
    ASSERT_TRUE(o.engaged);
    EXPECT_TRUE(t.Drop());
    EXPECT_FALSE(t.Drop());
    for (int i = 0; i < 10; ++i) {
        t.Update(s, In(on, 0.9f), o);
        EXPECT_FALSE(o.engaged);
        EXPECT_FALSE(o.grabbed);
        EXPECT_TRUE(o.owns_grip);
        EXPECT_NEAR(o.weight, 0.0f, 1e-6f);
    }
    t.Update(s, In(on, 0.1f), o);
    EXPECT_FALSE(o.owns_grip);
    t.Update(s, In(on, 0.9f), o);
    EXPECT_TRUE(o.grabbed);
}

TEST(TwoHandRelease, ADropWithoutAGrabLeavesTheGripToTheQuickItems) {
    // A long hitch (> 1 s, UpdateTwoHand's Drop) with the left grip held for something else (a
    // throwable held with the grip in left_scheme=1, a squeeze off the handle): the grip is not
    // the grab's, so the quick items keep seeing it held (no involuntary release or throw).
    const Settings s{};
    const Vec3 off{0.30f, 0.0f, 0.14f};
    Tracker t;
    Output o;
    t.Update(s, In(off, 0.0f), o);
    t.Update(s, In(off, 0.9f), o);
    ASSERT_FALSE(o.engaged);
    ASSERT_FALSE(o.owns_grip);
    EXPECT_FALSE(t.Drop());
    t.Update(s, In(off, 0.9f), o);
    EXPECT_FALSE(o.owns_grip);
    EXPECT_FALSE(o.engaged);
    // Released for a context change with the grip still held: still owned after a drop.
    const Vec3 on{0.0f, 0.0f, 0.14f};
    Tracker u;
    u.Update(s, In(on, 0.0f), o);
    u.Update(s, In(on, 0.9f), o);
    ASSERT_TRUE(o.engaged);
    Input menu = In(on, 0.9f);
    menu.gameplay = false;
    u.Update(s, menu, o);
    ASSERT_EQ(o.released, Why::Context);
    EXPECT_TRUE(o.owns_grip);
    EXPECT_FALSE(u.Drop());
    u.Update(s, In(on, 0.9f), o);
    EXPECT_TRUE(o.owns_grip);
    EXPECT_FALSE(o.grabbed);
}

TEST(TwoHandBlend, EasesOntoAndOffTheHandle) {
    Settings s{};
    s.blend_s = 0.1f;
    Tracker t;
    Output o;
    const Vec3 on{0.0f, 0.0f, 0.14f};
    Input in = In(on, 0.0f);
    in.dt = 0.02f;
    t.Update(s, in, o);
    EXPECT_NEAR(o.weight, 0.0f, 1e-6f);
    in.grip = 0.9f;
    t.Update(s, in, o);
    EXPECT_GT(o.weight, 0.0f);
    EXPECT_LT(o.weight, 0.2f);
    for (int i = 0; i < 4; ++i) {
        t.Update(s, in, o);
    }
    EXPECT_NEAR(o.weight, 1.0f, 1e-5f);
    in.grip = 0.0f;
    t.Update(s, in, o);
    EXPECT_GT(o.weight, 0.8f); // eases off, no snap
    for (int i = 0; i < 4; ++i) {
        t.Update(s, in, o);
    }
    EXPECT_NEAR(o.weight, 0.0f, 1e-5f);
}

TEST(TwoHandGame, FormsAndPlausibleGrips) {
    EXPECT_TRUE(TwoHandedForm(81, 3));  // Ludwig's Holy Blade B
    EXPECT_TRUE(TwoHandedForm(80, 3));  // Kirkhammer B
    EXPECT_TRUE(TwoHandedForm(50, 3));  // Hunter Axe B
    EXPECT_FALSE(TwoHandedForm(81, 1)); // form A: the gun hand
    EXPECT_FALSE(TwoHandedForm(70, 5)); // Saw Cleaver B: one-handed
    EXPECT_FALSE(TwoHandedForm(270, 3)); // Rakuyo B: two blades
    EXPECT_FALSE(TwoHandedForm(40, 3));  // Blade of Mercy B
    EXPECT_FALSE(TwoHandedForm(320, 3)); // Simon's Bowblade B: the bow
    EXPECT_FALSE(TwoHandedForm(-1, 3));
    EXPECT_TRUE(Plausible(DefaultGrip, 1.0f));
    EXPECT_FALSE(Plausible({true, {0.0f, 0.03f, 0.0f}, {}}, 1.0f));  // on the right fist
    EXPECT_FALSE(Plausible({true, {0.0f, 0.90f, 0.0f}, {}}, 1.0f));  // far off
    EXPECT_FALSE(Plausible({true, {0.20f, 0.08f, 0.0f}, {}}, 1.0f)); // not along the handle
    EXPECT_TRUE(Plausible({true, {0.0f, -0.25f, 0.05f}, {}}, 1.0f)); // the other side
    EXPECT_FALSE(Plausible({false, {0.0f, 0.2f, 0.0f}, {}}, 1.0f));
    EXPECT_TRUE(Plausible({true, {0.0f, 0.40f, 0.0f}, {}}, 2.0f)); // 20 cm at scale 2
    GripFilter f;
    f.Add({true, {0.0f, 0.20f, 0.0f}, {}}, Dt);
    EXPECT_NEAR(f.grip.t.y, 0.20f, 1e-6f);
    for (int i = 0; i < 90; ++i) {
        f.Add({true, {0.0f, 0.30f, 0.0f}, {}}, Dt);
    }
    EXPECT_NEAR(f.grip.t.y, 0.30f, 0.005f); // 1 s at tau 0.3 s
    EXPECT_EQ(f.readings, 91u);
}

TEST(TwoHandSwing, BothHandsMovingEarnTheBonus) {
    const Settings s{};
    SwingMark m;
    m.Update(7, true, true, 0.4f);
    m.Update(7, true, true, 1.3f);
    m.Update(7, false, true, 0.0f); // closed: stays marked
    EXPECT_TRUE(m.TwoHanded(s));
    EXPECT_NEAR(m.Factor(s), 1.05f, 1e-6f);
    m.Update(8, true, false, 2.0f); // a new window, one-handed
    EXPECT_FALSE(m.TwoHanded(s));
    EXPECT_NEAR(m.Factor(s), 1.0f, 1e-6f);
    m.Update(9, true, true, 0.3f); // held, but the left hand barely moved: a pivot
    EXPECT_FALSE(m.TwoHanded(s));
    m.Update(9, true, true, 0.9f);
    EXPECT_TRUE(m.TwoHanded(s));
    Settings big = s;
    big.damage = 5.0f;
    EXPECT_NEAR(m.Factor(big), 2.0f, 1e-6f);
    m.Update(0, false, true, 3.0f);
    EXPECT_FALSE(m.TwoHanded(s));
}

// The feel review (2026-10-06, two_hand_relative): a grab with the left hand off the handle's line
// turns nothing (the absolute rule turned the blade by that offset); moving the left hand round
// the right fist then steers the weapon by as much; the twist release counts from the grab's line.
TEST(TwoHandRelative, NoJumpAtTheGrabAndTheLeftHandSteers) {
    namespace B = VR::Body;
    const Settings s{};
    ASSERT_TRUE(s.relative);
    const Pose right{{0.1f, 1.1f, -0.4f}, B::AxisAngle({1.0f, 0.0f, 0.0f}, 0.3f)};
    const Handle h = HandleAt(right, R0, DefaultGrip, 1.0f);
    // The left hand 6 cm to the side of the game's grip point 14 cm down: 23 deg off the line.
    const Vec3 left_bone = h.left + Vec3{0.06f, 0.0f, 0.0f};
    Input in = In(left_bone, 0.0f, h);
    in.right_q_ok = true;
    in.right_q = right.q;
    Tracker t;
    Output o;
    t.Update(s, in, o);
    in.grip = 0.9f;
    t.Update(s, in, o);
    ASSERT_TRUE(o.grabbed);
    ASSERT_TRUE(o.ref_ok);
    EXPECT_NEAR(o.twist_deg, 0.0f, 1e-3f);
    const Pose left{left_bone, {}};
    const Applied abs = Apply(right, left, R0, L0, DefaultGrip, 1.0f, 1.0f);
    EXPECT_NEAR(abs.turn_deg, std::atan2(0.06f, 0.14f) / Deg, 0.05f);
    const Applied rel = Apply(right, left, R0, L0, DefaultGrip, 1.0f, 1.0f, &o.ref_local);
    EXPECT_NEAR(rel.turn_deg, 0.0f, 0.01f);
    EXPECT_GT(std::abs(B::Dot(rel.right.q, B::Normalize(right.q))), 0.99999f);
    EXPECT_LT(Dist(BoneOrigin(rel.left, L0), h.left), 1e-5f); // his fist on the handle
    // The left hand 20 deg round the right fist: the weapon turns 20 deg, keeping its 23 deg to
    // the hands' line; the right fist stays.
    const Vec3 d = left_bone - h.right;
    const Vec3 axis = B::Normalize(B::Cross(d, Vec3{0.0f, 1.0f, 0.0f}));
    const Vec3 moved = h.right + B::RotateAbout(d, axis, 20.0f * Deg);
    const Applied steer = Apply(right, {moved, {}}, R0, L0, DefaultGrip, 1.0f, 1.0f, &o.ref_local);
    EXPECT_NEAR(steer.turn_deg, 20.0f, 0.05f);
    const Handle h2 = HandleAt(steer.right, R0, DefaultGrip, 1.0f);
    EXPECT_LT(Dist(h2.right, h.right), 1e-5f);
    EXPECT_NEAR(B::Angle(h2.dir, moved - h.right), B::Angle(h.dir, d), 1e-3f);
    // Large steering angles stay held; the grip buttons own release.
    Input in70 = in;
    in70.left_bone = h.right + B::RotateAbout(d, axis, 70.0f * Deg);
    t.Update(s, in70, o);
    EXPECT_TRUE(o.engaged);
    EXPECT_NEAR(o.twist_deg, 70.0f, 0.1f);
    Input in80 = in;
    in80.left_bone = h.right + B::RotateAbout(d, axis, 80.0f * Deg);
    t.Update(s, in80, o);
    EXPECT_TRUE(o.engaged);
    EXPECT_EQ(o.released, Why::None);
    in80.grip = 0.0f;
    t.Update(s, in80, o);
    EXPECT_EQ(o.released, Why::Grip);
    // The grab's line is kept for the ease off the handle.
    EXPECT_TRUE(o.ref_ok);
    // two_hand_relative=0 (or no right orientation): no grab line, the old rule.
    Settings off = s;
    off.relative = false;
    Tracker t2;
    Output o2;
    in.grip = 0.0f;
    t2.Update(off, in, o2);
    in.grip = 0.9f;
    t2.Update(off, in, o2);
    ASSERT_TRUE(o2.grabbed);
    EXPECT_FALSE(o2.ref_ok);
    EXPECT_NEAR(o2.twist_deg, std::atan2(0.06f, 0.14f) / Deg, 0.05f);
}

} // namespace

TEST(TwoHandCalibration, NativeOffsetsSurviveWeaponChangesAndRejectTransitions) {
    VR::TwoHand::GripCache cache;
    using namespace VR::TwoHand;
    const GameGrip wheel{true, {0.22f, 0.08f, 0.02f}, VR::Body::AxisAngle({1, 0, 0}, 0.7f)};
    bool measured = false;
    cache.Select(120, 3, 0.01f);
    for (int i = 0; i < 20; ++i) cache.Observe(wheel, 1.0f, 0.01f, true);
    cache.Get(1.0f, measured);
    EXPECT_FALSE(measured); // transition settling is time based
    cache.Select(120, 3, 0.4f);
    for (int i = 0; i < 5; ++i) cache.Observe(wheel, 1.0f, 0.01f, true);
    const auto learned = cache.Get(1.0f, measured);
    ASSERT_TRUE(measured);
    EXPECT_NEAR(VR::Body::Length(learned.t - wheel.t), 0.0f, 1e-5f);
    EXPECT_NEAR(VR::Body::AngleBetween(learned.q, wheel.q), 0.0f, 1e-3f);
    cache.Select(81, 3, 0.4f);
    const auto fallback = cache.Get(2.0f, measured);
    EXPECT_FALSE(measured);
    EXPECT_NEAR(VR::Body::Length(fallback.t) / 2.0f, 0.14f, 1e-5f);
    const GameGrip sword{true, {0, 0.12f, 0}, {}};
    for (int i = 0; i < 5; ++i) cache.Observe(sword, 1.0f, 0.01f, true);
    cache.Select(120, 3, 0.01f);
    EXPECT_NEAR(VR::Body::Length(cache.Get(1.0f, measured).t - wheel.t), 0.0f, 1e-5f);
    EXPECT_TRUE(measured);
    // An item/attack pose cannot overwrite a learned grip, even if its position is plausible.
    for (int i = 0; i < 40; ++i) cache.Observe(sword, 1.0f, 0.01f, false);
    EXPECT_NEAR(VR::Body::Length(cache.Get(1.0f, measured).t - wheel.t), 0.0f, 1e-5f);
}

TEST(TwoHandSettings, GuidePersistsAndDefaultsOn) {
    namespace T = VR::TwoHand;
    const auto saved = T::GetTwoHandSettings();
    EXPECT_TRUE(T::Settings{}.guide);
    ASSERT_TRUE(T::ParseTwoHandSetting("two_hand_guide", "0"));
    EXPECT_FALSE(T::GetTwoHandSettings().guide);
    ASSERT_TRUE(T::ParseTwoHandSetting("two_hand_guide", "true"));
    EXPECT_TRUE(T::GetTwoHandSettings().guide);
    T::GetTwoHandSettings() = saved;
}

TEST(TwoHandGuide, OnlyAnAvailableHandleIsShown) {
    auto in = In({0, 0, 0.14f}, 0.0f);
    Settings s{};
    EXPECT_TRUE(GuideAvailable(s, in, false));
    EXPECT_FALSE(GuideAvailable(s, in, true));
    in.busy = true;
    EXPECT_FALSE(GuideAvailable(s, in, false));
    in.busy = false;
    in.two_handed = false;
    EXPECT_FALSE(GuideAvailable(s, in, false));
    in.grab_transforms = true;
    EXPECT_TRUE(GuideAvailable(s, in, false));
    in.blocked = true;
    EXPECT_FALSE(GuideAvailable(s, in, false));
    in.blocked = false;
    s.guide = false;
    EXPECT_FALSE(GuideAvailable(s, in, false));
}


TEST(TwoHandGrip, RetainedResqueezeStaysOwnedAfterEveryForcedRelease) {
    for (const auto why : {Why::Form, Why::Off, Why::Context, Why::Busy, Why::Tracking}) {
        SCOPED_TRACE(static_cast<int>(why));
        Settings s{};
        Tracker tracker;
        Output out;
        auto in = In(H0().left, 0.0f);
        in.family = 81;
        in.right_grip = 1.0f;
        tracker.Update(s, in, out);
        in.grip = 1.0f;
        tracker.Update(s, in, out);
        ASSERT_TRUE(out.grabbed);
        in.grip = 0.0f;
        tracker.Update(s, in, out);
        ASSERT_TRUE(out.engaged);
        // Re-squeeze on the very frame the hold is forced to drop.
        in.grip = 1.0f;
        switch (why) {
        case Why::Form: in.family = 50; break;
        case Why::Off: in.enabled = false; break;
        case Why::Context: in.gameplay = false; break;
        case Why::Busy: in.busy = true; break;
        case Why::Tracking: in.right_tracked = false; in.dt = s.lost_s + 0.01f; break;
        default: break;
        }
        tracker.Update(s, in, out);
        EXPECT_EQ(out.released, why);
        EXPECT_FALSE(out.engaged);
        EXPECT_TRUE(out.owns_grip); // quick items still receive zero grip
        for (int i = 0; i < 5; ++i) {
            tracker.Update(s, in, out);
            EXPECT_TRUE(out.owns_grip);
            EXPECT_FALSE(out.grabbed);
        }
        in.grip = 0.0f;
        tracker.Update(s, in, out);
        EXPECT_FALSE(out.owns_grip);
        // A later deliberate squeeze away from the handle belongs to quick items again.
        in.enabled = in.gameplay = in.right_tracked = true;
        in.busy = false;
        in.left_bone = {1.0f, 0.0f, 0.0f};
        in.grip = 1.0f;
        tracker.Update(s, in, out);
        EXPECT_FALSE(out.owns_grip);
    }
}

TEST(TwoHandSupport, NewCalibrationNeverPullsTheControllerUpTheHandle) {
    SupportGrip support;
    Settings settings;
    SupportGrip::Input in;
    in.held = in.tracked = true;
    in.family = 50;
    in.right_bone = R0;
    in.left_bone = L0;
    in.target = DefaultGrip;
    in.left.p = H0().left;
    // The first grab opens the weapon, but fallback calibration cannot constrain the hand.
    for (int i = 0; i < 30; ++i) {
        EXPECT_FALSE(support.Update(settings, in, Dt));
        const auto pose = support.ApplyTracked(in.right, in.left, R0, L0, Dt);
        EXPECT_LT(Dist(pose.left.p, in.left.p), 1e-6f);
        EXPECT_LT(VR::Body::AngleBetween(pose.right.q, in.right.q), 1e-4f);
        EXPECT_EQ(support.Weight(), 0.0f);
    }
    in.ready = true;
    in.target = {true, {0, 0.40f, 0}, VR::Body::AxisAngle({1, 0, 0}, 120.0f * Deg)};
    const Handle target = HandleAt(in.right, R0, in.target, 1.0f);
    ASSERT_GT(Dist(target.left, in.left.p), 0.25f);
    ASSERT_TRUE(support.Update(settings, in, Dt)); // grab here, not at the native 40 cm point
    for (int i = 0; i < 180; ++i) {
        support.Update(settings, in, Dt);
        const auto pose = support.ApplyTracked(in.right, in.left, R0, L0, Dt);
        EXPECT_LT(Dist(pose.left.p, in.left.p), 1e-6f);
    }
    EXPECT_NEAR(support.Grip().t.y, 0.14f, 1e-6f);
    // The controller, not the animated hand, moves to the final grip.
    in.left.p = target.left;
    in.left.q = VR::Body::AxisAngle({0, 0, 1}, 0.7f);
    for (int i = 0; i < 90; ++i) {
        support.Update(settings, in, Dt);
        support.ApplyTracked(in.right, in.left, R0, L0, Dt);
    }
    ASSERT_TRUE(support.Seated());
    ASSERT_FLOAT_EQ(support.Weight(), 1.0f);
    auto pose = support.ApplyTracked(in.right, in.left, R0, L0, Dt);
    EXPECT_LT(Dist(pose.left.p, in.left.p), 0.0021f);
    EXPECT_LT(VR::Body::AngleBetween(pose.left.q, in.left.q), 1e-3f);
    // Sliding does not steer the shaft; the deliberate left wrist twist contributes 30% roll.
    EXPECT_LT(VR::Body::AngleBetween(pose.right.q,
        VR::Body::AxisAngle({0, 0, 1}, 0.7f * 0.3f)), 1e-3f);
    // Later native animation cannot move an acquired point or rewrite the steering reference.
    in.target.t = {0.25f, -0.25f, 0};
    support.Update(settings, in, Dt);
    pose = support.ApplyTracked(in.right, in.left, R0, L0, Dt);
    EXPECT_LT(Dist(pose.left.p, in.left.p), 0.0021f);
    EXPECT_NEAR(support.Grip().t.y, 0.40f, 0.0021f);
    in.held = false;
    for (int i = 0; i < 20; ++i) support.Update(settings, in, Dt);
    EXPECT_FALSE(support.Seated());
    EXPECT_FLOAT_EQ(support.Weight(), 0.0f);
}

TEST(TwoHandSupport, CachedRailSurvivesUntransformAndAllowsEitherHandInFront) {
    GripCache cache;
    const GameGrip native{true, {0, -0.4f, 0}, {}};
    cache.Select(50, 3, 0.4f);
    for (int i = 0; i < 5; ++i) cache.Observe(native, 1.0f, Dt, true);
    cache.Select(50, 1, Dt);
    bool measured = false;
    const auto target = cache.Get(1.0f, measured);
    ASSERT_TRUE(measured);
    EXPECT_LT(Dist(target.t, native.t), 1e-6f);
    Handle handle = HandleAt({}, R0, target, 1.0f);
    handle.blade_clear = 0.30f; // a haft: bare shaft on the other side of the fist too
    EXPECT_TRUE(ZoneOf(handle, H0().left, 0.09f).inside);
    // Continuous grasp positions, not just the animation's native hand point.
    EXPECT_TRUE(ZoneOf(handle, handle.left * 0.5f, 0.09f).inside);
    EXPECT_TRUE(ZoneOf(handle, handle.left, 0.09f).inside);
}

TEST(TwoHandSupport, RealWristAndSteeringRemainAlignedWithCurvedScaledAndTrimmedGrips) {
    namespace B = VR::Body;
    for (float scale : {0.8f, 1.0f, 1.4f}) {
        for (Vec3 point : {Vec3{0, 0.4f, 0}, Vec3{0, -0.3f, 0}, Vec3{0.22f, 0.08f, 0.02f}}) {
            SupportGrip support;
            Settings settings;
            SupportGrip::Input in;
            in.held = in.ready = in.tracked = true;
            in.family = 120;
            in.scale = scale;
            in.target = {true, point * scale, B::AxisAngle({1, 0, 0}, 2.0f)};
            in.right_bone = {B::DefaultGrip(1, 20, 5, -8, 12), {0.01f, 0.02f, -0.01f}};
            in.left_bone = {B::DefaultGrip(0, 20), {-0.015f, 0.01f, 0.02f}};
            in.right = {{0.2f, 1.1f, -0.4f}, B::AxisAngle({0, 1, 0}, 0.4f)};
            in.left.q = B::AxisAngle({1, 0, 0}, -0.8f);
            const Handle h = HandleAt(in.right, in.right_bone, in.target, scale);
            in.left.p = h.left - B::Rotate(in.left.q, in.left_bone.offset);
            ASSERT_TRUE(support.Update(settings, in, Dt));
            for (int i = 0; i < 20; ++i) support.Update(settings, in, Dt);
            const auto at = support.ApplyTracked(in.right, in.left, in.right_bone, in.left_bone, Dt);
            EXPECT_LT(Dist(at.left.p, in.left.p), 1e-5f);
            EXPECT_LT(B::AngleBetween(at.left.q, in.left.q), 1e-3f);
            EXPECT_LT(B::AngleBetween(at.right.q, in.right.q), 1e-3f);
            // Sweep the real hand around the right fist, independently rolling the wrist.
            const auto turn = B::AxisAngle({0, 1, 0}, 0.4f);
            in.left.q = B::AxisAngle({0, 0, 1}, -1.2f);
            const auto bone = h.right + B::Rotate(turn, h.left - h.right);
            in.left.p = bone - B::Rotate(in.left.q, in.left_bone.offset);
            for (int i = 0; i < 90; ++i) {
                support.Update(settings, in, Dt);
                support.ApplyTracked(in.right, in.left, in.right_bone, in.left_bone, Dt);
            }
            const auto moved = support.ApplyTracked(in.right, in.left, in.right_bone, in.left_bone, Dt);
            EXPECT_LT(Dist(moved.left.p, in.left.p), 0.0021f);
            EXPECT_LT(B::AngleBetween(moved.left.q, in.left.q), 1e-3f);
            const auto placed = Place(in.right, in.left, in.right_bone, in.left_bone,
                                      support.Grip(), support.Scale(), 1, moved.turn_local);
            EXPECT_LT(Dist(placed.left.p, moved.left.p), 1e-5f);
            EXPECT_LT(B::AngleBetween(placed.left.q, moved.left.q), 1e-3f);
        }
    }
}

TEST(TwoHandSupport, CannotGrabWhileTransformingUntrackedOrOutsideRail) {
    SupportGrip support;
    Settings settings;
    SupportGrip::Input in;
    in.held = in.ready = in.tracked = true;
    in.family = 50;
    in.target = DefaultGrip;
    in.right_bone = R0;
    in.left_bone = L0;
    in.left.p = H0().left + Vec3{0.12f, 0, 0};
    EXPECT_FALSE(support.Update(settings, in, Dt));
    EXPECT_EQ(support.Weight(), 0);
    in.left.p = H0().left;
    in.ready = false;
    EXPECT_FALSE(support.Update(settings, in, Dt));
    in.ready = true;
    in.tracked = false;
    EXPECT_FALSE(support.Update(settings, in, Dt));
    in.tracked = true;
    ASSERT_TRUE(support.Update(settings, in, Dt));
    in.family = 81;
    in.target.t = {0, 0.5f, 0};
    in.left.p.x += 0.2f;
    EXPECT_FALSE(support.Update(settings, in, Dt));
    EXPECT_FALSE(support.Seated());
    EXPECT_EQ(support.Weight(), 0);
}

TEST(TwoHandGuide, TransformedMoonlightHoldNeverDispatchesTransform) {
    ASSERT_EQ(VR::Tricks::GripUseOf(260, true), VR::Tricks::GripUse::Transform);
    GuideGesture gesture;
    auto in = In(H0().left, 0.0f);
    in.family = 260;
    Settings settings;
    for (int i = 0; i < 90; ++i) {
        const auto out = gesture.Step(true, HandleAvailable(settings, in, false), 260, Dt);
        EXPECT_TRUE(out.owns); // suppress the normal right-grip transform edge
        EXPECT_FALSE(out.tap);
    }
    EXPECT_FALSE(gesture.Step(false, true, 260, Dt).tap);
    // A deliberate short tap transforms exactly once, on release.
    EXPECT_TRUE(gesture.Step(true, true, 260, Dt).owns);
    EXPECT_TRUE(gesture.Step(false, true, 260, Dt).tap);
    EXPECT_FALSE(gesture.Step(false, true, 260, Dt).tap);
    settings.guide = false;
    EXPECT_TRUE(HandleAvailable(settings, in, false)); // visual toggle preserves controls
    EXPECT_FALSE(GuideAvailable(settings, in, false));
}

TEST(TwoHandGuide, GrabOrContextChangeCancelsPendingTapUntilNewSqueeze) {
    for (int kind = 0; kind < 3; ++kind) {
        GuideGesture gesture;
        EXPECT_TRUE(gesture.Step(true, true, 260, Dt).owns);
        if (kind == 0) {
            // Acquired support hand makes HandleAvailable false, even if dropped again.
            gesture.Step(true, false, 260, Dt);
        } else if (kind == 1) {
            gesture.Step(true, true, 50, Dt); // weapon switch
        } else {
            gesture.Reset(true); // menu/focus/layout switch
        }
        EXPECT_FALSE(gesture.Step(true, true, 260, Dt).tap);
        EXPECT_FALSE(gesture.Step(false, true, 260, Dt).tap);
        EXPECT_TRUE(gesture.Step(true, true, 260, Dt).owns);
        EXPECT_TRUE(gesture.Step(false, true, 260, Dt).tap);
    }
    GuideGesture gesture;
    EXPECT_FALSE(gesture.Step(true, false, 260, Dt).owns);
    EXPECT_FALSE(gesture.Step(true, true, 260, Dt).owns); // held through unavailable context
    EXPECT_FALSE(gesture.Step(false, true, 260, Dt).tap);
    gesture.Step(true, true, 260, Dt);
    EXPECT_FALSE(gesture.Step(false, true, 260, 1.0f).tap); // hitch cannot become a short tap
}


TEST(TwoHandSliding, PassingHandsAndMovingDominantHandCannotFlipOrTranslateTheAnchor) {
    namespace B = VR::Body;
    for (int hz : {72, 90, 120}) {
        SCOPED_TRACE(hz);
        const float dt = 1.0f / hz;
        SupportGrip s;
        Settings cfg;
        SupportGrip::Input in;
        in.held = in.ready = in.tracked = true;
        in.family = 50;
        in.target = {true, {0, 0.4f, 0}, {}};
        in.blade_clear = 0.30f; // a haft: either hand can lead
        in.right_bone = R0;
        in.left_bone = L0;
        in.left.p = {0, 0, 0.3f};
        ASSERT_TRUE(s.Update(cfg, in, dt));
        for (int i = 0; i < hz; ++i) {
            s.Update(cfg, in, dt);
            s.ApplyTracked(in.right, in.left, R0, L0, dt);
        }
        for (int i = 0; i <= 2 * hz; ++i) {
            // Left passes the stationary right, then the right slides past the stationary left.
            const float k = static_cast<float>(i) / hz;
            if (i <= hz) in.left.p.z = 0.3f - k * 0.5f;
            else in.right.p.z = -(k - 1.0f) * 0.4f;
            s.Update(cfg, in, dt);
            const auto a = s.ApplyTracked(in.right, in.left, R0, L0, dt);
            EXPECT_LT(B::AngleBetween(a.right.q, in.right.q), 1e-3f);
            EXPECT_LT(Dist(BoneOrigin(a.right, R0), BoneOrigin(in.right, R0)), 1e-6f);
            EXPECT_LT(Dist(a.left.p, in.left.p), 0.014f); // small sliding filter lag only
            EXPECT_TRUE(s.Seated());
        }
        EXPECT_GT(s.Slide(), 0.15f);
        // End limits never pull the dominant controller or drop the retained grab.
        in.left.p.z = 3.0f;
        for (int i = 0; i < hz; ++i) {
            s.Update(cfg, in, dt);
            const auto a = s.ApplyTracked(in.right, in.left, R0, L0, dt);
            EXPECT_LT(Dist(a.right.p, in.right.p), 1e-6f);
        }
        // Both hands on the rail: the right one slid up to its blade-side end as it passed the
        // left (the moving hand slides), the left one at the pommel end.
        EXPECT_GE(s.RightSlide(), -0.30f - 1e-5f);
        EXPECT_LE(s.RightSlide() + s.Slide(), 0.44f + 1e-5f);
        EXPECT_TRUE(s.Seated());
    }
}

TEST(TwoHandSliding, OffCentreStraightSlideDoesNotSteerAndJitterDoesNotCreep) {
    SupportGrip s;
    Settings cfg;
    SupportGrip::Input in;
    in.held = in.ready = in.tracked = true;
    in.family = 50;
    in.target = {true, {0, 0.4f, 0}, {}};
    in.blade_clear = 0.30f; // a haft: either hand can lead
    in.right_bone = R0;
    in.left_bone = L0;
    in.left.p = {0.025f, 0, 0.2f};
    ASSERT_TRUE(s.Update(cfg, in, Dt));
    for (int i = 0; i < 180; ++i) {
        in.left.p.z = 0.2f + 0.2f * i / 179.0f;
        s.Update(cfg, in, Dt);
        const auto a = s.ApplyTracked(in.right, in.left, R0, L0, Dt);
        EXPECT_LT(VR::Body::AngleBetween(a.right.q, in.right.q), 1e-3f);
    }
    for (int i = 0; i < 90; ++i) {
        s.Update(cfg, in, Dt);
        s.ApplyTracked(in.right, in.left, R0, L0, Dt);
    }
    const float initial = s.Slide();
    for (int i = 0; i < 180; ++i) {
        in.left.p.z = initial + (i % 2 ? 0.001f : -0.001f);
        s.Update(cfg, in, Dt);
        s.ApplyTracked(in.right, in.left, R0, L0, Dt);
    }
    EXPECT_FLOAT_EQ(s.Slide(), initial);
}

TEST(TwoHandSliding, GrabbingBehindTheRightHandStartsWithoutReversingTheWeapon) {
    SupportGrip s;
    Settings cfg;
    SupportGrip::Input in;
    in.held = in.ready = in.tracked = true;
    in.family = 50;
    in.target = {true, {0, 0.4f, 0}, {}};
    in.blade_clear = 0.30f; // a haft: either hand can lead
    in.right_bone = R0;
    in.left_bone = L0;
    in.left.p = {0, 0, -0.2f};
    ASSERT_TRUE(s.Update(cfg, in, Dt));
    for (int i = 0; i < 90; ++i) {
        s.Update(cfg, in, Dt);
        const auto a = s.ApplyTracked(in.right, in.left, R0, L0, Dt);
        EXPECT_LT(VR::Body::AngleBetween(a.right.q, in.right.q), 1e-3f);
        EXPECT_LT(Dist(a.left.p, in.left.p), 1e-6f);
    }
    EXPECT_LT(s.Slide(), 0);
}


namespace {
struct SteeringRig {
    SupportGrip support;
    Settings cfg;
    SupportGrip::Input in;
    float dt;
    Applied last{};

    explicit SteeringRig(int hz) : dt(1.0f / hz) {
        in.held = in.ready = in.tracked = true;
        in.family = 50;
        in.target = {true, {0, 0.4f, 0}, {}};
        in.blade_clear = 0.30f; // a haft: either hand can lead
        in.right_bone = R0;
        in.left_bone = L0;
        for (int i = 0; i < hz; ++i) Step(0);
    }
    Applied Step(float degrees, float radius = 0.3f) {
        in.left.p = VR::Body::Rotate(VR::Body::AxisAngle({0, 1, 0}, degrees * Deg),
                                    {0, 0, radius});
        support.Update(cfg, in, dt);
        return last = support.ApplyTracked(in.right, in.left, R0, L0, dt);
    }
};
}

TEST(TwoHandSteering, FastLeverSwingsAndReversalsStayCloseToHands) {
    namespace B = VR::Body;
    for (int hz : {72, 90, 120}) {
        SCOPED_TRACE(hz);
        SteeringRig rig(hz);
        float peak_error = 0, total_error = 0;
        for (int i = 1; i <= 2 * hz; ++i) {
            // Triangle wave: 600 degrees/s, reversing at +/-60 degrees. The right
            // wrist stays still, so the entire lever swing must pass through steering.
            const float phase = std::fmod(i * rig.dt, 0.4f);
            const float angle = phase <= 0.1f ? 600 * phase :
                                phase <= 0.3f ? 120 - 600 * phase : 600 * phase - 240;
            const auto a = rig.Step(angle);
            const float error = B::AngleBetween(a.right.q,
                B::AxisAngle({0, 1, 0}, angle * Deg)) / Deg;
            peak_error = std::max(peak_error, error);
            total_error += error;
            EXPECT_LT(Dist(BoneOrigin(a.right, R0), BoneOrigin(rig.in.right, R0)), 1e-6f);
        }
        std::printf("  steering %d Hz: peak %.2f deg, mean %.2f deg at 600 deg/s\n",
                    hz, peak_error, total_error / (2 * hz));
        EXPECT_LT(peak_error, 6.0f);
        EXPECT_LT(total_error / (2 * hz), 3.5f);
    }
}

TEST(TwoHandSteering, StationaryNoiseIsSuppressedAfterASwing) {
    namespace B = VR::Body;
    for (int hz : {72, 90, 120}) {
        SCOPED_TRACE(hz);
        SteeringRig rig(hz);
        for (int i = 0; i < hz / 10; ++i) rig.Step(600 * i * rig.dt);
        for (int i = 0; i < hz; ++i) rig.Step(0);
        float peak = 0;
        for (int i = 0; i < hz; ++i) {
            const auto a = rig.Step(i % 2 ? 0.35f : -0.35f);
            peak = std::max(peak, B::AngleBetween(a.right.q, {}) / Deg);
        }
        std::printf("  aiming %d Hz: peak %.3f deg for +/-0.35 deg noise\n", hz, peak);
        EXPECT_LT(peak, 0.13f); // +/-0.35-degree input noise remains visibly attenuated.
    }
}

TEST(TwoHandSteering, TrackingRecoveryAndOverlapDoNotCountAsFastMotion) {
    namespace B = VR::Body;
    for (int hz : {72, 90, 120}) {
        for (int gap : {0, 1, 2, 3, 4, 5}) {
            SCOPED_TRACE(hz);
            SCOPED_TRACE(gap);
            SteeringRig rig(hz);
            for (int i = 1; i <= hz / 12; ++i) rig.Step(600 * i * rig.dt);
            if (gap == 1) {
                const Quat before = rig.last.right.q;
                const auto a = rig.Step(-60, 0.01f);
                EXPECT_LT(B::AngleBetween(before, a.right.q), 1e-3f);
            } else if (gap == 2) {
                rig.Step(45, 0.045f); // partially faded steering must also discard speed
            } else if (gap >= 3) {
                const float dt = gap == 3 ? 0.0f : gap == 4 ?
                    std::numeric_limits<float>::quiet_NaN() : 0.2f;
                rig.support.Update(rig.cfg, rig.in, dt);
            } else {
                rig.in.tracked = false;
                rig.support.Update(rig.cfg, rig.in, rig.dt);
                rig.in.tracked = true;
            }
            const Quat before = rig.last.right.q;
            const auto a = rig.Step(60);
            const float remaining = B::AngleBetween(before, B::AxisAngle({0, 1, 0}, 60 * Deg));
            const float correction = B::AngleBetween(before, a.right.q);
            // Resume gently (under a third of the gap in one frame), instead of treating
            // the jump as high-speed steering. Keep this a behavioral bound, not a formula.
            // (Overlapping hands are a crossing: the hand's offset there, 1 cm, becomes its new
            // grab offset, a fraction of a degree at 30 cm.)
            EXPECT_LT(correction, remaining * 0.31f + (gap == 1 ? 0.5f : 0.05f) * Deg);
            EXPECT_GT(correction, remaining * 0.1f);
            EXPECT_TRUE(B::IsFinite(a.right.p));
        }
    }
}


TEST(TwoHandSteering, NewGrabCannotInheritPreviousSwingSpeed) {
    namespace B = VR::Body;
    for (int hz : {72, 90, 120}) {
        for (int reset : {0, 1, 2}) {
            SCOPED_TRACE(hz);
            SCOPED_TRACE(reset);
            SteeringRig rig(hz);
            rig.cfg.blend_s = 0; // inspect steering directly, without acquisition blending
            for (int i = 1; i <= hz / 12; ++i) rig.Step(600 * i * rig.dt);
            if (reset == 0) rig.in.held = false;
            if (reset == 1) rig.in.ready = false;
            if (reset == 2) ++rig.in.family;
            // Exercise release, transform readiness and weapon-family changes.
            rig.in.left.p = {0, 0, 0.3f};
            rig.support.Update(rig.cfg, rig.in, rig.dt);
            rig.in.held = rig.in.ready = true;
            EXPECT_LT(B::AngleBetween(rig.Step(0).right.q, {}), 1e-3f);
            ASSERT_TRUE(rig.support.Seated());
            for (int i = 0; i < hz / 2; ++i) {
                const auto a = rig.Step(i % 2 ? 0.35f : -0.35f);
                EXPECT_LT(B::AngleBetween(a.right.q, {}) / Deg, 0.13f);
            }
        }
    }
}


TEST(TwoHandRoll, RightLeftAndTogetherHaveSeventyThirtyInfluenceWithoutMovingAnchor) {
    namespace B = VR::Body;
    for (float side : {-1.0f, 1.0f}) {
        for (int wrist : {0, 1, 2}) {
            SteeringRig rig(90);
            rig.Step(0, side * 0.25f);
            const auto right0 = rig.in.right;
            for (int i = 1; i <= 40; ++i) {
                const float roll = i * Deg;
                rig.in.right.q = B::AxisAngle({0, 0, 1}, wrist == 0 ? 0 : roll);
                rig.in.left.q = B::AxisAngle({0, 0, 1}, wrist == 1 ? 0 : roll);
                const auto out = rig.Step(0, side * 0.25f);
                const float share = wrist == 0 ? 0.3f : wrist == 1 ? 0.7f : 1.0f;
                EXPECT_LT(B::AngleBetween(out.right.q, B::AxisAngle({0, 0, 1}, roll * share)), 1e-3f);
                EXPECT_LT(Dist(BoneOrigin(out.right, R0), BoneOrigin(right0, R0)), 1e-6f);
                EXPECT_LT(B::AngleBetween(out.left.q, rig.in.left.q), 1e-3f);
            }
        }
    }
}

TEST(TwoHandRoll, PitchDoesNotRollAndTwistCrossesTheQuaternionSeamContinuously) {
    namespace B = VR::Body;
    SteeringRig pitch(90);
    pitch.in.left.q = B::AxisAngle({1, 0, 0}, 50 * Deg);
    EXPECT_LT(B::AngleBetween(pitch.Step(0).right.q, {}), 1e-3f);
    SteeringRig roll(90);
    for (int i = 1; i <= 220; ++i) {
        roll.in.left.q = B::AxisAngle({0, 0, 1}, i * Deg);
        const auto out = roll.Step(0);
        EXPECT_LT(B::AngleBetween(out.right.q, B::AxisAngle({0, 0, 1}, std::min(i * 0.3f * Deg, SupportGrip::RollMaxRad))), 1e-3f);
    }
    // Recovery must not add an occlusion's wrist jump to the weapon.
    const auto before = roll.last.right.q;
    roll.in.tracked = false;
    roll.support.Update(roll.cfg, roll.in, roll.dt);
    roll.in.tracked = true;
    roll.in.left.q = {};
    EXPECT_LT(B::AngleBetween(roll.Step(0).right.q, before), 1e-3f);
    roll.in.held = false;
    for (int i = 0; i < 30; ++i) roll.Step(0);
    EXPECT_LT(B::AngleBetween(roll.last.right.q, roll.in.right.q), 1e-3f);
}

TEST(TwoHandMotion, SingleHandFormsAndSeparateDualWeaponsNeverOptIn) {
    for (int family : {20, 50, 80, 120, 310}) {
        EXPECT_TRUE(SharedMotion(family, 3, true));
        EXPECT_FALSE(SharedMotion(family, 3, false)); // transformed, but used one-handed
        for (int style : {1, 2, 4, 5}) EXPECT_FALSE(SharedMotion(family, style, true));
    }
    for (int family : {-1, 0, 10, 40, 90, 270, 320}) {
        EXPECT_FALSE(SharedMotion(family, 3, true));
    }
}

// The steered blade's direction in the grip frame, and its two sampled points.
struct BladePoints {
    Vec3 tip, lever;
};
BladePoints PointsAlong(Vec3 dir) {
    const Vec3 d = VR::Body::Normalize(dir);
    return {AlongBlade({}, d, BladeTipM), AlongBlade({}, d, BladeLeverM)};
}

// One frame of the presenter's two-handed classifier input (openxr_presenter.cpp
// UpdatePhysicalMelee): the lever point moves, the hand (the weapon pose) is the pose point.
VR::Melee::Input ClassifierInput(double t, const Pose& weapon, const BladeSample& sample,
                                 Vec3 head) {
    VR::Melee::Input in;
    in.t = t;
    in.active = in.pos_valid = in.ori_valid = sample.valid;
    in.head_ok = true;
    in.head_p = {head.x, head.y, head.z};
    in.q = {weapon.q.x, weapon.q.y, weapon.q.z, weapon.q.w};
    in.p = {sample.lever.x, sample.lever.y, sample.lever.z};
    in.v = {sample.lever_velocity.x, sample.lever_velocity.y, sample.lever_velocity.z};
    in.pose_valid = true;
    in.pose_p = {weapon.p.x, weapon.p.y, weapon.p.z};
    return in;
}

TEST(TwoHandMotion, BladeRotationDrivesBothPointsEvenWithStationaryGrip) {
    namespace B = VR::Body;
    for (int hz : {72, 90, 120}) {
        SCOPED_TRACE(hz);
        BladeMotion motion;
        // An off-axis axe head sets the direction; the points sit at fixed distances along it.
        const BladePoints pts = PointsAlong({0.12f, 0, -1.1f});
        BladeSample sample;
        for (int i = 0; i <= hz; ++i) {
            const auto ns = static_cast<std::int64_t>((1.0 + double(i) / hz) * 1e9);
            const Pose pose{{0.3f, 1.0f, -0.3f}, B::AxisAngle({0, 1, 0}, i * 600.0f / hz * Deg)};
            sample = motion.Step(true, true, ns, 1, 50, 3, pose, pts.tip, pts.lever,
                                 {0, 1.6f, 0});
            if (i) {
                ASSERT_TRUE(sample.valid);
                EXPECT_NEAR(sample.speed, 600 * Deg * BladeTipM, 0.03f);
                EXPECT_NEAR(sample.lever_speed, 600 * Deg * BladeLeverM, 0.02f);
            }
        }
        // Damage speed on the one-handed scale (80 cm), whatever the weapon's length.
        EXPECT_GT(VR::Melee::SpeedFactor(sample.speed, {}), 1.0f);
    }
}

TEST(TwoHandMotion, LeverChopCanCommitWithoutControllerTranslation) {
    namespace B = VR::Body;
    namespace M = VR::Melee;
    const BladePoints pts = PointsAlong({0, 0, -1});
    for (int hz : {72, 90, 120}) {
        SCOPED_TRACE(hz);
        BladeMotion motion;
        M::Classifier blade_classifier, raw_classifier;
        auto cfg = M::Config::Make(true, false, 140, 100, false, 0);
        cfg.clutchless = true;
        bool blade_fired = false, raw_fired = false;
        for (int i = 0; i < 2 * hz; ++i) {
            const double t = 1.0 + double(i) / hz;
            const float angle = i < hz ? 60 : std::max(-60.0f, 60 - (i - hz) * 600.0f / hz);
            const Pose displayed{{0.3f, 1.1f, -0.3f}, B::AxisAngle({1, 0, 0}, angle * Deg)};
            const auto sample = motion.Step(true, true, static_cast<std::int64_t>(t * 1e9),
                1, 50, 3, displayed, pts.tip, pts.lever, {0, 1.6f, 0});
            M::Input in = ClassifierInput(t, displayed, sample, {0, 1.6f, 0});
            blade_fired |= blade_classifier.Update(in, cfg).fired;
            in.q = {};
            in.p = {0.3f, 1.1f, -0.3f};
            in.v = {};
            in.pose_valid = false;
            raw_fired |= raw_classifier.Update(in, cfg).fired;
        }
        EXPECT_TRUE(blade_fired);
        EXPECT_FALSE(raw_fired);
    }
}

// The classifier's thresholds were tuned on the hand: a wrist flick that only turns the weapon
// 40 degrees at 5 rad/s is no attack, although the 80 cm tip passes them.
TEST(TwoHandMotion, WristFlickIsNoAttackAtTheLeverPoint) {
    namespace B = VR::Body;
    namespace M = VR::Melee;
    const BladePoints pts = PointsAlong({0, 0, -1});
    for (int hz : {72, 90, 120}) {
        SCOPED_TRACE(hz);
        for (bool tip : {false, true}) {
            BladeMotion motion;
            M::Classifier classifier;
            auto cfg = M::Config::Make(true, false, 140, 100, false, 0);
            cfg.clutchless = true;
            bool fired = false;
            for (int i = 0; i < 2 * hz; ++i) {
                const double t = 1.0 + double(i) / hz;
                const float sweep = 5.0f / Deg * (i - hz) / hz; // 5 rad/s, in degrees
                const float angle = i < hz ? 0.0f : std::max(-40.0f, -sweep);
                const Pose displayed{{0.3f, 1.1f, -0.3f}, B::AxisAngle({1, 0, 0}, angle * Deg)};
                const auto sample = motion.Step(true, true, static_cast<std::int64_t>(t * 1e9),
                    1, 50, 3, displayed, pts.tip, pts.lever, {0, 1.6f, 0});
                M::Input in = ClassifierInput(t, displayed, sample, {0, 1.6f, 0});
                if (tip) {
                    in.p = {sample.tip.x, sample.tip.y, sample.tip.z};
                    in.v = {sample.velocity.x, sample.velocity.y, sample.velocity.z};
                }
                fired |= classifier.Update(in, cfg).fired;
            }
            EXPECT_EQ(fired, tip) << (tip ? "the tip passes the hand's thresholds"
                                          : "the lever point stays below them");
        }
    }
}

// A raised guard: the blade high (between the pose angle and the wind-up angle) with the hands at
// the chest. The wound-up pose is judged at the hand, not at a point out along the blade.
TEST(TwoHandMotion, GuardWithTheBladeHighIsNotWoundUpByItsPointAlongTheBlade) {
    namespace B = VR::Body;
    namespace M = VR::Melee;
    const auto cfg = M::Config::Make(true, false, 140, 100, false, 0);
    const Vec3 head{0, 1.6f, 0};
    const Pose guard{{0.1f, 1.25f, -0.3f}, B::AxisAngle({1, 0, 0}, 85 * Deg)};
    const Vec3 far = guard.p + B::Rotate(guard.q, Vec3{0, 0, -BladeTipM});
    for (bool pose : {true, false}) {
        M::Classifier classifier;
        for (int i = 0; i < 10; ++i) {
            M::Input in;
            in.t = 1.0 + i * Dt;
            in.active = in.pos_valid = in.ori_valid = in.head_ok = true;
            in.head_p = {head.x, head.y, head.z};
            in.q = {guard.q.x, guard.q.y, guard.q.z, guard.q.w};
            in.p = {far.x, far.y, far.z};
            in.pose_valid = pose;
            in.pose_p = {guard.p.x, guard.p.y, guard.p.z};
            classifier.Update(in, cfg);
        }
        EXPECT_EQ(classifier.WoundUpPose(cfg), !pose);
    }
}

TEST(TwoHandMotion, TranslationCancelsAndDiscontinuitiesCannotCreateSpeed) {
    for (int gap = 0; gap < 9; ++gap) {
        BladeMotion motion;
        Pose pose{};
        Vec3 head{}, tip{0, 0, -0.8f}, lever{0, 0, -0.3f};
        std::int64_t ns = 1'000'000'000;
        std::uint64_t space = 1;
        int family = 50;
        motion.Step(true, true, ns, space, family, 3, pose, tip, lever, head);
        pose.p = head = {0.01f, 0, 0};
        auto out = motion.Step(true, true, ns += 10'000'000, space, family, 3, pose, tip, lever,
                               head);
        ASSERT_TRUE(out.valid);
        EXPECT_NEAR(out.speed, 0, 1e-5f); // head and weapon translated together
        EXPECT_NEAR(out.lever_speed, 0, 1e-5f);
        if (gap == 0) ++space;
        if (gap == 1) family = 80;
        if (gap == 2) ns += 100'000'000;
        if (gap == 3) tip.z = -1.3f;
        if (gap == 4) pose.p.x += 1.0f;
        if (gap == 5) pose.q = VR::Body::AxisAngle({0, 1, 0}, 90 * Deg);
        if (gap == 8) lever.x = 0.05f;
        out = motion.Step(gap != 6, gap != 7, ns += 10'000'000, space, family, 3, pose, tip, lever,
                          head);
        EXPECT_FALSE(out.valid);
        EXPECT_FLOAT_EQ(out.speed, 0);
        EXPECT_FLOAT_EQ(out.lever_speed, 0);
    }
}

// The rail's blade side is learned per weapon from its striking part, a hand's width short.
TEST(TwoHandRail, BladeClearanceIsLearnedPerWeaponAndKeptForFormA) {
    EXPECT_FLOAT_EQ(BladeClearOf(0.04f), 0.0f);   // a sword: the blade starts at the guard
    EXPECT_NEAR(BladeClearOf(0.95f), 0.89f, 1e-6f); // an axe head far out (the rail caps it)
    EXPECT_FLOAT_EQ(BladeClearOf(std::numeric_limits<float>::quiet_NaN()), 0.0f);
    BladeClearCache cache;
    EXPECT_FLOAT_EQ(cache.Get(50), 0.0f); // unknown: no blade side
    cache.Set(50, 0.89f);
    cache.Set(120, 0.0f);
    EXPECT_FLOAT_EQ(cache.Get(50), 0.89f);
    EXPECT_FLOAT_EQ(cache.Get(120), 0.0f);
    EXPECT_FLOAT_EQ(cache.Get(-1), 0.0f);
    Handle h = H0();
    h.blade_clear = cache.Get(50);
    EXPECT_FLOAT_EQ(RailOf(h).lo, -RailBladeMaxM);
    EXPECT_NEAR(RailOf(h).hi, 0.18f, 1e-6f);
}

// A re-grab while the last hold still eases out carries the weapon's shown angle: no snap.
TEST(TwoHandSupport, RegrabDuringTheEaseOutKeepsTheShownAngle) {
    namespace B = VR::Body;
    SupportGrip s;
    Settings cfg;
    SupportGrip::Input in;
    in.held = in.ready = in.tracked = true;
    in.family = 50;
    in.target = {true, {0, 0.4f, 0}, {}};
    in.blade_clear = 0.30f;
    in.right_bone = R0;
    in.left_bone = L0;
    in.left.p = {0, 0, 0.3f};
    ASSERT_TRUE(s.Update(cfg, in, Dt));
    // Steer 12 degrees round Y and settle (the hand stays within the zone of the controller's
    // own rail, where a re-grab is taken).
    in.left.p = B::Rotate(B::AxisAngle({0, 1, 0}, 12 * Deg), Vec3{0, 0, 0.3f});
    Applied shown{};
    for (int i = 0; i < 90; ++i) {
        s.Update(cfg, in, Dt);
        shown = s.ApplyTracked(in.right, in.left, R0, L0, Dt);
    }
    ASSERT_GT(B::AngleBetween(shown.right.q, in.right.q), 10 * Deg);
    // Let go for three frames (part way out), then take it again where the hand is now.
    in.held = false;
    for (int i = 0; i < 3; ++i) {
        s.Update(cfg, in, Dt);
        shown = s.ApplyTracked(in.right, in.left, R0, L0, Dt);
    }
    in.held = true;
    ASSERT_TRUE(s.Update(cfg, in, Dt));
    const Applied next = s.ApplyTracked(in.right, in.left, R0, L0, Dt);
    EXPECT_LT(B::AngleBetween(next.right.q, shown.right.q), 1 * Deg);
}

// The left wrist's share of the roll is bounded, and turning back undoes it at once.
TEST(TwoHandRoll, LeftWristShareIsBoundedAndUnwindsImmediately) {
    namespace B = VR::Body;
    SteeringRig rig(90);
    for (int i = 1; i <= 300; ++i) {
        rig.in.left.q = B::AxisAngle({0, 0, 1}, i * Deg);
        rig.Step(0);
    }
    const float bounded = B::AngleBetween(rig.last.right.q, rig.in.right.q);
    EXPECT_NEAR(bounded, SupportGrip::RollMaxRad, 1e-3f);
    // 300 degrees of wrist; turning back 20 degrees already turns the blade back.
    for (int i = 299; i >= 280; --i) {
        rig.in.left.q = B::AxisAngle({0, 0, 1}, i * Deg);
        rig.Step(0);
    }
    EXPECT_NEAR(B::AngleBetween(rig.last.right.q, rig.in.right.q),
                SupportGrip::RollMaxRad - 20 * Deg * SupportGrip::RollShare, 1e-3f);
}

// Steering filters on the published poses' clock: a stale or hitching input dt cannot change
// how the weapon follows the hands.
TEST(TwoHandSteering, ApplyClockNotTheInputClockDrivesTheFilter) {
    namespace B = VR::Body;
    SteeringRig a(90), b(90);
    for (int i = 1; i <= 30; ++i) {
        const float deg = 4.0f * i;
        a.Step(deg);
        b.in.left.p = B::Rotate(B::AxisAngle({0, 1, 0}, deg * Deg), Vec3{0, 0, 0.3f});
        b.support.Update(b.cfg, b.in, 0.045f); // the input loop's dt, not the frame's
        b.last = b.support.ApplyTracked(b.in.right, b.in.left, R0, L0, b.dt);
        EXPECT_LT(B::AngleBetween(a.last.right.q, b.last.right.q), 1e-4f);
    }
    // No frame time (a repeated pose): no steering step, no speed.
    const Applied held = b.last;
    b.in.left.p = B::Rotate(B::AxisAngle({0, 1, 0}, 90 * Deg), Vec3{0, 0, 0.3f});
    b.support.Update(b.cfg, b.in, b.dt);
    const Applied same = b.support.ApplyTracked(b.in.right, b.in.left, R0, L0, 0.0f);
    EXPECT_LT(B::AngleBetween(same.right.q, held.right.q), 1e-4f);
}

// Passing the left hand to the other side of the right one: the controllers cannot go through
// each other, so the hand goes round the right fist (on whichever side the weapon's lean leaves
// free). The detour must not swing the weapon (the 2026-10-13 headset log: 100-145 degrees), and
// steering works again from where the hand comes out.
TEST(TwoHandCrossing, GoingRoundTheRightFistKeepsTheWeaponAndSteersAgainAfter) {
    namespace B = VR::Body;
    for (int hz : {72, 90, 120}) {
        for (Vec3 side : {Vec3{1, 0, 0}, Vec3{-1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, -0.7f, 0.7f}}) {
            SCOPED_TRACE(hz);
            const float dt = 1.0f / hz;
            SupportGrip s;
            Settings cfg;
            SupportGrip::Input in;
            in.held = in.ready = in.tracked = true;
            in.family = 50;
            in.target = {true, {0, 0.4f, 0}, {}};
            in.blade_clear = 0.30f; // a haft
            in.right_bone = R0;
            in.left_bone = L0;
            in.left.p = {0, 0, 0.25f};
            ASSERT_TRUE(s.Update(cfg, in, dt));
            for (int i = 0; i < hz / 2; ++i) {
                s.Update(cfg, in, dt);
                s.ApplyTracked(in.right, in.left, R0, L0, dt);
            }
            // One second from 25 cm behind the right fist to 25 cm in front, 9 cm round it.
            const Vec3 off = B::Normalize(Vec3{side.x, side.y, 0}) * 0.09f;
            float worst = 0;
            for (int i = 0; i <= hz; ++i) {
                const float z = 0.25f - 0.5f * i / hz;
                const float round = 1.0f - B::Smoothstep(0.04f, 0.14f, std::abs(z));
                in.left.p = Vec3{0, 0, z} + off * round;
                s.Update(cfg, in, dt);
                const auto a = s.ApplyTracked(in.right, in.left, R0, L0, dt);
                worst = std::max(worst, B::AngleBetween(a.right.q, in.right.q));
            }
            // (A few degrees where the hand comes out still a little off the shaft.)
            EXPECT_LT(worst, 8 * Deg);
            EXPECT_LT(s.Slide(), -0.2f); // now in front of the right hand
            // Steering from there: 5 cm sideways at 25 cm turns the weapon about 11 degrees.
            Applied a{};
            for (int i = 0; i < hz; ++i) {
                s.Update(cfg, in, dt);
                a = s.ApplyTracked(in.right, in.left, R0, L0, dt);
            }
            const Quat settled = a.right.q;
            in.left.p = Vec3{0, 0, -0.25f} + B::Normalize(Vec3{side.y, -side.x, 0}) * 0.05f;
            for (int i = 0; i < hz; ++i) {
                s.Update(cfg, in, dt);
                a = s.ApplyTracked(in.right, in.left, R0, L0, dt);
            }
            EXPECT_NEAR(B::AngleBetween(a.right.q, settled), std::atan2(0.05f, 0.25f), 1.5f * Deg);
        }
    }
}

namespace {
// A haft along the right controller's +Z: the game's left grip 40 cm toward the pommel (+Z; the
// rail ends at 44 cm there), the blade toward -Z (bare shaft 30 cm up, `blade_clear`);
// `left_z`: where the left hand takes it (+: behind the right, toward the pommel; -: in front).
struct SlideRig {
    SupportGrip s;
    Settings cfg;
    SupportGrip::Input in;
    explicit SlideRig(float left_z = 0.3f, float blade_clear = 0.30f) {
        in.held = in.ready = in.tracked = true;
        in.family = 50;
        in.target = {true, {0, 0.4f, 0}, {}};
        in.blade_clear = blade_clear;
        in.right_bone = R0;
        in.left_bone = L0;
        in.left.p = {0, 0, left_z};
        grabbed = s.Update(cfg, in, Dt);
        for (int i = 0; i < 30; ++i) Step();
    }
    Applied Step() {
        s.Update(cfg, in, Dt);
        return s.ApplyTracked(in.right, in.left, R0, L0, Dt);
    }
    // Move the hands by these offsets over `frames` (0.5 s at 90 Hz by default), then settle.
    void Move(Vec3 d_right, Vec3 d_left, int frames = 45) {
        const Vec3 r0 = in.right.p, l0 = in.left.p;
        for (int i = 1; i <= frames; ++i) {
            const float k = static_cast<float>(i) / frames;
            in.right.p = r0 + d_right * k;
            in.left.p = l0 + d_left * k;
            Step();
        }
        for (int i = 0; i < 30; ++i) Step();
    }
    // The weapon's own grip point in the room: the right controller's, moved in the right fist.
    Vec3 WeaponGrip() const {
        return in.right.p + VR::Body::Rotate(in.right.q, s.WeaponShiftGrip());
    }
    float LeftOn() const { return s.RightSlide() + s.Slide(); }
    bool grabbed{false};
};
} // namespace

// Only the left hand moves along the weapon: it slides; the weapon stays in the right fist.
TEST(TwoHandSlide, MovingTheLeftHandSlidesIt) {
    SlideRig rig(0.3f);
    ASSERT_TRUE(rig.grabbed);
    const Vec3 weapon = rig.WeaponGrip();
    rig.Move({}, {0, 0, -0.1f});
    EXPECT_NEAR(rig.s.Slide(), 0.2f, 0.003f);
    EXPECT_NEAR(rig.s.RightSlide(), 0.0f, 0.001f);
    EXPECT_LT(Dist(rig.WeaponGrip(), weapon), 0.001f);
}

// Only the right hand moves along the weapon (with the left behind it or in front): it slides; the
// weapon stays where it is in the room and the left hand keeps its place on it.
TEST(TwoHandSlide, MovingTheRightHandSlidesIt) {
    for (float left_z : {0.3f, -0.25f}) {
        SCOPED_TRACE(left_z);
        SlideRig rig(left_z);
        ASSERT_TRUE(rig.grabbed);
        const Vec3 weapon = rig.WeaponGrip();
        const float left_on = rig.LeftOn();
        rig.Move({0, 0, 0.1f}, {});
        EXPECT_NEAR(rig.s.RightSlide(), 0.1f, 0.004f);
        EXPECT_NEAR(rig.LeftOn(), left_on, 0.004f);
        EXPECT_LT(Dist(rig.WeaponGrip(), weapon), 0.004f);
    }
}

// Both hands moving together along the weapon (a thrust, a step): nothing slides; the weapon goes
// with them.
TEST(TwoHandSlide, MovingBothTogetherSlidesNothing) {
    SlideRig rig(0.3f);
    const Vec3 weapon = rig.WeaponGrip();
    const float slide = rig.s.Slide();
    rig.Move({0, 0, -0.15f}, {0, 0, -0.15f});
    EXPECT_NEAR(rig.s.Slide(), slide, 0.002f);
    EXPECT_NEAR(rig.s.RightSlide(), 0.0f, 0.002f);
    EXPECT_NEAR(Dist(rig.WeaponGrip(), weapon), 0.15f, 0.002f);
}

// Passing the left hand round the right one to the front: the weapon neither moves nor flails (no
// jump, no flip, a few degrees at most), and from there the right hand slides when it moves.
TEST(TwoHandSlide, PassingTheHandsNeitherMovesNorFlailsTheWeapon) {
    namespace B = VR::Body;
    for (int hz : {72, 90, 120}) {
        SCOPED_TRACE(hz);
        SlideRig rig(0.25f);
        const Vec3 weapon = rig.WeaponGrip();
        const float dt = 1.0f / hz;
        Quat last = rig.s.ApplyTracked(rig.in.right, rig.in.left, R0, L0, dt).right.q;
        const Quat start = last;
        float worst_step = 0, worst = 0;
        for (int i = 0; i <= hz; ++i) { // one second round the right fist, 9 cm out at it
            const float z = 0.25f - 0.5f * i / hz;
            const float round = 1.0f - B::Smoothstep(0.04f, 0.14f, std::abs(z));
            rig.in.left.p = Vec3{0.09f * round, 0, z};
            rig.s.Update(rig.cfg, rig.in, dt);
            const auto a = rig.s.ApplyTracked(rig.in.right, rig.in.left, R0, L0, dt);
            worst_step = std::max(worst_step, B::AngleBetween(a.right.q, last));
            worst = std::max(worst, B::AngleBetween(a.right.q, start));
            last = a.right.q;
            EXPECT_LT(Dist(rig.WeaponGrip(), weapon), 1e-4f);
        }
        EXPECT_LT(worst_step, 1.5f * Deg);
        EXPECT_LT(worst, 8 * Deg);
        EXPECT_NEAR(rig.s.RightSlide(), 0.0f, 0.001f);
        EXPECT_LT(rig.s.Slide(), -0.2f); // the left hand now in front
        rig.Move({0, 0, 0.06f}, {});
        EXPECT_NEAR(rig.s.RightSlide(), 0.06f, 0.004f);
        EXPECT_LT(Dist(rig.WeaponGrip(), weapon), 0.004f);
    }
}

// Both hands held still with tracking jitter (+-1 mm): nothing creeps along the weapon.
TEST(TwoHandSlide, JitterNeverCreepsTheWeaponThroughTheFist) {
    SlideRig rig(0.3f);
    const Vec3 r0 = rig.in.right.p, l0 = rig.in.left.p;
    const float slide = rig.s.Slide();
    std::uint32_t seed = 12345;
    const auto noise = [&] {
        seed = seed * 1664525u + 1013904223u;
        return (static_cast<float>(seed >> 8) / 16777216.0f - 0.5f) * 0.002f;
    };
    for (int i = 0; i < 90 * 20; ++i) { // 20 seconds
        rig.in.right.p = r0 + Vec3{noise(), noise(), noise()};
        rig.in.left.p = l0 + Vec3{noise(), noise(), noise()};
        rig.Step();
    }
    EXPECT_LT(std::abs(rig.s.RightSlide()), 0.001f);
    EXPECT_LT(std::abs(rig.s.Slide() - slide), 0.003f);
}

// A swing moves the weapon, not the hands on it: nothing slides until it has calmed down.
TEST(TwoHandSlide, NothingSlidesDuringASwing) {
    SlideRig rig(0.3f);
    rig.in.swing_mps = 1.2f;
    const float slide = rig.s.Slide(), right_slide = rig.s.RightSlide();
    for (int i = 1; i <= 30; ++i) {
        rig.in.left.p = {0, 0, 0.3f - 0.08f * i / 30.0f};
        rig.in.right.p = {0, 0, 0.05f * i / 30.0f};
        rig.Step();
        EXPECT_TRUE(rig.s.Swinging());
    }
    EXPECT_FLOAT_EQ(rig.s.Slide(), slide);
    EXPECT_FLOAT_EQ(rig.s.RightSlide(), right_slide);
    // Slower than SwingOffMps but not yet for SwingCalmS: still locked; then free again.
    rig.in.swing_mps = 0.3f;
    rig.Step();
    EXPECT_TRUE(rig.s.Swinging());
    for (int i = 0; i < 20; ++i) rig.Step();
    EXPECT_FALSE(rig.s.Swinging());
    for (int i = 0; i < 40; ++i) rig.Step();
    EXPECT_NE(rig.s.Slide(), slide);
}

// Walls, not springs: the upper hand always leaves room for the lower one at the pommel end (no
// holding the weapon with both hands at its very bottom), and on a sword the right hand goes no
// lower than the game's own left grip.
TEST(TwoHandSlide, WallsKeepTheHoldBalanced) {
    SlideRig haft(0.3f);
    haft.Move({}, {0, 0, 0.14f});         // the left hand to the very bottom (44 cm)
    haft.Move({0, 0, 0.42f}, {}, 90);     // the right hand down after it
    EXPECT_LE(haft.LeftOn(), 0.44f + 1e-5f);
    EXPECT_LE(haft.s.RightSlide(), 0.44f - SupportGrip::UpperRoomM + 1e-5f);
    SlideRig sword(0.3f, 0.0f);
    sword.Move({}, {0, 0, -0.18f});       // the left hand up the handle (12 cm)
    sword.Move({0, 0, 0.43f}, {}, 90);    // the right hand down past it, toward the pommel
    EXPECT_LE(sword.s.RightSlide(), 0.40f + 1e-5f); // the game's own left grip
    EXPECT_GE(sword.LeftOn(), -1e-5f);              // never onto the blade
    // The right hand stopped at its wall carries the weapon on: the hunter's left hand stays on
    // the real one.
    const auto a = sword.Step();
    EXPECT_LT(Dist(BoneOrigin(a.left, L0), BoneOrigin(sword.in.left, L0)), 0.004f);
}

// After a swing the hands catch up with where they really are: that is the left hand's slide, never
// the weapon creeping in the right fist.
TEST(TwoHandSlide, AfterASwingTheCatchUpIsTheLeftHands) {
    SlideRig rig(0.3f);
    for (int swing = 0; swing < 5; ++swing) {
        rig.in.swing_mps = 1.2f;
        for (int i = 1; i <= 20; ++i) { // the swing: both hands move, the left drifts 3 cm
            rig.in.right.p = {0.02f * i / 20.0f, 0, 0.01f * i / 20.0f};
            rig.in.left.p = {0.02f * i / 20.0f, 0, 0.3f - 0.03f * (swing % 2 ? -1 : 1) * i / 20.0f};
            rig.Step();
        }
        rig.in.swing_mps = 0.0f;
        for (int i = 0; i < 60; ++i) rig.Step();
    }
    EXPECT_LT(std::abs(rig.s.RightSlide()), 0.002f);
    const auto a = rig.Step();
    EXPECT_LT(Dist(BoneOrigin(a.left, L0), BoneOrigin(rig.in.left, L0)), 0.004f);
}

// Letting go of the weapon (both grips) eases it back to the game's grip point in the right fist;
// one-handed it never moves there at all.
TEST(TwoHandSlide, LettingGoEasesTheWeaponBackToItsGrip) {
    SlideRig rig(-0.25f);
    rig.Move({0, 0, 0.08f}, {});
    ASSERT_GT(VR::Body::Length(rig.s.WeaponShift()), 0.05f);
    rig.in.held = false;
    float last = VR::Body::Length(rig.s.WeaponShift());
    for (int i = 0; i < 30; ++i) {
        rig.Step();
        const float now = VR::Body::Length(rig.s.WeaponShift());
        EXPECT_LE(now, last + 1e-6f);
        last = now;
    }
    EXPECT_LT(last, 1e-5f);
    SupportGrip never;
    EXPECT_FLOAT_EQ(VR::Body::Length(never.WeaponShift()), 0.0f);
}

TEST(TwoHandHum, FaintSpeedDependentFeedbackStopsOnDetachAndRespectsGain) {
    SwingHum hum;
    EXPECT_EQ(hum.Step(true, 2.0f, Dt, 1), 0);
    const float low = hum.Step(true, 4.0f, Dt, 1);
    EXPECT_GT(low, 0);
    float high = 0;
    for (int i = 0; i < 90; ++i) high = hum.Step(true, 20.0f, Dt, 1);
    EXPECT_GT(high, low);
    EXPECT_LE(high, 0.08f);
    EXPECT_EQ(hum.Step(false, 20.0f, Dt, 1), 0);
    EXPECT_EQ(hum.Step(true, 20.0f, Dt, 0), 0);
    for (int i = 0; i < 90; ++i) high = hum.Step(true, 20.0f, Dt, 0.5f);
    EXPECT_NEAR(high, 0.04f, 1e-5f);
    for (int i = 0; i < 90; ++i) high = hum.Step(true, 0, Dt, 1);
    EXPECT_EQ(high, 0);
    const auto saved = GetTwoHandSettings();
    EXPECT_TRUE(ParseTwoHandSetting("two_hand_swing_hum", "0"));
    EXPECT_EQ(GetTwoHandSettings().swing_hum, 0);
    EXPECT_TRUE(ParseTwoHandSetting("two_hand_swing_hum", "2"));
    EXPECT_EQ(GetTwoHandSettings().swing_hum, 1);
    GetTwoHandSettings() = saved;
}
