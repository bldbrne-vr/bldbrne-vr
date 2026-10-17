// SPDX-License-Identifier: GPL-2.0-or-later

// Your height (core/vr/player_body.h; the VR page's General > Body rows "Height" and "Adjust
// height"): the eye offset (As the hunter: the adjustment alone, + 0 = the camera before the height
// rows bit for bit; As the player: your eye over the headset's floor, 1:1, wherever the origin is;
// every cm of Adjust height a cm of eye, monotonic; no floor or seated: as the hunter), vr.ini
// (height_base, height_adjust_cm clamped to -10..+10, the older game_height_cm / player_height_cm
// ignored: the user's file comes up As the hunter, 0 cm), the body scale k of an eye offset, the
// body adapting (his stance below his eye, his arms' stretch above it, hands on the controllers in
// five poses As the hunter -10 / 0 / +10 and As the player at 150..190 cm), the arms k times as
// long in the IK (arm_ik.h lengthScale: 1 is bit for bit the old solve), the belt's draw zone
// scaled (belt_vial.h BodyScaled) and the page's two rows (vr_page.h: labels, help lines, values,
// toggling, the signed adjustment, live while held, Defaults).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "c0000_test_skeleton.h"

#include "core/vr/arm_ik.h"
#include "core/vr/belt_vial.h"
#include "core/vr/body_solver.h"
#include "core/vr/c0000_skeleton.h"
#include "core/vr/head_room.h"
#include "core/vr/player_body.h"
#include "core/vr/vr_page.h"

