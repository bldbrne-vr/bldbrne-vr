// SPDX-License-Identifier: GPL-2.0-or-later

// The hunter's skeleton read at runtime (core/vr/c0000_rig.h, core/vr/bloodborne_skeleton.cpp):
// the FLVER and BND4 readers on synthetic files, the build (the reference pose's FK, the dummies
// into their Havok bone's frame, the lengths, the Havok -> FLVER map by name) on the tests'
// mannequin, its refusals (a missing bone, dummy or eye; names or parents not c0000's), the
// digests, and the Active() gate. On the game-data tier (BBVR_GAME_DATA_TESTS) also the hunter's
// own skeleton from the user's dump: digests, FK and symmetry.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "c0000_test_skeleton.h"
#include "core/vr/arm_ik.h"
#include "core/vr/c0000_rig.h"
#include "core/vr/c0000_skeleton.h"

namespace {

namespace B = VR::Body;
namespace C = VR::Body::C0000;
namespace R = VR::Body::C0000::Rig;
using u8 = std::uint8_t;

template <typename T>
void Put(std::vector<u8>& d, std::size_t at, T v) {
    if (d.size() < at + sizeof(T)) {
        d.resize(at + sizeof(T));
    }
    std::memcpy(d.data() + at, &v, sizeof(T));
}
void Put3(std::vector<u8>& d, std::size_t at, const std::array<float, 3>& v) {
    for (int k = 0; k < 3; ++k) {
        Put(d, at + 4 * static_cast<std::size_t>(k), v[static_cast<std::size_t>(k)]);
    }
}
std::size_t PutUtf16(std::vector<u8>& d, const std::string& s) {
    const std::size_t at = d.size();
    for (const char c : s) {
        d.push_back(static_cast<u8>(c));
        d.push_back(0);
    }
    d.push_back(0);
    d.push_back(0);
    return at;
}

// A FLVER2 with these bones and dummies (no materials or meshes), names in UTF-16.
std::vector<u8> MakeFlver(const R::FlverRig& rig) {
    const std::size_t nd = rig.dummies.size(), nb = rig.bones.size();
    std::vector<u8> d(0x80 + 0x40 * nd + 0x80 * nb, 0);
    std::memcpy(d.data(), "FLVER\0L\0", 8);
    Put<std::uint32_t>(d, 0x08, 0x20014);
    Put<std::int32_t>(d, 0x14, static_cast<std::int32_t>(nd));
    Put<std::int32_t>(d, 0x18, 0);
    Put<std::int32_t>(d, 0x1C, static_cast<std::int32_t>(nb));
    d[0x49] = 1;
    for (std::size_t i = 0; i < nd; ++i) {
        const R::FlverDummy& x = rig.dummies[i];
        const std::size_t o = 0x80 + 0x40 * i;
        Put3(d, o, x.pos);
        Put3(d, o + 0x10, x.fwd);
        Put(d, o + 0x1C, x.ref);
        Put(d, o + 0x1E, x.parent);
        Put3(d, o + 0x20, x.up);
        Put(d, o + 0x2C, x.attach);
    }
    for (std::size_t i = 0; i < nb; ++i) {
        const R::FlverBone& b = rig.bones[i];
        const std::size_t o = 0x80 + 0x40 * nd + 0x80 * i;
        Put3(d, o, b.t);
        Put3(d, o + 0x10, b.r);
        Put(d, o + 0x1C, b.parent);
        Put3(d, o + 0x20, b.s);
        const std::size_t name = PutUtf16(d, b.name);
        Put<std::uint32_t>(d, o + 0x0C, static_cast<std::uint32_t>(name));
    }
    return d;
}

// A FLVER rig for the mannequin: every Havok bone (in reverse order, so the map is not the
// identity) as a root at its model position with no rotation, an extra non-Havok bone, and the
// body's dummies given in a FLVER bone's frame (`parent`) and moving with another (`attach`).
int FlverIndexOf(int havok) {
    return C::BoneCount - 1 - havok;
}
R::FlverRig MannequinFlver() {
    const C::Skeleton& sk = C::Test::Mannequin();
    R::FlverRig rig;
    for (int j = 0; j < C::BoneCount; ++j) {
        const int hk = C::BoneCount - 1 - j;
        rig.bones.push_back({std::string{C::BoneNames[static_cast<std::size_t>(hk)]}, -1,
                             sk.model[static_cast<std::size_t>(hk)].t, {0, 0, 0}, {1, 1, 1}});
    }
    rig.bones.push_back({"Not_A_Havok_Bone", 0, {0.1f, 0.2f, 0.3f}, {0.5f, 0.0f, 0.0f}, {1, 1, 1}});
    const auto dummy = [&](std::int16_t ref, int parent_hk, int attach_hk, std::array<float, 3> pos,
                           std::array<float, 3> fwd, std::array<float, 3> up) {
        rig.dummies.push_back({ref, static_cast<std::int16_t>(FlverIndexOf(parent_hk)),
                               static_cast<std::int16_t>(FlverIndexOf(attach_hk)), pos, fwd, up});
    };
    dummy(999, C::Head, C::Head, {0, 0, 0}, {0, 0, 1}, {0, 1, 0}); // not a body dummy: ignored
    dummy(C::DmyIdHead, C::Head, C::Head, {0.0f, 0.02f, -0.1f}, {0, 0, -2}, {0, 3, 0});
    dummy(C::DmyIdEye, C::Head, C::Head, {-0.03f, 0.08f, -0.08f}, {0, 0, -1}, {0, 1, 0});
    dummy(C::DmyIdEye, C::Head, C::Head, {0.03f, 0.08f, -0.08f}, {0, 0, -1}, {0, 1, 0});
    dummy(C::DmyIdHead59, C::Head, C::Head, {-0.03f, 0.08f, -0.13f}, {0, 0, -1}, {0, 1, 0});
    dummy(C::DmyIdLockOn, C::Spine2, C::Spine1, {0, 0.05f, -0.1f}, {0, 0, -1}, {0, 1, 0});
    dummy(C::DmyIdRightWeaponBase, C::R_Weapon, C::R_Weapon, {0, 0, 0}, {0, 0, -1}, {0, 1, 0});
    dummy(C::DmyIdRightTwoHand, C::R_Hand, C::R_Weapon, {0.05f, 0, 0}, {0, 0, -1}, {0, 1, 0});
    dummy(C::DmyIdLeftWeaponBase, C::L_Weapon, C::L_Weapon, {0, 0, 0}, {0, 0, -1}, {0, 1, 0});
    dummy(C::DmyIdLeftHand, C::L_Hand, C::L_Hand, {0.01f, 0, 0}, {1, 0, 0}, {0, 1, 0});
    dummy(C::DmyIdLeftGun, C::L_Weapon, C::L_Weapon, {0.01f, 0, 0}, {0, 1, -1}, {0, 1, 1});
    dummy(C::DmyIdLeftGun2, C::L_Weapon, C::L_Weapon, {0.02f, 0, 0}, {0, 0, -1}, {0, 1, 0});
    dummy(C::DmyIdLeftGun3, C::L_Weapon, C::L_Weapon, {0.03f, 0, 0}, {1, 0, -1}, {1, 1, 1});
    dummy(C::DmyIdLeftGun3, C::Head, C::Head, {9, 9, 9}, {0, 0, -1}, {0, 1, 0}); // a second 348
    return rig;
}

B::Vec3 V(const std::array<float, 3>& a) {
    return {a[0], a[1], a[2]};
}
B::Quat Q(const std::array<float, 4>& a) {
    return {a[0], a[1], a[2], a[3]};
}

// Where the build should put a dummy: its model position (the FLVER parent's identity frame at
// its bone's model position) in the attach bone's reference frame.
B::Vec3 ExpectedLocal(int attach_hk, B::Vec3 model_pos) {
    const C::Transform& m = C::Test::Mannequin().model[static_cast<std::size_t>(attach_hk)];
    return B::Rotate(B::Conjugate(Q(m.q)), model_pos - V(m.t));
}

} // namespace

