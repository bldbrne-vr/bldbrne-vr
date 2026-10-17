// SPDX-License-Identifier: GPL-2.0-or-later

// VR body pose edits (core/vr/body_solver.h) and phantom-hand gating (core/vr/anim_classes.h,
// core/vr/c0000_anim_lengths.h) on the c0000 reference pose. Covers: the animation classes and
// lengths, the gate (holds, next-id and length releases, the game's cancel windows on clips seen
// every frame, the weapon switch's swing lock, the hard gates and their precedence, the pickup
// exception, arm_anim_gate=0) and the plan's blend sequence at 60 Hz; the torso-yaw filter
// (deadzone, follow, wrap); the elbow hint (values, continuity, mirror; a hand raised across the
// face or over the head never brings the elbow into view, folded arms keep it forward); the
// solver: untouched pose
// at zero weights, the torso yaw on UpperTurn and its clamp, the upper body under the eye with
// its clamps and low-pass, hiding (only the listed bones; arms_only keeps the arms' size), every
// mock preset through the mapping (reachable within 1 mm, out of reach detached at 10% stretch),
// smooth paths and preset changes without elbow flips, twist helpers that never snap, the arm
// weight's blend, the elbow hint taken from the edited torso, mirror symmetry and the guards.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "c0000_test_skeleton.h"

#include "core/vr/anim_classes.h"
#include "core/vr/arm_ik.h"
#include "core/vr/body_frame.h"
#include "core/vr/body_presets.h"
#include "core/vr/body_solver.h"
#include "core/vr/c0000_anim_lengths.h"
#include "core/vr/c0000_skeleton.h"
#include "core/vr/c0000_transform_lengths.h"
#include "core/vr/daze.h"
#include "core/vr/transform_clips.h"

namespace {

namespace B = VR::Body;
namespace C = VR::Body::C0000;
namespace P = VR::BodyPresets;
using B::AnimClass;
using B::QsTransform;
using B::Quat;
using B::Vec3;

constexpr float kDeg = B::Deg;
constexpr std::int64_t kMs = 1'000'000;

using Pose = std::array<QsTransform, C::BoneCount>;

Vec3 ToVec(const std::array<float, 3>& v) {
    return {v[0], v[1], v[2]};
}
Quat ToQuat(const std::array<float, 4>& q) {
    return {q[0], q[1], q[2], q[3]};
}

Pose Reference() {
    Pose pose{};
    for (size_t i = 0; i < pose.size(); ++i) {
        const C::Transform& t = C::Test::TestSkeleton().local[i];
        pose[i] = {ToVec(t.t), B::Normalize(ToQuat(t.q)), ToVec(t.s)};
    }
    return pose;
}

Pose Fk(const Pose& local) {
    Pose model{};
    B::LocalToModel(local.data(), C::Parents.data(), C::BoneCount, model.data());
    return model;
}

std::string Str(const char* text) {
    return text != nullptr ? std::string{text} : std::string{"(null)"};
}

bool SameBits(const QsTransform& a, const QsTransform& b) {
    return std::memcmp(&a, &b, sizeof(QsTransform)) == 0;
}

float Dist(Vec3 a, Vec3 b) {
    return B::Length(a - b);
}

float AngleDeg(Vec3 a, Vec3 b) {
    return B::Angle(a, b) / kDeg;
}

// The hunter's eye (dummy 58 midpoint on Head), model space.
Vec3 EyeOf(const Pose& local) {
    const auto head = B::ModelOf(local.data(), C::Parents.data(), C::BoneCount, C::Head);
    return head->t + B::Rotate(head->q, ToVec(C::Test::TestSkeleton().eye_center_on_head));
}

// The world heading change (the game's yaw, atan2(x, z) of a world direction) a model-space
// direction stands for: the model faces -Z.
float Heading(Vec3 v) {
    return std::atan2(-v.x, -v.z);
}

// The torso's forward (UpperTurn's model rotation applied to the reference forward).
Vec3 TorsoForward(const Pose& local) {
    const auto ref = B::ModelOf(Reference().data(), C::Parents.data(), C::BoneCount, C::UpperTurn);
    const auto now = B::ModelOf(local.data(), C::Parents.data(), C::BoneCount, C::UpperTurn);
    return B::Rotate(now->q * B::Conjugate(ref->q), {0.0f, 0.0f, -1.0f});
}

B::GateSignals Ids(std::initializer_list<std::int32_t> ids) {
    B::GateSignals s{};
    s.ring_valid = true;
    for (const std::int32_t id : ids) {
        s.new_ids[s.new_count++] = id;
    }
    return s;
}

B::GateSignals Quiet() {
    B::GateSignals s{};
    s.ring_valid = true;
    return s;
}

// One behaviour step as the game's ring reports it: every playing clip with its time before the
// step (0 on its first step), and the step's cancel windows (core/vr/anim_classes.h).
struct Clip {
    std::int32_t id;
    float from;
};
B::GateSignals Frame(std::initializer_list<Clip> clips, std::uint64_t allow, bool move,
                     bool cancel_valid = true) {
    B::GateSignals s{};
    s.ring_valid = true;
    for (const Clip& c : clips) {
        s.new_ids[s.new_count] = c.id;
        s.new_from[s.new_count++] = c.from;
    }
    s.cancel_valid = cancel_valid;
    s.allow = allow;
    s.move_ok = move;
    return s;
}

double Millis(std::int64_t ns) {
    return static_cast<double>(ns) / static_cast<double>(kMs);
}

// What idle accepts (TAE JumpTable 4, 16, 26, 31, 32: as read in the game).
constexpr std::uint64_t kIdleAllow = 0xC3FFEB;
constexpr std::uint64_t kAttacks = B::AllowRightAttack | B::AllowLeftAttack;

// The default body input: chr facing model forward (yaw 0), the head forward, the camera eye at
// the hunter's own eye (no shift needed).
B::BodyInput DefaultInput(const Pose& pose) {
    B::BodyInput in{};
    in.dt = 1.0f / 60.0f;
    in.chr_yaw = 0.0f;
    in.head_yaw = 0.0f;
    in.eye_m = EyeOf(pose);
    return in;
}

// Model target of a controller grip (tracking space, the mock presets' convention: blade yaw,
// pitch, roll in degrees) for a character facing tracking forward, the camera eye at (0, 1.6, -0.1)
// (fp_eye_height 1.6, fp_forward 0.1), as the pose hook maps it.
B::ModelTarget GripTarget(Vec3 p, float yaw_deg, float pitch_deg, float roll_deg, int side) {
    B::ChrFrame frame{};
    const float yaw = 0.6f;
    const float s = std::sin(yaw), c = std::cos(yaw);
    B::ChrFrameFromRows({-c, 0.0f, -s, 5.0f, 0.0f, 1.0f, 0.0f, 1.0f, s, 0.0f, -c, -7.0f}, frame);
    B::Anchor a{};
    a.valid = true;
    a.eye_world = B::ToWorld(frame, {0.0f, P::HeadHeight, -0.1f});
    a.eye_rel = a.eye_world - frame.t;
    a.head_k = {0.0f, P::HeadHeight, 0.0f};
    a.base_yaw = yaw;
    a.world_scale = 1.0f;
    B::TrackedHand hand{};
    hand.position_valid = hand.position_tracked = hand.orientation_valid = true;
    hand.p = p;
    hand.q = ToQuat(P::GripRotation(yaw_deg, pitch_deg, roll_deg));
    return B::HandToModel(a, frame, hand, B::DefaultGrip(side, 0.0f));
}

B::ModelTarget PresetTarget(const P::Preset& preset, int side, double t) {
    const P::HandPose scripted = P::Evaluate(preset, side, t);
    B::ChrFrame frame{};
    const float yaw = 0.6f;
    const float s = std::sin(yaw), c = std::cos(yaw);
    B::ChrFrameFromRows({-c, 0.0f, -s, 5.0f, 0.0f, 1.0f, 0.0f, 1.0f, s, 0.0f, -c, -7.0f}, frame);
    B::Anchor a{};
    a.valid = true;
    a.eye_world = B::ToWorld(frame, {0.0f, P::HeadHeight, -0.1f});
    a.eye_rel = a.eye_world - frame.t;
    a.head_k = {0.0f, P::HeadHeight, 0.0f};
    a.base_yaw = yaw;
    a.world_scale = 1.0f;
    B::TrackedHand hand{};
    hand.position_valid = hand.position_tracked = hand.orientation_valid = true;
    hand.p = ToVec(scripted.p);
    hand.q = ToQuat(scripted.q);
    return B::HandToModel(a, frame, hand, B::DefaultGrip(side, 0.0f));
}

} // namespace

// ---- Animation classes and lengths ---------------------------------------------------------

TEST(AnimClasses, Classify) {
    struct Case {
        std::int32_t id;
        AnimClass expected;
    };
    const std::array<Case, 32> cases = {{
        {0, AnimClass::Idle},
        {2200, AnimClass::Idle},
        {3000, AnimClass::Locomotion},
        {5000, AnimClass::Locomotion},
        {7000, AnimClass::Dodge},
        {7100, AnimClass::Dodge},
        {21007200, AnimClass::Dodge}, // a weapon's motion variant of the roll
        {7800, AnimClass::FallJump},
        {8100, AnimClass::FallJump},
        {9000, AnimClass::Grabbed},
        {10000, AnimClass::GuardReaction},
        {11000, AnimClass::WeaponSwitch},
        {11110, AnimClass::WeaponSwitch}, // the left hand's
        {12011, AnimClass::WeaponSwitch},
        {103000, AnimClass::Unverified},
        {19000, AnimClass::Transform}, // the Saw Cleaver's A -> B (L1)
        {19001, AnimClass::Transform}, // its upper-body variant (walking)
        {19610, AnimClass::Transform}, // the Boom Hammer's closing
        {19999, AnimClass::Transform},
        {26019030, AnimClass::Transform}, // a motion variant's
        {20100, AnimClass::Damage},
        {24000, AnimClass::Death},
        {103030000, AnimClass::AttackRight}, // a103_030000: R1
        {100033000, AnimClass::AttackRight}, // the hunter's throw
        {103040000, AnimClass::AttackBoth},
        {102051000, AnimClass::AttackLeft},
        {100000, AnimClass::ItemUse},
        {101140, AnimClass::Pickup},
        {101100, AnimClass::Event},
        {101011, AnimClass::Event}, // a ladder step
        {104000, AnimClass::Gesture},
        {-1, AnimClass::Unknown},
    }};
    for (const Case& c : cases) {
        EXPECT_EQ(B::ClassifyAnim(c.id), c.expected) << "id " << c.id;
    }
    EXPECT_EQ(B::ClassifyAnim(60000), AnimClass::Unknown);
    EXPECT_EQ(B::ClassifyAnim(105000), AnimClass::Unknown);
}

TEST(AnimClasses, Policies) {
    for (int i = 0; i < B::AnimClassCount; ++i) {
        const auto c = static_cast<AnimClass>(i);
        const B::AnimPolicy p = B::PolicyOf(c);
        EXPECT_NE(Str(B::AnimClassName(c)), "?");
        if (B::Animates(p)) {
            EXPECT_GT(p.cap_s, 0.0f) << B::AnimClassName(c);
            EXPECT_FALSE(p.releases) << B::AnimClassName(c);
        } else {
            EXPECT_FALSE(p.release_on_next) << B::AnimClassName(c);
            EXPECT_TRUE(p.torso) << B::AnimClassName(c);
        }
    }
    // The plan's table.
    EXPECT_FALSE(B::PolicyOf(AnimClass::AttackRight).right_ik);
    EXPECT_TRUE(B::PolicyOf(AnimClass::AttackRight).left_ik);
    EXPECT_TRUE(B::PolicyOf(AnimClass::AttackLeft).right_ik);
    EXPECT_TRUE(B::PolicyOf(AnimClass::AttackLeft).left_ik);
    EXPECT_FALSE(B::PolicyOf(AnimClass::Dodge).right_ik);
    EXPECT_FALSE(B::PolicyOf(AnimClass::Dodge).left_ik);
    EXPECT_TRUE(B::PolicyOf(AnimClass::Pickup).right_ik);
    EXPECT_FALSE(B::PolicyOf(AnimClass::Unknown).releases);
    // A hit, a guard reaction and a dodge end with the next idle or move, and like an item use with
    // the game's own cancel windows; a weapon switch keeps your arms.
    EXPECT_TRUE(B::PolicyOf(AnimClass::Damage).release_on_next);
    EXPECT_TRUE(B::PolicyOf(AnimClass::GuardReaction).release_on_next);
    for (const AnimClass c : {AnimClass::Damage, AnimClass::GuardReaction, AnimClass::Dodge,
                              AnimClass::ItemUse, AnimClass::Transform}) {
        EXPECT_TRUE(B::PolicyOf(c).cancel_release) << B::AnimClassName(c);
    }
    // The game's transform: a full-body clip (both arms, the torso), ended by the next idle or
    // move; it never ends another clip's hold.
    {
        const B::AnimPolicy p = B::PolicyOf(AnimClass::Transform);
        EXPECT_FALSE(p.right_ik || p.left_ik || p.torso);
        EXPECT_TRUE(p.release_on_next);
        EXPECT_FALSE(p.releases);
        EXPECT_EQ(Str(B::AnimClassName(AnimClass::Transform)), "transform");
    }
    for (const AnimClass c : {AnimClass::AttackRight, AnimClass::AttackBoth, AnimClass::Event,
                              AnimClass::Grabbed, AnimClass::Death, AnimClass::Unverified}) {
        EXPECT_FALSE(B::PolicyOf(c).cancel_release) << B::AnimClassName(c);
    }
    EXPECT_FALSE(B::Animates(B::PolicyOf(AnimClass::WeaponSwitch)));
    EXPECT_FALSE(B::PolicyOf(AnimClass::WeaponSwitch).releases);
}

TEST(AnimClasses, Lengths) {
    EXPECT_NEAR(C::AnimLengthSeconds(0), 4.57f, 1e-4f);
    EXPECT_NEAR(C::AnimLengthSeconds(7200), 2.1f, 1e-4f);
    EXPECT_NEAR(C::AnimLengthSeconds(101140), 2.43f, 1e-4f);
    EXPECT_NEAR(C::AnimLengthSeconds(103030000), 2.77f, 1e-4f);
    EXPECT_LT(C::AnimLengthSeconds(110), 0.0f); // a zero-length TAE entry
    EXPECT_LT(C::AnimLengthSeconds(30000), 0.0f);
    EXPECT_LT(C::AnimLengthSeconds(-5), 0.0f);
    EXPECT_LT(C::AnimLengthSeconds(999999999), 0.0f);
    // Holds: length + 0.1 s within the cap, the cap without a length.
    EXPECT_EQ(B::AnimHoldNs(20000, 3.0f), 1070 * kMs);
    EXPECT_EQ(B::AnimHoldNs(7200, 2.1f), 2100 * kMs);
    EXPECT_EQ(B::AnimHoldNs(30000, 3.0f), 3000 * kMs);
    for (const C::AnimLength& entry : C::AnimLengths) {
        EXPECT_GT(entry.seconds, 0.0f);
        EXPECT_LT(entry.seconds, 120.0f);
    }
}

// The game's transform clips hold by their own (hkx) length, not the TAE's 4.57 s (an event over
// 0 .. 4.567 s in every one of them).
TEST(AnimClasses, TransformLengths) {
    const float cap = B::PolicyOf(AnimClass::Transform).cap_s;
    EXPECT_NEAR(C::AnimLengthSeconds(19000), 4.57f, 1e-4f); // the TAE's, unchanged
    EXPECT_NEAR(B::HoldLengthSeconds(19000), 1.167f, 1e-4f);
    EXPECT_EQ(B::AnimHoldNs(19000, cap), 1267 * kMs); // was 4670 ms
    EXPECT_EQ(B::AnimHoldNs(19010, cap), 1267 * kMs);
    EXPECT_EQ(B::AnimHoldNs(19001, cap), 1267 * kMs);   // the walking variant
    EXPECT_EQ(B::AnimHoldNs(19020, cap), 2600 * kMs);   // Chikage A -> B
    EXPECT_EQ(B::AnimHoldNs(19160, cap), 1600 * kMs);   // Logarius' Wheel A -> B
    EXPECT_EQ(B::AnimHoldNs(19610, cap), 1700 * kMs);   // the Boom Hammer's closing (19600's)
    EXPECT_EQ(B::AnimHoldNs(19620, cap), 3267 * kMs);   // Bloodletter A -> B, the longest
    EXPECT_EQ(B::AnimHoldNs(19232, cap), 1333 * kMs);   // plays 19230's animation
    EXPECT_EQ(B::AnimHoldNs(26019030, cap), 830 * kMs); // a variant's: the TAE's 0.73 s
    EXPECT_EQ(B::AnimHoldNs(19990, cap), 3500 * kMs);   // no clip: the cap
    EXPECT_LT(C::TransformLengthSeconds(20000), 0.0f);
    EXPECT_NEAR(B::HoldLengthSeconds(20000), C::AnimLengthSeconds(20000), 1e-6f);
    // Every a000_019xxx entry with a TAE length has a clip length, all under the cap; and every
    // weapon's transform clip of the weapon-only table (core/vr/transform_clips.h) with a length
    // has the same one here.
    for (const C::AnimLength& entry : C::AnimLengths) {
        if (entry.id >= 19000 && entry.id <= 19999) {
            EXPECT_GT(C::TransformLengthSeconds(entry.id), 0.0f) << entry.id;
        }
    }
    for (const C::AnimLength& entry : C::TransformLengths) {
        EXPECT_GE(entry.id, 19000);
        EXPECT_LE(entry.id, 19999);
        EXPECT_GE(entry.seconds, 0.8f) << entry.id;
        EXPECT_LT(entry.seconds + 0.1f, cap) << entry.id;
        EXPECT_EQ(B::ClassifyAnim(entry.id), AnimClass::Transform);
    }
    for (const VR::Transform::Clip& clip : VR::Transform::Clips) {
        if (clip.length > 0.0f) {
            EXPECT_NEAR(C::TransformLengthSeconds(clip.clip), clip.length, 1e-3f) << clip.clip;
        }
    }
}

// ---- Gate -------------------------------------------------------------------------------------

TEST(AnimGate, BlendSequenceAt60Hz) {
    // The plan's S1 sequence: an R1 id takes the right arm (weight under 0.05 within 80 ms), a
    // locomotion id gives it back (above 0.95 within 0.33 s), and a dodge held without a new id is
    // released at its length + 0.1 s cap.
    B::AnimGate gate;
    std::array<B::BlendWeight, 2> w{};
    w[0].Set(true);
    w[1].Set(true);
    const std::int64_t frame = 1'000'000'000 / 60;
    const float dt = 1.0f / 60.0f;
    std::int64_t now = 1000 * kMs;
    auto step = [&](const B::GateSignals& s) {
        now += frame;
        const B::GateOut out = gate.Step(s, now);
        for (size_t i = 0; i < 2; ++i) {
            w[i].Update(out.ik[i], dt, 0.30f, 0.08f);
        }
        return out;
    };
    for (int i = 0; i < 10; ++i) {
        step(Quiet());
    }
    const std::int64_t r1 = now + frame;
    B::GateOut out = step(Ids({103030000}));
    EXPECT_FALSE(out.ik[C::Right]);
    EXPECT_TRUE(out.ik[C::Left]);
    EXPECT_EQ(out.reason, B::GateReason::Anim);
    EXPECT_TRUE(out.torso);
    std::int64_t low_at = -1;
    for (int i = 0; i < 30; ++i) {
        if (low_at < 0 && w[C::Right].Value() < 0.05f) {
            low_at = now - r1;
        }
        step(Quiet());
    }
    ASSERT_TRUE(low_at >= 0);
    EXPECT_LE(low_at, 80 * kMs);
    EXPECT_GT(w[C::Left].Value(), 0.999f);
    // Walking replaces the attack.
    const std::int64_t walk = now + frame;
    out = step(Ids({3000}));
    EXPECT_TRUE(out.ik[C::Right]);
    EXPECT_EQ(Str(out.released), "next id");
    std::int64_t high_at = -1;
    for (int i = 0; i < 40; ++i) {
        if (high_at < 0 && w[C::Right].Value() > 0.95f) {
            high_at = now - walk;
        }
        step(Quiet());
    }
    ASSERT_TRUE(high_at >= 0);
    EXPECT_LE(high_at, 330 * kMs);
    // A roll with nothing after it: both arms until its 2.1 s cap.
    const std::int64_t roll = now + frame;
    out = step(Ids({7200}));
    EXPECT_FALSE(out.ik[C::Right]);
    EXPECT_FALSE(out.ik[C::Left]);
    EXPECT_FALSE(out.torso);
    std::int64_t back_at = -1;
    for (int i = 0; i < 200 && back_at < 0; ++i) {
        out = step(Quiet());
        if (out.ik[C::Right] && out.ik[C::Left]) {
            back_at = now - roll;
            EXPECT_EQ(Str(out.released), "length");
        }
    }
    EXPECT_GE(back_at, 2100 * kMs);
    EXPECT_LE(back_at, 2100 * kMs + frame);
}

TEST(AnimGate, ReleasesOnlyWhereTheClassAllows) {
    B::AnimGate gate;
    std::int64_t now = 0;
    // An unverified class (103000) is not ended by walking, only by its cap (no TAE length).
    B::GateOut out = gate.Step(Ids({103000}), now += 10 * kMs);
    EXPECT_FALSE(out.ik[C::Right]);
    out = gate.Step(Ids({3000}), now += 100 * kMs);
    EXPECT_FALSE(out.ik[C::Right]);
    EXPECT_TRUE(out.released == nullptr);
    out = gate.Step(Quiet(), now += 5000 * kMs);
    EXPECT_TRUE(out.ik[C::Right]);
    // A hit is ended by walking out of it (the move cancel), or by idling.
    out = gate.Step(Ids({20000}), now += 10 * kMs);
    EXPECT_FALSE(out.ik[C::Right]);
    out = gate.Step(Ids({3000}), now += 100 * kMs);
    EXPECT_TRUE(out.ik[C::Right]);
    EXPECT_EQ(Str(out.released), "next id");
    // An unknown id never ends an attack; an idle id does.
    out = gate.Step(Ids({103030000}), now += 10 * kMs);
    EXPECT_FALSE(out.ik[C::Right]);
    out = gate.Step(Ids({60000}), now += 10 * kMs);
    EXPECT_FALSE(out.ik[C::Right]);
    out = gate.Step(Ids({0}), now += 10 * kMs);
    EXPECT_TRUE(out.ik[C::Right]);
    // A chained attack restarts the hold; a gun shot (left-hand attack) ends a right attack.
    gate.Step(Ids({103030000}), now += 10 * kMs);
    gate.Step(Ids({103030010}), now += 2500 * kMs);
    out = gate.Step(Quiet(), now += 1000 * kMs);
    EXPECT_FALSE(out.ik[C::Right]) << "the chain restarted the hold";
    out = gate.Step(Ids({102051000}), now += 10 * kMs);
    EXPECT_TRUE(out.ik[C::Right]);
    EXPECT_TRUE(out.ik[C::Left]);
}

