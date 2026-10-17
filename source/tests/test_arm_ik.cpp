// SPDX-License-Identifier: GPL-2.0-or-later

// Arm IK core for the VR body (core/vr/arm_ik.h) on the tests' synthetic humanoid in the
// hunter's bone order (tests/c0000_test_skeleton.h: our own proportions, none of the game's
// numbers).
// Covers: Havok FK rules and local<->model round trips; the two-bone solver (reach, clamping,
// pole, degenerate input; fuzzed with the mannequin's arm lengths); SolveArm on the arm chains:
// reachable targets within 1 mm, unreachable targets clamp without flips, the elbow follows the
// pole, continuity under small target motion, the pole singularity, clavicle reach, twist
// helpers, wrist-driven elbow, blending and guards.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <optional>
#include <random>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "c0000_test_skeleton.h"

#include "core/vr/arm_ik.h"

namespace {

namespace B = VR::Body;
using B::QsTransform;
using B::Quat;
using B::Vec3;

constexpr float kDeg = B::Deg;
constexpr int kBones = 108;

constexpr int kSpine = 44, kSpine1 = 46, kSpine2 = 48, kUpperTurn = 43;
constexpr int kLClavicle = 50, kLUpperArm = 51, kLForearm = 52, kLHand = 53, kLWeapon = 69;
constexpr int kRClavicle = 79, kRUpperArm = 80, kRForearm = 81, kRHand = 82, kRWeapon = 98;
constexpr int kRForeArmTwist = 99, kRForeArmTwist1 = 100, kRElbowHelper = 101;
constexpr int kHead = 78;

struct Skeleton {
    std::vector<QsTransform> local;
    std::array<short, kBones> parents{};
};

// The test skeleton's reference pose (the c0000 bone order; scales 1).
Skeleton Hunter() {
    namespace C = B::C0000;
    const C::Skeleton& sk = C::Test::TestSkeleton();
    Skeleton s;
    s.local.resize(kBones);
    for (int i = 0; i < kBones; ++i) {
        const C::Transform& b = sk.local[static_cast<size_t>(i)];
        s.parents[i] = C::Parents[static_cast<size_t>(i)];
        s.local[i].t = {b.t[0], b.t[1], b.t[2]};
        s.local[i].q = B::Normalize(Quat{b.q[0], b.q[1], b.q[2], b.q[3]});
    }
    return s;
}

// The test skeleton's upper arm and forearm (shoulder -> elbow, elbow -> wrist).
float UpperArm() {
    return B::C0000::Test::TestSkeleton().upper_arm_length;
}
float Forearm() {
    return B::C0000::Test::TestSkeleton().forearm_length;
}

std::vector<QsTransform> Fk(const Skeleton& s) {
    std::vector<QsTransform> model(s.local.size());
    B::LocalToModel(s.local.data(), s.parents.data(), static_cast<int>(s.local.size()),
                    model.data());
    return model;
}

float Dist(Vec3 a, Vec3 b) {
    return B::Length(a - b);
}
float RotDeg(const Quat& a, const Quat& b) {
    return B::AngleBetween(a, b) / kDeg;
}
float AngleDeg(Vec3 a, Vec3 b) {
    return B::Angle(a, b) / kDeg;
}
Vec3 Mirror(Vec3 v) {
    return {-v.x, v.y, v.z};
}
bool SameBits(const QsTransform& a, const QsTransform& b) {
    return std::memcmp(&a, &b, sizeof(QsTransform)) == 0;
}

struct Rng {
    std::mt19937 gen;
    explicit Rng(unsigned seed) : gen(seed) {}
    float Uniform(float lo, float hi) {
        return std::uniform_real_distribution<float>(lo, hi)(gen);
    }
    Vec3 UnitVector() {
        const float z = Uniform(-1.0f, 1.0f);
        const float phi = Uniform(0.0f, 2.0f * B::Pi);
        const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        return {r * std::cos(phi), r * std::sin(phi), z};
    }
    // Uniform random rotation (Shoemake).
    Quat Rotation() {
        const float u1 = Uniform(0.0f, 1.0f), u2 = Uniform(0.0f, 1.0f), u3 = Uniform(0.0f, 1.0f);
        const float a = std::sqrt(1.0f - u1), b = std::sqrt(u1);
        return B::Normalize(Quat{a * std::sin(2.0f * B::Pi * u2), a * std::cos(2.0f * B::Pi * u2),
                                 b * std::sin(2.0f * B::Pi * u3), b * std::cos(2.0f * B::Pi * u3)});
    }
    Quat SmallRotation(float max_angle) {
        return B::AxisAngle(UnitVector(), Uniform(-max_angle, max_angle));
    }
};

// A combat-like animated pose: torso lean and twist, clavicle, shoulder, elbow and wrist motion.
void Animate(Skeleton& s, Rng& rng) {
    auto turn = [&](int bone, float max_deg) {
        s.local[bone].q = B::Normalize(rng.SmallRotation(max_deg * kDeg) * s.local[bone].q);
    };
    turn(kUpperTurn, 40.0f);
    turn(kSpine, 15.0f);
    turn(kSpine1, 10.0f);
    turn(kSpine2, 10.0f);
    for (const int arm : {0, 1}) {
        const int c = arm == 0 ? kRClavicle : kLClavicle;
        turn(c, 12.0f);
        turn(c + 1, 70.0f);
        s.local[c + 2].q = B::AxisAngle({0.0f, 1.0f, 0.0f}, rng.Uniform(5.0f, 130.0f) * kDeg);
        turn(c + 2, 8.0f);
        turn(c + 3, 60.0f);
    }
}

B::ArmParams DefaultParams(const B::ArmRig& rig) {
    B::ArmParams p;
    p.pole = B::BodyPole({}, rig.side);
    return p;
}

// Effector target that puts the wrist (hand bone) exactly at `wrist` with effector rotation `grip`.
B::ArmTarget TargetForWrist(const Skeleton& s, const B::ArmRig& rig, Vec3 wrist, const Quat& grip) {
    const QsTransform& eff = s.local[rig.effector];
    const Quat hand = B::Normalize(grip * B::Conjugate(eff.q));
    return {wrist + B::Rotate(hand, eff.t), grip, 1.0f};
}

// ---- Math -----------------------------------------------------------------------------------

TEST(ArmIkMath, QuaternionHelpers) {
    const Quat qz = B::AxisAngle({0.0f, 0.0f, 1.0f}, 90.0f * kDeg);
    const Vec3 y = B::Rotate(qz, {1.0f, 0.0f, 0.0f});
    EXPECT_NEAR(y.x, 0.0f, 1e-6f);
    EXPECT_NEAR(y.y, 1.0f, 1e-6f);

    Rng rng(7);
    for (int i = 0; i < 2000; ++i) {
        const Quat a = rng.Rotation(), b = rng.Rotation();
        const Vec3 v = rng.UnitVector() * rng.Uniform(0.1f, 2.0f);
        // Hamilton product composes rotations: (a*b) v = a (b v).
        EXPECT_LT(Dist(B::Rotate(a * b, v), B::Rotate(a, B::Rotate(b, v))), 1e-5f);
        // Basis round trip.
        const Quat back =
            B::FromBasis(B::Rotate(a, {1, 0, 0}), B::Rotate(a, {0, 1, 0}), B::Rotate(a, {0, 0, 1}));
        EXPECT_LT(RotDeg(back, a), 0.02f);
        // Shortest arc.
        const Vec3 u = rng.UnitVector(), w = rng.UnitVector();
        EXPECT_LT(Dist(B::Rotate(B::FromTo(u, w), u), w), 1e-5f);
        // Slerp end points and midpoint.
        EXPECT_LT(RotDeg(B::Slerp(a, b, 0.0f), a), 0.02f);
        EXPECT_LT(RotDeg(B::Slerp(a, b, 1.0f), b), 0.02f);
        const Quat mid = B::Slerp(a, b, 0.5f);
        EXPECT_NEAR(RotDeg(a, mid), RotDeg(mid, b), 0.05f);
        // Fractional rotation.
        EXPECT_NEAR(B::AngleOf(B::ScaleRotation(a, 0.5f)), 0.5f * B::AngleOf(a), 1e-3f);
        // Swing-twist: twist about an axis survives any perpendicular swing.
        const Vec3 axis = rng.UnitVector();
        const Vec3 perp = B::AnyPerpendicular(axis);
        const float tw = rng.Uniform(-3.0f, 3.0f);
        const Quat q = B::AxisAngle(perp, rng.Uniform(-2.5f, 2.5f)) * B::AxisAngle(axis, tw);
        EXPECT_NEAR(B::TwistAngle(q, axis), tw, 1e-3f);
        // Frame from (axis, hinge) pairs.
        const Vec3 dir = rng.UnitVector();
        const Vec3 hinge = B::Normalize(B::Cross(dir, rng.UnitVector()));
        const auto f = B::FrameFromAxes({1, 0, 0}, {0, 1, 0}, dir, hinge);
        ASSERT_TRUE(f.has_value());
        EXPECT_LT(Dist(B::Rotate(*f, {1, 0, 0}), dir), 1e-5f);
        EXPECT_LT(Dist(B::Rotate(*f, {0, 1, 0}), hinge), 1e-5f);
    }
    // Antiparallel and identical directions.
    EXPECT_LT(Dist(B::Rotate(B::FromTo({0, 1, 0}, {0, -1, 0}), {0, 1, 0}), {0, -1, 0}), 1e-5f);
    EXPECT_LT(RotDeg(B::FromTo({0, 1, 0}, {0, 1, 0}), Quat{}), 1e-3f);
    // A hinge along its bone has no frame.
    EXPECT_FALSE(B::FrameFromAxes({1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}).has_value());
    // Degenerate input stays finite.
    EXPECT_TRUE(B::IsFinite(B::Normalize(Quat{0, 0, 0, 0})));
    EXPECT_TRUE(B::IsFinite(B::Normalize(Vec3{NAN, 0, 0})));
}

// ---- Poses ----------------------------------------------------------------------------------

TEST(ArmIkPose, HavokCompositionIgnoresParentScale) {
    QsTransform parent{{1, 2, 3}, B::AxisAngle({0, 1, 0}, 90.0f * kDeg), {2, 2, 2}};
    QsTransform child{{1, 0, 0}, Quat{}, {0.5f, 3, 1}};
    const QsTransform m = B::Compose(parent, child);
    // +X rotated by +90 deg about +Y is -Z; the parent's scale of 2 does not stretch it.
    EXPECT_LT(Dist(m.t, {1, 2, 2}), 1e-6f);
    EXPECT_NEAR(m.s.x, 1.0f, 1e-6f);
    EXPECT_NEAR(m.s.y, 6.0f, 1e-6f);
    const QsTransform back = B::LocalFromModel(parent, m);
    EXPECT_LT(Dist(back.t, child.t), 1e-6f);
    EXPECT_NEAR(back.s.y, 3.0f, 1e-6f);
}

TEST(ArmIkPose, BindPoseMatchesTheSkeletonsModel) {
    const Skeleton s = Hunter();
    const auto m = Fk(s);
    // The float FK of the locals reproduces the skeleton's own model pose (core/vr/c0000_rig.h,
    // double precision), every bone; the arms mirror each other.
    const auto& sk = B::C0000::Test::TestSkeleton();
    for (int i = 0; i < kBones; ++i) {
        const auto& r = sk.model[static_cast<size_t>(i)];
        EXPECT_LT(Dist(m[i].t, {r.t[0], r.t[1], r.t[2]}), 2e-6f) << "bone " << i;
        EXPECT_LT(RotDeg(m[i].q, Quat{r.q[0], r.q[1], r.q[2], r.q[3]}), 0.01f) << "bone " << i;
    }
    for (const auto [l, r] : {std::pair{kLHand, kRHand}, std::pair{kLWeapon, kRWeapon},
                              std::pair{kLForearm, kRForearm}}) {
        EXPECT_LT(Dist(m[l].t, Mirror(m[r].t)), 1e-5f) << l;
    }
    // Arm lengths.
    EXPECT_NEAR(B::Length(s.local[kRForearm].t), UpperArm(), 1e-6f);
    EXPECT_NEAR(B::Length(s.local[kRHand].t), Forearm(), 1e-6f);
    // The elbow hinge is +Y of the upper arm: upperDir x foreDir in the bind pose.
    const Vec3 du = B::Normalize(m[kRForearm].t - m[kRUpperArm].t);
    const Vec3 df = B::Normalize(m[kRHand].t - m[kRForearm].t);
    EXPECT_LT(AngleDeg(B::Cross(du, df), B::Rotate(m[kRUpperArm].q, {0, 1, 0})), 2.0f);
    const Vec3 ldu = B::Normalize(m[kLForearm].t - m[kLUpperArm].t);
    const Vec3 ldf = B::Normalize(m[kLHand].t - m[kLForearm].t);
    EXPECT_LT(AngleDeg(B::Cross(ldu, ldf), B::Rotate(m[kLUpperArm].q, {0, 1, 0})), 2.0f);
}

TEST(ArmIkPose, LocalModelRoundTripAndChainWalk) {
    Rng rng(11);
    for (int iter = 0; iter < 50; ++iter) {
        Skeleton s = Hunter();
        for (auto& x : s.local) {
            x.q = B::Normalize(rng.SmallRotation(30.0f * kDeg) * x.q);
            x.t = x.t + rng.UnitVector() * rng.Uniform(0.0f, 0.01f);
            x.s = {rng.Uniform(0.9f, 1.1f), rng.Uniform(0.9f, 1.1f), rng.Uniform(0.9f, 1.1f)};
        }
        const auto model = Fk(s);
        std::vector<QsTransform> back(kBones);
        EXPECT_TRUE(B::ModelToLocal(model.data(), s.parents.data(), kBones, back.data()));
        for (int i = 0; i < kBones; ++i) {
            EXPECT_LT(Dist(back[i].t, s.local[i].t), 2e-5f);
            EXPECT_LT(RotDeg(back[i].q, s.local[i].q), 0.01f);
            EXPECT_NEAR(back[i].s.x, s.local[i].s.x, 1e-4f);
        }
        for (const int bone : {kHead, kRWeapon, kLWeapon, kRElbowHelper, 0}) {
            const auto m = B::ModelOf(s.local.data(), s.parents.data(), kBones, bone);
            ASSERT_TRUE(m.has_value());
            EXPECT_LT(Dist(m->t, model[bone].t), 1e-5f);
            EXPECT_LT(RotDeg(m->q, model[bone].q), 0.01f);
        }
    }
    // A parent after its child is reported.
    Skeleton bad = Hunter();
    bad.parents[10] = 20;
    std::vector<QsTransform> m(kBones);
    EXPECT_FALSE(B::LocalToModel(bad.local.data(), bad.parents.data(), kBones, m.data()));
    EXPECT_FALSE(B::ModelOf(bad.local.data(), bad.parents.data(), kBones, 11).has_value());
    EXPECT_FALSE(B::ModelOf(bad.local.data(), bad.parents.data(), kBones, kBones).has_value());
}

TEST(ArmIkPose, HavokLayoutRoundTrip) {
    EXPECT_EQ(sizeof(B::HkQsTransform), static_cast<size_t>(0x30));
    B::HkQsTransform h{{1, 2, 3, 77}, {0, 0, 0, 1}, {1, 1, 1, 88}};
    QsTransform x = B::FromHavok(h);
    EXPECT_NEAR(x.t.y, 2.0f, 0.0f);
    x.t = {4, 5, 6};
    x.q = B::AxisAngle({0, 0, 1}, 0.5f);
    B::ToHavok(x, h);
    EXPECT_NEAR(h.t[2], 6.0f, 0.0f);
    EXPECT_NEAR(h.t[3], 77.0f, 0.0f); // padding lanes untouched
    EXPECT_NEAR(h.s[3], 88.0f, 0.0f);
    EXPECT_NEAR(h.q[3], std::cos(0.25f), 1e-6f);
}

// ---- Two-bone solver ------------------------------------------------------------------------

TEST(TwoBoneIk, ReachableTargetsHitWithinOneMillimetre) {
    const Skeleton s = Hunter();
    const float base1 = B::Length(s.local[kRForearm].t);
    const float base2 = B::Length(s.local[kRHand].t);
    const B::TwoBoneLimits lim;
    Rng rng(21);
    float worst = 0.0f;
    for (int iter = 0; iter < 20000; ++iter) {
        // The skeleton's lengths, also scaled like other body proportions.
        const float k = iter < 5000 ? 1.0f : rng.Uniform(0.85f, 1.15f);
        const float l1 = base1 * k * (iter < 5000 ? 1.0f : rng.Uniform(0.9f, 1.1f));
        const float l2 = base2 * k * (iter < 5000 ? 1.0f : rng.Uniform(0.9f, 1.1f));
        const float rmin = B::ChordLength(l1, l2, lim.maxFlex);
        const float rmax = B::ChordLength(l1, l2, lim.minFlex);
        const Vec3 S = rng.UnitVector() * rng.Uniform(0.0f, 2.0f);
        const Vec3 dir = rng.UnitVector();
        const Vec3 T = S + dir * rng.Uniform(rmin + 1e-4f, rmax - 1e-4f);
        Vec3 pole = rng.UnitVector();
        if (B::Length(B::Perp(pole, dir)) < 0.05f) {
            continue; // resolved by SolveArm, see the singularity tests
        }
        const auto sol = B::SolveTwoBone(S, T, l1, l2, pole, lim);
        ASSERT_TRUE(sol.has_value());
        const float err = Dist(sol->wrist, T);
        worst = std::max(worst, err);
        EXPECT_LT(err, 0.001f);
        EXPECT_NEAR(Dist(sol->elbow, S), l1, 1e-4f);
        EXPECT_NEAR(Dist(sol->wrist, sol->elbow), l2, 1e-4f);
        EXPECT_NEAR(sol->stretch, 1.0f, 1e-6f);
        EXPECT_GE(sol->flex, lim.minFlex - 1e-3f);
        EXPECT_LE(sol->flex, lim.maxFlex + 1e-3f);
        // The elbow is on the pole side, and the hinge is perpendicular to both bones.
        EXPECT_GT(B::Dot(sol->elbow - S, B::Perp(pole, dir)), 0.0f);
        EXPECT_NEAR(B::Dot(sol->hinge, sol->upperDir), 0.0f, 1e-4f);
        EXPECT_NEAR(B::Dot(sol->hinge, sol->foreDir), 0.0f, 1e-4f);
        EXPECT_GT(B::Dot(B::Cross(sol->upperDir, sol->foreDir), sol->hinge), 0.0f);
    }
    std::printf("  two-bone reachable: worst wrist error %.3g mm\n", worst * 1000.0);
}

TEST(TwoBoneIk, UnreachableTargetsClampAlongTheLineWithoutFlips) {
    const float l1 = UpperArm(), l2 = Forearm();
    const B::TwoBoneLimits lim;
    const float rmax = B::ChordLength(l1, l2, lim.minFlex);
    Rng rng(22);
    for (int ray = 0; ray < 200; ++ray) {
        const Vec3 S{0.1f, 1.4f, 0.0f};
        const Vec3 dir = rng.UnitVector();
        Vec3 pole = rng.UnitVector();
        if (B::Length(B::Perp(pole, dir)) < 0.1f) {
            continue;
        }
        const Vec3 side = B::Normalize(B::Perp(pole, dir));
        float prev_reach = 0.0f;
        Vec3 prev_elbow{};
        for (float d = 0.30f; d < 2.25f * (l1 + l2); d += 0.002f) {
            const auto sol = B::SolveTwoBone(S, S + dir * d, l1, l2, pole, lim);
            ASSERT_TRUE(sol.has_value());
            const float reach = Dist(sol->wrist, S);
            EXPECT_LT(AngleDeg(sol->wrist - S, dir), 0.01f); // straight at the target
            EXPECT_GT(B::Dot(sol->bendDir, side), 0.9999f);  // same elbow side throughout
            EXPECT_LE(sol->stretch, 1.0f + lim.maxStretch + 1e-6f);
            EXPECT_GE(sol->flex, lim.minFlex - 1e-3f);
            EXPECT_GE(reach, prev_reach - 1e-6f);
            if (d > rmax * (1.0f + lim.maxStretch) + 1e-3f) {
                EXPECT_NEAR(sol->reachDeficit, d - rmax * (1.0f + lim.maxStretch), 1e-4f);
            }
            if (d > 0.30f) {
                EXPECT_LT(Dist(sol->elbow, prev_elbow), 0.02f);
            }
            prev_reach = reach;
            prev_elbow = sol->elbow;
        }
        // Beyond 2.25x the arm length the target is treated as bad tracking.
        EXPECT_FALSE(B::SolveTwoBone(S, S + dir * 1.3f, l1, l2, pole, lim).has_value());
    }
}

TEST(TwoBoneIk, TooCloseTargetsFoldToTheLimit) {
    const float l1 = UpperArm(), l2 = Forearm();
    const B::TwoBoneLimits lim;
    const float rmin = B::ChordLength(l1, l2, lim.maxFlex);
    const Vec3 S{0, 1.4f, 0};
    for (const float d : {0.0f, 1e-7f, 0.01f, 0.05f, rmin - 0.01f}) {
        const auto sol =
            B::SolveTwoBone(S, S + Vec3{0, 0, -1} * d, l1, l2, {0, -1, 0}, lim, {0, 0, -1});
        ASSERT_TRUE(sol.has_value());
        EXPECT_TRUE(B::IsFinite(sol->elbow) && B::IsFinite(sol->wrist));
        EXPECT_NEAR(Dist(sol->wrist, S), rmin, 1e-4f);
        EXPECT_LE(sol->flex, lim.maxFlex + 1e-3f);
        EXPECT_LT(sol->reachDeficit, 0.0f);
        EXPECT_LT(sol->elbow.y, S.y); // still on the pole side
    }
}

TEST(TwoBoneIk, ElbowFollowsThePole) {
    const float l1 = UpperArm(), l2 = Forearm();
    const Vec3 S{-0.17f, 1.44f, 0.03f};
    const Vec3 T = S + Vec3{0.1f, -0.2f, -0.35f};
    const Vec3 axis = B::Normalize(T - S);
    const Vec3 p0 = B::AnyPerpendicular(axis);
    Vec3 prev{};
    for (int deg = 0; deg <= 360; ++deg) {
        const Vec3 side = B::RotateAbout(p0, axis, deg * kDeg);
        // The hint may lean along the arm; only its perpendicular part matters.
        const Vec3 pole = side + axis * (0.7f * std::sin(deg * 0.05f));
        const auto sol = B::SolveTwoBone(S, T, l1, l2, pole);
        ASSERT_TRUE(sol.has_value());
        EXPECT_LT(AngleDeg(sol->bendDir, side), 0.01f);
        EXPECT_LT(AngleDeg(B::Perp(sol->elbow - S, axis), side), 0.01f);
        EXPECT_LT(Dist(sol->wrist, T), 1e-5f);
        if (deg > 0) {
            EXPECT_LT(Dist(sol->elbow, prev), 0.006f); // 1 deg of a <= 0.28 m radius circle
        }
        prev = sol->elbow;
    }
}

TEST(TwoBoneIk, DegenerateInputsAreRejectedOrResolved) {
    const float l1 = UpperArm(), l2 = Forearm();
    const Vec3 S{0, 1.4f, 0};
    EXPECT_FALSE(B::SolveTwoBone(S, {NAN, 1, 0}, l1, l2, {0, -1, 0}).has_value());
    EXPECT_FALSE(B::SolveTwoBone(S, {INFINITY, 1, 0}, l1, l2, {0, -1, 0}).has_value());
    EXPECT_FALSE(B::SolveTwoBone(S, {0, 1, 0}, 0.0f, l2, {0, -1, 0}).has_value());
    EXPECT_FALSE(B::SolveTwoBone(S, {0, 1, 0}, l1, NAN, {0, -1, 0}).has_value());
    EXPECT_FALSE(B::SolveTwoBone(S, {0, 1, 0}, l1, l2, {NAN, -1, 0}).has_value());
    // Pole along the arm, or no pole at all: some perpendicular elbow, still exact.
    for (const Vec3 pole : {Vec3{0, -1, 0}, Vec3{0, 1, 0}, Vec3{}}) {
        const auto sol = B::SolveTwoBone(S, S + Vec3{0, -0.4f, 0}, l1, l2, pole);
        ASSERT_TRUE(sol.has_value());
        EXPECT_TRUE(B::IsFinite(sol->elbow));
        EXPECT_NEAR(B::Dot(sol->bendDir, sol->axis), 0.0f, 1e-5f);
        EXPECT_LT(Dist(sol->wrist, S + Vec3{0, -0.4f, 0}), 1e-5f);
    }
    // Bad limits are sanitized.
    B::TwoBoneLimits odd;
    odd.minFlex = -1.0f;
    odd.maxFlex = 10.0f;
    odd.maxStretch = NAN;
    const auto sol = B::SolveTwoBone(S, S + Vec3{0, 0, -0.3f}, l1, l2, {0, -1, 0}, odd);
    ASSERT_TRUE(sol.has_value());
    EXPECT_TRUE(B::IsFinite(sol->elbow) && B::IsFinite(sol->wrist));
}

// ---- SolveArm on the hunter -----------------------------------------------------------------

TEST(ArmIk, BindTargetReproducesTheBindPose) {
    for (const bool right : {true, false}) {
        Skeleton s = Hunter();
        const B::ArmRig rig = right ? B::HunterRightArm() : B::HunterLeftArm();
        const auto bind = Fk(s);
        B::ArmParams p;                 // pole zero: keep the animation's elbow side
        p.limits.minFlex = 5.0f * kDeg; // the bind arm is bent only 7 deg
        p.clavicleReach = 0.0f;
        p.rollToPole = 0.0f;
        p.swingToPole = 0.0f;
        B::ArmState st;
        const B::ArmTarget t{bind[rig.effector].t, bind[rig.effector].q, 1.0f};
        const auto r = B::SolveArm(s.local.data(), s.parents.data(), kBones, rig, t, p, st);
        ASSERT_EQ(r.status, B::ArmStatus::Solved);
        EXPECT_LT(r.error, 1e-4f);
        const Skeleton ref = Hunter();
        for (const int bone : {rig.upperArm, rig.forearm, rig.hand}) {
            EXPECT_LT(RotDeg(s.local[bone].q, ref.local[bone].q), 0.5f) << "bone " << bone;
            EXPECT_LT(Dist(s.local[bone].t, ref.local[bone].t), 1e-4f);
        }
        EXPECT_NEAR(r.stretch, 1.0f, 1e-5f);
    }
}

TEST(ArmIk, ReachableTargetsOnHunterWithinOneMillimetre) {
    Rng rng(31);
    float worst_pos = 0.0f, worst_rot = 0.0f;
    for (int iter = 0; iter < 6000; ++iter) {
        Skeleton s = Hunter();
        Animate(s, rng);
        const B::ArmRig rig = (iter & 1) == 0 ? B::HunterRightArm() : B::HunterLeftArm();
        const auto anim = Fk(s);
        const Skeleton before = s;
        // Wrist 0.22-0.48 m from the animated shoulder: reachable even after the clavicle turns
        // (it moves the shoulder <= 6.2 cm); any grip orientation.
        const Quat grip = rng.Rotation();
        const Vec3 wrist = anim[rig.upperArm].t + rng.UnitVector() * rng.Uniform(0.22f, 0.48f);
        const B::ArmTarget target = TargetForWrist(s, rig, wrist, grip);
        B::ArmParams p = DefaultParams(rig);
        p.pole = p.pole + rng.UnitVector() * rng.Uniform(0.0f, 0.9f);
        p.rollToPole = rng.Uniform(0.0f, 0.6f);
        p.swingToPole = rng.Uniform(0.0f, 0.6f);
        B::ArmState st;
        const auto r = B::SolveArm(s.local.data(), s.parents.data(), kBones, rig, target, p, st,
                                   iter % 3 == 0 ? 0.0f : 1.0f / 90.0f);
        ASSERT_EQ(r.status, B::ArmStatus::Solved);
        const auto model = Fk(s);
        const float pos_err = Dist(model[rig.effector].t, target.position);
        const float rot_err = RotDeg(model[rig.effector].q, grip);
        worst_pos = std::max(worst_pos, pos_err);
        worst_rot = std::max(worst_rot, rot_err);
        EXPECT_LT(pos_err, 0.001f);
        EXPECT_LT(rot_err, 0.05f);
        EXPECT_TRUE(r.reached);
        EXPECT_LT(Dist(r.effector, model[rig.effector].t), 1e-4f);
        EXPECT_LT(Dist(r.elbow, model[rig.forearm].t), 1e-4f);
        EXPECT_GE(r.flex, 10.0f * kDeg - 1e-3f);
        EXPECT_LE(r.flex, 150.0f * kDeg + 1e-3f);
        // Only the listed bones change; written rotations stay unit.
        std::array<bool, kBones> written{};
        for (int i = 0; i < r.writtenCount; ++i) {
            written[r.written[i]] = true;
            const Quat& q = s.local[r.written[i]].q;
            EXPECT_NEAR(B::Dot(q, q), 1.0f, 1e-4f);
            EXPECT_TRUE(B::IsFinite(s.local[r.written[i]]));
        }
        for (int i = 0; i < kBones; ++i) {
            if (!written[i]) {
                EXPECT_TRUE(SameBits(s.local[i], before.local[i])) << "bone " << i;
            }
        }
        EXPECT_FALSE(written[rig.effector]); // the weapon bone keeps its attach transform
    }
    std::printf("  hunter reachable: worst effector error %.3g mm, %.3g deg\n", worst_pos * 1000.0,
                worst_rot);
}

TEST(ArmIk, UnreachableTargetsClampWithoutFlips) {
    const B::BodyFrame body;
    for (const bool right : {true, false}) {
        const B::ArmRig rig = right ? B::HunterRightArm() : B::HunterLeftArm();
        const Vec3 out = body.right * rig.side;
        const std::array<Vec3, 7> dirs = {body.forward,
                                          body.up,
                                          out,
                                          B::Normalize(body.forward - body.up),
                                          B::Normalize(body.forward - out * 0.8f),
                                          B::Normalize(out - body.up - body.forward * 0.5f),
                                          B::Normalize(-body.forward + out * 0.3f)};
        for (const Vec3 dir : dirs) {
            const Skeleton base = Hunter();
            const auto anim = Fk(base);
            const Vec3 S0 = anim[rig.upperArm].t;
            const Quat grip = anim[rig.effector].q;
            const float arm =
                B::Length(base.local[rig.forearm].t) + B::Length(base.local[rig.hand].t);
            B::ArmParams p = DefaultParams(rig);
            B::ArmState st;
            Vec3 prev_bend{}, prev_elbow{};
            float prev_err = 0.0f;
            bool first = true;
            for (float d = 0.40f; d < 1.18f; d += 0.001f) {
                Skeleton s = base;
                const B::ArmTarget t = TargetForWrist(s, rig, S0 + dir * d, grip);
                const auto r = B::SolveArm(s.local.data(), s.parents.data(), kBones, rig, t, p, st,
                                           1.0f / 90.0f);
                ASSERT_EQ(r.status, B::ArmStatus::Solved);
                EXPECT_TRUE(B::IsFinite(r.elbow) && B::IsFinite(r.effector));
                EXPECT_LE(r.stretch, 1.1f + 1e-5f);
                EXPECT_GE(r.flex, 10.0f * kDeg - 1e-3f);
                // The arm points straight at the wrist goal.
                const Vec3 wrist_goal = S0 + dir * d;
                EXPECT_LT(AngleDeg(r.wrist - r.shoulder, wrist_goal - r.shoulder), 0.05f);
                if (!first) {
                    EXPECT_LT(AngleDeg(r.bendDir, prev_bend), 3.0f) << "d " << d;
                    EXPECT_LT(Dist(r.elbow, prev_elbow), 0.01f) << "d " << d;
                    if (d > 0.75f) {
                        EXPECT_GE(r.error, prev_err - 1e-5f); // detaches progressively
                    }
                }
                if (d > 0.70f) {
                    EXPECT_FALSE(r.reached);
                    EXPECT_GT(r.reachDeficit, 0.0f);
                }
                first = false;
                prev_bend = r.bendDir;
                prev_elbow = r.elbow;
                prev_err = r.error;
            }
            // Far beyond the arm: rejected as a tracking error, pose untouched.
            Skeleton s = base;
            B::ArmState st2;
            const auto r =
                B::SolveArm(s.local.data(), s.parents.data(), kBones, rig,
                            TargetForWrist(s, rig, S0 + dir * (2.3f * arm), grip), p, st2);
            EXPECT_EQ(r.status, B::ArmStatus::TooFar);
            for (int i = 0; i < kBones; ++i) {
                EXPECT_TRUE(SameBits(s.local[i], base.local[i]));
            }
        }
    }
}

TEST(ArmIk, ElbowFollowsThePoleOnHunter) {
    const B::ArmRig rig = B::HunterRightArm();
    const Skeleton base = Hunter();
    const auto anim = Fk(base);
    const Vec3 wrist = anim[rig.upperArm].t + Vec3{0.05f, -0.15f, -0.38f};
    B::ArmParams p;
    p.rollToPole = 0.0f;
    p.swingToPole = 0.0f;
    p.clavicleReach = 0.0f;
    const Vec3 axis = B::Normalize(wrist - anim[rig.upperArm].t);
    const Vec3 p0 = B::AnyPerpendicular(axis);
    B::ArmState st;
    for (int deg = 0; deg < 360; deg += 3) {
        Skeleton s = base;
        p.pole = B::RotateAbout(p0, axis, deg * kDeg) + axis * 0.4f;
        const auto r = B::SolveArm(s.local.data(), s.parents.data(), kBones, rig,
                                   TargetForWrist(s, rig, wrist, anim[rig.effector].q), p, st);
        ASSERT_EQ(r.status, B::ArmStatus::Solved);
        const auto m = Fk(s);
        const Vec3 side = B::Perp(m[rig.forearm].t - m[rig.upperArm].t, axis);
        EXPECT_LT(AngleDeg(side, B::Perp(p.pole, axis)), 0.05f) << "deg " << deg;
        EXPECT_LT(r.error, 1e-4f);
    }
}

// Walks a target along a smooth path at 90 Hz and checks that nothing jumps between frames.
struct PathStats {
    float max_elbow_step = 0.0f;
    float max_bone_step_deg = 0.0f;
    float max_bend_step_deg = 0.0f;
    float max_target_step = 0.0f;
    float max_error = 0.0f;
    int failures = 0;
};

PathStats RunPath(const B::ArmRig& rig, const B::ArmParams& p, float speed, float seconds,
                  float hz = 90.0f) {
    PathStats st;
    const Skeleton base = Hunter();
    const auto anim = Fk(base);
    const Vec3 S0 = anim[rig.upperArm].t;
    const B::BodyFrame body;
    const Quat grip0 = anim[rig.effector].q;
    B::ArmState state;
    const float dt = 1.0f / hz;
    std::vector<QsTransform> prev;
    Vec3 prev_elbow{}, prev_bend{}, prev_target{};
    for (int frame = 0; frame * dt < seconds; ++frame) {
        const float t = frame * dt * speed;
        const Vec3 center =
            S0 + body.forward * 0.32f - body.up * 0.12f + body.right * (0.05f * rig.side);
        const Vec3 pos = center + body.right * (0.20f * std::sin(0.9f * t)) +
                         body.up * (0.22f * std::sin(1.3f * t + 1.0f)) +
                         body.forward * (0.12f * std::sin(0.7f * t + 2.0f));
        const Quat grip = B::AxisAngle({0.3f, 1.0f, 0.2f}, 1.1f * std::sin(0.6f * t)) *
                          B::AxisAngle({1.0f, 0.0f, 0.4f}, 1.3f * std::sin(0.45f * t + 0.5f)) *
                          grip0;
        Skeleton s = base;
        const B::ArmTarget target{pos, grip, 1.0f};
        const auto r =
            B::SolveArm(s.local.data(), s.parents.data(), kBones, rig, target, p, state, dt);
        if (r.status != B::ArmStatus::Solved || !B::IsFinite(r.elbow)) {
            ++st.failures;
            continue;
        }
        st.max_error = std::max(st.max_error, r.reached ? r.error : 0.0f);
        if (frame > 0) {
            st.max_target_step = std::max(st.max_target_step, Dist(pos, prev_target));
            st.max_elbow_step = std::max(st.max_elbow_step, Dist(r.elbow, prev_elbow));
            st.max_bend_step_deg = std::max(st.max_bend_step_deg, AngleDeg(r.bendDir, prev_bend));
            for (const int bone : {rig.clavicle, rig.upperArm, rig.forearm, rig.hand}) {
                st.max_bone_step_deg =
                    std::max(st.max_bone_step_deg, RotDeg(s.local[bone].q, prev[bone].q));
            }
        }
        prev = s.local;
        prev_elbow = r.elbow;
        prev_bend = r.bendDir;
        prev_target = pos;
    }
    return st;
}

TEST(ArmIk, SmallTargetMotionIsContinuous) {
    for (const bool right : {true, false}) {
        const B::ArmRig rig = right ? B::HunterRightArm() : B::HunterLeftArm();
        for (const float tau : {0.05f, 0.0f}) {
            B::ArmParams p = DefaultParams(rig);
            p.poleTau = tau;
            const PathStats st = RunPath(rig, p, 1.0f, 40.0f);
            std::printf("  path %s tau %.2f: target step %.2f mm, elbow step %.2f mm, bone step "
                        "%.2f deg, elbow side step %.2f deg, error %.3g mm\n",
                        right ? "R" : "L", tau, st.max_target_step * 1000.0f,
                        st.max_elbow_step * 1000.0f, st.max_bone_step_deg, st.max_bend_step_deg,
                        st.max_error * 1000.0f);
            EXPECT_EQ(st.failures, 0);
            EXPECT_LT(st.max_error, 0.001f);
            // Near full extension the elbow moves up to ~3x the hand (two-bone geometry with the
            // 10 degree floor); a flip would also turn the elbow side by tens of degrees.
            EXPECT_LT(st.max_elbow_step, 4.0f * st.max_target_step);
            EXPECT_LT(st.max_bone_step_deg, 4.0f);
            EXPECT_LT(st.max_bend_step_deg, 4.0f);
        }
    }
}

TEST(ArmIk, FastSwingsStaySolvedAndSmooth) {
    // Sword-swing speeds (hand up to ~2.5 m/s, wrist up to ~6 rad/s) at the headset rate and at
    // the game's 30 Hz pose rate.
    for (const bool right : {true, false}) {
        const B::ArmRig rig = right ? B::HunterRightArm() : B::HunterLeftArm();
        for (const float hz : {90.0f, 30.0f}) {
            const PathStats st = RunPath(rig, DefaultParams(rig), 7.0f, 30.0f, hz);
            std::printf("  swing %s %2.0f Hz: target step %.1f mm, elbow step %.1f mm, bone step "
                        "%.1f deg, elbow side step %.1f deg, error %.3g mm\n",
                        right ? "R" : "L", hz, st.max_target_step * 1000.0f,
                        st.max_elbow_step * 1000.0f, st.max_bone_step_deg, st.max_bend_step_deg,
                        st.max_error * 1000.0f);
            EXPECT_EQ(st.failures, 0);
            EXPECT_LT(st.max_error, 0.001f);
            EXPECT_LT(st.max_elbow_step, 4.0f * st.max_target_step);
            EXPECT_LT(st.max_bend_step_deg, hz > 60.0f ? 15.0f : 40.0f);
        }
    }
}

// The wrist circling the shoulder close in (0.15-0.28 m: the elbow folded to 120-150 degrees)
// in four planes, beside, above, behind and in front of the shoulder, at 1 m/s, the grip rolling
// up to +-160 degrees about the blade and wobbling, at 60 Hz. Where the elbow folds the wrist roll
// is ill defined and the pole passes along the arm: per frame, the elbow may move at most what the
// shoulder-wrist line's own motion explains (3x the wrist's step) plus 5 mm, the turn by the wrist
// at most 6 degrees, the elbow side at most 6 degrees plus the line's own turn.
struct WhipStats {
    float worst_excess = -1.0f; // max over frames of elbow step - 3 x wrist step, m
    float worst_elbow = 0.0f;
    float worst_turn_step = 0.0f;    // degrees
    float worst_side_excess = -1e9f; // side step - axis step, degrees
    int failures = 0;
    int frames = 0;
};

WhipStats FoldedSweep(const B::ArmRig& rig, const B::ArmParams& p, float radius, Vec3 u, Vec3 v,
                      float roll_deg) {
    const Skeleton base = Hunter();
    const auto anim = Fk(base);
    const Vec3 S0 = anim[rig.upperArm].t;
    const Quat grip0 = anim[rig.effector].q;
    const float dt = 1.0f / 60.0f;
    B::ArmState state;
    WhipStats w;
    Vec3 prev_elbow{}, prev_wrist{}, prev_bend{}, prev_axis{};
    float prev_turn = 0.0f;
    bool have = false;
    for (int f = 0; f < 8 * 60; ++f) {
        const float t = static_cast<float>(f) * dt;
        const float a = t / radius; // 1 m/s round the circle
        const Vec3 dir = B::Normalize(u * std::cos(a) + v * std::sin(a));
        const Quat grip = B::AxisAngle(dir, roll_deg * kDeg * std::sin(0.8f * t)) *
                          B::AxisAngle({0.3f, 1.0f, 0.2f}, 0.9f * std::sin(0.5f * t)) * grip0;
        Skeleton s = base;
        const auto r =
            B::SolveArm(s.local.data(), s.parents.data(), kBones, rig,
                        TargetForWrist(base, rig, S0 + dir * radius, grip), p, state, dt);
        if (r.status != B::ArmStatus::Solved || !B::IsFinite(r.elbow)) {
            ++w.failures;
            have = false;
            continue;
        }
        ++w.frames;
        const Vec3 axis = B::Normalize(r.wrist - r.shoulder);
        if (have) {
            const float elbow = Dist(r.elbow, prev_elbow);
            w.worst_elbow = std::max(w.worst_elbow, elbow);
            w.worst_excess = std::max(w.worst_excess, elbow - 3.0f * Dist(r.wrist, prev_wrist));
            w.worst_turn_step =
                std::max(w.worst_turn_step, std::abs(r.poleTurn - prev_turn) / kDeg);
            w.worst_side_excess = std::max(w.worst_side_excess, AngleDeg(r.bendDir, prev_bend) -
                                                                    AngleDeg(axis, prev_axis));
        }
        have = true;
        prev_elbow = r.elbow;
        prev_wrist = r.wrist;
        prev_bend = r.bendDir;
        prev_axis = axis;
        prev_turn = r.poleTurn;
    }
    return w;
}

TEST(ArmIk, FoldedArmSweepsWithoutElbowWhip) {
    const B::BodyFrame body;
    WhipStats all{}, before{};
    for (const bool right : {true, false}) {
        const B::ArmRig rig = right ? B::HunterRightArm() : B::HunterLeftArm();
        const Vec3 out = body.right * rig.side;
        const std::array<std::pair<Vec3, Vec3>, 4> planes{{
            {body.forward, body.up},                                                  // sagittal
            {body.forward, out},                                                      // horizontal
            {out, body.up},                                                           // frontal
            {B::Normalize(body.forward + out), B::Normalize(body.up - body.forward)}, // oblique
        }};
        for (const auto& [u, v] : planes) {
            for (const float radius : {0.15f, 0.20f, 0.28f}) {
                for (const float roll : {0.0f, 90.0f, 160.0f}) {
                    const WhipStats w = FoldedSweep(rig, DefaultParams(rig), radius, u, v, roll);
                    EXPECT_EQ(w.failures, 0);
                    EXPECT_GT(w.frames, 400);
                    EXPECT_LT(w.worst_excess, 0.005f) << radius << " roll " << roll;
                    EXPECT_LE(w.worst_turn_step, 6.0f + 1e-3f) << radius << " roll " << roll;
                    EXPECT_LE(w.worst_side_excess, 6.5f) << radius << " roll " << roll;
                    all.worst_excess = std::max(all.worst_excess, w.worst_excess);
                    all.worst_elbow = std::max(all.worst_elbow, w.worst_elbow);
                    all.worst_turn_step = std::max(all.worst_turn_step, w.worst_turn_step);
                    all.worst_side_excess = std::max(all.worst_side_excess, w.worst_side_excess);
                    // For the log: the same sweep without the rate limits and the fold fade.
                    B::ArmParams raw = DefaultParams(rig);
                    raw.maxPoleTurnRate = 0.0f;
                    raw.maxBendRate = 0.0f;
                    raw.foldFadeStart = 1e-3f;
                    const WhipStats o = FoldedSweep(rig, raw, radius, u, v, roll);
                    before.worst_excess = std::max(before.worst_excess, o.worst_excess);
                    before.worst_elbow = std::max(before.worst_elbow, o.worst_elbow);
                    before.worst_turn_step = std::max(before.worst_turn_step, o.worst_turn_step);
                }
            }
        }
    }
    std::printf("  folded sweeps: elbow step max %.1f mm, over 3x the wrist's %.1f mm, turn step "
                "max %.1f deg, side step over the line's %.1f deg (without the limits: %.1f mm, "
                "%.1f mm, %.1f deg)\n",
                all.worst_elbow * 1000.0f, all.worst_excess * 1000.0f, all.worst_turn_step,
                all.worst_side_excess, before.worst_elbow * 1000.0f, before.worst_excess * 1000.0f,
                before.worst_turn_step);
}

TEST(ArmIk, PoleSingularityDoesNotFlipTheElbow) {
    // The hand sweeps through the direction the pole points at (out, down and back), where the
    // pole says nothing about the elbow side.
    const B::BodyFrame body;
    for (const bool right : {true, false}) {
        B::ArmRig rig = right ? B::HunterRightArm() : B::HunterLeftArm();
        rig.effector = -1; // place the wrist itself
        const Skeleton base = Hunter();
        const auto anim = Fk(base);
        const Vec3 S0 = anim[rig.upperArm].t;
        B::ArmParams p = DefaultParams(rig);
        p.clavicleReach = 0.0f;
        const Vec3 pole = B::Normalize(p.pole);
        const Vec3 across = B::Normalize(B::Cross(pole, body.up)); // sweep direction
        B::ArmState st;
        float max_step = 0.0f;
        Vec3 prev{};
        const int frames = 240;
        for (int f = 0; f <= frames + 60; ++f) {
            const float a =
                (std::min(f, frames) / static_cast<float>(frames) - 0.5f) * 80.0f * kDeg;
            const Vec3 dir = B::Normalize(pole * std::cos(a) + across * std::sin(a));
            Skeleton s = base;
            const B::ArmTarget t{S0 + dir * 0.45f, std::nullopt, 1.0f};
            const auto r =
                B::SolveArm(s.local.data(), s.parents.data(), kBones, rig, t, p, st, 1.0f / 90.0f);
            ASSERT_EQ(r.status, B::ArmStatus::Solved);
            EXPECT_TRUE(B::IsFinite(r.elbow));
            EXPECT_LT(r.error, 0.001f);
            if (f > 0) {
                max_step = std::max(max_step, AngleDeg(r.bendDir, prev));
            }
            prev = r.bendDir;
            if (f == 0 || f >= frames + 50) {
                // Away from the singularity the elbow is where the pole says.
                const Vec3 axis = B::Normalize(r.wrist - r.shoulder);
                EXPECT_GT(B::Dot(r.bendDir, B::Normalize(B::Perp(pole, axis))), 0.99f)
                    << "frame " << f;
            }
        }
        std::printf("  pole singularity %s: largest elbow side step %.2f deg per frame\n",
                    right ? "R" : "L", max_step);
        EXPECT_LT(max_step, 10.0f);
    }
}

TEST(ArmIk, ClavicleShrugsOnlyTowardFarTargets) {
    const B::ArmRig rig = B::HunterRightArm();
    const B::BodyFrame body;
    const Skeleton base = Hunter();
    const auto anim = Fk(base);
    const Vec3 S0 = anim[rig.upperArm].t;
    auto solve = [&](Vec3 dir, float d, Skeleton& s, float reach = 0.15f) {
        s = base;
        B::ArmParams p = DefaultParams(rig);
        p.clavicleReach = reach;
        B::ArmState st;
        return B::SolveArm(s.local.data(), s.parents.data(), kBones, rig,
                           TargetForWrist(s, rig, S0 + dir * d, anim[rig.effector].q), p, st);
    };
    Skeleton s;
    // Near: the clavicle keeps its animated rotation.
    auto r = solve(body.forward, 0.25f, s);
    ASSERT_EQ(r.status, B::ArmStatus::Solved);
    EXPECT_TRUE(SameBits(s.local[rig.clavicle], base.local[rig.clavicle]));
    // Far forward and far up: the shoulder comes toward the target, within the limit.
    for (const Vec3 dir : {body.forward, body.up}) {
        r = solve(dir, 0.65f, s);
        ASSERT_EQ(r.status, B::ArmStatus::Solved);
        const float turn = RotDeg(s.local[rig.clavicle].q, base.local[rig.clavicle].q);
        EXPECT_GT(turn, 2.0f);
        EXPECT_LE(turn, 20.0f + 1e-3f);
        EXPECT_GT(B::Dot(r.shoulder - S0, dir), 0.005f);
        // The shrug buys reach: the hand gets closer than with a fixed clavicle.
        Skeleton fixed;
        const auto rf = solve(dir, 0.65f, fixed, 0.0f);
        EXPECT_LT(r.error, rf.error - 0.005f);
        EXPECT_TRUE(SameBits(fixed.local[rig.clavicle], base.local[rig.clavicle]));
    }
    // Down is damped: a smaller turn than the same distance up.
    Skeleton up_s, down_s;
    solve(body.up, 0.65f, up_s);
    solve(-body.up, 0.65f, down_s);
    EXPECT_LT(RotDeg(down_s.local[rig.clavicle].q, base.local[rig.clavicle].q),
              RotDeg(up_s.local[rig.clavicle].q, base.local[rig.clavicle].q));
}

// The reach catch-up (clavicleCatchUp, 2026-10-08: in a deep forward duck the hand fell short of
// its controller): a target the arm reaches is solved bit for bit as without it; a target beyond
// the arm's reach with its stretch turns the clavicle further toward it by just what closes the
// gap (the hand on the target), never more than the limit (less for a shoulder that drops: down
// to clavicleDownScale of it), and the turn grows from nothing at the edge of reach (no step).
TEST(ArmIk, ClavicleCatchUpOnlyForTargetsOutOfReach) {
    const B::ArmRig rig = B::HunterRightArm();
    const B::BodyFrame body;
    const Skeleton base = Hunter();
    const auto anim = Fk(base);
    const Vec3 S0 = anim[rig.upperArm].t;
    constexpr float CatchUp = 25.0f;
    auto solve = [&](Vec3 dir, float d, Skeleton& s, float catch_up_deg) {
        s = base;
        B::ArmParams p = DefaultParams(rig);
        p.clavicleCatchUp = catch_up_deg * kDeg;
        B::ArmState st;
        return B::SolveArm(s.local.data(), s.parents.data(), kBones, rig,
                           TargetForWrist(s, rig, S0 + dir * d, anim[rig.effector].q), p, st);
    };
    const Vec3 down_fwd = B::Normalize(body.forward * 0.8f - body.up * 0.6f);
    for (const Vec3 dir : {body.forward, body.up, down_fwd}) {
        float prev_turn = -1.0f;
        float first_out = -1.0f;
        int caught = 0;
        for (float d = 0.30f; d <= 1.00f; d += 0.005f) {
            Skeleton plain, with;
            const auto rp = solve(dir, d, plain, 0.0f);
            const auto rw = solve(dir, d, with, CatchUp);
            ASSERT_EQ(rp.status, B::ArmStatus::Solved);
            ASSERT_EQ(rw.status, B::ArmStatus::Solved);
            // The extra turn over the plain solve's clavicle.
            const float turn = RotDeg(with.local[rig.clavicle].q, plain.local[rig.clavicle].q);
            if (rp.reachDeficit <= 0.0f) {
                // Within reach: the same solve, bit for bit.
                for (int b = 0; b < kBones; ++b) {
                    ASSERT_TRUE(SameBits(with.local[b], plain.local[b])) << "bone " << b << " d " << d;
                }
                EXPECT_EQ(rw.error, rp.error);
            } else {
                if (first_out < 0.0f) {
                    first_out = d;
                }
                EXPECT_LE(rw.error, rp.error + 1e-5f) << d;
                EXPECT_LE(turn, CatchUp + 1e-2f) << d;
                if (rw.error < 0.002f) {
                    ++caught; // the hand on the target
                } else {
                    // Short of it only at the limit (at least the down-scaled one).
                    EXPECT_GT(turn, 0.4f * CatchUp - 0.05f) << d;
                }
            }
            if (prev_turn >= 0.0f) {
                // 5 mm of target travel never turns the clavicle by more than 2 degrees.
                EXPECT_LT(std::abs(turn - prev_turn), 2.0f) << d;
            }
            prev_turn = turn;
        }
        EXPECT_GT(first_out, 0.0f) << "the sweep reaches beyond the arm";
        EXPECT_GT(caught, 3) << "some targets out of reach are caught up";
        // Far beyond: the full catch-up (forward and up; less where the shoulder drops), and the
        // hand nearer than without it.
        Skeleton plain, with;
        const auto rp = solve(dir, 1.2f, plain, 0.0f);
        const auto rw = solve(dir, 1.2f, with, CatchUp);
        const float far_turn = RotDeg(with.local[rig.clavicle].q, plain.local[rig.clavicle].q);
        if (B::Dot(dir, body.up) >= 0.0f) {
            EXPECT_NEAR(far_turn, CatchUp, 0.05f);
        } else {
            EXPECT_LT(far_turn, CatchUp - 1.0f);
            EXPECT_GT(far_turn, 0.4f * CatchUp - 0.05f);
        }
        EXPECT_LT(rw.error, rp.error - 0.01f);
    }
    // Straight down (a hanging hand far below the arm's reach): the shoulder drops at most
    // clavicleDownScale (0.4) of the limit.
    Skeleton plain, with;
    solve(-body.up, 1.0f, plain, 0.0f);
    solve(-body.up, 1.0f, with, CatchUp);
    const float down_turn = RotDeg(with.local[rig.clavicle].q, plain.local[rig.clavicle].q);
    EXPECT_LT(down_turn, 0.6f * CatchUp);
    EXPECT_GT(down_turn, 0.4f * CatchUp - 0.05f);
}

TEST(ArmIk, HandRollFeedsTwistHelpersAndTurnsTheElbow) {
    B::ArmRig rig = B::HunterRightArm();
    rig.effector = -1; // roll about the wrist itself, so the wrist does not move
    const Skeleton base = Hunter();
    const auto anim = Fk(base);
    const Vec3 wrist = anim[rig.upperArm].t + Vec3{0.02f, -0.20f, -0.34f};
    const Quat hand0 = anim[rig.hand].q;
    B::ArmParams p = DefaultParams(rig);
    p.clavicleReach = 0.0f;
    p.swingToPole = 0.0f;
    auto solve = [&](float roll_deg, float roll_to_pole, Skeleton& s) {
        s = base;
        B::ArmParams q = p;
        q.rollToPole = roll_to_pole;
        B::ArmState st;
        // Roll about the forearm direction of the roll-free solution.
        Skeleton tmp = base;
        B::ArmState st0;
        const auto r0 = B::SolveArm(tmp.local.data(), tmp.parents.data(), kBones, rig,
                                    B::ArmTarget{wrist, hand0, 1.0f}, q, st0);
        const Vec3 fore = B::Normalize(r0.wrist - r0.elbow);
        const Quat goal = B::AxisAngle(fore, roll_deg * kDeg) * hand0;
        return B::SolveArm(s.local.data(), s.parents.data(), kBones, rig,
                           B::ArmTarget{wrist, goal, 1.0f}, q, st);
    };
    Skeleton a, b;
    auto r0 = solve(0.0f, 0.0f, a);
    auto r1 = solve(60.0f, 0.0f, b);
    ASSERT_EQ(r1.status, B::ArmStatus::Solved);
    const Vec3 xf{1, 0, 0};
    const auto twist_change = [&](int bone) {
        return B::WrapPi(B::TwistAngle(b.local[bone].q, xf) - B::TwistAngle(a.local[bone].q, xf)) /
               kDeg;
    };
    EXPECT_NEAR(twist_change(kRForeArmTwist), 60.0f, 1.0f);
    EXPECT_NEAR(twist_change(kRForeArmTwist1), 30.0f, 0.5f);
    EXPECT_LT(Dist(r1.elbow, r0.elbow), 1e-4f); // no roll-to-pole: the elbow stays
    EXPECT_LT(r1.error, 1e-4f);
    // With roll-to-pole the elbow turns the same way as the hand, 0.35 of the roll.
    auto r2 = solve(0.0f, 0.35f, a);
    auto r3 = solve(60.0f, 0.35f, b);
    const Vec3 axis = B::Normalize(r2.wrist - r2.shoulder);
    const float turn = B::SignedAngle(r2.bendDir, r3.bendDir, axis) / kDeg;
    std::printf("  roll 60 deg -> elbow turn %.2f deg\n", turn);
    EXPECT_NEAR(turn, 21.0f, 2.5f);
    EXPECT_LT(r3.error, 1e-4f);
}

TEST(ArmIk, WristSwingPullsTheElbowTowardARelaxedWrist) {
    const B::ArmRig rig = B::HunterRightArm();
    const Skeleton base = Hunter();
    const auto anim = Fk(base);
    Rng rng(41);
    int better = 0, total = 0;
    for (int iter = 0; iter < 500; ++iter) {
        const Vec3 wrist = anim[rig.upperArm].t + rng.UnitVector() * rng.Uniform(0.25f, 0.45f);
        const Quat grip = rng.Rotation();
        B::ArmParams p = DefaultParams(rig);
        p.rollToPole = 0.0f;
        p.clavicleReach = 0.0f;
        Skeleton s0 = base, s1 = base;
        B::ArmState a, b;
        const B::ArmTarget t = TargetForWrist(base, rig, wrist, grip);
        p.swingToPole = 0.0f;
        const auto r0 = B::SolveArm(s0.local.data(), s0.parents.data(), kBones, rig, t, p, a);
        p.swingToPole = 0.5f;
        const auto r1 = B::SolveArm(s1.local.data(), s1.parents.data(), kBones, rig, t, p, b);
        ASSERT_EQ(r1.status, B::ArmStatus::Solved);
        EXPECT_LT(r1.error, 0.001f);
        EXPECT_LE(std::abs(r1.poleTurn), 75.0f * kDeg + 1e-4f);
        if (std::abs(r1.poleTurn) > 1.0f * kDeg) {
            ++total;
            better += r1.wristSwing < r0.wristSwing ? 1 : 0;
        }
    }
    std::printf("  wrist swing: relaxed wrist improved in %d of %d turned cases\n", better, total);
    EXPECT_GT(total, 50);
    EXPECT_EQ(better, total);
}

TEST(ArmIk, BlendWeightsAndPartialIk) {
    B::BlendWeight w;
    EXPECT_NEAR(w.Value(), 0.0f, 0.0f);
    for (int i = 0; i < 15; ++i) {
        w.Update(true, 0.01f);
    }
    EXPECT_NEAR(w.progress, 0.5f, 1e-5f);
    EXPECT_NEAR(w.Value(), 1.0f - std::cos(0.25f * B::Pi), 1e-5f);
    for (int i = 0; i < 20; ++i) {
        w.Update(true, 0.01f);
    }
    EXPECT_NEAR(w.Value(), 1.0f, 0.0f);
    for (int i = 0; i < 8; ++i) {
        w.Update(false, 0.01f);
    }
    EXPECT_NEAR(w.Value(), 0.0f, 1e-5f);
    w.Update(true, NAN);
    EXPECT_NEAR(w.Value(), 0.0f, 0.0f);

    const B::ArmRig rig = B::HunterRightArm();
    const Skeleton base = Hunter();
    const auto anim = Fk(base);
    const B::ArmTarget t =
        TargetForWrist(base, rig, anim[rig.upperArm].t + Vec3{0.1f, 0.05f, -0.4f},
                       B::AxisAngle({1, 0, 0}, 0.7f) * anim[rig.effector].q);
    B::ArmParams p = DefaultParams(rig);
    Skeleton full = base, half = base, none = base;
    B::ArmState s1, s2, s3;
    B::SolveArm(full.local.data(), full.parents.data(), kBones, rig, t, p, s1);
    p.weight = 0.5f;
    const auto rh = B::SolveArm(half.local.data(), half.parents.data(), kBones, rig, t, p, s2);
    ASSERT_EQ(rh.status, B::ArmStatus::Solved);
    for (int i = 0; i < rh.writtenCount; ++i) {
        const int bone = rh.written[i];
        EXPECT_NEAR(RotDeg(base.local[bone].q, half.local[bone].q),
                    0.5f * RotDeg(base.local[bone].q, full.local[bone].q), 0.02f);
    }
    p.weight = 0.0f;
    EXPECT_EQ(B::SolveArm(none.local.data(), none.parents.data(), kBones, rig, t, p, s3).status,
              B::ArmStatus::Skipped);
    for (int i = 0; i < kBones; ++i) {
        EXPECT_TRUE(SameBits(none.local[i], base.local[i]));
    }
}

TEST(ArmIk, GuardsLeaveThePoseUntouched) {
    const Skeleton base = Hunter();
    const auto anim = Fk(base);
    const B::ArmRig rig = B::HunterRightArm();
    const B::ArmTarget good = TargetForWrist(
        base, rig, anim[rig.upperArm].t + Vec3{0, -0.2f, -0.3f}, anim[rig.effector].q);
    auto check = [&](const char* name, B::ArmStatus expected, Skeleton s, const B::ArmRig& rg,
                     const B::ArmTarget& t, const B::ArmParams& p) {
        const Skeleton before = s;
        B::ArmState st;
        st.valid = true;
        st.bendDir = {1, 0, 0};
        const auto r = B::SolveArm(s.local.data(), s.parents.data(), kBones, rg, t, p, st, 0.01f);
        EXPECT_EQ(r.status, expected) << name;
        EXPECT_FALSE(st.valid) << name; // the next good frame starts fresh
        for (int i = 0; i < kBones; ++i) {
            EXPECT_TRUE(SameBits(s.local[i], before.local[i])) << name << " bone " << i;
        }
    };
    const B::ArmParams p = DefaultParams(rig);
    B::ArmTarget t = good;
    t.position.x = NAN;
    check("nan target", B::ArmStatus::BadTarget, base, rig, t, p);
    t = good;
    t.rotation = Quat{NAN, 0, 0, 1};
    check("nan rotation", B::ArmStatus::BadTarget, base, rig, t, p);
    t = good;
    t.position = t.position + Vec3{0, 0, -5.0f};
    check("too far", B::ArmStatus::TooFar, base, rig, t, p);
    Skeleton s = base;
    s.local[kRForearm].q.w = NAN;
    check("nan pose", B::ArmStatus::BadPose, s, rig, good, p);
    s = base;
    s.local[kSpine2].t.y = INFINITY;
    check("nan parent pose", B::ArmStatus::BadPose, s, rig, good, p);
    s = base;
    s.local[kRHand].t = {};
    check("zero forearm", B::ArmStatus::BadPose, s, rig, good, p);
    s = base;
    s.local[kRUpperArm].q = Quat{0, 0, 0, 0};
    check("zero rotation", B::ArmStatus::BadPose, s, rig, good, p);
    B::ArmRig bad = rig;
    bad.forearm = kRHand;
    check("wrong chain", B::ArmStatus::BadRig, base, bad, good, p);
    bad = rig;
    bad.hand = 500;
    check("out of range", B::ArmStatus::BadRig, base, bad, good, p);
    bad = rig;
    bad.hingeUpper = {1, 0, 0};
    check("hinge along bone", B::ArmStatus::BadRig, base, bad, good, p);
    bad = rig;
    bad.helpers[0].bone = kRElbowHelper; // a wrist-twist helper must hang off the forearm
    check("helper parent", B::ArmStatus::BadRig, base, bad, good, p);
    bad = rig;
    bad.helpers[1] = bad.helpers[0];
    check("helper twice", B::ArmStatus::BadRig, base, bad, good, p);
    B::ArmParams nan_weight = p;
    nan_weight.weight = NAN;
    check("nan weight", B::ArmStatus::Skipped, base, rig, good, nan_weight);
    // NaN dt or pole: still solves, finite.
    Skeleton ok = base;
    B::ArmParams odd = p;
    odd.pole = {NAN, 0, 0};
    B::ArmState st;
    const auto r = B::SolveArm(ok.local.data(), ok.parents.data(), kBones, rig, good, odd, st, NAN);
    EXPECT_EQ(r.status, B::ArmStatus::Solved);
    EXPECT_LT(r.error, 0.001f);
}

TEST(ArmIk, LeftArmMirrorsRightArm) {
    Rng rng(51);
    const Skeleton base = Hunter();
    const auto anim = Fk(base);
    const B::ArmRig right = B::HunterRightArm(), left = B::HunterLeftArm();
    float worst = 0.0f;
    for (int iter = 0; iter < 300; ++iter) {
        const Vec3 off = rng.UnitVector() * rng.Uniform(0.2f, 0.5f);
        Skeleton rs = base, ls = base;
        B::ArmState a, b;
        const B::ArmTarget rt{anim[right.upperArm].t + off, std::nullopt, 1.0f};
        const B::ArmTarget lt{anim[left.upperArm].t + Mirror(off), std::nullopt, 1.0f};
        const auto r = B::SolveArm(rs.local.data(), rs.parents.data(), kBones, right, rt,
                                   DefaultParams(right), a);
        const auto l = B::SolveArm(ls.local.data(), ls.parents.data(), kBones, left, lt,
                                   DefaultParams(left), b);
        ASSERT_EQ(r.status, B::ArmStatus::Solved);
        ASSERT_EQ(l.status, B::ArmStatus::Solved);
        worst = std::max({worst, Dist(Mirror(r.elbow), l.elbow), Dist(Mirror(r.wrist), l.wrist)});
        EXPECT_LT(Dist(Mirror(r.elbow), l.elbow), 0.001f);
        EXPECT_LT(Dist(Mirror(r.effector), l.effector), 0.001f);
    }
    std::printf("  mirror: worst left/right difference %.3g mm\n", worst * 1000.0f);
}

} // namespace