TEST(C0000Rig, FlverReaderReadsBonesAndDummies) {
    const R::FlverRig in = MannequinFlver();
    const std::vector<u8> file = MakeFlver(in);
    std::string why;
    const auto out = R::ParseFlver(file, why);
    ASSERT_TRUE(out.has_value());
    ASSERT_EQ(out->bones.size(), in.bones.size());
    ASSERT_EQ(out->dummies.size(), in.dummies.size());
    for (std::size_t i = 0; i < in.bones.size(); ++i) {
        EXPECT_EQ(out->bones[i].name, in.bones[i].name);
        EXPECT_EQ(out->bones[i].parent, in.bones[i].parent);
        EXPECT_EQ(std::memcmp(out->bones[i].t.data(), in.bones[i].t.data(), 12), 0);
        EXPECT_EQ(std::memcmp(out->bones[i].r.data(), in.bones[i].r.data(), 12), 0);
    }
    for (std::size_t i = 0; i < in.dummies.size(); ++i) {
        EXPECT_EQ(out->dummies[i].ref, in.dummies[i].ref);
        EXPECT_EQ(out->dummies[i].parent, in.dummies[i].parent);
        EXPECT_EQ(out->dummies[i].attach, in.dummies[i].attach);
        EXPECT_EQ(std::memcmp(out->dummies[i].up.data(), in.dummies[i].up.data(), 12), 0);
    }
    // Not a FLVER, a truncated one, absurd counts: refused with a reason.
    std::vector<u8> bad = file;
    bad[0] = 'X';
    EXPECT_FALSE(R::ParseFlver(bad, why).has_value());
    EXPECT_FALSE(why.empty());
    EXPECT_FALSE(R::ParseFlver(std::span<const u8>(file.data(), 0x100), why).has_value());
    bad = file;
    Put<std::int32_t>(bad, 0x1C, 100000);
    EXPECT_FALSE(R::ParseFlver(bad, why).has_value());
}