TEST(AnimGate, StrongestNewIdWins) {
    B::AnimGate gate;
    std::int64_t now = 0;
    // A full-body move and an attack layer in one step: the full-body move holds both arms.
    B::GateOut out = gate.Step(Ids({103030000, 7200, 3000}), now += 10 * kMs);
    EXPECT_FALSE(out.ik[C::Right]);
    EXPECT_FALSE(out.ik[C::Left]);
    EXPECT_EQ(out.owner, AnimClass::Dodge);
    EXPECT_EQ(out.owner_id, 7200);
    // A newer move of any class replaces the held one.
    out = gate.Step(Ids({103030000}), now += 100 * kMs);
    EXPECT_EQ(out.owner, AnimClass::AttackRight);
    EXPECT_TRUE(out.ik[C::Left]);
    EXPECT_TRUE(out.torso);
}

TEST(AnimGate, HardGatesAndPrecedence) {
    B::AnimGate gate;
    std::int64_t now = 0;
    B::GateSignals s = Quiet();
    s.event_valid = s.throw_valid = s.ladder_valid = s.hp_valid = true;
    B::GateOut out = gate.Step(s, now += 10 * kMs);
    EXPECT_TRUE(out.ik[C::Right]);
    EXPECT_TRUE(out.ik[C::Left]);
    EXPECT_EQ(out.reason, B::GateReason::None);
    EXPECT_TRUE(out.changed); // the first step
    out = gate.Step(s, now += 10 * kMs);
    EXPECT_FALSE(out.changed);
    s.event = true;
    out = gate.Step(s, now += 10 * kMs);
    EXPECT_EQ(out.reason, B::GateReason::Event);
    EXPECT_FALSE(out.ik[C::Right]);
    EXPECT_FALSE(out.torso);
    EXPECT_TRUE(out.changed);
    s.ladder = 2;
    out = gate.Step(s, now += 10 * kMs);
    EXPECT_EQ(out.reason, B::GateReason::Ladder);
    s.throw_state = 3;
    out = gate.Step(s, now += 10 * kMs);
    EXPECT_EQ(out.reason, B::GateReason::Throw);
    s.hp = 0;
    out = gate.Step(s, now += 10 * kMs);
    EXPECT_EQ(out.reason, B::GateReason::Dead);
    // Invalid signals carry nothing.
    s.event_valid = s.throw_valid = s.ladder_valid = s.hp_valid = false;
    out = gate.Step(s, now += 10 * kMs);
    EXPECT_EQ(out.reason, B::GateReason::None);
    EXPECT_TRUE(out.ik[C::Right] && out.ik[C::Left]);
}


TEST(AnimGate, FreehandVisceralKeepsTrackedBodyFromFirstFrameButNeverEnablesAttacks) {
    B::AnimGate gate;
    auto s = Ids({100033000});
    s.visceral_freehand = true;
    s.throw_valid = true;
    s.throw_state = 3;
    auto out = gate.Step(s, 10 * kMs);
    EXPECT_TRUE(out.ik[C::Left] && out.ik[C::Right]);
    EXPECT_TRUE(out.torso);
    EXPECT_TRUE(out.kept);
    EXPECT_FALSE(out.attack_ok[C::Left] || out.attack_ok[C::Right]);
    EXPECT_EQ(out.reason, B::GateReason::Throw);
    s.new_count = 0;
    s.throw_state = 2;
    out = gate.Step(s, 20 * kMs);
    EXPECT_FALSE(out.ik[C::Left] || out.ik[C::Right]); // enemy grabs unchanged
    EXPECT_FALSE(out.torso);
    s.throw_state = 3;
    s.hp_valid = true;
    s.hp = 0;
    out = gate.Step(s, 30 * kMs);
    EXPECT_EQ(out.reason, B::GateReason::Dead);
    EXPECT_FALSE(out.ik[C::Left] || out.ik[C::Right]);
    EXPECT_FALSE(out.torso);
}

TEST(AnimGate, FreehandThrowEndDoesNotReplayExtractionButNewHitStillOwnsArms) {
    for (bool hit : {false, true}) {
        B::AnimGate gate;
        auto s = Ids({100033000});
        s.visceral_freehand = true;
        s.throw_valid = true;
        s.throw_state = 3;
        gate.Step(s, 10 * kMs);
        s = hit ? Ids({20203}) : Quiet();
        s.keep_hit = false;
        s.visceral_freehand = true;
        s.throw_valid = true;
        s.throw_state = 0;
        const auto out = gate.Step(s, 20 * kMs);
        EXPECT_EQ(out.ik[C::Right] && out.ik[C::Left], !hit);
    }
}

TEST(AnimGate, HitArrivingBeforeFreehandThrowClearsStillKeepsItsRecovery) {
    B::AnimGate gate;
    auto s = Ids({100033000});
    s.visceral_freehand = true;
    s.throw_valid = true;
    s.throw_state = 3;
    gate.Step(s, 10 * kMs);
    s = Ids({20203});
    s.visceral_freehand = true;
    s.throw_valid = true;
    s.throw_state = 3;
    gate.Step(s, 20 * kMs);
    s.new_count = 0;
    s.throw_state = 0;
    const auto out = gate.Step(s, 30 * kMs);
    EXPECT_EQ(out.owner, AnimClass::Damage);
    EXPECT_TRUE(out.held);
    EXPECT_FALSE(out.ik[C::Left] || out.ik[C::Right]);
}

TEST(AnimGate, VisceralRecoveryReturnsVisualArmsButKeepsCombatGated) {
    B::AnimGate gate;
    auto s = Quiet();
    s.throw_valid = true;
    s.throw_state = 3;
    auto out = gate.Step(s, 10 * kMs);
    EXPECT_FALSE(out.ik[C::Right]);
    s.visceral_recovery = true;
    out = gate.Step(s, 20 * kMs);
    EXPECT_TRUE(out.ik[C::Right] && out.ik[C::Left]);
    EXPECT_FALSE(out.attack_ok[C::Right] || out.attack_ok[C::Left]);
    EXPECT_FALSE(out.torso);
    EXPECT_EQ(out.reason, B::GateReason::Throw);
    s.throw_state = 2; // a victim of an enemy grab never gets this override
    out = gate.Step(s, 30 * kMs);
    EXPECT_FALSE(out.ik[C::Right] || out.ik[C::Left]);
    s.throw_state = 3;
    s.hp_valid = true;
    s.hp = 0;
    out = gate.Step(s, 40 * kMs);
    EXPECT_EQ(out.reason, B::GateReason::Dead);
    EXPECT_FALSE(out.ik[C::Right] || out.ik[C::Left]);
}

TEST(AnimGate, LadderDeadLatch) {
    // Killed on a ladder: the death states never write the ladder state, so it keeps its climbing
    // value through the respawn. After HP <= 0 (or a death clip) that value is ignored, the arms
    // and torso are yours again, until the state changes or a climbing clip arrives while alive.
    B::AnimGate gate;
    std::int64_t now = 0;
    B::GateSignals s = Quiet();
    s.ladder_valid = s.hp_valid = true;
    s.hp = 500;
    s.ladder = 8; // climbing, idle
    EXPECT_EQ(gate.Step(s, now += 10 * kMs).reason, B::GateReason::Ladder);
    s.hp = 0;
    EXPECT_EQ(gate.Step(s, now += 10 * kMs).reason, B::GateReason::Dead);
    s.hp = 500; // respawned, the byte still 8
    B::GateOut out = gate.Step(s, now += 3000 * kMs);
    EXPECT_EQ(out.reason, B::GateReason::None);
    EXPECT_TRUE(out.ik[C::Right] && out.ik[C::Left] && out.torso);
    EXPECT_TRUE(out.ladder_ignored);
    // A ladder clip that is no climb (an attack on the ladder, an event-class id) does not release
    // it; its own animation hold still applies.
    s = Ids({101035});
    s.ladder_valid = s.hp_valid = true;
    s.hp = 500;
    s.ladder = 8;
    out = gate.Step(s, now += 10 * kMs);
    EXPECT_EQ(out.reason, B::GateReason::Anim);
    EXPECT_TRUE(out.ladder_ignored);
    s.new_count = 0;
    out = gate.Step(s, now += 11000 * kMs);
    EXPECT_EQ(out.reason, B::GateReason::None);
    // The next ladder: getting on changes the state (0: from below): the game's arms at once.
    s.ladder = 0;
    out = gate.Step(s, now += 10 * kMs);
    EXPECT_EQ(out.reason, B::GateReason::Ladder);
    EXPECT_FALSE(out.ladder_ignored);
    // A death clip latches too (HP not read yet); a climbing clip while alive releases it with the
    // byte unchanged.
    s.hp_valid = false;
    s.ladder = 8;
    gate.Step(s, now += 10 * kMs);
    s = Ids({24700}); // the ladder death
    s.ladder_valid = true;
    s.ladder = 8;
    gate.Step(s, now += 10 * kMs);
    s.new_count = 0;
    out = gate.Step(s, now += 31000 * kMs);
    EXPECT_EQ(out.reason, B::GateReason::None);
    EXPECT_TRUE(out.ladder_ignored);
    s = Ids({101010}); // getting on from below
    s.ladder_valid = true;
    s.ladder = 8;
    out = gate.Step(s, now += 10 * kMs);
    EXPECT_EQ(out.reason, B::GateReason::Ladder);
    // Killed off a ladder (the byte -1): the next ladder's state releases it.
    s = Quiet();
    s.ladder_valid = s.hp_valid = true;
    s.ladder = -1;
    s.hp = 0;
    gate.Step(s, now += 10 * kMs);
    s.hp = 500; // after the mount's hold
    EXPECT_EQ(gate.Step(s, now += 11000 * kMs).reason, B::GateReason::None);
    s.ladder = 1;
    EXPECT_EQ(gate.Step(s, now += 10 * kMs).reason, B::GateReason::Ladder);
    // Climbing clips: getting on, the steps (also the fast ones); not the idle, the attacks or
    // the dismounts.
    for (const std::int32_t id : {101010, 101020, 101011, 101012, 101015, 101016, 101021, 101022}) {
        EXPECT_TRUE(B::IsClimbClip(id)) << id;
    }
    for (const std::int32_t id : {101030, 101035, 101013, 101023, 101050, 24700, 7200, -1}) {
        EXPECT_FALSE(B::IsClimbClip(id)) << id;
    }
}

TEST(AnimGate, PickupKeepsTheArmsThroughItsEventFlag) {
    B::AnimGate gate;
    std::int64_t now = 0;
    B::GateSignals s = Ids({B::PickupAnimId});
    s.event_valid = s.event = true;
    B::GateOut out = gate.Step(s, now += 10 * kMs);
    EXPECT_TRUE(out.ik[C::Right] && out.ik[C::Left]);
    EXPECT_EQ(out.reason, B::GateReason::None);
    s.new_count = 0;
    out = gate.Step(s, now += 2000 * kMs);
    EXPECT_TRUE(out.ik[C::Right] && out.ik[C::Left]);
    // After the pickup's length + 0.1 s the event flag counts again.
    out = gate.Step(s, now += 600 * kMs);
    EXPECT_EQ(out.reason, B::GateReason::Event);
}

TEST(AnimGate, AnimGateOffKeepsOnlyTheHardGates) {
    B::AnimGate gate;
    std::int64_t now = 0;
    B::GateSignals s = Ids({7200});
    s.anim_gate = false;
    B::GateOut out = gate.Step(s, now += 10 * kMs);
    EXPECT_TRUE(out.ik[C::Right] && out.ik[C::Left]);
    EXPECT_TRUE(out.torso);
    EXPECT_TRUE(out.held); // still tracked, for the log
    s.new_count = 0;
    s.throw_valid = true;
    s.throw_state = 2;
    out = gate.Step(s, now += 10 * kMs);
    EXPECT_EQ(out.reason, B::GateReason::Throw);
}

// A grab sat out (the cine lane's timeline, 2026-10-05): the throw state stays on 0.7-0.9 s after
// the game's cancel window opens and VR comes back. The arms come back 50 ms into the window (the
// cinematic's rule: the window seen closed first in this throw), not when the throw state ends;
// the rest of that throw state never takes them again; the next grab does. A fatal one stays.
TEST(AnimGate, GrabHandsTheArmsBackAtItsCancelWindow) {
    const std::int64_t frame = 1'000'000'000 / 60;
    const auto grab = [&](B::AnimGate& gate, std::int64_t& now, std::int32_t hp, bool release,
                          std::int64_t& back_after_window) {
        back_after_window = -1;
        // The last clip's requests are still open on the throw's first updates: never counted.
        for (int i = 0; i < 3; ++i) {
            B::GateSignals s = Frame({}, kIdleAllow, true);
            s.throw_valid = s.hp_valid = true;
            s.throw_state = 3;
            s.hp = hp;
            s.throw_release = release;
            EXPECT_EQ(gate.Step(s, now += frame).reason,
                      hp > 0 ? B::GateReason::Throw : B::GateReason::Dead);
        }
        // The grab itself: no requests (2 s).
        for (int i = 0; i < 120; ++i) {
            B::GateSignals s = Frame({{9000, i * (1.0f / 60.0f)}}, 0, false);
            s.throw_valid = s.hp_valid = true;
            s.throw_state = 3;
            s.hp = hp;
            s.throw_release = release;
            const B::GateOut out = gate.Step(s, now += frame);
            EXPECT_FALSE(out.ik[C::Right]);
            EXPECT_FALSE(out.ik[C::Left]);
        }
        // The cancel window (R1 / R2 and a move) while the throw state plays on 0.8 s.
        const std::int64_t window = now + frame;
        for (int i = 0; i < 48; ++i) {
            B::GateSignals s = Frame({{9000, 2.0f + i * (1.0f / 60.0f)}}, kIdleAllow, true);
            s.throw_valid = s.hp_valid = true;
            s.throw_state = 3;
            s.hp = hp;
            s.throw_release = release;
            const B::GateOut out = gate.Step(s, now += frame);
            if (back_after_window < 0 && out.ik[C::Right] && out.ik[C::Left] &&
                out.reason != B::GateReason::Throw) {
                back_after_window = now - window;
                EXPECT_TRUE(out.throw_released);
                EXPECT_TRUE(out.torso);
            }
        }
        // The throw state ends.
        B::GateSignals s = Frame({}, kIdleAllow, true);
        s.throw_valid = s.hp_valid = true;
        s.throw_state = 0;
        s.hp = hp;
        gate.Step(s, now += frame);
    };
    B::AnimGate gate;
    std::int64_t now = 1000 * kMs;
    std::int64_t back = -1;
    grab(gate, now, 500, true, back);
    EXPECT_GE(back, 50 * kMs);
    EXPECT_LE(back, 50 * kMs + frame);
    // The next grab takes the arms again, and hands them back the same way.
    grab(gate, now, 500, true, back);
    EXPECT_GE(back, 50 * kMs);
    EXPECT_LE(back, 50 * kMs + frame);
    // cinematic_release=0: the whole throw state, as before.
    B::AnimGate old;
    grab(old, now, 500, false, back);
    EXPECT_EQ(back, -1);
    // A fatal grab (HP 0) stays the game's.
    B::AnimGate fatal;
    grab(fatal, now, 0, true, back);
    EXPECT_EQ(back, -1);
}

// The light stagger a000_020134 as the game plays it (QA run c14 and the mock arena run f2): its
// clip is seen every frame for 0.833 s; its TAE takes R1 / R2 and L2 at 0.33 s (JumpTable 4 and 16)
// and a move at 0.40 s (JumpTable 11), then idle follows. The arms come back at the attack window
// (vanilla R1 would interrupt the stagger there), the torso at the move window, and the clip
// playing on never takes them again; the next hit (the same clip from 0) does.
TEST(AnimGate, CancelWindowsHandTheArmsBack) {
    B::AnimGate gate;
    const std::int64_t frame = 1'000'000'000 / 60;
    const float dt = 1.0f / 60.0f;
    std::int64_t now = 1000 * kMs;
    for (int i = 0; i < 5; ++i) {
        gate.Step(Frame({{0, 1.0f + i * dt}}, kIdleAllow, true), now += frame);
    }
    for (int hit = 0; hit < 2; ++hit) {
        const std::int64_t start = now + frame;
        std::int64_t right_at = -1, left_at = -1, torso_at = -1;
        int flips = 0;
        bool last_right = true;
        for (int i = 0; i * dt < 0.833f; ++i) {
            const float t = i * dt;
            // The first step still carries idle's requests (a stale frame): no release from it.
            const std::uint64_t allow =
                i == 0 ? kIdleAllow : (t >= 0.33f ? kAttacks | 0x400004 : 0);
            const B::GateOut out =
                gate.Step(Frame({{20134, t}}, allow, i > 0 && t >= 0.40f), now += frame);
            if (i == 0) {
                EXPECT_FALSE(out.ik[C::Right]);
                EXPECT_FALSE(out.ik[C::Left]);
                EXPECT_FALSE(out.torso);
                EXPECT_EQ(out.owner, AnimClass::Damage);
            }
            if (right_at < 0 && out.ik[C::Right]) {
                right_at = now - start;
            }
            if (left_at < 0 && out.ik[C::Left]) {
                left_at = now - start;
            }
            if (torso_at < 0 && out.torso) {
                torso_at = now - start;
                EXPECT_EQ(Str(out.released), "move cancel");
            }
            flips += out.ik[C::Right] != last_right ? 1 : 0;
            last_right = out.ik[C::Right];
            EXPECT_TRUE(out.attack_ok[C::Right]);
        }
        // Within one frame of the windows (0.33 / 0.40 s); the arm never goes back to the game.
        EXPECT_NEAR(Millis(right_at), 330.0, 17.0) << "hit " << hit;
        EXPECT_EQ(left_at, right_at) << "hit " << hit;
        EXPECT_NEAR(Millis(torso_at), 400.0, 17.0) << "hit " << hit;
        EXPECT_EQ(flips, 2) << "game, then yours, once"; // the first frame's take counts as one
        // Idle after it.
        const B::GateOut out = gate.Step(Frame({{0, 0.0f}}, kIdleAllow, true), now += frame);
        EXPECT_TRUE(out.ik[C::Right] && out.ik[C::Left] && out.torso);
        EXPECT_FALSE(out.held);
    }
    // Without the cancel signal (a mismatch at install) only the next id ends it: idle at 0.833 s.
    const std::int64_t start = now + frame;
    std::int64_t back_at = -1;
    for (int i = 0; i * dt < 1.2f && back_at < 0; ++i) {
        const float t = i * dt;
        const B::GateOut out =
            t < 0.833f ? gate.Step(Frame({{20134, t}}, 0, false, false), now += frame)
                       : gate.Step(Frame({{0, t - 0.833f}}, 0, false, false), now += frame);
        if (out.ik[C::Right]) {
            back_at = now - start;
            EXPECT_EQ(Str(out.released), "next id");
        }
    }
    EXPECT_NEAR(Millis(back_at), 833.0, 17.0);
}

// A roll (a000_007101, seen every frame for 1.4 s; R1 at 0.60 s, a move at 0.70 s) hands the arms
// back at its attack window; a clip that held both arms keeps its hold while it is seen every frame
// and lets go its length + 0.1 s after the last time (no signal, no next id).
TEST(AnimGate, RollAndClipsSeenEveryFrame) {
    B::AnimGate gate;
    const std::int64_t frame = 1'000'000'000 / 60;
    const float dt = 1.0f / 60.0f;
    std::int64_t now = 0;
    std::int64_t right_at = -1;
    const std::int64_t start = now + frame;
    for (int i = 0; i * dt < 1.4f; ++i) {
        const float t = i * dt;
        const B::GateOut out = gate.Step(
            Frame({{7101, t}}, t >= 0.60f ? kIdleAllow : 0x30020, t >= 0.70f), now += frame);
        if (right_at < 0 && out.ik[C::Right]) {
            right_at = now - start;
        }
    }
    EXPECT_NEAR(Millis(right_at), 600.0, 17.0);
    // A hit without any cancel signal, seen for 2.5 s (longer than its 1.07 s hold).
    gate.Reset();
    now = 0;
    B::GateOut out{};
    for (int i = 0; i * dt < 2.5f; ++i) {
        out = gate.Step(Frame({{20000, i * dt}}, 0, false, false), now += frame);
        EXPECT_FALSE(out.ik[C::Right]) << "frame " << i;
    }
    const std::int64_t last = now;
    std::int64_t back = -1;
    for (int i = 0; i < 120 && back < 0; ++i) {
        out = gate.Step(Frame({}, 0, false, false), now += frame);
        if (out.ik[C::Right]) {
            back = now - last;
            EXPECT_EQ(Str(out.released), "length");
        }
    }
    EXPECT_NEAR(Millis(back), 1070.0, 17.0);
}

// arm_keep_on_hit / arm_keep_on_dodge: which clips leave the arms yours.
TEST(AnimClasses, KeepKinds) {
    using B::KeepKind;
    for (const std::int32_t id : {20000, 20020, 20104, 20134, 20203, 20214, 20234, 20334, 20399}) {
        EXPECT_EQ(B::KeepKindOf(id), KeepKind::Hit) << id;
    }
    // Knockdowns and launches (the hunter on the floor) and the other damage clips: the game's.
    for (const std::int32_t id : {20400, 20430, 20500, 20510, 20600, 20700, 20800, 20900, 21000,
                                  21300, 22000, 23999, 24000}) {
        EXPECT_EQ(B::KeepKindOf(id), KeepKind::None) << id;
    }
    for (const std::int32_t id : {7000, 7050, 7100, 7103, 7107, 7150, 7157, 7200, 7250, 21007200}) {
        EXPECT_EQ(B::KeepKindOf(id), KeepKind::Dodge) << id;
    }
    // The backstep and roll up from lying (after a knockdown), the jump, falls, other classes.
    for (const std::int32_t id : {7300, 7350, 7400, 7457, 7800, 7108, 8000, 0, 3000, 11000, 30000,
                                  100000, 101140, -1}) {
        EXPECT_EQ(B::KeepKindOf(id), KeepKind::None) << id;
    }
    EXPECT_TRUE(B::KeepsArms(20203, true, false));
    EXPECT_FALSE(B::KeepsArms(20203, false, true));
    EXPECT_TRUE(B::KeepsArms(7103, false, true));
    EXPECT_FALSE(B::KeepsArms(7103, true, false));
    EXPECT_FALSE(B::KeepsArms(20500, true, true));
    // Simon's Bowblade's bow clips (vr.ini bow): a149_04xxxx only, and only with bow on.
    for (const std::int32_t id : {149040000, 149042620, 149042630, 149044500, 149046000}) {
        EXPECT_EQ(B::KeepKindOf(id), KeepKind::Bow) << id;
    }
    for (const std::int32_t id : {149030000, 149050000, 148040000, 40000, 101040000}) {
        EXPECT_EQ(B::KeepKindOf(id), KeepKind::None) << id;
    }
    EXPECT_TRUE(B::KeepsArms(149040000, false, false, true));
    EXPECT_FALSE(B::KeepsArms(149040000, true, true, false));
    // A blow on the raised shield (vr.ini shield_raise): the guard reactions it holds, any TAE
    // group; one that breaks the guard (0103x0 and up) is the game's stagger.
    for (const std::int32_t id : {10000, 10012, 10029, 2010020}) {
        EXPECT_EQ(B::KeepKindOf(id), KeepKind::Guard) << id;
    }
    for (const std::int32_t id : {10030, 10093, 2010040}) {
        EXPECT_EQ(B::KeepKindOf(id), KeepKind::None) << id;
    }
    EXPECT_TRUE(B::KeepsArms(10010, false, false, false, true));
    EXPECT_FALSE(B::KeepsArms(10010, true, true, true, false));
    // The game's transform animations (vr.ini hand_transform): the Chikage's 19020, the
    // Bloodletter's 19620, Tonitrus' charge, the Boom Hammer's closing.
    for (const std::int32_t id : {19020, 19230, 19620, 19630, 19140, 3019400}) {
        EXPECT_EQ(B::KeepKindOf(id), KeepKind::Transform) << id;
    }
    EXPECT_TRUE(B::KeepsArms(19020, false, false, false, false, true));
    EXPECT_FALSE(B::KeepsArms(19020, true, true, true, true, false));
}

