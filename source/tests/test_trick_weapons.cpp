// SPDX-License-Identifier: GPL-2.0-or-later

// Trick weapons played by hand (core/vr/trick_weapons.h) at 90 Hz: the right grip's use per
// weapon (frees the left hand, arms the stab, held to charge, lifted up); the dual blades split
// when the freed hand takes the hilt and the hands pull apart, and lock back together side by
// side; the armed Bloodletter driven into the chest (not pointing away, nor swept past); a
// cooldown after each; form B kept only while both hands hold the handle; the charged blast; the
// shield up while the left hand is held in front; the moonlight wave's aim.

#include <cmath>
#include <initializer_list>
#include <sstream>
#include <utility>

#include <gtest/gtest.h>

#include "core/vr/trick_weapons.h"
#include "core/vr/melee_window.h"
#include "core/vr/stake_driver.h"

namespace {

namespace T = VR::Tricks;
using V = VR::Body::Vec3;

constexpr float Dt = 1.0f / 90.0f;
constexpr V Head{0.0f, 1.6f, 0.0f};

T::Frame Make(V left, V right, bool grip, V axis = {}) {
    T::Frame f{};
    f.dt = Dt;
    f.head = Head;
    f.forward = {0.0f, 0.0f, -1.0f};
    f.left = {true, left, {}, grip, true};
    f.right = {true, right, {}, false, true};
    f.axis = axis;
    return f;
}

struct Rig {
    T::HandTransform ht;
    T::Settings s{};
    T::Kind kind{T::Kind::None};
    bool form_b{false};
    bool ready{true}; // the right grip freed the left hand / armed the stab