// ---- Natural arms (body_natural_arms, 2026-10-08) ---------------------------------------------

TEST(ArmIkMath, TrackAngleAndSoftExcess) {
    // SoftExcess: 0 well inside, x - free well past it, C1 across the easing band.
    const float f = 75.0f * kDeg, s = 15.0f * kDeg;
    EXPECT_EQ(B::SoftExcess(-100.0f * kDeg, f, s), 0.0f);
    EXPECT_EQ(B::SoftExcess(60.0f * kDeg, f, s), 0.0f);
    EXPECT_NEAR(B::SoftExcess(90.0f * kDeg, f, s), 15.0f * kDeg, 1e-6f);
    EXPECT_NEAR(B::SoftExcess(120.0f * kDeg, f, s), 45.0f * kDeg, 1e-6f);
    float max_jump = 0.0f, max_slope_jump = 0.0f, prev = 0.0f, prev_slope = 0.0f;
    const float h = 0.01f * kDeg;
    for (int i = 0; i <= 20000; ++i) {
        const float x = (40.0f + 0.005f * static_cast<float>(i)) * kDeg;
        const float y = B::SoftExcess(x, f, s);
        const float slope = (B::SoftExcess(x + h, f, s) - B::SoftExcess(x - h, f, s)) / (2.0f * h);
        if (i > 0) {
            max_jump = std::max(max_jump, std::abs(y - prev));
            max_slope_jump = std::max(max_slope_jump, std::abs(slope - prev_slope));
        }
        EXPECT_GE(slope, -1e-3f);
        EXPECT_LE(slope, 1.0f + 1e-3f);
        prev = y;
        prev_slope = slope;
    }
    EXPECT_LT(max_jump, 0.01f * kDeg);
    EXPECT_LT(max_slope_jump, 0.01f);
    // TrackAngle: the first value as it is, then on round past 180, round to the other side past
    // the span.
    float t = 0.0f;
    EXPECT_NEAR(B::TrackAngle(170.0f * kDeg, t, false, 240.0f * kDeg), 170.0f * kDeg, 1e-5f);
    EXPECT_NEAR(B::TrackAngle(-170.0f * kDeg, t, true, 240.0f * kDeg), 190.0f * kDeg, 1e-4f);
    EXPECT_NEAR(B::TrackAngle(-130.0f * kDeg, t, true, 240.0f * kDeg), 230.0f * kDeg, 1e-4f);
    EXPECT_NEAR(B::TrackAngle(-110.0f * kDeg, t, true, 240.0f * kDeg), -110.0f * kDeg, 1e-4f);
    EXPECT_NEAR(B::TrackAngle(-170.0f * kDeg, t, true, 240.0f * kDeg), -170.0f * kDeg, 1e-4f);
    EXPECT_NEAR(B::TrackAngle(170.0f * kDeg, t, true, 240.0f * kDeg), -190.0f * kDeg, 1e-4f);
}