namespace {

namespace PB = VR::PlayerBody;
namespace B = VR::Body;
namespace C = VR::Body::C0000;
namespace Page = VR::Page;
using B::QsTransform;
using B::Vec3;
using PB::HeightBase;

using Pose = std::array<QsTransform, C::BoneCount>;

constexpr float Fp = 1.58f; // the user's fp_eye_height_m (the hunter's first-person eye)

Pose Reference() {
    Pose pose{};
    for (size_t i = 0; i < pose.size(); ++i) {
        const C::Transform& t = C::Test::TestSkeleton().local[i];
        pose[i] = {{t.t[0], t.t[1], t.t[2]},
                   B::Normalize(B::Quat{t.q[0], t.q[1], t.q[2], t.q[3]}),
                   {t.s[0], t.s[1], t.s[2]}};
    }
    return pose;
}

Vec3 ModelPos(const Pose& local, int bone) {
    return B::ModelOf(local.data(), C::Parents.data(), C::BoneCount, bone)->t;
}

bool BitEqual(float a, float b) {
    return std::memcmp(&a, &b, sizeof(float)) == 0;
}

// The camera tick's vertical composition (core/vr/vr_session.cpp OnCameraTick, positional, the
// head inside the room): the render offset's y is the head's height from the 6DoF origin, plus the
// eye offset when it is not 0, times world_scale; the camera's eye over his feet is fp plus that.
float CameraOffsetY(float head_y, float origin_y, float eye_offset, float world_scale) {
    float y = 0.0f;
    y += (head_y - origin_y) * world_scale;
    if (eye_offset != 0.0f) {
        y += eye_offset * world_scale;
    }
    return y;
}
// ... and the camera before the height rows (2026-10-09 02:54; "game height not set"): the head's
// height from the origin, nothing else.
float CameraOffsetYBefore(float head_y, float origin_y, float world_scale) {
    float y = 0.0f;
    y += (head_y - origin_y) * world_scale;
    return y;
}

// ---- The eye offset -----------------------------------------------------------------------------

TEST(PlayerBodyHeight, AsTheHunterZeroIsTheCameraBeforePlusTheLift) {
    // As the hunter + 0 cm: exactly HunterEyeLiftM (2 cm, the user's 2026-10-13 base) whatever
    // the floor, the origin or the stance, so the camera tick adds just that and the floor guard
    // and the late warp's soles use fp plus it.
    for (const bool floor : {false, true}) {
        for (const bool seated : {false, true}) {
            for (const float over_floor : {1.40f, 1.567f, 1.58f, 1.80f, std::nanf("")}) {
                for (const float fp : {1.58f, 1.55f, 1.6f, 1.2f}) {
                    const float off =
                        PB::EyeOffsetM(HeightBase::Hunter, 0, fp, floor, over_floor, seated);
                    EXPECT_TRUE(BitEqual(off, PB::HunterEyeLiftM))
                        << floor << seated << over_floor << fp;
                    EXPECT_NEAR(PB::StandingEyeM(off, fp), fp + 0.02f, 1e-6f);
                }
            }
        }
    }
    EXPECT_EQ(PB::BodyScaleFor(0.0f), 1.0f);
    EXPECT_EQ(PB::StandDrop(0.0f), 0.0f);
    EXPECT_EQ(PB::RiseStretch(0.0f, 1.0f), 0.0f);
    // The camera for the same head pose (the head in tracking space, the origin the recenter's):
    // the one before the height rows plus the lift, at every world scale.
    for (const float ws : {1.0f, 0.97f, 1.3f}) {
        for (const float origin : {-0.016f, 0.0f, 0.052f}) {
            for (const float head : {-0.6f, -0.3f, -0.016f, 0.0f, 0.004f, 0.2f}) {
                const float off = PB::EyeOffsetM(HeightBase::Hunter, 0, Fp, true, 1.58f, false);
                EXPECT_NEAR(CameraOffsetY(head, origin, off, ws),
                            CameraOffsetYBefore(head, origin, ws) + PB::HunterEyeLiftM * ws, 1e-6f)
                    << ws << " " << origin << " " << head;
            }
        }
    }
}

TEST(PlayerBodyHeight, EveryCentimetreOfAdjustIsACentimetreOfEye) {
    // As the hunter: the eye the lift plus exactly the adjustment higher (+5 = 5 cm), monotonic.
    float last = -1.0f;
    for (int cm = PB::AdjustMinCm; cm <= PB::AdjustMaxCm; ++cm) {
        const float off = PB::EyeOffsetM(HeightBase::Hunter, cm, Fp, true, 1.5912f, false);
        EXPECT_NEAR(off, PB::HunterEyeLiftM + static_cast<float>(cm) * 0.01f, 1e-6f) << cm;
        EXPECT_NEAR(PB::StandingEyeM(off, Fp) - Fp, 0.02f + cm * 0.01f, 1e-6f) << cm;
        EXPECT_GT(off, last) << cm;
        last = off;
    }
    const auto hunter_eye = [](int cm) {
        return PB::StandingEyeM(PB::EyeOffsetM(HeightBase::Hunter, cm, Fp, false, 0.0f, false), Fp);
    };
    EXPECT_NEAR(hunter_eye(0), 1.60f, 1e-6f); // fp 1.58 + the 2 cm lift
    EXPECT_NEAR(hunter_eye(5), 1.65f, 1e-6f);
    EXPECT_NEAR(hunter_eye(-10), 1.50f, 1e-6f);
    EXPECT_NEAR(hunter_eye(10), 1.70f, 1e-6f);
    // As the player: your eye over the floor plus the adjustment, monotonic, a cm a cm.
    for (const float eye : {1.404f, 1.5122f, 1.567f, 1.657f, 1.778f}) {
        float prev = -1.0f;
        for (int cm = PB::AdjustMinCm; cm <= PB::AdjustMaxCm; ++cm) {
            const float off = PB::EyeOffsetM(HeightBase::Player, cm, Fp, true, eye, false);
            EXPECT_NEAR(PB::StandingEyeM(off, Fp), eye + cm * 0.01f, 2e-6f) << eye << " " << cm;
            EXPECT_GT(off, prev) << eye << " " << cm;
            prev = off;
        }
    }
    // The user: their Quest's floor about 9 cm low (a 177 cm person's eye 1.657 m read 1.567 m):
    // As the player + 9 is their real eye.
    const float user = PB::EyeOffsetM(HeightBase::Player, 9, Fp, true, 1.567f, false);
    EXPECT_NEAR(PB::StandingEyeM(user, Fp), 1.657f, 2e-6f);
}

TEST(PlayerBodyHeight, AsThePlayerIsYourEyeOverTheFloorWhereverTheOriginIs) {
    // The camera's eye over his feet = your head's height over the headset's floor (+ the
    // adjustment), standing or ducking, whether the recenter caught you standing straight, ducked
    // or on your toes: the origin only centres the room.
    for (const float origin_over_floor : {1.30f, 1.5912f, 1.66f, 1.75f}) {
        const float floor_y = -1.20f; // the floor in tracking space
        const float origin_y = floor_y + origin_over_floor;
        for (const float head_over_floor : {0.95f, 1.30f, 1.5912f, 1.62f, 1.70f}) {
            for (const int cm : {-10, 0, 4, 10}) {
                const float off =
                    PB::EyeOffsetM(HeightBase::Player, cm, Fp, true, origin_y - floor_y, false);
                const float camera_eye =
                    Fp + CameraOffsetY(floor_y + head_over_floor, origin_y, off, 1.0f);
                EXPECT_NEAR(camera_eye, head_over_floor + cm * 0.01f, 3e-6f)
                    << origin_over_floor << " " << head_over_floor << " " << cm;
            }
        }
    }
    // The mock's player (the head 1.5912 m over its floor, a real 170 cm person): +0.011 m over
    // the hunter's eye.
    EXPECT_NEAR(PB::EyeOffsetM(HeightBase::Player, 0, Fp, true, 1.5912f, false), 0.0112f, 1e-6f);
}

TEST(PlayerBodyHeight, AsThePlayerWithoutAFloorOrSeatedIsTheHunter) {
    for (const int cm : {-10, -3, 0, 7}) {
        const float hunter = PB::EyeOffsetM(HeightBase::Hunter, cm, Fp, true, 1.7f, false);
        // No floor-level space (or not located yet).
        EXPECT_TRUE(
            BitEqual(PB::EyeOffsetM(HeightBase::Player, cm, Fp, false, 1.7f, false), hunter));
        // Stance: Seated: your eye over the floor is not your height.
        EXPECT_TRUE(BitEqual(PB::EyeOffsetM(HeightBase::Player, cm, Fp, true, 1.2f, true), hunter));
        // A non-finite reading.
        EXPECT_TRUE(BitEqual(PB::EyeOffsetM(HeightBase::Player, cm, Fp, true, std::nanf(""), false),
                             hunter));
    }
    EXPECT_TRUE(PB::PlayerInEffect(HeightBase::Player, true, false));
    EXPECT_FALSE(PB::PlayerInEffect(HeightBase::Player, false, false));
    EXPECT_FALSE(PB::PlayerInEffect(HeightBase::Player, true, true));
    EXPECT_FALSE(PB::PlayerInEffect(HeightBase::Hunter, true, false));
}

// ---- vr.ini -------------------------------------------------------------------------------------

// The emulator's parser for the height keys (vr_session.cpp ParseSettingsLine): one line.
PB::HeightKey ParseLine(PB::HeightSetting& h, const std::string& line) {
    const size_t eq = line.find('=');
    return PB::ParseHeightSetting(h, line.substr(0, eq), line.substr(eq + 1));
}

TEST(PlayerBodyHeight, AnOlderVrIniComesUpAsTheHunterZero) {
    // The user's vr.ini after their tries with the two older rows: both lines read and ignored.
    PB::HeightSetting h{};
    for (const char* line : {"body_mode=no_head", "player_height_cm=163", "game_height_cm=190"}) {
        const std::string l{line};
        const auto r = ParseLine(h, l);
        if (l.starts_with("body_mode")) {
            EXPECT_EQ(r, PB::HeightKey::Other);
            EXPECT_FALSE(PB::IsHeightKey("body_mode"));
        } else {
            EXPECT_EQ(r, PB::HeightKey::Legacy) << l;
            EXPECT_TRUE(PB::IsHeightKey(l.substr(0, l.find('='))));
        }
    }
    EXPECT_EQ(h.base, HeightBase::Hunter);
    EXPECT_EQ(h.adjust_cm, 0);
    // ... so the camera is the hunter's (with its 2 cm lift).
    EXPECT_TRUE(
        BitEqual(PB::EyeOffsetM(h.base, h.adjust_cm, Fp, true, 1.527f, false), PB::HunterEyeLiftM));
    // A legacy line after the new keys changes nothing either.
    PB::HeightSetting set{HeightBase::Player, 4};
    EXPECT_EQ(ParseLine(set, "game_height_cm=150"), PB::HeightKey::Legacy);
    EXPECT_EQ(ParseLine(set, "player_height_cm=0"), PB::HeightKey::Legacy);
    EXPECT_EQ(set.base, HeightBase::Player);
    EXPECT_EQ(set.adjust_cm, 4);
}

TEST(PlayerBodyHeight, TheKeysParseAndClamp) {
    PB::HeightSetting h{};
    EXPECT_EQ(ParseLine(h, "height_base=player"), PB::HeightKey::Set);
    EXPECT_EQ(h.base, HeightBase::Player);
    EXPECT_EQ(ParseLine(h, "height_base=hunter"), PB::HeightKey::Set);
    EXPECT_EQ(h.base, HeightBase::Hunter);
    EXPECT_EQ(ParseLine(h, "height_base=tall"), PB::HeightKey::Bad); // unchanged
    EXPECT_EQ(h.base, HeightBase::Hunter);
    EXPECT_EQ(ParseLine(h, "height_adjust_cm=7"), PB::HeightKey::Set);
    EXPECT_EQ(h.adjust_cm, 7);
    EXPECT_EQ(ParseLine(h, "height_adjust_cm=-4"), PB::HeightKey::Set);
    EXPECT_EQ(h.adjust_cm, -4);
    EXPECT_EQ(ParseLine(h, "height_adjust_cm=15"), PB::HeightKey::Set);
    EXPECT_EQ(h.adjust_cm, 10);
    EXPECT_EQ(ParseLine(h, "height_adjust_cm=-12.6"), PB::HeightKey::Set);
    EXPECT_EQ(h.adjust_cm, -10);
    EXPECT_EQ(ParseLine(h, "height_adjust_cm=3.4"), PB::HeightKey::Set);
    EXPECT_EQ(h.adjust_cm, 3);
    EXPECT_EQ(ParseLine(h, "height_adjust_cm=1e9"), PB::HeightKey::Set);
    EXPECT_EQ(h.adjust_cm, 10);
    for (const char* bad : {"height_adjust_cm=", "height_adjust_cm=abc", "height_adjust_cm=4cm",
                            "height_adjust_cm=nan"}) {
        EXPECT_EQ(ParseLine(h, bad), PB::HeightKey::Bad) << bad;
        EXPECT_EQ(h.adjust_cm, 10) << bad;
    }
    EXPECT_EQ(ParseLine(h, "stereo=1"), PB::HeightKey::Other);
    // The clamp and the row's text.
    EXPECT_EQ(PB::ClampAdjustCm(-40), -10);
    EXPECT_EQ(PB::ClampAdjustCm(25), 10);
    EXPECT_EQ(PB::ClampAdjustCm(-3), -3);
    EXPECT_EQ(PB::AdjustM(25), PB::AdjustM(10));
    EXPECT_NEAR(PB::AdjustM(10), 0.10f, 1e-7f);
    EXPECT_EQ(PB::AdjustText(3), "+3 cm");
    EXPECT_EQ(PB::AdjustText(0), "0 cm");
    EXPECT_EQ(PB::AdjustText(-4), "-4 cm");
    EXPECT_EQ(PB::AdjustText(30), "+10 cm");
    EXPECT_STREQ(PB::BaseIni(HeightBase::Hunter), "hunter");
    EXPECT_STREQ(PB::BaseIni(HeightBase::Player), "player");
}

// ---- The body scale -----------------------------------------------------------------------------

TEST(PlayerBody, ScaleOfAStatureAndOfAnEyeOffset) {
    EXPECT_EQ(PB::BodyScale(0), 1.0f);
    EXPECT_EQ(PB::BodyScale(-5), 1.0f);
    EXPECT_EQ(PB::BodyScale(167), 1.0f);
    EXPECT_NEAR(PB::BodyScale(190), 190.0f / 167.0f, 1e-6f);
    EXPECT_EQ(PB::BodyScale(140), PB::ScaleMin); // 0.838 -> 0.90
    EXPECT_EQ(PB::BodyScale(210), PB::ScaleMax); // 1.257 -> 1.15
    // An eye offset: the hunter's stature plus the offset as stature, exactly 1 for 0.
    EXPECT_EQ(PB::BodyScaleFor(0.0f), 1.0f);
    EXPECT_EQ(PB::BodyScaleFor(-0.0f), 1.0f);
    EXPECT_EQ(PB::BodyScaleFor(std::nanf("")), 1.0f);
    EXPECT_NEAR(PB::BodyScaleFor(0.10f), 1.0f + 0.10f / 1.56312f, 1e-5f);
    EXPECT_NEAR(PB::BodyScaleFor(-0.10f), 1.0f - 0.10f / 1.56312f, 1e-5f);
    // A 177 cm player (eye 1.657 m) As the player: about a 175 cm person's arms.
    EXPECT_NEAR(PB::BodyScaleFor(1.657f - Fp), 1.0493f, 1e-3f);
    EXPECT_EQ(PB::BodyScaleFor(-1.0f), PB::ScaleMin);
    EXPECT_EQ(PB::BodyScaleFor(1.0f), PB::ScaleMax);
    float last = 0.0f;
    for (int mm = -200; mm <= 200; mm += 5) {
        const float k = PB::BodyScaleFor(mm * 0.001f);
        EXPECT_GE(k, last) << mm;
        last = k;
    }
    EXPECT_NEAR(PB::StatureCm(0.0f, Fp), 168.8f, 0.05f); // the hunter's eye is a 168.8 cm one's
}

TEST(PlayerBody, StanceAndStretchFromTheEyeOffset) {
    // An eye 10 cm below the hunter's: his stance 10 cm lower, no stretch.
    const float low = -0.10f;
    EXPECT_NEAR(PB::StandDrop(low), 0.10f, 1e-6f);
    EXPECT_EQ(PB::RiseStretch(low, 1.0f), 0.0f);
    // 10 cm above: no stance, the arms may stretch 0.10 / (0.634 k) more.
    const float high = 0.10f;
    EXPECT_EQ(PB::StandDrop(high), 0.0f);
    EXPECT_NEAR(PB::RiseStretch(high, PB::BodyScaleFor(high)),
                0.10f / (PB::HunterArmM * PB::BodyScaleFor(high)), 1e-5f);
    EXPECT_EQ(PB::RiseStretch(5.0f, 1.0f), PB::RiseStretchMax);
    EXPECT_EQ(PB::StandDrop(std::nanf("")), 0.0f);
    EXPECT_EQ(PB::RiseStretch(std::nanf(""), 1.0f), 0.0f);
}

// ---- The arms: lengthScale ----------------------------------------------------------------------

struct ArmCase {
    Pose pose;
    B::ArmResult r;
};

ArmCase SolveRight(Vec3 target_from_shoulder, float scale, bool set_scale) {
    ArmCase c{Reference(), {}};
    const B::ArmRig rig = B::HunterRightArm();
    const Vec3 shoulder = ModelPos(c.pose, rig.upperArm);
    B::ArmParams p = B::HunterArmParams();
    if (set_scale) {
        p.lengthScale = scale;
    }
    B::ArmState st{};
    c.r = B::SolveArm(c.pose.data(), C::Parents.data(), C::BoneCount, rig,
                      {shoulder + target_from_shoulder, std::nullopt, 1.0f}, p, st, 0.0f);
    return c;
}

TEST(PlayerBodyArms, ScaleOneIsTheOldSolveBitForBit) {
    for (const Vec3 t : {Vec3{-0.15f, -0.50f, -0.20f}, Vec3{-0.05f, -0.30f, -0.35f},
                         Vec3{-0.30f, 0.10f, -0.40f}, Vec3{-0.10f, -0.75f, 0.0f}}) {
        const ArmCase a = SolveRight(t, 1.0f, false);
        const ArmCase b = SolveRight(t, 1.0f, true);
        EXPECT_EQ(std::memcmp(a.pose.data(), b.pose.data(), sizeof(Pose)), 0);
        EXPECT_EQ(std::memcmp(&a.r.error, &b.r.error, sizeof(float)), 0);
    }
}

TEST(PlayerBodyArms, ATallPlayersRelaxedHandIsReached) {
    // Straight down, 0.75 m from the shoulder: past the hunter's arm plus its 10% stretch
    // (0.634 x 1.1 = 0.70 m to the grip), within a 190 cm player's (k 1.138).
    const Vec3 hang{-0.05f, -0.75f, 0.02f};
    const ArmCase plain = SolveRight(hang, 1.0f, false);
    const ArmCase tall = SolveRight(hang, PB::BodyScale(190), true);
    ASSERT_EQ(plain.r.status, B::ArmStatus::Solved);
    ASSERT_EQ(tall.r.status, B::ArmStatus::Solved);
    EXPECT_GT(plain.r.error, 0.03f); // detached: the reach tick
    EXPECT_TRUE(B::Detached(plain.r));
    EXPECT_LT(tall.r.error, 0.002f);
    EXPECT_FALSE(B::Detached(tall.r));
    // The written forearm and hand translations are the bones k (times the IK's stretch) as long.
    const Pose ref = Reference();
    const float l1 = B::Length(ref[C::R_Forearm].t), l2 = B::Length(ref[C::R_Hand].t);
    const float k = PB::BodyScale(190);
    EXPECT_NEAR(B::Length(tall.pose[C::R_Forearm].t) / l1, k * tall.r.stretch, 1e-4f);
    EXPECT_NEAR(B::Length(tall.pose[C::R_Hand].t) / l2, k * tall.r.stretch, 1e-4f);
    EXPECT_NEAR(B::Length(tall.pose[C::R_Forearm].t) / B::Length(tall.pose[C::R_Hand].t), l1 / l2,
                1e-4f);
}

TEST(PlayerBodyArms, AShortPlayersHandIsReachedWithAStraighterElbow) {
    // 0.57 m from the shoulder (a 155 cm player's hanging grip): reached either way; the shorter
    // arm bends less, as the player's own straight arm.
    const Vec3 hang{-0.05f, -0.565f, 0.02f};
    const ArmCase plain = SolveRight(hang, 1.0f, false);
    const ArmCase shorter = SolveRight(hang, PB::BodyScale(155), true);
    EXPECT_LT(plain.r.error, 0.002f);
    EXPECT_LT(shorter.r.error, 0.002f);
    EXPECT_LT(shorter.r.flex + 0.2f, plain.r.flex); // at least ~11 degrees straighter
}

TEST(PlayerBodyArms, TheSolverPassesTheArmScale) {
    B::BodySolver plain, tall;
    Pose a = Reference(), b = Reference();
    B::BodyInput in{};
    in.eye_m = Vec3{0.0f, 1.654f, -0.066f};
    in.body_weight = 1.0f;
    const Vec3 shoulder = ModelPos(a, C::R_UpperArm);
    // The hand as it hangs in the bind pose (the weapon bone's own model rotation).
    const B::Quat hanging = B::ModelOf(a.data(), C::Parents.data(), C::BoneCount, C::R_Weapon)->q;
    const auto right = static_cast<size_t>(C::Right);
    in.target[right] = B::ModelTarget{shoulder + Vec3{-0.05f, -0.75f, 0.02f}, hanging};
    in.arm_weight = {0.0f, 1.0f};
    B::BodyParams p{};
    // The arm alone (the shoulder's catch-up for targets out of reach, BodyParams::shoulder_reach,
    // would close part of this gap: tests/test_arm_ik.cpp tests it).
    p.shoulder_reach = 0.0f;
    const auto out_plain = plain.Solve(a.data(), in, p);
    p.arm_scale = PB::BodyScale(190);
    const auto out_tall = tall.Solve(b.data(), in, p);
    ASSERT_TRUE(out_plain.ok);
    ASSERT_TRUE(out_tall.ok);
    EXPECT_GT(out_plain.arm[right].error, 0.03f);
    EXPECT_LT(out_tall.arm[right].error, 0.01f);
}

// ---- The belt's draw zone -----------------------------------------------------------------------

TEST(PlayerBodyBelt, ScaledWithTheBody) {
    const VR::Belt::Zone z = VR::Belt::Standing;
    const VR::Belt::Zone same = VR::Belt::BodyScaled(z, PB::BodyScaleFor(0.0f));
    EXPECT_EQ(std::memcmp(&same, &z, sizeof(z)), 0);
    const float k = PB::BodyScale(190);
    const VR::Belt::Zone tall = VR::Belt::BodyScaled(z, k);
    EXPECT_NEAR(tall.below_max, z.below_max * k, 1e-6f);
    EXPECT_NEAR(tall.out_min, z.out_min * k, 1e-6f);
    // A 190 cm player's hand hanging at the left thigh, 1.12 m below the eyes: outside the
    // unscaled box (1.05 m), inside the scaled one.
    const VR::Belt::P3 head{0.0f, 1.778f, 0.0f};
    const VR::Belt::P3 hand{-0.28f, 1.778f - 1.12f, 0.05f};
    const VR::Belt::P3 forward{0.0f, 0.0f, -1.0f};
    EXPECT_FALSE(VR::Belt::Locate(head, hand, forward, z).inside);
    EXPECT_TRUE(VR::Belt::Locate(head, hand, forward, tall).inside);
}

// ---- The body adapts ----------------------------------------------------------------------------

// The hunter's eye in the bind pose (the solver's own: dummy 58's midpoint on Head, of the
// installed test skeleton).
Vec3 BindEye(const Pose& local) {
    const auto head = B::ModelOf(local.data(), C::Parents.data(), C::BoneCount, C::Head);
    const auto& e = C::Test::TestSkeleton().eye_center_on_head;
    return head->t + B::Rotate(head->q, Vec3{e[0], e[1], e[2]});
}

struct BodyRun {
    B::BodyOutput out;
    float eye_gap{};   // the camera's eye minus his after the solve (+ = the camera above), m
    float max_err{};   // both hands, the worst pose, m
    std::array<float, 5> err{}; // per pose: hang, forward, overhead, side, chest (worst hand)
};

// A standing player `real_cm` tall with the camera's eye `offset` above the hunter's (the eye
// offset of the rows) in the bind pose: the camera's standing eye at fp + offset over his feet (the
// bind pose's eye stands for the animation's), your shoulders and arms a `real_cm` person's around
// it, both hands in five poses (hanging, straight forward, overhead, out to the side, at the
// chest), 2 s of 60 Hz solves so the filters settle. `adapt` false: the body as before the height
// rows (no stance, no stretch, k 1). `body_offset` (NaN: `offset`): the offset the body is fitted to
// (vr_session's HeightBodyOffsetM) when it is not the camera's. `short_stretch` false: without
// ShortStretch (the build before it). `reach_frac`: the hands this far out along your arm.
BodyRun RunBody(int real_cm, float offset, bool adapt, float body_offset = std::nanf(""),
                bool short_stretch = true, float reach_frac = 0.97f) {
    const Pose ref = Reference();
    const Vec3 eye0 = BindEye(ref);
    const Vec3 cam{eye0.x, Fp + offset, eye0.z};
    const float r = static_cast<float>(real_cm) / 168.8f; // the hunter's body is a 168.8 cm one
    const float reach = PB::HunterArmM * static_cast<float>(real_cm) / 167.0f * reach_frac;
    B::BodyParams p{};
    p.torso_follow = false;
    p.align_down = 0.80f;
    if (adapt) {
        const float fit = std::isnan(body_offset) ? offset : body_offset;
        p.arm_scale = PB::BodyScaleFor(fit);
        p.stand_drop = PB::StandDrop(fit);
        p.rise_stretch = PB::RiseStretch(fit, p.arm_scale) +
                         (short_stretch ? PB::ShortStretch(p.arm_scale) : 0.0f);
    }
    const B::Quat hang_q = B::ModelOf(ref.data(), C::Parents.data(), C::BoneCount, C::R_Weapon)->q;
    BodyRun run{};
    // Model right is -X, forward -Z.
    for (int pose_i = 0; pose_i < 5; ++pose_i) {
        B::BodySolver solver;
        B::BodyInput in{};
        in.dt = 1.0f / 60.0f;
        in.eye_m = cam;
        in.body_weight = 1.0f;
        in.arm_weight = {1.0f, 1.0f};
        for (int side = 0; side < 2; ++side) {
            const B::ArmRig rig = side == C::Right ? B::HunterRightArm() : B::HunterLeftArm();
            const float out_x = side == C::Right ? -1.0f : 1.0f;
            // Your shoulder: the hunter's, from his eye, scaled to your body, from the camera.
            const Vec3 shoulder = cam + (ModelPos(ref, rig.upperArm) - eye0) * r;
            Vec3 dir{};
            switch (pose_i) {
            case 0: // hanging, a little forward and out
                dir = B::Normalize(Vec3{0.12f * out_x, -1.0f, -0.12f});
                break;
            case 1: // straight forward
                dir = Vec3{0.0f, 0.0f, -1.0f};
                break;
            case 2: // overhead
                dir = B::Normalize(Vec3{0.15f * out_x, 1.0f, -0.15f});
                break;
            case 3: // out to the side, a little forward and down
                dir = B::Normalize(Vec3{out_x, -0.3f, -0.4f});
                break;
            default: // at the chest, 25 cm in front of the sternum
                break;
            }
            const Vec3 target = pose_i == 4   ? cam + Vec3{0.08f * out_x, -0.36f * r, -0.25f * r}
                                : pose_i == 3 ? shoulder + dir * (reach * 0.95f)
                                              : shoulder + dir * reach;
            in.target[static_cast<size_t>(side)] = B::ModelTarget{target, hang_q};
        }
        B::BodyOutput out{};
        for (int frame = 0; frame < 120; ++frame) {
            Pose pose = ref;
            out = solver.Solve(pose.data(), in, p);
        }
        float worst = 0.0f;
        for (int side = 0; side < 2; ++side) {
            const B::ArmResult& a = out.arm[static_cast<size_t>(side)];
            worst = std::max(worst, a.status == B::ArmStatus::Solved ? a.error : 1.0f);
        }
        run.err[static_cast<size_t>(pose_i)] = worst;
        run.max_err = std::max(run.max_err, worst);
        if (pose_i == 0) {
            run.out = out;
            run.eye_gap = out.cam_above;
        }
    }
    return run;
}

void PrintRun(const char* what, const BodyRun& run, float offset) {
    std::printf("%-26s eye %+6.1f cm  k %.3f  camera-eye vs his %+5.1f cm  shift %+6.1f cm  duck "
                "%.2f  hands mm %5.1f %5.1f %5.1f %5.1f %5.1f\n",
                what, offset * 100.0f, PB::BodyScaleFor(offset), run.eye_gap * 100.0f,
                run.out.shift.y * 100.0f, run.out.duck, run.err[0] * 1000.0f, run.err[1] * 1000.0f,
                run.err[2] * 1000.0f, run.err[3] * 1000.0f, run.err[4] * 1000.0f);
}

TEST(PlayerBodyAdapts, AsTheHunterMinusTenToPlusTen) {
    // A real 170 cm player As the hunter at -10 / 0 / +10 cm: the hands on the controllers in every
    // pose; below his eye his stance comes down by all of it (his eye keeps its relation to the
    // camera, no duck posture), above it his arms reach the overhead hands.
    const BodyRun zero = RunBody(170, 0.0f, true);
    PrintRun("hunter +0 (real 170)", zero, 0.0f);
    ASSERT_TRUE(zero.out.ok);
    EXPECT_LT(zero.max_err, 0.005f);
    for (const int cm : {-10, -5, 5, 10}) {
        const float off = PB::EyeOffsetM(HeightBase::Hunter, cm, Fp, false, 0.0f, false);
        const BodyRun run = RunBody(170, off, true);
        PrintRun(("hunter " + PB::AdjustText(cm) + " (real 170)").c_str(), run, off);
        ASSERT_TRUE(run.out.ok) << cm;
        EXPECT_LT(run.max_err, 0.005f) << cm;
        if (cm < 0) {
            EXPECT_NEAR(run.eye_gap, zero.eye_gap, 0.002f) << cm;
            EXPECT_NEAR(run.out.duck, zero.out.duck, 1e-3f) << cm;
            EXPECT_NEAR(run.out.shift.y, zero.out.shift.y - PB::StandDrop(off), 0.002f) << cm;
        }
    }
    // Before (no stance): -10 left a partial duck standing (the deadband fading).
    EXPECT_GT(RunBody(170, -0.10f, false).out.duck, 0.05f);
}

TEST(PlayerBodyAdapts, AsThePlayerAt150To190) {
    // As the player with a true floor: the camera's eye is your own (height x 0.936); his body
    // follows (shorter: his stance comes down; taller: his arms k as long and stretching up).
    const BodyRun zero = RunBody(170, 0.0f, true);
    for (const int real : {150, 160, 170, 177, 180, 190}) {
        const float eye = static_cast<float>(real) * 0.01f * PB::EyeToStature;
        const float off = PB::EyeOffsetM(HeightBase::Player, 0, Fp, true, eye, false);
        const BodyRun run = RunBody(real, off, true);
        PrintRun(("player, real " + std::to_string(real)).c_str(), run, off);
        ASSERT_TRUE(run.out.ok) << real;
        EXPECT_LT(run.max_err, 0.005f) << real;
        if (off < 0.0f) {
            EXPECT_NEAR(run.eye_gap, zero.eye_gap, 0.002f) << real;
            EXPECT_NEAR(run.out.shift.y, zero.out.shift.y - PB::StandDrop(off), 0.002f) << real;
        }
    }
    // A 190 cm player's overhead hands: reached (the arms k as long and the stretch).
    const float off190 = PB::EyeOffsetM(HeightBase::Player, 0, Fp, true, 1.9f * PB::EyeToStature,
                                        false);
    const BodyRun with = RunBody(190, off190, true);
    const BodyRun without = RunBody(190, off190, false);
    std::printf("player 190: overhead %.1f mm adapted, %.1f mm with the body before the rows\n",
                with.err[2] * 1000.0f, without.err[2] * 1000.0f);
    EXPECT_LT(with.err[2], 0.005f);
    EXPECT_GT(without.err[2], 0.02f);
}

TEST(PlayerBodyAdapts, ShorterArmsStillReachYourHandsOutOfTheirReach) {
    // A tall player's real arms with the camera's eye below the hunter's (As the hunter -5 / -10,
    // As the player with a floor read low): k < 1 shortens his arms, and a hand out at the real
    // arm's full length (overhead, forward, to the side) is still reached: ShortStretch lets them
    // out to his own full reach. Relaxed hands keep the shorter arm (no stretch).
    EXPECT_EQ(PB::ShortStretch(1.0f), 0.0f);
    EXPECT_EQ(PB::ShortStretch(1.1f), 0.0f);
    EXPECT_EQ(PB::ShortStretch(0.0f), 0.0f);
    EXPECT_EQ(PB::ShortStretch(std::nanf("")), 0.0f);
    for (const float k : {0.90f, 0.936f, 0.97f}) {
        // His shorter arm with the IK's stretch and this one reaches exactly his own full reach.
        EXPECT_NEAR(k * (1.0f + PB::IkStretch + PB::ShortStretch(k)), 1.0f + PB::IkStretch, 1e-6f);
    }
    for (const int real : {177, 185}) {
        for (const float off : {-0.05f, -0.10f}) {
            const BodyRun with = RunBody(real, off, true, std::nanf(""), true, 1.0f);
            const BodyRun without = RunBody(real, off, true, std::nanf(""), false, 1.0f);
            const BodyRun own = RunBody(real, 0.0f, true, std::nanf(""), true, 1.0f);
            PrintRun(("real " + std::to_string(real) + ", eye " +
                      std::to_string(static_cast<int>(std::lround(off * 100.0f))) + " cm")
                         .c_str(),
                     with, off);
            PrintRun("  ... without ShortStretch", without, off);
            ASSERT_TRUE(with.out.ok);
            // At least as good as at his own height (k 1) in every pose, and every pose within 5 mm
            // where k 1 reaches it.
            for (size_t i = 0; i < with.err.size(); ++i) {
                EXPECT_LE(with.err[i], std::max(own.err[i], 0.005f) + 1e-4f) << real << " " << off;
            }
            // A relaxed hand within the shorter arm's reach: nothing stretches, the arm bit for bit
            // the one without ShortStretch (the shorter look stays).
            const BodyRun relaxed = RunBody(real, off, true, std::nanf(""), true, 0.70f);
            const BodyRun relaxed0 = RunBody(real, off, true, std::nanf(""), false, 0.70f);
            for (int side = 0; side < 2; ++side) {
                const B::ArmResult& r = relaxed.out.arm[static_cast<size_t>(side)];
                const B::ArmResult& r0 = relaxed0.out.arm[static_cast<size_t>(side)];
                EXPECT_EQ(r.stretch, 1.0f) << real << " " << off << " " << side;
                EXPECT_TRUE(BitEqual(r.elbow.x, r0.elbow.x) && BitEqual(r.elbow.y, r0.elbow.y) &&
                            BitEqual(r.elbow.z, r0.elbow.z))
                    << real << " " << off << " " << side;
            }
        }
    }
}

TEST(PlayerBodyHeight, TheBodysOffsetIsTheEyeOffsetExceptAsThePlayerAboveTheOrigin) {
    // The body's offset (vr_session HeightBodyOffsetM): As the hunter (and As the player without a
    // floor or seated) the eye offset bit for bit wherever the head is; As the player your head
    // above the origin adds to it, at or below the origin nothing does.
    const float inf = std::numeric_limits<float>::infinity();
    for (const float off : {-0.1f, -0.013f, 0.0f, 0.0112f, 0.1f}) {
        for (const float head : {-0.5f, -0.01f, 0.0f, 0.003f, 0.25f, 0.5f, std::nanf(""), inf}) {
            EXPECT_TRUE(BitEqual(PB::BodyOffsetM(off, false, head), off)) << off << " " << head;
            if (!(head > 0.0f) || !std::isfinite(head)) {
                EXPECT_TRUE(BitEqual(PB::BodyOffsetM(off, true, head), off)) << off << " " << head;
            } else {
                EXPECT_EQ(PB::BodyOffsetM(off, true, head), off + head) << off << " " << head;
            }
        }
    }
    // As the hunter + 0: exactly +0.0f, so the body gets no stance, no stretch and k 1.
    EXPECT_TRUE(BitEqual(PB::BodyOffsetM(0.0f, false, 0.25f), 0.0f));
}

TEST(PlayerBodyAdapts, AsThePlayerARecenterTakenBentThenStandingUp) {
    // As the player, a recenter (or the camera's takeover) taken while you were bent 10 / 25 cm,
    // then you stand up: the camera's eye is your real eye over the floor either way; the body fits
    // him to your standing eye again (the same stance, eye gap, arms and hands as after a recenter
    // taken standing), where the eye offset alone left his stance as low and his arms as short as
    // the bent height.
    for (const int real : {160, 177}) {
        const float eye = static_cast<float>(real) * 0.01f * PB::EyeToStature;
        const float off_std = PB::EyeOffsetM(HeightBase::Player, 0, Fp, true, eye, false);
        const BodyRun standing = RunBody(real, off_std, true);
        PrintRun(("player " + std::to_string(real) + ", standing recenter").c_str(), standing,
                 off_std);
        for (const float bent : {0.10f, 0.25f}) {
            const float off_bent =
                PB::EyeOffsetM(HeightBase::Player, 0, Fp, true, eye - bent, false);
            // Standing up: your head `bent` above the origin; the camera's eye is your eye.
            const float floor_y = -1.3f, origin_y = floor_y + eye - bent;
            const float camera = Fp + CameraOffsetY(origin_y + bent, origin_y, off_bent, 1.0f);
            EXPECT_NEAR(camera, eye, 3e-6f) << real << " " << bent;
            const float fit = PB::BodyOffsetM(off_bent, true, bent);
            EXPECT_NEAR(fit, off_std, 3e-6f) << real << " " << bent;
            const BodyRun now = RunBody(real, camera - Fp, true, fit);
            const BodyRun before = RunBody(real, camera - Fp, true, off_bent);
            PrintRun(("  bent " + std::to_string(static_cast<int>(bent * 100.0f)) +
                      " cm, then standing")
                         .c_str(),
                     now, fit);
            PrintRun("  ... the eye offset alone", before, off_bent);
            ASSERT_TRUE(now.out.ok);
            EXPECT_LT(now.max_err, 0.005f) << real << " " << bent;
            EXPECT_NEAR(now.eye_gap, standing.eye_gap, 0.002f) << real << " " << bent;
            EXPECT_NEAR(now.out.shift.y, standing.out.shift.y, 0.002f) << real << " " << bent;
            if (bent > 0.2f) {
                EXPECT_GT(std::abs(before.eye_gap - standing.eye_gap), 0.03f) << real;
            }
        }
    }
}

TEST(PlayerBodyAdapts, ZeroStanceAndStretchAreTheOldSolveBitForBit) {
    const Pose ref = Reference();
    B::BodyInput in{};
    in.dt = 1.0f / 60.0f;
    in.eye_m = BindEye(ref) + Vec3{0.02f, -0.30f, -0.12f};
    in.body_weight = 1.0f;
    in.arm_weight = {1.0f, 1.0f};
    const B::Quat q = B::ModelOf(ref.data(), C::Parents.data(), C::BoneCount, C::R_Weapon)->q;
    in.target[C::Right] = B::ModelTarget{Vec3{-0.25f, 1.10f, -0.45f}, q};
    in.target[C::Left] = B::ModelTarget{Vec3{0.30f, 1.60f, -0.30f}, q};
    B::BodyParams a{}, b{};
    // As the hunter + 0 cm: what the body code sets.
    b.arm_scale = PB::BodyScaleFor(0.0f);
    b.stand_drop = PB::StandDrop(0.0f);
    b.rise_stretch = PB::RiseStretch(0.0f, b.arm_scale);
    B::BodySolver sa, sb;
    for (int frame = 0; frame < 30; ++frame) {
        Pose pa = ref, pb = ref;
        const B::BodyOutput oa = sa.Solve(pa.data(), in, a);
        const B::BodyOutput ob = sb.Solve(pb.data(), in, b);
        ASSERT_TRUE(oa.ok && ob.ok);
        ASSERT_EQ(std::memcmp(pa.data(), pb.data(), sizeof(Pose)), 0) << frame;
        ASSERT_EQ(std::memcmp(&oa.shift, &ob.shift, sizeof(Vec3)), 0) << frame;
    }
}

TEST(PlayerBodyAdapts, ShiftPostureDucksAsDeepBelowHisEye) {
    // body_posture=shift (UpperPos follows the eye in full): a 150 cm player As the player puts the
    // camera's standing eye StandDrop below his; the camera's deepest duck (align_down 0.80 under
    // that) is reached, not cut 0.80 under his own eye.
    const Pose ref = Reference();
    const float stand =
        PB::StandDrop(PB::EyeOffsetM(HeightBase::Player, 0, Fp, true, 1.404f, false));
    ASSERT_GT(stand, 0.17f);
    const auto run = [&](float stand_drop) {
        B::BodyInput in{};
        in.dt = 1.0f / 60.0f;
        in.eye_m = BindEye(ref) - Vec3{0.0f, stand + 0.80f, 0.0f};
        in.body_weight = 1.0f;
        B::BodyParams p{};
        p.posture = B::PostureMode::Shift;
        p.torso_follow = false;
        p.align_down = 0.80f;
        p.stand_drop = stand_drop;
        B::BodySolver solver;
        B::BodyOutput out{};
        for (int frame = 0; frame < 120; ++frame) {
            Pose pose = ref;
            out = solver.Solve(pose.data(), in, p);
        }
        return out;
    };
    const B::BodyOutput before = run(0.0f);
    const B::BodyOutput after = run(stand);
    ASSERT_TRUE(before.ok && after.ok);
    EXPECT_NEAR(before.shift.y, -0.80f, 0.002f);
    EXPECT_NEAR(after.shift.y, -(stand + 0.80f), 0.002f);
}

// ---- The page -----------------------------------------------------------------------------------

// The emulator's store in miniature: the values of the two keys as the settings hold them (parsed
// like vr.ini lines, vr_page_runtime.cpp ValueIn), the rest a map.
struct Store : Page::Store {
    PB::HeightSetting h{};
    std::map<std::string, std::string> v{{"seated", "0"}};
    std::vector<std::string> removed;
    std::vector<std::string> sets;
    std::string Get(const std::string& k) override {
        if (k == "height_base") {
            return PB::BaseIni(h.base);
        }
        if (k == "height_adjust_cm") {
            return std::to_string(h.adjust_cm);
        }
        const auto it = v.find(k);
        return it != v.end() ? it->second : std::string{"0"};
    }
    void Set(const std::string& k, const std::string& value) override {
        sets.push_back(k + "=" + value);
        if (PB::ParseHeightSetting(h, k, value) == PB::HeightKey::Other) {
            v[k] = value;
        }
    }
    void Remove(const std::string& k) override {
        if (k == "height_base") {
            h.base = HeightBase::Hunter;
        } else if (k == "height_adjust_cm") {
            h.adjust_cm = 0;
        }
        v.erase(k);
        removed.push_back(k);
    }
};

const Page::Row& RowById(const char* id) {
    return Page::Rows()[static_cast<size_t>(Page::RowIndex(id))];
}

TEST(PlayerBodyPage, TheTwoRowsOfGeneral) {
    // General > Body: Height then Adjust height; Stance is on Controls (slot 4).
    const Page::Tab& general = Page::Tabs()[0];
    EXPECT_STREQ(general.headers[0], "Body");
    EXPECT_EQ(general.rows[0], Page::RowIndex("height"));
    EXPECT_EQ(general.rows[1], Page::RowIndex("height_adjust"));
    EXPECT_EQ(general.rows[2], Page::RowIndex("move_with"));
    EXPECT_EQ(Page::Tabs()[1].rows[4], Page::RowIndex("stance"));
    const Page::Row& height = RowById("height");
    EXPECT_STREQ(height.label, "Height");
    EXPECT_STREQ(height.help,
                 "As the hunter: his eye height. As the player: your real height (from the "
                 "headset's floor).");
    EXPECT_EQ(height.widget, Page::Widget::Choice);
    EXPECT_STREQ(height.keys[0], "height_base");
    ASSERT_EQ(height.options.size(), 2u);
    EXPECT_STREQ(height.options[0].text, "As the hunter");
    EXPECT_STREQ(height.options[0].values[0], "hunter");
    EXPECT_STREQ(height.options[1].text, "As the player");
    EXPECT_STREQ(height.options[1].values[0], "player");
    EXPECT_FALSE(height.restart || height.placeholder);
    const Page::Row& adjust = RowById("height_adjust");
    EXPECT_STREQ(adjust.label, "Adjust height");
    EXPECT_STREQ(adjust.help,
                 "Taller or shorter, from the choice above. Also corrects a floor that is a few cm "
                 "off.");
    EXPECT_EQ(adjust.widget, Page::Widget::Number);
    EXPECT_STREQ(adjust.keys[0], "height_adjust_cm");
    EXPECT_EQ(adjust.min, PB::AdjustMinCm);
    EXPECT_EQ(adjust.max, PB::AdjustMaxCm);
    EXPECT_EQ(adjust.step, 1);
    EXPECT_TRUE(adjust.zero_text == nullptr); // 0 is "0 cm", a value like any other
    EXPECT_FALSE(adjust.restart || adjust.placeholder);
    // The older rows are gone: no row writes their keys.
    EXPECT_EQ(Page::RowIndex("real_height"), -1);
    for (const Page::Row& row : Page::Rows()) {
        for (const char* key : row.keys) {
            if (key != nullptr) {
                const std::string k{key};
                EXPECT_TRUE(k != "game_height_cm" && k != "player_height_cm" &&
                            k != "seated_eye_cm")
                    << row.id;
            }
        }
    }
}

TEST(PlayerBodyPage, TheUsersOlderVrIniReadsAsTheHunterZero) {
    Store store;
    for (const char* line : {"player_height_cm=163", "game_height_cm=190"}) {
        const std::string l{line};
        store.Set(l.substr(0, l.find('=')), l.substr(l.find('=') + 1));
    }
    EXPECT_EQ(Page::ValueText(RowById("height"), store), "As the hunter");
    EXPECT_EQ(Page::ValueText(RowById("height_adjust"), store), "0 cm");
}

TEST(PlayerBodyPage, HeightTogglesAndAdjustStepsLive) {
    Store store;
    std::vector<std::string> log;
    Page::Model m{store, [&log](const std::string& l) { log.push_back(l); }};
    m.Reset();
    ASSERT_EQ(m.GetState().focus, 0); // Height
    // Left / right (and the triggers) toggle the two; A does nothing.
    m.Move(Page::Dir::Right);
    EXPECT_EQ(store.h.base, HeightBase::Player);
    EXPECT_EQ(Page::ValueText(RowById("height"), store), "As the player");
    m.Move(Page::Dir::Right);
    EXPECT_EQ(store.h.base, HeightBase::Hunter);
    m.Value(-1);
    EXPECT_EQ(store.h.base, HeightBase::Player);
    m.Move(Page::Dir::Left);
    EXPECT_EQ(store.h.base, HeightBase::Hunter);
    const auto before = store.sets.size();
    EXPECT_EQ(m.Confirm(), Page::Action::None);
    EXPECT_EQ(store.sets.size(), before);
    auto changes = m.TakeChanges();
    ASSERT_EQ(changes.size(), 4u);
    EXPECT_EQ(changes[0].row, "height");
    EXPECT_EQ(changes[0].text, "As the player");
    EXPECT_FALSE(changes[0].restart);
    ASSERT_FALSE(log.empty());
    EXPECT_NE(log[0].find("height_base=player"), std::string::npos);
    EXPECT_NE(log[0].find("[live]"), std::string::npos);
    // Adjust height: 1 cm a step, "+3 cm" / "0 cm" / "-4 cm", clamped to -10..+10.
    m.Move(Page::Dir::Down);
    ASSERT_EQ(m.GetState().focus, 1);
    for (int i = 0; i < 3; ++i) {
        m.Move(Page::Dir::Right);
    }
    EXPECT_EQ(store.h.adjust_cm, 3);
    EXPECT_EQ(Page::ValueText(RowById("height_adjust"), store), "+3 cm");
    m.Move(Page::Dir::Left);
    m.Move(Page::Dir::Left);
    m.Move(Page::Dir::Left);
    EXPECT_EQ(Page::ValueText(RowById("height_adjust"), store), "0 cm");
    for (int i = 0; i < 4; ++i) {
        m.Value(-1);
    }
    EXPECT_EQ(store.h.adjust_cm, -4);
    EXPECT_EQ(Page::ValueText(RowById("height_adjust"), store), "-4 cm");
    for (int i = 0; i < 20; ++i) {
        m.Move(Page::Dir::Left);
    }
    EXPECT_EQ(store.h.adjust_cm, -10);
    EXPECT_EQ(store.sets.back(), "height_adjust_cm=-10");
    for (int i = 0; i < 25; ++i) {
        m.Move(Page::Dir::Right);
    }
    EXPECT_EQ(store.h.adjust_cm, 10);
    EXPECT_EQ(Page::ValueText(RowById("height_adjust"), store), "+10 cm");
    changes = m.TakeChanges();
    ASSERT_FALSE(changes.empty());
    EXPECT_EQ(changes.back().text, "+10 cm");
    EXPECT_NE(log.back().find("height_adjust_cm=10"), std::string::npos);
    EXPECT_NE(log.back().find("[live]"), std::string::npos);
}

TEST(PlayerBodyPage, AdjustIsLiveWhileHeld) {
    // The stick held right on Adjust height for 1 s (the presenter's pace, PressRepeat): a step at
    // the push, then about 8 a second, each one through the store at once (the camera's next tick
    // uses it).
    Store store;
    Page::Model m{store};
    m.Reset();
    m.Move(Page::Dir::Down);
    Page::PressRepeat held;
    std::vector<int> values;
    for (int frame = 0; frame < 90; ++frame) {
        if (held.Step(true, 1.0f / 90.0f)) {
            m.Move(Page::Dir::Right);
            values.push_back(store.h.adjust_cm);
        }
    }
    ASSERT_GE(values.size(), 6u);
    ASSERT_LE(values.size(), 7u);
    for (size_t i = 0; i < values.size(); ++i) {
        EXPECT_EQ(values[i], static_cast<int>(i) + 1);
    }
}

TEST(PlayerBodyPage, DefaultsIsAsTheHunterZero) {
    Store store;
    store.h = {HeightBase::Player, 7};
    const auto c = Page::Defaults(Page::Tabs()[0], store);
    EXPECT_EQ(store.h.base, HeightBase::Hunter);
    EXPECT_EQ(store.h.adjust_cm, 0);
    EXPECT_NE(std::find(c.removed.begin(), c.removed.end(), "height_base"), c.removed.end());
    EXPECT_NE(std::find(c.removed.begin(), c.removed.end(), "height_adjust_cm"), c.removed.end());
    EXPECT_EQ(Page::ValueText(RowById("height"), store), "As the hunter");
    EXPECT_EQ(Page::ValueText(RowById("height_adjust"), store), "0 cm");
}

TEST(PlayerBodyPage, WorldSizeKeepsItsOwnZero) {
    // The Number rows with a zero text step as before: World size from "Headset" to 63, below
    // 40 mm back to the headset's own.
    Store store;
    const Page::Row& ipd = RowById("ipd");
    auto c = Page::Step(ipd, store, 1);
    EXPECT_EQ(store.v["ipd_mm"], "63");
    EXPECT_EQ(c.text, "63 mm");
    store.v["ipd_mm"] = "40";
    c = Page::Step(ipd, store, -1);
    EXPECT_EQ(store.v["ipd_mm"], "0");
    EXPECT_EQ(c.text, "Headset");
    store.v["ipd_mm"] = "80";
    Page::Step(ipd, store, 1);
    EXPECT_EQ(store.v["ipd_mm"], "80");
}

} // namespace