// A kept hit (arm_keep_on_hit): the same clip and windows as CancelWindowsHandTheArmsBack. Both
// arms and the torso alignment stay yours every frame; swings and shots wait for exactly the frame
// the game would have handed each arm back (R1 / L2 at 0.33 s), and the hold ends at the move
// cancel (0.40 s) as before.
TEST(AnimGate, KeptHitKeepsTheArmsAndWaitsForTheGamesWindow) {
    const std::int64_t frame = 1'000'000'000 / 60;
    const float dt = 1.0f / 60.0f;
    for (const bool cancel_signal : {true, false}) {
        B::AnimGate kept, plain;
        std::int64_t now = 1000 * kMs;
        const auto step = [&](B::GateSignals s, B::GateOut& a, B::GateOut& b) {
            now += frame;
            b = plain.Step(s, now);
            s.keep_hit = true;
            s.keep_dodge = true;
            a = kept.Step(s, now);
        };
        B::GateOut a{}, b{};
        for (int i = 0; i < 5; ++i) {
            step(Frame({{0, 1.0f + i * dt}}, kIdleAllow, true, cancel_signal), a, b);
        }
        const std::int64_t start = now + frame;
        std::int64_t ok_at = -1, game_back_at = -1;
        for (int i = 0; i * dt < 0.833f; ++i) {
            const float t = i * dt;
            const std::uint64_t allow =
                i == 0 ? kIdleAllow : (t >= 0.33f ? kAttacks | 0x400004 : 0);
            step(Frame({{20134, t}}, allow, i > 0 && t >= 0.40f, cancel_signal), a, b);
            EXPECT_TRUE(a.ik[C::Right] && a.ik[C::Left]) << "frame " << i;
            EXPECT_TRUE(a.torso) << "frame " << i;
            EXPECT_EQ(a.reason, a.held ? B::GateReason::Anim : B::GateReason::None);
            EXPECT_EQ(a.kept, a.held);
            // The swing lock is the game's hand-over, frame for frame.
            EXPECT_EQ(a.attack_ok[C::Right], !b.held || b.ik[C::Right]) << "frame " << i;
            EXPECT_EQ(a.attack_ok[C::Left], !b.held || b.ik[C::Left]) << "frame " << i;
            if (ok_at < 0 && a.attack_ok[C::Right]) {
                ok_at = now - start;
            }
            if (game_back_at < 0 && b.ik[C::Right]) {
                game_back_at = now - start;
            }
            if (i == 0) {
                EXPECT_TRUE(a.kept);
                EXPECT_FALSE(a.attack_ok[C::Right]);
                EXPECT_FALSE(a.attack_ok[C::Left]);
                EXPECT_FALSE(b.ik[C::Right]);
            }
        }
        if (cancel_signal) {
            EXPECT_NEAR(Millis(ok_at), 330.0, 17.0);
            EXPECT_EQ(ok_at, game_back_at);
        } else {
            EXPECT_LT(ok_at, 0) << "no window seen: the swing lock holds through the clip";
        }
        // The next id (idle) ends it either way.
        step(Frame({{0, 0.0f}}, kIdleAllow, true, cancel_signal), a, b);
        EXPECT_TRUE(a.attack_ok[C::Right] && a.attack_ok[C::Left]);
        EXPECT_FALSE(a.kept);
        EXPECT_TRUE(a.ik == b.ik);
    }
}

// A kept roll (arm_keep_on_dodge): a000_007101 as in RollAndClipsSeenEveryFrame; the arms stay
// yours, the swing waits for R1 at 0.60 s. A knockdown (a000_020500) and, with only
// arm_keep_on_hit, the roll still take the arms. arm_anim_gate=0 keeps today's behaviour (no
// lock), and the hard gates still take both arms.
TEST(AnimGate, KeptDodgeAndWhatStaysTheGames) {
    const std::int64_t frame = 1'000'000'000 / 60;
    const float dt = 1.0f / 60.0f;
    const auto roll = [&](bool keep_hit, bool keep_dodge, bool anim_gate, std::int64_t& ok_at,
                          bool& ik_always) {
        B::AnimGate gate;
        std::int64_t now = 0;
        const std::int64_t start = now + frame;
        ok_at = -1;
        ik_always = true;
        for (int i = 0; i * dt < 1.4f; ++i) {
            const float t = i * dt;
            B::GateSignals s =
                Frame({{7101, t}}, t >= 0.60f ? kIdleAllow : 0x30020, t >= 0.70f);
            s.keep_hit = keep_hit;
            s.keep_dodge = keep_dodge;
            s.anim_gate = anim_gate;
            const B::GateOut out = gate.Step(s, now += frame);
            ik_always = ik_always && out.ik[C::Right] && out.ik[C::Left];
            if (ok_at < 0 && out.attack_ok[C::Right] && out.ik[C::Right]) {
                ok_at = now - start;
            }
        }
    };
    std::int64_t ok_at = -1;
    bool ik_always = false;
    roll(false, true, true, ok_at, ik_always);
    EXPECT_TRUE(ik_always);
    EXPECT_NEAR(Millis(ok_at), 600.0, 17.0);
    roll(true, false, true, ok_at, ik_always);
    EXPECT_FALSE(ik_always) << "arm_keep_on_dodge=0: the roll takes the arms";
    EXPECT_NEAR(Millis(ok_at), 600.0, 17.0);
    roll(true, true, false, ok_at, ik_always);
    EXPECT_TRUE(ik_always);
    EXPECT_EQ(ok_at, 0) << "arm_anim_gate=0: nothing waits (as before)";

    // A knockdown with both keeps on: the game's arms.
    B::AnimGate gate;
    std::int64_t now = 0;
    B::GateSignals s = Frame({{20500, 0.0f}}, 0, false);
    s.keep_hit = s.keep_dodge = true;
    B::GateOut out = gate.Step(s, now += frame);
    EXPECT_FALSE(out.ik[C::Right] || out.ik[C::Left]);
    EXPECT_FALSE(out.kept);
    // A kept stagger, then death: the hard gate takes both arms.
    gate.Reset();
    s = Frame({{20203, 0.0f}}, 0, false);
    s.keep_hit = true;
    out = gate.Step(s, now += frame);
    EXPECT_TRUE(out.kept && out.ik[C::Right] && !out.attack_ok[C::Right]);
    s = Frame({{20203, dt}}, 0, false);
    s.keep_hit = true;
    s.hp_valid = true;
    s.hp = 0;
    out = gate.Step(s, now += frame);
    EXPECT_FALSE(out.ik[C::Right] || out.ik[C::Left] || out.kept);
    EXPECT_EQ(out.reason, B::GateReason::Dead);
    // keep_torso=0: the kept clip's torso is the game's (its class policy).
    gate.Reset();
    s = Frame({{20203, 0.0f}}, 0, false);
    s.keep_hit = true;
    s.keep_torso = false;
    out = gate.Step(s, now += frame);
    EXPECT_TRUE(out.kept && out.ik[C::Right] && out.ik[C::Left]);
    EXPECT_FALSE(out.torso);
}

// The game's transform (L1; transform=vanilla, or a weapon the weapon-only transform leaves on L1):
// the Saw Cleaver's a000_019000, seen every frame for its 1.167 s (the form switch at 0.467 s, R1 /
// L2 taken from 0.5 s, a move from 0.7 s), then the form-B idle a003_000000. Both arms and the
// torso are the game's until its windows, never the 5.8 s of its TAE length (4.57 s after the
// last frame).
TEST(AnimGate, GameTransformHandsTheArmsBackAfterItsClip) {
    B::AnimGate gate;
    const std::int64_t frame = 1'000'000'000 / 60;
    const float dt = 1.0f / 60.0f;
    constexpr float kClip = 1.167f;
    constexpr std::int32_t kIdleB = 3000000;
    std::int64_t now = 0;
    gate.Step(Frame({{0, 1.0f}}, kIdleAllow, true), now += frame);
    // With the game's cancel windows.
    {
        const std::int64_t start = now + frame;
        std::int64_t right_at = -1, left_at = -1, torso_at = -1;
        for (int i = 0; i * dt < kClip; ++i) {
            const float t = i * dt;
            const std::uint64_t allow = i == 0 ? kIdleAllow : (t >= 0.5f ? kIdleAllow : 0x30020);
            const B::GateOut out =
                gate.Step(Frame({{19000, t}}, allow, i > 0 && t >= 0.7f), now += frame);
            if (i == 0) {
                EXPECT_FALSE(out.ik[C::Right] || out.ik[C::Left] || out.torso);
                EXPECT_EQ(out.owner, AnimClass::Transform);
                EXPECT_EQ(out.hold_ns, 1267 * kMs);
            }
            if (right_at < 0 && out.ik[C::Right]) {
                right_at = now - start;
            }
            if (left_at < 0 && out.ik[C::Left]) {
                left_at = now - start;
            }
            if (torso_at < 0 && out.torso) {
                torso_at = now - start;
                EXPECT_EQ(Str(out.released), "move cancel");
            }
            if (torso_at >= 0) {
                EXPECT_FALSE(out.held) << "the clip playing on does not take the arms again";
            }
        }
        EXPECT_NEAR(Millis(right_at), 500.0, 17.0);
        EXPECT_EQ(left_at, right_at);
        EXPECT_NEAR(Millis(torso_at), 700.0, 17.0);
        const B::GateOut out = gate.Step(Frame({{kIdleB, 0.0f}}, kIdleAllow, true), now += frame);
        EXPECT_TRUE(out.ik[C::Right] && out.ik[C::Left] && out.torso);
    }
    // Without the cancel signal: the idle after the clip ends it (the next id).
    {
        const std::int64_t start = now + frame;
        std::int64_t back_at = -1;
        for (int i = 0; i * dt < 2.0f && back_at < 0; ++i) {
            const float t = i * dt;
            const B::GateOut out =
                t < kClip ? gate.Step(Frame({{19010, t}}, 0, false, false), now += frame)
                          : gate.Step(Frame({{kIdleB, t - kClip}}, 0, false, false), now += frame);
            if (out.ik[C::Right]) {
                back_at = now - start;
                EXPECT_TRUE(out.ik[C::Left] && out.torso);
                EXPECT_EQ(Str(out.released), "next id");
            }
        }
        EXPECT_NEAR(Millis(back_at), 1167.0, 20.0);
    }
    // Nothing at all after it: its clip length + 0.1 s after the last frame (was 4.67 s).
    {
        const std::int64_t start = now + frame;
        std::int64_t back_at = -1;
        for (int i = 0; i * dt < 6.0f && back_at < 0; ++i) {
            const float t = i * dt;
            const B::GateOut out =
                t < kClip ? gate.Step(Frame({{19000, t}}, 0, false, false), now += frame)
                          : gate.Step(Frame({}, 0, false, false), now += frame);
            if (out.ik[C::Right]) {
                back_at = now - start;
                EXPECT_EQ(Str(out.released), "length");
            }
        }
        EXPECT_NEAR(Millis(back_at), 1167.0 + 1267.0, 34.0);
    }
    // Walking: the upper-body variant a000_019001 over the run cycle; the run cycle alone after it.
    {
        const std::int64_t start = now + frame;
        std::int64_t back_at = -1;
        for (int i = 0; i * dt < 2.0f && back_at < 0; ++i) {
            const float t = i * dt;
            const B::GateOut out =
                t < kClip
                    ? gate.Step(Frame({{4000, 1.0f + t}, {19001, t}}, 0, false, false), now += frame)
                    : gate.Step(Frame({{4000, 1.0f + t}}, 0, false, false), now += frame);
            if (t < kClip) {
                EXPECT_FALSE(out.ik[C::Right]) << "frame " << i;
            }
            if (out.ik[C::Right]) {
                back_at = now - start;
            }
        }
        EXPECT_NEAR(Millis(back_at), 1167.0, 20.0);
    }
}

// A weapon switch (a000_011000 for 0.167 s, then a000_011010 for 0.567 s; the game takes R1 again
// at 0.23 s into the second) keeps both arms yours the whole time, and only the swing and the shot
// wait for the game; the clip playing on does not lock them again, the next switch does.
TEST(AnimGate, WeaponSwitchKeepsTheArms) {
    B::AnimGate gate;
    const std::int64_t frame = 1'000'000'000 / 60;
    const float dt = 1.0f / 60.0f;
    std::int64_t now = 0;
    gate.Step(Frame({{0, 1.0f}}, kIdleAllow, true), now += frame);
    for (int round = 0; round < 2; ++round) {
        const std::int64_t start = now + frame;
        std::int64_t free_at = -1;
        int locks = 0;
        bool last_ok = true;
        for (int i = 0; i * dt < 0.167f; ++i) {
            const B::GateOut out = gate.Step(Frame({{11000, i * dt}}, 0, false), now += frame);
            EXPECT_TRUE(out.ik[C::Right] && out.ik[C::Left] && out.torso);
            EXPECT_FALSE(out.attack_ok[C::Right]);
            EXPECT_FALSE(out.attack_ok[C::Left]);
            EXPECT_TRUE(out.switching);
            locks += last_ok && !out.attack_ok[C::Right] ? 1 : 0;
            last_ok = out.attack_ok[C::Right];
        }
        const std::int64_t second = now + frame;
        for (int i = 0; i * dt < 0.567f; ++i) {
            const float t = i * dt;
            const B::GateOut out = gate.Step(
                Frame({{11010, t}}, t >= 0.23f ? kIdleAllow : 0, t >= 0.33f), now += frame);
            EXPECT_TRUE(out.ik[C::Right] && out.ik[C::Left] && out.torso);
            EXPECT_FALSE(out.held);
            if (free_at < 0 && out.attack_ok[C::Right]) {
                free_at = now - second;
            }
            locks += last_ok && !out.attack_ok[C::Right] ? 1 : 0;
            last_ok = out.attack_ok[C::Right];
        }
        EXPECT_NEAR(Millis(free_at), 230.0, 17.0) << "round " << round;
        EXPECT_GE(now - start, 0);
        EXPECT_EQ(locks, 1) << "one lock per switch, never again while its clips play on";
        const B::GateOut out = gate.Step(Frame({{0, 0.0f}}, kIdleAllow, true), now += frame);
        EXPECT_TRUE(out.attack_ok[C::Right] && out.attack_ok[C::Left]);
    }
    // Without the cancel signal the lock lasts the switch's own 0.75 s.
    std::int64_t free_at = -1;
    const std::int64_t start = now + frame;
    for (int i = 0; i < 90 && free_at < 0; ++i) {
        const B::GateOut out = gate.Step(Frame({{11000, i * dt}}, 0, false, false), now += frame);
        if (out.attack_ok[C::Right]) {
            free_at = now - start;
        }
    }
    EXPECT_NEAR(Millis(free_at), 750.0, 17.0);
}

// ---- Torso yaw ----------------------------------------------------------------------------------

TEST(TorsoYaw, DeadzoneFollowWrap) {
    B::TorsoYaw t;
    t.Reset(0.0f);
    const float dz = 25.0f * kDeg, tau = 0.25f, dt = 1.0f / 60.0f;
    // Inside the deadzone nothing moves.
    for (int i = 0; i < 120; ++i) {
        t.Update((i % 2 == 0 ? 20.0f : -24.0f) * kDeg, dz, tau, dt);
    }
    EXPECT_EQ(t.yaw, 0.0f);
    // At 60 degrees it follows toward 35: 1 - e^-2 of the way after 0.5 s, all of it later.
    for (int i = 0; i < 30; ++i) {
        t.Update(60.0f * kDeg, dz, tau, dt);
    }
    EXPECT_NEAR(t.yaw / kDeg, 35.0f * (1.0f - std::exp(-2.0f)), 0.5f);
    for (int i = 0; i < 300; ++i) {
        t.Update(60.0f * kDeg, dz, tau, dt);
    }
    EXPECT_NEAR(t.yaw / kDeg, 35.0f, 0.01f);
    // Across +-180 the short way round.
    t.Reset(170.0f * kDeg);
    for (int i = 0; i < 600; ++i) {
        t.Update(-150.0f * kDeg, dz, tau, dt);
    }
    EXPECT_NEAR(B::WrapPi(t.yaw - (-175.0f * kDeg)) / kDeg, 0.0f, 0.01f);
    // tau 0: at once.
    t.Reset(0.0f);
    t.Update(90.0f * kDeg, dz, 0.0f, dt);
    EXPECT_NEAR(t.yaw / kDeg, 65.0f, 1e-3f);
}

// The relative follow (the default): the torso's turn from the legs is the head's excess over the
// deadzone from the legs' heading, with no memory: back inside the deadzone it goes back to the
// legs, and turning the legs and the view together (a snap turn) never twists the waist.
TEST(TorsoYaw, RelativeFollowsTheLegs) {
    B::TorsoYaw t;
    const float dz = 25.0f * kDeg, tau = 0.25f, dt = 1.0f / 60.0f;
    for (int i = 0; i < 120; ++i) {
        t.UpdateRelative((i % 2 == 0 ? 20.0f : -24.0f) * kDeg, dz, tau, dt);
    }
    EXPECT_EQ(t.yaw, 0.0f);
    for (int i = 0; i < 30; ++i) {
        t.UpdateRelative(60.0f * kDeg, dz, tau, dt);
    }
    EXPECT_NEAR(t.yaw / kDeg, 35.0f * (1.0f - std::exp(-2.0f)), 0.5f);
    for (int i = 0; i < 300; ++i) {
        t.UpdateRelative(60.0f * kDeg, dz, tau, dt);
    }
    EXPECT_NEAR(t.yaw / kDeg, 35.0f, 0.01f);
    // Back to 20 degrees (inside the deadzone): the torso returns to the legs (the world-anchored
    // filter stayed at 35 - 15 = 20 degrees off them, a lasting twist).
    for (int i = 0; i < 300; ++i) {
        t.UpdateRelative(20.0f * kDeg, dz, tau, dt);
    }
    EXPECT_NEAR(t.yaw / kDeg, 0.0f, 0.01f);
    // Behind the legs across +-180: the short way round, clamped later by torso_max.
    t.Reset(0.0f);
    for (int i = 0; i < 600; ++i) {
        t.UpdateRelative(-170.0f * kDeg, dz, tau, dt);
    }
    EXPECT_NEAR(t.yaw / kDeg, -145.0f, 0.01f);
    t.Reset(0.0f);
    t.UpdateRelative(90.0f * kDeg, dz, 0.0f, dt);
    EXPECT_NEAR(t.yaw / kDeg, 65.0f, 1e-3f);
}

// A snap turn (the legs and the view turn 30 degrees together) six times while walking: the
// relative follow keeps the torso on the legs; the old world-anchored one left it 25 degrees
// behind them (the waist twisted) until the head turned further.
TEST(BodySolver, SnapTurnsNeverTwistTheWaist) {
    const Pose ref = Reference();
    for (const bool relative : {true, false}) {
        B::BodySolver solver;
        B::BodyInput in = DefaultInput(ref);
        in.body_weight = 1.0f;
        B::BodyParams p{};
        p.align = false;
        p.torso_relative = relative;
        Pose pose{};
        B::BodyOutput out{};
        float yaw = 0.0f;
        for (int turn = 0; turn < 6; ++turn) {
            yaw += 30.0f * kDeg;
            in.chr_yaw = B::WrapPi(yaw);
            in.head_yaw = B::WrapPi(yaw);
            for (int i = 0; i < 90; ++i) {
                pose = ref;
                out = solver.Solve(pose.data(), in, p);
            }
        }
        ASSERT_TRUE(out.ok);
        if (relative) {
            EXPECT_NEAR(out.torso_delta / kDeg, 0.0f, 1e-3f);
        } else {
            EXPECT_NEAR(std::abs(out.torso_delta) / kDeg, 25.0f, 0.5f);
        }
    }
}

// ---- Elbow hint ---------------------------------------------------------------------------------

TEST(ArmPole, ValuesContinuityMirror) {
    const B::BodyFrame f{};
    const Vec3 out_r = f.right; // the right arm's side
    // Neutral: hand below and in front of the shoulder, on its own side.
    const Vec3 neutral = B::ArmPole(f, 1.0f, {-0.05f, -0.3f, -0.3f}, {-0.2f, -0.3f, -0.3f});
    EXPECT_LT(Dist(neutral, out_r * 0.7f + f.up * -1.0f + f.forward * -0.3f), 1e-5f);
    // Raised, behind the head, across, behind the back.
    EXPECT_LT(Dist(B::ArmPole(f, 1.0f, {0.0f, 0.3f, -0.3f}, {-0.2f, 0.3f, -0.3f}),
                   out_r * 1.0f + f.up * -0.2f),
              1e-5f);
    EXPECT_LT(Dist(B::ArmPole(f, 1.0f, {0.0f, 0.3f, 0.3f}, {-0.2f, 0.3f, 0.3f}),
                   out_r * 0.6f + f.up * 0.6f + f.forward * 0.6f),
              1e-5f);
    EXPECT_LT(Dist(B::ArmPole(f, 1.0f, {0.4f, -0.2f, -0.3f}, {0.3f, -0.2f, -0.3f}),
                   out_r * 0.2f + f.up * -0.6f + f.forward * 0.8f),
              1e-5f);
    EXPECT_LT(Dist(B::ArmPole(f, 1.0f, {0.0f, -0.3f, 0.3f}, {-0.2f, -0.3f, 0.3f}),
                   out_r * 0.9f + f.up * -0.3f + f.forward * -0.6f),
              1e-5f);
    // Continuous and never short over a grid of hand positions; the left arm is the mirror.
    float max_step = 0.0f, min_length = 1e9f, max_mirror = 0.0f;
    const float h = 0.005f;
    for (float x = -0.6f; x <= 0.6f; x += 0.05f) {
        for (float y = -0.7f; y <= 0.6f; y += 0.05f) {
            for (float z = -0.6f; z <= 0.5f; z += 0.05f) {
                const Vec3 rel{x, y, z};
                const Vec3 spine = rel + Vec3{-0.17f, 0.1f, 0.0f};
                const Vec3 p = B::ArmPole(f, 1.0f, rel, spine);
                min_length = std::min(min_length, B::Length(p));
                for (const Vec3 d : {Vec3{h, 0, 0}, Vec3{0, h, 0}, Vec3{0, 0, h}}) {
                    max_step = std::max(max_step, Dist(p, B::ArmPole(f, 1.0f, rel + d, spine + d)));
                }
                const Vec3 m =
                    B::ArmPole(f, -1.0f, {-rel.x, rel.y, rel.z}, {-spine.x, spine.y, spine.z});
                max_mirror = std::max(max_mirror, Dist(m, {-p.x, p.y, p.z}));
            }
        }
    }
    std::printf("  pole: shortest %.3f, largest change %.4f per 5 mm, mirror error %.2g\n",
                min_length, max_step, max_mirror);
    EXPECT_GT(min_length, 0.5f);
    EXPECT_LT(max_step, 0.12f); // smoothstep slopes over 0.15 m ranges
    EXPECT_LT(max_mirror, 1e-5f);
}