    T::HandTransform::Out Step(const T::Frame& f) {
        return ht.Step(f, kind, form_b, ready, s);
    }
    // The left hand moved from `a` to `b` over `secs` (the right one still), grip as given;
    // returns the first gesture other than None (or None).
    T::Gesture Move(V a, V b, V right, float secs, bool grip, V axis = {}, bool* owned = nullptr) {
        const int n = std::max(1, static_cast<int>(secs / Dt));
        for (int i = 1; i <= n; ++i) {
            const float k = static_cast<float>(i) / n;
            T::Frame f = Make(a + (b - a) * k, right, grip, axis);
            f.left.v = (b - a) * (1.0f / secs);
            const auto o = Step(f);
            if (owned != nullptr) {
                *owned = o.owns_grip;
            }
            if (o.gesture != T::Gesture::None) {
                return o.gesture;
            }
        }
        return T::Gesture::None;
    }
};

TEST(TrickWeapons, Kinds) {
    EXPECT_EQ(T::KindOf(40), T::Kind::DualBlades);
    EXPECT_EQ(T::KindOf(270), T::Kind::DualBlades);
    EXPECT_EQ(T::KindOf(290), T::Kind::Bloodletter);
    EXPECT_EQ(T::KindOf(81), T::Kind::None); // (the grab on the handle: core/vr/two_hand_grip.h)
    EXPECT_EQ(T::KindOf(130), T::Kind::None);
    EXPECT_EQ(T::KindOf(70), T::Kind::None);
    EXPECT_TRUE(T::UsesL1(20));
    EXPECT_TRUE(T::UsesL1(130));
    EXPECT_FALSE(T::UsesL1(40));
    EXPECT_EQ(T::ShotWeaponOf(1000)->charge_s, T::RifleChargeS); // the Rifle Spear's blast
    EXPECT_EQ(T::ShotWeaponOf(1100)->charge_s, T::BlastChargeS);
    EXPECT_EQ(T::ShotWeaponOf(1010)->charge_s, 0.0f); // the Reiterpallasch fires at the squeeze
    ASSERT_NE(T::ShotWeaponOf(1010), nullptr);
    EXPECT_EQ(T::ShotWeaponOf(1010)->shot, T::Shot::GunBlade);
    EXPECT_EQ(T::ShotWeaponOf(2600)->shot, T::Shot::MoonWave);
    EXPECT_EQ(T::ShotWeaponOf(1100)->shot, T::Shot::StakeBlast);
    EXPECT_EQ(T::ShotWeaponOf(700), nullptr);
}

constexpr V Fist{0.20f, 1.25f, -0.35f}; // the right hand in front of the chest

TEST(TrickWeapons, TheHiltTakenAndPulledApartSplits) {
    Rig r;
    r.kind = T::Kind::DualBlades;
    const V at{Fist.x - 0.06f, Fist.y, Fist.z};
    r.Step(Make(at, Fist, false));
    EXPECT_EQ(r.Step(Make(at, Fist, true)).gesture, T::Gesture::Grab);
    bool owned = false;
    const V out{at.x - 0.25f, at.y, at.z};
    EXPECT_EQ(r.Move(at, out, Fist, 0.3f, true, {}, &owned), T::Gesture::Split);
    EXPECT_TRUE(owned); // the arc never sees this squeeze
    EXPECT_TRUE(r.Step(Make(out, Fist, true)).owns_grip);
    EXPECT_FALSE(r.Step(Make(out, Fist, false)).owns_grip); // let go
}

TEST(TrickWeapons, NoSplitFromAfarTooShortLetGoOrTooLate) {
    Rig far;
    far.kind = T::Kind::DualBlades;
    const V away{Fist.x - 0.30f, Fist.y, Fist.z};
    far.Step(Make(away, Fist, false));
    EXPECT_EQ(far.Step(Make(away, Fist, true)).gesture, T::Gesture::None);
    EXPECT_FALSE(far.Step(Make(away, Fist, true)).owns_grip);

    Rig short_pull;
    short_pull.kind = T::Kind::DualBlades;
    const V at{Fist.x - 0.06f, Fist.y, Fist.z};
    short_pull.Step(Make(at, Fist, false));
    short_pull.Step(Make(at, Fist, true));
    EXPECT_EQ(short_pull.Move(at, {at.x - 0.10f, at.y, at.z}, Fist, 0.3f, true), T::Gesture::None);
    EXPECT_EQ(short_pull.Step(Make({at.x - 0.10f, at.y, at.z}, Fist, false)).gesture,
              T::Gesture::Let);

    Rig late;
    late.kind = T::Kind::DualBlades;
    late.Step(Make(at, Fist, false));
    late.Step(Make(at, Fist, true));
    EXPECT_EQ(late.Move(at, at, Fist, T::SplitTimeoutS + 0.1f, true), T::Gesture::Let);
}

TEST(TrickWeapons, TheBladesLockTogetherSideBySide) {
    Rig r;
    r.kind = T::Kind::DualBlades;
    r.form_b = true;
    const V fwd{0.0f, 0.0f, -1.0f};
    const V at{Fist.x - 0.08f, Fist.y, Fist.z};
    T::Frame f = Make(at, Fist, false, fwd);
    f.left_axis = fwd;
    r.Step(f);
    f.left.grip = true;
    const auto o = r.Step(f);
    EXPECT_EQ(o.gesture, T::Gesture::Join);
    EXPECT_TRUE(o.owns_grip);

    // Crossed (90 degrees apart): no.
    Rig crossed;
    crossed.kind = T::Kind::DualBlades;
    crossed.form_b = true;
    T::Frame c = Make(at, Fist, false, fwd);
    c.left_axis = {1.0f, 0.0f, 0.0f};
    crossed.Step(c);
    c.left.grip = true;
    EXPECT_EQ(crossed.Step(c).gesture, T::Gesture::None);

    // Apart: no.
    Rig apart;
    apart.kind = T::Kind::DualBlades;
    apart.form_b = true;
    T::Frame a = Make({Fist.x - 0.30f, Fist.y, Fist.z}, Fist, false, fwd);
    a.left_axis = fwd;
    apart.Step(a);
    a.left.grip = true;
    EXPECT_EQ(apart.Step(a).gesture, T::Gesture::None);
}

// A squeeze begun as the hands close in still takes when they meet within SqueezeS; one that
// never gets there (too far, or the blades crossed) is a soft no, and never the quick items'.
TEST(TrickWeapons, ASqueezeOnTheWayInStillTakes) {
    const V fwd{0.0f, 0.0f, -1.0f};
    const V coming{Fist.x - 0.24f, Fist.y, Fist.z}; // (inside NearM, outside JoinM)
    const V met{Fist.x - 0.15f, Fist.y, Fist.z}; // controllers' rings touching: 15 cm apart
    Rig r;
    r.kind = T::Kind::DualBlades;
    r.form_b = true;
    T::Frame f = Make(coming, Fist, false, fwd);
    f.left_axis = fwd;
    r.Step(f);
    f.left.grip = true;
    const auto first = r.Step(f);
    EXPECT_EQ(first.gesture, T::Gesture::None);
    EXPECT_TRUE(first.owns_grip);
    f.left.p = met;
    EXPECT_EQ(r.Step(f).gesture, T::Gesture::Join);

    // Squeezed on the way, the blades never side by side: a soft no after SqueezeS.
    Rig crossed;
    crossed.kind = T::Kind::DualBlades;
    crossed.form_b = true;
    T::Frame c = Make(met, Fist, false, fwd);
    c.left_axis = {1.0f, 0.0f, 0.0f};
    crossed.Step(c);
    c.left.grip = true;
    bool missed = false;
    for (int i = 0; i < static_cast<int>(T::SqueezeS / Dt) + 3; ++i) {
        const auto o = crossed.Step(c);
        EXPECT_EQ(o.gesture, T::Gesture::None);
        missed = missed || o.missed;
    }
    EXPECT_TRUE(missed);

    // The split's hilt the same way.
    Rig split;
    split.kind = T::Kind::DualBlades;
    split.Step(Make(coming, Fist, false));
    split.Step(Make(coming, Fist, true));
    EXPECT_EQ(split.Step(Make(met, Fist, true)).gesture, T::Gesture::Grab);
}

// Positions the cameras only infer (a hand at the cheek, behind the other) still take the hilt.
TEST(TrickWeapons, InferredHandsGrab) {
    const V at{Fist.x - 0.06f, Fist.y, Fist.z};
    Rig r;
    r.kind = T::Kind::DualBlades;
    T::Frame f = Make(at, Fist, false);
    f.left.tracked = false;
    r.Step(f);
    f.left.grip = true;
    EXPECT_EQ(r.Step(f).gesture, T::Gesture::Grab);
}

// The dual blades split only once the right grip freed the left hand (the user, 2026-10-12): a
// squeeze at the hilt before that is the left grip's own (never owned).
TEST(TrickWeapons, NoSplitUntilTheRightGripFreesTheHand) {
    Rig r;
    r.kind = T::Kind::DualBlades;
    r.ready = false;
    const V at{Fist.x - 0.06f, Fist.y, Fist.z};
    r.Step(Make(at, Fist, false));
    const auto o = r.Step(Make(at, Fist, true));
    EXPECT_EQ(o.gesture, T::Gesture::None);
    EXPECT_FALSE(o.owns_grip);
    EXPECT_EQ(r.Move(at, {at.x - 0.25f, at.y, at.z}, Fist, 0.3f, true), T::Gesture::None);
}

constexpr V Palm{-0.20f, 1.25f, -0.40f}; // the left hand, out of the way

// The right hand from `from` to `to` over `secs`, the weapon along `axis`; the first gesture.
T::Gesture Swing(Rig& r, V from, V to, float secs, V axis, V left = Palm,
                 bool right_grip = true) {
    const int n = static_cast<int>(secs / Dt);
    for (int i = 0; i <= n; ++i) {
        const float k = static_cast<float>(i) / n;
        T::Frame f = Make(left, from + (to - from) * k, false, axis);
        f.right.v = (to - from) * (1.0f / secs);
        f.right.grip = right_grip;
        const auto o = r.Step(f);
        if (o.gesture != T::Gesture::None) {
            return o.gesture;
        }
    }
    return T::Gesture::None;
}

TEST(TrickWeapons, TheBloodletterDrivenIntoTheChest) {
    const V chest{Head.x, Head.y - T::ChestDownM, Head.z};
    const V from{0.0f, chest.y, -1.25f};
    const V to{0.0f, chest.y, -0.95f};
    const V back = VR::Body::Normalize(chest - from); // turned round: pointing at the chest
    Rig r;
    r.kind = T::Kind::Bloodletter;
    EXPECT_EQ(Swing(r, from, to, 0.25f, back), T::Gesture::Stab);
    // Not armed (the right grip not squeezed first), or already in form B: no.
    Rig unarmed;
    unarmed.kind = T::Kind::Bloodletter;
    unarmed.ready = false;
    EXPECT_EQ(Swing(unarmed, from, to, 0.25f, back), T::Gesture::None);
    Rig in_b;
    in_b.kind = T::Kind::Bloodletter;
    in_b.form_b = true;
    EXPECT_EQ(Swing(in_b, from, to, 0.25f, back), T::Gesture::None);
    // Held the right way round (pointing away), moved slowly toward you: no.
    Rig away;
    away.kind = T::Kind::Bloodletter;
    EXPECT_EQ(Swing(away, {0.0f, chest.y, -0.30f}, {0.0f, chest.y, -0.20f}, 0.25f,
                    {0.0f, 0.0f, -1.0f}),
              T::Gesture::None);
    // Its head swept sideways across the chest (a wind-up), not driven along its line: no.
    Rig sweep;
    sweep.kind = T::Kind::Bloodletter;
    EXPECT_EQ(Swing(sweep, {-0.40f, chest.y, -0.90f}, {0.20f, chest.y, -0.90f}, 0.30f,
                    {0.0f, 0.0f, 1.0f}),
              T::Gesture::None);
}

// Held as it is (its head away from you), its back end, the hilt, driven back-first into the chest
// or the face: the stab too. Slow, the other way, sideways or unarmed: no.
TEST(TrickWeapons, BloodletterHiltDrivenIntoTheChestOrFace) {
    const V chest{Head.x, Head.y - T::ChestDownM, Head.z};
    const V away{0.0f, 0.0f, -1.0f}; // the weapon pointing ahead, as held
    for (const V target : {chest, Head}) {
        Rig r;
        r.kind = T::Kind::Bloodletter;
        const V from{target.x, target.y, target.z - 0.45f};
        const V to{target.x, target.y, target.z - 0.15f};
        EXPECT_EQ(Swing(r, from, to, 0.25f, away), T::Gesture::Stab);
        Rig slow;
        slow.kind = T::Kind::Bloodletter;
        EXPECT_EQ(Swing(slow, from, to, 1.0f, away), T::Gesture::None);
        Rig out;
        out.kind = T::Kind::Bloodletter;
        EXPECT_EQ(Swing(out, to, from, 0.25f, away), T::Gesture::None); // pulled away from you
        Rig unarmed;
        unarmed.kind = T::Kind::Bloodletter;
        EXPECT_EQ(Swing(unarmed, from, to, 0.25f, away, Palm, false), T::Gesture::None);
    }
    // The hilt swept sideways past the chest (not driven along the weapon's line): no.
    Rig sweep;
    sweep.kind = T::Kind::Bloodletter;
    EXPECT_EQ(Swing(sweep, {-0.40f, chest.y, chest.z - 0.15f}, {0.30f, chest.y, chest.z - 0.15f},
                    0.30f, away),
              T::Gesture::None);
}

TEST(TrickWeapons, BloodletterStabWithinNormalReach) {
    const float y = Head.y - T::ChestDownM;
    Rig r;
    r.kind = T::Kind::Bloodletter;
    EXPECT_EQ(Swing(r, {0.0f, y, -0.60f}, {0.0f, y, -0.36f}, 0.25f,
                    {0.0f, 0.0f, 1.0f}), T::Gesture::Stab);
    Rig slow;
    slow.kind = T::Kind::Bloodletter;
    EXPECT_EQ(Swing(slow, {0.0f, y, -0.60f}, {0.0f, y, -0.36f}, 1.0f,
                    {0.0f, 0.0f, 1.0f}), T::Gesture::None);
    Rig released;
    released.kind = T::Kind::Bloodletter;
    EXPECT_EQ(Swing(released, {0.0f, y, -0.60f}, {0.0f, y, -0.36f}, 0.25f,
                    {0.0f, 0.0f, 1.0f}, Palm, false), T::Gesture::None);
    Rig lean;
    lean.kind = T::Kind::Bloodletter;
    for (int i = 0; i < 30; ++i) {
        auto f = Make(Palm, {0.0f, y, -0.60f}, false, {0.0f, 0.0f, 1.0f});
        f.right.grip = true;
        f.head.z -= i * 0.01f;
        EXPECT_EQ(lean.Step(f).gesture, T::Gesture::None);
    }
}

TEST(TrickWeapons, BloodletterStabWorksWithoutTheLeftController) {
    EXPECT_TRUE(T::HandTransformArmsReady(T::Kind::Bloodletter, 0.0f, 1.0f));
    EXPECT_FALSE(T::HandTransformArmsReady(T::Kind::Bloodletter, 1.0f, 0.0f));
    EXPECT_FALSE(T::HandTransformArmsReady(T::Kind::DualBlades, 0.0f, 1.0f));
    EXPECT_FALSE(T::HandTransformArmsReady(T::Kind::DualBlades, 1.0f, 0.0f));
    EXPECT_TRUE(T::HandTransformArmsReady(T::Kind::DualBlades, 1.0f, 1.0f));
    EXPECT_FALSE(T::HandTransformArmsReady(T::Kind::None, 1.0f, 1.0f));

    // Exercise the presenter's arm gate followed by the gesture with no left-hand pose.
    for (const bool right_tracked : {false, true}) {
        Rig r;
        r.kind = T::Kind::Bloodletter;
        bool stabbed = false;
        for (int i = 0; i <= 27; ++i) {
            auto f = Make({}, {0.0f, Head.y - T::ChestDownM, -0.60f + i * 0.01f},
                          false, {0.0f, 0.0f, 1.0f});
            f.left = {};
            f.right.grip = true;
            f.right.tracked = right_tracked;
            f.right.v = {0.0f, 0.0f, 0.9f};
            if (T::HandTransformArmsReady(r.kind, 0.0f, 1.0f)) {
                stabbed |= r.Step(f).gesture == T::Gesture::Stab;
            }
        }
        EXPECT_EQ(stabbed, right_tracked);
    }
}

TEST(TrickWeapons, OneGestureThenACooldown) {
    const V chest{Head.x, Head.y - T::ChestDownM, Head.z};
    const V from{0.0f, chest.y, -1.25f};
    const V to{0.0f, chest.y, -0.95f};
    const V back = VR::Body::Normalize(chest - from);
    Rig r;
    r.kind = T::Kind::Bloodletter;
    EXPECT_EQ(Swing(r, from, to, 0.25f, back), T::Gesture::Stab);
    EXPECT_EQ(Swing(r, from, to, 0.25f, back), T::Gesture::None); // within the cooldown
    for (int i = 0; i < static_cast<int>(T::CooldownS / Dt); ++i) {
        r.Step(Make(Palm, from, false, back));
    }
    EXPECT_EQ(Swing(r, from, to, 0.25f, back), T::Gesture::Stab);
}

TEST(TrickWeapons, AGripHeldAsItArmsIsNoSqueeze) {
    const V at{Fist.x - 0.06f, Fist.y, Fist.z};
    Rig r;
    r.kind = T::Kind::DualBlades;
    r.ht.Arm(true);
    EXPECT_EQ(r.Step(Make(at, Fist, true)).gesture, T::Gesture::None);
    r.Step(Make(at, Fist, false));
    EXPECT_EQ(r.Step(Make(at, Fist, true)).gesture, T::Gesture::Grab);
}

// ---- The right grip ----

TEST(TrickWeapons, WhatTheRightGripDoes) {
    using U = T::GripUse;
    for (const int grab : {20, 50, 51, 80, 81, 100}) {
        EXPECT_TRUE(T::GrabFamily(grab)) << grab;
        EXPECT_EQ(T::GripUseOf(grab, false), U::Free) << grab;
        EXPECT_EQ(T::GripUseOf(grab, true), U::Transform) << grab; // (letting go turns it back)
    }
    EXPECT_FALSE(T::GrabFamily(260)); // the Holy Moonlight Sword stays one-handed (the user's)
    EXPECT_EQ(T::GripUseOf(260, false), U::Transform);
    EXPECT_EQ(T::GripUseOf(40, false), U::Free);
    EXPECT_EQ(T::GripUseOf(270, true), U::Transform);
    EXPECT_EQ(T::GripUseOf(290, false), U::Arm);
    EXPECT_EQ(T::GripUseOf(290, true), U::Transform);
    EXPECT_EQ(T::GripUseOf(130, false), U::Lift);
    EXPECT_EQ(T::GripUseOf(130, true), U::Lift);
    EXPECT_EQ(T::GripUseOf(280, false), U::Hold);
    EXPECT_EQ(T::GripUseOf(280, true), U::Transform); // (closing: the game's L1)
    EXPECT_EQ(T::GripUseOf(90, false), U::Hold);
    EXPECT_EQ(T::GripUseOf(70, false), U::Transform);
    EXPECT_EQ(T::HoldSOf(90), 2.0f);
    // The left hand's own L2, known without learning: the fist (the guns' and shields' punch),
    // the Torch, the Fist of Gratia; others are learned in the game.
    EXPECT_EQ(T::KnownLeftRow(T::FistVariation * 10), 800);
    EXPECT_EQ(T::KnownLeftRow(20000), 800);
    EXPECT_EQ(T::KnownLeftRow(34000), 800);
    EXPECT_EQ(T::KnownLeftRow(4001), -1);
    EXPECT_EQ(T::KnownLeftRow(1011), -1);
    // Simon's Bowblade: held 0.5 s into the bow (no accidental squeeze), a squeeze back.
    EXPECT_EQ(T::GripUseOf(320, false), U::Hold);
    EXPECT_EQ(T::GripUseOf(320, true), U::Transform);
    EXPECT_EQ(T::HoldSOf(320), 0.5f);
    EXPECT_TRUE(T::CoolsDown(130));
    EXPECT_TRUE(T::CoolsDown(280));
    EXPECT_FALSE(T::CoolsDown(90));
}

// The right grip pressed (latched) with the right hand at `p`, the weapon along `axis`.
T::Frame Grip(bool grip, V p = Fist, V axis = {}) {
    T::Frame f = Make(Palm, p, false, axis);
    f.right.grip = grip;
    return f;
}

TEST(TrickWeapons, TheRightGripFreesTheLeftHandUntilUsedOrLate) {
    using E = T::RightGrip::Event;
    const T::Settings s{};
    T::RightGrip g;
    EXPECT_EQ(g.Step(Grip(false), T::GripUse::Free, 81, s).event, E::None);
    auto o = g.Step(Grip(true), T::GripUse::Free, 81, s);
    EXPECT_EQ(o.event, E::Freed);
    EXPECT_TRUE(o.ready);
    EXPECT_TRUE(g.Step(Grip(false), T::GripUse::Free, 81, s).ready); // a squeeze, not a hold
    // A second squeeze takes it back.
    EXPECT_EQ(g.Step(Grip(true), T::GripUse::Free, 81, s).event, E::Unready);
    g.Step(Grip(false), T::GripUse::Free, 81, s);
    EXPECT_EQ(g.Step(Grip(true), T::GripUse::Free, 81, s).event, E::Freed);
    // Used (the grab transformed it): idle.
    g.Consume();
    EXPECT_FALSE(g.Step(Grip(true), T::GripUse::Free, 81, s).ready);
    // Unused: back after FreeS.
    g.Step(Grip(false), T::GripUse::Free, 81, s);
    g.Step(Grip(true), T::GripUse::Free, 81, s);
    bool late = false;
    for (int i = 0; i < static_cast<int>((T::FreeS + 0.1f) / Dt); ++i) {
        late = late || g.Step(Grip(false), T::GripUse::Free, 81, s).event == E::Unready;
    }
    EXPECT_TRUE(late);
    // The weapon or its form changed: forgotten, silently.
    g.Step(Grip(true), T::GripUse::Arm, 290, s);
    EXPECT_FALSE(g.Step(Grip(false), T::GripUse::Transform, 290, s).ready);
}

TEST(TrickWeapons, BloodletterGripStaysArmedUntilReleaseOrConsumed) {
    T::RightGrip grip;
    const T::Settings settings{};
    EXPECT_TRUE(grip.Step(Grip(true), T::GripUse::Arm, 290, settings).ready);
    for (int i = 0; i < 900; ++i) {
        EXPECT_TRUE(grip.Step(Grip(true), T::GripUse::Arm, 290, settings).ready);
    }
    EXPECT_FALSE(grip.Step(Grip(false), T::GripUse::Arm, 290, settings).ready);
    EXPECT_TRUE(grip.Step(Grip(true), T::GripUse::Arm, 290, settings).ready);
    grip.Consume();
    EXPECT_FALSE(grip.Step(Grip(true), T::GripUse::Arm, 290, settings).ready);
    grip.Step(Grip(false), T::GripUse::Arm, 290, settings);
    EXPECT_TRUE(grip.Step(Grip(true), T::GripUse::Arm, 290, settings).ready);
}

TEST(TrickWeapons, StakeAttackRowKeepsTheTriggerForAChargedPunch) {
    EXPECT_TRUE(T::StakePunch(1100, true, true));
    EXPECT_FALSE(T::StakePunch(1100, true, false)); // bullet route stays available
    EXPECT_FALSE(T::StakePunch(1100, false, true));
    EXPECT_FALSE(T::StakePunch(1000, true, true));
}

TEST(TrickWeapons, RifleSpearChargesInThreeTenthsAndOnlyFiresOnRelease) {
    T::Blast blast;
    const auto& spear = *T::ShotWeaponOf(1000);
    EXPECT_FLOAT_EQ(spear.cadence_s, 1.0f);
    blast.Step(true, true, Dt, spear.charge_s, spear.cadence_s);
    for (int i = 0; i < 20; ++i) {
        EXPECT_NE(blast.Step(true, true, Dt, spear.charge_s, spear.cadence_s).event,
                  T::Blast::Event::Full);
    }
    bool full = false;
    for (int i = 0; i < 9; ++i) {
        full |= blast.Step(true, true, Dt, spear.charge_s, spear.cadence_s).event ==
                T::Blast::Event::Full;
    }
    EXPECT_TRUE(full);
    EXPECT_EQ(blast.Step(false, true, Dt, spear.charge_s, spear.cadence_s).event,
              T::Blast::Event::Fire);
    EXPECT_EQ(blast.Step(true, true, Dt, spear.charge_s, spear.cadence_s).event,
              T::Blast::Event::Cooling);
    // A new charge is available after the one-second cooldown.
    blast.Step(false, true, 1.0f, spear.charge_s, spear.cadence_s);
    EXPECT_EQ(blast.Step(true, true, Dt, spear.charge_s, spear.cadence_s).event,
              T::Blast::Event::Start);
}

TEST(TrickWeapons, HeldTheGripChargesThenCoolsDown) {
    using E = T::RightGrip::Event;
    T::Settings s{};
    s.charge_cooldown_s = 2.0f;
    T::RightGrip g;
    g.Step(Grip(false), T::GripUse::Hold, 280, s);
    EXPECT_EQ(g.Step(Grip(true), T::GripUse::Hold, 280, s).event, E::Start);
    // Let go half way: nothing.
    E last = E::None;
    for (int i = 0; i < static_cast<int>(0.7f / Dt); ++i) {
        last = g.Step(Grip(true), T::GripUse::Hold, 280, s).event;
    }
    EXPECT_EQ(last, E::None);
    EXPECT_EQ(g.Step(Grip(false), T::GripUse::Hold, 280, s).event, E::Short);
    // Held through: it fires at HoldSOf, once.
    g.Step(Grip(true), T::GripUse::Hold, 280, s);
    int fired = 0;
    float at = 0.0f;
    for (int i = 1; i < static_cast<int>(2.0f / Dt); ++i) {
        const auto o = g.Step(Grip(true), T::GripUse::Hold, 280, s);
        if (o.event == E::Fire) {
            ++fired;
            at = static_cast<float>(i) * Dt;
        }
    }
    EXPECT_EQ(fired, 1);
    EXPECT_NEAR(at, T::HoldSOf(280), 2.0f * Dt);
    // Again within the cooldown: the deny; after it, a new charge.
    g.Step(Grip(false), T::GripUse::Hold, 280, s);
    EXPECT_EQ(g.Step(Grip(true), T::GripUse::Hold, 280, s).event, E::Cooling);
    for (int i = 0; i < static_cast<int>(2.0f / Dt); ++i) {
        g.Step(Grip(false), T::GripUse::Hold, 280, s);
    }
    EXPECT_EQ(g.Step(Grip(true), T::GripUse::Hold, 280, s).event, E::Start);
    // The Beast Claw: 2 s, and no cooldown.
    T::RightGrip claw;
    claw.Step(Grip(true), T::GripUse::Hold, 90, s);
    for (int i = 0; i < static_cast<int>(2.1f / Dt); ++i) {
        claw.Step(Grip(true), T::GripUse::Hold, 90, s);
    }
    claw.Step(Grip(false), T::GripUse::Hold, 90, s);
    EXPECT_EQ(claw.Step(Grip(true), T::GripUse::Hold, 90, s).event, E::Start);
}

// Tonitrus: the grip held and the weapon lifted up over the head, pointing up.
TEST(TrickWeapons, TonitrusLiftedUpCharges) {
    using E = T::RightGrip::Event;
    const T::Settings s{};
    const V up{0.0f, 1.0f, 0.0f};
    const V low{0.25f, 1.30f, -0.30f};
    const V high{0.25f, Head.y + 0.15f, -0.20f};
    const auto lift = [&](T::RightGrip& g, V from, V to, V axis) {
        E got = E::None;
        g.Step(Grip(false, from, axis), T::GripUse::Lift, 130, s);
        for (int i = 0; i <= 40; ++i) {
            const V p = from + (to - from) * (static_cast<float>(i) / 40.0f);
            const auto o = g.Step(Grip(true, p, axis), T::GripUse::Lift, 130, s);
            got = o.event == E::Fire ? E::Fire : got;
        }
        return got;
    };
    T::RightGrip g;
    EXPECT_EQ(lift(g, low, high, up), E::Fire);
    // Within the cooldown: the deny.
    g.Step(Grip(false, high, up), T::GripUse::Lift, 130, s);
    EXPECT_EQ(g.Step(Grip(true, high, up), T::GripUse::Lift, 130, s).event, E::Cooling);
    // Raised with the wrist's natural tilt (50 degrees off vertical): counts.
    T::RightGrip tilted;
    const V tilt = VR::Body::Normalize(V{0.0f, 0.64f, -0.77f});
    EXPECT_EQ(lift(tilted, low, high, tilt), E::Fire);
    // Lifted pointing forward, or not up past the eyes, or not held: no charge.
    T::RightGrip level;
    EXPECT_EQ(lift(level, low, high, {0.0f, 0.0f, -1.0f}), E::None);
    T::RightGrip short_lift;
    EXPECT_EQ(lift(short_lift, low, {low.x, Head.y - 0.05f, low.z}, up), E::None);
    T::RightGrip loose;
    loose.Step(Grip(false, low, up), T::GripUse::Lift, 130, s);
    for (int i = 0; i <= 40; ++i) {
        const V p = low + (high - low) * (static_cast<float>(i) / 40.0f);
        EXPECT_NE(loose.Step(Grip(false, p, up), T::GripUse::Lift, 130, s).event, E::Fire);
    }
    // Squeezed already high: lower it a little and lift again.
    T::RightGrip again;
    const V top{high.x, Head.y + 0.10f, high.z};
    again.Step(Grip(false, top, up), T::GripUse::Lift, 130, s);
    EXPECT_EQ(again.Step(Grip(true, top, up), T::GripUse::Lift, 130, s).event, E::Start);
    EXPECT_EQ(lift(again, {top.x, top.y - 0.30f, top.z}, {top.x, top.y + 0.05f, top.z}, up),
              E::Fire);
}

// Form B only while both hands hold it: letting go turns it back after the grace, never during a
// swing, and a grab again in time keeps it.
TEST(TrickWeapons, FormBLastsWhileBothHandsHoldIt) {
    T::HoldForm h;
    EXPECT_FALSE(h.Step(Dt, true, true, false, false));
    EXPECT_FALSE(h.Step(Dt, true, false, true, false)); // let go
    bool back = false;
    int frames = 0;
    while (!back && frames < 100) {
        back = h.Step(Dt, true, false, false, false);
        ++frames;
    }
    EXPECT_TRUE(back);
    EXPECT_NEAR(frames * Dt, T::ReleaseGraceS, 2.0f * Dt);
    // Grabbed again in time: kept.
    T::HoldForm regrab;
    regrab.Step(Dt, true, false, true, false);
    regrab.Step(Dt, true, false, false, false);
    EXPECT_FALSE(regrab.Step(Dt, true, true, false, false));
    for (int i = 0; i < 60; ++i) {
        EXPECT_FALSE(regrab.Step(Dt, true, false, false, false));
    }
    // During a swing: the whole grace after it (time to take the handle again).
    T::HoldForm swing;
    swing.Step(Dt, true, false, true, true);
    for (int i = 0; i < 60; ++i) {
        EXPECT_FALSE(swing.Step(Dt, true, false, false, true));
    }
    int after = 0;
    while (!swing.Step(Dt, true, false, false, false) && after < 100) {
        ++after;
    }
    EXPECT_NEAR((after + 1) * Dt, T::ReleaseGraceS, 2.0f * Dt);
    // ... and taken again within it: kept.
    T::HoldForm retaken;
    retaken.Step(Dt, true, false, true, true);
    retaken.Step(Dt, true, false, false, false);
    EXPECT_FALSE(retaken.Step(Dt, true, true, false, false));
    for (int i = 0; i < 60; ++i) {
        EXPECT_FALSE(retaken.Step(Dt, true, false, false, false));
    }
    // Let go before the transform into B finished: back once it is in B (and the grace over).
    T::HoldForm early;
    early.Step(Dt, false, false, true, true);
    for (int i = 0; i < 30; ++i) {
        EXPECT_FALSE(early.Step(Dt, false, false, false, true));
    }
    bool reverted = false;
    for (int i = 0; i < 60 && !reverted; ++i) {
        reverted = early.Step(Dt, true, false, false, false);
    }
    EXPECT_TRUE(reverted);
    // Never in B (refused): nothing.
    T::HoldForm never;
    never.Step(Dt, false, false, true, false);
    for (int i = 0; i < 60; ++i) {
        EXPECT_FALSE(never.Step(Dt, false, false, false, false));
    }
}

TEST(TrickWeapons, StakeBlastRelocatesOnlyItsOwnNativeSphere) {
    std::array<std::uint8_t, 0x300> entry{};
    const auto put = [&](std::size_t at, auto value) {
        std::memcpy(entry.data() + at, &value, sizeof(value));
    };
    put(0x8C, std::int32_t{101100930});
    put(0xA0, 1.0f);
    put(0x188, std::int32_t{60});
    put(0x18C, std::int32_t{-1});
    auto expected = entry;
    const std::int32_t tip = 11206;
    std::memcpy(expected.data() + 0x188, &tip, sizeof(tip));
    ASSERT_TRUE(T::RelocateStakeBlast(entry));
    EXPECT_EQ(entry, expected); // damage, radius, and every other field are untouched
    EXPECT_TRUE(T::RelocateStakeBlast(entry)); // refresh is idempotent
    put(0x8C, std::int32_t{101100910});
    const auto other = entry;
    EXPECT_FALSE(T::RelocateStakeBlast(entry));
    EXPECT_EQ(entry, other);
    put(0x8C, std::int32_t{101100930});
    put(0x188, std::int32_t{300});
    EXPECT_FALSE(T::RelocateStakeBlast(entry));
    put(0x188, std::int32_t{60});
    put(0xA0, 0.4f);
    EXPECT_FALSE(T::RelocateStakeBlast(entry));
    EXPECT_FALSE(T::RelocateStakeBlast(std::span{entry}.first(32)));
}

TEST(TrickWeapons, StakeReleaseOpensOnePoweredAttackWithoutAPunch) {
    T::Blast blast;
    VR::Melee::WindowTracker tracker;
    VR::Melee::TrackerConfig cfg{};
    VR::Melee::TrackerInput in{};
    in.dt = Dt;
    in.swings = true;
    for (int frame = 0; frame < 180; ++frame) {
        const auto charge = blast.Step(true, true, Dt, T::BlastChargeS, T::BlastCooldownS);
        EXPECT_NE(charge.event, T::Blast::Event::Fire);
        EXPECT_FALSE(tracker.Step(in, cfg).opened);
    }
    ASSERT_EQ(blast.Step(false, true, Dt, T::BlastChargeS, T::BlastCooldownS).event,
              T::Blast::Event::Fire);
    ASSERT_TRUE(tracker.OpenPowered(in, cfg).opened);
    EXPECT_TRUE(tracker.Current().powered);
    EXPECT_EQ(tracker.Current().kind, VR::Melee::MoveKind::Charged);
    EXPECT_EQ(tracker.Current().tip_speed, 0.0f);
    for (int frame = 0; frame < 90; ++frame) {
        EXPECT_NE(blast.Step(false, true, Dt, T::BlastChargeS, T::BlastCooldownS).event,
                  T::Blast::Event::Fire);
    }
}

TEST(TrickWeapons, ChargedStakeCannotDischargeAfterContextLossOrWhileBlocked) {
    for (const bool reset : {false, true}) {
        T::Blast blast;
        for (int frame = 0; frame < 180; ++frame) {
            blast.Step(true, true, Dt, T::BlastChargeS, T::BlastCooldownS);
        }
        if (reset) {
            blast.Reset(true);
        } else {
            blast.Step(true, false, Dt, T::BlastChargeS, T::BlastCooldownS);
        }
        EXPECT_NE(blast.Step(false, true, Dt, T::BlastChargeS, T::BlastCooldownS).event,
                  T::Blast::Event::Fire);
        EXPECT_EQ(blast.Step(true, true, Dt, T::BlastChargeS, T::BlastCooldownS).event,
                  T::Blast::Event::Start);
        EXPECT_EQ(blast.Step(false, true, Dt, T::BlastChargeS, T::BlastCooldownS).event,
                  T::Blast::Event::Short);
    }
}

TEST(TrickWeapons, TheBlastChargesAndFiresOnTheRelease) {
    using E = T::Blast::Event;
    T::Blast b;
    const float charge = T::BlastChargeS;
    const float cool = T::BlastCooldownS;
    EXPECT_EQ(b.Step(true, true, Dt, charge, cool).event, E::Start);
    E full = E::None;
    for (int i = 0; i < static_cast<int>((charge + 0.05f) / Dt); ++i) {
        const auto o = b.Step(true, true, Dt, charge, cool);
        full = o.event == E::Full ? E::Full : full;
        EXPECT_TRUE(o.charging);
    }
    EXPECT_EQ(full, E::Full);
    EXPECT_EQ(b.Step(false, true, Dt, charge, cool).event, E::Fire);
    // Within the cooldown: refused; after it, again.
    EXPECT_EQ(b.Step(true, true, Dt, charge, cool).event, E::Cooling);
    for (int i = 0; i < static_cast<int>(cool / Dt) + 2; ++i) {
        b.Step(false, true, Dt, charge, cool);
    }
    EXPECT_EQ(b.Step(true, true, Dt, charge, cool).event, E::Start);
    // Let go early: nothing fired.
    EXPECT_EQ(b.Step(false, true, Dt, charge, cool).event, E::Short);
    // Not allowed (swings held back): refused; taken away mid-charge: dropped silently.
    EXPECT_EQ(b.Step(true, false, Dt, charge, cool).event, E::Refused);
    b.Step(false, true, Dt, charge, cool);
    b.Step(true, true, Dt, charge, cool);
    const auto o = b.Step(true, false, Dt, charge, cool);
    EXPECT_EQ(o.event, E::None);
    EXPECT_FALSE(o.charging);
}

// The Whirligig Saw: either trigger with both hands on it spins it up, let go it spins down;
// the trigger alone (one hand) does nothing; a hand off the handle stops it.
TEST(TrickWeapons, SawRightGripDoesNothingInEitherForm) {
    EXPECT_TRUE(T::GrabFamily(T::SawFamily));
    EXPECT_FALSE(T::UsesL1(T::SawFamily));
    T::RightGrip grip;
    for (const bool form_b : {false, true}) {
        const auto use = T::GripUseOf(T::SawFamily, form_b);
        EXPECT_EQ(use, T::GripUse::None);
        for (int frame = 0; frame < 240; ++frame) {
            const auto o = grip.Step(Grip(frame % 120 != 0), use, T::SawFamily, T::Settings{});
            EXPECT_EQ(o.event, T::RightGrip::Event::None);
            EXPECT_FALSE(o.ready);
            EXPECT_FALSE(o.charging);
        }
    }
}

TEST(TrickWeapons, SawAcceptsEitherHeldTriggerThroughGrabAndHandover) {
    for (const bool left : {false, true}) {
        T::SawSpin saw;
        EXPECT_EQ(saw.Step(left, !left, false, Dt).rev, 0.0f);
        EXPECT_EQ(saw.Step(left, !left, true, Dt).event, T::SawSpin::Event::Start);
        for (int i = 0; i < 60; ++i) {
            saw.Step(left, !left, true, Dt);
        }
        EXPECT_EQ(saw.Step(true, true, true, Dt).rev, 1.0f);
        const auto handover = saw.Step(!left, left, true, Dt);
        EXPECT_EQ(handover.event, T::SawSpin::Event::None);
        EXPECT_EQ(handover.rev, 1.0f);
        EXPECT_EQ(saw.Step(false, false, true, Dt).event, T::SawSpin::Event::Stop);
        EXPECT_EQ(saw.Step(left, !left, true, Dt).event, T::SawSpin::Event::Start);
        EXPECT_EQ(saw.Step(left, !left, false, Dt).event, T::SawSpin::Event::Stop);
        saw.Reset();
        EXPECT_FALSE(saw.Spinning());
    }
}

TEST(TrickWeapons, SawDamageWaitsForRevThenRepeatsWithoutHandMovement) {
    T::SawSpin saw;
    T::SawAttackClock attack;
    int hits = 0;
    int last_hit = -1;
    for (int frame = 0; frame < 450; ++frame) {
        const auto o = saw.Step(false, true, true, Dt);
        if (attack.Step(o.rev, true, Dt)) {
            EXPECT_EQ(o.rev, 1.0f);
            if (last_hit >= 0) {
                EXPECT_NEAR((frame - last_hit) * Dt, T::SawHitS, Dt);
            }
            last_hit = frame;
            ++hits;
        }
        EXPECT_GE(attack.Time(), 0.0f);
        EXPECT_LT(attack.Time(), T::SawLoopS);
    }
    EXPECT_GE(hits, 10);
    const float time = attack.Time();
    const auto coast = saw.Step(false, false, true, Dt);
    EXPECT_FALSE(attack.Step(coast.rev, false, Dt));
    EXPECT_FALSE(attack.Active());
    EXPECT_GT(coast.rev, 0.0f);
    EXPECT_NE(attack.Time(), time); // blade coasts, contact damage stops immediately
}

TEST(TrickWeapons, SawClockHasNoBurstAfterHitchAndStaleRequestsExpire) {
    T::SawAttackClock attack;
    EXPECT_TRUE(attack.Step(1.0f, true, Dt));
    EXPECT_FALSE(attack.Step(1.0f, true, 30.0f)); // capped, no catch-up hits
    EXPECT_FALSE(attack.Step(1.0f, true, -1.0f));
    EXPECT_FALSE(attack.Step(1.0f, false, Dt));
    EXPECT_TRUE(attack.Step(1.0f, true, Dt));
    constexpr std::int64_t start = 1'000'000'000;
    EXPECT_TRUE(T::SawFresh(start, start));
    EXPECT_TRUE(T::SawFresh(start, start + T::SawFreshNs - 1));
    EXPECT_FALSE(T::SawFresh(start, start + T::SawFreshNs));
    EXPECT_FALSE(T::SawFresh(0, start));
    EXPECT_FALSE(T::SawFresh(start, start - 1));
}

TEST(TrickWeapons, TheSawSpinsWhileBothHandsHoldTheTrigger) {
    using E = T::SawSpin::Event;
    T::SawSpin saw;
    for (int i = 0; i < 30; ++i) {
        const auto o = saw.Step(true, false, Dt); // one hand: nothing
        EXPECT_EQ(o.event, E::None);
        EXPECT_EQ(o.level, 0.0f);
    }
    EXPECT_EQ(saw.Step(false, true, Dt).event, E::None);
    EXPECT_EQ(saw.Step(true, true, Dt).event, E::Start);
    int full_at = -1;
    float lo = 1.0f;
    float hi = 0.0f;
    for (int i = 2; i < 90; ++i) {
        const auto o = saw.Step(true, true, Dt);
        if (o.event == E::Full) {
            full_at = i;
        }
        if (o.rev >= 1.0f) {
            lo = std::min(lo, o.level);
            hi = std::max(hi, o.level);
        }
    }
    EXPECT_NEAR(full_at * Dt, T::SpinUpS, 2.0f * Dt);
    EXPECT_GT(hi - lo, T::SpinGrain); // the teeth's grain
    EXPECT_LE(hi, 0.8f);              // under a hit's feel
    EXPECT_EQ(saw.Step(false, true, Dt).event, E::Stop);
    int still_at = -1;
    float prev = 1.0f;
    for (int i = 2; i < 90 && still_at < 0; ++i) {
        const auto o = saw.Step(false, true, Dt);
        EXPECT_LE(o.rev, prev); // winding down
        prev = o.rev;
        if (o.event == E::Still) {
            still_at = i;
            EXPECT_EQ(o.level, 0.0f);
        }
    }
    EXPECT_NEAR(still_at * Dt, T::SpinDownS, 2.0f * Dt);
    EXPECT_FALSE(saw.Spinning());
    // A hand off the handle mid-spin stops it, the trigger still held.
    saw.Step(true, true, Dt);
    EXPECT_TRUE(saw.Spinning());
    EXPECT_EQ(saw.Step(true, false, Dt).event, E::Stop);
}

// Out of its form the blast's cooldown still runs down; a charge's cooldown is cancelled when its
// transform never reached the game.
TEST(TrickWeapons, CooldownsRunOnAndCancel) {
    using E = T::Blast::Event;
    T::Blast b;
    b.Step(true, true, Dt, 0.0f, 3.0f); // (no charge time: full at once)
    b.Step(true, true, Dt, 0.0f, 3.0f);
    EXPECT_EQ(b.Step(false, true, Dt, 0.0f, 3.0f).event, E::Fire);
    for (int i = 0; i < static_cast<int>(3.1f / Dt); ++i) {
        b.Idle(false, Dt); // the weapon turned back meanwhile
    }
    EXPECT_EQ(b.Step(true, true, Dt, 0.0f, 3.0f).event, E::Start);

    using G = T::RightGrip::Event;
    T::Settings s{};
    T::RightGrip g;
    g.Step(Grip(true), T::GripUse::Hold, 280, s);
    G fired = G::None;
    for (int i = 0; i < static_cast<int>(2.0f / Dt) && fired != G::Fire; ++i) {
        fired = g.Step(Grip(true), T::GripUse::Hold, 280, s).event;
    }
    ASSERT_EQ(fired, G::Fire);
    g.CancelCooldown(); // the L1 never went out
    g.Step(Grip(false), T::GripUse::Hold, 280, s);
    EXPECT_EQ(g.Step(Grip(true), T::GripUse::Hold, 280, s).event, G::Start);
}

TEST(TrickWeapons, TheCannedL2List) {
    EXPECT_TRUE(T::InKeyList("bloodletter,kos_parasite", "kos_parasite"));
    EXPECT_TRUE(T::InKeyList(" bloodletter , holy_moonlight_sword", "holy_moonlight_sword"));
    EXPECT_FALSE(T::InKeyList("bloodletter", "blood"));
    EXPECT_FALSE(T::InKeyList("", "bloodletter"));
    EXPECT_FALSE(T::InKeyList("bloodletter", ""));
    EXPECT_TRUE(T::InKeyList(T::Settings{}.l2_canned, "bloodletter"));
}

TEST(TrickWeapons, TheShieldIsUpWhileHeldInFront) {
    const T::Settings s{};
    T::ShieldRaise shield;
    const V up{-0.10f, 1.35f, -0.30f};
    T::Frame f = Make(up, Fist, false);
    EXPECT_FALSE(shield.Step(f, s)); // not at once: held there a moment
    for (int i = 0; i < 10; ++i) {
        shield.Step(f, s);
    }
    EXPECT_TRUE(shield.Up());
    // A hand resting in front of the belly: no guard.
    T::ShieldRaise belly;
    for (int i = 0; i < 20; ++i) {
        belly.Step(Make({-0.10f, 1.08f, -0.32f}, Fist, false), s);
    }
    EXPECT_FALSE(belly.Up());
    // Eased back a little: still up; lowered: down, after a moment out of the zone (a duck or a
    // lean across its edge never flickers it).
    EXPECT_TRUE(shield.Step(Make({-0.10f, 1.35f, -(s.shield_m - 0.03f)}, Fist, false), s));
    EXPECT_TRUE(shield.Step(Make({-0.10f, 1.35f, -(s.shield_m - 0.12f)}, Fist, false), s));
    const T::Frame lowered = Make({-0.20f, 0.95f, -0.05f}, Fist, false);
    for (int i = 0; i < 15; ++i) { // 0.17 s
        EXPECT_TRUE(shield.Step(lowered, s));
    }
    for (int i = 0; i < 5; ++i) {
        shield.Step(lowered, s);
    }
    EXPECT_FALSE(shield.Up());
    T::ShieldRaise dip; // ... and back in time: still up
    for (int i = 0; i < 12; ++i) {
        dip.Step(f, s);
    }
    for (int i = 0; i < 10; ++i) {
        dip.Step(lowered, s);
    }
    EXPECT_TRUE(dip.Step(f, s));
    for (int i = 0; i < 15; ++i) {
        EXPECT_TRUE(dip.Step(lowered, s));
    }
    // Held at the upper chest (36 cm below the eyes, 20 cm out): up.
    T::ShieldRaise chest;
    for (int i = 0; i < 12; ++i) {
        chest.Step(Make({-0.10f, 1.24f, -0.20f}, Fist, false), s);
    }
    EXPECT_TRUE(chest.Up());
    // A ready stance lower down (the hand 42 cm below the eyes): no guard.
    T::ShieldRaise ready;
    for (int i = 0; i < 20; ++i) {
        ready.Step(Make({-0.10f, 1.18f, -0.35f}, Fist, false), s);
    }
    EXPECT_FALSE(ready.Up());
    // Above the head: no.
    T::ShieldRaise high;
    for (int i = 0; i < 10; ++i) {
        high.Step(Make({-0.10f, 1.85f, -0.30f}, Fist, false), s);
    }
    EXPECT_FALSE(high.Up());
    // The hand lost a moment (the other hand crossing it): still up; lost for longer: down.
    for (int i = 0; i < 10; ++i) {
        shield.Step(f, s);
    }
    T::Frame lost = f;
    lost.left.valid = false;
    for (int i = 0; i < 9; ++i) { // 0.1 s
        EXPECT_TRUE(shield.Step(lost, s));
    }
    for (int i = 0; i < 30; ++i) {
        shield.Step(lost, s);
    }
    EXPECT_FALSE(shield.Up());
}

TEST(TrickWeapons, TheWaveFliesOutThroughTheBlade) {
    const V chest{0.0f, 1.3f, 0.0f};
    const V fwd{0.0f, 0.0f, -1.0f};
    const V ahead = T::WaveAim(chest, {0.0f, 1.3f, -0.8f}, fwd);
    EXPECT_NEAR(ahead.z, -1.0f, 1e-4f);
    const V left = T::WaveAim(chest, {-0.6f, 1.3f, -0.6f}, fwd);
    EXPECT_NEAR(left.x, -std::sqrt(0.5f), 1e-3f);
    // Swung low: never steeper than 20 degrees down.
    const V low = T::WaveAim(chest, {0.0f, 0.5f, -0.5f}, fwd);
    EXPECT_NEAR(std::asin(low.y) / VR::Body::Deg, -20.0f, 0.1f);
    // Straight above the chest: forward.
    const V over = T::WaveAim(chest, {0.0f, 2.2f, -0.01f}, fwd);
    EXPECT_LT(over.z, -0.9f);
}

// moon_wave: a swing sweeping through the line of sight sends the wave where you look; one beside
// you, or up past your view, keeps its own line at its fastest.
TEST(TrickWeapons, TheWaveGoesWhereYouLookWhenTheSwingCrossesIt) {
    const V head{0.0f, 1.6f, 0.0f};
    const V chest{0.0f, 1.28f, 0.0f};
    const V fwd{0.0f, 0.0f, -1.0f};
    const V gaze = VR::Body::Normalize(V{0.3f, -0.1f, -1.0f}); // looking a little right and down
    // A sweep from the left to the right at eye level, fastest at its start (far left).
    T::WaveSweep sweep{};
    for (int i = 0; i <= 20; ++i) {
        const float a = (-60.0f + 6.0f * static_cast<float>(i)) * VR::Body::Deg;
        const V mid{0.6f * std::sin(a), 1.5f, -0.6f * std::cos(a)};
        sweep.Step(head, gaze, chest, mid, fwd, i == 0 ? 9.0f : 5.0f);
    }
    EXPECT_TRUE(sweep.AtGaze());
    const V at = sweep.Aim(head, gaze, fwd);
    EXPECT_NEAR(at.x, gaze.x, 1e-3f);
    EXPECT_NEAR(at.y, gaze.y, 1e-3f);
    // Looking forward, swinging up on your right (the blade never crosses your view): the
    // swing's own line at its fastest.
    T::WaveSweep up{};
    for (int i = 0; i <= 10; ++i) {
        const V mid{0.7f, 1.0f + 0.1f * static_cast<float>(i), -0.2f};
        up.Step(head, fwd, chest, mid, fwd, i == 5 ? 8.0f : 3.0f);
    }
    EXPECT_FALSE(up.AtGaze());
    const V own = up.Aim(head, fwd, fwd);
    EXPECT_GT(own.x, 0.9f); // out to the right, where the blade went
    // A sweep beside you from front right to behind, fastest at its start: the line it had then
    // (ahead and to the right), not the one it ended on.
    T::WaveSweep side{};
    for (int i = 0; i <= 10; ++i) {
        const V mid{0.6f, 1.4f, -0.3f + 0.06f * static_cast<float>(i)};
        side.Step(head, fwd, chest, mid, fwd, i == 0 ? 9.0f : 4.0f);
    }
    EXPECT_FALSE(side.AtGaze());
    EXPECT_LT(side.Aim(head, fwd, fwd).z, -0.3f);
    // Looking up at the sky through a chop: no steeper than a wave goes (20 degrees).
    T::WaveSweep sky{};
    const V high = VR::Body::Normalize(V{0.0f, 1.0f, -0.5f});
    sky.Step(head, high, chest, head + high * 0.6f, fwd, 6.0f);
    EXPECT_TRUE(sky.AtGaze());
    EXPECT_NEAR(std::asin(sky.Aim(head, high, fwd).y) / VR::Body::Deg, 20.0f, 0.1f);
}

// A blow on the shield: its size from the guard reaction the game plays.
TEST(TrickWeapons, TheBlowsSizeFromTheGuardReaction) {
    EXPECT_EQ(T::GuardBlowSize(10000), 0);
    EXPECT_EQ(T::GuardBlowSize(10002), 0);
    EXPECT_EQ(T::GuardBlowSize(10012), 1);
    EXPECT_EQ(T::GuardBlowSize(2010021), 2); // any TAE group
    EXPECT_EQ(T::GuardBlowSize(10030), 3);   // the guard broken
    EXPECT_EQ(T::GuardBlowSize(10093), 3);
    EXPECT_EQ(T::GuardBlowSize(20000), 0);   // not a guard reaction
    EXPECT_EQ(T::GuardBlowSize(-1), 0);
}

// The hum: gentle while held, revving with the swing's speed, only for the weapons that run.
TEST(TrickWeapons, TheHumRevsWithTheSwing) {
    const T::Hum saw = T::HumOf(3100);
    EXPECT_GE(saw.idle, 0.10f);           // felt: a steady level gets no haptic floor
    EXPECT_LE(saw.idle + saw.rev, 0.36f); // a full rev under a hit's knock
    EXPECT_GE(T::HumOf(2800).idle, 0.10f);
    EXPECT_EQ(T::HumOf(700).idle, 0.0f);
    EXPECT_EQ(T::HumRev(0.5f), 0.0f); // held still, a slow move
    EXPECT_NEAR(T::HumRev(3.0f), 0.5f, 1e-6f);
    EXPECT_EQ(T::HumRev(9.0f), 1.0f); // a full swing
}

// melee_left: which weapons attack with the left hand, and whose a row's capsule is.
TEST(TrickWeapons, TheLeftHandsWeaponsAndItsCapsule) {
    for (const std::int32_t v : {400, 2700, 900, 101}) {
        EXPECT_TRUE(T::LeftPartWeapon(v)) << v;
    }
    EXPECT_FALSE(T::LeftPartWeapon(700)); // the Saw Cleaver: one blade
    for (const std::int32_t v : {2000, 3400, 100}) {
        EXPECT_TRUE(T::LeftMeleeWeapon(v)) << v;
    }
    EXPECT_FALSE(T::LeftMeleeWeapon(1400)); // a pistol: its punch is the fist's
    EXPECT_FALSE(T::LeftMeleeWeapon(1900)); // a shield
    EXPECT_TRUE(T::LeftMeleeWeapon(T::FistVariation));
    for (const std::int32_t v : {1400, 1410}) {
        EXPECT_TRUE(T::PunchGun(v)) << v;
    }
    for (const std::int32_t v : {600, 610, 1500, 3300, 1800, 100}) {
        EXPECT_FALSE(T::PunchGun(v)) << v; // the long guns, the cannons, the sprayers, the fist
    }
    EXPECT_EQ(T::SideOf(0.25f, 0.70f), T::CapsuleSide::Left);
    EXPECT_EQ(T::SideOf(0.70f, 0.25f), T::CapsuleSide::Right);
    EXPECT_EQ(T::SideOf(0.30f, 0.35f), T::CapsuleSide::Unsure); // the hands together
    EXPECT_EQ(T::SideOf(1.60f, 2.00f), T::CapsuleSide::Unsure); // far from both (put away)
}

// A left part's first swing teaches its row: a candidate's hitbox at the left fist is learned, at
// the right one it is the wrong row (the next one is tried), with no side seen (no hitbox yet, the
// fists together) it waits, and gives up for that swing after ProbeFrames updates.
TEST(TrickWeapons, ALeftPartsFirstSwingTeachesItsRow) {
    using T::Probe;
    EXPECT_EQ(T::ProbeStep(true, 0.20f, 0.80f, 1), Probe::Learned); // the left claw's hitbox
    EXPECT_EQ(T::ProbeStep(true, 0.80f, 0.20f, 1), Probe::Wrong);   // the right hand's row
    EXPECT_EQ(T::ProbeStep(true, 0.30f, 0.35f, 1), Probe::Wait);    // the fists together
    EXPECT_EQ(T::ProbeStep(false, 0.0f, 0.0f, 1), Probe::Wait);     // no hitbox built yet
    EXPECT_EQ(T::ProbeStep(false, 0.20f, 0.80f, 1), Probe::Wait);   // nothing seen: no verdict
    EXPECT_EQ(T::ProbeStep(false, 0.0f, 0.0f, T::ProbeFrames - 1), Probe::Wait);
    EXPECT_EQ(T::ProbeStep(false, 0.0f, 0.0f, T::ProbeFrames), Probe::Unseen);
    EXPECT_EQ(T::ProbeStep(true, 0.30f, 0.35f, T::ProbeFrames), Probe::Unseen);
    // Seen in time, the side wins whatever the count.
    EXPECT_EQ(T::ProbeStep(true, 0.20f, 0.80f, T::ProbeFrames + 3), Probe::Learned);
    EXPECT_EQ(T::ProbeStep(true, 1.50f, 1.90f, 1), Probe::Wait); // far from both: not on the hand
}

// The left hand's learned rows (melee_left_rows): kept per weapon key, replaced when learned
// again, malformed items skipped.
TEST(TrickWeapons, TheLeftRowsLearnedAreKept) {
    EXPECT_EQ(T::LeftRowOf("", 4001), -1);
    std::string rows = T::WithLeftRow("", 4001, 455);
    EXPECT_EQ(rows, "4001:455");
    rows = T::WithLeftRow(rows, 20000, 810);
    EXPECT_EQ(T::LeftRowOf(rows, 4001), 455);
    EXPECT_EQ(T::LeftRowOf(rows, 20000), 810);
    EXPECT_EQ(T::LeftRowOf(rows, 9001), -1);
    rows = T::WithLeftRow(rows, 4001, 460);
    EXPECT_EQ(rows, "20000:810,4001:460");
    EXPECT_EQ(T::LeftRowOf("junk,4001:x,:5,27001:470", 27001), 470);
    EXPECT_EQ(T::WithLeftRow("junk,4001:x,27001:470", 9001, 400), "27001:470,9001:400");
}

TEST(TrickWeapons, SettingsParse) {
    T::Settings& s = T::GetTrickSettings();
    const T::Settings saved = s;
    EXPECT_TRUE(T::ParseTrickSetting("hand_transform", "0"));
    EXPECT_FALSE(s.hand_transform);
    EXPECT_TRUE(T::ParseTrickSetting("shield_raise_cm", "100"));
    EXPECT_NEAR(s.shield_m, 0.45f, 1e-5f); // clamped
    EXPECT_TRUE(T::ParseTrickSetting("hand_transform_pull_cm", "25"));
    EXPECT_NEAR(s.pull_m, 0.25f, 1e-5f);
    EXPECT_TRUE(T::Settings{}.weapon_wall);
    EXPECT_FALSE(T::Settings{}.weapon_wall_map); // weapons only by default
    std::ostringstream defaults;
    T::WriteTrickDefaults(defaults);
    EXPECT_NE(defaults.str().find("weapon_wall=2\n"), std::string::npos);
    EXPECT_TRUE(T::ParseTrickSetting("weapon_wall", "1"));
    EXPECT_TRUE(s.weapon_wall);
    EXPECT_TRUE(s.weapon_wall_map);
    // 2: weapons only, solid against each other, through the walls.
    EXPECT_TRUE(T::ParseTrickSetting("weapon_wall", "2"));
    EXPECT_TRUE(s.weapon_wall);
    EXPECT_FALSE(s.weapon_wall_map);
    EXPECT_TRUE(T::ParseTrickSetting("weapon_wall", "0"));
    EXPECT_FALSE(s.weapon_wall);
    EXPECT_TRUE(T::ParseTrickSetting("charge_cooldown_s", "500"));
    EXPECT_NEAR(s.charge_cooldown_s, 120.0f, 1e-5f); // clamped
    EXPECT_TRUE(T::ParseTrickSetting("saw_spin", "0"));
    EXPECT_FALSE(s.saw_spin);
    EXPECT_TRUE(T::ParseTrickSetting("l2_canned", "holy_moonlight_sword"));
    EXPECT_EQ(s.l2_canned, "holy_moonlight_sword");
    EXPECT_FALSE(T::ParseTrickSetting("hand_transforms", "1"));
    s = saved;
}

} // namespace