TEST(C0000Rig, Bnd4FindsAFileByItsBaseName) {
    // BND4: header 0x40, file headers of 0x24 bytes {flags, -1, size s64, size s64, offset u32,
    // id s32, name u32}, UTF-16 names.
    std::vector<u8> d(0x40 + 2 * 0x24, 0);
    std::memcpy(d.data(), "BND4", 4);
    Put<std::int32_t>(d, 0x0C, 2);
    Put<std::int64_t>(d, 0x20, 0x24);
    d[0x30] = 1;
    const std::size_t n0 = PutUtf16(d, "N:\\SPRJ\\data\\Model\\chr\\c0000\\c0000.hkx");
    const std::size_t n1 = PutUtf16(d, "N:\\SPRJ\\data\\Model\\chr\\c0000\\C0000.FLVER");
    const std::size_t f0 = d.size();
    d.insert(d.end(), {1, 2, 3});
    const std::size_t f1 = d.size();
    d.insert(d.end(), {4, 5, 6, 7});
    const auto entry = [&](int k, std::size_t at, std::int64_t size, std::size_t name) {
        const std::size_t e = 0x40 + 0x24 * static_cast<std::size_t>(k);
        Put<std::int64_t>(d, e + 8, size);
        Put<std::int64_t>(d, e + 16, size);
        Put<std::uint32_t>(d, e + 24, static_cast<std::uint32_t>(at));
        Put<std::int32_t>(d, e + 28, k);
        Put<std::uint32_t>(d, e + 32, static_cast<std::uint32_t>(name));
    };
    entry(0, f0, 3, n0);
    entry(1, f1, 4, n1);
    const auto flver = R::Bnd4Find(d, "c0000.flver");
    ASSERT_TRUE(flver.has_value());
    ASSERT_EQ(flver->size(), 4u);
    EXPECT_EQ((*flver)[0], 4);
    EXPECT_FALSE(R::Bnd4Find(d, "skeleton.hkx").has_value());
    d[0] = 'X';
    EXPECT_FALSE(R::Bnd4Find(d, "c0000.flver").has_value());
}

