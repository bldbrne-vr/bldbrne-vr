// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

// The Bloodborne hunter skeleton (chr c0000) in the bone order of the game's Havok pose: the
// hkaPose behind the behavior module's pose node (SprjChrBehaviorModule+0x28, pose at +0x38) has
// exactly these 108 bones. No emulator dependencies, so IK unit tests can use it.
//
// What is here are interop identifiers only: the bone indices and names the code looks bones up
// by (checked against the game's own hkaSkeleton before anything is used) and the hierarchy
// (parent indices). The skeleton's numbers -- the reference pose, the dummy polys, the bone
// lengths, the Havok -> FLVER map -- are never compiled in: they are read at runtime from the
// user's own game (core/vr/bloodborne_skeleton.cpp: the reference pose from the hkaSkeleton in the
// game's memory once c0000 is loaded, the dummies from chr/c0000.chrbnd.dcx -> c0000.flver) and
// built by core/vr/c0000_rig.h into a Skeleton (below). Until then Active() is nullptr and the
// features that need it wait; if the data cannot be read or is not the retail v1.09 skeleton they
// stay off (one log line). Unit tests install a synthetic humanoid of their own
// (tests/c0000_test_skeleton.h).
//
// Conventions (verified in the eboot, BB 1.09):
//  - hkQsTransform: translation, rotation quaternion (x, y, z, w), scale. Local = relative to
//    the parent bone. The w lanes of translation and scale are unused.
//  - Model space = FD4 model space: meters, +Y up, the character faces -Z (its forward axis
//    in world is -column 2 of ChrCtrl+0x90), left arm on +X, right arm on -X. World =
//    W * model with W = the 3x4 row-major [R|t] at ChrCtrl+0x90 (ChrCtrl = PlayerIns+0x58).
//  - Forward kinematics exactly as hkaPose::syncModelSpace (0x6C4F20): a root (parent -1) is
//    copied; otherwise model.t = P.t + rotate(P.q, L.t) (the parent scale does NOT scale the
//    translation), model.q = P.q * L.q (Hamilton product), model.s = P.s * L.s per axis.
//    Parents always precede children, so one pass in index order computes the whole pose.
//  - Bone axes: on the arm chain local +X points down the bone toward the child joint (the
//    child's local translation is almost pure +X). Head: local +X = up the neck, +Z = the
//    face's forward, +Y = the character's left.
namespace VR::Body::C0000 {

// Interop tables: keep clang-format away.
// clang-format off

constexpr int BoneCount = 108;

// Havok bone indices (the hkaPose order).
enum Bone : int {
    Master = 0,
    L_Foot_Target2 = 1,
    L_Foot_Target1 = 2,
    L_Foot_Target = 3,
    R_Foot_Target2 = 4,
    R_Foot_Target1 = 5,
    R_Foot_Target = 6,
    Root = 7,
    Pelvis = 8,
    B_Weapon_Case_pelvis = 9,
    L_Thigh = 10,
    L_Calf = 11,
    L_Foot = 12,
    L_Toe0 = 13,
    Support_L_FootTwist1 = 14,
    Support_L_CalfTwist = 15,
    Support_L_CalfTwist1 = 16,
    Support_L_Skirt_Under = 17,
    Support_L_Knee = 18,
    Support_L_ThighTwist = 19,
    Support_L_Skirt_Knee = 20,
    Support_L_ThighTwist1 = 21,
    L_Weapon_Case_pelvis = 22,
    L_Weapon_Case_pelvis1 = 23,
    R_Thigh = 24,
    R_Calf = 25,
    R_Foot = 26,
    R_Toe0 = 27,
    Support_R_FootTwist1 = 28,
    Support_R_CalfTwist = 29,
    Support_R_CalfTwist1 = 30,
    Support_R_Skirt_Under = 31,
    Support_R_Knee = 32,
    Support_R_ThighTwist = 33,
    Support_R_Skirt_Knee = 34,
    Support_R_ThighTwist1 = 35,
    R_Weapon_Case_pelvis = 36,
    R_Weapon_Case_pelvis1 = 37,
    Support_L_Hip = 38,
    Support_L_Pelvis_Armor = 39,
    Support_R_Hip = 40,
    Support_R_Pelvis_Armor = 41,
    UpperPos = 42,
    UpperTurn = 43,
    Spine = 44,
    B_Weapon_Case_spine = 45,
    Spine1 = 46,
    B_Weapon_Case_spine1 = 47,
    Spine2 = 48,
    B_Weapon_Case_spine2 = 49,
    L_Clavicle = 50,
    L_UpperArm = 51,
    L_Forearm = 52,
    L_Hand = 53,
    L_Finger0 = 54,
    L_Finger01 = 55,
    L_Finger02 = 56,
    L_Finger1 = 57,
    L_Finger11 = 58,
    L_Finger12 = 59,
    L_Finger2 = 60,
    L_Finger21 = 61,
    L_Finger22 = 62,
    L_Finger3 = 63,
    L_Finger31 = 64,
    L_Finger32 = 65,
    L_Finger4 = 66,
    L_Finger41 = 67,
    L_Finger42 = 68,
    L_Weapon = 69,
    Support_L_ForeArmTwist = 70,
    Support_L_ForeArmTwist1 = 71,
    Support_L_Elbow = 72,
    Support_L_UpArmTwist = 73,
    Support_L_UpArmTwist1 = 74,
    Support_L_UpArmTwist_NoRotX = 75,
    Support_L_Shoulder = 76,
    Neck = 77,
    Head = 78,
    R_Clavicle = 79,
    R_UpperArm = 80,
    R_Forearm = 81,
    R_Hand = 82,
    R_Finger0 = 83,
    R_Finger01 = 84,
    R_Finger02 = 85,
    R_Finger1 = 86,
    R_Finger11 = 87,
    R_Finger12 = 88,
    R_Finger2 = 89,
    R_Finger21 = 90,
    R_Finger22 = 91,
    R_Finger3 = 92,
    R_Finger31 = 93,
    R_Finger32 = 94,
    R_Finger4 = 95,
    R_Finger41 = 96,
    R_Finger42 = 97,
    R_Weapon = 98,
    Support_R_ForeArmTwist = 99,
    Support_R_ForeArmTwist1 = 100,
    Support_R_Elbow = 101,
    Support_R_UpArmTwist = 102,
    Support_R_UpArmTwist1 = 103,
    Support_R_UpArmTwist_NoRotX = 104,
    Support_R_Shoulder = 105,
    Support_L_Axilla = 106,
    Support_R_Axilla = 107,
};

inline constexpr std::array<std::string_view, BoneCount> BoneNames = {
    "Master", "L_Foot_Target2", "L_Foot_Target1", "L_Foot_Target",
    "R_Foot_Target2", "R_Foot_Target1", "R_Foot_Target", "Root",
    "Pelvis", "B_Weapon_Case_pelvis", "L_Thigh", "L_Calf",
    "L_Foot", "L_Toe0", "Support_L_FootTwist1", "Support_L_CalfTwist",
    "Support_L_CalfTwist1", "Support_L_Skirt_Under", "Support_L_Knee", "Support_L_ThighTwist",
    "Support_L_Skirt_Knee", "Support_L_ThighTwist1", "L_Weapon_Case_pelvis", "L_Weapon_Case_pelvis1",
    "R_Thigh", "R_Calf", "R_Foot", "R_Toe0",
    "Support_R_FootTwist1", "Support_R_CalfTwist", "Support_R_CalfTwist1", "Support_R_Skirt_Under",
    "Support_R_Knee", "Support_R_ThighTwist", "Support_R_Skirt_Knee", "Support_R_ThighTwist1",
    "R_Weapon_Case_pelvis", "R_Weapon_Case_pelvis1", "Support_L_Hip", "Support_L_Pelvis_Armor",
    "Support_R_Hip", "Support_R_Pelvis_Armor", "UpperPos", "UpperTurn",
    "Spine", "B_Weapon_Case_spine", "Spine1", "B_Weapon_Case_spine1",
    "Spine2", "B_Weapon_Case_spine2", "L_Clavicle", "L_UpperArm",
    "L_Forearm", "L_Hand", "L_Finger0", "L_Finger01",
    "L_Finger02", "L_Finger1", "L_Finger11", "L_Finger12",
    "L_Finger2", "L_Finger21", "L_Finger22", "L_Finger3",
    "L_Finger31", "L_Finger32", "L_Finger4", "L_Finger41",
    "L_Finger42", "L_Weapon", "Support_L_ForeArmTwist", "Support_L_ForeArmTwist1",
    "Support_L_Elbow", "Support_L_UpArmTwist", "Support_L_UpArmTwist1", "Support_L_UpArmTwist_NoRotX",
    "Support_L_Shoulder", "Neck", "Head", "R_Clavicle",
    "R_UpperArm", "R_Forearm", "R_Hand", "R_Finger0",
    "R_Finger01", "R_Finger02", "R_Finger1", "R_Finger11",
    "R_Finger12", "R_Finger2", "R_Finger21", "R_Finger22",
    "R_Finger3", "R_Finger31", "R_Finger32", "R_Finger4",
    "R_Finger41", "R_Finger42", "R_Weapon", "Support_R_ForeArmTwist",
    "Support_R_ForeArmTwist1", "Support_R_Elbow", "Support_R_UpArmTwist", "Support_R_UpArmTwist1",
    "Support_R_UpArmTwist_NoRotX", "Support_R_Shoulder", "Support_L_Axilla", "Support_R_Axilla",
};

// Parent bone index, -1 for the root (Master).
inline constexpr std::array<std::int16_t, BoneCount> Parents = {
    -1, 0, 1, 2, 0, 4, 5, 0, 7, 8, 8, 10,
    11, 12, 12, 11, 11, 11, 10, 10, 19, 10, 8, 8,
    8, 24, 25, 26, 26, 25, 25, 25, 24, 24, 33, 24,
    8, 8, 8, 8, 8, 8, 7, 42, 43, 44, 44, 46,
    46, 48, 48, 50, 51, 52, 53, 54, 55, 53, 57, 58,
    53, 60, 61, 53, 63, 64, 53, 66, 67, 53, 52, 52,
    51, 51, 51, 51, 50, 48, 77, 48, 79, 80, 81, 82,
    83, 84, 82, 86, 87, 82, 89, 90, 82, 92, 93, 82,
    95, 96, 82, 81, 81, 80, 80, 80, 80, 79, 48, 48,
};

// ---------------------------------------------------------------------------------------------
// Named chains

// Body: Master > Root > UpperPos > UpperTurn > Spine > Spine1 > Spine2 > Neck > Head. UpperPos
// carries the whole upper body (spine, arms, neck, head); UpperTurn is the torso-yaw joint;
// Pelvis (child of Root) carries the legs.
inline constexpr std::array<Bone, 9> SpineChain = {Master, Root, UpperPos, UpperTurn, Spine,
                                                  Spine1, Spine2, Neck, Head};

enum Side : int { Left = 0, Right = 1 }; // OpenXR hand order

struct ArmChain {
    Bone clavicle;   // child of Spine2
    Bone upper_arm;  // shoulder joint
    Bone forearm;    // elbow joint
    Bone hand;       // wrist joint
    Bone weapon;     // R_/L_Weapon: weapon attach bone (dmy 300 / 340 sit on its origin)
    Bone fore_arm_twist;          // Support_*_ForeArmTwist: wrist end of the forearm
    Bone fore_arm_twist1;         // Support_*_ForeArmTwist1: mid forearm
    Bone elbow;                   // Support_*_Elbow (child of upper_arm, at the elbow)
    Bone up_arm_twist;            // Support_*_UpArmTwist (shoulder end)
    Bone up_arm_twist1;           // Support_*_UpArmTwist1 (mid upper arm)
    Bone up_arm_twist_no_rot_x;   // Support_*_UpArmTwist_NoRotX
    Bone shoulder;                // Support_*_Shoulder (child of the clavicle)
    Bone axilla;                  // Support_*_Axilla (child of Spine2)
    std::array<std::array<Bone, 3>, 5> fingers; // thumb (Finger0) .. little (Finger4), base..tip
};

inline constexpr ArmChain LeftArm = {
    L_Clavicle, L_UpperArm, L_Forearm, L_Hand, L_Weapon,
    Support_L_ForeArmTwist, Support_L_ForeArmTwist1, Support_L_Elbow,
    Support_L_UpArmTwist, Support_L_UpArmTwist1, Support_L_UpArmTwist_NoRotX,
    Support_L_Shoulder, Support_L_Axilla,
    {{{L_Finger0, L_Finger01, L_Finger02}, {L_Finger1, L_Finger11, L_Finger12}, {L_Finger2, L_Finger21, L_Finger22}, {L_Finger3, L_Finger31, L_Finger32}, {L_Finger4, L_Finger41, L_Finger42}}},
};
inline constexpr ArmChain RightArm = {
    R_Clavicle, R_UpperArm, R_Forearm, R_Hand, R_Weapon,
    Support_R_ForeArmTwist, Support_R_ForeArmTwist1, Support_R_Elbow,
    Support_R_UpArmTwist, Support_R_UpArmTwist1, Support_R_UpArmTwist_NoRotX,
    Support_R_Shoulder, Support_R_Axilla,
    {{{R_Finger0, R_Finger01, R_Finger02}, {R_Finger1, R_Finger11, R_Finger12}, {R_Finger2, R_Finger21, R_Finger22}, {R_Finger3, R_Finger31, R_Finger32}, {R_Finger4, R_Finger41, R_Finger42}}},
};
inline constexpr std::array<ArmChain, 2> Arms = {LeftArm, RightArm};

// ---------------------------------------------------------------------------------------------
// The skeleton's numbers, read at runtime (see the top of this file).

struct Transform {
    std::array<float, 3> t;
    std::array<float, 4> q; // x, y, z, w
    std::array<float, 3> s;
};

// A dummy poly (c0000.flver bind pose) in the local frame of the Havok bone it moves with.
// forward/up are unit vectors (the FLVER stores display lengths). At runtime the game's
// ChrIns::GetDmyPolyMtx 0x18C3BF0 gives the exact world matrices; these are for the math that
// needs them in a bone's frame (weapon calibration, eye height, the gun's barrel, the gauge).
struct DummyPoly {
    std::int32_t id;
    Bone bone;
    std::array<float, 3> position;
    std::array<float, 3> forward;
    std::array<float, 3> up;
};

// Dummy ids (c0000.flver ReferenceID).
constexpr std::int32_t DmyIdHead = 7;             // on Head, front of the face (centre line)
constexpr std::int32_t DmyIdEye = 58;             // on Head, twice: the eyes
constexpr std::int32_t DmyIdHead59 = 59;          // on Head, about 5 cm in front of the right eye
constexpr std::int32_t DmyIdLockOn = 220;         // lock-on point on Spine1 (chest)
constexpr std::int32_t DmyIdRightWeaponBase = 300; // weapon attach (FormA) on R_Weapon
constexpr std::int32_t DmyIdRightTwoHand = 320;   // two-handed grip on R_Weapon
constexpr std::int32_t DmyIdLeftWeaponBase = 340; // off-hand weapon attach on L_Weapon
constexpr std::int32_t DmyIdLeftHand = 344;       // on L_Hand (the wrist)
constexpr std::int32_t DmyIdLeftGun = 345;        // off-hand gun attach (pistols) on L_Weapon
constexpr std::int32_t DmyIdLeftGun2 = 347;       // off-hand gun attach on L_Weapon
constexpr std::int32_t DmyIdLeftGun3 = 348;       // off-hand gun attach on L_Weapon
// The Church Cannon's attach on L_Weapon: read when c0000.flver has it (not in the digest).
constexpr std::int32_t DmyIdLeftGun4 = 350;

struct Skeleton {
    // Reference (bind) pose in local space, hkaSkeleton::m_referencePose.
    std::array<Transform, BoneCount> local{};
    // The same through the syncModelSpace FK above (bind-relative offsets, the reference reach).
    std::array<Transform, BoneCount> model{};
    // Havok index -> c0000.flver bone index (render, dummy-poly and ChrCtrl+0x1B8 map order; the
    // game's map at ChrCtrl+0x1B8 is the inverse: {u32 FLVER count, +8 s16* hk index}).
    std::array<std::int16_t, BoneCount> havok_to_flver{};
    // The body's dummy polys (the first of each id in the FLVER; ids above).
    DummyPoly head7{};
    DummyPoly lock_on{};
    DummyPoly right_weapon_base{};
    DummyPoly right_two_hand{};
    DummyPoly left_weapon_base{};
    DummyPoly left_gun{};
    DummyPoly left_gun2{};
    DummyPoly left_gun3{};
    DummyPoly left_gun4{}; // id 0 when the FLVER has no dummy 350
    DummyPoly left_hand{};
    DummyPoly head59{};
    // Dummy 58 twice on Head: the eyes, left first (Head local +Y is the character's left).
    std::array<DummyPoly, 2> eyes{};
    // Midpoint of the two eyes in Head local space (the first-person eye on the model).
    std::array<float, 3> eye_center_on_head{};
    // Bone lengths of the reference pose (|local translation| of the child joint), meters, of
    // the right arm (the skeleton is left/right symmetric to 1e-6). Use live locals at runtime
    // (proportion sliders and animation can stretch them).
    float clavicle_offset{};  // Spine2 -> clavicle joint
    float clavicle_length{};  // clavicle -> shoulder (|UpperArm.t|)
    float upper_arm_length{}; // shoulder -> elbow (|Forearm.t|)
    float forearm_length{};   // elbow -> wrist (|Hand.t|)
    float hand_to_weapon{};   // wrist -> weapon grip (|Weapon.t|)
    // Straight-line reach in the reference pose (the arms hang down, slightly bent).
    float reference_shoulder_to_hand{};
    float reference_shoulder_to_weapon{};
};

// The skeleton in use: nullptr until it was read (core/vr/bloodborne_skeleton.cpp) or a test
// installed one. Once set it never changes and is never freed, so a reader on any thread may keep
// the pointer.
inline std::atomic<const Skeleton*>& ActiveSlot() {
    static std::atomic<const Skeleton*> slot{nullptr};
    return slot;
}
[[nodiscard]] inline const Skeleton* Active() {
    return ActiveSlot().load(std::memory_order_acquire);
}
inline void Install(const Skeleton* skeleton) {
    ActiveSlot().store(skeleton, std::memory_order_release);
}
// For code that only runs once the skeleton is in (the body's solve, gated on Active()): a
// zero skeleton instead of a crash if a caller forgot the gate.
[[nodiscard]] inline const Skeleton& Ref() {
    static const Skeleton none{};
    const Skeleton* s = Active();
    return s != nullptr ? *s : none;
}

// ---------------------------------------------------------------------------------------------
constexpr bool ParentsPrecedeChildren() {
    for (std::size_t i = 0; i < Parents.size(); ++i) {
        const int parent = Parents[i];
        if (parent >= static_cast<int>(i) || parent < -1 || (parent == -1) != (i == 0)) {
            return false;
        }
    }
    return true;
}
static_assert(ParentsPrecedeChildren());
static_assert(Parents[R_UpperArm] == R_Clavicle && Parents[R_Forearm] == R_UpperArm &&
              Parents[R_Hand] == R_Forearm && Parents[R_Weapon] == R_Hand &&
              Parents[R_Clavicle] == Spine2);
static_assert(Parents[L_UpperArm] == L_Clavicle && Parents[L_Forearm] == L_UpperArm &&
              Parents[L_Hand] == L_Forearm && Parents[L_Weapon] == L_Hand &&
              Parents[L_Clavicle] == Spine2);
static_assert(Parents[Head] == Neck && Parents[Neck] == Spine2 && Parents[Spine2] == Spine1 &&
              Parents[Spine1] == Spine && Parents[Spine] == UpperTurn &&
              Parents[UpperTurn] == UpperPos && Parents[UpperPos] == Root && Parents[Root] == Master);
static_assert(BoneNames[R_Hand] == "R_Hand" && BoneNames[L_Weapon] == "L_Weapon");

// clang-format on

} // namespace VR::Body::C0000