namespace {

// The natural roll on the hunter's right arm, the swing term off (only the roll moves the elbow).
B::ArmParams NaturalParams(const B::ArmRig& rig) {
    B::ArmParams p = DefaultParams(rig);
    p.clavicleReach = 0.0f;
    p.swingToPole = 0.0f;
    p.naturalRoll = true;
    p.trackTwist = true;
    p.maxWristTwist = 150.0f * kDeg;
    p.maxTwistRate = 720.0f * kDeg;
    return p;
}

} // namespace

TEST(ArmIk, NaturalRollTurnsTheElbowOnlyPastTheForearm) {
    // The hand rolled about the forearm, the wrist where it is. Inside the forearm's own range
    // (palm down up to 55 degrees, palm up up to 70: the 75 / 90 degree ranges less the easing)
    // the elbow does not move at all; past it the elbow turns 0.8 of the excess, out for
    // pronation, in for supination; the twist helpers carry the forearm's twist (the wrist one
    // all of it, the mid-forearm one half); the hand is exact.
    B::ArmRig rig = B::HunterRightArm();
    rig.effector = -1;
    const Skeleton base = Hunter();
    const auto anim = Fk(base);
    const Vec3 wrist = anim[rig.upperArm].t + Vec3{0.02f, -0.20f, -0.34f};
    const Quat hand0 = anim[rig.hand].q;
    const B::ArmParams p = NaturalParams(rig);
    Vec3 fore{};
    Quat relaxed{}; // the hand on that forearm with a relaxed (neutral) wrist
    {
        Skeleton tmp = base;
        B::ArmState st;
        const auto r0 = B::SolveArm(tmp.local.data(), tmp.parents.data(), kBones, rig,
                                    B::ArmTarget{wrist, hand0, 1.0f}, p, st);
        fore = B::Normalize(r0.wrist - r0.elbow);
        relaxed = B::Normalize(Fk(tmp)[rig.forearm].q * rig.handNeutral);
    }
    const auto solve = [&](float roll_deg, Skeleton& s) {
        s = base;
        B::ArmState st;
        return B::SolveArm(s.local.data(), s.parents.data(), kBones, rig,
                           B::ArmTarget{wrist, B::AxisAngle(fore, roll_deg * kDeg) * relaxed, 1.0f},
                           p, st);
    };
    Skeleton s0;
    const auto r0 = solve(0.0f, s0);
    float max_in_range = 0.0f;
    for (int deg = -70; deg <= 55; deg += 5) {
        Skeleton s;
        const auto r = solve(static_cast<float>(deg), s);
        ASSERT_EQ(r.status, B::ArmStatus::Solved);
        EXPECT_LT(r.error, 1e-4f);
        max_in_range = std::max(max_in_range, Dist(r.elbow, r0.elbow));
    }
    Skeleton sp, ss;
    const auto rp = solve(120.0f, sp);
    const auto rs = solve(-150.0f, ss);
    const Vec3 xf{1, 0, 0};
    // Each bone's twist about the forearm from its animated (bind) local, as the helpers take it.
    const auto twist = [&](const Skeleton& s, int bone) {
        return B::TwistAngle(B::Normalize(s.local[bone].q * B::Conjugate(base.local[bone].q)), xf) /
               kDeg;
    };
    const float hand_twist = twist(sp, rig.hand), h99 = twist(sp, kRForeArmTwist),
                h100 = twist(sp, kRForeArmTwist1);
    std::printf("  in range: elbow moves %.2f mm at most; palm down 120: roll %.1f, turn %.1f deg, "
                "hand twist %.1f, helpers %.1f / %.1f; palm up 150: roll %.1f, turn %.1f deg\n",
                max_in_range * 1000.0f, rp.wristRoll / kDeg, rp.poleTurn / kDeg, hand_twist, h99,
                h100, rs.wristRoll / kDeg, rs.poleTurn / kDeg);
    EXPECT_LT(max_in_range, 0.001f); // the easing's first degrees at most
    EXPECT_NEAR(rp.poleTurn / kDeg, 0.8f * (rp.wristRoll / kDeg - 75.0f), 0.5f);
    EXPECT_GT(rp.poleTurn, 20.0f * kDeg);
    EXPECT_NEAR(rs.poleTurn / kDeg, 0.8f * (rs.wristRoll / kDeg + 90.0f), 0.5f);
    EXPECT_LT(rs.poleTurn, -30.0f * kDeg);
    EXPECT_NEAR(h99, hand_twist, 0.5f);
    EXPECT_NEAR(h100, 0.5f * h99, 0.5f);
    EXPECT_LT(rp.error, 1e-4f);
    EXPECT_LT(rs.error, 1e-4f);
}