TEST(C0000Rig, BuildPutsTheDummiesInTheirHavokBonesFrames) {
    const C::Skeleton& mk = C::Test::Mannequin();
    const R::FlverRig flver = MannequinFlver();
    C::Skeleton s{};
    std::string why;
    ASSERT_TRUE(R::Build(mk.local, flver, s, why));
    EXPECT_TRUE(why.empty());
    // The reference pose as given, its FK as the mannequin's, the lengths.
    for (int i = 0; i < C::BoneCount; ++i) {
        const auto k = static_cast<std::size_t>(i);
        EXPECT_EQ(std::memcmp(&s.local[k], &mk.local[k], sizeof(C::Transform)), 0) << i;
        EXPECT_EQ(std::memcmp(&s.model[k], &mk.model[k], sizeof(C::Transform)), 0) << i;
        // Havok -> FLVER by name.
        EXPECT_EQ(s.havok_to_flver[k], FlverIndexOf(i)) << i;
    }
    EXPECT_EQ(s.upper_arm_length, mk.upper_arm_length);
    EXPECT_NEAR(s.upper_arm_length, B::Length(V(mk.local[C::R_Forearm].t)), 1e-6f);
    EXPECT_NEAR(s.reference_shoulder_to_hand,
                B::Length(V(mk.model[C::R_Hand].t) - V(mk.model[C::R_UpperArm].t)), 1e-6f);
    // Each dummy: on its attach bone, its position from its parent bone's frame, unit axes.
    const auto check = [&](const C::DummyPoly& d, int id, int attach, int parent,
                           std::array<float, 3> pos, B::Vec3 fwd, B::Vec3 up) {
        EXPECT_EQ(d.id, id);
        EXPECT_EQ(static_cast<int>(d.bone), attach) << id;
        const B::Vec3 model = V(mk.model[static_cast<std::size_t>(parent)].t) + V(pos);
        const B::Vec3 want = ExpectedLocal(attach, model);
        EXPECT_LT(B::Length(V(d.position) - want), 1e-5f) << id;
        const B::Quat inv = B::Conjugate(Q(mk.model[static_cast<std::size_t>(attach)].q));
        EXPECT_LT(B::Length(V(d.forward) - B::Rotate(inv, B::Normalize(fwd))), 1e-5f) << id;
        EXPECT_LT(B::Length(V(d.up) - B::Rotate(inv, B::Normalize(up))), 1e-5f) << id;
        EXPECT_NEAR(B::Length(V(d.forward)), 1.0f, 1e-6f) << id;
    };
    check(s.head7, C::DmyIdHead, C::Head, C::Head, {0.0f, 0.02f, -0.1f}, {0, 0, -1}, {0, 1, 0});
    check(s.lock_on, C::DmyIdLockOn, C::Spine1, C::Spine2, {0, 0.05f, -0.1f}, {0, 0, -1},
          {0, 1, 0});
    check(s.right_two_hand, C::DmyIdRightTwoHand, C::R_Weapon, C::R_Hand, {0.05f, 0, 0},
          {0, 0, -1}, {0, 1, 0});
    check(s.left_gun, C::DmyIdLeftGun, C::L_Weapon, C::L_Weapon, {0.01f, 0, 0}, {0, 1, -1},
          {0, 1, 1});
    // The first of an id wins (348 twice).
    check(s.left_gun3, C::DmyIdLeftGun3, C::L_Weapon, C::L_Weapon, {0.03f, 0, 0}, {1, 0, -1},
          {1, 1, 1});
    // The eyes: left (Head local +Y, the character's left) first; their midpoint.
    EXPECT_GT(s.eyes[0].position[1], s.eyes[1].position[1]);
    for (int k = 0; k < 3; ++k) {
        EXPECT_NEAR(s.eye_center_on_head[static_cast<std::size_t>(k)],
                    0.5f * (s.eyes[0].position[static_cast<std::size_t>(k)] +
                            s.eyes[1].position[static_cast<std::size_t>(k)]),
                    1e-6f);
    }
}

// The Church Cannon's attach (350): read when the FLVER has it, on its bone; the build never needs
// it, and it is not in the retail digest.
TEST(C0000Rig, TheChurchCannonsAttachIsOptional) {
    const C::Skeleton& mk = C::Test::Mannequin();
    R::FlverRig flver = MannequinFlver();
    C::Skeleton s{};
    std::string why;
    ASSERT_TRUE(R::Build(mk.local, flver, s, why));
    EXPECT_EQ(s.left_gun4.id, 0);
    const auto digest = R::FlverDigest(flver);
    flver.dummies.push_back({static_cast<std::int16_t>(C::DmyIdLeftGun4),
                             static_cast<std::int16_t>(FlverIndexOf(C::L_Weapon)),
                             static_cast<std::int16_t>(FlverIndexOf(C::L_Weapon)),
                             {0.04f, 0.0f, 0.0f},
                             {0.0f, 0.0f, -1.0f},
                             {0.0f, 1.0f, 0.0f}});
    EXPECT_EQ(R::FlverDigest(flver), digest);
    ASSERT_TRUE(R::Build(mk.local, flver, s, why));
    EXPECT_EQ(s.left_gun4.id, C::DmyIdLeftGun4);
    EXPECT_EQ(static_cast<int>(s.left_gun4.bone), static_cast<int>(C::L_Weapon));
    EXPECT_NEAR(B::Length(V(s.left_gun4.forward)), 1.0f, 1e-6f);
}