namespace {

// The elbow hint before the near-chest term (a hand across the body only from 0.10 m past the
// midline): the reference for where an elbow raised across the face or over the head goes.
Vec3 PoleWithoutNearChest(const B::BodyFrame& f, float side, Vec3 hand_from_shoulder,
                          Vec3 hand_from_spine) {
    const Vec3 out = f.right * side;
    const float up = B::Dot(hand_from_shoulder, f.up);
    const float forward = B::Dot(hand_from_shoulder, f.forward);
    const float across = -B::Dot(hand_from_spine, out);
    const float w_up = B::Smoothstep(-0.05f, 0.15f, up);
    const float w_back = B::Smoothstep(0.05f, 0.20f, -forward);
    const float w_across = B::Smoothstep(0.10f, 0.25f, across) * (1.0f - w_back);
    const Vec3 low = B::Lerp(Vec3{0.7f, -1.0f, -0.3f}, Vec3{0.9f, -0.3f, -0.6f}, w_back);
    const Vec3 high = B::Lerp(Vec3{1.0f, -0.2f, 0.0f}, Vec3{0.6f, 0.6f, 0.6f}, w_back);
    const Vec3 p = B::Lerp(B::Lerp(low, high, w_up), Vec3{0.2f, -0.6f, 0.8f}, w_across);
    return out * p.x + f.up * p.y + f.forward * p.z;
}

} // namespace

TEST(ArmPole, RaisedAcrossTheFaceKeepsTheElbowOutOfView) {
    // The right hand over a grid in front of the face (up to 0.25 m across the midline, 0.1 m
    // under to 0.3 m over the eye height, 0-0.3 m in front of it) and over the head (0.15-0.45 m
    // above it, 0.15 m behind to 0.3 m in front), blade level, up and down: the near-chest term
    // must never bring the elbow into view (within 10 cm of the camera eye, or within 35 degrees
    // of the view axis closer than 0.5 m) where the hint without it kept it out. Arms folded in
    // front of the chest still get their elbows further forward than without it.
    const Pose ref = Reference();
    const Pose model = Fk(ref);
    const B::ArmRig rig = B::HunterRightArm();
    const Vec3 shoulder = model[rig.upperArm].t;
    const Vec3 spine = model[C::Spine2].t;
    const Vec3 eye{0.0f, P::HeadHeight, -0.1f}; // GripTarget's camera eye; the view is -Z
    const auto solve = [&](bool near_chest, const B::ModelTarget& t) {
        B::ArmParams params = B::HunterArmParams();
        params.pole = near_chest ? B::ArmPole(B::BodyFrame{}, rig.side, t.p - shoulder, t.p - spine)
                                 : PoleWithoutNearChest(B::BodyFrame{}, rig.side, t.p - shoulder,
                                                        t.p - spine);
        B::ArmState state{};
        B::ArmResult r{};
        for (int i = 0; i < 60; ++i) { // the elbow side settles (its turn rate is limited)
            Pose pose = ref;
            r = B::SolveArm(pose.data(), C::Parents.data(), C::BoneCount, rig, {t.p, t.q, 1.0f},
                            params, state, 1.0f / 60.0f);
        }
        return r;
    };
    const auto in_view = [&](Vec3 elbow) {
        const Vec3 v = elbow - eye;
        const float d = B::Length(v);
        return d < 0.10f || (d < 0.5f && AngleDeg(v, {0.0f, 0.0f, -1.0f}) < 35.0f);
    };
    std::vector<Vec3> face, overhead, folded; // tracking offsets from the head
    for (const float dy : {0.30f, 0.20f, 0.10f, 0.0f, -0.10f}) {
        for (const float dx : {0.10f, 0.0f, -0.05f, -0.10f, -0.15f, -0.20f, -0.25f}) {
            for (const float fwd : {0.0f, 0.10f, 0.20f, 0.30f}) {
                face.push_back({dx, dy, -fwd});
            }
        }
    }
    for (int iy = 0; iy <= 6; ++iy) {
        for (int ix = 0; ix <= 10; ++ix) {
            for (int iz = 0; iz <= 9; ++iz) {
                overhead.push_back({-0.30f + 0.05f * static_cast<float>(ix),
                                    0.15f + 0.05f * static_cast<float>(iy),
                                    0.15f - 0.05f * static_cast<float>(iz)});
            }
        }
    }
    for (int iy = 0; iy <= 4; ++iy) {
        for (int ix = 0; ix <= 6; ++ix) {
            for (int iz = 0; iz <= 4; ++iz) {
                folded.push_back({-0.25f + 0.05f * static_cast<float>(ix),
                                  -0.45f + 0.05f * static_cast<float>(iy),
                                  -0.08f - 0.04f * static_cast<float>(iz)});
            }
        }
    }
    int cells = 0, newly_in_view = 0, in_view_before = 0;
    for (const auto* grid : {&face, &overhead}) {
        for (const Vec3& d : *grid) {
            for (const float pitch : {0.0f, 60.0f, -60.0f}) {
                const B::ModelTarget t =
                    GripTarget(Vec3{0.0f, P::HeadHeight, 0.0f} + d, 0.0f, pitch, 0.0f, C::Right);
                const B::ArmResult before = solve(false, t);
                const B::ArmResult now = solve(true, t);
                ASSERT_EQ(now.status, B::ArmStatus::Solved);
                ++cells;
                in_view_before += in_view(before.elbow) ? 1 : 0;
                if (in_view(now.elbow) && !in_view(before.elbow)) {
                    ++newly_in_view;
                    std::printf("  elbow in view: hand (%.2f %.2f %.2f) pitch %.0f, %.3f m from "
                                "the eye\n",
                                d.x, d.y, d.z, pitch, Dist(now.elbow, eye));
                }
            }
        }
    }
    double forward_before = 0.0, forward_now = 0.0;
    for (const Vec3& d : folded) {
        for (const float pitch : {0.0f, 60.0f, -60.0f}) {
            const B::ModelTarget t =
                GripTarget(Vec3{0.0f, P::HeadHeight, 0.0f} + d, 0.0f, pitch, 0.0f, C::Right);
            forward_before += spine.z - solve(false, t).elbow.z;
            forward_now += spine.z - solve(true, t).elbow.z;
        }
    }
    const double n_folded = static_cast<double>(folded.size() * 3);
    std::printf("  face and overhead: %d hands, elbows in view %d without the near-chest term, %d "
                "more with it; folded arms: elbow %.1f mm in front of Spine2 (%.1f without)\n",
                cells, in_view_before, newly_in_view, forward_now / n_folded * 1000.0,
                forward_before / n_folded * 1000.0);
    EXPECT_EQ(newly_in_view, 0);
    EXPECT_GT(forward_now / n_folded, forward_before / n_folded + 0.012);
}

// ---- Solver -----------------------------------------------------------------------------------

TEST(BodySolver, ZeroWeightsLeaveThePoseUntouched) {
    const Pose ref = Reference();
    Pose pose = ref;
    B::BodySolver solver;
    B::BodyInput in = DefaultInput(pose);
    in.head_yaw = 80.0f * kDeg;
    in.eye_m = in.eye_m + Vec3{0.1f, -0.2f, 0.1f};
    in.target[C::Right] = PresetTarget(P::Presets[2], C::Right, 0.5);
    in.target[C::Left] = PresetTarget(P::Presets[2], C::Left, 0.5);
    for (int i = 0; i < 30; ++i) {
        const B::BodyOutput out = solver.Solve(pose.data(), in, {});
        ASSERT_TRUE(out.ok);
        EXPECT_TRUE(out.written.none());
        EXPECT_EQ(out.arm[0].status, B::ArmStatus::Skipped);
    }
    for (int i = 0; i < C::BoneCount; ++i) {
        EXPECT_TRUE(SameBits(pose[i], ref[i])) << "bone " << i;
    }
}

TEST(BodySolver, HidingWritesOnlyItsBones) {
    for (const B::HideMode mode : {B::HideMode::None, B::HideMode::Head, B::HideMode::ArmsOnly}) {
        const Pose ref = Reference();
        Pose pose = ref;
        B::BodySolver solver;
        B::BodyInput in = DefaultInput(pose);
        in.hide = true;
        B::BodyParams p{};
        p.hide = mode;
        const B::BodyOutput out = solver.Solve(pose.data(), in, p);
        ASSERT_TRUE(out.ok);
        int count = 0;
        const auto bones = B::HiddenBones(mode, count);
        EXPECT_EQ(static_cast<int>(out.written.count()), count);
        EXPECT_EQ(out.hidden, count);
        for (int k = 0; k < count; ++k) {
            EXPECT_TRUE(out.written.test(static_cast<size_t>(bones[static_cast<size_t>(k)].bone)));
        }
        for (int i = 0; i < C::BoneCount; ++i) {
            if (!out.written.test(static_cast<size_t>(i))) {
                EXPECT_TRUE(SameBits(pose[i], ref[i])) << "bone " << i;
            }
        }
        const Pose model = Fk(pose);
        const Pose ref_model = Fk(ref);
        for (int i = 0; i < C::BoneCount; ++i) {
            // Translations ignore the parent's scale: nothing moves.
            EXPECT_LT(Dist(model[i].t, ref_model[i].t), 1e-6f);
        }
        if (mode == B::HideMode::Head) {
            EXPECT_NEAR(model[C::Head].s.x, 1e-4f, 1e-7f);
            EXPECT_NEAR(model[C::R_Hand].s.x, 1.0f, 1e-5f);
        }
        if (mode == B::HideMode::ArmsOnly) {
            EXPECT_NEAR(model[C::Head].s.x, 0.01f, 1e-5f);
            EXPECT_NEAR(model[C::Spine2].s.y, 0.01f, 1e-5f);
            for (const int bone : {C::R_Hand, C::L_Hand, C::R_Weapon, C::R_UpperArm,
                                   C::Support_R_Axilla, C::R_Finger12}) {
                EXPECT_NEAR(model[bone].s.x, 1.0f, 1e-4f) << "bone " << bone;
                EXPECT_NEAR(model[bone].s.z, 1.0f, 1e-4f) << "bone " << bone;
            }
        }
    }
}

TEST(BodySolver, TorsoYawFollowsTheHeadOnUpperTurn) {
    const Pose ref = Reference();
    B::BodySolver solver;
    B::BodyInput in = DefaultInput(ref);
    in.body_weight = 1.0f;
    B::BodyParams p{};
    p.align = false;
    Pose pose{};
    // Head 20 degrees left of the legs: inside the deadzone, the torso stays.
    in.head_yaw = 20.0f * kDeg;
    for (int i = 0; i < 60; ++i) {
        pose = ref;
        const B::BodyOutput out = solver.Solve(pose.data(), in, p);
        ASSERT_TRUE(out.ok);
        EXPECT_EQ(out.torso_delta, 0.0f);
    }
    // 60 degrees: the torso settles at 35, applied on UpperTurn only.
    in.head_yaw = 60.0f * kDeg;
    B::BodyOutput out{};
    for (int i = 0; i < 300; ++i) {
        pose = ref;
        out = solver.Solve(pose.data(), in, p);
    }
    ASSERT_TRUE(out.ok);
    EXPECT_NEAR(out.torso_delta / kDeg, 35.0f, 0.05f);
    EXPECT_TRUE(out.written.test(C::UpperTurn));
    EXPECT_EQ(out.written.count(), size_t{1});
    EXPECT_NEAR(Heading(TorsoForward(pose)) / kDeg, 35.0f, 0.05f);
    // The legs keep their pose.
    const Pose model = Fk(pose), ref_model = Fk(ref);
    EXPECT_LT(Dist(model[C::R_Foot].t, ref_model[C::R_Foot].t), 1e-6f);
    // The character turned the other way: clamped at torso_max.
    in.chr_yaw = -60.0f * kDeg;
    in.head_yaw = 100.0f * kDeg;
    for (int i = 0; i < 600; ++i) {
        pose = ref;
        out = solver.Solve(pose.data(), in, p);
    }
    EXPECT_NEAR(out.torso_delta / kDeg, 75.0f, 1e-3f);
    // Half the body weight: half the yaw.
    in.body_weight = 0.5f;
    pose = ref;
    out = solver.Solve(pose.data(), in, p);
    EXPECT_NEAR(out.torso_delta / kDeg, 37.5f, 1e-3f);
    // torso_follow=0: never.
    p.torso_follow = false;
    pose = ref;
    out = solver.Solve(pose.data(), in, p);
    EXPECT_EQ(out.torso_delta, 0.0f);
    EXPECT_FALSE(out.written.test(C::UpperTurn));
}

TEST(BodySolver, UpperBodyUnderTheEye) {
    // body_posture=shift: the upper body slides against the hips.
    const Pose ref = Reference();
    const Vec3 eye0 = EyeOf(ref);
    B::BodyParams p{};
    p.posture = B::PostureMode::Shift;
    p.torso_follow = false;
    // A reachable offset: the eye lands on the target (the first solve has no filter history).
    {
        B::BodySolver solver;
        B::BodyInput in = DefaultInput(ref);
        in.body_weight = 1.0f;
        in.eye_m = eye0 + Vec3{0.08f, -0.05f, -0.1f};
        Pose pose = ref;
        const B::BodyOutput out = solver.Solve(pose.data(), in, p);
        ASSERT_TRUE(out.ok);
        EXPECT_LT(Dist(EyeOf(pose), in.eye_m), 1e-4f);
        EXPECT_LT(out.eye_residual, 1e-4f);
        EXPECT_TRUE(out.written.test(C::UpperPos));
        // The legs stay, the arms move with the torso.
        const Pose model = Fk(pose), ref_model = Fk(ref);
        EXPECT_LT(Dist(model[C::L_Foot].t, ref_model[C::L_Foot].t), 1e-6f);
        EXPECT_LT(Dist(model[C::R_Hand].t - ref_model[C::R_Hand].t, out.shift), 1e-4f);
    }
    // Clamps: horizontal 0.2 m, vertical -0.6 .. +0.15 m.
    {
        B::BodySolver solver;
        B::BodyInput in = DefaultInput(ref);
        in.body_weight = 1.0f;
        in.eye_m = eye0 + Vec3{0.5f, -1.0f, 0.0f};
        Pose pose = ref;
        B::BodyOutput out = solver.Solve(pose.data(), in, p);
        EXPECT_NEAR(std::hypot(out.shift.x, out.shift.z), 0.2f, 1e-5f);
        EXPECT_NEAR(out.shift.y, -0.6f, 1e-5f);
        EXPECT_NEAR(out.eye_residual, std::hypot(0.3f, 0.4f), 1e-4f);
        in.eye_m = eye0 + Vec3{0.0f, 0.5f, 0.0f};
        solver.Reset();
        pose = ref;
        out = solver.Solve(pose.data(), in, p);
        EXPECT_NEAR(out.shift.y, 0.15f, 1e-5f);
    }
    // Low-pass: a step of the eye is followed with the 50 ms time constant.
    {
        B::BodySolver solver;
        B::BodyInput in = DefaultInput(ref);
        in.body_weight = 1.0f;
        Pose pose = ref;
        solver.Solve(pose.data(), in, p);
        in.eye_m = eye0 + Vec3{0.1f, 0.0f, 0.0f};
        in.dt = 0.05f;
        pose = ref;
        const B::BodyOutput out = solver.Solve(pose.data(), in, p);
        EXPECT_NEAR(out.shift.x, 0.1f * (1.0f - std::exp(-1.0f)), 1e-4f);
    }
    // body_back: the eye ends up behind the target along the torso's forward (-Z at yaw 0).
    {
        B::BodySolver solver;
        B::BodyInput in = DefaultInput(ref);
        in.body_weight = 1.0f;
        p.back = 0.06f;
        Pose pose = ref;
        solver.Solve(pose.data(), in, p);
        EXPECT_LT(Dist(EyeOf(pose), in.eye_m + Vec3{0.0f, 0.0f, 0.06f}), 1e-4f);
        p.back = 0.0f;
    }
    // body_align=0 or weight 0: no shift.
    {
        B::BodySolver solver;
        B::BodyInput in = DefaultInput(ref);
        in.body_weight = 1.0f;
        in.eye_m = eye0 + Vec3{0.1f, 0.0f, 0.0f};
        B::BodyParams off = p;
        off.align = false;
        Pose pose = ref;
        const B::BodyOutput out = solver.Solve(pose.data(), in, off);
        EXPECT_FALSE(out.written.test(C::UpperPos));
        EXPECT_NEAR(out.eye_residual, 0.1f, 1e-4f);
    }
}

TEST(BodySolver, TorsoYawThenShiftKeepTheEye) {
    // With the torso turned 35 degrees the eye (in front of the yaw axis) swings sideways; the
    // shift brings it back onto the target.
    const Pose ref = Reference();
    B::BodySolver solver;
    B::BodyInput in = DefaultInput(ref);
    in.body_weight = 1.0f;
    in.head_yaw = 60.0f * kDeg;
    in.dt = 0.0f;
    Pose pose{};
    B::BodyOutput out{};
    solver.torso.Reset(35.0f * kDeg);
    pose = ref;
    B::BodyParams shift{};
    shift.posture = B::PostureMode::Shift;
    out = solver.Solve(pose.data(), in, shift);
    ASSERT_TRUE(out.ok);
    EXPECT_NEAR(out.torso_delta / kDeg, 35.0f, 1e-3f);
    EXPECT_GT(Dist(out.eye, in.eye_m), 0.02f);
    EXPECT_LT(Dist(EyeOf(pose), in.eye_m), 1e-4f);
    // The lean (the default) brings it back by leaning the trunk instead: within 3 mm, nothing
    // slides.
    B::BodySolver lean_solver;
    lean_solver.torso.Reset(35.0f * kDeg);
    pose = ref;
    out = lean_solver.Solve(pose.data(), in, {});
    ASSERT_TRUE(out.ok);
    EXPECT_NEAR(out.torso_delta / kDeg, 35.0f, 1e-3f);
    EXPECT_LT(Dist(EyeOf(pose), in.eye_m), 0.003f);
    EXPECT_FALSE(out.written.test(C::UpperPos));
}

// body_posture=lean: the trunk leans from the waist to put the eye over the camera's; the trunk
// keeps its length, the hips and legs stay, the chest never slides over the hips.
TEST(BodySolver, LeanKeepsTheTrunkWhole) {
    const Pose ref = Reference();
    const Pose ref_model = Fk(ref);
    const Vec3 eye0 = EyeOf(ref);
    B::BodyParams p{};
    p.torso_follow = false;
    const float trunk0 = Dist(ref_model[C::Neck].t, ref_model[C::Pelvis].t);
    struct Case {
        Vec3 offset;  // the goal from the hunter's eye (model axes: forward -Z, right -X)
        float within; // m: how near the eye lands
    };
    // Forward 12 cm (a run's lean taken back, a lean of yours), back 2 cm (the bind pose already
    // leans back 2.4 of the 20 degrees allowed), right 8 cm, left 10
    // cm, forward-left; heights within the 8 cm deadband are left alone.
    for (const Case& c : {Case{{0.0f, 0.0f, -0.12f}, 0.004f}, Case{{0.0f, 0.0f, 0.02f}, 0.003f},
                          Case{{-0.08f, 0.0f, 0.0f}, 0.003f}, Case{{0.10f, 0.0f, 0.0f}, 0.004f},
                          Case{{0.06f, -0.05f, -0.08f}, 0.004f}}) {
        B::BodySolver solver;
        B::BodyInput in = DefaultInput(ref);
        in.body_weight = 1.0f;
        in.eye_m = eye0 + c.offset;
        Pose pose = ref;
        const B::BodyOutput out = solver.Solve(pose.data(), in, p);
        ASSERT_TRUE(out.ok);
        const Pose model = Fk(pose);
        const Vec3 eye = EyeOf(pose);
        // Horizontally on the goal; the height left as it was (within the deadband) but for the
        // lean's own drop (1 - cos).
        EXPECT_LT(std::hypot(eye.x - in.eye_m.x, eye.z - in.eye_m.z), c.within)
            << c.offset.x << "," << c.offset.z;
        std::printf("lean case (%.2f,%.2f,%.2f): eye off goal %.4f m horizontally, height %+.4f m, "
                    "lean %.2f/%.2f deg\n",
                    c.offset.x, c.offset.y, c.offset.z,
                    std::hypot(eye.x - in.eye_m.x, eye.z - in.eye_m.z), eye.y - eye0.y,
                    out.lean_fwd / kDeg, out.lean_right / kDeg);
        EXPECT_NEAR(eye.y, eye0.y, 0.03f);
        EXPECT_FALSE(out.written.test(C::UpperPos));
        EXPECT_EQ(out.shift.y, 0.0f);
        // The trunk keeps its length, the hips and feet do not move.
        EXPECT_NEAR(Dist(model[C::Neck].t, model[C::Pelvis].t), trunk0, 0.002f);
        EXPECT_LT(Dist(model[C::Pelvis].t, ref_model[C::Pelvis].t), 1e-6f);
        EXPECT_LT(Dist(model[C::L_Foot].t, ref_model[C::L_Foot].t), 1e-6f);
        // The waist bends: the bottom of the trunk (Spine) stays at the hips, the chest goes
        // with the lean.
        EXPECT_LT(Dist(model[C::Spine].t, ref_model[C::Spine].t), 0.002f);
        EXPECT_GT(B::Dot(model[C::Spine2].t - ref_model[C::Spine2].t, c.offset), 0.0f);
        // out.lean_eye is the eye's travel.
        EXPECT_LT(Dist(out.lean_eye, eye - eye0), 1e-4f);
    }
}