TEST(ArmIk, NaturalRollIsContinuousThroughHalfATurn) {
    // One arm state, the hand rolled 2 degrees a frame (90 Hz) from palm down 100 to 260 and back:
    // the roll is followed past 180 degrees (no wrap, no fade), the elbow keeps turning the same
    // way, every step small, the hand exact.
    B::ArmRig rig = B::HunterRightArm();
    rig.effector = -1;
    const Skeleton base = Hunter();
    const auto anim = Fk(base);
    const Vec3 wrist = anim[rig.upperArm].t + Vec3{0.02f, -0.20f, -0.34f};
    const Quat hand0 = anim[rig.hand].q;
    B::ArmParams p = NaturalParams(rig);
    p.maxPoleTurnNatural = 180.0f * kDeg;
    Vec3 fore{};
    {
        Skeleton tmp = base;
        B::ArmState st;
        const auto r0 = B::SolveArm(tmp.local.data(), tmp.parents.data(), kBones, rig,
                                    B::ArmTarget{wrist, hand0, 1.0f}, p, st);
        fore = B::Normalize(r0.wrist - r0.elbow);
    }
    B::ArmState st;
    float prev_turn = 0.0f, max_turn_step = 0.0f, max_roll = 0.0f, max_err = 0.0f;
    bool first = true;
    std::vector<int> degs;
    for (int d = 100; d <= 236; d += 2) {
        degs.push_back(d);
    }
    for (int d = 234; d >= 100; d -= 2) {
        degs.push_back(d);
    }
    for (const int d : degs) {
        Skeleton s = base;
        const auto r = B::SolveArm(
            s.local.data(), s.parents.data(), kBones, rig,
            B::ArmTarget{wrist, B::AxisAngle(fore, static_cast<float>(d) * kDeg) * hand0, 1.0f}, p,
            st, 1.0f / 90.0f);
        ASSERT_EQ(r.status, B::ArmStatus::Solved);
        if (!first) {
            max_turn_step = std::max(max_turn_step, std::abs(r.poleTurn - prev_turn));
        }
        first = false;
        prev_turn = r.poleTurn;
        max_roll = std::max(max_roll, r.wristRoll);
        max_err = std::max(max_err, r.error);
    }
    std::printf("  roll 100 .. 236 .. 100: followed up to %.1f deg, elbow turn step %.2f deg a "
                "frame at most, hand %.3f mm\n",
                max_roll / kDeg, max_turn_step / kDeg, max_err * 1000.0f);
    EXPECT_GT(max_roll, 220.0f * kDeg);
    EXPECT_LT(max_turn_step, 2.5f * kDeg);
    EXPECT_LT(max_err, 1e-4f);
}