TEST(C0000Rig, BuildRefusesWhatItCannotPlace) {
    const C::Skeleton& mk = C::Test::Mannequin();
    C::Skeleton s{};
    std::string why;
    // A Havok bone the FLVER does not have.
    R::FlverRig no_bone = MannequinFlver();
    no_bone.bones[static_cast<std::size_t>(FlverIndexOf(C::R_Hand))].name = "Renamed";
    EXPECT_FALSE(R::Build(mk.local, no_bone, s, why));
    EXPECT_NE(why.find("R_Hand"), std::string::npos) << why;
    // A body dummy missing.
    R::FlverRig no_dummy = MannequinFlver();
    for (auto& d : no_dummy.dummies) {
        if (d.ref == C::DmyIdLeftHand) {
            d.ref = 1;
        }
    }
    why.clear();
    EXPECT_FALSE(R::Build(mk.local, no_dummy, s, why));
    EXPECT_NE(why.find("344"), std::string::npos) << why;
    // One eye.
    R::FlverRig one_eye = MannequinFlver();
    for (auto& d : one_eye.dummies) {
        if (d.ref == C::DmyIdEye) {
            d.ref = 1;
            break;
        }
    }
    why.clear();
    EXPECT_FALSE(R::Build(mk.local, one_eye, s, why));
    EXPECT_NE(why.find("eye"), std::string::npos) << why;
    // A dummy whose parent bone does not exist.
    R::FlverRig bad_parent = MannequinFlver();
    bad_parent.dummies[1].parent = 5000;
    why.clear();
    EXPECT_FALSE(R::Build(mk.local, bad_parent, s, why));
}

TEST(C0000Rig, InteropNamesAndParentsAreChecked) {
    std::array<std::string, C::BoneCount> names;
    std::array<std::int16_t, C::BoneCount> parents{};
    for (int i = 0; i < C::BoneCount; ++i) {
        names[static_cast<std::size_t>(i)] = C::BoneNames[static_cast<std::size_t>(i)];
        parents[static_cast<std::size_t>(i)] = C::Parents[static_cast<std::size_t>(i)];
    }
    EXPECT_TRUE(R::CheckInterop(names, parents).empty());
    auto swapped = names;
    std::swap(swapped[C::L_Hand], swapped[C::R_Hand]);
    EXPECT_NE(R::CheckInterop(swapped, parents).find("L_Hand"), std::string::npos);
    auto moved = parents;
    moved[C::R_Weapon] = C::R_Forearm;
    EXPECT_NE(R::CheckInterop(names, moved).find("parent"), std::string::npos);
}

TEST(C0000Rig, DigestsSeeEveryInputAndOnlyThose) {
    const C::Skeleton& mk = C::Test::Mannequin();
    std::array<std::int16_t, C::BoneCount> parents{};
    std::copy(C::Parents.begin(), C::Parents.end(), parents.begin());
    const auto h0 = R::HavokDigest(parents, mk.local);
    auto local = mk.local;
    local[C::L_Finger32].q[3] = std::nextafter(local[C::L_Finger32].q[3], 2.0f);
    EXPECT_NE(R::HavokDigest(parents, local), h0);
    // The mannequin is not the retail skeleton.
    EXPECT_NE(h0, R::RetailHavokDigest);
    R::FlverRig flver = MannequinFlver();
    const auto f0 = R::FlverDigest(flver);
    flver.dummies[0].pos[0] = 5.0f; // dummy 999: not a body dummy
    EXPECT_EQ(R::FlverDigest(flver), f0);
    flver.dummies[1].pos[0] = 5.0f;
    EXPECT_NE(R::FlverDigest(flver), f0);
    flver = MannequinFlver();
    flver.bones.back().t[1] = 0.25f;
    EXPECT_NE(R::FlverDigest(flver), f0);
    EXPECT_NE(f0, R::RetailFlverDigest);
}