TEST(BodySolver, LeanClampsAndHeightBeyondTheDeadband) {
    const Pose ref = Reference();
    const Vec3 eye0 = EyeOf(ref);
    B::BodyParams p{};
    p.torso_follow = false;
    // Far beyond the limits: the trunk (waist -> neck) ends at 35 degrees forward, 20 back, 20 to
    // the side from vertical, the rest stays as the residual.
    {
        const Pose m = Fk(ref);
        const Vec3 trunk = m[C::Neck].t - m[C::UpperTurn].t;
        const float anim_fwd = std::atan2(-trunk.z, trunk.y) / kDeg;
        const float anim_right = std::atan2(-trunk.x, trunk.y) / kDeg;
        B::BodySolver solver;
        B::BodyInput in = DefaultInput(ref);
        in.body_weight = 1.0f;
        in.eye_m = eye0 + Vec3{0.0f, 0.0f, -0.6f};
        Pose pose = ref;
        const B::BodyOutput out = solver.Solve(pose.data(), in, p);
        EXPECT_NEAR(out.lean_fwd / kDeg, 35.0f - anim_fwd, 1e-3f);
        EXPECT_GT(out.eye_residual, 0.2f);
        in.eye_m = eye0 + Vec3{0.0f, 0.0f, 0.6f}; // backward
        solver.Reset();
        pose = ref;
        const float back = solver.Solve(pose.data(), in, p).lean_fwd / kDeg;
        EXPECT_NEAR(back, -20.0f - anim_fwd, 1e-3f);
        in.eye_m = eye0 + Vec3{-0.6f, 0.0f, 0.0f}; // to his right
        solver.Reset();
        pose = ref;
        const float side = solver.Solve(pose.data(), in, p).lean_right / kDeg;
        EXPECT_NEAR(side, 20.0f - anim_right, 1e-3f);
    }
    // An animation that already leans past a limit (a sprint's 40 degrees) keeps its lean when the
    // eye is where it is; leaning it back is allowed, further forward is not.
    {
        // 40 degrees forward about model right (-X) at the waist, in model space.
        Pose bent = ref;
        const Quat parent = Fk(ref)[C::UpperPos].q;
        bent[C::UpperTurn].q =
            B::Normalize(B::Conjugate(parent) * B::AxisAngle({-1.0f, 0.0f, 0.0f}, 40.0f * kDeg) *
                         parent * bent[C::UpperTurn].q);
        {
            const Pose m = Fk(bent);
            const Vec3 trunk = m[C::Neck].t - m[C::UpperTurn].t;
            EXPECT_NEAR(std::atan2(-trunk.z, trunk.y) / kDeg, 40.0f, 3.0f);
        }
        B::BodySolver solver;
        B::BodyInput in = DefaultInput(bent);
        in.body_weight = 1.0f;
        Pose pose = bent;
        B::BodyOutput out = solver.Solve(pose.data(), in, p);
        EXPECT_NEAR(out.lean_fwd, 0.0f, 1e-4f);
        in.eye_m = EyeOf(bent) + Vec3{0.0f, 0.0f, -0.1f};
        solver.Reset();
        pose = bent;
        out = solver.Solve(pose.data(), in, p);
        EXPECT_NEAR(out.lean_fwd, 0.0f, 1e-4f);
        in.eye_m = EyeOf(bent) + Vec3{0.0f, 0.0f, 0.15f};
        solver.Reset();
        pose = bent;
        out = solver.Solve(pose.data(), in, p);
        EXPECT_LT(out.lean_fwd / kDeg, -10.0f);
    }
    // Height: within the deadband nothing; beyond it the rest moves the upper body down (a duck);
    // a camera above the eye leaves the trunk as it is. Without the duck posture (body_duck_lean_deg
    // 35) the deadband holds at every depth; with it (the default) it fades out over 0.10 .. 0.35 m
    // of drop (DuckWeight), so a duck follows the camera all the way down to align_down.
    B::BodyParams flat = p;
    flat.lean_fwd_low_max = flat.lean_fwd_max;
    for (const bool duck_posture : {false, true}) {
        const B::BodyParams& q = duck_posture ? p : flat;
        for (const float dy : {-0.05f, 0.07f, -0.2f, 0.12f, -0.5f, -0.9f, 0.3f}) {
            B::BodySolver solver;
            B::BodyInput in = DefaultInput(ref);
            in.body_weight = 1.0f;
            in.eye_m = eye0 + Vec3{0.0f, dy, 0.0f};
            Pose pose = ref;
            const B::BodyOutput out = solver.Solve(pose.data(), in, q);
            const float band = 0.08f * (1.0f - B::DuckWeight(q, -dy));
            const float rest = dy > band ? dy - band : (dy < -band ? dy + band : 0.0f);
            // Never up (lean_up_max 0: the trunk is not stretched to a camera above the eye).
            const float want = std::clamp(rest, -0.6f, 0.0f);
            EXPECT_NEAR(out.shift.y, want, 1e-5f) << dy;
            EXPECT_EQ(out.written.test(C::UpperPos), want != 0.0f) << dy;
            EXPECT_NEAR(EyeOf(pose).y, eye0.y + want, 1e-4f) << dy;
            if (!duck_posture && dy > -0.85f && dy < -0.08f) {
                EXPECT_NEAR(EyeOf(pose).y - in.eye_m.y, 0.08f, 1e-4f) << dy; // 8 cm above
            }
            if (duck_posture && dy == -0.5f) {
                EXPECT_NEAR(EyeOf(pose).y, in.eye_m.y, 1e-4f) << dy; // at the camera's eye
            }
        }
    }
    // The height follows slowly (vertical_tau 0.25 s): a step of 0.2 m (0.12 m past the
    // deadband) after one 50 ms frame has moved 1 - exp(-0.2) of the way.
    {
        B::BodySolver solver;
        B::BodyInput in = DefaultInput(ref);
        in.body_weight = 1.0f;
        Pose pose = ref;
        solver.Solve(pose.data(), in, flat);
        in.eye_m = eye0 + Vec3{0.0f, -0.2f, 0.0f};
        in.dt = 0.05f;
        pose = ref;
        const B::BodyOutput out = solver.Solve(pose.data(), in, flat);
        EXPECT_NEAR(out.shift.y, -0.12f * (1.0f - std::exp(-0.2f)), 1e-4f);
    }
    // Weight 0 or body_align=0: nothing.
    {
        B::BodySolver solver;
        B::BodyInput in = DefaultInput(ref);
        in.body_weight = 0.0f;
        in.eye_m = eye0 + Vec3{0.0f, -0.3f, -0.1f};
        Pose pose = ref;
        B::BodyOutput out = solver.Solve(pose.data(), in, p);
        EXPECT_FALSE(out.written.test(C::UpperPos));
        EXPECT_FALSE(out.written.test(C::Spine));
        in.body_weight = 1.0f;
        B::BodyParams off = p;
        off.align = false;
        solver.Reset();
        pose = ref;
        out = solver.Solve(pose.data(), in, off);
        EXPECT_FALSE(out.written.test(C::UpperPos));
        EXPECT_FALSE(out.written.test(C::Spine));
    }
}

// The camera against his eye after the alignment (BodyOutput cam_ahead / cam_above, the body
// cutaway's boost, core/vr/body_cut_boost.h): a head further back than his trunk leans back (5
// degrees) is behind his eye; a head drop within the 8 cm deadband is below it.
TEST(BodySolver, CameraAgainstHisEyeForTheCutawayBoost) {
    const Pose ref = Reference();
    const Vec3 eye0 = EyeOf(ref);
    B::BodyParams p{};
    p.torso_follow = false;
    const auto solve = [&](Vec3 offset) {
        B::BodySolver solver;
        B::BodyInput in = DefaultInput(ref);
        in.body_weight = 1.0f;
        in.eye_m = eye0 + offset;
        Pose pose = ref;
        return solver.Solve(pose.data(), in, p);
    };
    const B::BodyOutput level = solve({0.0f, 0.0f, 0.0f});
    EXPECT_NEAR(level.cam_ahead, 0.0f, 1e-3f);
    EXPECT_NEAR(level.cam_above, 0.0f, 1e-3f);
    // The new 20-degree limit follows a 20 cm lean; a larger lean still leaves a
    // residual that activates the collar cutaway. Model forward is -z.
    EXPECT_NEAR(solve({0.0f, 0.0f, 0.20f}).cam_ahead, 0.0f, 0.01f);
    const B::BodyOutput back = solve({0.0f, 0.0f, 0.40f});
    EXPECT_LT(back.cam_ahead, -0.08f);
    EXPECT_NEAR(back.cam_above, 0.0f, 0.03f);
    const B::BodyOutput low = solve({0.0f, -0.06f, 0.0f}); // a small drop: the deadband
    EXPECT_NEAR(low.cam_above, -0.06f, 2e-3f);
    EXPECT_NEAR(low.cam_ahead, 0.0f, 2e-3f);
    const B::BodyOutput ahead = solve({0.0f, 0.0f, -0.10f}); // leaning in: he follows
    EXPECT_GT(ahead.cam_ahead, -0.01f);
    // The sizes match the residual.
    EXPECT_NEAR(std::hypot(back.cam_ahead, back.cam_above), back.eye_residual, 0.02f);
}

// A duck bends at the hips (2026-10-08): the forward limit is lean_fwd_max (35 degrees) while the
// camera's eye is level with the hunter's, above it or less than lean_low_from (0.10 m) below,
// lean_fwd_low_max (70) from lean_low_full (0.35 m) below, smooth in between; lean_fwd_low_max at
// or under lean_fwd_max is the old limit everywhere.
TEST(BodySolver, LeanForwardLimitRisesInADuck) {
    B::BodyParams p{};
    for (const float drop : {-0.5f, -0.1f, 0.0f, 0.05f, 0.10f}) {
        EXPECT_NEAR(B::LeanForwardLimit(p, drop) / kDeg, 35.0f, 1e-4f) << drop;
    }
    for (const float drop : {0.35f, 0.5f, 1.0f}) {
        EXPECT_NEAR(B::LeanForwardLimit(p, drop) / kDeg, 70.0f, 1e-4f) << drop;
    }
    EXPECT_NEAR(B::LeanForwardLimit(p, 0.225f) / kDeg, 52.5f, 1e-3f); // the midpoint
    float prev = B::LeanForwardLimit(p, 0.0f);
    for (float drop = 0.001f; drop < 0.5f; drop += 0.001f) {
        const float now = B::LeanForwardLimit(p, drop);
        EXPECT_GE(now, prev - 1e-7f) << drop;          // never down as the duck deepens
        EXPECT_LT((now - prev) / kDeg, 0.25f) << drop; // < 0.25 degrees a millimetre: no step
        prev = now;
    }
    B::BodyParams old = p;
    old.lean_fwd_low_max = old.lean_fwd_max;
    B::BodyParams lower = p;
    lower.lean_fwd_low_max = 20.0f * kDeg;
    for (const float drop : {-0.2f, 0.0f, 0.2f, 0.4f, 0.9f}) {
        EXPECT_EQ(B::LeanForwardLimit(old, drop), old.lean_fwd_max) << drop;
        EXPECT_EQ(B::LeanForwardLimit(lower, drop), lower.lean_fwd_max) << drop;
    }
    EXPECT_EQ(B::LeanForwardLimit(p, std::numeric_limits<float>::quiet_NaN()), p.lean_fwd_max);
    // The duck weight (it also fades the vertical deadband): 0 standing, 1 from 0.35 m, 0
    // everywhere without the duck posture.
    EXPECT_EQ(B::DuckWeight(p, 0.05f), 0.0f);
    EXPECT_EQ(B::DuckWeight(p, -0.4f), 0.0f);
    EXPECT_NEAR(B::DuckWeight(p, 0.225f), 0.5f, 1e-5f);
    EXPECT_EQ(B::DuckWeight(p, 0.6f), 1.0f);
    for (const float drop : {-0.2f, 0.0f, 0.2f, 0.4f, 0.9f}) {
        EXPECT_EQ(B::DuckWeight(old, drop), 0.0f) << drop;
    }
}

namespace {

// A grip at tracking `p` for a head at tracking `head` whose camera eye is model `eye` (the
// hands' mapping: the camera's eye plus the hand's offset from your real head).
B::ModelTarget FollowTarget(Vec3 p, Vec3 head, Vec3 eye, float pitch_deg, int side) {
    B::ChrFrame frame{};
    const float yaw = 0.6f;
    const float s = std::sin(yaw), c = std::cos(yaw);
    B::ChrFrameFromRows({-c, 0.0f, -s, 5.0f, 0.0f, 1.0f, 0.0f, 1.0f, s, 0.0f, -c, -7.0f}, frame);
    B::Anchor a{};
    a.valid = true;
    a.eye_world = B::ToWorld(frame, eye);
    a.eye_rel = a.eye_world - frame.t;
    a.head_k = head;
    a.base_yaw = yaw;
    a.world_scale = 1.0f;
    B::TrackedHand hand{};
    hand.position_valid = hand.position_tracked = hand.orientation_valid = true;
    hand.p = p;
    hand.q = ToQuat(P::GripRotation(0.0f, pitch_deg, 0.0f));
    return B::HandToModel(a, frame, hand, B::DefaultGrip(side, 0.0f));
}

} // namespace

// The duck scenario on the reference pose (the mock's guard hands following the head, as in the
// duck runs): standing poses (level, a 0.25 / 0.37 m forward lean, 0.30 m sideways) are solved bit
// for bit as before (nothing of the duck posture moves standing, and arms that reach never use the
// shoulder's catch-up); a straight duck follows the camera all the way down (the 8 cm deadband
// faded out); a forward-leaning duck (the user's playtest: 0.56 m down, 0.37 m forward) and a
// deeper one (0.65 / 0.45) put both hands on their controllers, which the old 35-degree trunk and
// fixed shoulders did not (5-75 mm short), with the eye nearer the camera's.
TEST(BodySolver, ADuckLeansFurtherAndTheHandsReach) {
    const Pose ref = Reference();
    const Vec3 eye0 = EyeOf(ref);
    B::BodyParams now{};
    now.torso_follow = false;
    B::BodyParams old = now;
    old.lean_fwd_low_max = old.lean_fwd_max;
    old.shoulder_reach = 0.0f;
    struct Case {
        const char* name;
        Vec3 d; // the head's offset in tracking space (x right, y up, -z forward)
        int kind; // 0 standing, 1 straight duck, 2 forward duck
    };
    const Case cases[] = {{"stand", {0.0f, 0.0f, 0.0f}, 0},
                          {"forward 0.25", {0.0f, 0.0f, -0.25f}, 0},
                          {"forward 0.37", {0.0f, 0.0f, -0.37f}, 0},
                          {"side 0.30", {0.30f, 0.0f, 0.0f}, 0},
                          {"straight duck 0.50", {0.0f, -0.50f, 0.0f}, 1},
                          {"straight duck 0.65", {0.0f, -0.65f, 0.0f}, 1},
                          {"playtest duck", {0.0f, -0.56f, -0.37f}, 2},
                          {"deep forward duck", {0.0f, -0.65f, -0.45f}, 2}};
    for (const Case& k : cases) {
        const Vec3 head{k.d.x, P::HeadHeight + k.d.y, k.d.z};
        // Model right is -X (tracking x flips), forward -Z in both.
        const Vec3 eye = eye0 + Vec3{-k.d.x, k.d.y, k.d.z};
        B::BodyInput in{};
        in.dt = 0.0f;
        in.eye_m = eye;
        in.body_weight = 1.0f;
        in.arm_weight = {1.0f, 1.0f};
        in.target[C::Right] =
            FollowTarget({0.18f + k.d.x, 1.25f + k.d.y, -0.40f + k.d.z}, head, eye, 45.0f, C::Right);
        in.target[C::Left] =
            FollowTarget({-0.22f + k.d.x, 1.10f + k.d.y, -0.30f + k.d.z}, head, eye, 0.0f, C::Left);
        Pose a = ref, b = ref;
        B::BodySolver sa, sb;
        const B::BodyOutput oa = sa.Solve(a.data(), in, old);
        const B::BodyOutput ob = sb.Solve(b.data(), in, now);
        ASSERT_TRUE(oa.ok && ob.ok) << k.name;
        std::printf("%-18s before: lean %5.1f deg eye res %4.1f cm R %5.1f L %5.1f mm | after: "
                    "lean %5.1f deg (limit %4.1f) eye res %4.1f cm R %5.1f L %5.1f mm\n",
                    k.name, oa.lean_fwd / kDeg, oa.eye_residual * 100.0f,
                    oa.arm[C::Right].error * 1000.0f, oa.arm[C::Left].error * 1000.0f,
                    ob.lean_fwd / kDeg, ob.lean_fwd_limit / kDeg, ob.eye_residual * 100.0f,
                    ob.arm[C::Right].error * 1000.0f, ob.arm[C::Left].error * 1000.0f);
        if (k.kind == 0) {
            for (int i = 0; i < C::BoneCount; ++i) {
                ASSERT_TRUE(SameBits(a[i], b[i])) << k.name << " bone " << i;
            }
            EXPECT_TRUE(ob.arm[C::Right].reached && ob.arm[C::Left].reached) << k.name;
            continue;
        }
        EXPECT_NEAR(ob.duck, 1.0f, 1e-6f) << k.name;
        if (k.kind == 1) {
            // Straight down: no lean; the body follows past the old deadband (8 cm short before)
            // to the camera or the 0.60 m align_down.
            EXPECT_NEAR(oa.eye_residual, 0.08f, 1e-3f) << k.name;
            EXPECT_NEAR(ob.shift.y, std::max(k.d.y, -0.60f), 1e-3f) << k.name;
            EXPECT_NEAR(ob.eye_residual, std::max(-0.60f - k.d.y, 0.0f), 1e-3f) << k.name;
            EXPECT_TRUE(ob.arm[C::Right].reached && ob.arm[C::Left].reached) << k.name;
            continue;
        }
        EXPECT_GT(std::max(oa.arm[C::Right].error, oa.arm[C::Left].error), 0.005f) << k.name;
        EXPECT_LT(ob.arm[C::Right].error, 0.002f) << k.name;
        EXPECT_LT(ob.arm[C::Left].error, 0.002f) << k.name;
        EXPECT_LE(ob.eye_residual, oa.eye_residual + 1e-4f) << k.name;
        EXPECT_GT(ob.lean_fwd, oa.lean_fwd) << k.name;
        EXPECT_LE(ob.lean_fwd, ob.lean_fwd_limit + 1e-4f) << k.name;
    }
}

// The duck 30 cm deeper than the sphere (positional_limit_down_m 0.80, the evening of 2026-10-08):
// the camera's eye goes down to 0.80 m (a deeper head is clamped there: the 0.90 m straight duck,
// the 0.85 m dip), and bloodborne_body.cpp raises align_down to that depth. Before (align_down
// 0.60) a straight duck past 0.60 m left his eyes and shoulders that much above the camera's; now
// he comes all the way down and both hands stay on their controllers (straight, the deep forward
// duck 0.75 down / 0.30 forward, and the playtest duck at the new depth's posture).
TEST(BodySolver, ADuckToTheDeeperRoomIsFollowed) {
    const Pose ref = Reference();
    const Vec3 eye0 = EyeOf(ref);
    B::BodyParams old{};
    old.torso_follow = false;
    B::BodyParams now = old;
    now.align_down = 0.80f; // the camera's deepest drop (HeadRoom::MaxDrop with the defaults)
    constexpr float Room = 0.80f;
    struct Case {
        const char* name;
        Vec3 d; // the real head's offset in tracking space (x right, y up, -z forward)
        bool straight;
    };
    const Case cases[] = {{"straight duck 0.70", {0.0f, -0.70f, 0.0f}, true},
                          {"straight duck 0.80", {0.0f, -0.80f, 0.0f}, true},
                          {"straight duck 0.90", {0.0f, -0.90f, 0.0f}, true},
                          {"dip 0.85", {0.0f, -0.85f, 0.0f}, true},
                          {"playtest duck", {0.0f, -0.56f, -0.37f}, false},
                          {"deep forward duck", {0.0f, -0.75f, -0.30f}, false},
                          {"deepest forward duck", {0.0f, -0.80f, -0.30f}, false}};
    for (const Case& k : cases) {
        const Vec3 head{k.d.x, P::HeadHeight + k.d.y, k.d.z};
        // The camera stops at the room's floor; the hands keep their offset from the real head.
        const float cam_y = std::max(k.d.y, -Room);
        const Vec3 eye = eye0 + Vec3{-k.d.x, cam_y, k.d.z};
        B::BodyInput in{};
        in.dt = 0.0f;
        in.eye_m = eye;
        in.body_weight = 1.0f;
        in.arm_weight = {1.0f, 1.0f};
        in.target[C::Right] =
            FollowTarget({0.18f + k.d.x, 1.25f + k.d.y, -0.40f + k.d.z}, head, eye, 45.0f, C::Right);
        in.target[C::Left] =
            FollowTarget({-0.22f + k.d.x, 1.10f + k.d.y, -0.30f + k.d.z}, head, eye, 0.0f, C::Left);
        Pose a = ref, b = ref;
        B::BodySolver sa, sb;
        const B::BodyOutput oa = sa.Solve(a.data(), in, old);
        const B::BodyOutput ob = sb.Solve(b.data(), in, now);
        ASSERT_TRUE(oa.ok && ob.ok) << k.name;
        std::printf("%-20s align 0.60: lean %5.1f deg shift %+6.3f eye res %4.1f cm R %5.1f L "
                    "%5.1f mm | 0.80: lean %5.1f deg shift %+6.3f eye res %4.1f cm R %5.1f L %5.1f "
                    "mm\n",
                    k.name, oa.lean_fwd / kDeg, oa.shift.y, oa.eye_residual * 100.0f,
                    oa.arm[C::Right].error * 1000.0f, oa.arm[C::Left].error * 1000.0f,
                    ob.lean_fwd / kDeg, ob.shift.y, ob.eye_residual * 100.0f,
                    ob.arm[C::Right].error * 1000.0f, ob.arm[C::Left].error * 1000.0f);
        EXPECT_NEAR(ob.duck, 1.0f, 1e-6f) << k.name;
        EXPECT_LT(ob.arm[C::Right].error, 0.002f) << k.name;
        EXPECT_LT(ob.arm[C::Left].error, 0.002f) << k.name;
        EXPECT_LE(ob.eye_residual, oa.eye_residual + 1e-4f) << k.name;
        EXPECT_GE(ob.shift.y, -0.80f - 1e-4f) << k.name; // never deeper than the room
        if (k.straight) {
            // Straight down: no lean; the body follows the camera all the way (to the room's
            // 0.80 m), where align_down 0.60 stopped it.
            EXPECT_NEAR(ob.shift.y, cam_y, 1e-3f) << k.name;
            EXPECT_LT(ob.eye_residual, 1e-3f) << k.name;
            EXPECT_NEAR(oa.eye_residual, -0.60f - cam_y, 1e-3f) << k.name;
        } else {
            EXPECT_LE(ob.lean_fwd, ob.lean_fwd_limit + 1e-4f) << k.name;
            EXPECT_LT(ob.eye_residual, 0.05f) << k.name;
        }
    }
}

