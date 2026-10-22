// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <vector>
#include <miniz.h>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/file_sys/fs.h"
#include "core/vr/bloodborne_skeleton.h"
#include "core/vr/c0000_rig.h"
#include "core/vr/c0000_skeleton.h"
#include "core/vr/hook_util.h"
#include "core/vr/tpf_patch.h"

namespace VR::Bloodborne {

namespace {

namespace C = VR::Body::C0000;
namespace R = VR::Body::C0000::Rig;
using Hook::Plausible;
using Hook::Read;

constexpr const char* FlverArchive = "/app0/dvdroot_ps4/chr/c0000.chrbnd.dcx";
// hkaSkeleton (hk_2014): name +0x10, parents {s16*, count} +0x18, bones {16 bytes: hkStringPtr
// name (bit 0 = owned), lock translation} +0x28, reference pose {hkQsTransform (48 bytes)*, count}
// +0x38.
constexpr u32 OffsetSkelParents = 0x18;
constexpr u32 OffsetSkelParentCount = 0x20;
constexpr u32 OffsetSkelBones = 0x28;
constexpr u32 OffsetSkelCount = 0x30;
constexpr u32 OffsetSkelPose = 0x38;
constexpr u32 OffsetSkelPoseCount = 0x40;

enum State : int { NotYet = 0, Ready = 1, Failed = 2 };
std::atomic<int> g_state{NotYet};
std::mutex g_mutex;
std::string g_failure;

// The skeleton's raw inputs from the game's memory, or why not: an object that does not look like
// a loaded 108-bone hkaSkeleton (yet) is not an error, the next call tries again.
std::string ReadHavok(const u8* skeleton, R::HavokRig& out) {
    if (!Plausible(skeleton)) {
        return "no hkaSkeleton";
    }
    const u8* const parents = Read<const u8*>(skeleton + OffsetSkelParents);
    const u8* const bones = Read<const u8*>(skeleton + OffsetSkelBones);
    const u8* const pose = Read<const u8*>(skeleton + OffsetSkelPose);
    const s32 n_parents = Read<s32>(skeleton + OffsetSkelParentCount);
    const s32 n_bones = Read<s32>(skeleton + OffsetSkelCount);
    const s32 n_pose = Read<s32>(skeleton + OffsetSkelPoseCount);
    if (n_parents != C::BoneCount || n_bones != C::BoneCount || n_pose != C::BoneCount) {
        return fmt::format("hkaSkeleton arrays {} / {} / {} (expected {})", n_parents, n_bones,
                           n_pose, C::BoneCount);
    }
    if (!Plausible(parents, 2) || !Plausible(bones) || !Plausible(pose, 16)) {
        return "hkaSkeleton array pointers";
    }
    for (int i = 0; i < C::BoneCount; ++i) {
        const auto k = static_cast<size_t>(i);
        out.parents[k] = Read<s16>(parents + 2 * k);
        const auto* name = reinterpret_cast<const u8*>(Read<u64>(bones + 16 * k) & ~u64{1});
        if (!Plausible(name, 1)) {
            return fmt::format("bone {} has no name", i);
        }
        out.names[k].assign(reinterpret_cast<const char*>(name),
                            strnlen(reinterpret_cast<const char*>(name), 64));
        const u8* const t = pose + 48 * k;
        std::memcpy(out.local[k].t.data(), t, 12);
        std::memcpy(out.local[k].q.data(), t + 16, 16);
        std::memcpy(out.local[k].s.data(), t + 32, 12);
    }
    return {};
}

// c0000.flver from the game's files, or "" and why.
std::string ReadFlver(R::FlverRig& out) {
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    const auto file = mnt->ReadFile(FlverArchive);
    if (!file) {
        return fmt::format("{} not readable", FlverArchive);
    }
    const auto dcx = TpfPatch::ParseDcx(*file);
    if (!dcx || dcx->unpacked > (16u << 20)) {
        return fmt::format("{} is not a DCX", FlverArchive);
    }
    std::vector<u8> bnd(dcx->unpacked);
    mz_ulong length = dcx->unpacked;
    if (mz_uncompress(bnd.data(), &length, file->data() + dcx->stream,
                      static_cast<mz_ulong>(file->size() - dcx->stream)) != MZ_OK ||
        length != dcx->unpacked) {
        return fmt::format("{}: the zlib stream does not inflate", FlverArchive);
    }
    const auto flver = R::Bnd4Find(bnd, "c0000.flver");
    if (!flver) {
        return fmt::format("{} holds no c0000.flver", FlverArchive);
    }
    std::string why;
    auto rig = R::ParseFlver(*flver, why);
    if (!rig) {
        return "c0000.flver: " + why;
    }
    out = std::move(*rig);
    return {};
}

void Dump(const char* path, const C::Skeleton& s) {
    std::FILE* f = std::fopen(path, "w");
    if (f == nullptr) {
        LOG_WARNING(Core, "VR skeleton: VR_SKELETON_DUMP={} cannot be written", path);
        return;
    }
    const auto v = [f](const float* p, int n) {
        for (int i = 0; i < n; ++i) {
            std::fprintf(f, " %.9g", static_cast<double>(p[i]));
        }
    };
    for (int i = 0; i < C::BoneCount; ++i) {
        const auto k = static_cast<size_t>(i);
        for (const auto* t : {&s.local[k], &s.model[k]}) {
            std::fprintf(f, "%s %d", t == &s.local[k] ? "local" : "model", i);
            v(t->t.data(), 3);
            v(t->q.data(), 4);
            v(t->s.data(), 3);
            std::fprintf(f, "\n");
        }
        std::fprintf(f, "h2f %d %d\n", i, s.havok_to_flver[k]);
    }
    const auto dummy = [&](const char* name, const C::DummyPoly& d) {
        std::fprintf(f, "dummy %s %d %d", name, d.id, static_cast<int>(d.bone));
        v(d.position.data(), 3);
        v(d.forward.data(), 3);
        v(d.up.data(), 3);
        std::fprintf(f, "\n");
    };
    dummy("DmyHead7", s.head7);
    dummy("DmyLockOn", s.lock_on);
    dummy("DmyRightWeaponBase", s.right_weapon_base);
    dummy("DmyRightTwoHand", s.right_two_hand);
    dummy("DmyLeftWeaponBase", s.left_weapon_base);
    dummy("DmyLeftGun", s.left_gun);
    dummy("DmyLeftGun2", s.left_gun2);
    dummy("DmyLeftGun3", s.left_gun3);
    dummy("DmyLeftHand", s.left_hand);
    dummy("DmyHead59", s.head59);
    dummy("eye0", s.eyes[0]);
    dummy("eye1", s.eyes[1]);
    std::fprintf(f, "EyeCenterOnHead");
    v(s.eye_center_on_head.data(), 3);
    std::fprintf(f, "\n");
    const std::pair<const char*, float> lengths[] = {
        {"ClavicleOffset", s.clavicle_offset},
        {"ClavicleLength", s.clavicle_length},
        {"UpperArmLength", s.upper_arm_length},
        {"ForearmLength", s.forearm_length},
        {"HandToWeapon", s.hand_to_weapon},
        {"ReferenceShoulderToHand", s.reference_shoulder_to_hand},
        {"ReferenceShoulderToWeapon", s.reference_shoulder_to_weapon},
    };
    for (const auto& [name, value] : lengths) {
        std::fprintf(f, "%s %.9g\n", name, static_cast<double>(value));
    }
    std::fclose(f);
    LOG_INFO(Core, "VR skeleton: VR_SKELETON_DUMP wrote {}", path);
}

bool Fail(const std::string& reason) {
    g_failure = reason;
    g_state.store(Failed, std::memory_order_release);
    LOG_WARNING(Core,
                "VR skeleton: read at runtime, matches the reference: no ({}); the VR body, the "
                "gun's barrel, the vial's arm zone and the wrist gauge stay off",
                reason);
    return false;
}

} // namespace

bool EnsureSkeleton(const u8* hk_skeleton) {
    const int state = g_state.load(std::memory_order_acquire);
    if (state != NotYet) {
        return state == Ready;
    }
    std::scoped_lock lock{g_mutex};
    if (g_state.load(std::memory_order_relaxed) != NotYet) {
        return g_state.load(std::memory_order_relaxed) == Ready;
    }
    if (!Plausible(hk_skeleton)) {
        return false; // not loaded yet: the next call tries again
    }
    R::HavokRig havok;
    if (std::string why = ReadHavok(hk_skeleton, havok); !why.empty()) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG_INFO(Core, "VR skeleton: not readable yet ({}); trying again", why);
        }
        return false;
    }
    if (std::string why = R::CheckInterop(havok.names, havok.parents); !why.empty()) {
        return Fail("the game's skeleton is not c0000's: " + why);
    }
    R::FlverRig flver;
    if (std::string why = ReadFlver(flver); !why.empty()) {
        return Fail(why);
    }
    const u64 havok_digest = R::HavokDigest(havok.parents, havok.local);
    const u64 flver_digest = R::FlverDigest(flver);
    if (havok_digest != R::RetailHavokDigest || flver_digest != R::RetailFlverDigest) {
        return Fail(fmt::format(
            "not the retail v1.09 data: reference pose digest {:#018x}{}, c0000.flver digest "
            "{:#018x}{}",
            havok_digest, havok_digest == R::RetailHavokDigest ? " (ok)" : " (differs)",
            flver_digest, flver_digest == R::RetailFlverDigest ? " (ok)" : " (differs)"));
    }
    auto* skeleton = new C::Skeleton{}; // installed for the session: never freed
    if (std::string why; !R::Build(havok.local, flver, *skeleton, why)) {
        delete skeleton;
        return Fail(why);
    }
    C::Install(skeleton);
    g_state.store(Ready, std::memory_order_release);
    LOG_INFO(Core,
             "VR skeleton: read at runtime, matches the reference: yes (the reference pose of {} "
             "bones from the game's hkaSkeleton at {}, {} bones and the body's dummies from "
             "c0000.flver; digests {:#018x} {:#018x}; eye on Head ({:.4f}, {:.4f}, {:.4f}), "
             "upper arm {:.4f} m, forearm {:.4f} m)",
             C::BoneCount, fmt::ptr(hk_skeleton), flver.bones.size(), havok_digest, flver_digest,
             skeleton->eye_center_on_head[0], skeleton->eye_center_on_head[1],
             skeleton->eye_center_on_head[2], skeleton->upper_arm_length,
             skeleton->forearm_length);
    if (const char* path = std::getenv("VR_SKELETON_DUMP"); path != nullptr && *path != '\0') {
        Dump(path, *skeleton);
    }
    return true;
}

const char* SkeletonFailure() {
    return g_state.load(std::memory_order_acquire) == Failed ? g_failure.c_str() : "";
}

} // namespace VR::Bloodborne