TEST(C0000Rig, TheGateBeforeTheSkeletonIsIn) {
    // Installed by the fixture; the rigs take the hand's reference rotation from it.
    ASSERT_NE(C::Active(), nullptr);
    const C::Skeleton& sk = *C::Active();
    const B::ArmRig r = B::HunterRightArm();
    const B::ArmRig l = B::HunterLeftArm();
    EXPECT_EQ(std::memcmp(&r.handNeutral, sk.local[C::R_Hand].q.data(), 16), 0);
    EXPECT_EQ(std::memcmp(&l.handNeutral, sk.local[C::L_Hand].q.data(), 16), 0);
    // Without a skeleton Ref() gives the zero skeleton (callers gate on Active()).
    C::Install(nullptr);
    EXPECT_EQ(C::Active(), nullptr);
    EXPECT_EQ(C::Ref().upper_arm_length, 0.0f);
    C::Install(&C::Test::TestSkeleton());
    EXPECT_EQ(C::Active(), &C::Test::TestSkeleton());
}

TEST(C0000Rig, TheMannequinIsAMirroredHumanoid) {
    const C::Skeleton& mk = C::Test::Mannequin();
    for (const auto [l, r] : {std::pair{C::L_UpperArm, C::R_UpperArm}, std::pair{C::L_Hand, C::R_Hand},
                              std::pair{C::L_Weapon, C::R_Weapon}, std::pair{C::L_Foot, C::R_Foot},
                              std::pair{C::L_Finger42, C::R_Finger42}}) {
        const B::Vec3 a = V(mk.model[static_cast<std::size_t>(l)].t);
        const B::Vec3 b = V(mk.model[static_cast<std::size_t>(r)].t);
        EXPECT_LT(B::Length(a - B::Vec3{-b.x, b.y, b.z}), 1e-5f) << l;
        EXPECT_GT(a.x, 0.0f) << l; // left on +X
    }
    // Eyes above the shoulders, the head facing -Z.
    const C::Transform& head = mk.model[C::Head];
    const B::Vec3 eye = V(head.t) + B::Rotate(Q(head.q), V(mk.eye_center_on_head));
    EXPECT_NEAR(eye.y, 1.635f, 0.01f);
    EXPECT_LT(eye.z, V(head.t).z);
    EXPECT_NEAR(mk.upper_arm_length, 0.29f, 1e-5f);
    EXPECT_NEAR(mk.forearm_length, 0.265f, 1e-5f);
}

#if defined(BBVR_GAME_DATA_TESTS)
TEST(C0000Rig, TheHuntersSkeletonFromTheDump) {
    const C::Skeleton* game = C::Test::GameSkeleton();
    if (game == nullptr) {
        std::printf("  (skipped: no dump)\n");
        return;
    }
    // FK in float of the read locals reproduces the built model pose; the arms are symmetric.
    std::array<B::QsTransform, C::BoneCount> local{}, model{};
    for (int i = 0; i < C::BoneCount; ++i) {
        const auto& t = game->local[static_cast<std::size_t>(i)];
        local[static_cast<std::size_t>(i)] = {V(t.t), Q(t.q), V(t.s)};
    }
    ASSERT_TRUE(B::LocalToModel(local.data(), C::Parents.data(), C::BoneCount, model.data()));
    for (int i = 0; i < C::BoneCount; ++i) {
        EXPECT_LT(B::Length(model[static_cast<std::size_t>(i)].t -
                            V(game->model[static_cast<std::size_t>(i)].t)),
                  2e-6f)
            << i;
    }
    EXPECT_NEAR(B::Length(V(game->local[C::L_Forearm].t)), game->upper_arm_length, 1e-5f);
    EXPECT_NEAR(B::Length(V(game->local[C::L_Hand].t)), game->forearm_length, 1e-5f);
    // The game's map (ChrCtrl+0x1B8) is checked live; here: a permutation into the FLVER's bones.
    std::array<bool, 512> seen{};
    for (const std::int16_t f : game->havok_to_flver) {
        ASSERT_GE(f, 0);
        ASSERT_LT(f, 512);
        EXPECT_FALSE(seen[static_cast<std::size_t>(f)]);
        seen[static_cast<std::size_t>(f)] = true;
    }
}
#endif