TEST(BodySolver, PresetsThroughTheMapping) {
    // Every mock preset, both arms through the solver with the torso and the shift on: reachable
    // presets within 1 mm, out_of_reach detached at 10% stretch, everything finite.
    const std::vector<std::string_view> reachable = {"rest",        "still",        "guard",
                                                     "blade_level", "wrist_inject", "lost"};
    const Pose ref = Reference();
    for (const P::Preset& preset : P::Presets) {
        B::BodySolver solver;
        B::BodyInput in = DefaultInput(ref);
        in.body_weight = 1.0f;
        in.eye_m = {0.0f, P::HeadHeight, -0.1f};
        in.arm_weight = {1.0f, 1.0f};
        in.hide = true;
        for (const int side : {C::Left, C::Right}) {
            in.target[static_cast<size_t>(side)] = PresetTarget(preset, side, 0.5);
        }
        Pose pose{};
        B::BodyOutput out{};
        for (int i = 0; i < 20; ++i) {
            pose = ref;
            out = solver.Solve(pose.data(), in, {});
        }
        ASSERT_TRUE(out.ok) << preset.name;
        for (const int side : {C::Left, C::Right}) {
            const B::ArmResult& r = out.arm[static_cast<size_t>(side)];
            std::printf("  %-13s %s %-8s err %7.1f mm stretch %.3f flex %5.1f pole turn %5.1f "
                        "detached %d\n",
                        std::string{preset.name}.c_str(), side == C::Right ? "R" : "L",
                        B::ArmStatusName(r.status), r.error * 1000.0f, r.stretch, r.flex / kDeg,
                        r.poleTurn / kDeg, B::Detached(r) ? 1 : 0);
            EXPECT_EQ(r.status, B::ArmStatus::Solved);
            EXPECT_LE(r.stretch, 1.1001f);
            const bool expect_reached =
                std::find(reachable.begin(), reachable.end(), preset.name) != reachable.end() ||
                side == C::Left;
            if (expect_reached) {
                EXPECT_LT(r.error, 0.001f) << preset.name;
                // The rendered effector (FK of the written pose) is the target.
                const auto eff = B::ModelOf(pose.data(), C::Parents.data(), C::BoneCount,
                                            side == C::Right ? C::R_Weapon : C::L_Weapon);
                const B::ModelTarget& t = *in.target[static_cast<size_t>(side)];
                EXPECT_LT(Dist(eff->t, t.p), 0.001f);
                EXPECT_LT(B::AngleBetween(eff->q, t.q) / kDeg, 0.1f);
            }
            if (preset.name == "out_of_reach" && side == C::Right) {
                EXPECT_TRUE(B::Detached(r));
                EXPECT_NEAR(r.stretch, 1.1f, 1e-3f);
            }
        }
        for (const QsTransform& x : pose) {
            EXPECT_TRUE(B::IsFinite(x));
        }
    }
}

TEST(BodySolver, NoElbowFlipsAcrossPresetChanges) {
    // The right hand moves between presets (minimum jerk, 0.4 s) at 60 Hz with the torso and shift
    // on: the elbow side never jumps and no arm bone turns more than it must.
    const Pose ref = Reference();
    B::BodySolver solver;
    B::BodyInput in = DefaultInput(ref);
    in.body_weight = 1.0f;
    in.eye_m = {0.0f, P::HeadHeight, -0.1f};
    in.arm_weight = {1.0f, 1.0f};
    const std::vector<std::string_view> order = {
        "rest",  "guard", "high",          "over_shoulder", "behind_head", "cross",
        "still", "rest",  "blade_level",   "reach_fwd",     "cross",       "high",
        "guard", "rest",  "over_shoulder", "rest"};
    std::vector<B::ModelTarget> stops;
    for (const std::string_view name : order) {
        stops.push_back(PresetTarget(*P::Find(name), C::Right, 0.5));
    }
    in.target[C::Left] = PresetTarget(P::Presets[0], C::Left, 0.5);
    const float dt = 1.0f / 60.0f;
    float max_side_step = 0.0f, max_elbow_step = 0.0f, max_target_step = 0.0f;
    Vec3 prev_side{}, prev_elbow{}, prev_target{};
    bool have_prev = false;
    int failures = 0;
    for (size_t k = 0; k + 1 < stops.size(); ++k) {
        for (int f = 0; f <= 48; ++f) {
            const float s =
                static_cast<float>(P::MinJerk(std::min(1.0, static_cast<double>(f) * dt / 0.4)));
            B::ModelTarget t{};
            t.p = B::Lerp(stops[k].p, stops[k + 1].p, s);
            t.q = B::Slerp(stops[k].q, stops[k + 1].q, s);
            in.target[C::Right] = t;
            in.dt = dt;
            Pose pose = ref;
            const B::BodyOutput out = solver.Solve(pose.data(), in, {});
            const B::ArmResult& r = out.arm[C::Right];
            if (!out.ok || r.status != B::ArmStatus::Solved) {
                ++failures;
                continue;
            }
            if (have_prev) {
                max_side_step = std::max(max_side_step, AngleDeg(r.bendDir, prev_side));
                max_elbow_step = std::max(max_elbow_step, Dist(r.elbow, prev_elbow));
                max_target_step = std::max(max_target_step, Dist(t.p, prev_target));
            }
            prev_side = r.bendDir;
            prev_elbow = r.elbow;
            prev_target = t.p;
            have_prev = true;
        }
    }
    std::printf("  preset changes: target step %.1f mm, elbow step %.1f mm, elbow side step %.1f "
                "deg per frame\n",
                max_target_step * 1000.0f, max_elbow_step * 1000.0f, max_side_step);
    EXPECT_EQ(failures, 0);
    // The fastest change (rest -> over_shoulder, the hand passing 15 cm from the shoulder at
    // 4.7 m/s with the elbow folded at its limit) swings the elbow round quickly; a flip would
    // turn its side by over 90 degrees in a frame and move it more than the two-bone geometry
    // allows (~3x the hand near full extension, arm_ik's own bound).
    EXPECT_LT(max_side_step, 45.0f);
    EXPECT_LT(max_elbow_step, 4.0f * max_target_step);
}

TEST(BodySolver, SmoothPathIsContinuous) {
    // The game's pose rate (60 Hz frames), 60 s: the right controller wanders through a guard
    // region (hand up to ~0.6 m/s; blade yaw +-40, pitch -20 .. 70, roll +-70 degrees, up to
    // ~110 deg/s), the left one too, the torso following a head that looks around, the eye moving
    // sideways. No arm bone, helper or the torso may turn more than the plan's 10 degrees in a
    // frame (outside a blend), and the elbow side never jumps.
    const Pose ref = Reference();
    B::BodySolver solver;
    B::BodyInput in = DefaultInput(ref);
    in.body_weight = 1.0f;
    in.arm_weight = {1.0f, 1.0f};
    const float dt = 1.0f / 60.0f;
    float max_bone_step = 0.0f, max_side_step = 0.0f;
    int worst_bone = -1;
    Pose prev{};
    std::array<Vec3, 2> prev_side{};
    int failures = 0;
    for (int frame = 0; frame < 60 * 60; ++frame) {
        const float t = static_cast<float>(frame) * dt;
        in.head_yaw = 50.0f * kDeg * std::sin(0.3f * t);
        in.eye_m = Vec3{0.03f * std::sin(0.7f * t), 1.6f, -0.1f};
        for (const int side : {C::Left, C::Right}) {
            const float k = side == C::Right ? 1.0f : -1.0f;
            const float u = t + (side == C::Right ? 0.0f : 2.0f);
            const Vec3 p{k * (0.18f + 0.18f * std::sin(0.9f * u)),
                         1.20f + 0.25f * std::sin(1.3f * u + 1.0f),
                         -0.38f + 0.12f * std::sin(0.7f * u + 2.0f)};
            in.target[static_cast<size_t>(side)] =
                GripTarget(p, k * 40.0f * std::sin(0.5f * u), 25.0f + 45.0f * std::sin(0.8f * u),
                           k * 70.0f * std::sin(0.6f * u + 0.3f), side);
        }
        in.dt = dt;
        Pose pose = ref;
        const B::BodyOutput out = solver.Solve(pose.data(), in, {});
        if (!out.ok || out.arm[C::Right].status != B::ArmStatus::Solved ||
            out.arm[C::Left].status != B::ArmStatus::Solved) {
            ++failures;
            continue;
        }
        if (frame > 0) {
            for (const int bone :
                 {C::R_Clavicle, C::R_UpperArm, C::R_Forearm, C::R_Hand, C::Support_R_ForeArmTwist,
                  C::Support_R_ForeArmTwist1, C::Support_R_Elbow, C::Support_R_Shoulder,
                  C::L_Clavicle, C::L_UpperArm, C::L_Forearm, C::L_Hand, C::Support_L_ForeArmTwist,
                  C::UpperTurn}) {
                const float step = B::AngleBetween(pose[bone].q, prev[bone].q) / kDeg;
                if (step > max_bone_step) {
                    max_bone_step = step;
                    worst_bone = bone;
                }
            }
            for (size_t i = 0; i < 2; ++i) {
                max_side_step = std::max(max_side_step, AngleDeg(out.arm[i].bendDir, prev_side[i]));
            }
        }
        prev = pose;
        prev_side = {out.arm[0].bendDir, out.arm[1].bendDir};
    }
    std::printf("  smooth path: bone step %.2f deg (bone %d), elbow side step %.2f deg per frame\n",
                max_bone_step, worst_bone, max_side_step);
    EXPECT_EQ(failures, 0);
    EXPECT_LT(max_bone_step, 10.0f);
    EXPECT_LT(max_side_step, 6.0f);
}

TEST(BodySolver, TwistHelpersNeverSnap) {
    C0000_NEEDS_GAME_SKELETON(); // tuned on the hunter's chest, arms and hand frames
    // The old arms (natural_arms off; NaturalArms.TwistHelpersFollowThroughHalfATurn has the
    // natural ones). The controller rolls a full turn and more (1 degree a frame) in a guard.
    // Within 120 degrees of upright the forearm twist helpers follow the roll smoothly; held
    // upside down the wrist has to bend about 125 degrees, the twist about the forearm is barely
    // defined and the helpers turn over quickly, but they fade through zero instead of snapping
    // from +120 to -120 degrees in one frame (core/vr/arm_ik.h HelperTwist).
    const Pose ref = Reference();
    B::BodySolver solver;
    B::BodyInput in = DefaultInput(ref);
    in.arm_weight = {0.0f, 1.0f};
    B::BodyParams old_arms{};
    old_arms.natural_arms = false;
    Pose prev{};
    float max_step = 0.0f, max_step_upright = 0.0f, max_twist = 0.0f;
    for (int roll = -200; roll <= 200; ++roll) {
        in.target[C::Right] =
            GripTarget({0.18f, 1.25f, -0.40f}, 0.0f, 45.0f, static_cast<float>(roll), C::Right);
        Pose pose = ref;
        const B::BodyOutput out = solver.Solve(pose.data(), in, old_arms);
        ASSERT_TRUE(out.ok);
        ASSERT_EQ(out.arm[C::Right].status, B::ArmStatus::Solved);
        if (roll > -200) {
            for (const int bone : {C::Support_R_ForeArmTwist, C::Support_R_ForeArmTwist1}) {
                const float step = B::AngleBetween(pose[bone].q, prev[bone].q) / kDeg;
                max_step = std::max(max_step, step);
                if (std::abs(roll) <= 120) {
                    max_step_upright = std::max(max_step_upright, step);
                }
            }
        }
        max_twist = std::max(max_twist, B::AngleBetween(pose[C::Support_R_ForeArmTwist].q,
                                                        ref[C::Support_R_ForeArmTwist].q) /
                                            kDeg);
        prev = pose;
    }
    std::printf("  roll sweep: twist helper step %.2f deg per degree of roll within 120 deg of "
                "upright, %.2f upside down, largest twist %.1f deg\n",
                max_step_upright, max_step, max_twist);
    EXPECT_LT(max_step_upright, 4.0f);
    EXPECT_LT(max_step, 20.0f); // a snap turns a helper by 120+ degrees at once
    EXPECT_LE(max_twist, 120.5f);
    EXPECT_GT(max_twist, 100.0f);
}

TEST(BodySolver, ArmWeightBlendsWithTheAnimation) {
    // Weight 1 puts the weapon on the target, 0 leaves the animated arm, and in between the arm
    // lies in between (local-space blend: the effector moves monotonically toward the target).
    const Pose ref = Reference();
    const Vec3 animated = Fk(ref)[C::R_Weapon].t;
    const B::ModelTarget target = PresetTarget(*P::Find("guard"), C::Right, 0.5);
    float last = Dist(animated, target.p);
    for (const float w : {0.25f, 0.5f, 0.75f, 1.0f}) {
        B::BodySolver solver;
        B::BodyInput in = DefaultInput(ref);
        in.arm_weight = {0.0f, w};
        in.target[C::Right] = target;
        Pose pose = ref;
        const B::BodyOutput out = solver.Solve(pose.data(), in, {});
        ASSERT_TRUE(out.ok);
        const float d = Dist(Fk(pose)[C::R_Weapon].t, target.p);
        EXPECT_LT(d, last) << "weight " << w;
        last = d;
        if (w < 1.0f) {
            EXPECT_GT(d, 0.005f) << "weight " << w;
        }
    }
    EXPECT_LT(last, 0.001f);
}

TEST(BodySolver, PoleUsesTheEditedTorso) {
    // The elbow hint is taken from the shoulder and spine after the torso yaw and the shift.
    const Pose ref = Reference();
    B::BodySolver solver;
    B::BodyInput in = DefaultInput(ref);
    in.body_weight = 1.0f;
    in.head_yaw = 70.0f * kDeg;
    in.eye_m = in.eye_m + Vec3{0.12f, -0.1f, -0.1f};
    in.dt = 0.0f;
    in.arm_weight = {1.0f, 1.0f};
    // Hands at shoulder height, where the hint's weights are still changing (the shift moves the
    // shoulders 10 cm down).
    in.target[C::Right] = GripTarget({0.2f, 1.45f, -0.35f}, 0.0f, 20.0f, 0.0f, C::Right);
    in.target[C::Left] = GripTarget({-0.2f, 1.45f, -0.35f}, 0.0f, 20.0f, 0.0f, C::Left);
    for (const B::PostureMode posture : {B::PostureMode::Shift, B::PostureMode::Lean}) {
        B::BodyParams params{};
        params.posture = posture;
        B::BodySolver solver;
        solver.torso.Reset(45.0f * kDeg);
        Pose pose = ref;
        const B::BodyOutput out = solver.Solve(pose.data(), in, params);
        ASSERT_TRUE(out.ok);
        // The shift (Shift) or the lean (Lean; the height stays within its deadband).
        ASSERT_TRUE(posture == B::PostureMode::Lean || B::LengthSq(out.shift) > 1e-4f);
        ASSERT_TRUE(posture == B::PostureMode::Shift || std::abs(out.lean_right) > 2.0f * kDeg);
        // The pose before the arms: the same edits without arms.
        B::BodySolver torso_only;
        torso_only.torso.Reset(45.0f * kDeg);
        B::BodyInput no_arms = in;
        no_arms.arm_weight = {0.0f, 0.0f};
        Pose edited = ref;
        ASSERT_TRUE(torso_only.Solve(edited.data(), no_arms, params).ok);
        const Pose model = Fk(edited);
        const B::BodyFrame frame = B::TurnedFrame(out.torso_delta);
        for (const int side : {C::Left, C::Right}) {
            const auto i = static_cast<size_t>(side);
            const B::ArmRig rig = side == C::Right ? B::HunterRightArm() : B::HunterLeftArm();
            const Vec3 p = in.target[i]->p;
            const Vec3 expected = B::ArmPole(frame, rig.side, p - model[rig.upperArm].t,
                                             p - model[C::Spine2].t, params.natural_arms);
            EXPECT_LT(Dist(out.pole[i], expected), 1e-5f) << "side " << side;
        }
    }
}

TEST(BodySolver, LeftMirrorsRight) {
    const Pose ref = Reference();
    const Pose ref_model = Fk(ref);
    B::BodySolver solver;
    B::BodyInput in = DefaultInput(ref);
    in.arm_weight = {1.0f, 1.0f};
    const Vec3 rp = ref_model[C::R_UpperArm].t + Vec3{-0.1f, -0.15f, -0.35f};
    const Quat rq = B::AxisAngle({1.0f, 0.2f, 0.0f}, 0.4f) * ref_model[C::R_Weapon].q;
    in.target[C::Right] = B::ModelTarget{rp, rq};
    // The left target: the mirror image (x -> -x) of the right one; the left weapon bone's frame
    // is the right one's mirror with its Z axis flipped, so its rotation mirrors as (x, -y, -z, w)
    // composed with the flip.
    const Quat lq0 = ref_model[C::L_Weapon].q;
    const Quat rq0 = ref_model[C::R_Weapon].q;
    const Quat delta = rq * B::Conjugate(rq0);
    const Quat mirrored_delta{delta.x, -delta.y, -delta.z, delta.w};
    in.target[C::Left] = B::ModelTarget{{-rp.x, rp.y, rp.z}, B::Normalize(mirrored_delta * lq0)};
    Pose pose = ref;
    const B::BodyOutput out = solver.Solve(pose.data(), in, {});
    ASSERT_TRUE(out.ok);
    const B::ArmResult& r = out.arm[C::Right];
    const B::ArmResult& l = out.arm[C::Left];
    EXPECT_LT(Dist(r.elbow, {-l.elbow.x, l.elbow.y, l.elbow.z}), 1e-3f);
    EXPECT_LT(
        Dist(out.pole[C::Right], {-out.pole[C::Left].x, out.pole[C::Left].y, out.pole[C::Left].z}),
        1e-4f);
    EXPECT_LT(r.error, 1e-3f);
    EXPECT_LT(l.error, 1e-3f);
}

TEST(BodySolver, GuardsWriteNothing) {
    const Pose ref = Reference();
    const float nan = std::nanf("");
    const auto check = [&](B::BodyInput in, const char* what, Pose pose) {
        B::BodySolver solver;
        const B::BodyOutput out = solver.Solve(pose.data(), in, {});
        EXPECT_FALSE(out.ok) << what;
        EXPECT_TRUE(out.written.none()) << what;
        EXPECT_NE(out.fail, nullptr) << what;
    };
    B::BodyInput in = DefaultInput(ref);
    in.body_weight = 1.0f;
    in.arm_weight = {1.0f, 1.0f};
    in.hide = true;
    in.target[C::Right] = PresetTarget(P::Presets[0], C::Right, 0.5);
    {
        B::BodyInput bad = in;
        bad.eye_m.x = nan;
        check(bad, "eye", ref);
    }
    {
        B::BodyInput bad = in;
        bad.head_yaw = nan;
        check(bad, "head yaw", ref);
    }
    {
        B::BodyInput bad = in;
        bad.target[C::Right]->q.w = nan;
        check(bad, "target", ref);
    }
    {
        B::BodyInput bad = in;
        bad.arm_weight[1] = nan;
        check(bad, "weight", ref);
    }
    {
        Pose pose = ref;
        pose[C::L_Finger12].t.y = std::numeric_limits<float>::infinity();
        check(in, "pose", pose);
    }
    // A failed solve resets the filters: the next good frame starts fresh.
    B::BodySolver solver;
    Pose pose = ref;
    EXPECT_TRUE(solver.Solve(pose.data(), in, {}).ok);
    B::BodyInput bad = in;
    bad.chr_yaw = nan;
    pose = ref;
    EXPECT_FALSE(solver.Solve(pose.data(), bad, {}).ok);
    EXPECT_FALSE(solver.torso.valid);
    EXPECT_FALSE(solver.shift_valid);
}

// Attacks through dodges (dodge_attack): a kept quickstep takes swings and shots from its first
// frame (dodge_attack), while attack_ok stays the game's window (items, the crank and the
// transform still wait for R1 at 0.60 s); a kept stagger never does; dodge_attack=0, a dodge the
// game keeps the arms of (arm_keep_on_dodge=0), a weapon switch or death turn it off.
TEST(AnimGate, DodgeAttackGoesThroughKeptDodges) {
    const std::int64_t frame = 1'000'000'000 / 60;
    const float dt = 1.0f / 60.0f;
    for (const std::int32_t id : {7101, 7000, 7200, 7153}) {
        B::AnimGate gate;
        std::int64_t now = 0;
        bool always = true;
        std::int64_t window_at = -1;
        const std::int64_t start = now + frame;
        for (int i = 0; i * dt < 0.9f; ++i) {
            const float t = i * dt;
            B::GateSignals s = Frame({{id, t}}, t >= 0.60f ? kIdleAllow : 0x30020, false);
            s.keep_dodge = true;
            s.dodge_attack = true;
            const B::GateOut out = gate.Step(s, now += frame);
            always = always && out.kept && out.dodge_attack && out.ik[C::Right] && out.ik[C::Left];
            if (window_at < 0 && out.attack_ok[C::Right]) {
                window_at = now - start;
            }
        }
        EXPECT_TRUE(always) << id;
        EXPECT_NEAR(Millis(window_at), 600.0, 17.0) << id;
    }
    B::AnimGate gate;
    std::int64_t now = 0;
    // A kept stagger: swings wait for its window.
    B::GateSignals s = Frame({{20203, 0.0f}}, 0, false);
    s.keep_hit = s.keep_dodge = s.dodge_attack = true;
    B::GateOut out = gate.Step(s, now += frame);
    EXPECT_TRUE(out.kept);
    EXPECT_FALSE(out.dodge_attack);
    // dodge_attack=0: the old wait.
    gate.Reset();
    s = Frame({{7101, 0.0f}}, 0x30020, false);
    s.keep_dodge = true;
    out = gate.Step(s, now += frame);
    EXPECT_TRUE(out.kept && !out.dodge_attack && !out.attack_ok[C::Right]);
    // arm_keep_on_dodge=0: the game's arms, nothing goes through.
    gate.Reset();
    s = Frame({{7101, 0.0f}}, 0x30020, false);
    s.dodge_attack = true;
    out = gate.Step(s, now += frame);
    EXPECT_FALSE(out.kept || out.dodge_attack);
    // A wake-up roll (7402, the game's arms) never.
    gate.Reset();
    s = Frame({{7402, 0.0f}}, 0x30020, false);
    s.keep_dodge = s.dodge_attack = true;
    out = gate.Step(s, now += frame);
    EXPECT_FALSE(out.dodge_attack);
    // Death mid-dodge: the hard gate takes both arms.
    gate.Reset();
    s = Frame({{7101, 0.0f}}, 0x30020, false);
    s.keep_dodge = s.dodge_attack = true;
    out = gate.Step(s, now += frame);
    EXPECT_TRUE(out.dodge_attack);
    s = Frame({{7101, dt}}, 0x30020, false);
    s.keep_dodge = s.dodge_attack = true;
    s.hp_valid = true;
    s.hp = 0;
    out = gate.Step(s, now += frame);
    EXPECT_FALSE(out.dodge_attack || out.kept);
}