TEST(ArmIk, KeepOutTurnsTheElbowOutOfTheTrunk) {
    // A pole that would put the elbow into the trunk (in toward the midline): with the keep-out
    // the elbow turns out about the shoulder-wrist line (the pronation sense) just onto its
    // surface; the hand stays exact. Without it the elbow is inside.
    const B::ArmRig rig = B::HunterRightArm();
    const Skeleton base = Hunter();
    const auto anim = Fk(base);
    B::ElbowKeepOut k{};
    k.enabled = true;
    k.base = anim[kSpine].t;
    const Vec3 wrist = anim[rig.upperArm].t + Vec3{0.12f, -0.12f, -0.30f}; // in front, across
    B::ArmParams p = DefaultParams(rig);
    p.clavicleReach = 0.0f;
    p.swingToPole = 0.0f;
    p.rollToPole = 0.0f;
    p.pole = Vec3{0.8f, -0.6f, 0.0f}; // toward the midline (+X is the hunter's left)
    const B::ArmTarget t = TargetForWrist(base, rig, wrist, anim[rig.effector].q);
    Skeleton a = base, b = base;
    B::ArmState sa, sb;
    const auto ra = B::SolveArm(a.local.data(), a.parents.data(), kBones, rig, t, p, sa);
    p.keepOut = k;
    const auto rb = B::SolveArm(b.local.data(), b.parents.data(), kBones, rig, t, p, sb);
    std::printf("  elbow depth %.3f without the keep-out, %.4f with it (turned out %.1f deg)\n",
                B::KeepOutDepth(k, ra.elbow), B::KeepOutDepth(k, rb.elbow), rb.keptOut / kDeg);
    ASSERT_EQ(rb.status, B::ArmStatus::Solved);
    EXPECT_GT(B::KeepOutDepth(k, ra.elbow), 0.05f);
    EXPECT_LE(B::KeepOutDepth(k, rb.elbow), 1e-3f);
    EXPECT_GT(B::KeepOutDepth(k, rb.elbow), -0.02f); // just onto the surface
    EXPECT_GT(rb.keptOut, 0.0f);
    EXPECT_GT(rb.poleTurn, 0.0f); // the right arm's out sense
    EXPECT_LT(rb.error, 0.001f);
    EXPECT_LT(RotDeg(Fk(b)[rig.effector].q, t.rotation.value()), 0.05f);
}
