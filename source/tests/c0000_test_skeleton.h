// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#if defined(BBVR_GAME_DATA_TESTS)
#include <fstream>
#include <iterator>
#include <miniz.h>
#include "core/vr/tpf_patch.h"
#endif

#include "core/vr/c0000_rig.h"
#include "core/vr/c0000_skeleton.h"

// The unit tests' skeleton: a synthetic humanoid mannequin in the hunter's bone order (the interop
// indices, names and hierarchy of core/vr/c0000_skeleton.h) with our own proportions and frames --
// none of the game's numbers. It follows the rig's conventions so the body code reads it the way
// it reads the hunter: model space metres, +Y up, facing -Z, left on +X; the spine's local +X up
// the spine, +Y to the left, +Z forward; the arm bones' local +X down the bone toward the child
// joint with the elbow hinge on the upper arm's +Y; the right side the left mirrored (M = x -> -x;
// a bone's model axes X' = M X, Y' = -M Y, Z' = M Z, the weapon bones' X' = M X, Y' = M Y,
// Z' = -M Z); the weapon bones with the blade (-Y) forward.
//
// A 1.75 m adult: hips 0.98 m, shoulders 1.44 m, eyes 1.635 m; upper arm 0.29 m, forearm
// 0.265 m, the arms hanging 45 degrees out in the frontal plane with the elbows bent 12 degrees
// forward and the wrists rolled 90 degrees.
//
// Including this header installs it as C0000::Active() for the test program. Built with
// BBVR_GAME_DATA_TESTS the tests run on the hunter's own skeleton instead, read from the user's
// dump (the game-data tier, below).
namespace VR::Body::C0000::Test {

namespace Detail {

using Rig::s16;
using Rig::s32;
using Rig::u8;

struct V3 {
    double x{}, y{}, z{};
};
inline V3 operator+(V3 a, V3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline V3 operator-(V3 a, V3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline V3 operator*(V3 a, double s) {
    return {a.x * s, a.y * s, a.z * s};
}
inline double Dot(V3 a, V3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline V3 Cross(V3 a, V3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline V3 Unit(V3 a) {
    return a * (1.0 / std::sqrt(Dot(a, a)));
}

// A rotation by its columns (the bone's local axes in model space).
struct R3 {
    V3 x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};
};
inline V3 Apply(const R3& r, V3 v) {
    return r.x * v.x + r.y * v.y + r.z * v.z;
}
inline R3 Mul(const R3& a, const R3& b) {
    return {Apply(a, b.x), Apply(a, b.y), Apply(a, b.z)};
}
inline R3 Transpose(const R3& r) {
    return {{r.x.x, r.y.x, r.z.x}, {r.x.y, r.y.y, r.z.y}, {r.x.z, r.y.z, r.z.z}};
}
inline R3 AxisAngle(V3 axis, double deg) {
    const V3 a = Unit(axis);
    const double t = deg * 3.14159265358979323846 / 180.0;
    const double c = std::cos(t), s = std::sin(t), k = 1.0 - c;
    return {{c + a.x * a.x * k, a.y * a.x * k + a.z * s, a.z * a.x * k - a.y * s},
            {a.x * a.y * k - a.z * s, c + a.y * a.y * k, a.z * a.y * k + a.x * s},
            {a.x * a.z * k + a.y * s, a.y * a.z * k - a.x * s, c + a.z * a.z * k}};
}
// X along `x`, Y as close to `y_hint` as possible.
inline R3 Along(V3 x, V3 y_hint) {
    const V3 X = Unit(x);
    const V3 Z = Unit(Cross(X, y_hint));
    return {X, Cross(Z, X), Z};
}
inline std::array<float, 4> Quat(const R3& r) {
    // Columns x, y, z -> m[row][col].
    const double m00 = r.x.x, m11 = r.y.y, m22 = r.z.z;
    const double m01 = r.y.x, m02 = r.z.x, m10 = r.x.y, m12 = r.z.y, m20 = r.x.z, m21 = r.y.z;
    double x, y, z, w;
    const double tr = m00 + m11 + m22;
    if (tr > 0) {
        const double s = std::sqrt(tr + 1.0) * 2;
        w = 0.25 * s, x = (m21 - m12) / s, y = (m02 - m20) / s, z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2;
        w = (m21 - m12) / s, x = 0.25 * s, y = (m01 + m10) / s, z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2;
        w = (m02 - m20) / s, x = (m01 + m10) / s, y = 0.25 * s, z = (m12 + m21) / s;
    } else {
        const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2;
        w = (m10 - m01) / s, x = (m02 + m20) / s, y = (m12 + m21) / s, z = 0.25 * s;
    }
    return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z),
            static_cast<float>(w)};
}
inline std::array<float, 3> F3(V3 v) {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

struct Pose {
    V3 p{};
    R3 r{};
};

inline Skeleton Build() {
    // Frames by convention (columns: the bone's local X, Y, Z in model space).
    const R3 root{{0, 0, -1}, {0, 1, 0}, {1, 0, 0}};
    const R3 leg{{0, -1, 0}, {-1, 0, 0}, {0, 0, -1}};
    const R3 toe{{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}};
    const R3 spine{{0, 1, 0}, {1, 0, 0}, {0, 0, -1}};
    const R3 ident{};
    const R3 weapon{{0, 1, 0}, {0, 0, 1}, {1, 0, 0}}; // blade (-Y) forward, X up

    std::array<Pose, BoneCount> m{};
    std::array<bool, BoneCount> set{};
    const auto put = [&](Bone b, V3 p, const R3& r) {
        m[static_cast<std::size_t>(b)] = {p, r};
        set[static_cast<std::size_t>(b)] = true;
    };
    // Centre.
    const double hip_y = 0.98;
    put(Master, {0, hip_y, 0}, root);
    put(Root, {0, hip_y, 0}, root);
    put(Pelvis, {0, hip_y, 0}, leg);
    put(B_Weapon_Case_pelvis, {0, hip_y + 0.01, 0.11}, ident);
    put(UpperPos, {0, hip_y, 0}, spine);
    put(UpperTurn, {0, hip_y, 0}, spine);
    put(Spine, {0, hip_y, 0}, spine);
    put(B_Weapon_Case_spine, {0, 1.08, 0.11}, ident);
    put(Spine1, {0, 1.12, 0.0}, spine);
    put(B_Weapon_Case_spine1, {0, 1.24, 0.11}, ident);
    put(Spine2, {0, 1.25, 0.0}, spine);
    put(B_Weapon_Case_spine2, {0, 1.38, 0.11}, ident);
    put(Neck, {0, 1.50, 0.01}, spine);
    put(Head, {0, 1.56, 0.0}, spine);
    // Left leg (hip joint 9 cm out; knee 0.52 m, ankle 0.08 m).
    const V3 hip{0.09, hip_y, 0}, knee{0.09, 0.52, 0}, ankle{0.09, 0.08, 0}, toes{0.09, 0, -0.14};
    put(L_Foot_Target2, hip, leg);
    put(L_Foot_Target1, knee, leg);
    put(L_Foot_Target, ankle, leg);
    put(L_Thigh, hip, leg);
    put(L_Calf, knee, leg);
    put(L_Foot, ankle, leg);
    put(L_Toe0, toes, toe);
    put(Support_L_FootTwist1, {0.09, 0.02, -0.05}, toe);
    put(Support_L_CalfTwist, ankle, leg);
    put(Support_L_CalfTwist1, (knee + ankle) * 0.5, leg);
    put(Support_L_Skirt_Under, knee, leg);
    put(Support_L_Knee, knee, leg);
    put(Support_L_ThighTwist, hip, leg);
    put(Support_L_Skirt_Knee, knee, leg);
    put(Support_L_ThighTwist1, (hip + knee) * 0.5, leg);
    put(L_Weapon_Case_pelvis, {0.19, 1.10, -0.05}, ident);
    put(L_Weapon_Case_pelvis1, {0.20, 0.99, 0}, ident);
    put(Support_L_Hip, hip, leg);
    put(Support_L_Pelvis_Armor, {0.17, 1.05, 0}, leg);
    // Left arm.
    const V3 clavicle{0.02, 1.43, -0.03};
    const V3 shoulder{0.185, 1.44, 0.03};
    const R3 clav = Along(shoulder - clavicle, {0, 1, 0});
    const R3 upper{{0.70710678, -0.70710678, 0}, {0.70710678, 0.70710678, 0}, {0, 0, 1}};
    const V3 elbow = shoulder + upper.x * 0.29;
    const R3 fore = Mul(upper, AxisAngle({0, 1, 0}, 12.0));
    const R3 elbow_helper = Mul(upper, AxisAngle({0, 1, 0}, 6.0));
    const V3 wrist = elbow + fore.x * 0.265;
    const R3 hand = Mul(fore, AxisAngle({1, 0, 0}, 90.0));
    put(L_Clavicle, clavicle, clav);
    put(L_UpperArm, shoulder, upper);
    put(L_Forearm, elbow, fore);
    put(L_Hand, wrist, hand);
    put(L_Weapon, wrist + Apply(hand, {0.08, 0.0, 0.02}), weapon);
    put(Support_L_ForeArmTwist, wrist, fore);
    put(Support_L_ForeArmTwist1, (elbow + wrist) * 0.5, fore);
    put(Support_L_Elbow, elbow, elbow_helper);
    put(Support_L_UpArmTwist, shoulder, upper);
    put(Support_L_UpArmTwist1, (shoulder + elbow) * 0.5, upper);
    put(Support_L_UpArmTwist_NoRotX, shoulder, upper);
    put(Support_L_Shoulder, shoulder, clav);
    put(Support_L_Axilla, {0.11, 1.33, 0.02}, spine);
    // Left fingers: bases on the hand (its X toward the fingers, its Y from the index to the
    // little finger), joints straight along the hand; the thumb turned in.
    struct Finger {
        Bone bones[3];
        V3 base;
        double lengths[2];
        R3 turn;
    };
    const Finger fingers[] = {
        {{L_Finger0, L_Finger01, L_Finger02}, {0.022, -0.030, 0.020}, {0.050, 0.032},
         Mul(AxisAngle({1, 0, 0}, -60.0), AxisAngle({0, 0, 1}, -30.0))},
        {{L_Finger1, L_Finger11, L_Finger12}, {0.092, -0.030, -0.010}, {0.045, 0.026}, {}},
        {{L_Finger2, L_Finger21, L_Finger22}, {0.095, -0.010, -0.012}, {0.050, 0.030}, {}},
        {{L_Finger3, L_Finger31, L_Finger32}, {0.090, 0.010, -0.010}, {0.047, 0.028}, {}},
        {{L_Finger4, L_Finger41, L_Finger42}, {0.083, 0.029, -0.005}, {0.040, 0.022}, {}},
    };
    for (const Finger& f : fingers) {
        const R3 r = Mul(hand, f.turn);
        V3 p = wrist + Apply(hand, f.base);
        put(f.bones[0], p, r);
        p = p + r.x * f.lengths[0];
        put(f.bones[1], p, r);
        p = p + r.x * f.lengths[1];
        put(f.bones[2], p, r);
    }
    // The right side: the left mirrored.
    for (int i = 0; i < BoneCount; ++i) {
        const std::string_view name = BoneNames[static_cast<std::size_t>(i)];
        std::string mirror{name};
        if (name.starts_with("R_") || name.starts_with("Support_R_")) {
            const std::size_t at = name.starts_with("R_") ? 0 : 8;
            mirror[at] = 'L';
            for (int j = 0; j < BoneCount; ++j) {
                if (BoneNames[static_cast<std::size_t>(j)] == mirror) {
                    const Pose& l = m[static_cast<std::size_t>(j)];
                    const auto mir = [](V3 v) { return V3{-v.x, v.y, v.z}; };
                    const bool weapon_bone = name == "R_Weapon";
                    R3 r{mir(l.r.x), mir(l.r.y) * (weapon_bone ? 1.0 : -1.0),
                         mir(l.r.z) * (weapon_bone ? -1.0 : 1.0)};
                    put(static_cast<Bone>(i), mir(l.p), r);
                }
            }
        }
    }
    Skeleton s{};
    for (int i = 0; i < BoneCount; ++i) {
        const auto k = static_cast<std::size_t>(i);
        if (!set[k]) {
            return {}; // a bone without a place: the tests fail loudly on the zero skeleton
        }
        const int parent = Parents[k];
        V3 t = m[k].p;
        R3 r = m[k].r;
        if (parent >= 0) {
            const Pose& p = m[static_cast<std::size_t>(parent)];
            const R3 pt = Transpose(p.r);
            t = Apply(pt, m[k].p - p.p);
            r = Mul(pt, m[k].r);
        }
        s.local[k] = {F3(t), Quat(r), {1.0f, 1.0f, 1.0f}};
    }
    // Dummies (Head: +X up, +Y left, +Z forward; the weapon bones: +X up, -Y the blade).
    const auto dummy = [](s32 id, Bone b, V3 p, V3 f, V3 u) {
        return DummyPoly{id, b, F3(p), F3(Unit(f)), F3(Unit(u))};
    };
    s.eyes = {dummy(DmyIdEye, Head, {0.075, 0.032, 0.085}, {0, 0, 1}, {1, 0, 0}),
              dummy(DmyIdEye, Head, {0.075, -0.032, 0.085}, {0, 0, 1}, {1, 0, 0})};
    s.eye_center_on_head = {0.075f, 0.0f, 0.085f};
    s.head7 = dummy(DmyIdHead, Head, {0.03, 0, 0.10}, {0, 0, 1}, {1, 0, 0});
    s.head59 = dummy(DmyIdHead59, Head, {0.085, -0.032, 0.135}, {0, 0, 1}, {1, 0, 0});
    s.lock_on = dummy(DmyIdLockOn, Spine1, {0.20, 0, 0.08}, {0, 0, 1}, {1, 0, 0});
    s.right_weapon_base = dummy(DmyIdRightWeaponBase, R_Weapon, {0, 0, 0}, {0, 0, 1}, {0, 1, 0});
    s.right_two_hand = dummy(DmyIdRightTwoHand, R_Weapon, {0, 0.05, -0.005}, {0, 0, 1}, {0, 1, 0});
    s.left_weapon_base = dummy(DmyIdLeftWeaponBase, L_Weapon, {0, 0, 0}, {0, 0, 1}, {0, 1, 0});
    // The guns' attach frames: their forward (the part's +Z, the barrel's back) tipped up from
    // the weapon bone's +Y by 25 / 45 / 60 degrees about its Z.
    const auto gun = [&](s32 id, double deg, V3 at) {
        const double t = deg * 3.14159265358979323846 / 180.0;
        return dummy(id, L_Weapon, at, {std::sin(t), std::cos(t), 0}, {std::cos(t), -std::sin(t), 0});
    };
    s.left_gun = gun(DmyIdLeftGun, 25.0, {-0.012, -0.003, 0});
    s.left_gun2 = gun(DmyIdLeftGun2, 45.0, {0.008, -0.02, -0.004});
    s.left_gun3 = gun(DmyIdLeftGun3, 60.0, {0, 0.009, 0});
    s.left_hand = dummy(DmyIdLeftHand, L_Hand, {-0.018, 0, -0.007}, {0, 1, 0}, {-1, 0, 0});
    for (int i = 0; i < BoneCount; ++i) {
        s.havok_to_flver[static_cast<std::size_t>(i)] = static_cast<s16>(i);
    }
    Rig::FinishModel(s);
    return s;
}

} // namespace Detail

inline const Skeleton& Mannequin() {
    static const Skeleton s = Detail::Build();
    return s;
}

#if defined(BBVR_GAME_DATA_TESTS)
// The game-data tier (BBVR_GAME_DATA_TESTS, linked with miniz): the hunter's own skeleton, built by
// the emulator's builder (core/vr/c0000_rig.h) from external data in BBVR_GAME_DIR.
// Nothing from it is written anywhere. nullptr (the mannequin is used) when
// the files are missing or are not the retail v1.09 data.
namespace Detail {
inline std::optional<std::vector<u8>> ReadDcx(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return std::nullopt;
    }
    const std::vector<u8> file{std::istreambuf_iterator<char>(f), {}};
    const auto dcx = TpfPatch::ParseDcx(file);
    if (!dcx) {
        return std::nullopt;
    }
    std::vector<u8> out(dcx->unpacked);
    mz_ulong length = dcx->unpacked;
    if (mz_uncompress(out.data(), &length, file.data() + dcx->stream,
                      static_cast<mz_ulong>(file.size() - dcx->stream)) != MZ_OK ||
        length != dcx->unpacked) {
        return std::nullopt;
    }
    return out;
}

inline std::optional<Skeleton> LoadGame(std::string& why) {
    const char* env = std::getenv("BBVR_GAME_DIR");
    std::string root = env != nullptr ? env : "./ROM/CUSA03173/dvdroot_ps4/";
    if (!root.empty() && root.back() != '/' && root.back() != '\\') {
        root += '/';
    }
    const auto ani = ReadDcx(root + "chr/c0000.anibnd.dcx");
    const auto chr = ReadDcx(root + "chr/c0000.chrbnd.dcx");
    if (!ani || !chr) {
        why = root + "chr/c0000.anibnd.dcx / c0000.chrbnd.dcx not readable";
        return std::nullopt;
    }
    const auto hkx = Rig::Bnd4Find(*ani, "skeleton.hkx");
    const auto flv = Rig::Bnd4Find(*chr, "c0000.flver");
    if (!hkx || !flv) {
        why = "no skeleton.hkx / c0000.flver inside";
        return std::nullopt;
    }
    const auto havok = Rig::ParseHkxSkeleton(*hkx, why);
    const auto flver = havok ? Rig::ParseFlver(*flv, why) : std::nullopt;
    if (!havok || !flver) {
        return std::nullopt;
    }
    if (why = Rig::CheckInterop(havok->names, havok->parents); !why.empty()) {
        return std::nullopt;
    }
    if (Rig::HavokDigest(havok->parents, havok->local) != Rig::RetailHavokDigest ||
        Rig::FlverDigest(*flver) != Rig::RetailFlverDigest) {
        why = "not the retail v1.09 data";
        return std::nullopt;
    }
    Skeleton s{};
    if (!Rig::Build(havok->local, *flver, s, why)) {
        return std::nullopt;
    }
    return s;
}
} // namespace Detail

inline const Skeleton* GameSkeleton() {
    static const std::optional<Skeleton> s = [] {
        std::string why;
        auto loaded = Detail::LoadGame(why);
        if (loaded) {
            std::printf("  (game-data tier: the hunter's skeleton from the dump, digests match)\n");
        } else {
            std::printf("  (game-data tier unavailable: %s; the mannequin)\n", why.c_str());
        }
        return loaded;
    }();
    return s ? &*s : nullptr;
}
#else
inline const Skeleton* GameSkeleton() {
    return nullptr;
}
#endif

// True when the tests run on the hunter's own skeleton (the game-data tier).
inline bool OnGameData() {
    return GameSkeleton() != nullptr;
}

inline const Skeleton& TestSkeleton() {
    const Skeleton* game = GameSkeleton();
    return game != nullptr ? *game : Mannequin();
}

inline const bool Installed = (Install(&TestSkeleton()), true);

} // namespace VR::Body::C0000::Test

// A test calibrated on the hunter's own body (his proportions and his rig's frames): it runs on
// the game-data tier only and passes as skipped on the mannequin.
#define C0000_NEEDS_GAME_SKELETON()                                                                \
    do {                                                                                           \
        if (!::VR::Body::C0000::Test::OnGameData()) {                                              \
            std::printf("  (skipped on the mannequin: calibrated on the hunter's skeleton, the "   \
                        "game-data tier runs it)\n");                                              \
            return;                                                                                \
        }                                                                                          \
    } while (false)