// Stagger vignette (core/vr/daze.h): a kept stagger is Stagger until its R1 / L2 window (0.33 s),
// a knockdown (the game's arms) Knockdown until its own, a dodge or a guard reaction never; the
// fader reaches the edge opacity in 90 ms, holds, clears in 150 ms, and a reset ends it at once.
TEST(Daze, LevelsFollowTheGateAndFade) {
    namespace D = VR::Daze;
    const std::int64_t frame = 1'000'000'000 / 60;
    const float dt = 1.0f / 60.0f;
    for (const bool keep : {true, false}) {
        B::AnimGate gate;
        std::int64_t now = 0;
        gate.Step(Frame({{0, 1.0f}}, kIdleAllow, true), now += frame);
        const std::int64_t start = now + frame;
        std::int64_t clear_at = -1;
        for (int i = 0; i * dt < 0.6f; ++i) {
            const float t = i * dt;
            const std::uint64_t allow = i == 0 ? kIdleAllow : (t >= 0.33f ? kAttacks : 0);
            B::GateSignals s = Frame({{20134, t}}, allow, false);
            s.keep_hit = keep;
            const B::GateOut out = gate.Step(s, now += frame);
            const D::Level l = D::LevelOf(out);
            if (clear_at < 0 && l == D::Level::None) {
                clear_at = now - start;
            } else if (clear_at < 0) {
                EXPECT_EQ(l, D::Level::Stagger) << "frame " << i;
            }
        }
        EXPECT_NEAR(Millis(clear_at), 330.0, 17.0) << "keep " << keep;
    }
    B::AnimGate gate;
    std::int64_t now = 0;
    B::GateOut out = gate.Step(Frame({{20500, 0.0f}}, 0, false), now += frame);
    EXPECT_EQ(D::LevelOf(out), D::Level::Knockdown);
    gate.Reset();
    out = gate.Step(Frame({{22100, 0.0f}}, 0, false), now += frame);
    EXPECT_EQ(D::LevelOf(out), D::Level::Knockdown);
    for (const std::int32_t id : {7101, 7200, 10000, 30000, 24000}) {
        gate.Reset();
        B::GateSignals s = Frame({{id, 0.0f}}, 0, false);
        s.keep_hit = s.keep_dodge = true;
        out = gate.Step(s, now += frame);
        EXPECT_EQ(D::LevelOf(out), D::Level::None) << id;
    }
    // Dead in a stagger: none.
    gate.Reset();
    B::GateSignals s = Frame({{20203, 0.0f}}, 0, false);
    s.keep_hit = true;
    s.hp_valid = true;
    s.hp = 0;
    out = gate.Step(s, now += frame);
    EXPECT_EQ(D::LevelOf(out), D::Level::None);

    // The fader.
    D::Fader f;
    float a = 0.0f;
    int frames_in = 0;
    while (a < D::StaggerAlpha - 1e-4f && frames_in < 100) {
        a = f.Step(D::Level::Stagger, false, 1.0f / 90.0f, 1.0f);
        ++frames_in;
    }
    EXPECT_NEAR(frames_in / 90.0, D::FadeInS, 1.5 / 90.0);
    for (int i = 0; i < 90; ++i) {
        a = f.Step(D::Level::Stagger, false, 1.0f / 90.0f, 1.0f);
    }
    EXPECT_NEAR(a, D::StaggerAlpha, 1e-5f);
    // Turning into a knockdown: no drop, then the knockdown's opacity.
    const float before = a;
    a = f.Step(D::Level::Knockdown, false, 1.0f / 90.0f, 1.0f);
    EXPECT_GE(a, before - 1e-4f);
    for (int i = 0; i < 30; ++i) {
        a = f.Step(D::Level::Knockdown, false, 1.0f / 90.0f, 1.0f);
    }
    EXPECT_NEAR(a, D::KnockdownAlpha, 1e-5f);
    EXPECT_EQ(f.Shown(), D::Level::Knockdown);
    // Clearing: 150 ms, the knockdown's profile kept while it fades.
    int frames_out = 0;
    while (a > 0.0f && frames_out < 100) {
        a = f.Step(D::Level::None, false, 1.0f / 90.0f, 1.0f);
        ++frames_out;
        if (a > 0.0f) {
            EXPECT_EQ(f.Shown(), D::Level::Knockdown);
        }
    }
    EXPECT_NEAR(frames_out / 90.0, D::FadeOutS, 1.5 / 90.0);
    // Strength scales (capped), a reset ends it at once.
    for (int i = 0; i < 30; ++i) {
        a = f.Step(D::Level::Knockdown, false, 1.0f / 90.0f, 1.5f);
    }
    EXPECT_NEAR(a, D::MaxAlpha, 1e-5f);
    EXPECT_EQ(f.Step(D::Level::Knockdown, true, 1.0f / 90.0f, 1.0f), 0.0f);
    EXPECT_EQ(f.Shown(), D::Level::None);
    for (int i = 0; i < 30; ++i) {
        a = f.Step(D::Level::Stagger, false, 1.0f / 90.0f, 0.5f);
    }
    EXPECT_NEAR(a, 0.5f * D::StaggerAlpha, 1e-5f);
}

// A flurry (stagger_vignette_merge_ms=400): a clearing drops to the faint rest at once (the "you
// can act" cue), holds it to 400 ms after the clear and then fades it; a blow staggering again
// inside that time fades in from the rest (never a clear view between, a fresh fade-in of 90 ms
// from clear); merge 0 is the full 150 ms fade.
TEST(Daze, AFlurryMergesInsteadOfFlashing) {
    namespace D = VR::Daze;
    const float dt = 1.0f / 90.0f;
    const float merge = 0.4f;
    const auto full = [&](D::Fader& f) {
        for (int i = 0; i < 30; ++i) {
            f.Step(D::Level::Stagger, false, dt, 1.0f, merge);
        }
    };
    D::Fader f;
    full(f);
    EXPECT_FALSE(f.Merged());
    ASSERT_NEAR(f.Linear(), 1.0f, 1e-6f);
    // The drop to the rest: (1 - 0.3) x 150 ms = 105 ms.
    int frames = 0;
    while (f.Linear() > D::MergeFloor + 1e-5f && frames < 100) {
        f.Step(D::Level::None, false, dt, 1.0f, merge);
        ++frames;
    }
    EXPECT_NEAR(frames * dt, (1.0f - D::MergeFloor) * D::FadeOutS, 1.5f * dt);
    const float rest = f.Step(D::Level::None, false, dt, 1.0f, merge);
    ++frames;
    EXPECT_NEAR(rest, D::StaggerAlpha * 0.216f, 1e-3f); // smoothstep(0.3)
    // Held to 400 ms after the clear.
    while ((frames + 1) * dt < merge - 0.5f * dt) {
        const float held = f.Step(D::Level::None, false, dt, 1.0f, merge); // (evaluated once)
        EXPECT_NEAR(held, rest, 1e-6f);
        ++frames;
    }
    // A blow staggering again (at 0.4 s): from the rest, full in (1 - 0.3) x 90 ms = 63 ms.
    int in = 0;
    float a = rest;
    float lowest = rest;
    while (a < D::StaggerAlpha - 1e-4f && in < 100) {
        a = f.Step(D::Level::Stagger, false, dt, 1.0f, merge);
        lowest = std::min(lowest, a);
        ++in;
    }
    EXPECT_TRUE(f.Merged());
    EXPECT_GE(lowest, rest - 1e-6f);
    EXPECT_NEAR(in * dt, (1.0f - D::MergeFloor) * D::FadeInS, 1.5f * dt);
    // No blow after it: the rest goes 400 ms after the clear (45 ms fade).
    frames = 0;
    while (f.Shown() != D::Level::None && frames < 200) {
        f.Step(D::Level::None, false, dt, 1.0f, merge);
        ++frames;
    }
    EXPECT_NEAR(frames * dt, merge + D::MergeFloor * D::FadeOutS, 1.5f * dt);
    EXPECT_EQ(f.Linear(), 0.0f);
    // A fresh stagger from clear: not merged, 90 ms.
    in = 0;
    a = 0.0f;
    while (a < D::StaggerAlpha - 1e-4f && in < 100) {
        a = f.Step(D::Level::Stagger, false, dt, 1.0f, merge);
        ++in;
    }
    EXPECT_FALSE(f.Merged());
    EXPECT_NEAR(in * dt, D::FadeInS, 1.5f * dt);
    // A short stagger (cleared at 0.2 linear) holds its own lower level, never rises.
    D::Fader s;
    for (int i = 0; i < 2; ++i) {
        s.Step(D::Level::Stagger, false, dt, 1.0f, merge);
    }
    const float low = s.Linear();
    ASSERT_LT(low, D::MergeFloor);
    for (int i = 0; i < 20; ++i) {
        s.Step(D::Level::None, false, dt, 1.0f, merge);
        EXPECT_NEAR(s.Linear(), low, 1e-6f);
    }
    // Death in the rest: off at once.
    const float dead = s.Step(D::Level::None, true, dt, 1.0f, merge);
    EXPECT_EQ(dead, 0.0f);
    EXPECT_EQ(s.Shown(), D::Level::None);
    // merge 0: the full 150 ms fade.
    D::Fader z;
    for (int i = 0; i < 30; ++i) {
        z.Step(D::Level::Stagger, false, dt, 1.0f, 0.0f);
    }
    frames = 0;
    while (z.Shown() != D::Level::None && frames < 100) {
        z.Step(D::Level::None, false, dt, 1.0f, 0.0f);
        ++frames;
    }
    EXPECT_NEAR(frames * dt, D::FadeOutS, 1.5f * dt);
}

// ---- Natural arms (body_natural_arms, 2026-10-08) ---------------------------------------------
// The user: crossed hands and hands brought to the chest "don't fold like real human hands". The
// hand poses are the arms:repro human reference's controllers (a human-like arm posed on the
// hunter's own bones; its weapon bone is the controller), in the mock presets' convention.

namespace {

struct Grip {
    Vec3 p;
    float yaw, pitch, roll;
};
struct GripPair {
    const char* name;
    Grip r, l;
};

// The arms:repro human poses (scratchpad overnight/arms/ana/presets.txt) and hands brought to the
// chest (the user's 12:05 note: fists at the sternum, under the chin, on the collarbones, both
// hands together, hugging the shoulders).
const std::vector<GripPair>& HumanGrips() {
    static const std::vector<GripPair> grips = {
        {"guard", {{0.117f, 1.212f, -0.314f}, 58.3f, 37.2f, 48.7f},
         {{-0.146f, 1.070f, -0.243f}, -43.9f, 29.0f, -26.5f}},
        {"fold_RoL", {{-0.204f, 1.331f, -0.117f}, 133.9f, -16.9f, 109.9f},
         {{0.197f, 1.299f, -0.048f}, -143.3f, -12.0f, -105.8f}},
        {"fold_LoR", {{-0.197f, 1.299f, -0.048f}, 142.9f, -13.4f, 105.9f},
         {{0.204f, 1.331f, -0.117f}, -134.5f, -15.5f, -109.7f}},
        {"Xblock", {{-0.095f, 1.337f, -0.346f}, 98.1f, 18.0f, 81.2f},
         {{0.098f, 1.316f, -0.324f}, -99.9f, 19.4f, -80.4f}},
        {"R_Lshoulder", {{-0.180f, 1.484f, -0.000f}, 145.8f, -12.2f, 149.5f},
         {{-0.146f, 1.070f, -0.243f}, -43.9f, 29.0f, -26.5f}},
        {"R_Lhip", {{-0.164f, 0.982f, -0.050f}, 160.2f, -32.6f, 47.5f},
         {{-0.146f, 1.070f, -0.243f}, -43.9f, 29.0f, -26.5f}},
        {"X_overhead", {{-0.110f, 1.971f, -0.002f}, 88.5f, -17.2f, 170.7f},
         {{0.114f, 1.947f, -0.023f}, -90.7f, -20.7f, -169.3f}},
        {"behind_head", {{-0.044f, 1.708f, 0.219f}, 39.5f, 3.6f, -93.4f},
         {{0.044f, 1.708f, 0.219f}, -39.5f, 2.2f, 93.4f}},
        {"palms_up", {{0.101f, 1.275f, -0.268f}, -51.1f, 30.7f, -97.9f},
         {{-0.101f, 1.275f, -0.268f}, 50.9f, 29.3f, 97.8f}},
        {"palms_down", {{0.107f, 1.239f, -0.277f}, 83.6f, -11.5f, 97.1f},
         {{-0.107f, 1.239f, -0.277f}, -83.8f, -10.1f, -97.1f}},
        {"praying", {{0.026f, 1.352f, -0.201f}, 176.5f, 85.2f, 179.2f},
         {{-0.026f, 1.352f, -0.201f}, 166.7f, 85.0f, 164.2f}},
        {"vial", {{0.079f, 1.155f, -0.247f}, 75.3f, 38.2f, 52.6f},
         {{0.115f, 1.214f, -0.207f}, -112.1f, -28.6f, -101.3f}},
        {"paper", {{0.117f, 1.212f, -0.314f}, 58.3f, 37.2f, 48.7f},
         {{0.140f, 1.469f, -0.410f}, -96.9f, -9.6f, -96.6f}},
        {"pron150", {{0.135f, 1.257f, -0.289f}, 25.8f, -53.7f, 162.2f},
         {{-0.146f, 1.070f, -0.243f}, -43.9f, 29.0f, -26.5f}},
        {"sup150", {{0.127f, 1.274f, -0.284f}, -36.3f, -25.5f, -126.2f},
         {{-0.146f, 1.070f, -0.243f}, -43.9f, 29.0f, -26.5f}},
        {"pron175", {{0.134f, 1.265f, -0.287f}, -9.7f, -49.0f, -158.6f},
         {{-0.146f, 1.070f, -0.243f}, -43.9f, 29.0f, -26.5f}},
        // fists against the sternum, the forearms folded in front
        {"sternum", {{0.03f, 1.24f, -0.20f}, 90.0f, 40.0f, 60.0f},
         {{-0.03f, 1.24f, -0.20f}, -90.0f, 40.0f, -60.0f}},
        // both hands under the chin
        {"chin", {{0.03f, 1.42f, -0.21f}, 20.0f, 70.0f, 0.0f},
         {{-0.03f, 1.42f, -0.21f}, -20.0f, 70.0f, 0.0f}},
        // on the collarbones, palms down
        {"collar", {{0.07f, 1.44f, -0.13f}, 150.0f, -30.0f, 90.0f},
         {{-0.07f, 1.44f, -0.13f}, -150.0f, -30.0f, -90.0f}},
        // both hands together at the chest
        {"together", {{0.02f, 1.25f, -0.30f}, 10.0f, 75.0f, 0.0f},
         {{-0.02f, 1.25f, -0.30f}, -10.0f, 75.0f, 0.0f}},
        // hugging the shoulders
        {"hug", {{-0.21f, 1.40f, -0.09f}, 150.0f, -10.0f, 120.0f},
         {{0.20f, 1.33f, -0.12f}, -150.0f, -10.0f, -120.0f}},
    };
    return grips;
}

B::BodyParams ArmsAloneParams(bool natural) {
    B::BodyParams p{};
    p.align = false;
    p.torso_follow = false;
    p.natural_arms = natural;
    return p;
}

std::array<std::optional<B::ModelTarget>, 2> TargetsOf(const GripPair& g) {
    std::array<std::optional<B::ModelTarget>, 2> t{};
    t[C::Right] = GripTarget(g.r.p, g.r.yaw, g.r.pitch, g.r.roll, C::Right);
    t[C::Left] = GripTarget(g.l.p, g.l.yaw, g.l.pitch, g.l.roll, C::Left);
    return t;
}

// Both arms held on `t` for `frames` frames at 90 Hz (the elbow side settles).
B::BodyOutput HoldArms(B::BodySolver& solver, Pose& pose,
                       const std::array<std::optional<B::ModelTarget>, 2>& t, bool natural,
                       int frames = 90) {
    const Pose ref = Reference();
    B::BodyInput in = DefaultInput(ref);
    in.dt = 1.0f / 90.0f;
    in.body_weight = 1.0f;
    in.arm_weight = {1.0f, 1.0f};
    in.target = t;
    B::BodyOutput out{};
    for (int i = 0; i < frames; ++i) {
        pose = ref;
        out = solver.Solve(pose.data(), in, ArmsAloneParams(natural));
    }
    return out;
}

int ForearmOf(int side) {
    return side == C::Right ? C::R_Forearm : C::L_Forearm;
}
int WeaponOf(int side) {
    return side == C::Right ? C::R_Weapon : C::L_Weapon;
}

// The rendered effector (FK of the written pose) against the target: metres, degrees.
std::pair<float, float> EffectorError(const Pose& pose, int side, const B::ModelTarget& t) {
    const auto eff = B::ModelOf(pose.data(), C::Parents.data(), C::BoneCount, WeaponOf(side));
    return {Dist(eff->t, t.p), B::AngleBetween(eff->q, t.q) / kDeg};
}

// The trunk the natural elbows keep out of, on the reference pose.
B::ElbowKeepOut ReferenceTrunk() {
    const Pose model = Fk(Reference());
    return B::TrunkKeepOut(model[C::Spine].t, model[C::Neck].t, B::BodyFrame{});
}

// The forearm twist left in the wrist skin: the hand's twist about the forearm (from the
// reference) minus the wrist twist helper's, degrees.
float WristSkinTwist(const Pose& pose, int side) {
    const Pose ref = Reference();
    const int hand = side == C::Right ? C::R_Hand : C::L_Hand;
    const int helper = side == C::Right ? C::Support_R_ForeArmTwist : C::Support_L_ForeArmTwist;
    const Vec3 axis = B::Normalize(ref[static_cast<size_t>(hand)].t);
    const float h = B::TwistAngle(B::Normalize(pose[hand].q * B::Conjugate(ref[hand].q)), axis);
    const float w = B::TwistAngle(B::Normalize(pose[helper].q * B::Conjugate(ref[helper].q)), axis);
    return B::WrapPi(h - w) / kDeg;
}

B::ModelTarget LerpTarget(const B::ModelTarget& a, const B::ModelTarget& b, float k) {
    return {B::Lerp(a.p, b.p, k), B::Slerp(a.q, b.q, k)};
}

float MinJerk(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * x * (10.0f - 15.0f * x + 6.0f * x * x);
}

} // namespace

TEST(NaturalArms, HandsStayExactlyOnTheControllers) {
    // Every human pose (crossed, folded, at the chest, overhead, behind the head, rolled past the
    // forearm's range) and every reachable mock preset: both hands within 1 mm and 0.1 degrees of
    // their controllers, with the natural arms as without.
    std::vector<std::array<std::optional<B::ModelTarget>, 2>> targets;
    for (const GripPair& g : HumanGrips()) {
        targets.push_back(TargetsOf(g));
    }
    for (const std::string_view name : {"rest", "still", "guard", "blade_level", "wrist_inject"}) {
        for (const P::Preset& preset : P::Presets) {
            if (preset.name == name) {
                std::array<std::optional<B::ModelTarget>, 2> t{};
                t[C::Right] = PresetTarget(preset, C::Right, 0.5);
                t[C::Left] = PresetTarget(preset, C::Left, 0.5);
                targets.push_back(t);
            }
        }
    }
    float worst_m = 0.0f, worst_deg = 0.0f;
    for (const auto& t : targets) {
        B::BodySolver solver;
        Pose pose{};
        const B::BodyOutput out = HoldArms(solver, pose, t, true, 30);
        ASSERT_TRUE(out.ok);
        for (const int side : {C::Left, C::Right}) {
            ASSERT_EQ(out.arm[static_cast<size_t>(side)].status, B::ArmStatus::Solved);
            const auto [m, deg] = EffectorError(pose, side, *t[static_cast<size_t>(side)]);
            worst_m = std::max(worst_m, m);
            worst_deg = std::max(worst_deg, deg);
        }
        for (const QsTransform& x : pose) {
            EXPECT_TRUE(B::IsFinite(x));
        }
    }
    std::printf("  %zu poses: hands at most %.3f mm and %.4f deg off their controllers\n",
                targets.size(), worst_m * 1000.0f, worst_deg);
    EXPECT_LT(worst_m, 0.001f);
    EXPECT_LT(worst_deg, 0.1f);
}

TEST(NaturalArms, FoldedArmsHangTheirElbowsAndNeverFlip) {
    C0000_NEEDS_GAME_SKELETON(); // tuned on the hunter's chest, arms and hand frames
    // Both arms from the guard into arms folded across the chest (right over left) and back,
    // minimum jerk, 1 s each way at 90 Hz. The elbows hang below the wrists in front of the chest
    // (the old arms held them 7 cm higher and further forward, the repro's worst pose), stay out
    // of the trunk, and move smoothly: no flip, no jump.
    const GripPair& guard = HumanGrips()[0];
    const GripPair& fold = HumanGrips()[1];
    const auto a = TargetsOf(guard), b = TargetsOf(fold);
    const B::ElbowKeepOut trunk = ReferenceTrunk();
    for (const bool natural : {false, true}) {
        B::BodySolver solver;
        const Pose ref = Reference();
        B::BodyInput in = DefaultInput(ref);
        in.dt = 1.0f / 90.0f;
        in.body_weight = 1.0f;
        in.arm_weight = {1.0f, 1.0f};
        std::array<Vec3, 2> prev_elbow{}, prev_bend{};
        float max_step = 0.0f, max_bend = 0.0f, max_depth = -1.0f, max_err = 0.0f;
        Pose pose{};
        B::BodyOutput out{};
        for (int f = 0; f <= 180; ++f) {
            const float k = MinJerk(f <= 90 ? f / 90.0f : (180 - f) / 90.0f);
            for (const int side : {C::Left, C::Right}) {
                const auto i = static_cast<size_t>(side);
                in.target[i] = LerpTarget(*a[i], *b[i], k);
            }
            pose = ref;
            out = solver.Solve(pose.data(), in, ArmsAloneParams(natural));
            ASSERT_TRUE(out.ok);
            const Pose model = Fk(pose);
            for (const int side : {C::Left, C::Right}) {
                const auto i = static_cast<size_t>(side);
                const Vec3 elbow = model[static_cast<size_t>(ForearmOf(side))].t;
                const Vec3 bend = out.arm[i].bendDir;
                if (f > 0) {
                    max_step = std::max(max_step, Dist(elbow, prev_elbow[i]));
                    max_bend = std::max(max_bend, AngleDeg(bend, prev_bend[i]));
                }
                prev_elbow[i] = elbow;
                prev_bend[i] = bend;
                max_depth = std::max(max_depth, B::KeepOutDepth(trunk, elbow));
                max_err = std::max(max_err, EffectorError(pose, side, *in.target[i]).first);
            }
        }
        // Held folded: where the elbows are.
        Pose held{};
        B::BodySolver hold;
        HoldArms(hold, held, b, natural);
        const Pose model = Fk(held);
        std::printf("  %s: elbow step %.2f cm, elbow side %.2f deg per frame at most, trunk depth "
                    "%.3f, hands %.3f mm;",
                    natural ? "natural" : "old", max_step * 100.0f, max_bend, max_depth,
                    max_err * 1000.0f);
        for (const int side : {C::Right, C::Left}) {
            const Vec3 elbow = model[static_cast<size_t>(ForearmOf(side))].t;
            const Vec3 wrist = model[static_cast<size_t>(side == C::Right ? C::R_Hand : C::L_Hand)].t;
            const Vec3 shoulder =
                model[static_cast<size_t>(side == C::Right ? C::R_UpperArm : C::L_UpperArm)].t;
            const float below_wrist = wrist.y - elbow.y;
            const float below_shoulder = shoulder.y - elbow.y;
            const float forward = -(elbow.z - model[C::Spine2].t.z);
            std::printf(" %s elbow %.1f cm under the shoulder, %.1f under the wrist, %.1f in front",
                        side == C::Right ? "R" : "L", below_shoulder * 100.0f,
                        below_wrist * 100.0f, forward * 100.0f);
            if (natural) {
                EXPECT_GT(below_shoulder, 0.19f);
                EXPECT_GT(below_wrist, 0.05f);
                EXPECT_GT(forward, 0.05f); // in front of the chest, not beside or behind it
                EXPECT_LT(B::KeepOutDepth(trunk, elbow), 0.06f);
            }
        }
        std::printf("\n");
        EXPECT_LT(max_err, 0.001f);
        if (natural) {
            EXPECT_LT(max_step, 0.012f); // the human reference's elbow moves 0.66 cm a frame
            EXPECT_LT(max_bend, 3.0f);
            EXPECT_LT(max_depth, 0.06f); // within 1.2 cm of the trunk's surface
        }
    }
}

TEST(NaturalArms, HandsBroughtToTheChestStayOutOfIt) {
    C0000_NEEDS_GAME_SKELETON(); // tuned on the hunter's chest, arms and hand frames
    // The user's 12:05 note: hands brought close to the chest. Both arms from arm's length in front
    // to fists at the sternum, up under the chin, onto the collarbones, together at the chest,
    // into a hug and folded, and back out, 1 s each (minimum jerk, 90 Hz): the natural elbows stay
    // out of the trunk (within 1 cm of its surface), never flip, move no faster than the old
    // arms' do on the same path, and the hands stay on the controllers.
    std::vector<std::array<std::optional<B::ModelTarget>, 2>> keys;
    {
        std::array<std::optional<B::ModelTarget>, 2> away{};
        away[C::Right] = GripTarget({0.18f, 1.30f, -0.55f}, 0.0f, 30.0f, 0.0f, C::Right);
        away[C::Left] = GripTarget({-0.18f, 1.30f, -0.55f}, 0.0f, 30.0f, 0.0f, C::Left);
        keys.push_back(away);
    }
    for (const char* name : {"sternum", "chin", "collar", "together", "hug", "fold_LoR"}) {
        for (const GripPair& g : HumanGrips()) {
            if (std::string_view{g.name} == name) {
                keys.push_back(TargetsOf(g));
            }
        }
    }
    keys.push_back(keys.front());
    const B::ElbowKeepOut trunk = ReferenceTrunk();
    std::array<float, 2> step{}, depth{};
    for (const bool natural : {false, true}) {
        const auto n = static_cast<size_t>(natural ? 1 : 0);
        B::BodySolver solver;
        const Pose ref = Reference();
        B::BodyInput in = DefaultInput(ref);
        in.dt = 1.0f / 90.0f;
        in.body_weight = 1.0f;
        in.arm_weight = {1.0f, 1.0f};
        std::array<Vec3, 2> prev_elbow{}, prev_bend{};
        float max_bend = 0.0f, max_err = 0.0f;
        std::array<float, 8> seg_step{}, seg_depth{-1, -1, -1, -1, -1, -1, -1, -1};
        depth[n] = -1.0f;
        const int frames = 90 * static_cast<int>(keys.size() - 1);
        for (int f = 0; f <= frames; ++f) {
            const int seg = std::min(f / 90, static_cast<int>(keys.size()) - 2);
            const auto s_ = static_cast<size_t>(seg);
            const float k = MinJerk(static_cast<float>(f - seg * 90) / 90.0f);
            for (const int side : {C::Left, C::Right}) {
                const auto i = static_cast<size_t>(side);
                in.target[i] = LerpTarget(*keys[s_][i], *keys[s_ + 1][i], k);
            }
            Pose pose = ref;
            const B::BodyOutput out = solver.Solve(pose.data(), in, ArmsAloneParams(natural));
            ASSERT_TRUE(out.ok);
            const Pose model = Fk(pose);
            for (const int side : {C::Left, C::Right}) {
                const auto i = static_cast<size_t>(side);
                const Vec3 elbow = model[static_cast<size_t>(ForearmOf(side))].t;
                const Vec3 bend = out.arm[i].bendDir;
                if (f > 0) {
                    seg_step[s_] = std::max(seg_step[s_], Dist(elbow, prev_elbow[i]));
                    max_bend = std::max(max_bend, AngleDeg(bend, prev_bend[i]));
                }
                prev_elbow[i] = elbow;
                prev_bend[i] = bend;
                seg_depth[s_] = std::max(seg_depth[s_], B::KeepOutDepth(trunk, elbow));
                max_err = std::max(max_err, EffectorError(pose, side, *in.target[i]).first);
            }
        }
        std::printf("  %s:", natural ? "natural" : "old    ");
        for (size_t k = 0; k + 1 < keys.size(); ++k) {
            std::printf(" [%zu] %.2f cm %.2f", k, seg_step[k] * 100.0f, seg_depth[k]);
            step[n] = std::max(step[n], seg_step[k]);
            depth[n] = std::max(depth[n], seg_depth[k]);
        }
        std::printf(" | elbow side %.2f deg a frame, hands %.3f mm (elbow step a frame, trunk "
                    "depth per segment)\n",
                    max_bend, max_err * 1000.0f);
        EXPECT_LT(max_err, 0.001f);
        if (natural) {
            EXPECT_LT(max_bend, 4.0f);
        }
    }
    EXPECT_LT(depth[1], 0.06f); // 1.2 cm into the trunk's 20 x 14.5 cm ellipse at most
    EXPECT_LT(step[1], std::max(0.015f, step[0] + 0.005f));
}

TEST(NaturalArms, ForearmTakesTheRollBeforeTheElbow) {
    C0000_NEEDS_GAME_SKELETON(); // tuned on the hunter's chest, arms and hand frames
    // The right hand at the guard rolled about the forearm (the wrist where it is) past the
    // forearm's own range: palm down 140 degrees turns the natural elbow out and up and the twist
    // goes down the forearm (the wrist skin keeps almost none, the mid-forearm helper half of the
    // wrist helper's); palm up 150 turns the elbow in, but never into the trunk. (Within the range
    // the roll moves no elbow: ArmIk.NaturalRollTurnsTheElbowOnlyPastTheForearm.)
    const GripPair& guard = HumanGrips()[0];
    const auto base = TargetsOf(guard);
    Pose pose0{};
    B::BodySolver s0;
    HoldArms(s0, pose0, base, true);
    const Pose m0 = Fk(pose0);
    const Vec3 wrist = m0[C::R_Hand].t;
    const Vec3 axis = B::Normalize(m0[C::R_Hand].t - m0[C::R_Forearm].t);
    const auto rolled = [&](float deg) { // + = pronation (palm turning down)
        const Quat q = B::AxisAngle(axis, deg * kDeg);
        auto t = base;
        t[C::Right] = B::ModelTarget{wrist + B::Rotate(q, base[C::Right]->p - wrist),
                                     B::Normalize(q * base[C::Right]->q)};
        return t;
    };
    const auto solve = [&](float deg, bool natural, Pose& p) {
        B::BodySolver s;
        const B::BodyOutput out = HoldArms(s, p, rolled(deg), natural);
        EXPECT_LT(EffectorError(p, C::Right, *rolled(deg)[C::Right]).first, 0.001f);
        return out.arm[C::Right];
    };
    const B::ElbowKeepOut trunk = ReferenceTrunk();
    Pose still{}, pron{}, sup{}, pron_old{};
    solve(0.0f, true, still);
    const B::ArmResult r_pron = solve(140.0f, true, pron);
    const B::ArmResult r_sup = solve(-150.0f, true, sup);
    solve(140.0f, false, pron_old);
    const Vec3 e0 = Fk(still)[C::R_Forearm].t;
    const Vec3 e_pron = Fk(pron)[C::R_Forearm].t;
    const Vec3 e_sup = Fk(sup)[C::R_Forearm].t;
    const float out_pron = -(e_pron.x - e0.x); // the right arm's out is model -X
    const float up_pron = e_pron.y - e0.y;
    const float out_sup = -(e_sup.x - e0.x);
    const float skin = WristSkinTwist(pron, C::Right);
    const float skin_old = WristSkinTwist(pron_old, C::Right);
    const Pose ref = Reference();
    const Vec3 ax = B::Normalize(ref[C::R_Hand].t);
    const auto twist_of = [&](const Pose& p, int bone) {
        return B::TwistAngle(B::Normalize(p[bone].q * B::Conjugate(ref[bone].q)), ax) / kDeg;
    };
    const float h99 = twist_of(pron, C::Support_R_ForeArmTwist);
    const float h100 = twist_of(pron, C::Support_R_ForeArmTwist1);
    std::printf("  palm down 140: roll %.0f deg, elbow turn %.0f, %.1f cm out, %.1f up, wrist skin "
                "%.1f deg (old %.1f), helpers %.1f / %.1f; palm up 150: roll %.0f, turn %.0f "
                "(%.0f of it out of the trunk), %.1f cm out, trunk depth %.3f\n",
                r_pron.wristRoll / kDeg, r_pron.poleTurn / kDeg, out_pron * 100.0f,
                up_pron * 100.0f, skin, skin_old, h99, h100, r_sup.wristRoll / kDeg,
                r_sup.poleTurn / kDeg, r_sup.keptOut / kDeg, out_sup * 100.0f,
                B::KeepOutDepth(trunk, e_sup));
    EXPECT_GT(r_pron.wristRoll, 100.0f * kDeg);
    EXPECT_GT(r_pron.poleTurn, 20.0f * kDeg);
    EXPECT_GT(out_pron + up_pron, 0.05f);
    EXPECT_LT(r_sup.wristRoll, -100.0f * kDeg);
    EXPECT_LT(out_sup, 0.0f);
    EXPECT_LT(B::KeepOutDepth(trunk, e_sup), 0.06f);
    EXPECT_LT(std::abs(skin), 10.0f);
    EXPECT_GT(std::abs(h99), 100.0f);
    EXPECT_NEAR(h100, 0.5f * h99, 3.0f);
}

TEST(NaturalArms, TwistHelpersFollowThroughHalfATurn) {
    // The controller rolled at the guard, 1 degree a frame (60 Hz): -150 .. +150 and back (as far
    // as a hand goes), then on round a full turn and more. The natural twist helpers follow the
    // roll continuously through 180 degrees (the old ones faded through zero there and the wrist
    // skin snapped 14-16 degrees a frame); past what an arm can do they unwind at their rate
    // (720 degrees/s), never in one frame. The hand stays on the controller throughout.
    const Pose ref = Reference();
    B::BodySolver solver;
    B::BodyInput in = DefaultInput(ref);
    in.arm_weight = {0.0f, 1.0f};
    std::vector<int> rolls;
    for (int r = -150; r <= 150; ++r) {
        rolls.push_back(r);
    }
    for (int r = 149; r >= -150; --r) {
        rolls.push_back(r);
    }
    const size_t human = rolls.size();
    for (int r = -149; r <= 300; ++r) {
        rolls.push_back(r);
    }
    Pose prev{};
    float max_step_human = 0.0f, max_step = 0.0f, max_twist = 0.0f, max_err = 0.0f;
    for (size_t k = 0; k < rolls.size(); ++k) {
        in.target[C::Right] = GripTarget({0.18f, 1.25f, -0.40f}, 0.0f, 45.0f,
                                         static_cast<float>(rolls[k]), C::Right);
        Pose pose = ref;
        const B::BodyOutput out = solver.Solve(pose.data(), in, ArmsAloneParams(true));
        ASSERT_TRUE(out.ok);
        ASSERT_EQ(out.arm[C::Right].status, B::ArmStatus::Solved);
        if (k > 0) {
            for (const int bone : {C::Support_R_ForeArmTwist, C::Support_R_ForeArmTwist1}) {
                const float step = B::AngleBetween(pose[bone].q, prev[bone].q) / kDeg;
                (k < human ? max_step_human : max_step) =
                    std::max(k < human ? max_step_human : max_step, step);
            }
        }
        max_twist = std::max(max_twist, B::AngleBetween(pose[C::Support_R_ForeArmTwist].q,
                                                        ref[C::Support_R_ForeArmTwist].q) /
                                            kDeg);
        max_err = std::max(max_err, EffectorError(pose, C::Right, *in.target[C::Right]).first);
        prev = pose;
    }
    std::printf("  roll -150..150..-150: twist helper step %.2f deg per degree of roll; on round "
                "to 300: %.2f a frame; largest twist %.1f deg; hand %.3f mm\n",
                max_step_human, max_step, max_twist, max_err * 1000.0f);
    EXPECT_LT(max_step_human, 4.0f);
    EXPECT_LE(max_step, 12.01f); // 720 degrees/s at 60 Hz
    EXPECT_LE(max_twist, 150.5f);
    EXPECT_GT(max_twist, 120.0f);
    EXPECT_LT(max_err, 0.001f);
}

TEST(NaturalArms, StandingAndCombatPosesAsBefore) {
    C0000_NEEDS_GAME_SKELETON(); // tuned on the hunter's chest, arms and hand frames
    // Poses that already looked right (the mock's rest, still, guard, blade level, reach, wrist
    // and the swing through its arc, the gun held out, the human guard and vial) keep their elbows
    // within a few centimetres of the old arms' (3 on average, 5 at most), and their hands
    // exactly.
    std::vector<std::array<std::optional<B::ModelTarget>, 2>> targets;
    for (const P::Preset& preset : P::Presets) {
        const std::string_view n = preset.name;
        if (n == "rest" || n == "still" || n == "guard" || n == "blade_level" ||
            n == "reach_fwd" || n == "wrist_inject" || n == "swing") {
            for (const double t : {0.1, 0.3, 0.5, 0.7, 0.9}) {
                std::array<std::optional<B::ModelTarget>, 2> x{};
                x[C::Right] = PresetTarget(preset, C::Right, t);
                x[C::Left] = PresetTarget(preset, C::Left, t);
                targets.push_back(x);
            }
        }
    }
    {
        std::array<std::optional<B::ModelTarget>, 2> gun{};
        gun[C::Right] = GripTarget({0.15f, 1.45f, -0.60f}, 0.0f, 0.0f, 0.0f, C::Right);
        gun[C::Left] = GripTarget({-0.15f, 1.40f, -0.55f}, 0.0f, 0.0f, 0.0f, C::Left);
        targets.push_back(gun);
    }
    for (const char* name : {"guard", "vial"}) {
        for (const GripPair& g : HumanGrips()) {
            if (std::string_view{g.name} == name) {
                targets.push_back(TargetsOf(g));
            }
        }
    }
    float worst = 0.0f, sum = 0.0f;
    int n = 0;
    for (const auto& t : targets) {
        Pose a{}, b{};
        B::BodySolver sa, sb;
        HoldArms(sa, a, t, false, 60);
        HoldArms(sb, b, t, true, 60);
        const Pose ma = Fk(a), mb = Fk(b);
        for (const int side : {C::Left, C::Right}) {
            const float d = Dist(ma[static_cast<size_t>(ForearmOf(side))].t,
                                 mb[static_cast<size_t>(ForearmOf(side))].t);
            worst = std::max(worst, d);
            sum += d;
            ++n;
            EXPECT_LT(EffectorError(b, side, *t[static_cast<size_t>(side)]).first, 0.001f);
        }
    }
    std::printf("  %d arms: the natural elbow %.1f cm from the old one on average, %.1f at most\n",
                n, sum / static_cast<float>(n) * 100.0f, worst * 100.0f);
    // The old roll term turned the elbow from the first degree of roll (0.35 x roll): that part
    // is gone, the rest is as it was.
    EXPECT_LT(worst, 0.05f);
    EXPECT_LT(sum / static_cast<float>(n), 0.03f);
}

TEST(NaturalArms, SwitchOffIsTheOldArms) {
    // natural_arms false: the old pole, roll term and helpers (the arm_ik defaults), bit for bit
    // the arms a caller of SolveArm with HunterArmParams() gets.
    const Pose ref = Reference();
    for (const GripPair& g : HumanGrips()) {
        const auto t = TargetsOf(g);
        Pose a{};
        B::BodySolver solver;
        const B::BodyOutput out = HoldArms(solver, a, t, false, 1);
        ASSERT_TRUE(out.ok);
        Pose b = ref;
        const Pose model = Fk(ref);
        for (const int side : {C::Left, C::Right}) {
            const B::ArmRig rig = side == C::Right ? B::HunterRightArm() : B::HunterLeftArm();
            const Vec3 p = t[static_cast<size_t>(side)]->p;
            B::ArmParams params = B::HunterArmParams();
            params.pole = B::ArmPole(B::BodyFrame{}, rig.side, p - model[rig.upperArm].t,
                                     p - model[C::Spine2].t);
            params.clavicleCatchUp = B::BodyParams{}.shoulder_reach;
            B::ArmState state{};
            B::SolveArm(b.data(), C::Parents.data(), C::BoneCount, rig,
                        {p, t[static_cast<size_t>(side)]->q, 1.0f}, params, state, 1.0f / 90.0f);
        }
        for (int i = 0; i < C::BoneCount; ++i) {
            EXPECT_EQ(std::memcmp(&a[i], &b[i], sizeof(QsTransform)), 0) << g.name << " " << i;
        }
    }
}

TEST(BodySolver, ReturnToTrackedHandIgnoresLaterAnimationSnaps) {
    // The game's recovery clip changes to idle midway through the 300 ms return.
    // Both runs have the same last visible pose and identical live targets.
    // That later source change must not kick the returning arm off its path.
    B::BodySolver continuous, snapped;
    B::BodyParams params{};
    B::BodyInput in{};
    const Pose rest = Reference();
    const Pose model = Fk(rest);
    in.dt = 1.f / 90.f;
    in.body_weight = 0;
    in.hide = false;
    in.target[C::Right] = B::ModelTarget{model[C::R_Weapon].t, model[C::R_Weapon].q};
    for (int frame = 0; frame <= 30; ++frame) {
        Pose a = rest, b = rest;
        const int upper = B::HunterRightArm().upperArm;
        a[upper].q = B::Normalize(B::AxisAngle({0, 1, 0}, .6f) * a[upper].q);
        b[upper].q = frame < 7 ? a[upper].q : rest[upper].q;
        in.arm_weight[C::Right] = B::EaseInSine(std::min(frame / 27.f, 1.f));
        ASSERT_TRUE(continuous.Solve(a.data(), in, params).ok);
        ASSERT_TRUE(snapped.Solve(b.data(), in, params).ok);
        if (frame >= 7) {
            const auto ma = Fk(a), mb = Fk(b);
            EXPECT_LT(Dist(ma[C::R_Hand].t, mb[C::R_Hand].t), .0001f) << frame;
        }
    }
}

TEST(BodySolver, CompletedReturnTracksMovingControllerWithoutAdditionalLag) {
    B::BodySolver solver;
    B::BodyParams params{};
    B::BodyInput in{};
    in.dt = 1.f / 90.f;
    in.hide = false;
    in.body_weight = 0;
    const Pose rest = Reference(), model = Fk(rest);
    const B::ModelTarget initial{model[C::R_Weapon].t, model[C::R_Weapon].q};
    in.target[C::Right] = initial;
    auto pose = rest;
    ASSERT_TRUE(solver.Solve(pose.data(), in, params).ok);
    for (int frame = 1; frame <= 40; ++frame) {
        pose = rest;
        in.arm_weight[C::Right] = B::EaseInSine(std::min(frame / 27.f, 1.f));
        in.target[C::Right]->p = initial.p + Vec3{0, 0, -.001f * frame};
        const auto out = solver.Solve(pose.data(), in, params);
        ASSERT_TRUE(out.ok);
        if (frame >= 27)
            EXPECT_LT(Dist(Fk(pose)[C::R_Weapon].t, in.target[C::Right]->p), .002f);
    }
}
TEST(BodySolver, ResetDiscardsThePreviousReturnPose) {
    B::BodySolver reused, fresh;
    B::BodyParams params{};
    B::BodyInput in{};
    auto pose = Reference();
    const auto model = Fk(pose);
    in.dt = 1.f / 90.f;
    in.hide = false;
    in.body_weight = 0;
    in.target[C::Right] = B::ModelTarget{model[C::R_Weapon].t, model[C::R_Weapon].q};
    ASSERT_TRUE(reused.Solve(pose.data(), in, params).ok);
    in.arm_weight[C::Right] = .2f;
    pose = Reference();
    ASSERT_TRUE(reused.Solve(pose.data(), in, params).ok);
    reused.Reset();
    auto a = Reference(), b = a;
    ASSERT_TRUE(reused.Solve(a.data(), in, params).ok);
    ASSERT_TRUE(fresh.Solve(b.data(), in, params).ok);
    EXPECT_LT(Dist(Fk(a)[C::R_Hand].t, Fk(b)[C::R_Hand].t), .00001f);
}

TEST(BodySolver, SupportHandFollowsTheVisibleWeaponDuringArmBlend) {
    const Pose ref = Reference();
    const Pose model = Fk(ref);
    B::BodyInput in = DefaultInput(ref);
    in.dt = 0.0f;
    in.arm_weight = {1.0f, 0.5f};
    in.two_hand = 1.0f;
    const auto& native = model[C::R_Weapon];
    const Vec3 requested = (model[C::L_UpperArm].t + model[C::R_UpperArm].t) * 0.5f +
                           Vec3{0.0f, -0.20f, -0.30f};
    in.target[C::Right] = B::ModelTarget{requested, native.q};
    in.target[C::Left] = B::ModelTarget{requested + Vec3{0.12f, 0, 0}, native.q};
    B::BodyParams params{};
    params.torso_follow = false;
    params.natural_arms = false;
    Pose pose = ref;
    B::BodySolver solver;
    const auto out = solver.Solve(pose.data(), in, params);
    ASSERT_TRUE(out.ok);
    ASSERT_EQ(out.arm[C::Right].status, B::ArmStatus::Solved);
    ASSERT_EQ(out.arm[C::Left].status, B::ArmStatus::Solved);
    const auto actual = Fk(pose);
    const auto& r = actual[C::R_Weapon];
    const auto& l = actual[C::L_Weapon];
    const Quat turn = B::Normalize(r.q * B::Conjugate(native.q));
    const Vec3 expected = r.t + B::Rotate(turn, {0.12f, 0, 0});
    EXPECT_GT(Dist(r.t, requested), 0.03f); // meaningful right-hand blend displacement
    EXPECT_LT(Dist(l.t, expected), 0.002f);
    EXPECT_LT(B::AngleBetween(l.q, r.q), 0.002f);
}
