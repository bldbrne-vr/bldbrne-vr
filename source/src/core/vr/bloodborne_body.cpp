// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include <algorithm>
#include "common/sync_log.h"
#include <array>
#include <atomic>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "common/thread.h"
#include "core/vr/anim_classes.h"
#include "core/vr/arm_ik.h"
#include "core/vr/bb_weapons.h"
#include "core/vr/blade_sounds.h"
#include "core/vr/game_sounds.h"
#include "core/vr/grind_audio.h"
#include "core/vr/bloodborne_body.h"
#include "core/vr/grip_lab_pose.h"
#include "core/vr/bloodborne_visceral_pull.h"
#include "core/vr/bloodborne_cinematic.h"
#include "core/vr/bloodborne_evasion.h"
#include "core/vr/bow.h"
#include "core/vr/bow_pose.h"
#include "core/vr/combat_channel.h"
#include "core/vr/bloodborne_gun.h"
#include "core/vr/bloodborne_held.h"
#include "core/vr/bloodborne_layout.h"
#include "core/vr/bloodborne_facing.h"
#include "core/vr/bloodborne_melee.h"
#include "core/vr/bloodborne_motion.h"
#include "core/vr/bloodborne_skeleton.h"
#include "core/vr/body_channel.h"
#include "core/vr/body_feedback.h"
#include "core/vr/body_frame.h"
#include "core/vr/body_posture.h"
#include "core/vr/body_settings.h"
#include "core/vr/body_solver.h"
#include "core/vr/c0000_skeleton.h"
#include "core/vr/cinematic_state.h"
#include "core/vr/daze.h"
#include "core/vr/feature_cost.h"
#include "core/vr/grip_adjust.h"
#include "core/vr/gun_aim.h"
#include "core/vr/hand_grip.h"
#include "core/vr/lever_channel.h"
#include "core/vr/lever_grip.h"
#include "core/vr/music_box_grip.h"
#include "core/vr/head_room.h"
#include "core/vr/hit_stun.h"
#include "core/vr/hook_util.h"
#include "core/vr/bloodborne_visceral_stage.h"
#include "core/vr/hook_wrap.h"
#include "core/vr/live_tune.h"
#include "core/vr/player_body.h"
#include "core/vr/pose_refresh.h"
#include "core/vr/quick_settings.h"
#include "core/vr/solid_weapon.h"
#include "core/vr/trick_weapons.h"
#include "core/vr/two_hand_channel.h"
#include "core/vr/weapon_wall.h"

namespace VR::Bloodborne {

namespace {

namespace B = VR::Body;
namespace C = VR::Body::C0000;
namespace L = Layout;
using Hook::Plausible;
using Hook::Read;

// Module-relative addresses in the 1.09 eboot (patch file address - 0x400000).
// ChrIns pose copy, `void(ChrIns*)`: unless the behaviour module is idle it copies the behaviour
// graph's local pose into the character's hkaPose (setPoseLocalSpace 0x6C49B0 through 0x1DF0C60:
// every bone flag = 2, model out of sync, local in sync).
constexpr u64 PoseCopyRva = 0x18BE020;
constexpr std::array<u8, 23> PoseCopySig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53,       // push rbp; mov rbp, rsp; push r14; push rbx
    0x48, 0x8B, 0x87, 0xB0, 0x03, 0x00, 0x00,       // mov rax, [rdi+0x3B0]
    0x48, 0x8B, 0x58, 0x30, 0x48, 0x85, 0xDB, 0x74, // mov rbx, [rax+0x30]; test rbx, rbx; jz
    0x5C,
};
// Its vtable slot (+0xE0) in both PlayerIns vtables, 0x538F310 and the subclass 0x538FA80:
// R_X86_64_RELATIVE, 0 in the file, base + 0x18BE020 after relocation. The ChrIns base and the
// other characters' vtables (0x538E3B0, 0x538ECA0) are never touched.
constexpr std::array<u64, 2> PoseCopySlotRvas = {0x538F3F0, 0x538FB60};
// VR_BODY_HOOK=code: the function's tail, where every path ends: `add r14, 0x38; mov rdi,
// [rbx+0x10]; mov rsi, r14; pop rbx; pop r14; pop rbp; jmp 0x1DF0C60` (rbx = the behaviour module,
// [rbx+0x08] its character, r14 = the pose node, rsp 16-byte aligned). It runs for every character.
constexpr u64 PoseTailRva = 0x18BE07F;
constexpr std::array<u8, 20> PoseTailSig = {
    0x49, 0x83, 0xC6, 0x38,       // add r14, 0x38
    0x48, 0x8B, 0x7B, 0x10,       // mov rdi, [rbx+0x10]
    0x4C, 0x89, 0xF6,             // mov rsi, r14
    0x5B, 0x41, 0x5E, 0x5D,       // pop rbx; pop r14; pop rbp
    0xE9, 0xCD, 0x2B, 0x53, 0x00, // jmp 0x1DF0C60
};
constexpr u64 SetPoseRva = 0x1DF0C60;
// ChrIns::GetDmyPolyMtx, `s32 (ChrIns*, Mtx34* out, const s32* id, u32 max)`: world 3x4 rows of the
// dummy polys with that id. Log mode only (the FK and target checks), called from the render-camera
// hook.
constexpr u64 DmyPolyRva = 0x18C3BF0;
constexpr std::array<u8, 32> DmyPolySig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC,
    0x28, 0x41, 0x89, 0xCF, 0x48, 0x89, 0xD3, 0x48, 0x89, 0x75, 0xB0, 0x4C, 0x8B, 0x35, 0xCE, 0xFF,
};
// PlayerIns vt+0x388, `s32 GetWeaponId(PlayerIns*, s32 hand)` (GetBehaviorId's weapon): equip
// data E = [chr+0x4B8], hand -1 (right) -> slot [E+0x10] * 2 + 1, -2 (left) -> [E+0x0C] * 2, the
// id is s32 [E + 0x64 + 4 * slot]. Read directly (the whole function is the signature: it holds
// the layout), never called.
// blade_sounds (BladeSounds): the game's PlaySound, `void(u64* handle_out, const float pos[3], s32
// type, s32 id)` (TAE event 128's call), and event 128's sound type for SoundType 1 (`lea rcx,
// [table]; cmp byte [rcx+3], 0; mov ecx, 0xA; jnz; mov ecx, 1`), as core/vr/bloodborne_quickitems.cpp.
constexpr u64 PlaySoundRva = 0x13E0290;
constexpr std::array<u8, 24> PlaySoundSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x81, 0xEC,
    0x28, 0x01, 0x00, 0x00, 0x49, 0x89, 0xF6, 0x48, 0x89, 0xFB, 0x4C, 0x8B,
};
constexpr u64 SoundTypeRva = 0x1A2641B;
constexpr std::array<u8, 23> SoundTypeSig = {
    0x48, 0x8D, 0x0D, 0x26, 0xE5, 0xD0, 0x02, 0x80, 0x79, 0x03, 0x00, 0xB9,
    0x0A, 0x00, 0x00, 0x00, 0x75, 0x05, 0xB9, 0x01, 0x00, 0x00, 0x00,
};
constexpr u64 WeaponIdRva = 0x18FB090;
constexpr std::array<u8, 85> WeaponIdSig = {
    0x48, 0x8B, 0x87, 0xB8, 0x04, 0x00, 0x00, 0x8D, 0x4E, 0x06, 0x83, 0xF9, 0x05, 0x77, 0x3E,
    0x48, 0x8D, 0x15, 0x42, 0x00, 0x00, 0x00, 0x48, 0x63, 0x0C, 0x8A, 0x48, 0x01, 0xD1, 0xFF,
    0xE1, 0x8B, 0x48, 0x14, 0x8D, 0x74, 0x09, 0x04, 0xEB, 0x25, 0x8B, 0x48, 0x18, 0x8D, 0x74,
    0x09, 0x04, 0xEB, 0x1C, 0x8B, 0x48, 0x1C, 0xEB, 0x03, 0x8B, 0x48, 0x20, 0x8D, 0x74, 0x09,
    0x05, 0xEB, 0x0E, 0x8B, 0x70, 0x0C, 0x01, 0xF6, 0xEB, 0x07, 0x8B, 0x48, 0x10, 0x8D, 0x74,
    0x09, 0x01, 0x48, 0x63, 0xCE, 0x8B, 0x44, 0x88, 0x64, 0xC3,
};
constexpr u32 OffsetChrEquip = 0x4B8;
constexpr u32 OffsetEquipLeftSlot = 0x0C;
constexpr u32 OffsetEquipRightSlot = 0x10;
constexpr u32 OffsetEquipIds = 0x64;
// Log only [U]: the ChrAsm model set [chr+0x4D0] holds the weapon part objects at +0x28 + slot * 8
// (model slots 11-14: the right weapon's parts 0-3, vtable 0x539D100); a part attached to a body
// dummy has +0x138 = 2 and the dummy id at +0x13C (WeaponPart::AttachToDmy 0x1CA5760).
constexpr u32 OffsetChrAsm = 0x4D0;
constexpr u32 OffsetAsmParts = 0x28;
constexpr u64 WeaponPartVtableRva = 0x539D100;
constexpr u32 OffsetPartMode = 0x138;
constexpr u32 OffsetPartDummy = 0x13C;
constexpr s32 RightPartSlot = 11;
constexpr u64 WorldChrManRva = 0x553E878;
constexpr u32 OffsetMainPlayer = 0x60;
// Debug-menu byte "character update: multi-threaded" (1 in the data segment): while set,
// PlayerIns::Update (site A) and vt+0xE8 run as job-system jobs; the pose copy always runs inline
// on the loop thread, before the render-camera hook.
constexpr u64 ChrUpdateJobsRva = 0x5128342;

constexpr u32 OffsetChrModules = 0x3B0;
constexpr u32 OffsetBehaviorModule = 0x30;
constexpr u32 OffsetChrCtrl = 0x58;
constexpr u32 OffsetCtrlOwner = 0x08;
constexpr u32 OffsetCtrlWorld = 0x90;    // W: three float4 rows {R row i, t_i}
constexpr u32 OffsetCtrlBoneMap = 0x1B8; // {u32 FLVER bone count, +0x08 s16* Havok bone}
// SprjChrBehaviorModule: +0x10 the hkb wrapper (its +0x18 is set while the graph runs), +0x28 the
// pose node (hkaPose at +0x38), +0x384 the "update pose" byte.
constexpr u32 OffsetModWrapper = 0x10;
constexpr u32 OffsetWrapperGraph = 0x18;
constexpr u32 OffsetModNode = 0x28;
constexpr u32 OffsetModActive = 0x384;
constexpr u32 OffsetNodePose = 0x38;
// hkaPose: skeleton, local {data, size}, model, bone flags {data, size}, the two in-sync bytes.
constexpr u32 OffsetPoseSkeleton = 0x00;
constexpr u32 OffsetPoseLocal = 0x08;
constexpr u32 OffsetPoseLocalSize = 0x10;
constexpr u32 OffsetPoseFlags = 0x28;
constexpr u32 OffsetPoseFlagsSize = 0x30;
constexpr u32 OffsetPoseSync = 0x38; // u8 model in sync, u8 local in sync
constexpr u16 SyncLocalOnly = 0x0100;
constexpr u32 BoneFlagCopied = 2;
// hkaSkeleton: parents {s16*} +0x18, bones {16 bytes each, hkStringPtr name (bit 0 = owned)}
// +0x28, bone count +0x30.
constexpr u32 OffsetSkelParents = 0x18;
constexpr u32 OffsetSkelBones = 0x28;
constexpr u32 OffsetSkelCount = 0x30;

constexpr s64 Ms = 1'000'000;
// A valid anchor still drives the body this long (a hitch between the camera and the pose copy);
// an older one eases the body out.
constexpr s64 AnchorMaxAgeNs = 1000 * Ms;
constexpr s64 HandsMaxAgeNs = 100 * Ms; // an older sample: the XR frame thread stopped
constexpr s64 HoldNs = 300 * Ms;        // tracking lost: the last pose is held this long
constexpr float SlowBlendOutS = 0.4f;   // an arm going back after a tracking loss or death
constexpr s64 WindowNs = 500 * Ms;      // VR_BODY_LOG lines
constexpr s64 DetailNs = 2000 * Ms;     // VR_BODY_LOG=2: per-frame lines after a MARK
constexpr s64 SnapshotMaxAgeNs = 50 * Ms;
constexpr s64 FailLogNs = 5000 * Ms;  // solve failures are logged at most this often
constexpr s64 PerfLogNs = 60000 * Ms; // the always-on perf summary line
// body_back_down_m ramps in between these view pitches (radians, + = down).
constexpr float BackDownStart = 25.0f * B::Deg;
constexpr float BackDownFull = 60.0f * B::Deg;
constexpr u64 RecordMaxBytes = u64{2} << 30;

// ---- Environment switches (VR_ARM_* are accepted for VR_BODY_*) -------------------------------

const char* Env(const char* name, const char* alias) {
    const char* value = std::getenv(name);
    return value != nullptr ? value : std::getenv(alias);
}

// VR_BODY_DRYRUN: solve the whole body every fresh frame and log, never write.
bool DryRun() {
    static const bool enabled = Env("VR_BODY_DRYRUN", "VR_ARM_DRYRUN") != nullptr;
    return enabled;
}

// VR_BODY_LOG: 1 = a pose and a view line every 0.5 s and the arm owner edges; 2 = also one line
// per frame for 2 s after each MARK. The dry run logs at level 1 at least.
int LogLevel() {
    static const int level = [] {
        const char* value = Env("VR_BODY_LOG", "VR_ARM_LOG");
        const int requested = value != nullptr ? std::max(std::atoi(value), 1) : 0;
        return std::max(requested, DryRun() ? 1 : 0);
    }();
    return level;
}

// VR_BODY_POSTURE_LOG=<n>: every n-th fresh solve (1 = every one) a `VR posture:` line with the
// hunter's body shape before (the game's animation) and after the body's edits: eye, trunk lean
// and length, waist twist, the chest's offset from the hips, and what moved them (the eye goal,
// the upper body's shift, the torso yaw, the lean). 0 / unset = off.
int PostureLogEvery() {
    static const int every = [] {
        const char* value = std::getenv("VR_BODY_POSTURE_LOG");
        return value != nullptr ? std::max(std::atoi(value), 1) : 0;
    }();
    return every;
}

// VR_ANIM_LOG: every animation id the TimeAct ring receives, with its class, its entry's three
// floats and what it does to the arms.
bool AnimLog() {
    static const bool enabled = std::getenv("VR_ANIM_LOG") != nullptr;
    return enabled;
}

// VR_BODY_RECORD=<file>: one RecordFrame per fresh pose copy (core/vr/body_frame.h).
const std::string& RecordPath() {
    static const std::string path = [] {
        const char* value = Env("VR_BODY_RECORD", "VR_ARM_RECORD");
        return value != nullptr ? std::string{value} : std::string{};
    }();
    return path;
}

enum class HookMethod { Slot, Code, Off };

// VR_BODY_HOOK=slot (default) | code | off.
HookMethod Method() {
    static const HookMethod method = [] {
        const char* value = std::getenv("VR_BODY_HOOK");
        const std::string text = value != nullptr ? value : "slot";
        if (text == "code") {
            return HookMethod::Code;
        }
        if (text == "off") {
            return HookMethod::Off;
        }
        if (text != "slot") {
            LOG_WARNING(Core, "VR body: VR_BODY_HOOK={} is not slot, code or off; using slot",
                        text);
        }
        return HookMethod::Slot;
    }();
    return method;
}

const char* MethodName(HookMethod method) {
    switch (method) {
    case HookMethod::Slot:
        return "vtable slots";
    case HookMethod::Code:
        return "tail patch";
    case HookMethod::Off:
        return "off";
    }
    return "?";
}

// ---- Install state (set before the hook can run) ------------------------------------------------

using DmyPolyFn = s32(PS4_SYSV_ABI*)(void* chr, void* out, const s32* ids, u32 max);

u8* g_base = nullptr; // the eboot (BodyEbootLoaded)
u64 g_size = 0;
bool g_relocated = false; // OnEbootRelocated ran
bool g_tried = false;     // the install was decided (either path)
std::atomic_bool g_installed{false};
std::atomic_bool g_disabled{false};
std::unique_ptr<Xbyak::CodeGenerator> g_code;
DmyPolyFn g_dmy_poly = nullptr;
// The engine code behind each animation gate signal matched at install (compared, never patched):
// the TimeAct history push, HKS act 8 (event flag), IsInThrow, HKS act 26 (ladder state), the TAE
// JumpTable events behind the cancel windows.
bool g_gate_ring = false;
bool g_gate_event = false;
bool g_gate_throw = false;
bool g_gate_ladder = false;
bool g_gate_cancel = false;
bool g_weapon_ok = false; // PlayerIns vt+0x388 matched: the equipped weapon ids can be read
using PlaySoundFn = void(PS4_SYSV_ABI*)(u64* handle, const float* pos, s32 type, s32 id);
PlaySoundFn g_play_sound = nullptr; // blade_sounds (null: the signatures did not match)
const u8* g_sound_table = nullptr;

// VR_BLADE_SOUND_LOG (tests): every blade sound logged, not just the first 20.
bool BladeSoundLog() {
    static const bool on = std::getenv("VR_BLADE_SOUND_LOG") != nullptr;
    return on;
}
std::atomic<u64> g_other_calls{0}; // pose copies of other characters (phantoms; every NPC in the
                                   // tail patch)
std::atomic<u64> g_busy_calls{0};  // a main-player call while another one ran
std::atomic_flag g_busy;           // clear: no main-player call running

std::string Rva(const u8* pointer) {
    if (g_base != nullptr && pointer >= g_base && pointer < g_base + g_size) {
        return fmt::format("+{:#x}", static_cast<u64>(pointer - g_base));
    }
    return fmt::format("{}", fmt::ptr(pointer));
}

std::string Pos(B::Vec3 v) {
    return fmt::format("({:.3f},{:.3f},{:.3f})", v.x, v.y, v.z);
}

std::string Cm(B::Vec3 v) {
    return fmt::format("({:.1f},{:.1f},{:.1f})", v.x * 100.0f, v.y * 100.0f, v.z * 100.0f);
}

u8* MainPlayer() {
    u8* const world = Read<u8*>(g_base + WorldChrManRva);
    u8* const chr = Plausible(world) ? Read<u8*>(world + OffsetMainPlayer) : nullptr;
    return Plausible(chr) ? chr : nullptr;
}

// W of the character (ChrCtrl+0x90), when its ChrCtrl names it as the owner.
bool ReadWorld(const u8* chr, std::array<float, 12>& rows) {
    u8* const ctrl = Read<u8*>(chr + OffsetChrCtrl);
    if (!Plausible(ctrl) || Read<const u8*>(ctrl + OffsetCtrlOwner) != chr) {
        return false;
    }
    std::memcpy(rows.data(), ctrl + OffsetCtrlWorld, sizeof(rows));
    return true;
}

void Disable(const std::string& reason) {
    if (!g_disabled.exchange(true)) {
        LOG_WARNING(Core, "VR body: DISABLED ({}); the game's own pose stays as it is", reason);
        // The presenter plays the deny pattern on both hands (body_haptics).
        B::PushBodyEvent({B::BodyEventKind::Disabled, -1});
    }
}

B::HideMode HideModeOf(B::BodyMode mode) {
    switch (mode) {
    case B::BodyMode::NoHead:
        // body_head_shadow: the head keeps its size (it casts its shadow); the renderer leaves it
        // out of the first-person colour passes instead (vk_rasterizer_vr_draws.cpp).
        return B::HeadHiddenByRenderer() ? B::HideMode::None : B::HideMode::Head;
    case B::BodyMode::ArmsOnly:
        // The renderer draws only his hands and forearms in the first-person colour passes
        // (vk_rasterizer_vr_draws.cpp, vr_hands_table.h): nothing is scaled, the shadow maps keep
        // his whole body. VR_TUNE_FILE body.arms_draw=0: the old Spine scaling (A/B tests).
        return B::DrawnArmsNow() == B::DrawnArms::Forearms ? B::HideMode::None
                                                           : B::HideMode::ArmsOnly;
    case B::BodyMode::HandsOnly: // the renderer draws only his hands; nothing is scaled
    case B::BodyMode::Full:
        break;
    }
    return B::HideMode::None;
}

B::BodyParams Params(const B::BodySettings& b) {
    B::BodyParams p{};
    p.torso_follow = b.torso_follow;
    // VR_TUNE_FILE body.torso_rel=0: the old world-anchored torso follow (A/B tests only).
    p.torso_relative = VR::Tune::Get("body.torso_rel", 1.0f) != 0.0f;
    p.torso_deadzone = b.torso_deadzone_deg * B::Deg;
    p.torso_max = b.torso_max_deg * B::Deg;
    p.torso_tau = b.torso_follow_ms * 0.001f;
    p.align = b.body_align;
    p.lean_side_max = b.body_lean_side_deg * VR::Body::Deg;
    // VR_TUNE_FILE: live A/B (tests): body.posture 0 = shift, 1 = lean; body.vdead_m.
    p.posture = VR::Tune::Get("body.posture", b.body_posture_lean ? 1.0f : 0.0f) != 0.0f
                    ? B::PostureMode::Lean
                    : B::PostureMode::Shift;
    p.vertical_deadband = VR::Tune::Get("body.vdead_m", b.body_vertical_deadband_m);
    // body.duck_lean_deg / body.shoulder_reach_deg: live A/B of the duck's lean and the shoulder's
    // reach (tests).
    p.lean_fwd_low_max = VR::Tune::Get("body.duck_lean_deg", b.body_duck_lean_deg) * B::Deg;
    p.shoulder_reach = VR::Tune::Get("body.shoulder_reach_deg", b.body_shoulder_reach_deg) * B::Deg;
    // body.natural_arms 0/1, body.pronation_deg / body.supination_deg / body.twist_max_deg: live
    // A/B of the natural arms (tests).
    p.natural_arms =
        VR::Tune::Get("body.natural_arms", b.body_natural_arms ? 1.0f : 0.0f) != 0.0f;
    p.forearm_pronation =
        VR::Tune::Get("body.pronation_deg", b.body_forearm_pronation_deg) * B::Deg;
    p.forearm_supination =
        VR::Tune::Get("body.supination_deg", b.body_forearm_supination_deg) * B::Deg;
    p.wrist_twist_max = VR::Tune::Get("body.twist_max_deg", b.body_wrist_twist_max_deg) * B::Deg;
    p.align_max = b.body_align_max_m;
    p.back = VR::Tune::Get("body.back_m", b.body_back_m);
    p.align_tau = b.body_align_ms * 0.001f;
    p.hide = HideModeOf(B::EffectiveBodyMode());
    return p;
}

// ---- Main-player pose copy (loop thread; the busy flag keeps it to one call at a time) ----------

struct ArmWindow {
    u32 solved{0};
    u32 reached{0};
    u32 detached{0};
    u32 failed{0};
    u32 held{0};
    u32 lost{0};
    u32 detaches{0}; // new detaches (DetachWatch; each may tick the hand)
    float max_error{0.0f};
};

// One new TimeAct entry (VR_ANIM_LOG prints its floats).
struct NewAnim {
    s32 id{-1};
    std::array<float, 3> f{};
};

enum GateSignal : u32 { GateRing, GateEvent, GateThrow, GateLadder, GateCancel, GateSignalCount };

// Solid weapons (vr.ini weapon_wall, SolidWeapons): each hand's weapon against the walls, its
// contact's feel (core/vr/weapon_wall.h Contact), the two weapons' clash, what goes to the
// presenter.
struct SolidSide {
    std::array<VR::Solid::Weapon, 2> weapon{};
    std::array<VR::WeaponWall::Contact, 2> touch{};
    VR::WeaponWall::Contact clash{};
    std::array<VR::Solid::Pose, 2> last{}; // the controller's pose for each weapon, last felt
    std::array<bool, 2> have_last{};
    s64 last_ns{0};                        // ... then
    float yaw{0.0f};                       // base_yaw last update (a snap turn carries them)
    bool have_yaw{false};
    B::Vec3 clash_side{}; // the two weapons' touch normal last update (they keep to its side)
    float clash_share{0.5f}; // right hand's share of the separation
    bool clash_met{false};
    bool active{false}; // on last update (off: let go once, then nothing runs)
    std::array<int, 2> shape_logged{-1, -1}; // (logs) 0 the hand, 1 its line
    std::array<bool, 2> touch_logged{};
    VR::WeaponWall::State state{};
};

// Blade sounds (vr.ini blade_sounds, BladeSounds): the two weapons meeting, whether they are solid
// or pass through each other.
struct BladeSide {
    VR::BladeSound::Director director{};
    std::array<VR::Solid::Pose, 2> last{}; // each hand's weapon pose, last heard
    bool have_last{false};
    s64 last_ns{0};
    float yaw{0.0f};
    bool have_yaw{false};
    B::Vec3 side{}; // from the left line toward the right one, last update
    bool have_side{false};
    // weapon_wall on: the solid clash of this update (SolidWeapons), heard in its place.
    bool solid_fresh{false};
    VR::BladeSound::Sample solid{};
    B::Vec3 solid_point{};
    u32 played{0};
    float grind_pan{0.0f}; // the grind's place (core/vr/grind_audio.h), last heard
};

struct PoseSide {
    VR::GrabCamera::ThrowPlacementController throw_controller{};
    VR::GrabCamera::ThrowPlacement throw_placement{};
    const u8* throw_player{};
    VR::GrabCamera::Sample grab_eye{};
    bool throw_seen{false}; // an outgoing freehand throw was published (its camera holds)
    s32 throw_clip{-1};     // ... the throw's clip (the gate's owner while the throw holds)
    bool throw_over{false}; // another clip took over this throw: the camera lets go
    bool first_logged{false};
    const u8* checked_skeleton{nullptr};
    bool skeleton_wait_logged{false};
    bool map_checked{false};
    // The inputs of this fresh frame. `anchor.eye_rel` is the eye this frame is rendered from
    // (NextEyeRel: a pair-locked second eye stays where its first eye was).
    B::Anchor anchor{};
    B::ChrFrame frame{};
    B::HandsSample used{}; // the hand sample the arms use (reused for a pair's second eye)
    bool have_used{false};
    bool reused{false};
    s64 anchor_age_ns{-1};
    s64 hands_age_ns{-1};
    const char* inactive{"no frame yet"}; // why the body is not driven (nullptr = driven)
    // `inactive` is a change of the view (no first-person anchor, cinematic flat, the virtual
    // screen): everything resets at once. Otherwise (an anchor over 1 s old, W not a rotation, a
    // non-finite anchor) the weights ease out with the last inputs.
    bool view_off{true};
    // Stereo pairs (core/vr/body_frame.h EyeTracker, fed every pose copy): the eye of this frame,
    // the predicted W drift and a second eye's pair lock decision (published for the camera).
    B::EyeTracker eyes{};
    B::EyeSource eye_source{B::EyeSource::Anchor};
    B::Vec3 drift{};
    // The last driven frame's inputs, kept while the body eases out.
    bool have_inputs{false};
    B::Vec3 last_eye_m{};
    B::Vec3 last_ahead_m{}; // the camera's lead over the hunter's eyes (fp_ahead_m), model space
    float last_head_yaw{0.0f};
    float last_view_pitch{0.0f};
    float last_chr_yaw{0.0f}; // W's heading at the last pose copy with a valid W
    // The body (core/vr/body_solver.h) on a host copy of the game's locals, and who owns the arms
    // (core/vr/anim_classes.h).
    std::array<B::QsTransform, C::BoneCount> locals{};
    B::BodySolver solver{};
    B::BodyOutput out{};
    bool solved{false}; // `out` is this frame's
    B::AnimGate gate{};
    B::GateOut gate_out{};
    bool roomscale_idle{false};
    const char* roomscale_idle_why{""}; // why not (the room-scale walk's log)
    std::array<B::HandHold, 2> hold{};
    u32 hold_space{0}; // the tracking space `hold` is in (HandsSample::space_serial)
    std::array<B::HandUse, 2> use{B::HandUse::Lost, B::HandUse::Lost};
    std::array<B::ModelTarget, 2> target{}; // each arm's last target (kept while it blends out)
    SolidSide solid{};                      // weapon_wall: the weapons against the walls
    BladeSide blade{};                      // blade_sounds: the two weapons meeting
    std::array<bool, 2> have_target{};
    std::array<B::BlendWeight, 2> arm_w{};
    std::array<float, 2> arm_weight{};
    B::BlendWeight body_w{};
    float body_weight{0.0f};
    s64 last_solve_ns{0};
    // The game's pose: our edits written and not overwritten by a fresh copy since, where (the
    // character, its pose node and local array: RestoreGame writes only into that pose), and the
    // game's own locals of those bones (RestoreGame).
    bool edits_in_pose{false};
    const u8* edits_chr{nullptr};
    const u8* edits_pose{nullptr};
    const B::HkQsTransform* edits_local{nullptr};
    std::bitset<C::BoneCount> edited{};
    std::array<B::HkQsTransform, C::BoneCount> animated{};
    std::array<B::HkQsTransform, C::BoneCount> written_locals{};
    u32 held_refreshes{};
    s64 last_pose_call_ns{}, max_pose_gap_ns{};
    bool first_write_logged{false};
    // The held item's grip (core/vr/hand_grip.h): its kind and how far the fingers are closed.
    s32 grip_goods{-1};
    u8 grip_kind{0};
    float grip_weight{0.0f};
    s64 grip_log_ns{0}; // VR_GRIP_LOG
    u64 writes{0};
    s64 last_fail_log_ns{0};
    // The right hand's weapon (read with PlayerIns vt+0x388's layout) and its grip (vr.ini
    // weapon_grip_*: fetched again when the weapon or the saved grips change).
    s32 weapon_id{-2}; // -2 = not read yet, -1 = unknown
    s32 weapon_slot{-1};
    s32 weapon_family{-1};
    s32 left_weapon_id{-2};
    s32 left_weapon_slot{-1};
    u32 grip_generation{0};
    bool grip_fetched{false};
    B::GripTrim grip{};
    bool grip_custom{false};
    // Hands off the controller (the haptic tick; the ghost hand on the presenter).
    std::array<B::DetachWatch, 2> detach{};
    // The time budget (always on) and its 60 s summary line.
    B::BudgetGuard budget{};
    s64 perf_start{0};
    u64 perf_calls{0};
    s64 perf_sum_ns{0};
    s64 perf_max_ns{0};
    u64 perf_over{0}; // single calls over the budget
    u64 perf_writes{0};
    u64 perf_detaches{0};
    u32 status_seq{0};
    // TimeAct ring tracking (the entries pushed since the last fresh copy).
    const u8* ring_module{nullptr};
    s32 ring_write{-1};
    std::array<NewAnim, B::MaxNewIds> new_anims{};
    u32 new_count{0};
    u32 guard_serial{0}; // guard reactions seen (BodyStatus::guard_serial) ...
    s32 guard_clip{-1};  // ... and the newest one's clip
    std::array<bool, GateSignalCount> gate_warned{};
    // VR_ANIM_LOG: the cancel windows' last values (a line on every change).
    u64 last_allow{~u64{0}};
    bool last_move{false};
    std::array<s32, 64> seen_unknown{}; // ids of unknown or unverified classes logged once
    u32 seen_unknown_count{0};
    bool seen_unknown_full{false}; // the one "table full" line was logged
    // VR_BODY_LOG window.
    s64 window_start{0};
    u32 calls{0};
    u32 fresh{0};
    u32 stale{0};
    u32 active{0};
    u32 easing{0};
    u32 reuses{0};
    u32 eye_locked{0};
    u32 eye_pair{0};
    u32 glitches{0};
    u32 drawn_heading{0}; // pose copies placed in W turned to his physics heading (as drawn)
    u32 window_writes{0};
    u32 failures{0};
    const char* fail_why{""};
    const char* stale_why{""};
    float torso_max{0.0f};
    float shift_max{0.0f};
    float residual_max{0.0f};
    s64 time_sum_ns{0};
    s64 time_max_ns{0};
    std::array<ArmWindow, 2> arm{};
    u32 marks_seen{0};
    s64 detail_until_ns{0};
    // VR_BODY_POSTURE_LOG (scratch off the guest thread's stack).
    std::array<B::QsTransform, C::BoneCount> posture_model{};
    B::Posture posture_anim{};
    u64 posture_frames{0};
    // Your height's eye offset (model units) last logged (NaN: the first solve logs it), the body's
    // offset logged with it and when (As the player your head above the origin moves the body's).
    float logged_game_eye{std::numeric_limits<float>::quiet_NaN()};
    float logged_body_eye{std::numeric_limits<float>::quiet_NaN()};
    s64 logged_body_ns{0};
    std::array<B::QsTransform, C::BoneCount> standing_pose{};
    const u8* standing_player{};
    bool levelup_pose{};
    s32 posture_anim_id{-1}; // the newest animation id the ring gave
    B::Vec3 posture_last_t{};
    s64 posture_last_ns{0};
    // VR_BODY_RECORD (the frame is kept here, off the guest thread's stack).
    std::FILE* record{nullptr};
    bool record_failed{false};
    u64 record_bytes{0};
    u32 record_unflushed{0};
    B::RecordFrame record_frame{};
};

PoseSide& Pose() {
    static PoseSide side;
    return side;
}

// What the render-camera hook needs from this frame's pose copy (W-drift, FK and target checks).
// The copy and the hook run on the loop thread in that order; the mutex keeps a pose copy on
// another thread (the single-character update path) safe.
struct Snapshot {
    s64 ns{0};
    const u8* chr{nullptr};
    std::array<float, 12> w{};
    std::array<B::HkQsTransform, C::BoneCount> locals{}; // the game's locals after the copy (and
                                                         // after the body's write)
    bool wrote{false};
    std::array<float, 2> arm_weight{};
    std::array<bool, 2> arm_solved{};
    std::array<B::ModelTarget, 2> target{};
    s32 right_family{-1}; // the hands' weapon families (core/vr/bb_weapons.h)
    s32 left_family{-1};
};
std::mutex g_snapshot_mutex;
Snapshot g_snapshot{};

// The main player's pose node, structurally checked (not whether it is fresh), or nullptr and why.
u8* PoseNode(const u8* chr, const char*& why) {
    u8* const mods = Read<u8*>(chr + OffsetChrModules);
    u8* const mod = Plausible(mods) ? Read<u8*>(mods + OffsetBehaviorModule) : nullptr;
    if (!Plausible(mod)) {
        why = "no behaviour module";
        return nullptr;
    }
    u8* const node = Read<u8*>(mod + OffsetModNode);
    if (!Plausible(node)) {
        why = "no pose node";
        return nullptr;
    }
    u8* const pose = node + OffsetNodePose;
    if (Read<s32>(pose + OffsetPoseLocalSize) != C::BoneCount ||
        !Plausible(Read<u8*>(pose + OffsetPoseLocal), 16)) {
        why = "local array";
        return nullptr;
    }
    return pose;
}

// The pose node the original just copied into, or nullptr and the reason it did not: the guard of
// 0x18BE020 and 0x1DF0C60, then the state setPoseLocalSpace leaves (model out of sync, local in
// sync, bone flags 2) and the 108-bone arrays.
u8* FreshPose(const u8* chr, const char*& why) {
    u8* const mods = Read<u8*>(chr + OffsetChrModules);
    u8* const mod = Plausible(mods) ? Read<u8*>(mods + OffsetBehaviorModule) : nullptr;
    if (!Plausible(mod)) {
        why = "no behaviour module";
        return nullptr;
    }
    u8* const wrapper = Read<u8*>(mod + OffsetModWrapper);
    if (Read<u8>(mod + OffsetModActive) == 0 || !Plausible(wrapper) ||
        Read<u64>(wrapper + OffsetWrapperGraph) == 0) {
        why = "module idle";
        return nullptr;
    }
    u8* const pose = PoseNode(chr, why);
    if (pose == nullptr) {
        return nullptr;
    }
    if (Read<u16>(pose + OffsetPoseSync) != SyncLocalOnly) {
        why = "sync state";
        return nullptr;
    }
    u8* const flags = Read<u8*>(pose + OffsetPoseFlags);
    if (Read<s32>(pose + OffsetPoseFlagsSize) != C::BoneCount || !Plausible(flags, 4) ||
        Read<u32>(flags) != BoneFlagCopied ||
        Read<u32>(flags + 4 * C::R_UpperArm) != BoneFlagCopied ||
        Read<u32>(flags + 4 * (C::BoneCount - 1)) != BoneFlagCopied) {
        why = "bone flags";
        return nullptr;
    }
    return pose;
}

// The skeleton behind the pose must be c0000's (count, parents, names); checked whenever its
// pointer changes. A mismatch turns the body off.
bool ValidateSkeleton(PoseSide& s, const u8* pose) {
    const u8* const skeleton = Read<u8*>(pose + OffsetPoseSkeleton);
    if (skeleton != nullptr && skeleton == s.checked_skeleton) {
        return true;
    }
    std::string reason;
    if (!Plausible(skeleton)) {
        reason = "no skeleton";
    } else if (const u32 count = Read<u32>(skeleton + OffsetSkelCount); count != C::BoneCount) {
        reason = fmt::format("{} bones (expected {})", count, C::BoneCount);
    } else {
        const u8* const parents = Read<u8*>(skeleton + OffsetSkelParents);
        const u8* const bones = Read<u8*>(skeleton + OffsetSkelBones);
        if (!Plausible(parents, 2) || !Plausible(bones)) {
            reason = "no parent or bone array";
        }
        for (int i = 0; i < C::BoneCount && reason.empty(); ++i) {
            const s16 parent = Read<s16>(parents + 2 * i);
            if (parent != C::Parents[static_cast<size_t>(i)]) {
                reason = fmt::format("bone {} parent {} (expected {})", i, parent,
                                     C::Parents[static_cast<size_t>(i)]);
                break;
            }
            const auto* name = reinterpret_cast<const char*>(Read<u64>(bones + 16 * i) & ~u64{1});
            const std::string_view expected = C::BoneNames[static_cast<size_t>(i)];
            if (!Plausible(reinterpret_cast<const u8*>(name), 1) ||
                std::strncmp(name, expected.data(), expected.size()) != 0 ||
                name[expected.size()] != '\0') {
                reason = fmt::format("bone {} name differs (expected {})", i, expected);
            }
        }
    }
    if (!reason.empty()) {
        Disable(fmt::format("skeleton check: {}", reason));
        return false;
    }
    // The skeleton's numbers (reference pose, dummies, lengths), read from the game once
    // (core/vr/bloodborne_skeleton.cpp; usually already done by the render-camera tick).
    if (!EnsureSkeleton(skeleton)) {
        if (const char* failure = SkeletonFailure(); *failure != '\0') {
            Disable(fmt::format("skeleton: {}", failure));
        } else if (!s.skeleton_wait_logged) {
            s.skeleton_wait_logged = true;
            LOG_INFO(Core, "VR body: waiting for the skeleton's reference pose to be readable");
        }
        return false;
    }
    s.checked_skeleton = skeleton;
    LOG_INFO(Core, "VR body: skeleton ok at {} ({} bones; parents and names match c0000)",
             fmt::ptr(skeleton), C::BoneCount);
    return true;
}

// Log mode: ChrCtrl+0x1B8 maps FLVER bones to Havok bones; it must be the inverse of the
// skeleton's havok_to_flver (c0000.flver's bones by name; e.g. FLVER 41 -> R_UpperArm 80). Logged
// only.
void CheckBoneMap(const u8* chr) {
    const C::Skeleton* const sk = C::Active();
    if (sk == nullptr) {
        return;
    }
    u8* const ctrl = Read<u8*>(chr + OffsetChrCtrl);
    u8* const map = Plausible(ctrl) ? Read<u8*>(ctrl + OffsetCtrlBoneMap) : nullptr;
    u8* const havok = Plausible(map) ? Read<u8*>(map + 8) : nullptr;
    if (!Plausible(havok, 2)) {
        LOG_WARNING(Core, "VR body: bone map ChrCtrl+0x1B8 not readable (ChrCtrl {}, map {})",
                    fmt::ptr(ctrl), fmt::ptr(map));
        return;
    }
    const u32 count = Read<u32>(map);
    u32 mismatches = 0;
    std::string first;
    for (int i = 0; i < C::BoneCount; ++i) {
        const s32 flver = sk->havok_to_flver[static_cast<size_t>(i)];
        const s16 found = flver >= 0 && static_cast<u32>(flver) < count
                              ? Read<s16>(havok + 2 * flver)
                              : static_cast<s16>(-2);
        if (found != i) {
            ++mismatches;
            if (first.empty()) {
                first = fmt::format(", first: FLVER {} -> {} (expected {})", flver, found, i);
            }
        }
    }
    std::string arms;
    for (const int bone : {C::R_UpperArm, C::R_Forearm, C::R_Hand, C::R_Weapon, C::L_UpperArm,
                           C::L_Forearm, C::L_Hand, C::L_Weapon}) {
        const s32 flver = sk->havok_to_flver[static_cast<size_t>(bone)];
        arms += fmt::format(" {}->{}", flver,
                            static_cast<u32>(flver) < count ? Read<s16>(havok + 2 * flver) : -2);
    }
    LOG_INFO(Core, "VR body: bone map {} (FLVER bones {}, {} of {} differ{}) arms{}",
             mismatches == 0 ? "ok" : "MISMATCH", count, mismatches, C::BoneCount, first, arms);
}

// ---- The weapon in hand ----------------------------------------------------------------------

// A hand's equipped weapon (EquipParamWeapon id), read the way PlayerIns vt+0x388 does; -1 when
// it cannot be read (the function did not match at install, or an implausible slot).
s32 ReadWeaponId(const u8* chr, bool right, s32* selected_slot = nullptr) {
    if (selected_slot != nullptr) {
        *selected_slot = -1;
    }
    if (!g_weapon_ok) {
        return -1;
    }
    u8* const equip = Read<u8*>(chr + OffsetChrEquip);
    if (!Plausible(equip)) {
        return -1;
    }
    const s32 active = Read<s32>(equip + (right ? OffsetEquipRightSlot : OffsetEquipLeftSlot));
    if (active < 0 || active > 2) {
        return -1;
    }
    if (selected_slot != nullptr) {
        *selected_slot = active;
    }
    return Read<s32>(equip + OffsetEquipIds + 4 * (active * 2 + (right ? 1 : 0)));
}

std::string WeaponText(s32 id) {
    if (id < 0) {
        return "unknown";
    }
    const s32 family = B::Weapons::FamilyOf(id);
    const auto* f = B::Weapons::FindFamily(family);
    return f != nullptr ? fmt::format("{} ({}, {})", id, f->name, B::Weapons::HoldName(f->hold))
                        : fmt::format("{} (family {})", id, family);
}

// Log mode only [U]: how the right weapon's four parts are attached (`dmy300`: to that body
// dummy, the in-hand part is on 300 or 320-326; `-`: no part object).
std::string RightParts(const u8* chr) {
    u8* const parts = Read<u8*>(chr + OffsetChrAsm);
    if (!Plausible(parts)) {
        return " parts ?";
    }
    std::string text = " parts";
    for (s32 i = 0; i < 4; ++i) {
        u8* const part = Read<u8*>(parts + OffsetAsmParts + 8 * (RightPartSlot + i));
        if (!Plausible(part) || Read<u8*>(part) != g_base + WeaponPartVtableRva) {
            text += fmt::format(" {}:-", i);
            continue;
        }
        const s32 mode = Read<s32>(part + OffsetPartMode);
        text += mode == 2 ? fmt::format(" {}:dmy{}", i, Read<s32>(part + OffsetPartDummy))
                          : fmt::format(" {}:mode{}", i, mode);
    }
    return text;
}

// The hands' weapons and the right hand's grip: fetched again (and logged) when a weapon or the
// saved grips change. The left hand keeps the global weapon_grip_* trims.
void UpdateWeapon(PoseSide& s, const u8* chr) {
    const s32 id = ReadWeaponId(chr, true, &s.weapon_slot);
    const s32 left = ReadWeaponId(chr, false, &s.left_weapon_slot);
    const u32 generation = B::WeaponGripGeneration();
    if (id == s.weapon_id && left == s.left_weapon_id && s.grip_fetched &&
        generation == s.grip_generation) {
        return;
    }
    s.weapon_id = id;
    s.left_weapon_id = left;
    s.weapon_family = B::Weapons::FamilyOf(id);
    s.grip_custom = false;
    s.grip = s.weapon_family >= 0 ? B::WeaponGrip(s.weapon_family, &s.grip_custom)
                                  : B::DefaultWeaponGrip();
    s.grip_generation = generation;
    s.grip_fetched = true;
    const B::GripTrim& g = s.grip;
    LOG_INFO(Core,
             "VR arm: weapon R {} L {} | right grip pitch {:.1f} yaw {:.1f} roll {:.1f} deg offset "
             "({:.1f},{:.1f},{:.1f}) cm from {}{}",
             WeaponText(id), WeaponText(left), g.pitch_deg, g.yaw_deg, g.roll_deg,
             g.offset_m[0] * 100.0f, g.offset_m[1] * 100.0f, g.offset_m[2] * 100.0f,
             s.grip_custom ? B::WeaponGripKey(s.weapon_family)
                           : std::string{"weapon_grip_pitch_deg/_yaw_deg/_roll_deg"},
             LogLevel() > 0 ? RightParts(chr) : std::string{});
}

// The anchor and the hands of this fresh frame, and whether the body is driven at all.
void Gather(PoseSide& s, s64 now, bool native_throw) {
    const auto& settings = B::GetBodySettings();
    B::Anchor anchor{};
    const bool have_anchor = B::LatestAnchor(anchor) && anchor.valid;
    // The pose sees paired positioning before the camera announces its first
    // hold. Prime the world lock from the preceding rendered frame immediately.
    VR::Combat::VisceralHold receipt{};
    const bool receipt_fresh = VR::Combat::VisceralHoldChannel().Load(receipt) &&
        now >= receipt.ns && now - receipt.ns <= 250'000'000;
    if (have_anchor && native_throw && VR::GetSettings().dodge_camera &&
        (!receipt_fresh || ((receipt.camera_valid ? receipt.camera_active : receipt.active) &&
                            !receipt.camera_cancelled)) &&
        now >= anchor.steady_ns && now - anchor.steady_ns <= 150'000'000)
        anchor.rig_held = true;
    s.anchor_age_ns = have_anchor ? now - anchor.steady_ns : -1;
    // Stereo pairs: the eye this frame is rendered from (core/vr/body_frame.h NextEyeRel). A
    // pair-locked second eye is the first eye's world position against W at the camera: W now plus
    // its move between the last pose copy and the camera after it (zero when W is final here). A
    // second eye's lock is decided here, once: the comfort camera follows this decision.
    const B::EyePlacement eye = s.eyes.Place(have_anchor ? &anchor : nullptr, s.frame, now);
    s.anchor = anchor;
    B::Vec3 turn_eye{}; // the eye's swing round the turn (added to eye_rel below)
    if (have_anchor) {
        // The heading this frame's camera will have: the turn queued since its last update is
        // claimed for it (VR::ClaimTurnForArms; a pair's second eye keeps the first's).
        const float turn = VR::ClaimTurnForArms(!B::ReuseHands(anchor));
        s.anchor.base_yaw = std::remainder(anchor.base_yaw + turn, 2.0f * 3.14159265f);
        if (turn != 0.0f) {
            // The eye swings round with the view: your head's offset from the 6DoF origin and the
            // first-person lead turn with it (MockXR 2026-10-18: a 30 deg snap left the hands 52
            // mm off for that frame, then they snapped back).
            const float c = std::cos(turn), sn = std::sin(turn);
            const B::Vec3 h = anchor.head_offset;
            s.anchor.head_offset = {c * h.x + sn * h.z, h.y, -sn * h.x + c * h.z};
            turn_eye = s.anchor.head_offset - h;
        }
    }
    s.eye_source = eye.source;
    s.drift = eye.drift;
    if (have_anchor) {
        s.anchor.eye_rel = eye.eye_rel + turn_eye;
        s.anchor.rig_offset = eye.rig_offset;
        float phys_yaw = 99.0f;
        if (Common::SyncLog::On()) {
            PhysicsYawNow(phys_yaw);
        }
        Common::SyncLog::Line("arms rec=%llu t=(%.4f %.4f %.4f) yaw=%.5f eye_rel=(%.4f %.4f %.4f) "
                              "drift=(%.4f %.4f %.4f) base_yaw=%.5f scale=%.4f src=%d phys=%.5f",
                              static_cast<unsigned long long>(anchor.record), s.frame.t.x,
                              s.frame.t.y, s.frame.t.z, B::ChrYaw(s.frame), s.anchor.eye_rel.x,
                              s.anchor.eye_rel.y, s.anchor.eye_rel.z, eye.drift.x, eye.drift.y,
                              eye.drift.z, s.anchor.base_yaw, anchor.world_scale,
                              static_cast<int>(eye.source), phys_yaw);
        s.eye_locked += s.eye_source == B::EyeSource::Locked ? 1 : 0;
        s.eye_pair += s.eye_source == B::EyeSource::PairFirst ? 1 : 0;
    }
    if (eye.decided) {
        B::PublishPairLock(eye.lock);
    }
    B::HandsSample latest{};
    const bool have_hands = B::LatestHands(latest);
    // Arm pair sync: the second eye of a stereo pair takes the first eye's hand sample (while it
    // is fresh: after a hitch the pair is broken anyway).
    s.reused = settings.arm_pair_sync && s.have_used && have_anchor && B::ReuseHands(anchor) &&
               now - s.used.steady_ns <= HandsMaxAgeNs;
    if (s.reused) {
        ++s.reuses;
    } else if (have_hands) {
        s.used = latest;
        s.have_used = true;
    }
    s.hands_age_ns = s.have_used ? now - s.used.steady_ns : -1;
    Common::SyncLog::Line("pose seq=%llu hands_t=%lld anchor_rec=%llu reused=%d yaw=%.5f",
                          static_cast<unsigned long long>(s.used.seq),
                          static_cast<long long>(s.used.xr_time),
                          static_cast<unsigned long long>(anchor.record), s.reused ? 1 : 0,
                          s.anchor.base_yaw);

    const char* inactive = nullptr;
    bool view_off = true;
    if (!have_anchor) {
        inactive = "no anchor (not the first-person follow camera)";
    } else if (VR::CinematicFlat()) {
        inactive = "cinematic flat";
    } else if (VR::GetDisplayMode() != VR::DisplayMode::Auto) {
        inactive = "virtual screen";
    } else {
        view_off = false;
        if (s.anchor_age_ns > AnchorMaxAgeNs) {
            inactive = "anchor older than 1 s";
        } else if (!s.frame.valid) {
            inactive = "W is not a rotation";
        } else if (!B::IsFinite(s.anchor.eye_rel) || !std::isfinite(anchor.view_yaw)) {
            inactive = "anchor not finite";
        }
    }
    s.inactive = inactive;
    s.view_off = inactive != nullptr && view_off;
    if (inactive == nullptr) {
        ++s.active;
    }
}

// The first-person view is on now: a first-person anchor, not cinematic flat, not the virtual
// screen (the stale-copy path: no fresh inputs were gathered).
bool FirstPersonNow() {
    B::Anchor anchor{};
    return B::LatestAnchor(anchor) && anchor.valid && !VR::CinematicFlat() &&
           VR::GetDisplayMode() == VR::DisplayMode::Auto;
}

// The body is driven now (the stale-copy path).
bool DrivenNow(s64 now) {
    B::Anchor anchor{};
    return FirstPersonNow() && B::LatestAnchor(anchor) && now - anchor.steady_ns <= AnchorMaxAgeNs;
}

// ---- Animation gate (read at the pose copy: after this frame's behaviour step) -----------------

void WarnGate(PoseSide& s, GateSignal signal, const std::string& what) {
    if (!s.gate_warned[signal]) {
        s.gate_warned[signal] = true;
        LOG_WARNING(Core, "VR body: {}; that animation gate signal is off while it differs", what);
    }
}

// The TimeAct ring's entries pushed since the last fresh copy (the write index advanced). A new
// module (a new character object) starts over: its current entries are not new.
bool ReadRing(PoseSide& s, const u8* mods, B::GateSignals& g) {
    u8* const ta = Read<u8*>(mods + L::OffsetTimeActModule);
    if (!Plausible(ta)) {
        return false;
    }
    if (Read<u8*>(ta) != g_base + L::TimeActVtableRva) {
        WarnGate(s, GateRing,
                 fmt::format("TimeAct module {} has vtable {} (expected +{:#x})", fmt::ptr(ta),
                             Rva(Read<u8*>(ta)), L::TimeActVtableRva));
        return false;
    }
    const s32 write = Read<s32>(ta + L::OffsetTimeActWrite);
    if (write < 0 || write >= L::TimeActRingSize) {
        return false;
    }
    if (ta != s.ring_module || s.ring_write < 0) {
        s.ring_module = ta;
        s.ring_write = write;
        return true;
    }
    const s32 count = (write - s.ring_write + L::TimeActRingSize) % L::TimeActRingSize;
    for (s32 k = 0; k < count; ++k) {
        const s32 index = (s.ring_write + k) % L::TimeActRingSize;
        const u8* const entry = ta + L::OffsetTimeActRing + index * L::TimeActEntrySize;
        NewAnim& anim = s.new_anims[s.new_count];
        anim.id = Read<s32>(entry);
        anim.f = {Read<float>(entry + 4), Read<float>(entry + 8), Read<float>(entry + 12)};
        g.new_ids[g.new_count] = anim.id;
        g.new_from[g.new_count++] = anim.f[0];
        ++s.new_count;
    }
    s.ring_write = write;
    return true;
}

// The hard gates and the ring. Every module is checked (vtable, owner) before it is read; a
// mismatch turns that signal off while it lasts (logged once).
B::GateSignals ReadGate(PoseSide& s, const u8* chr) {
    B::GateSignals g{};
    g.visceral_recovery = VisceralPullRecoverArms(chr);
    g.visceral_freehand = VisceralPullFreehand();
    g.anim_gate = B::GetBodySettings().arm_anim_gate;
    g.keep_hit = B::GetBodySettings().arm_keep_on_hit;
    g.keep_dodge = B::GetBodySettings().arm_keep_on_dodge;
    // The Bowblade's bow clips (core/vr/bow.h): only while the bow is up, as the presenter sees it
    // (the melee update published its bow form and hand).
    {
        VR::Combat::MeleeStatus ms{};
        g.keep_bow = VR::Bow::GetBowSettings().enabled && VR::Combat::LatestMeleeStatus(ms) &&
                     ms.bow_hand >= 0 && B::SteadyNowNs() - ms.ns < 300'000'000;
    }
    // A shield held up (vr.ini shield_raise, core/vr/trick_weapons.h): a blow on it leaves the arms
    // where you hold them.
    g.keep_guard = VR::Tricks::GetTrickSettings().shield_raise &&
                   VR::Tricks::IsShieldFamily(B::Weapons::FamilyOf(s.left_weapon_id));
    // The game's transform animations (hand_transform): your hands did it, they stay yours.
    g.keep_transform = VR::Tricks::GetTrickSettings().hand_transform;
    // VR_TUNE_FILE body.keep_torso=0 (A/B tests): a kept clip's torso plays the game's animation.
    g.keep_torso = VR::Tune::Get("body.keep_torso", 1.0f) != 0.0f;
    g.dodge_attack = VR::GetSettings().dodge_attack;
    {
        const auto& vs = VR::GetSettings(); // the grab's arms come back with the cinematic's VR
        g.throw_release = vs.cinematic_release;
        g.throw_release_ns =
            static_cast<std::int64_t>(std::max(vs.cinematic_exit_ms, 0.0f) * 1'000'000.0f);
    }
    s.new_count = 0;
    u8* const mods = Read<u8*>(chr + L::OffsetChrModules);
    if (!Plausible(mods)) {
        return g;
    }
    const auto module = [mods](u32 offset) -> u8* {
        u8* const pointer = Read<u8*>(mods + offset);
        return Plausible(pointer) ? pointer : nullptr;
    };
    if (g_gate_ring) {
        g.ring_valid = ReadRing(s, mods, g);
        // A blow on the shield: a guard reaction's start (the ring has a playing clip's later
        // steps too: their clip time before the step is past 0).
        for (u32 i = 0; i < g.new_count; ++i) {
            if (B::ClassifyAnim(g.new_ids[i]) == B::AnimClass::GuardReaction &&
                !(g.new_from[i] > 0.0f)) {
                ++s.guard_serial;
                s.guard_clip = g.new_ids[i];
            }
        }
    }
    if (g_gate_event) {
        u8* const flags = module(L::OffsetActionFlagModule);
        u8* const behavior = module(L::OffsetBehaviorModule);
        if (flags != nullptr && Read<u8*>(flags) != g_base + L::ActionFlagVtableRva) {
            WarnGate(s, GateEvent,
                     fmt::format("action-flag module {} has vtable {} (expected +{:#x})",
                                 fmt::ptr(flags), Rva(Read<u8*>(flags)), L::ActionFlagVtableRva));
        } else if (flags != nullptr && behavior != nullptr &&
                   Read<u8>(behavior + L::OffsetBehaviorReady) != 0) {
            g.event_valid = true;
            g.event = (Read<u8>(flags + L::OffsetActionFlags) & L::FlagEventAnim) != 0;
        }
    }
    if (g_gate_throw) {
        if (u8* const throws = module(L::OffsetThrowModule); throws != nullptr) {
            if (Read<u8*>(throws) != g_base + L::ThrowVtableRva ||
                Read<const u8*>(throws + L::OffsetOwner) != chr) {
                WarnGate(s, GateThrow,
                         fmt::format("throw module {} vtable {} (expected +{:#x}) owner {}",
                                     fmt::ptr(throws), Rva(Read<u8*>(throws)), L::ThrowVtableRva,
                                     fmt::ptr(Read<u8*>(throws + L::OffsetOwner))));
            } else {
                u8* const node = Read<u8*>(throws + L::OffsetThrowNode);
                g.throw_valid = true;
                g.throw_state = Plausible(node) && Read<const u8*>(node + L::OffsetOwner) == chr
                                    ? Read<u8>(node + L::OffsetThrowState)
                                    : u8{0};
            }
        }
    }
    if (g_gate_ladder) {
        u8* const ctrl = Read<u8*>(chr + L::OffsetChrCtrl);
        u8* const state = Plausible(ctrl) ? Read<u8*>(ctrl + L::OffsetActionState) : nullptr;
        if (Plausible(state)) {
            if (Read<u8*>(state) != g_base + L::ActionStateVtableRva ||
                Read<u8*>(state + L::OffsetOwner) != ctrl) {
                WarnGate(s, GateLadder,
                         fmt::format("HKS action state {} vtable {} (expected +{:#x}) owner {} "
                                     "(ChrCtrl {})",
                                     fmt::ptr(state), Rva(Read<u8*>(state)),
                                     L::ActionStateVtableRva,
                                     fmt::ptr(Read<u8*>(state + L::OffsetOwner)), fmt::ptr(ctrl)));
            } else {
                g.ladder_valid = true;
                g.ladder = Read<s8>(state + L::OffsetLadderState);
            }
        }
    }
    if (u8* const data = module(L::OffsetDataModule); data != nullptr) {
        g.hp_valid = true;
        g.hp = Read<s32>(data + L::OffsetHp);
    }
    if (g_gate_cancel) {
        if (u8* const requests = module(L::OffsetActionRequestModule); requests != nullptr) {
            if (Read<const u8*>(requests + L::OffsetOwner) != chr) {
                WarnGate(s, GateCancel,
                         fmt::format("action request module {} has owner {} (expected {})",
                                     fmt::ptr(requests),
                                     fmt::ptr(Read<u8*>(requests + L::OffsetOwner)),
                                     fmt::ptr(chr)));
            } else {
                g.cancel_valid = true;
                g.allow = Read<u64>(requests + L::OffsetRequestAllow);
                g.move_ok = (Read<u8>(requests + L::OffsetRequestMove) & L::RequestMoveBit) != 0;
                NoteCancelWindow(chr, g.allow, g.move_ok); // the cinematic's control back
            }
        }
    }
    return g;
}

const char* Owner(bool ik) {
    return ik ? "you" : "game";
}

// VR_ANIM_LOG: every new id. Ids of unknown or unverified classes are logged the first time
// they are seen even without it (they refine the class table).
void LogNewAnims(PoseSide& s, const B::GateOut& g) {
    for (u32 i = 0; i < s.new_count; ++i) {
        const NewAnim& a = s.new_anims[i];
        const B::AnimClass c = B::ClassifyAnim(a.id);
        const bool unsure = c == B::AnimClass::Unknown || c == B::AnimClass::Unverified;
        bool first = false;
        if (unsure) {
            const auto end = s.seen_unknown.begin() + s.seen_unknown_count;
            if (std::find(s.seen_unknown.begin(), end, a.id) == end) {
                if (s.seen_unknown_count < s.seen_unknown.size()) {
                    s.seen_unknown[s.seen_unknown_count++] = a.id;
                    first = true;
                } else if (!s.seen_unknown_full) {
                    s.seen_unknown_full = true;
                    LOG_INFO(Core,
                             "VR anim: {} unknown or unverified ids logged; new ones are "
                             "only logged with VR_ANIM_LOG from here on",
                             s.seen_unknown.size());
                }
            }
        }
        if (!AnimLog() && !first) {
            continue;
        }
        const B::AnimPolicy p = B::PolicyOf(c);
        const float length = C::AnimLengthSeconds(a.id);
        const auto& bs = B::GetBodySettings();
        const bool keep = bs.arm_anim_gate &&
                          B::KeepsArms(a.id, bs.arm_keep_on_hit, bs.arm_keep_on_dodge,
                                       VR::Bow::GetBowSettings().enabled,
                                       VR::Tricks::GetTrickSettings().shield_raise,
                                       VR::Tricks::GetTrickSettings().hand_transform);
        LOG_INFO(Core,
                 "VR anim: id {} (a{:03}_{:06}) class {} floats ({:.3f}, {:.3f}, {:.3f}) len {} -> "
                 "R {} L {} torso {}{} | arms now R {} L {} ({}){}",
                 a.id, a.id / 1'000'000, a.id % 1'000'000, B::AnimClassName(c), a.f[0], a.f[1],
                 a.f[2], length > 0.0f ? fmt::format("{:.2f} s", length) : std::string{"-"},
                 keep ? "you (kept)" : Owner(p.right_ik), keep ? "you (kept)" : Owner(p.left_ik),
                 p.torso || keep ? "kept" : "off",
                 B::Animates(p) ? fmt::format(" hold {} ms{}", B::AnimHoldNs(a.id, p.cap_s) / Ms,
                                              p.release_on_next ? " or next id" : "")
                                : std::string{},
                 Owner(g.ik[C::Right]), Owner(g.ik[C::Left]), B::GateReasonName(g.reason),
                 first && !AnimLog() ? " (first sight, unverified class)" : "");
    }
}

// VR_BODY_LOG: every change of who owns the arms.
void LogGateEdge(const B::GateOut& g) {
    LOG_INFO(Core, "VR arm: own R={} L={} torso={} cat={} class={} id={} age {} ms{}{}{}",
             Owner(g.ik[C::Right]), Owner(g.ik[C::Left]), g.torso ? "on" : "off",
             B::GateReasonName(g.reason), g.held ? B::AnimClassName(g.owner) : "-", g.owner_id,
             g.owner_age_ns / Ms,
             g.kept || !g.attack_ok[C::Right] || !g.attack_ok[C::Left]
                 ? fmt::format(" | {}attack R {} L {}",
                               g.kept ? fmt::format("kept ({}): ",
                                                    B::KeepKindName(B::KeepKindOf(g.owner_id)))
                                      : std::string{},
                               g.attack_ok[C::Right] ? "ok" : "waits",
                               g.attack_ok[C::Left] ? "ok" : "waits")
                 : std::string{},
             g.released != nullptr ? fmt::format(" (released: {})", g.released) : std::string{},
             g.cancelled[C::Right] || g.cancelled[C::Left]
                 ? fmt::format(" (cancel window: R {} L {})", g.cancelled[C::Right] ? 1 : 0,
                               g.cancelled[C::Left] ? 1 : 0)
                 : std::string{});
}

// ---- The held item's grip ----------------------------------------------------------------------

namespace G = VR::Body::Grip;
constexpr float GripBlendS = 0.12f; // the fingers close / open over this long

// VR_GRIP_LOG=1: once a second, each hand's animated finger locals (before the grip) as the turn
// from the open hand of the reference pose (axis, degrees): what the game's own grips look like.
void LogFingers(PoseSide& s, const B::HkQsTransform* local, s64 now) {
    static const bool enabled = std::getenv("VR_GRIP_LOG") != nullptr;
    if (!enabled || now - s.grip_log_ns < 1000 * Ms) {
        return;
    }
    s.grip_log_ns = now;
    const C::Skeleton& sk = C::Ref(); // the body runs only with the skeleton read
    for (int side = 0; side < 2; ++side) {
        std::string text;
        const int first = side == 0 ? C::L_Finger0 : C::R_Finger0;
        for (int k = 0; k < 15; ++k) {
            const int bone = first + k;
            const auto& r = sk.local[static_cast<size_t>(bone)].q;
            const B::Quat q0{r[0], r[1], r[2], r[3]};
            const B::Quat q{local[bone].q[0], local[bone].q[1], local[bone].q[2], local[bone].q[3]};
            B::Quat d = B::Conjugate(q0) * q;
            if (d.w < 0.0f) {
                d = {-d.x, -d.y, -d.z, -d.w};
            }
            const float angle = B::AngleOf(d);
            const B::Vec3 axis = B::Normalize(B::Vec3{d.x, d.y, d.z}, {0.0f, 0.0f, 0.0f});
            text += fmt::format(" {}{}:({:.2f},{:.2f},{:.2f}){:.0f}", k / 3, k % 3, axis.x, axis.y,
                                axis.z, angle / B::Deg);
        }
        LOG_INFO(Core, "VR grip: {} fingers (grip {} w {:.2f}){}", side == 0 ? "L" : "R",
                 G::KindName(static_cast<G::Kind>(s.grip_kind)), s.grip_weight, text);
    }
}

// The shape of `kind`, with VR_TUNE_FILE's values over it (grip.<kind>.f<finger>.<joint> and
// .spread in degrees, grip.<kind>.oppose).
G::Shape TunedShape(G::Kind kind, s32 goods = -1) {
    G::Shape shape = G::ShapeForGoods(goods, kind);
    if (!VR::Tune::Active()) {
        return shape;
    }
    const std::string base = fmt::format("grip.{}.", G::KindName(kind));
    for (int f = 0; f < 5; ++f) {
        for (int j = 0; j < 3; ++j) {
            float& bend = shape.bend[static_cast<size_t>(f)][static_cast<size_t>(j)];
            bend =
                VR::Tune::Get(fmt::format("{}f{}.{}", base, f, j).c_str(), bend / G::Deg) * G::Deg;
        }
        float& spread = shape.spread[static_cast<size_t>(f)];
        spread =
            VR::Tune::Get(fmt::format("{}f{}.spread", base, f).c_str(), spread / G::Deg) * G::Deg;
    }
    shape.thumb_oppose =
        VR::Tune::Get((base + "oppose").c_str(), shape.thumb_oppose / G::Deg) * G::Deg;
    return shape;
}

// The left fingers closed round the held item (the vial, a quick item: the held tick's grip of
// the last update), eased in and out over GripBlendS, over whatever the animation does with them
// (vr.ini held_grip). Written after the arms, into the same pose; RestoreGame puts the game's back.
void ApplyGrip(PoseSide& s, B::HkQsTransform* local, float dt) {
    const auto held = HeldGrip();
    const u8 want = held.kind;
    const bool on = want != 0 && want < static_cast<u8>(G::Kind::Count) &&
                    VR::Quick::GetQuickSettings().held_grip;
    if (on) {
        s.grip_kind = want;
        s.grip_goods = held.goods;
    }
    const float step = dt / GripBlendS;
    s.grip_weight = std::clamp(s.grip_weight + (on ? step : -step), 0.0f, 1.0f);
    if (s.grip_weight <= 0.0f || s.grip_kind == 0) {
        return;
    }
    const float w = s.grip_weight * s.grip_weight * (3.0f - 2.0f * s.grip_weight);
    const G::Shape shape = TunedShape(static_cast<G::Kind>(s.grip_kind), s.grip_goods);
    const float bend_sign = VR::Tune::Get("grip.bend_sign", 1.0f);
    const float oppose_sign = VR::Tune::Get("grip.oppose_sign", 1.0f);
    for (int f = 0; f < 5; ++f) {
        for (int j = 0; j < 3; ++j) {
            const int bone = G::LeftFingers[static_cast<size_t>(f)][static_cast<size_t>(j)];
            if (!s.edited.test(static_cast<size_t>(bone))) {
                s.animated[static_cast<size_t>(bone)] = local[bone];
                s.edited.set(static_cast<size_t>(bone));
            }
            B::QsTransform x = B::FromHavok(local[bone]);
            x.q = B::Slerp(x.q, G::ShapeLocal(shape, f, j, bend_sign, oppose_sign), w);
            B::ToHavok(x, local[bone]);
            s.locals[static_cast<size_t>(bone)] = x; // contact/attachment sees the drawn fingers
        }
    }
}

// A static finger shape around the held lever, restored with the other pose edits.
// The hinge remains entirely hand-driven; this does not play a pull animation.
void ApplyLeverGrip(PoseSide& s, B::HkQsTransform* local, s64 now) {
    VR::Lever::HandPresentation held{};
    if (!VR::Lever::Channel().PresentedHand(held) || !VR::Lever::Fresh(held.ns, now) ||
        held.space != s.used.space_serial || held.hand < 0 || held.hand > 1 ||
        s.arm_weight[held.hand] < 0.9f || !B::IsTracked(s.used.hand[held.hand]))
        return;
    const auto side = static_cast<C::Side>(held.hand);
    const auto& fingers = side == C::Right ? G::RightFingers : G::LeftFingers;
    const auto shape = G::ShapeOf(G::Kind::Handle);
    for (int f = 0; f < 5; ++f) {
        for (int j = 0; j < 3; ++j) {
            const int bone = fingers[f][j];
            if (!s.edited.test(bone)) {
                s.animated[bone] = local[bone];
                s.edited.set(bone);
            }
            auto x = B::FromHavok(local[bone]);
            x.q = G::ShapeLocal(shape, f, j, 1.0f, 1.0f, C::Ref(), side);
            B::ToHavok(x, local[bone]);
            s.locals[bone] = x;
        }
    }
}

void ApplyMusicBoxGrip(PoseSide& s, B::HkQsTransform* local, s64 now) {
    const float w = std::clamp(s.used.music_box_weight, 0.0f, 1.0f);
    if (w <= 0 || now - s.used.steady_ns > 250'000'000 || s.arm_weight[C::Right] < .9f || !B::IsTracked(s.used.hand[C::Right])) return;
    const auto shape = VR::MusicBox::CrankPinch();
    for (int f = 0; f < 5; ++f) for (int j = 0; j < 3; ++j) {
        const int bone = G::RightFingers[f][j];
        if (!s.edited.test(bone)) {
            s.animated[bone] = local[bone];
            s.edited.set(bone);
        }
        auto x = B::FromHavok(local[bone]);
        x.q = B::Slerp(x.q, G::ShapeLocal(shape, f, j, 1, 1, C::Ref(), C::Right), w);
        B::ToHavok(x, local[bone]);
        s.locals[bone] = x;
    }
}

// ---- The two-handed grip (core/vr/two_hand_grip.h) ---------------------------------------------

// [[PlayerIns+0xF0]]: the right weapon's arm style (1 form A, 3 form B two-handed, 5 form B
// one-handed), or -1 (as bloodborne_gun.cpp reads it).
constexpr u32 OffsetTwoHandArmStyle = 0xF0;

s32 ReadArmStyle(const u8* chr) {
    u8* const style = Read<u8*>(chr + OffsetTwoHandArmStyle);
    return Plausible(style, 4) ? Read<s32>(style) : -1;
}

struct TwoHandSide {
    VR::TwoHand::GripCache grips{};
    s32 family{-1};
    s32 style{-1};
    bool logged{false};
};

// Capture native animation before VR edits; retain each weapon's own position AND rotation.
// Context-owned arms and transform blends must not contaminate a calibrated handle.
void ObserveTwoHand(PoseSide& s, const u8* chr, float dt, s64 now) {
    namespace T = VR::TwoHand;
    static TwoHandSide th;
    const s32 style = ReadArmStyle(chr);
    const s32 family = s.weapon_family;
    const bool two = T::TwoHandedForm(family, style);
    if (family != th.family || style != th.style) {
        th.family = family;
        th.style = style;
        th.logged = false;
        LOG_INFO(Core, "VR two-hand: right weapon {} style {}: {}", B::WeaponName(family), style,
                 two ? "a two-handed form" : "not a two-handed form");
    }
    const float scale = std::max(s.anchor.world_scale, 1e-3f);
    th.grips.Select(family, style, dt);
    const B::GateOut& g = s.gate_out;
    const bool eligible = two && g.ik[C::Right] && g.ik[C::Left] && !g.held &&
                          !g.kept && !g.switching && !s.levelup_pose;
    T::GameGrip reading{};
    if (eligible) {
        const auto r = B::ModelOf(s.locals.data(), C::Parents.data(), C::BoneCount, C::R_Weapon);
        const auto l = B::ModelOf(s.locals.data(), C::Parents.data(), C::BoneCount, C::L_Weapon);
        if (r && l) {
            const B::Quat inv = B::Conjugate(B::Normalize(r->q));
            reading = {true, B::Rotate(inv, l->t - r->t),
                       B::Normalize(inv * B::Normalize(l->q))};
        }
    }
    th.grips.Observe(reading, scale, dt, eligible);
    T::GameStatus st{};
    st.ns = now;
    st.family = family;
    st.style = style;
    st.two_handed = two;
    st.grip = th.grips.Get(scale, st.measured);
    // Keep the learned transformed grip in form A too: it defines the cue/rail's axis and
    // estimated extent, not a forced hand position. Unknown grips only request a transform;
    // SupportGrip leaves the hand tracked until the native rail is available and reached.
    st.readings = th.grips.Readings();
    if (st.measured && !th.logged) {
        th.logged = true;
        const auto t = st.grip.t * (100.0f / scale);
        LOG_INFO(Core, "VR two-hand: the game's two-handed grip for {}: ({:.1f}, {:.1f}, {:.1f}) "
                       "cm, rotation {:.1f} deg (cached native pose)",
                 B::WeaponName(family), t.x, t.y, t.z, B::AngleOf(st.grip.q) / B::Deg);
    }
    const float pitch = VR::GetSettings().blade_pitch_deg;
    const B::GripTrim& tr = s.grip;
    st.right = {B::DefaultGrip(C::Right, pitch, tr.yaw_deg, tr.pitch_deg, tr.roll_deg),
                B::TrimOffset(tr)};
    const B::GripTrim tl = B::DefaultWeaponGrip();
    st.left = {B::DefaultGrip(C::Left, pitch, tl.yaw_deg, tl.pitch_deg, tl.roll_deg),
               B::TrimOffset(tl)};
    st.scale = scale;
    T::PublishGameStatus(st);
}

// ---- The body's edits ------------------------------------------------------------------------

// The body's edits of this fresh pose copy, written into the game's locals unless dry run. Runs on
// the loop thread right after the copy, before anything reads the pose (vt+0xE8/F0 sync the model
// pose from these locals; the weapon parts, dummy polys and skinning follow).
std::string PostureText(const B::Posture& p) {
    return fmt::format("eye {} lean {:+.1f}/{:+.1f} twist {:+.1f} trunk {:.1f} chest-pel {}",
                       Cm(p.eye), p.lean_fwd, p.lean_right, p.twist, p.trunk * 100.0f,
                       Cm(p.chest_from_pelvis));
}

// VR_BODY_POSTURE_LOG: the body shape before (the game's animation) and after the edits.
void LogPosture(PoseSide& s, const B::BodyInput& in, const B::Posture& out, s64 now) {
    // The character's speed in his own axes (forward = model -Z, right = model -X), m/s.
    B::Vec3 v{};
    if (s.frame.valid && s.posture_last_ns != 0 && now > s.posture_last_ns) {
        const float dt = static_cast<float>(now - s.posture_last_ns) * 1e-9f;
        const float scale = std::max(s.anchor.world_scale, 1e-3f);
        v = B::RotateToModel(s.frame, s.frame.t - s.posture_last_t) * (1.0f / (dt * scale));
    }
    if (s.frame.valid) {
        s.posture_last_t = s.frame.t;
        s.posture_last_ns = now;
    }
    const B::Posture& a = s.posture_anim;
    const B::Vec3 shear = out.chest_from_pelvis - a.chest_from_pelvis;
    LOG_INFO(Core,
             "VR posture: v fwd {:+.2f} right {:+.2f} m/s anim {} pitch {:.0f} w {:.2f} | anim {} "
             "| out {} | d shear {} cm trunk {:+.1f} cm lean {:+.1f}/{:+.1f} twist {:+.1f} | "
             "goal-anim eye {} shift {} torso {:+.1f} lean {:+.1f}/{:+.1f} res {:.1f} cm dip "
             "{:+.1f} cm",
             -v.z, -v.x, s.posture_anim_id, s.last_view_pitch / B::Deg, s.body_weight,
             PostureText(a), PostureText(out), Cm(shear), (out.trunk - a.trunk) * 100.0f,
             out.lean_fwd - a.lean_fwd, out.lean_right - a.lean_right, out.twist - a.twist,
             Cm(in.eye_m - a.eye), Cm(s.out.shift), s.out.torso_delta / B::Deg,
             s.out.lean_fwd / B::Deg, s.out.lean_right / B::Deg, s.out.eye_residual * 100.0f,
             s.anchor.camera_dip * 100.0f);
}

// ---- Solid weapons (vr.ini weapon_wall; core/vr/solid_weapon.h) -----------------------------

bool PairedPartLine(int hand, VR::Solid::Shape& sh) {
    B::Vec3 base{}, tip{};
    bool seen = false;
    if (MeleePartLine(hand, base, tip, seen) < 0) {
        return false;
    }
    const C::Skeleton* const sk = C::Active();
    if (seen && sk != nullptr) {
        const auto& dummy = hand == C::Left ? sk->left_weapon_base : sk->right_weapon_base;
        const B::Vec3 origin{dummy.position[0], dummy.position[1], dummy.position[2]};
        const B::Vec3 forward{dummy.forward[0], dummy.forward[1], dummy.forward[2]};
        const B::Vec3 up{dummy.up[0], dummy.up[1], dummy.up[2]};
        const B::Vec3 right = B::Cross(up, forward);
        const auto on_bone = [&](B::Vec3 p) {
            return origin + right * p.x + up * p.y + forward * p.z;
        };
        sh.line = true;
        sh.base = on_bone(base);
        sh.tip = on_bone(tip);
    }
    return true;
}

// The weapon in hand `i` on its weapon bone (SolidWeapons, BladeSounds): the right one its blade
// along R_Weapon -Y (MeleeBladeReach: the default grip's blade axis, its dummies within 3 degrees of
// it) or its part in that hand, the left a gun along its barrel (GunBarrelReach) or the right
// weapon's part in it (PairedPartLine: the dual blades' second blade), else the bare hand.
void HandShape(int i, float scale, VR::Solid::Shape& sh) {
    namespace S = VR::Solid;
    sh.hand_r = 0.05f * scale;
    sh.line_r = 0.02f * scale;
    if (i == C::Right) {
        float base = 0.0f;
        float tip = 0.0f;
        S::Shape part{};
        PairedPartLine(C::Right, part);
        const bool have_reach = MeleeBladeReach(base, tip);
        S::BladeOrPartLine(have_reach, base, tip, 0.05f * scale, part, sh);
    } else if (!PairedPartLine(C::Left, sh)) {
        s32 attach = -1;
        float back = 0.0f;
        float front = 0.0f;
        const B::Vec3 dir =
            GunBarrelReach(attach, back, front) ? VR::Gun::BarrelInWeaponBone(attach) : B::Vec3{};
        if (B::LengthSq(dir) > 0.0f && front > back + 0.05f * scale) {
            sh.line = true;
            sh.base = dir * back;
            sh.tip = dir * front;
        }
    }
}

// Before the arms are solved: each hand's weapon against the static map (the game's own sweep,
// CastMapSphere) and the two weapons against each other; the arms then hold the weapons where they
// are (their targets moved there) while the controllers go on. The right weapon is its blade along
// R_Weapon -Y (MeleeBladeReach: the default grip's blade axis, its dummies within 3 degrees of it),
// the left a gun along its barrel (GunBarrelReach) or the right weapon's part in it (PairedPartLine:
// the dual blades' second blade), else the bare hand; a left hand on the right weapon's handle
// (the two-handed grip) goes where that weapon goes. Each contact and the
// controller's motion where it touches go through WeaponWall::Contact (a strike, a scrape) to the
// presenter (the feel, the sparks: tracking space). A snap turn places the weapons again from the
// chest (no sweep through the turn, never into a wall); it and a stereo pair's second eye (its
// hands reused) are no motion of the hands: felt over the next fresh update.
// `lever_hand`: the hand on a lever (its fist pinned to the handle, its weapon hidden): not solid,
// so neither it nor the hidden weapon pushes the fist off the handle; let go, it is placed again
// from the chest like any hand the game gave back (-1: none).
void SolidWeapons(PoseSide& s, B::BodyInput& in, bool driven, int lever_hand, s64 now) {
    namespace S = VR::Solid;
    namespace WW = VR::WeaponWall;
    SolidSide& w = s.solid;
    if (!VR::Tricks::GetTrickSettings().weapon_wall) {
        // Off: nothing runs. Turned off, it lets go once: on again, the weapons start from the
        // hunter's chest and the hands' motion afresh (the strikes' count kept: no late thud).
        if (w.active) {
            w.active = false;
            for (int i = 0; i < 2; ++i) {
                w.weapon[i].Reset();
                w.touch[i] = {};
                w.have_last[i] = false;
            }
            w.clash = {};
            w.clash_met = false;
            w.have_yaw = false;
            w.last_ns = 0;
        }
        return;
    }
    w.active = true;
    const float turn =
        w.have_yaw ? std::remainder(s.anchor.base_yaw - w.yaw, 2.0f * 3.14159265f) : 0.0f;
    const bool snap = std::abs(turn) > 8.0f * B::Deg;
    w.yaw = s.anchor.base_yaw;
    w.have_yaw = true;
    const bool feel = !s.reused && !snap;
    const float dt = feel && w.last_ns != 0
                         ? std::clamp(static_cast<float>(now - w.last_ns) * 1e-9f, 0.0f, 0.1f)
                         : 0.0f;
    if (feel) {
        w.last_ns = now;
    }
    const bool on = driven && s.frame.valid;
    const float scale = std::max(s.anchor.world_scale, 1e-3f);
    S::Config cfg{};
    cfg.skin *= scale;
    cfg.back_m *= scale;
    cfg.let_go_m *= scale;
    cfg.let_go_arc_m *= scale;
    cfg.step_m *= scale;
    const B::Vec3 eye = s.frame.t + s.anchor.eye_rel;
    // Where a weapon is reached from: the hunter's chest over his feet (W), inside his own capsule,
    // which the game keeps out of the walls (never the camera's: your head can lean into one).
    const B::Vec3 safe = B::ToWorld(s.frame, {0.0f, 1.2f * scale, 0.0f});
    // weapon_wall=2 (weapons only): the walls are never hit; the weapons still meet each other.
    const bool walls = VR::Tricks::GetTrickSettings().weapon_wall_map;
    const auto cast = [walls](B::Vec3 a, B::Vec3 b, float r) -> S::Hit {
        if (!walls) {
            return {};
        }
        const float from[3] = {a.x, a.y, a.z};
        const float to[3] = {b.x, b.y, b.z};
        float hit[3]{};
        float n[3]{};
        float f = 1.0f;
        if (!CastMapSphere(from, to, r, hit, n, f)) {
            return {};
        }
        return {true, f, {n[0], n[1], n[2]}};
    };
    // Tracking space for the presenter (the body's own mapping, as ModelToTracking).
    const auto track = [&](B::Vec3 world) {
        return s.anchor.head_k + B::WorldToTracking(world - eye, s.anchor.base_yaw, scale);
    };
    const auto put = [](float out[3], B::Vec3 v) {
        out[0] = v.x;
        out[1] = v.y;
        out[2] = v.z;
    };
    std::array<S::Shape, 2> shape{};
    std::array<S::Pose, 2> pose{};
    std::array<S::Pose, 2> target{};
    std::array<S::Pose, 2> prev{};
    std::array<bool, 2> had{};
    std::array<bool, 2> own{};
    std::array<bool, 2> mine{};
    std::array<bool, 2> solid{};
    const float two = s.have_used ? std::clamp(s.used.two_hand, 0.0f, 1.0f) : 0.0f;
    if (snap) {
        w.clash_met = false;
    }
    for (int i = 0; i < 2; ++i) {
        own[i] = on && in.target[i].has_value() && s.arm_weight[i] >= B::DetachWatch::OwnedWeight;
        mine[i] = own[i] && !(i == C::Left && two > 0.0f) && i != lever_hand;
        WW::Touching& out = w.state.hand[i];
        if (!mine[i]) {
            w.weapon[i].Reset();
            w.touch[i] = {};
            w.have_last[i] = false;
            out.scraping = false;
            continue;
        }
        S::Shape& sh = shape[i];
        HandShape(i, scale, sh);
        if (w.shape_logged[i] != (sh.line ? 1 : 0)) {
            w.shape_logged[i] = sh.line ? 1 : 0;
            LOG_INFO(Core, "VR solid: the {} hand: {}", i == C::Right ? "right" : "left",
                     sh.line ? fmt::format("its weapon from {:.0f} to {:.0f} cm out of the hand",
                                           B::Length(sh.base) * 100.0f / scale,
                                           B::Length(sh.tip) * 100.0f / scale)
                             : std::string{"the bare hand"});
        }
        const B::ModelTarget& t = *in.target[i];
        target[i] = {B::ToWorld(s.frame, t.p), B::Normalize(s.frame.q * t.q)};
        if (snap) {
            w.weapon[i].Reset(); // placed again from the chest: a carried pose could be in a wall
            w.have_last[i] = false;
        }
        prev[i] = w.last[i];
        had[i] = w.have_last[i];
        const S::Result r = w.weapon[i].Step(target[i], safe, sh, cfg, cast);
        pose[i] = r.pose;
        solid[i] = !r.through;
        if (r.back) {
            LOG_INFO(Core, "VR solid: the {} hand's weapon was caught far from the controller: "
                           "back at it (clear from the chest)",
                     i == C::Right ? "right" : "left");
        }
        if (!feel) {
            continue;
        }
        // The contact as it feels: the controller's motion where the weapon touches.
        WW::Sample sample{};
        sample.dt = dt;
        if (r.contact.any && had[i] && dt > 0.0f) {
            const B::Vec3 local = B::Rotate(B::Conjugate(r.pose.q), r.contact.point - r.pose.p);
            const B::Vec3 v =
                (S::At(target[i], local) - S::At(prev[i], local)) * (1.0f / (dt * scale));
            sample.contact = true;
            sample.normal = r.contact.normal;
            sample.v = v;
            out.steel = r.contact.line;
            put(out.point, track(r.contact.point));
            put(out.normal, B::WorldToTracking(r.contact.normal, s.anchor.base_yaw, 1.0f));
            put(out.slide,
                B::WorldToTracking(B::Perp(v, r.contact.normal), s.anchor.base_yaw, 1.0f));
            if (!w.touch_logged[i]) {
                w.touch_logged[i] = true;
                LOG_INFO(Core, "VR solid: the {} hand's first contact: its {} at {:.2f} m/s, "
                               "the surface facing ({:.2f}, {:.2f}, {:.2f})",
                         i == C::Right ? "right" : "left", r.contact.line ? "weapon" : "hand",
                         B::Length(v), r.contact.normal.x, r.contact.normal.y,
                         r.contact.normal.z);
            }
        }
        const WW::Out o = w.touch[i].Step(sample);
        if (o.strike) {
            ++out.strike_serial;
            out.strike_speed = o.strike_speed;
        }
        out.scraping = o.scraping;
        out.scrape_speed = o.scrape_speed;
        w.last[i] = target[i];
        w.have_last[i] = true;
    }
    // The two weapons against each other (the right blade, the left gun), each kept to its side.
    WW::Sample clash{};
    clash.dt = dt;
    bool met = false;
    if (mine[0] && mine[1] && solid[0] && solid[1] && shape[0].line && shape[1].line) {
        const std::array<S::Pose, 2> was = pose;
        const S::Touch k =
            S::Separate(pose[C::Right], shape[C::Right], pose[C::Left], shape[C::Left],
                        w.clash_met ? &w.clash_side : nullptr, S::CrossM * scale,
                        had[0] && had[1] ? &prev[C::Right] : nullptr,
                        had[0] && had[1] ? &prev[C::Left] : nullptr,
                        w.clash_met ? w.clash_share : 0.5f);
        met = k.any;
        w.clash_side = k.normal;
        w.clash_share = k.a_share;
        if (k.any) {
            // Each pushed only where the map lets it (never into a wall); one that cannot move
            // leaves the whole push to the other, else they stay where they were.
            const std::array<B::Vec3, 2> push{pose[0].p - was[0].p, pose[1].p - was[1].p};
            std::array<bool, 2> moved{};
            for (int i = 0; i < 2; ++i) {
                const B::Vec3 by = push[i];
                pose[i] = was[i];
                moved[i] = w.weapon[i].Push(by, shape[i], cfg, cast, pose[i]);
            }
            if (moved[0] != moved[1]) {
                const int i = moved[0] ? 0 : 1;
                w.weapon[i].Push(-push[1 - i], shape[i], cfg, cast, pose[i]);
            }
        }
        if (feel && k.any && had[0] && had[1] && dt > 0.0f) {
            const auto motion = [&](int i) {
                const B::Vec3 local = B::Rotate(B::Conjugate(pose[i].q), k.point - pose[i].p);
                return (S::At(target[i], local) - S::At(prev[i], local)) * (1.0f / (dt * scale));
            };
            const B::Vec3 v = motion(C::Right) - motion(C::Left);
            clash.contact = true;
            clash.normal = k.normal;
            clash.v = v;
            s.blade.solid_point = k.point;
            w.state.clash.steel = true;
            put(w.state.clash.point, track(k.point));
            put(w.state.clash.normal, B::WorldToTracking(k.normal, s.anchor.base_yaw, 1.0f));
            put(w.state.clash.slide,
                B::WorldToTracking(B::Perp(v, k.normal), s.anchor.base_yaw, 1.0f));
        }
    }
    w.clash_met = met;
    s.blade.solid_fresh = feel;
    s.blade.solid = {clash.dt, clash.contact, clash.normal, clash.v};
    if (feel) {
        const WW::Out oc = w.clash.Step(clash);
        if (oc.strike) {
            ++w.state.clash.strike_serial;
            w.state.clash.strike_speed = oc.strike_speed;
        }
        w.state.clash.scraping = oc.scraping;
        w.state.clash.scrape_speed = oc.scrape_speed;
    }
    // The left hand on the right weapon's handle (eased by the grip's weight) goes where it went.
    const bool ride = own[C::Left] && two > 0.0f && mine[C::Right];
    if (ride) {
        const B::ModelTarget& t = *in.target[C::Left];
        const B::Vec3 p = B::ToWorld(s.frame, t.p);
        const B::Quat turn = B::Normalize(pose[C::Right].q * B::Conjugate(target[C::Right].q));
        const B::Vec3 held = pose[C::Right].p + B::Rotate(turn, p - target[C::Right].p);
        pose[C::Left] = {B::Lerp(p, held, two),
                         B::Normalize(B::ScaleRotation(turn, two) * (s.frame.q * t.q))};
    }
    // The arms hold the weapons where they are.
    for (int i = 0; i < 2; ++i) {
        if (!mine[i] && !(i == C::Left && ride)) {
            continue;
        }
        in.target[i]->p = B::ToModel(s.frame, pose[i].p);
        in.target[i]->q = B::Normalize(B::Conjugate(s.frame.q) * pose[i].q);
        s.target[i] = *in.target[i];
    }
    w.state.ns = now;
    WW::Channel().Store(w.state);
}

// blade_sounds (core/vr/blade_sounds.h), after SolidWeapons: the two weapons' lines (HandShape)
// meeting, heard through BladeSound::Director and played with the game's PlaySound where they
// meet. With weapon_wall on, its solid clash is what is heard; off, the weapons pass through each
// other and their lines meet here: closer than their thicknesses, or crossed over since the last
// update (a quick hit goes through in one), with the motion of the controllers where they meet.
// A left hand on the right weapon's handle (the two-handed grip) is one weapon. A snap turn and a
// stereo pair's second eye are no motion (as SolidWeapons). The hand on a lever (`lever_hand`, its
// weapon hidden) is not the hunter's to ring, as when the game has the arm (-1: none).
// Game:Main (the body's pose copy).
void BladeSounds(PoseSide& s, B::BodyInput& in, bool driven, int lever_hand, s64 now) {
    namespace S = VR::Solid;
    namespace BS = VR::BladeSound;
    BladeSide& b = s.blade;
    const BS::Settings& set = BS::GetSettings();
    // The grind (blade_grind): the director's level, published on every way out (an update that
    // hears nothing new holds it; the mixer silences a level not renewed for 150 ms).
    const bool grinding = set.on && set.grind != 0 && VR::Grind::GrindReady();
    struct Publish {
        const BS::Settings& set;
        const BladeSide& b;
        bool grinding;
        s64 now;
        ~Publish() {
            VR::Grind::PublishGrind({now, grinding ? b.director.Grind() : 0.0f, b.grind_pan,
                                     set.grind == 2 ? VR::Grind::LowPitch : 1.0f,
                                     set.grind_volume});
        }
    } publish{set, b, grinding, now};
    const bool solid = VR::Tricks::GetTrickSettings().weapon_wall && s.solid.active;
    const float two = s.have_used ? std::clamp(s.used.two_hand, 0.0f, 1.0f) : 0.0f;
    std::array<bool, 2> own{};
    for (int i = 0; i < 2; ++i) {
        own[i] = driven && s.frame.valid && in.target[i].has_value() &&
                 s.arm_weight[i] >= B::DetachWatch::OwnedWeight && i != lever_hand;
    }
    if (!set.on || g_play_sound == nullptr || !own[0] || !own[1] || two > 0.0f) {
        b.have_last = false;
        b.have_side = false;
        b.solid_fresh = false;
        b.last_ns = 0;
        b.director.Step({0.2f, false, {}, {}}, set, grinding); // apart (its contact over)
        return;
    }
    const float turn =
        b.have_yaw ? std::remainder(s.anchor.base_yaw - b.yaw, 2.0f * 3.14159265f) : 0.0f;
    const bool snap = std::abs(turn) > 8.0f * B::Deg;
    b.yaw = s.anchor.base_yaw;
    b.have_yaw = true;
    if (s.reused || snap) {
        b.have_last = b.have_last && !snap;
        b.have_side = b.have_side && !snap;
        return;
    }
    const float dt = b.last_ns != 0
                         ? std::clamp(static_cast<float>(now - b.last_ns) * 1e-9f, 0.0f, 0.1f)
                         : 0.0f;
    b.last_ns = now;
    const float scale = std::max(s.anchor.world_scale, 1e-3f);
    BS::Sample sample{};
    sample.dt = dt;
    B::Vec3 point{};
    if (solid) {
        // (The lines' own motion starts afresh if weapon_wall goes off: no stale pose.)
        b.have_last = false;
        b.have_side = false;
        if (!b.solid_fresh) {
            return;
        }
        b.solid_fresh = false;
        sample = b.solid;
        point = b.solid_point;
    } else {
        std::array<S::Shape, 2> shape{};
        std::array<S::Pose, 2> pose{};
        for (int i = 0; i < 2; ++i) {
            HandShape(i, scale, shape[i]);
            const B::ModelTarget& t = *in.target[i];
            pose[i] = {B::ToWorld(s.frame, t.p), B::Normalize(s.frame.q * t.q)};
        }
        const S::Shape& sr = shape[C::Right];
        const S::Shape& sl = shape[C::Left];
        if (!sr.line || !sl.line) {
            b.have_last = false;
            b.have_side = false;
            b.director.Step({dt, false, {}, {}}, set, grinding);
            return;
        }
        const B::Vec3 a0 = S::At(pose[C::Right], sr.base);
        const B::Vec3 a1 = S::At(pose[C::Right], sr.tip);
        const B::Vec3 b0 = S::At(pose[C::Left], sl.base);
        const B::Vec3 b1 = S::At(pose[C::Left], sl.tip);
        float ta = 0.0f;
        float tb = 0.0f;
        S::Closest(a0, a1, b0, b1, ta, tb);
        const B::Vec3 pa = B::Lerp(a0, a1, ta);
        const B::Vec3 pb = B::Lerp(b0, b1, tb);
        const B::Vec3 d = pa - pb;
        const float dist = B::Length(d);
        const float reach = sr.line_r + sl.line_r;
        // Crossed over since the last update: the side flipped where both lines go on past the
        // point (not off an end), and they are still near.
        const bool inside = ta > 0.02f && ta < 0.98f && tb > 0.02f && tb < 0.98f;
        const bool crossed = b.have_side && inside && B::Dot(d, b.side) < 0.0f &&
                             dist < 0.12f * scale;
        const bool met = dist < reach || crossed;
        const B::Vec3 across = B::Normalize(B::Cross(a1 - a0, b1 - b0),
                                            B::AnyPerpendicular(a1 - a0));
        const B::Vec3 normal = crossed ? b.side : B::Normalize(d, b.have_side ? b.side : across);
        if (dist > 1e-4f * scale) {
            b.side = d * (1.0f / dist);
            b.have_side = true;
        }
        point = B::Lerp(pa, pb, 0.5f);
        if (met && b.have_last && dt > 0.0f) {
            const auto motion = [&](int i) {
                const B::Vec3 local = B::Rotate(B::Conjugate(pose[i].q), point - pose[i].p);
                return (S::At(pose[i], local) - S::At(b.last[i], local)) * (1.0f / (dt * scale));
            };
            sample.contact = true;
            sample.normal = normal;
            sample.v = motion(C::Right) - motion(C::Left);
        }
        b.last = pose;
        b.have_last = true;
    }
    if (dt <= 0.0f) {
        return;
    }
    const BS::Out o = b.director.Step(sample, set, grinding);
    if (o.grind > 0.0f && s.have_used && s.used.head_valid) {
        // Left or right of the head, as the whip's drag (the mix turns with it).
        const B::Vec3 eye = s.anchor.eye_world +
                            B::TrackingToWorld(s.used.head_p - s.anchor.head_k,
                                               s.anchor.base_yaw, s.anchor.world_scale);
        const B::Vec3 right = B::TrackingToWorld(
            B::Rotate(B::Normalize(s.used.head_q), {1.0f, 0.0f, 0.0f}), s.anchor.base_yaw, 1.0f);
        b.grind_pan = std::clamp(B::Dot(B::Normalize(point - eye), right), -0.85f, 0.85f);
    }
    if (o.id < 0) {
        return;
    }
    alignas(16) std::array<float, 4> pos{point.x, point.y, point.z, 1.0f};
    u64 handle = 0;
    const s32 type = g_sound_table != nullptr && Read<u8>(g_sound_table + 3) != 0 ? 0xA : 1;
    g_play_sound(&handle, pos.data(), type, o.id);
    if (b.played < 20 || BladeSoundLog()) {
        ++b.played;
        LOG_INFO(Core, "VR blade: {} c{} (type {}) at {:.2f} m/s, {}, h={:#x}",
                 BS::KindName(o.kind), o.id, type, o.speed,
                 solid ? "the solid clash" : "the lines met", handle);
    }
}

// The bow's grip on the fist, learned at full draw (VR::Bow::HeldBow): the hold stays the player's
// own between shots. Game:Main (the body's pose copy) only.
VR::Bow::BowHold g_bow_hold{};

// Two hands on the weapon, the right one slid along it (the presenter's SupportGrip): the weapon
// moves in the hunter's right fist, the fist stays on its controller. Only R_Weapon moves, after
// the arms are solved, so his hand keeps its place and the weapon's hit dummies follow it.
void ApplyTwoHandShift(PoseSide& s) {
    if (!s.have_used || s.arm_weight[C::Right] <= 0.0f) {
        return;
    }
    const B::Vec3 shift = s.used.two_hand_weapon_shift;
    if (!B::IsFinite(shift) || B::LengthSq(shift) < 1e-10f) {
        return;
    }
    auto& weapon = s.locals[C::R_Weapon];
    weapon.t = weapon.t + B::Rotate(weapon.q, shift);
    s.out.written.set(C::R_Weapon);
}

// Keep the bow in its VR grip even with an empty string hand after firing.
// Place the game's arrow after IK when present, moving only the weapon bones. Both endpoints
// come from this stereo-paired pose, so the shaft cannot drift off the bow due to
// a newer controller sample or a freely rotating string-hand wrist.
void ApplyNativeBow(PoseSide& s, const u8* chr) {
    MeleeBowString({}, 0);
    const auto* sk = C::Active();
    const int bi = s.used.bow_hand;
    if (!sk || !VR::Bow::GetBowSettings().enabled || bi < 0 || bi > 1 ||
        s.weapon_family != 320 || !s.solved || !s.out.ok || s.arm_weight[bi] <= 0.0f) {
        return;
    }
    const bool arrow_present = s.used.bow_arrow && s.arm_weight[1 - bi] > 0.0f;
    VR::Bow::NativeGeometry geometry{};
    if (!MeleeBowGeometry(chr, geometry, arrow_present)) return;
    const bool nocked = arrow_present && s.used.bow_nocked;
    s.out.written |= VR::Bow::PoseNativeArrow(
        s.locals.data(), *sk, bi, nocked, geometry,
        VR::Bow::GetBowSettings().arrow_pitch_deg * B::Deg, g_bow_hold, arrow_present);
    if (nocked) {
        const auto& bd = bi == C::Left ? sk->left_weapon_base : sk->right_weapon_base;
        const auto& ad = bi == C::Left ? sk->right_weapon_base : sk->left_weapon_base;
        const auto bb = B::ModelOf(s.locals.data(), C::Parents.data(), C::BoneCount, bd.bone);
        const auto ab = B::ModelOf(s.locals.data(), C::Parents.data(), C::BoneCount, ad.bone);
        if (bb && ab) {
            const auto bow = B::Compose(*bb, VR::Bow::Attachment(bd));
            const auto arrow = B::Compose(B::Compose(*ab, VR::Bow::Attachment(ad)), geometry.arrow_root);
            const auto nock = arrow.t + B::Rotate(arrow.q, {0, VR::Bow::NativeNockM * arrow.s.y, 0});
            MeleeBowString(B::Rotate(B::Conjugate(bow.q), nock - bow.t), s.used.steady_ns);
        }
    }
}

void SolveBody(PoseSide& s, const u8* chr, const u8* pose, B::HkQsTransform* local, s64 now) {
    const auto& settings = B::GetBodySettings();
    s.solved = false;
    // A fresh copy holds the game's whole pose again: nothing of ours is left in it.
    s.edits_in_pose = false;
    const bool driven = s.inactive == nullptr;
    // Not driven without a change of the view (a long hitch, W not a rotation for a frame): the
    // last targets and inputs stay and the weights ease out (arm_blend_out_ms) instead of
    // snapping the arms, torso and upper body back to the animation.
    const bool easing =
        !driven && !s.view_off && s.have_inputs &&
        (s.arm_w[0].Value() > 0.0f || s.arm_w[1].Value() > 0.0f || s.body_w.Value() > 0.0f);
    if (!driven && !easing) {
        // Not driven: no write, and everything eases in again when it is.
        s.solver.Reset();
        for (size_t i = 0; i < 2; ++i) {
            s.arm_w[i].Set(false);
            s.arm_weight[i] = 0.0f;
            s.hold[i].Reset();
            s.use[i] = B::HandUse::Lost;
            s.have_target[i] = false;
            s.detach[i].Reset();
        }
        s.body_w.Set(false);
        s.body_weight = 0.0f;
        s.last_solve_ns = 0;
        s.have_inputs = false;
        return;
    }
    s.easing += easing ? 1 : 0;
    float dt = s.last_solve_ns != 0
                   ? std::clamp(static_cast<float>(now - s.last_solve_ns) * 1e-9f, 0.0f, 0.1f)
                   : 0.0f;
    if (easing) {
        // At most one game frame: the first easing frame after a long stall would otherwise take
        // the stall's 0.1 s and drop the weights at once (arm_blend_out_ms is 80 ms).
        dt = std::min(dt, std::clamp(VR::GameFrameSeconds(), 1.0f / 120.0f, 1.0f / 15.0f));
    }
    s.last_solve_ns = now;
    const float blend_in = settings.arm_blend_in_ms * 0.001f;
    const float blend_out = settings.arm_blend_out_ms * 0.001f;
    const float blade_pitch = VR::GetSettings().blade_pitch_deg;
    const B::GateOut& g = s.gate_out;
    const bool dead = g.reason == B::GateReason::Dead;
    const bool hands_fresh = s.have_used && s.hands_age_ns <= HandsMaxAgeNs;

    B::BodyInput in{};
    in.dt = dt;
    // The sample's own time and head for the hold's glitch check (a jump no hand can make, unless
    // the head jumped with it: a runtime recentre).
    const s64 sample_ns = s.used.xr_time > 0 ? s.used.xr_time : s.used.steady_ns;
    const B::HeadPose head =
        hands_fresh ? B::HeadPose{s.used.head_valid, s.used.head_p, s.used.head_q} : B::HeadPose{};
    if (s.have_used && s.used.space_serial != s.hold_space) {
        // A recenter: the held hands (and a regain's ease) move into the new tracking space.
        VR::Space::Shift shift{};
        VR::SpaceShiftBetween(s.hold_space, s.used.space_serial, shift);
        for (auto& hold : s.hold) {
            hold.MoveSpace(shift);
        }
        s.hold_space = s.used.space_serial;
    }
    VR::Lever::HandPresentation lever_hand{};
    const bool lever_present = VR::Lever::Channel().PresentedHand(lever_hand) &&
        VR::Lever::Fresh(lever_hand.ns, now) && lever_hand.space == s.used.space_serial &&
        lever_hand.hand >= 0 && lever_hand.hand <= 1;
    for (int side = 0; side < 2; ++side) {
        const auto i = static_cast<size_t>(side);
        if (driven) {
            // A sample the XR thread stopped refreshing counts as lost (held 0.3 s first).
            B::TrackedHand hand{};
            s.use[i] = s.hold[i].Update(hands_fresh ? s.used.hand[i] : B::TrackedHand{}, now,
                                        HoldNs, hand, sample_ns, head);
            s.glitches += s.hold[i].rejected ? 1 : 0;
            if (s.use[i] != B::HandUse::Lost) {
                // The right hand's weapon has its own grip (weapon_grip_<weapon>, the live
                // adjust); the left hand keeps the global trims.
                if (lever_present && lever_hand.hand == side) {
                    // Cache the closed-fist contact points once the runtime skeleton exists.
                    static const std::array<B::Vec3, 2> centres{
                        VR::Lever::GripCentre(C::Ref(), C::Left),
                        VR::Lever::GripCentre(C::Ref(), C::Right)};
                    const int effector = side == C::Right ? C::R_Weapon : C::L_Weapon;
                    const auto handle = B::HandToModel(s.anchor, s.frame, hand,
                                                       B::DefaultGrip(side, 0.0f));
                    const auto attachment = s.levelup_pose ? s.standing_pose[effector]
                                                          : B::FromHavok(local[effector]);
                    s.target[i] = VR::Lever::ContactTarget(handle, attachment, centres[i]);
                } else {
                    const B::GripTrim trim = side == C::Right ? s.grip : B::DefaultWeaponGrip();
                    const B::Quat grip = B::DefaultGrip(side, blade_pitch, trim.yaw_deg,
                                                        trim.pitch_deg, trim.roll_deg);
                    s.target[i] = B::HandToModel(s.anchor, s.frame,
                                                 B::OffsetGrip(hand, B::TrimOffset(trim)), grip);
                    if (side == C::Left) {
                        GunKick(s.target[i], now);
                    } else {
                        MeleeBite(s.target[i], now);
                        const B::ModelTarget unkicked = s.target[i];
                        BladeKick(s.target[i], now); // a blade's shot (core/vr/bloodborne_gun.h)
                        // Both hands on it: the hand on the shaft rides the kick (the left one is
                        // placed first: side 0).
                        const float two = s.have_used ? s.used.two_hand : 0.0f;
                        if (two > 0.0f && s.have_target[C::Left] &&
                            !(lever_present && lever_hand.hand == C::Left)) {
                            s.target[C::Left] = VR::Gun::CarryKick(unkicked, s.target[i],
                                                                   s.target[C::Left], two);
                            in.target[C::Left] = s.target[C::Left];
                        }

                    }
                }
                if (side == C::Right && hands_fresh && s.used.music_box_weight > 0.0f &&
                    !(lever_present && lever_hand.hand == side)) {
                    static const auto pinch = VR::MusicBox::CrankContact(C::Ref());
                    auto contact_hand = hand;
                    contact_hand.p = s.used.music_box_contact;
                    contact_hand.q = s.used.music_box_frame;
                    const auto contact = B::HandToModel(s.anchor, s.frame, contact_hand, B::Quat{});
                    const auto attachment = s.levelup_pose ? s.standing_pose[C::R_Weapon]
                                                          : B::FromHavok(local[C::R_Weapon]);
                    const auto target = VR::MusicBox::CrankHandTarget(contact, attachment, pinch);
                    const float w = std::clamp(s.used.music_box_weight, 0.0f, 1.0f);
                    s.target[i].p = s.target[i].p + (target.p - s.target[i].p)*w;
                    s.target[i].q = B::Slerp(s.target[i].q, target.q, w);
                }
                s.have_target[i] = true;
            }
        }
        // Lost: the last target, in model space, while the arm eases back to the animation.
        const bool wanted = driven && settings.arm_ik && g.ik[i] && s.use[i] != B::HandUse::Lost;
        const bool slow = s.use[i] == B::HandUse::Lost || dead;
        s.arm_weight[i] = s.arm_w[i].Update(wanted && s.have_target[i], dt, blend_in,
                                            slow ? SlowBlendOutS : blend_out);
        if (!s.have_target[i]) {
            s.arm_w[i].Set(false);
            s.arm_weight[i] = 0.0f;
        }
        in.target[i] = s.have_target[i] ? std::optional<B::ModelTarget>{s.target[i]} : std::nullopt;
        in.arm_weight[i] = s.arm_weight[i];
        ArmWindow& window = s.arm[i];
        window.lost += s.use[i] == B::HandUse::Lost ? 1 : 0;
        window.held += s.use[i] == B::HandUse::Held ? 1 : 0;
    }
    s.body_weight =
        s.body_w.Update(driven && g.torso && (settings.torso_follow || settings.body_align), dt,
                        blend_in, dead ? SlowBlendOutS : blend_out);
    in.body_weight = s.body_weight;
    if (driven) {
        // body_height_from_head: the comfort camera's vertical smoothing (stairs, slopes) and a
        // terrain offset between its physics position and the hunter's model (camera_dip) are
        // left out of the body's height goal: only your real head's height moves him (the hands
        // stay relative to the camera you see).
        B::Vec3 eye_rel = s.anchor.eye_rel;
        if (VR::Tune::Get("body.height_from_head", settings.body_height_from_head ? 1.0f : 0.0f) !=
                0.0f &&
            std::isfinite(s.anchor.camera_dip)) {
            eye_rel.y -= s.anchor.camera_dip;
        }
        s.last_eye_m = B::RotateToModel(s.frame, eye_rel);
        // fp_ahead_m / fp_ahead_down_m: the camera leads the hunter's eyes by this much along the
        // view's heading (bloodborne_camera.cpp); his eyes stay behind it, so the whole body
        // (legs included) is that much behind the camera and nothing between them bends.
        const float ahead =
            VR::FirstPersonAheadM(s.anchor.view_pitch) * std::max(s.anchor.world_scale, 1e-3f);
        s.last_ahead_m = B::RotateToModel(
            s.frame, B::Vec3{std::sin(s.anchor.view_yaw) * ahead, 0.0f,
                             std::cos(s.anchor.view_yaw) * ahead});
        s.last_head_yaw = s.anchor.view_yaw;
        s.last_view_pitch = s.anchor.view_pitch;
        s.have_inputs = true;
    }
    if (s.frame.valid) {
        s.last_chr_yaw = B::ChrYaw(s.frame);
    }
    in.chr_yaw = s.last_chr_yaw;
    in.head_yaw = s.last_head_yaw;
    in.eye_m = s.last_eye_m - s.last_ahead_m;
    // All descendants of Master (legs, torso, arms and attached weapons) move
    // with the same anchor as the eye and controller targets, before the solve.
    in.rig_offset = s.frame.valid ? B::RotateToModel(s.frame, s.anchor.rig_offset) : B::Vec3{};
    in.hide = B::EffectiveBodyMode() != B::BodyMode::Full;
    B::BodyParams params = Params(settings);
    // A duck: his upper body follows the camera as deep as the camera goes (head_room.h: the
    // camera's deepest drop, positional_limit_down_m, 0.80 m by default), never less than the
    // solver's own 0.60 m. With 0.60 a straight duck past it left his eyes (and shoulders) above
    // yours. VR_TUNE_FILE body.align_down_m: a fixed depth (A/B tests).
    {
        const auto& vs = VR::GetSettings();
        // The floor guard from the camera's standing eye (his own + your height's offset).
        const float down = VR::HeadRoom::DownLimit(
            vs.positional_limit_m, vs.positional_limit_down_m,
            VR::PlayerBody::StandingEyeM(VR::HeightEyeOffsetM(), vs.fp_eye_height_m));
        const float deepest =
            vs.positional ? VR::HeadRoom::MaxDrop(vs.positional_limit_m, down) : 0.0f;
        params.align_down = VR::Tune::Get(
            "body.align_down_m",
            std::max(params.align_down, deepest * std::max(s.anchor.world_scale, 1e-3f)));
    }
    // Your height (player_body.h; the page's Height and Adjust height): the camera's standing eye
    // against the hunter's (model units; exactly 0 As the hunter + 0 cm): lower, his upper body
    // comes down to your eye; higher, his arms stretch further toward hands his shoulders are
    // below; his arms k as long (k = 1 + the offset over a 167 cm person's eye height). The body's
    // offset (HeightBodyOffsetM): the camera's, except As the player your head above the origin
    // adds to it (standing up after a recenter taken bent).
    {
        const auto& vs = VR::GetSettings();
        const float eye_offset = VR::HeightBodyOffsetM();
        params.arm_scale = VR::PlayerBody::BodyScaleFor(eye_offset);
        const float world = std::max(s.anchor.world_scale, 1e-3f);
        const float game_eye = eye_offset * world;
        const float camera_eye = VR::HeightEyeOffsetM() * world;
        params.stand_drop = VR::PlayerBody::StandDrop(game_eye);
        // Taller: toward hands his shoulders are below; shorter arms (k < 1): back out to his own
        // full reach toward a hand out of theirs.
        params.rise_stretch = VR::PlayerBody::RiseStretch(game_eye, params.arm_scale) +
                              VR::PlayerBody::ShortStretch(params.arm_scale);
        // Logged on a change of the camera's by 5 mm (As the player's floor moves a little with the
        // runtime), or of the body's by 3 cm at most every 2 s (your head above the origin).
        const s64 log_now = B::SteadyNowNs();
        if (std::isnan(s.logged_game_eye) || std::abs(camera_eye - s.logged_game_eye) >= 0.005f ||
            (camera_eye == 0.0f) != (s.logged_game_eye == 0.0f) ||
            (std::abs(game_eye - s.logged_body_eye) >= 0.03f &&
             log_now - s.logged_body_ns >= 2'000'000'000)) {
            s.logged_game_eye = camera_eye;
            s.logged_body_eye = game_eye;
            s.logged_body_ns = log_now;
            LOG_INFO(Core,
                     "VR body: arms x{:.3f} (a {:.0f} cm person's{}); {} {}: the eye {:+.3f} from "
                     "the hunter's{}: {}",
                     params.arm_scale, VR::PlayerBody::StatureCm(eye_offset, vs.fp_eye_height_m),
                     params.arm_scale == 1.0f ? ", the hunter's own" : "",
                     vs.height_player ? "As the player" : "As the hunter",
                     VR::PlayerBody::AdjustText(vs.height_adjust_cm), game_eye,
                     game_eye != camera_eye
                         ? fmt::format(" (the camera's standing eye {:+.3f}, your head above "
                                       "the origin)",
                                       camera_eye)
                         : std::string{},
                     params.stand_drop > 0.0f
                         ? fmt::format("his stance {:.3f} lower", params.stand_drop)
                     : params.rise_stretch > 0.0f
                         ? fmt::format("his arms stretch up to {:.0f}% more toward hands out of "
                                       "reach",
                                       params.rise_stretch * 100.0f)
                         : std::string{"his own stance"});
        }
    }
    // Looking down: the upper body further back (body_back_down_m), so the coat and collar right
    // under the eye leave the view; level and upward views keep body_back_m.
    {
        const float t = std::clamp(
            (s.last_view_pitch - BackDownStart) / (BackDownFull - BackDownStart), 0.0f, 1.0f);
        const float down = VR::Tune::Get("body.back_down_m", settings.body_back_down_m);
        params.back += down * t * t * (3.0f - 2.0f * t);
    }
    for (int i = 0; i < C::BoneCount; ++i) {
        if (s.levelup_pose) {
            s.locals[static_cast<size_t>(i)] = s.standing_pose[static_cast<size_t>(i)];
        } else {
            s.locals[static_cast<size_t>(i)] = B::FromHavok(local[i]);
            s.standing_pose[static_cast<size_t>(i)] = s.locals[static_cast<size_t>(i)];
        }
    }
    {
        VR::FeatureCost::Scope cost{VR::FeatureCost::TwoHandObserve};
        ObserveTwoHand(s, chr, dt, now); // the game's two-handed grip, before our edits
    }
    const bool posture_log =
        PostureLogEvery() > 0 && (s.posture_frames++ % static_cast<u64>(PostureLogEvery())) == 0;
    if (posture_log) {
        B::LocalToModel(s.locals.data(), C::Parents.data(), C::BoneCount, s.posture_model.data());
        s.posture_anim = B::MeasurePosture(s.posture_model.data());
    }
    if (!s.levelup_pose) s.standing_player = chr;
    // The hand on a lever (its fist on the handle, its weapon hidden): neither solid nor ringing.
    const int lever_held = lever_present ? lever_hand.hand : -1;
    SolidWeapons(s, in, driven && !dead, lever_held, now); // weapon_wall: the weapons against the
                                                           // walls (off: returns at once)
    BladeSounds(s, in, driven && !dead, lever_held, now); // blade_sounds: the two weapons ring
    in.two_hand = s.have_used ? s.used.two_hand : 0.0f;
    s.out = s.solver.Solve(s.locals.data(), in, params);
    if (s.levelup_pose && s.out.ok) s.out.written.set();
    s.solved = true;
    if (posture_log && s.out.ok) {
        B::LocalToModel(s.locals.data(), C::Parents.data(), C::BoneCount, s.posture_model.data());
        LogPosture(s, in, B::MeasurePosture(s.posture_model.data()), now);
    }
    if (!s.out.ok) {
        // Nothing is written: this frame shows the game's own pose.
        ++s.failures;
        s.fail_why = s.out.fail;
        if (now - s.last_fail_log_ns >= FailLogNs) {
            s.last_fail_log_ns = now;
            LOG_WARNING(Core, "VR body: solve failed ({}); this frame keeps the game's pose",
                        s.out.fail != nullptr ? s.out.fail : "?");
        }
        return;
    }
    s.torso_max = std::max(s.torso_max, std::abs(s.out.torso_delta));
    s.shift_max = std::max(s.shift_max, B::Length(s.out.shift));
    s.residual_max = std::max(s.residual_max, s.out.eye_residual);
    for (size_t i = 0; i < 2; ++i) {
        const B::ArmResult& r = s.out.arm[i];
        ArmWindow& window = s.arm[i];
        if (r.status == B::ArmStatus::Skipped) {
            continue;
        }
        if (r.status != B::ArmStatus::Solved) {
            ++window.failed;
            continue;
        }
        ++window.solved;
        window.reached += r.reached ? 1 : 0;
        window.detached += B::Detached(r) ? 1 : 0;
        window.max_error = std::max(window.max_error, r.error);
    }
    // A tracked hand the hunter's arm cannot reach (its weapon bone more than 3 cm off the
    // controller): a tick on that hand, at most once per 0.5 s (body_haptics; not in the dry run,
    // where the game's arms are shown).
    for (int side = 0; side < 2; ++side) {
        const auto i = static_cast<size_t>(side);
        const B::ArmResult& r = s.out.arm[i];
        const u32 before = s.detach[i].count;
        const bool tick = s.detach[i].Update(s.arm_weight[i],
                                             driven && r.status == B::ArmStatus::Solved &&
                                                 s.use[i] == B::HandUse::Tracked,
                                             r.reached, r.error, now);
        if (s.detach[i].count == before) {
            continue;
        }
        ++s.arm[i].detaches;
        ++s.perf_detaches;
        // (Not while a kept hit or dodge moves his shoulders: the game did it, not your hand.)
        const bool kept = s.gate_out.kept;
        if (tick && settings.body_haptics && !DryRun() && !kept) {
            B::PushBodyEvent({B::BodyEventKind::Detached, side});
        }
        if (LogLevel() > 0) {
            LOG_INFO(Core,
                     "VR arm: {} hand detached: {:.1f} cm off the controller, stretch {:.3f}{}",
                     side == C::Right ? "R" : "L", r.error * 100.0f, r.stretch,
                     kept   ? " (kept clip: no tick)"
                     : tick ? ""
                            : " (no tick: one played less than 0.5 s ago)");
        }
    }
    // VR_TUNE_FILE body.write=0 (tests): like the dry run, the game's own pose is shown (the
    // posture log's "anim" side) while everything else runs.
    if (DryRun() || (VR::Tune::Active() && VR::Tune::Get("body.write", 1.0f) == 0.0f)) {
        return;
    }
    ApplyNativeBow(s, chr);
    ApplyTwoHandShift(s);
    // t.xyz, q and s.xyz of the written bones; the w lanes keep what the game left there. The
    // game's own locals of those bones are kept for RestoreGame.
    for (int i = 0; i < C::BoneCount; ++i) {
        if (s.out.written.test(static_cast<size_t>(i))) {
            s.animated[static_cast<size_t>(i)] = local[i];
            B::ToHavok(s.locals[static_cast<size_t>(i)], local[i]);
        }
    }
    s.edited = s.out.written;
    LogFingers(s, local, now);
    ApplyGrip(s, local, dt);
    ApplyLeverGrip(s, local, now);
    ApplyMusicBoxGrip(s, local, now);
    std::memcpy(s.written_locals.data(), local, sizeof(s.written_locals));
    s.edits_in_pose = s.edited.any();
    s.edits_chr = chr;
    s.edits_pose = pose;
    s.edits_local = local;
    ++s.writes;
    ++s.window_writes;
    if (!s.first_write_logged) {
        s.first_write_logged = true;
        LOG_INFO(Core,
                 "VR body: first write: {} bones (arm weights R {:.2f} L {:.2f}, body {:.2f}, "
                 "torso {:.1f} deg, shift {} cm, {} hidden) eye_m {} eye {} arms R {} L {}",
                 s.out.written.count(), s.arm_weight[C::Right], s.arm_weight[C::Left],
                 s.body_weight, s.out.torso_delta / B::Deg, Cm(s.out.shift), s.out.hidden,
                 Pos(in.eye_m), Pos(s.out.eye), B::ArmStatusName(s.out.arm[C::Right].status),
                 B::ArmStatusName(s.out.arm[C::Left].status));
    }
}

// A stale copy (the original skipped it) leaves our last write in the locals. When the
// first-person view is off (cinematic flat, the virtual screen, another camera), every bone we
// wrote gets the game's own local back once (the head and torso shown, the arms and torso in the
// animation's pose), so the flat screen shows the hunter whole from its first frame. The last
// sync already rebuilt the model pose (model in sync, bone flags cleared), and syncModelSpace
// (0x6C4F20) skips a pose in that state: the pose is left the way setPoseLocalSpace (0x6C49B0)
// leaves it (every bone flag 2, u16 0x0100 at +0x38: model out of sync, local in sync), so the
// next vt+0xF0 sync rebuilds the model pose from these locals.
void RestoreGame(PoseSide& s, const u8* chr) {
    if (!s.edits_in_pose || FirstPersonNow()) {
        return;
    }
    s.edits_in_pose = false;
    // Only into the pose we wrote: the same character, pose node, local array and skeleton (a load
    // that replaced the player's pose never gets another pose's locals).
    const char* why = "";
    u8* const pose = PoseNode(chr, why);
    if (pose == nullptr || chr != s.edits_chr || pose != s.edits_pose ||
        Read<const u8*>(pose + OffsetPoseSkeleton) != s.checked_skeleton) {
        return;
    }
    u8* const flags = Read<u8*>(pose + OffsetPoseFlags);
    if (Read<s32>(pose + OffsetPoseFlagsSize) != C::BoneCount || !Plausible(flags, 4)) {
        return;
    }
    auto* const local = Read<B::HkQsTransform*>(pose + OffsetPoseLocal);
    if (local != s.edits_local) {
        return;
    }
    for (int i = 0; i < C::BoneCount; ++i) {
        if (s.edited.test(static_cast<size_t>(i))) {
            local[i] = s.animated[static_cast<size_t>(i)];
        }
    }
    for (int i = 0; i < C::BoneCount; ++i) {
        std::memcpy(flags + 4 * i, &BoneFlagCopied, sizeof(BoneFlagCopied));
    }
    std::memcpy(pose + OffsetPoseSync, &SyncLocalOnly, sizeof(SyncLocalOnly));
    if (LogLevel() > 0) {
        LOG_INFO(Core,
                 "VR body: {} bones back to the game's pose (stale copy, first-person view off)",
                 s.edited.count());
    }
}

// Animation throttling can leave a fully synchronized pose from the preceding
// frame. Keep VR IK live even then, at this existing pose hook before model sync.
u8* RefreshHeldPose(PoseSide& s, const u8* chr, s64 now) {
    if (!s.edits_in_pose || !DrivenNow(now) || chr != s.edits_chr)
        return nullptr;
    const char* why = "";
    auto* pose = PoseNode(chr, why);
    if (!pose || pose != s.edits_pose ||
        Read<const u8*>(pose + OffsetPoseSkeleton) != s.checked_skeleton ||
        Read<u16>(pose + OffsetPoseSync) != 0x0101)
        return nullptr;
    auto* local = Read<B::HkQsTransform*>(pose + OffsetPoseLocal);
    auto* flags = Read<u8*>(pose + OffsetPoseFlags);
    if (local != s.edits_local || Read<s32>(pose + OffsetPoseFlagsSize) != C::BoneCount ||
        !Plausible(flags, 4))
        return nullptr;
    if (!B::RestoreHeldLocals(local, s.animated, s.written_locals, s.edited))
        return nullptr;
    for (int i = 0; i < C::BoneCount; ++i)
        std::memcpy(flags + 4 * i, &BoneFlagCopied, sizeof(BoneFlagCopied));
    std::memcpy(pose + OffsetPoseSync, &SyncLocalOnly, sizeof(SyncLocalOnly));
    s.edits_in_pose = false;
    ++s.held_refreshes;
    return pose;
}

// The frame's input (the game's animated locals), before the body edits them.
void RecordInput(PoseSide& s, const B::HkQsTransform* local) {
    if (!s.record_failed) {
        std::memcpy(s.record_frame.locals.data(), local, sizeof(s.record_frame.locals));
    }
}

// The rest of the frame, after the solve.
void Record(PoseSide& s, const std::array<float, 12>& rows, s64 now) {
    if (s.record_failed) {
        return;
    }
    if (s.record == nullptr) {
        s.record = std::fopen(RecordPath().c_str(), "wb");
        if (s.record == nullptr) {
            s.record_failed = true;
            LOG_WARNING(Core, "VR body: VR_BODY_RECORD: could not create {}", RecordPath());
            return;
        }
        std::setvbuf(s.record, nullptr, _IOFBF, 4 << 20);
        const B::RecordHeader header{};
        std::fwrite(&header, sizeof(header), 1, s.record);
        LOG_INFO(Core, "VR body: recording every fresh pose copy to {} ({} bytes a frame)",
                 RecordPath(), sizeof(B::RecordFrame));
    }
    B::RecordFrame& f = s.record_frame;
    f.steady_ns = now;
    f.w_rows = rows;
    f.anchor = s.anchor;
    f.hands = s.have_used ? s.used : B::HandsSample{};
    f.arm_weight = s.arm_weight;
    f.body_weight = s.body_weight;
    f.flags = (s.inactive == nullptr ? B::RecordAnchorActive : 0u) |
              (s.reused ? B::RecordHandsReused : 0u) | (DryRun() ? B::RecordDryRun : 0u);
    std::fwrite(&f, sizeof(f), 1, s.record);
    s.record_bytes += sizeof(f);
    if (++s.record_unflushed >= 60) {
        s.record_unflushed = 0;
        std::fflush(s.record); // a killed emulator loses at most about a second
    }
    if (s.record_bytes >= RecordMaxBytes) {
        std::fclose(s.record);
        s.record = nullptr;
        s.record_failed = true;
        LOG_INFO(Core, "VR body: recording stopped at {} MB", s.record_bytes >> 20);
    }
}

std::string ArmText(const PoseSide& s, int side) {
    const auto i = static_cast<size_t>(side);
    const char* name = side == C::Right ? "R" : "L";
    const ArmWindow& w = s.arm[i];
    const B::ArmResult& r = s.out.arm[i];
    if (!s.solved || r.status == B::ArmStatus::Skipped) {
        return fmt::format("{} w {:.2f} {} (held {} lost {})", name, s.arm_weight[i],
                           B::HandUseName(s.use[i]), w.held, w.lost);
    }
    return fmt::format("{} w {:.2f} {} {} err {:.1f}/{:.1f} mm reached {}/{} det {} (new {}) fail "
                       "{} st {:.3f} flex {:.0f} roll {:.0f} pole {:.0f} held {} lost {} p_m {} "
                       "trk {}",
                       name, s.arm_weight[i], B::HandUseName(s.use[i]), B::ArmStatusName(r.status),
                       r.error * 1000.0f, w.max_error * 1000.0f, w.reached, w.solved, w.detached,
                       w.detaches, w.failed, r.stretch, r.flex / B::Deg, r.wristRoll / B::Deg,
                       r.poleTurn / B::Deg, w.held, w.lost, Pos(s.target[i].p),
                       Pos(B::ModelToTracking(s.anchor, s.frame, s.target[i].p)));
}

std::string BodyText(const PoseSide& s) {
    if (!s.solved) {
        return "not solved";
    }
    if (!s.out.ok) {
        return fmt::format("FAILED ({})", s.out.fail != nullptr ? s.out.fail : "?");
    }
    return fmt::format("body w {:.2f} torso {:.1f} deg (max {:.1f}) shift {} cm (max {:.1f}) eye "
                       "res {:.1f} cm (max {:.1f}) hidden {} lean {:+.1f} deg (limit {:.1f})",
                       s.body_weight, s.out.torso_delta / B::Deg, s.torso_max / B::Deg,
                       Cm(s.out.shift), s.shift_max * 100.0f, s.out.eye_residual * 100.0f,
                       s.residual_max * 100.0f, s.out.hidden, s.out.lean_fwd / B::Deg,
                       s.out.lean_fwd_limit / B::Deg);
}

void LogWindow(PoseSide& s, s64 now) {
    const u64 others = g_other_calls.exchange(0, std::memory_order_relaxed);
    const u64 busy = g_busy_calls.exchange(0, std::memory_order_relaxed);
    const u32 calls = std::max(s.calls, 1u);
    std::string anchor;
    if (s.inactive != nullptr) {
        anchor = fmt::format("inactive: {}{}", s.inactive, s.solved ? " (easing out)" : "");
    } else {
        anchor = fmt::format("anchor age {} ms eye_m {} eye {} drift {} cm", s.anchor_age_ns / Ms,
                             Pos(B::RotateToModel(s.frame, s.anchor.eye_rel)),
                             B::EyeSourceName(s.eye_source), Cm(s.drift));
    }
    if (s.eye_locked != 0 || s.eye_pair != 0 || s.easing != 0 || s.glitches != 0) {
        anchor += fmt::format(" | eyes locked {} pair-first {} easing {} glitches {}", s.eye_locked,
                              s.eye_pair, s.easing, s.glitches);
    }
    std::string body;
    if (s.solved) {
        const B::GateOut& g = s.gate_out;
        body = fmt::format(" | {} | gate {}{} | {} | {} | {} {} fail {}{}", BodyText(s),
                           B::GateReasonName(g.reason),
                           g.held ? fmt::format(" {} {} {} ms", B::AnimClassName(g.owner),
                                                g.owner_id, g.owner_age_ns / Ms)
                                  : std::string{},
                           ArmText(s, C::Right), ArmText(s, C::Left),
                           DryRun() ? "dry run, solves" : "writes", s.window_writes, s.failures,
                           s.failures != 0 ? fmt::format(" ({})", s.fail_why) : std::string{});
    }
    LOG_INFO(Core,
             "VR body: pose fresh {}/{} ({:.0f}%){}{} other chrs {} busy {} | {} | hands age {} "
             "seq {}{} reuse {}{} | held animation refreshed {} pose gap max {:.1f} ms | "
             "{:.1f}/{:.1f} us",
             s.fresh, s.calls, 100.0f * static_cast<float>(s.fresh) / static_cast<float>(calls),
             s.stale != 0 ? fmt::format(" stale {} ({})", s.stale, s.stale_why) : std::string{},
             s.active != s.fresh ? fmt::format(" active {}", s.active) : std::string{}, others,
             busy, anchor,
             s.hands_age_ns >= 0 ? fmt::format("{} ms", s.hands_age_ns / Ms) : std::string{"-"},
             s.used.seq, s.used.simulated ? " (sim)" : "", s.reuses, body, s.held_refreshes,
             static_cast<double>(s.max_pose_gap_ns) * 1e-6,
             static_cast<double>(s.time_sum_ns) * 1e-3 / static_cast<double>(calls),
             static_cast<double>(s.time_max_ns) * 1e-3);
    s.window_start = now;
    s.calls = s.fresh = s.stale = s.active = s.reuses = s.window_writes = s.failures = 0;
    s.easing = s.eye_locked = s.eye_pair = s.glitches = s.held_refreshes = 0;
    s.torso_max = s.shift_max = s.residual_max = 0.0f;
    s.time_sum_ns = s.time_max_ns = s.max_pose_gap_ns = 0;
    s.arm = {};
}

void FinishCall(PoseSide& s, s64 start) {
    const s64 end = B::SteadyNowNs();
    s.time_sum_ns += end - start;
    s.time_max_ns = std::max(s.time_max_ns, end - start);
    if (LogLevel() == 0) {
        return;
    }
    if (s.window_start == 0) {
        s.window_start = end;
    } else if (end - s.window_start >= WindowNs) {
        LogWindow(s, end);
    }
}

// What the presenter needs (haptics, ghost hands, the grip adjust). `driven`: our edits are in
// the hunter's pose this frame (never in the dry run).
// Trial-only numerical diagnostics: final weapon-bone orientation against its IK target.
// File I/O and FK only run with an explicit path, for at most 180 seconds.
void TraceHandBones(const PoseSide& s, s64 now, bool driven) {
    static const char* path = std::getenv("VR_HAND_BONE_TRACE");
    if (!path || !driven || !s.frame.valid || !s.have_used) return;
    static bool done = false;
    static std::FILE* file = nullptr;
    static s64 start = 0;
    static unsigned frames = 0;
    if (done) return;
    if (!file) {
        file = std::fopen(path,"w");
        if (!file) { done = true; return; }
        start = now;
        std::fprintf(file,"steady_ns,xr_time,seq,hand,weight,detached,have_target,target_qx,target_qy,target_qz,target_qw,bone_qx,bone_qy,bone_qz,bone_qw,solve_status,weapon_written,ik_requested,solver_ok,target_px,target_py,target_pz,bone_px,bone_py,bone_pz,two_hand\n");
    }
    if (now-start > 180'000'000'000LL) { std::fclose(file); file=nullptr; done=true; return; }
    for (int side=0; side<2; ++side) {
        const auto bone=B::ModelOf(s.locals.data(),C::Parents.data(),C::BoneCount,
                                   side==C::Right ? C::R_Weapon : C::L_Weapon);
        if (!bone) continue;
        const auto a=B::Normalize(s.target[side].q), b=B::Normalize(bone->q);
        std::fprintf(file,"%lld,%lld,%llu,%d,%.6f,%d,%d,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%d,%d,%d,%d,%.7f,%.7f,%.7f,%.7f,%.7f,%.7f,%.6f\n",
            static_cast<long long>(now),static_cast<long long>(s.used.xr_time),
            static_cast<unsigned long long>(s.used.seq),side,s.arm_weight[side],
            s.detach[side].detached,s.have_target[side],a.x,a.y,a.z,a.w,b.x,b.y,b.z,b.w,
            static_cast<int>(s.out.arm[side].status),
            s.out.written.test(side==C::Right ? C::R_Weapon : C::L_Weapon),
            s.gate_out.ik[side],s.out.ok,s.target[side].p.x,s.target[side].p.y,s.target[side].p.z,
            bone->t.x,bone->t.y,bone->t.z,s.used.two_hand);
    }
    if (++frames%60==0) std::fflush(file);
}

void PublishStatus(PoseSide& s, s64 now, bool driven) {
    TraceHandBones(s,now,driven);
    B::BodyStatus status{};
    status.grab_eye = s.grab_eye;
    if (driven && !DryRun()) status.throw_placement = s.throw_placement;
    status.seq = ++s.status_seq;
    status.steady_ns = now;
    status.driven = driven && !DryRun();
    for (size_t i = 0; i < 2; ++i) {
        // A kept hit or dodge: the arm is drawn yours, but nothing uses it before the game's
        // cancel window (items, the crank: as when the game had it).
        const bool waits = s.gate_out.kept && !s.gate_out.attack_ok[i];
        status.drawn_weight[i] = status.driven ? s.arm_weight[i] : 0.0f;
        status.arm_weight[i] = waits ? 0.0f : status.drawn_weight[i];
        status.detached[i] = status.driven && s.detach[i].detached;
        status.tracked[i] = s.use[i] != B::HandUse::Lost;
    }
    status.weapon_id = s.weapon_id >= 0 ? s.weapon_id : -1;
    status.weapon_slot = s.weapon_slot;
    status.left_weapon_slot = s.left_weapon_slot;
    status.weapon_family = s.weapon_id >= 0 ? s.weapon_family : -1;
    status.left_weapon_family = s.left_weapon_id >= 0 ? B::Weapons::FamilyOf(s.left_weapon_id) : -1;
    status.guard_serial = s.guard_serial;
    status.guard_clip = s.guard_clip;
    // Physical evasion: leaning counts in idle, locomotion, your attacks, guard and items; never
    // while the game moves the body on its own.
    const B::GateOut& g = s.gate_out;
    const char* block = nullptr;
    switch (g.reason) {
    case B::GateReason::Dead:
        block = "dead";
        break;
    case B::GateReason::Throw:
        block = "throw";
        break;
    case B::GateReason::Ladder:
        block = "ladder";
        break;
    case B::GateReason::Event:
        block = "event";
        break;
    default:
        break;
    }
    // The VR heading (core/vr/bloodborne_facing.cpp) leaves him to the animations that place him
    // themselves; a dodge, a hit or an item keeps it (the game's lock-on keeps its target there
    // too).
    status.facing_block = block;
    if (block == nullptr && g.held) {
        switch (g.owner) {
        case B::AnimClass::Dodge:
            block = "dodge";
            break;
        case B::AnimClass::Grabbed:
            block = "grabbed";
            status.facing_block = block;
            break;
        case B::AnimClass::Damage:
            block = "damage";
            break;
        case B::AnimClass::Death:
            block = "death";
            status.facing_block = block;
            break;
        case B::AnimClass::Event:
            block = "event anim";
            status.facing_block = block;
            break;
        default:
            break;
        }
    }
    // Easing out (not driven, our edits still in the pose): the hurtboxes keep the game's.
    status.roomscale_yaw = s.frame.valid ? B::ChrYaw(s.frame) : 0.0f;
    status.roomscale_ok = status.driven && s.inactive == nullptr && block == nullptr &&
                          !g.held && !g.switching && s.roomscale_idle;
    status.roomscale_why = status.roomscale_ok     ? ""
                           : !status.driven        ? "the body is not driven"
                           : s.inactive != nullptr ? "the body is inactive"
                           : block != nullptr      ? block
                           : g.held                ? "an animation holds the arms"
                           : g.switching           ? "a weapon switch"
                                                   : s.roomscale_idle_why;
    status.evade_ok = status.driven && s.inactive == nullptr && block == nullptr;
    status.evade_block = block != nullptr ? block : (status.evade_ok ? "" : "body not driven");
    if (status.driven && s.frame.valid) {
        const B::Vec3 shift = B::RotateToWorld(s.frame, s.out.shift);
        status.upper_shift = {shift.x, shift.y, shift.z};
        const B::Vec3 lean = B::RotateToWorld(s.frame, s.out.lean_eye);
        status.upper_lean = {lean.x, lean.y, lean.z};
        // A kept hit or dodge: that shift and lean only bring the shoulders under your head for
        // the arms; the hurtboxes keep the game's animation (VR_TUNE_FILE body.hurt_anim=0: they
        // follow the drawn trunk, as before).
        status.look_only = g.kept && VR::Tune::Get("body.hurt_anim", 1.0f) != 0.0f;
        status.cam_ahead_m = s.out.cam_ahead;
        status.cam_above_m = s.out.cam_above;
        status.cam_side_m = s.out.cam_side;
    }
    if (status.driven && s.frame.valid && VR::Quick::GetQuickSettings().holster_mode != 0) {
        if (const auto* sk = C::Active()) {
            // Only the two short ancestor chains, not a second full-skeleton FK pass.
            const auto pelvis =
                B::ModelOf(s.locals.data(), C::Parents.data(), C::BoneCount, C::Pelvis);
            const auto chest =
                B::ModelOf(s.locals.data(), C::Parents.data(), C::BoneCount, C::Spine2);
            if (pelvis && chest) {
                const auto bind = [&](int bone) {
                    const auto& q = sk->model[bone].q;
                    return B::Quat{q[0], q[1], q[2], q[3]};
                };
                status.mounts.frame = {VR::BodyMounts::OfBone(*pelvis, bind(C::Pelvis)),
                                       VR::BodyMounts::OfBone(*chest, bind(C::Spine2))};
                // Skinning/dummies retain the character transform from this pose update.
                // Publish it with the bones; a newer ChrCtrl transform can lead the body.
                status.mounts.character = s.frame;
                status.mounts.space = s.used.space_serial;
                status.mounts.valid = true;
            }
        }
    }
    u64 contact_drain_word = 0;
    if (status.driven && s.frame.valid && s.out.ok && HeldVialOurs(now, contact_drain_word)) {
        auto locals = s.locals;
        // Visibility shrink must not shrink the anatomical head target.
        for (auto& local : locals)
            if (local.s.x == B::HiddenScale && local.s.y == B::HiddenScale &&
                local.s.z == B::HiddenScale) local.s = {1,1,1};
        std::array<B::QsTransform, C::BoneCount> posed{};
        B::LocalToModel(locals.data(), C::Parents.data(), C::BoneCount, posed.data());
        auto& anatomy = status.injection_body;
        anatomy.valid = true;
        anatomy.ns = now;
        anatomy.space = s.used.space_serial;
        anatomy.scale = 1.f / std::max(s.anchor.world_scale, .01f);
        anatomy.grip_valid = B::IsTracked(s.used.hand[C::Left]);
        anatomy.grip_p = s.used.hand[C::Left].p;
        anatomy.grip_q = s.used.hand[C::Left].q;
        anatomy.right_grip_valid = B::IsTracked(s.used.hand[C::Right]);
        anatomy.right_grip_p = s.used.hand[C::Right].p;
        anatomy.right_grip_q = s.used.hand[C::Right].q;
        for (size_t i = 0; i < VR::VialContact::Bones.size(); ++i)
            anatomy.p[i] = B::ModelToTracking(s.anchor, s.frame,
                                             posed[VR::VialContact::Bones[i]].t);
        // Centre of the thumb pad, just beyond its last joint, for the syringe cap.
        const auto& thumb = posed[C::L_Finger02];
        anatomy.p[26] = B::ModelToTracking(s.anchor, s.frame,
            thumb.t + B::Rotate(B::Normalize(thumb.q), B::Vec3{.012f,0,0}));
        if (s.used.head_valid) {
            // Headset exterior: 12 cm radius, centred 2 cm behind the eyes.
            anatomy.p[5] = s.used.head_p+B::Rotate(B::Normalize(s.used.head_q),
                                                  B::Vec3{0,0,.02f});
        }
    }
    status.daze = static_cast<u8>(VR::Daze::LevelOf(g));
    status.stun_clip = VR::HitStun::StunClip(g);
    B::PublishBodyStatus(status);
}

// The time budget (body_budget_us): every call's work (not the logs or the recording) into 5 s
// windows; two windows over it in a row turn the body off. A summary line every 60 s, always.
void CheckBudget(PoseSide& s, s64 work_ns, s64 now) {
    const float budget = B::GetBodySettings().body_budget_us;
    ++s.perf_calls;
    s.perf_sum_ns += work_ns;
    s.perf_max_ns = std::max(s.perf_max_ns, work_ns);
    if (budget > 0.0f && static_cast<float>(work_ns) > budget * 1000.0f) {
        ++s.perf_over;
    }
    const B::BudgetWindow w = s.budget.Add(work_ns, now, budget);
    if (w.closed && w.over) {
        LOG_WARNING(Core,
                    "VR body: over the time budget: {:.0f} us a pose copy on average (max {:.0f} "
                    "us, {} of {} calls over) in the last 5 s, budget body_budget_us={:.0f} ({} "
                    "of {} windows in a row)",
                    w.avg_us, w.max_us, w.over_calls, w.calls, budget, w.over_windows,
                    B::BudgetGuard::WindowsToDisable);
        if (w.exceeded) {
            Disable(fmt::format("over the time budget: {:.0f} us a pose copy on average for {} "
                                "windows of 5 s, body_budget_us={:.0f}",
                                w.avg_us, w.over_windows, budget));
        }
    }
    if (s.perf_start == 0) {
        s.perf_start = now;
    } else if (now - s.perf_start >= PerfLogNs) {
        const u64 calls = std::max<u64>(s.perf_calls, 1);
        LOG_INFO(Core,
                 "VR body: perf {:.0f} s: {} pose copies, body work avg {:.1f} us max {:.1f} us "
                 "(budget {:.0f} us, {} calls over), {} writes, {} detaches",
                 static_cast<double>(now - s.perf_start) * 1e-9, s.perf_calls,
                 static_cast<double>(s.perf_sum_ns) * 1e-3 / static_cast<double>(calls),
                 static_cast<double>(s.perf_max_ns) * 1e-3, budget, s.perf_over,
                 s.writes - s.perf_writes, s.perf_detaches);
        s.perf_start = now;
        s.perf_calls = s.perf_over = s.perf_detaches = 0;
        s.perf_sum_ns = s.perf_max_ns = 0;
        s.perf_writes = s.writes;
    }
}

void MainPoseWritten(u8* chr) {
    auto& s = Pose();
    const s64 now = B::SteadyNowNs();
    if (s.last_pose_call_ns != 0) {
        s.max_pose_gap_ns = std::max(s.max_pose_gap_ns, now - s.last_pose_call_ns);
    }
    s.last_pose_call_ns = now;
    ++s.calls;
    if (!s.first_logged) {
        s.first_logged = true;
        LOG_INFO(Core, "VR body: first main-player pose copy, player={} vtable={} thread '{}'",
                 fmt::ptr(chr), Rva(Read<u8*>(chr)), Common::GetCurrentThreadName());
    }
    const char* why = "";
    u8* pose = FreshPose(chr, why);
    const bool fresh_animation = pose != nullptr;
    if (!pose && std::strcmp(why, "sync state") == 0)
        pose = RefreshHeldPose(s, chr, now);
    if (pose == nullptr) {
        if (!s.grab_eye.own_throw) {
            s.grab_eye = {}; // (a throw's last sample stays: GrabCamera::Fresh ages it out)
        }
        // The original skipped the copy: the locals are last frame's (ours where we wrote).
        ++s.stale;
        s.stale_why = why;
        std::array<float, 12> rows{};
        B::ChrFrame frame{};
        if (ReadWorld(chr, rows)) {
            B::ChrFrameFromRows(rows, frame);
        }
        s.eyes.NoteCopy(frame, now);
        RestoreGame(s, chr);
        PublishStatus(s, now, s.solved && s.out.ok && DrivenNow(now));
        CheckBudget(s, B::SteadyNowNs() - now, now);
        FinishCall(s, now);
        return;
    }
    s.grab_eye = {};
    if (!ValidateSkeleton(s, pose)) {
        return;
    }
    s.fresh += fresh_animation ? 1 : 0;
    if (LogLevel() > 0 && !s.map_checked) {
        s.map_checked = true;
        CheckBoneMap(chr);
    }
    std::array<float, 12> rows{};
    s.frame = {};
    if (ReadWorld(chr, rows)) {
        B::ChrFrameFromRows(rows, s.frame);
        // W as it is drawn: the game applies the heading in his physics module (the facing's turn,
        // the game's own) to W after this copy, so the arms are placed in that frame (MockXR
        // 2026-10-18: that heading is W's at render to 0.000 deg over every turning frame; W read
        // here was up to 5 deg behind: the hands carried into each turn, then settling).
        float phys = 0.0f;
        if (s.frame.valid && PhysicsYawNow(phys)) {
            const float d = std::remainder(phys - B::ChrYaw(s.frame), 2.0f * 3.14159265f);
            if (std::abs(d) > 1e-5f && std::abs(d) < 0.6f) {
                const float c = std::cos(d), sn = std::sin(d);
                std::array<float, 12> turned = rows;
                for (int k = 0; k < 3; ++k) {
                    turned[k] = c * rows[k] + sn * rows[8 + k];
                    turned[8 + k] = -sn * rows[k] + c * rows[8 + k];
                }
                B::ChrFrame drawn{};
                if (B::ChrFrameFromRows(turned, drawn) &&
                    std::abs(std::remainder(B::ChrYaw(drawn) - phys, 2.0f * 3.14159265f)) < 1e-3f) {
                    s.frame = drawn;
                    ++s.drawn_heading;
                }
            }
        }
    }
    UpdateWeapon(s, chr);
    // Read native throw entry before predicting the eye/root for this pose.
    const B::GateSignals signals = ReadGate(s, chr);
    Gather(s, now, signals.visceral_freehand && signals.throw_valid &&
                   (signals.throw_state & 1) != 0 && signals.hp_valid && signals.hp > 0);
    // Who owns the arms: stepped on every fresh copy (driven or not), so the ring's ids and the
    // holds stay current.
    if (s.new_count > 0) {
        s.posture_anim_id = s.new_anims[s.new_count - 1].id;
    }
    const auto walk_class = B::ClassifyAnim(s.posture_anim_id);
    // (Not the move-cancel bit: the game raises it only in a clip's cancelable tail, never while
    // standing idle, so it kept the walk from ever starting from a stand; the classes are the
    // check: an idle or walking clip, no event, throw or ladder.)
    s.roomscale_idle = signals.ring_valid && signals.cancel_valid &&
                      signals.hp_valid && signals.hp > 0 && signals.event_valid &&
                      !signals.event && signals.throw_valid && signals.throw_state == 0 &&
                      signals.ladder_valid && signals.ladder == -1 &&
                      (walk_class == B::AnimClass::Idle || walk_class == B::AnimClass::Locomotion);
    s.roomscale_idle_why =
        s.roomscale_idle                                  ? ""
        : !signals.ring_valid                             ? "the animation ring is not read"
        : !signals.cancel_valid                           ? "the cancel windows are not read"
        : !signals.hp_valid || signals.hp <= 0            ? "dead or HP unread"
        : !signals.event_valid || signals.event           ? "an event animation"
        : !signals.throw_valid || signals.throw_state != 0 ? "a throw"
        : !signals.ladder_valid || signals.ladder != -1   ? "a ladder"
                                                          : "the animation is not idle or walking";
    s.gate_out = s.gate.Step(signals, now);
    // Keep the player's last standing pose during the Doll's known kneel sequence.
    // The guest animation/event still runs to completion; only this main-player
    // render pose changes. NPCs, root motion, and the level-up script stay untouched.
    const bool was_levelup_pose = s.levelup_pose;
    s.levelup_pose = signals.event_valid && signals.event &&
        s.gate_out.reason == B::GateReason::Event &&
        (VR::Cinematic::IsLevelUpKneel(s.gate_out.owner_id) ||
         VR::Cinematic::IsLevelUpKneel(s.posture_anim_id)) && s.standing_player == chr;
    if (s.levelup_pose != was_levelup_pose) {
        LOG_INFO(Core, "VR body: Doll level-up standing pose {} (clip {})",
                 s.levelup_pose ? "on" : "off", s.posture_anim_id);
    }
    if (s.levelup_pose) {
        s.gate_out.ik = {true, true};
        s.gate_out.torso = true;
        s.gate_out.attack_ok = {false, false};
        s.gate_out.kept = true;
    }
    if (s.gate_out.throw_released) {
        LOG_INFO(Core,
                 "VR arm: the grab's / visceral's cancel window: your arms back (swings, shots, "
                 "items) while its throw state plays on (cinematic_release)");
    }
    LogNewAnims(s, s.gate_out);
    if (AnimLog() && signals.cancel_valid &&
        (signals.allow != s.last_allow || signals.move_ok != s.last_move)) {
        s.last_allow = signals.allow;
        s.last_move = signals.move_ok;
        const B::GateOut& g = s.gate_out;
        LOG_INFO(Core,
                 "VR anim: cancel allow {:#x} (R1/R2 {} L2 {}) move {} | held {} {} {} ms | arms "
                 "R {} L {}{}{}{}",
                 signals.allow, (signals.allow & B::AllowRightAttack) != 0 ? 1 : 0,
                 (signals.allow & B::AllowLeftAttack) != 0 ? 1 : 0, signals.move_ok ? 1 : 0,
                 g.held ? B::AnimClassName(g.owner) : "-", g.owner_id, g.owner_age_ns / Ms,
                 Owner(g.ik[C::Right]), Owner(g.ik[C::Left]),
                 g.switching ? " | weapon switch: swings and shots wait" : "",
                 g.kept ? fmt::format(" | kept: attack R {} L {}",
                                      g.attack_ok[C::Right] ? "ok" : "waits",
                                      g.attack_ok[C::Left] ? "ok" : "waits")
                        : std::string{},
                 g.released != nullptr ? fmt::format(" (released: {})", g.released)
                                       : std::string{});
    }
    if (LogLevel() > 0 && s.gate_out.changed) {
        LogGateEdge(s.gate_out);
    }
    auto* const local = Read<B::HkQsTransform*>(pose + OffsetPoseLocal);
    // RefreshHeldPose also restores verified, untouched animation locals. Sampling those
    // keeps a throttled animation attached to its current character transform.
    const bool victim =
        VR::GetSettings().vr_grab_attacks &&
        VR::GrabCamera::Victim(signals.throw_valid, signals.throw_state) &&
        (s.gate_out.reason == B::GateReason::Throw || s.gate_out.reason == B::GateReason::Dead);
    // A freehand throw the hunter performs (a visceral, riposte, backstab): the camera holds
    // your place (GrabCamera::ThrowCamera) until the throw state clears (past the cancel window
    // too), or another clip takes over (a dodge or an attack out of the cancel window).
    const bool throwing = signals.visceral_freehand && signals.throw_valid &&
                          (signals.throw_state & 1) != 0 &&
                          s.gate_out.reason != B::GateReason::Dead;
    if (!throwing) {
        s.throw_seen = false;
        s.throw_over = false;
    } else if (s.gate_out.reason == B::GateReason::Throw) {
        s.throw_clip = s.gate_out.owner_id; // the throw's clip, while the throw holds the arms
    } else if (s.throw_seen && s.gate_out.owner_id != s.throw_clip) {
        s.throw_seen = false;
        s.throw_over = true; // (not again until this throw is over)
    }
    if (s.frame.valid && throwing && !s.throw_over) {
        // The model faces -Z. The victim's root: read as the hunter's own (its ChrCtrl must
        // point back to it; the rows must be a rotation).
        s.grab_eye = {{}, now, true, true};
        s.grab_eye.facing = B::RotateToWorld(s.frame, B::Vec3{0.0f, 0.0f, -1.0f});
        std::array<float, 12> rows{};
        B::ChrFrame victim_frame{};
        if (const u8* const target = VisceralPullVictim(chr);
            target != nullptr && ReadWorld(target, rows) &&
            B::ChrFrameFromRows(rows, victim_frame)) {
            s.grab_eye.victim = victim_frame.t;
            s.grab_eye.victim_valid = true;
        }
        s.throw_seen = true;
    } else if (s.frame.valid && victim) {
        if (const auto* skeleton = C::Active()) {
            std::array<B::QsTransform, C::BoneCount> untouched{};
            for (size_t i = 0; i < untouched.size(); ++i) {
                untouched[i] = B::FromHavok(local[i]);
            }
            if (const auto head = B::ModelOf(untouched.data(), C::Parents.data(),
                                            C::BoneCount, C::Head)) {
                const auto& e = skeleton->eye_center_on_head;
                const auto eye = VR::GrabCamera::Eye(s.frame, *head, {e[0], e[1], e[2]});
                s.grab_eye = {eye, now, B::IsFinite(eye)};
            }
        }
    }
    // Decide the artificial camera and whole-body placement before IK. The
    // renderer consumes this exact decision; there is no second clearance cast.
    s.throw_placement = {};
    if (s.throw_player != chr) {
        s.throw_controller.Reset();
        s.throw_player = chr;
    }
    if (VisceralStageEnabled()) {
        s.throw_controller.Reset();
        s.anchor.rig_offset = {};
        s.anchor.rig_held = false;
        if (s.inactive == nullptr &&
            VisceralStagePlacement(now, s.anchor.world_scale, s.throw_placement)) {
            s.anchor.eye_world = s.throw_placement.base + s.anchor.head_offset;
            s.anchor.eye_rel = s.anchor.eye_world - (s.frame.t + s.drift);
            s.anchor.rig_offset = s.throw_placement.root - (s.frame.t + s.drift);
            s.anchor.rig_held = true;
            s.anchor.camera_dip = s.throw_placement.base.y - s.throw_placement.root.y -
                VR::GetSettings().fp_eye_height_m * s.anchor.world_scale;
        }
    } else if (s.inactive != nullptr || !VR::GetSettings().dodge_camera) {
        s.throw_controller.Reset();
        s.anchor.rig_offset = {};
        s.anchor.rig_held = false;
    } else {
        VR::Combat::VisceralHold receipt{};
        (void)VR::Combat::VisceralHoldChannel().Load(receipt);
        auto policy = VR::GrabCamera::SelectSample(s.grab_eye, receipt,
                                                  signals.visceral_freehand, now);
        // Native ThrowState may clear before the finishing animation. The camera
        // receipt still owns that tail, and its retained victim still needs clearance.
        if (policy.valid && policy.own_throw && !policy.victim_valid) {
            policy.facing = B::RotateToWorld(s.frame, B::Vec3{0, 0, -1});
            std::array<float, 12> victim_rows{};
            B::ChrFrame victim_frame{};
            if (const u8* target = VisceralPullVictim(chr);
                target && ReadWorld(target, victim_rows) && B::ChrFrameFromRows(victim_rows, victim_frame)) {
                policy.victim = victim_frame.t;
                policy.victim_valid = true;
            }
        }
        if (policy.valid && !policy.own_throw) {
            s.throw_controller.Reset(); // incoming grabs use their animated eye
            s.anchor.rig_offset = {};
            s.anchor.rig_held = false;
        } else {
            const auto scale = s.anchor.world_scale;
            const bool advance = !B::ReuseHands(s.anchor) || s.anchor_age_ns > 250'000'000;
            s.throw_placement = s.throw_controller.Step(
                s.frame.t + s.drift, s.anchor.chr_t, s.anchor.unheld_base,
                s.anchor.head_offset, policy, scale, advance, now,
                [&](B::Vec3 eye) {
                    return CastVisceralSurface(VisceralPullVictim(chr), eye,
                                               policy.victim, policy.facing, scale);
                });
            if (s.throw_placement.valid) {
                s.anchor.eye_world = s.throw_placement.base + s.anchor.head_offset;
                s.anchor.eye_rel = s.anchor.eye_world - (s.frame.t + s.drift);
                s.anchor.rig_offset = s.throw_placement.root - (s.frame.t + s.drift);
                s.anchor.rig_held = s.throw_placement.holding;
                s.anchor.camera_dip = s.throw_placement.base.y - s.throw_placement.root.y -
                    VR::GetSettings().fp_eye_height_m * scale;
            }
        }
    }
    if (const auto* skeleton=C::Active()) {
        // Before IK overrides: the physics capsule stays flat, but Master hops.
        PublishJumpLift(B::FromHavok(local[C::Master]).t.y-
                        skeleton->local[C::Master].t[1],now);
    }
    if (!RecordPath().empty()) {
        RecordInput(s, local);
    }
    SolveBody(s, chr, pose, local, now);
    PublishStatus(s, now, s.solved && s.out.ok);
    CheckBudget(s, B::SteadyNowNs() - now, now);
    if (!RecordPath().empty()) {
        Record(s, rows, now);
    }
    if (LogLevel() > 0) {
        std::scoped_lock lock{g_snapshot_mutex};
        g_snapshot.ns = now;
        g_snapshot.chr = chr;
        g_snapshot.w = rows;
        std::memcpy(g_snapshot.locals.data(), local, sizeof(g_snapshot.locals));
        g_snapshot.wrote = s.solved && s.out.ok && !DryRun();
        for (size_t i = 0; i < 2; ++i) {
            g_snapshot.arm_weight[i] = s.solved ? s.arm_weight[i] : 0.0f;
            g_snapshot.arm_solved[i] =
                s.solved && s.out.ok && s.out.arm[i].status == B::ArmStatus::Solved;
            g_snapshot.target[i] = s.target[i];
        }
        g_snapshot.right_family = B::Weapons::FamilyOf(s.weapon_id);
        g_snapshot.left_family = B::Weapons::FamilyOf(s.left_weapon_id);
    }
    if (LogLevel() >= 2) {
        if (const u32 marks = B::MarkCount(); marks != s.marks_seen) {
            s.marks_seen = marks;
            s.detail_until_ns = now + DetailNs;
            LOG_INFO(Core, "VR body: MARK {}: one line per frame for 2 s", marks);
        }
        if (now < s.detail_until_ns) {
            std::string state = "active";
            if (s.inactive != nullptr) {
                state = s.inactive;
            } else {
                state = BodyText(s) + " | " + ArmText(s, C::Right) + " | " + ArmText(s, C::Left);
            }
            LOG_INFO(Core, "VR body: frame {} | hands age {} ms{} | gate {} | {}", s.anchor.record,
                     s.hands_age_ns / Ms, s.reused ? " reused" : "",
                     B::GateReasonName(s.gate_out.reason), state);
        }
    }
    FinishCall(s, now);
}

// Slot path: the wrapper's post call for every PlayerIns pose copy (rdi = the character). The
// held vial (core/vr/bloodborne_held.h) ticks here for the main player too, also while the body
// is disabled (with no arm gate then).
void PS4_SYSV_ABI OnPoseWritten(u8* chr) {
    if (chr == nullptr) {
        return;
    }
    u8* const world = Read<u8*>(g_base + WorldChrManRva);
    if (!Plausible(world) || Read<u8*>(world + OffsetMainPlayer) != chr) {
        g_other_calls.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (g_busy.test_and_set(std::memory_order_acquire)) {
        g_busy_calls.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool body = !g_disabled.load(std::memory_order_relaxed);
    if (body) {
        MainPoseWritten(chr);
    }
    const B::GateOut& g = Pose().gate_out;
    HeldPoseTick(chr, {body, g.reason, g.owner, g.held});
    // The gun after the held item (a vial stepping aside for a pull is seen this update): its
    // shot, flash and status (core/vr/bloodborne_gun.h).
    const PoseSide& s = Pose();
    // A weapon switch keeps both arms yours, but the game takes no shot or swing until it takes
    // R1 again (AnimGate's swing lock): the gun and the melee see the arm as the game's.
    // A kept dodge with dodge_attack takes the swing and the shot at once (GateOut::dodge_attack).
    GunPoseTick(chr, {body, body && s.inactive == nullptr && !DryRun(),
                      g.ik[C::Left] && (g.attack_ok[C::Left] || g.dodge_attack),
                      body ? s.arm_weight[C::Left] : 0.0f, g.reason,
                      body && g.dodge_attack && g.ik[C::Left]});
    // Physical melee: the swing's attack on the IK blade (core/vr/bloodborne_melee.h).
    MeleePoseTick(chr, {body, body && s.inactive == nullptr && !DryRun(),
                        g.ik[C::Right] && g.attack_ok[C::Right],
                        body ? s.arm_weight[C::Right] : 0.0f, g.reason, g.held, g.owner_id,
                        g.kept && g.ik[C::Right] && !g.attack_ok[C::Right],
                        body && g.dodge_attack && g.ik[C::Right],
                        g.ik[C::Left] && g.attack_ok[C::Left],
                        body ? s.arm_weight[C::Left] : 0.0f, s.reused, s.anchor.base_yaw,
                        s.edits_in_pose ? s.locals.data() : nullptr,
                        s.edits_in_pose && s.frame.valid ? &s.frame : nullptr, s.last_solve_ns});
    // The right hand's shots, after the melee update named the weapon and its form.
    BladeShotTick(chr, {body && s.inactive == nullptr && !DryRun(),
                        body && g.ik[C::Right] && g.attack_ok[C::Right] &&
                            s.arm_weight[C::Right] >= 0.5f,
                        g.reason});
    g_busy.clear(std::memory_order_release);
}

// Tail-patch path: after the copy of every character (rdi = the character, rsi = its pose).
void PS4_SYSV_ABI OnPoseWrittenTail(u8* chr, u8* /*pose*/) {
    OnPoseWritten(chr);
}

// ---- Render-camera hook (loop thread) -----------------------------------------------------------

struct RenderSide {
    bool first_logged{false};
    bool anchor_logged{false};
    s64 window_start{0};
    u32 ticks{0};
    u32 anchors{0};
    B::Vec3 eye_m{};
    // W drift between the pose copy and this hook.
    u32 drift_frames{0};
    float drift_yaw_max{0.0f};
    double drift_yaw_sum{0.0};
    float drift_angle_max{0.0f};
    float drift_distance_max{0.0f};
    // FK check against GetDmyPolyMtx (the game's dummies from the locals we left), and the
    // rendered weapon dummy against the arm's target.
    s64 last_proof_ns{0};
    std::array<float, 2> proof_error{-1.0f, -1.0f};    // with W read here
    std::array<float, 2> proof_error_e0{-1.0f, -1.0f}; // with W at the pose copy
    std::array<float, 2> target_error{-1.0f, -1.0f};   // dummy vs target, m
    std::array<float, 2> target_angle{-1.0f, -1.0f};   // dummy vs target, radians
    std::array<bool, 2> axes_rows{};                   // the game's dummy axes are its rows
    std::array<s32, 2> proof_found{};
    std::array<bool, 2> proof_logged{};
    // The weapon follows the hand (body plan S11): the in-hand blade line (dummies x100 -> x110
    // of a right-weapon part, dummy groups 10-13) against R_Weapon -Y (the default grip's blade
    // axis) and against the arm's target, its length (constant across poses), and the left gun's
    // attach dummy against the left target.
    s32 blade_group{-1};
    float blade_length{-1.0f};
    s32 blade_family{-1};          // the weapon the length range and the first line are for
    float blade_length_min{-1.0f}; // since that weapon was drawn
    float blade_length_max{-1.0f};
    float blade_vs_bone{-1.0f};   // radians
    float blade_vs_target{-1.0f}; // radians
    bool blade_logged{false};
    s32 gun_dummy{-1};
    float gun_error{-1.0f}; // m
    // Kept here, off the guest thread's stack.
    Snapshot snapshot{};
    std::array<B::QsTransform, C::BoneCount> proof_locals{};
};

RenderSide& Render() {
    static RenderSide side;
    return side;
}

// A dummy poly's world position (GetDmyPolyMtx), or false when the game has none with that id.
bool DummyAt(u8* chr, s32 id, B::Vec3& out) {
    alignas(16) std::array<float, 8 * 12> m{};
    if (g_dmy_poly(chr, m.data(), &id, 1) < 1) {
        return false;
    }
    out = {m[3], m[7], m[11]};
    return true;
}

// S11: the blade line of the part in the right hand. A part's dummies 100 and 110 (bone "edge":
// the Saw Cleaver's blade segment, 100 by the handle) exist for every right-weapon part that is
// loaded, sheathed or not; the one in hand has one of them within 0.25 m of R_Weapon.
void BladeCheck(RenderSide& r, u8* chr, const B::ChrFrame& render) {
    r.blade_group = -1;
    r.blade_length = r.blade_vs_bone = r.blade_vs_target = -1.0f;
    const auto weapon =
        B::ModelOf(r.proof_locals.data(), C::Parents.data(), C::BoneCount, C::R_Weapon);
    if (!weapon) {
        return;
    }
    const B::Vec3 grip = B::ToWorld(render, weapon->t);
    if (r.snapshot.right_family != r.blade_family) {
        r.blade_family = r.snapshot.right_family;
        r.blade_length_min = r.blade_length_max = -1.0f;
        r.blade_logged = false;
    }
    for (const s32 group : {10, 12, 11, 13}) {
        B::Vec3 root{}, tip{};
        if (!DummyAt(chr, group * 1000 + 100, root) || !DummyAt(chr, group * 1000 + 110, tip) ||
            std::min(B::Length(root - grip), B::Length(tip - grip)) > 0.25f) {
            continue;
        }
        const B::Vec3 line = tip - root;
        if (B::Length(line) < 0.01f) {
            continue;
        }
        const B::Vec3 down{0.0f, -1.0f, 0.0f}; // the default grip's blade axis on R_Weapon
        r.blade_group = group;
        r.blade_length = B::Length(line);
        r.blade_length_min = r.blade_length_min < 0.0f
                                 ? r.blade_length
                                 : std::min(r.blade_length_min, r.blade_length);
        r.blade_length_max = std::max(r.blade_length_max, r.blade_length);
        r.blade_vs_bone = B::Angle(line, B::RotateToWorld(render, B::Rotate(weapon->q, down)));
        const auto& snap = r.snapshot;
        if (snap.wrote && snap.arm_solved[C::Right] && snap.arm_weight[C::Right] >= 0.999f) {
            const B::Vec3 target =
                B::RotateToWorld(render, B::Rotate(snap.target[C::Right].q, down));
            r.blade_vs_target = B::Angle(line, target);
        }
        if (!r.blade_logged) {
            r.blade_logged = true;
            LOG_INFO(Core,
                     "VR body: blade check ({}) dmy {}100 -> {}110: {} -> {} ({:.1f} mm, {:.1f} cm "
                     "from R_Weapon), {:.1f} deg from R_Weapon -Y (the default grip's blade axis; "
                     "about 180 = the dummies run the other way)",
                     B::WeaponName(r.blade_family), group, group, Pos(root), Pos(tip),
                     r.blade_length * 1000.0f, B::Length(root - grip) * 100.0f,
                     r.blade_vs_bone / B::Deg);
        }
        return;
    }
}

// S11: the left gun's attach dummy (its weapon family's left form-A dummy on L_Weapon: 345, 347 or
// 348; 345 when unknown) against where the left target puts it, with the left arm fully on IK.
void GunCheck(RenderSide& r, u8* chr, const B::ChrFrame& render) {
    r.gun_dummy = -1;
    r.gun_error = -1.0f;
    const auto& snap = r.snapshot;
    if (!snap.wrote || !snap.arm_solved[C::Left] || snap.arm_weight[C::Left] < 0.999f ||
        HeldLeftHidden()) { // the held vial hid the gun
        return;
    }
    const C::Skeleton* const sk = C::Active();
    if (sk == nullptr) {
        return;
    }
    const C::DummyPoly* dummy = &sk->left_gun;
    if (const auto* f = B::Weapons::FindFamily(snap.left_family); f != nullptr) {
        for (const C::DummyPoly* d : {&sk->left_gun, &sk->left_gun2, &sk->left_gun3}) {
            if (d->id == f->dmy_left) {
                dummy = d;
            }
        }
    }
    B::Vec3 game{};
    if (!DummyAt(chr, dummy->id, game)) {
        return;
    }
    const B::ModelTarget& t = snap.target[C::Left];
    const B::Vec3 offset{dummy->position[0], dummy->position[1], dummy->position[2]};
    r.gun_dummy = dummy->id;
    r.gun_error = B::Length(B::ToWorld(render, t.p + B::Rotate(t.q, offset)) - game);
}

// The game's dummy 300 (R_Weapon) / 340 (L_Weapon) against host FK of the locals the pose copy
// left (the animation, or the body's write) composed with W: proves the FK, W and the space
// conventions, and with the write that nothing later overwrote the arms (expected under 1 mm).
// With an arm fully on IK, also the dummy against the arm's target: where the weapon is rendered
// against where your controller says it should be.
void ProveFk(RenderSide& r, u8* chr, const B::ChrFrame& e0, const B::ChrFrame& render) {
    const C::Skeleton* const sk = C::Active();
    if (sk == nullptr) {
        return;
    }
    auto& locals = r.proof_locals;
    for (size_t i = 0; i < locals.size(); ++i) {
        locals[i] = B::FromHavok(r.snapshot.locals[i]);
    }
    for (int side = 0; side < 2; ++side) {
        const auto i = static_cast<size_t>(side);
        const C::DummyPoly& dummy =
            side == C::Right ? sk->right_weapon_base : sk->left_weapon_base;
        const auto model = B::ModelOf(locals.data(), C::Parents.data(), C::BoneCount, dummy.bone);
        r.proof_error[i] = r.proof_error_e0[i] = r.target_error[i] = r.target_angle[i] = -1.0f;
        if (!model) {
            continue;
        }
        const B::Vec3 offset{dummy.position[0], dummy.position[1], dummy.position[2]};
        const B::Vec3 forward{dummy.forward[0], dummy.forward[1], dummy.forward[2]};
        const B::Vec3 up{dummy.up[0], dummy.up[1], dummy.up[2]};
        const B::Vec3 m = model->t + B::Rotate(model->q, B::Mul(model->s, offset));
        alignas(16) std::array<float, 8 * 12> out{};
        const s32 id = dummy.id;
        r.proof_found[i] = g_dmy_poly(chr, out.data(), &id, 1);
        if (r.proof_found[i] < 1) {
            if (!r.proof_logged[i]) {
                r.proof_logged[i] = true;
                LOG_INFO(Core, "VR body: FK check dmy {}: GetDmyPolyMtx found {}", dummy.id,
                         r.proof_found[i]);
            }
            continue;
        }
        const B::Vec3 game{out[3], out[7], out[11]};
        r.proof_error[i] = B::Length(B::ToWorld(render, m) - game);
        r.proof_error_e0[i] = e0.valid ? B::Length(B::ToWorld(e0, m) - game) : -1.0f;
        // The dummy's forward and up from the game's 3x4: its columns 2 and 1, or its rows
        // (whichever matches host FK; the first line logs both).
        const B::Vec3 fk_forward = B::RotateToWorld(render, B::Rotate(model->q, forward));
        const B::Vec3 fk_up = B::RotateToWorld(render, B::Rotate(model->q, up));
        const B::Vec3 col_forward{out[2], out[6], out[10]}, col_up{out[1], out[5], out[9]};
        const B::Vec3 row_forward{out[8], out[9], out[10]}, row_up{out[4], out[5], out[6]};
        const float col_err = std::max(B::Angle(fk_forward, col_forward), B::Angle(fk_up, col_up));
        const float row_err = std::max(B::Angle(fk_forward, row_forward), B::Angle(fk_up, row_up));
        r.axes_rows[i] = row_err < col_err;
        const B::Vec3 game_forward = r.axes_rows[i] ? row_forward : col_forward;
        const B::Vec3 game_up = r.axes_rows[i] ? row_up : col_up;
        if (r.snapshot.wrote && r.snapshot.arm_solved[i] && r.snapshot.arm_weight[i] >= 0.999f) {
            const B::ModelTarget& t = r.snapshot.target[i];
            const B::Vec3 target = t.p + B::Rotate(t.q, offset);
            r.target_error[i] = B::Length(B::ToWorld(render, target) - game);
            r.target_angle[i] =
                std::max(B::Angle(B::RotateToWorld(render, B::Rotate(t.q, forward)), game_forward),
                         B::Angle(B::RotateToWorld(render, B::Rotate(t.q, up)), game_up));
        }
        if (!r.proof_logged[i]) {
            r.proof_logged[i] = true;
            LOG_INFO(Core,
                     "VR body: FK check dmy {} on bone {}: found {}, game rows ({:.4f} {:.4f} "
                     "{:.4f} {:.4f}) ({:.4f} {:.4f} {:.4f} {:.4f}) ({:.4f} {:.4f} {:.4f} {:.4f}) "
                     "next ({:.4f} {:.4f} {:.4f} {:.4f}) | FK pos {} fwd {} up {} | error {:.2f} "
                     "mm (W at the copy {:.2f} mm), axes {:.2f} deg as columns, {:.2f} as rows",
                     dummy.id, static_cast<int>(dummy.bone), r.proof_found[i], out[0], out[1],
                     out[2], out[3], out[4], out[5], out[6], out[7], out[8], out[9], out[10],
                     out[11], out[12], out[13], out[14], out[15], Pos(B::ToWorld(render, m)),
                     Pos(fk_forward), Pos(fk_up), r.proof_error[i] * 1000.0f,
                     r.proof_error_e0[i] * 1000.0f, col_err / B::Deg, row_err / B::Deg);
        }
    }
    BladeCheck(r, chr, render);
    GunCheck(r, chr, render);
}

// Log mode: W drift between the pose copy and now, and (every 0.5 s) the FK and target checks.
void Measure(RenderSide& r, u8* chr, const B::ChrFrame& now_frame, s64 now) {
    std::array<float, 12> snapshot_w{};
    const bool proof_due = g_dmy_poly != nullptr && now - r.last_proof_ns >= WindowNs;
    {
        std::scoped_lock lock{g_snapshot_mutex};
        if (g_snapshot.chr != chr || now - g_snapshot.ns > SnapshotMaxAgeNs) {
            return; // no pose copy of this character in this frame
        }
        snapshot_w = g_snapshot.w;
        if (proof_due) {
            r.snapshot = g_snapshot;
        }
    }
    B::ChrFrame e0{};
    if (B::ChrFrameFromRows(snapshot_w, e0)) {
        const B::FrameDrift drift = B::Drift(e0, now_frame);
        ++r.drift_frames;
        r.drift_yaw_max = std::max(r.drift_yaw_max, std::abs(drift.yaw));
        r.drift_yaw_sum += std::abs(drift.yaw);
        r.drift_angle_max = std::max(r.drift_angle_max, drift.angle);
        r.drift_distance_max = std::max(r.drift_distance_max, drift.distance);
    }
    if (proof_due) {
        r.last_proof_ns = now;
        ProveFk(r, chr, e0, now_frame);
    }
}

void LogRender(RenderSide& r, s64 now) {
    const auto error = [](float metres) {
        return metres < 0.0f ? std::string{"-"} : fmt::format("{:.2f}", metres * 1000.0f);
    };
    const auto target = [&r](int side) {
        const auto i = static_cast<size_t>(side);
        if (r.target_error[i] < 0.0f) {
            return std::string{"-"};
        }
        return fmt::format("{:.1f} mm {:.1f} deg", r.target_error[i] * 1000.0f,
                           r.target_angle[i] / B::Deg);
    };
    const auto angle = [](float radians) {
        return radians < 0.0f ? std::string{"-"} : fmt::format("{:.1f}", radians / B::Deg);
    };
    std::string blade = "blade -";
    if (r.blade_group >= 0) {
        blade = fmt::format("blade grp {} {:.1f} mm ({:.1f}..{:.1f}) vs R_Weapon -Y {} deg vs "
                            "target {} deg",
                            r.blade_group, r.blade_length * 1000.0f, r.blade_length_min * 1000.0f,
                            r.blade_length_max * 1000.0f, angle(r.blade_vs_bone),
                            angle(r.blade_vs_target));
    }
    LOG_INFO(Core,
             "VR body: view anchors {}/{} eye_m {} | W drift (copy->render) yaw max {:.2f} avg "
             "{:.3f} deg rot max {:.2f} deg pos max {:.1f} cm ({} frames) | FK vs GetDmyPolyMtx "
             "dmy300 {} mm (W at copy {}) dmy340 {} mm (W at copy {}) | dummy vs target R {} L "
             "{} | {} | gun dmy {} {} mm",
             r.anchors, r.ticks, Pos(r.eye_m), r.drift_yaw_max / B::Deg,
             r.drift_frames != 0 ? r.drift_yaw_sum / static_cast<double>(r.drift_frames) / B::Deg
                                 : 0.0,
             r.drift_angle_max / B::Deg, r.drift_distance_max * 100.0f, r.drift_frames,
             error(r.proof_error[C::Right]), error(r.proof_error_e0[C::Right]),
             error(r.proof_error[C::Left]), error(r.proof_error_e0[C::Left]), target(C::Right),
             target(C::Left), blade, r.gun_dummy, error(r.gun_error));
    r.window_start = now;
    r.ticks = r.anchors = r.drift_frames = 0;
    r.drift_yaw_max = r.drift_angle_max = r.drift_distance_max = 0.0f;
    r.drift_yaw_sum = 0.0;
}

// ---- Install ------------------------------------------------------------------------------------

// The hunter's skeleton for everything that needs it, the body on or off (the gun's barrel, the
// vial's arm zone, the wrist gauge): from the main player's live pose (his behaviour graph
// running), as soon as he exists (core/vr/bloodborne_skeleton.cpp; once).
void ReadSkeleton() {
    if (*SkeletonFailure() != '\0') {
        return;
    }
    u8* const chr = MainPlayer();
    if (chr == nullptr) {
        return;
    }
    u8* const mods = Read<u8*>(chr + OffsetChrModules);
    u8* const mod = Plausible(mods) ? Read<u8*>(mods + OffsetBehaviorModule) : nullptr;
    u8* const wrapper = Plausible(mod) ? Read<u8*>(mod + OffsetModWrapper) : nullptr;
    if (!Plausible(wrapper) || Read<u8>(mod + OffsetModActive) == 0 ||
        Read<u64>(wrapper + OffsetWrapperGraph) == 0) {
        return;
    }
    const char* why = "";
    u8* const pose = PoseNode(chr, why);
    if (pose != nullptr) {
        EnsureSkeleton(Read<u8*>(pose + OffsetPoseSkeleton));
    }
}

// The settings allow a body (logged when they do not).
bool BodyWanted() {
    if (!B::GetBodySettings().body) {
        LOG_INFO(Core, "VR body: off (vr.ini body=0)");
        return false;
    }
    if (!VR::IsFirstPerson()) {
        LOG_INFO(Core, "VR body: off (third person: vr.ini first_person=0)");
        return false;
    }
    return true;
}

void CheckDmyPoly(u8* module) {
    if (std::memcmp(module + DmyPolyRva, DmyPolySig.data(), DmyPolySig.size()) == 0) {
        g_dmy_poly = reinterpret_cast<DmyPolyFn>(module + DmyPolyRva);
    } else if (LogLevel() > 0) {
        LOG_WARNING(Core, "VR body: GetDmyPolyMtx signature MISMATCH (no FK check); live bytes {}",
                    Hook::HexBytes(module + DmyPolyRva, DmyPolySig.size()));
    }
}

// The engine code behind the animation gate's signals (the same checks as the cinematic
// detection's). A mismatch turns that signal off for the session; without the ring the arms stay
// yours except for the signals left.
void CheckGateCode(u8* module) {
    const auto matches = [module](u64 rva, const auto& signature) {
        return std::memcmp(module + rva, signature.data(), signature.size()) == 0;
    };
    g_gate_ring = matches(L::TimeActPushRva, L::TimeActPushSig);
    g_gate_event = matches(L::Act8Rva, L::Act8Sig);
    g_gate_throw = matches(L::IsInThrowRva, L::IsInThrowSig);
    g_gate_ladder = matches(L::Act26Rva, L::Act26Sig);
    g_gate_cancel = matches(L::JumpTableRva, L::JumpTableSig) &&
                    matches(L::JumpTable4Rva, L::JumpTable4Sig) &&
                    matches(L::JumpTable11Rva, L::JumpTable11Sig) &&
                    matches(L::JumpTable16Rva, L::JumpTable16Sig);
    const auto verdict = [](bool ok) { return ok ? "ok" : "MISMATCH"; };
    LOG_INFO(Core,
             "VR body: animation gate taPush@1A28173={} act8@1A11557={} isInThrow@1A61550={} "
             "act26@1A11A67={} cancel windows@1A2173A={} (arm_anim_gate={} arm_keep_on_hit={} "
             "arm_keep_on_dodge={})",
             verdict(g_gate_ring), verdict(g_gate_event), verdict(g_gate_throw),
             verdict(g_gate_ladder), verdict(g_gate_cancel),
             B::GetBodySettings().arm_anim_gate ? 1 : 0,
             B::GetBodySettings().arm_keep_on_hit ? 1 : 0,
             B::GetBodySettings().arm_keep_on_dodge ? 1 : 0);
    const auto dump = [module](const char* name, u64 rva, size_t length) {
        LOG_WARNING(Core, "VR body: {} live bytes at {:#x}: {}", name, rva,
                    Hook::HexBytes(module + rva, length));
    };
    if (!g_gate_ring) {
        dump("TimeActPush", L::TimeActPushRva, L::TimeActPushSig.size());
    }
    if (!g_gate_event) {
        dump("act8", L::Act8Rva, L::Act8Sig.size());
    }
    if (!g_gate_throw) {
        dump("IsInThrow", L::IsInThrowRva, L::IsInThrowSig.size());
    }
    if (!g_gate_ladder) {
        dump("act26", L::Act26Rva, L::Act26Sig.size());
    }
    if (!g_gate_cancel) {
        dump("JumpTable", L::JumpTableRva, L::JumpTableSig.size());
        dump("JumpTable 4", L::JumpTable4Rva, L::JumpTable4Sig.size());
        dump("JumpTable 11", L::JumpTable11Rva, L::JumpTable11Sig.size());
        dump("JumpTable 16", L::JumpTable16Rva, L::JumpTable16Sig.size());
    }
}

// blade_sounds: the game's PlaySound and its sound type table (both compared, never patched).
// PlaySound's first 16 bytes may already be a jump: the bow's release-sound listener and
// VR_SOUND_TRACE detour them before the body installs (the 2026-10-13 run: no blade sounds), and
// a call through the detour still plays the sound; so only the bytes after them are compared.
constexpr size_t PlaySoundDetoured = 16;
void CheckSoundCode(u8* module) {
    // The game's recordings the VR mixers play (the grind, the Beast Cutter's close): decoded from
    // the player's files now, whatever the sound code below (they need no hook).
    VR::GameSounds::LoadAsync();
    const bool ok =
        std::memcmp(module + PlaySoundRva + PlaySoundDetoured,
                    PlaySoundSig.data() + PlaySoundDetoured,
                    PlaySoundSig.size() - PlaySoundDetoured) == 0 &&
        std::memcmp(module + SoundTypeRva, SoundTypeSig.data(), SoundTypeSig.size()) == 0;
    if (!ok) {
        LOG_WARNING(Core, "VR body: PlaySound / sound type signature MISMATCH: no blade sounds");
        return;
    }
    g_play_sound = reinterpret_cast<PlaySoundFn>(module + PlaySoundRva);
    s32 disp{};
    std::memcpy(&disp, module + SoundTypeRva + 3, sizeof(disp));
    g_sound_table = module + SoundTypeRva + 7 + disp;
    const auto& set = VR::BladeSound::GetSettings();
    LOG_INFO(Core, "VR body: PlaySound ok (blade_sounds={}, blade_grind={})", set.on ? 1 : 0,
             set.grind);
}

// PlayerIns vt+0x388 holds the equip data layout the weapon id is read with; without it every
// weapon uses the global grip and the grip adjust is refused.
void CheckWeaponCode(u8* module) {
    g_weapon_ok = std::memcmp(module + WeaponIdRva, WeaponIdSig.data(), WeaponIdSig.size()) == 0;
    if (g_weapon_ok) {
        LOG_INFO(Core, "VR body: weapon id code@18FB090=ok (per-weapon grips, grip adjust={})",
                 B::GetBodySettings().grip_adjust ? 1 : 0);
    } else {
        LOG_WARNING(Core,
                    "VR body: weapon id code@18FB090 MISMATCH (live bytes {}); every weapon uses "
                    "the weapon_grip_pitch_deg/_yaw_deg/_roll_deg grip, no grip adjust",
                    Hook::HexBytes(module + WeaponIdRva, WeaponIdSig.size()));
    }
}

void LogInstalled(const char* how) {
    const auto& b = B::GetBodySettings();
    LOG_INFO(
        Core,
        "VR body: pose hook installed ({}, {}); mode={} arm_ik={} pair_sync={} torso_follow={} "
        "({}/{} deg, {} ms) body_align={} ({}, vertical deadband {} m, duck lean {} deg, shoulder "
        "reach {} deg, {} m, back {} m, down {} m, {} ms) natural_arms={} (forearm {}/{} deg, "
        "twist {} deg) blend in/out "
        "{}/{} ms "
        "anim_gate={} grip trim pitch/yaw/roll={}/{}/{} blade_pitch={} grip_adjust={} "
        "body_haptics={} budget={} us | {}{}{}",
        MethodName(Method()), how, B::BodyModeName(b.mode), b.arm_ik ? 1 : 0,
        b.arm_pair_sync ? 1 : 0, b.torso_follow ? 1 : 0, b.torso_deadzone_deg, b.torso_max_deg,
        b.torso_follow_ms, b.body_align ? 1 : 0, b.body_posture_lean ? "lean" : "shift",
        b.body_vertical_deadband_m, b.body_duck_lean_deg, b.body_shoulder_reach_deg,
        b.body_align_max_m, b.body_back_m, b.body_back_down_m,
        b.body_align_ms, b.body_natural_arms ? 1 : 0, b.body_forearm_pronation_deg,
        b.body_forearm_supination_deg, b.body_wrist_twist_max_deg, b.arm_blend_in_ms,
        b.arm_blend_out_ms, b.arm_anim_gate ? 1 : 0,
        b.weapon_grip_pitch_deg, b.weapon_grip_yaw_deg, b.weapon_grip_roll_deg,
        VR::GetSettings().blade_pitch_deg, b.grip_adjust ? 1 : 0, b.body_haptics ? 1 : 0,
        b.body_budget_us,
        DryRun() ? "DRY RUN: the body is solved every frame, nothing written"
                 : "the body is written into the hunter's pose",
        LogLevel() > 0 ? fmt::format(" log={}", LogLevel()) : std::string{},
        RecordPath().empty() ? std::string{} : " record=" + RecordPath());
}

void InstallSlots(u8* module, u64 size, const char* how) {
    if (g_tried) {
        return;
    }
    g_tried = true;
    if (Method() == HookMethod::Off) {
        LOG_INFO(Core, "VR body: VR_BODY_HOOK=off, no body hook");
        return;
    }
    if (Method() != HookMethod::Slot || !BodyWanted()) {
        return;
    }
    if (size < PoseCopySlotRvas[1] + sizeof(u64) || size < WorldChrManRva + sizeof(u64)) {
        LOG_INFO(Core, "VR body: eboot too small ({:#x}) for Bloodborne 1.09; no body", size);
        return;
    }
    if (std::memcmp(module + PoseCopyRva, PoseCopySig.data(), PoseCopySig.size()) != 0) {
        LOG_WARNING(Core, "VR body: pose copy signature MISMATCH at {:#x} ({}); no body",
                    PoseCopyRva, Hook::HexBytes(module + PoseCopyRva, PoseCopySig.size()));
        return;
    }
    const u64 original = reinterpret_cast<u64>(module + PoseCopyRva);
    for (const u64 rva : PoseCopySlotRvas) {
        const u64 found = Read<u64>(module + rva);
        if (found != original) {
            LOG_WARNING(Core,
                        "VR body: vtable slot {:#x} holds {} (expected +{:#x}, i.e. not relocated "
                        "yet or another build); no body",
                        rva, Rva(reinterpret_cast<const u8*>(found)), PoseCopyRva);
            return;
        }
    }
    CheckDmyPoly(module);
    CheckGateCode(module);
    CheckWeaponCode(module);
    CheckSoundCode(module);
    void* stub = nullptr;
    try {
        g_code = std::make_unique<Xbyak::CodeGenerator>(256);
        stub =
            Hook::EmitPostCallWrapper(*g_code, original, reinterpret_cast<void*>(&OnPoseWritten));
    } catch (const Xbyak::Error& e) {
        LOG_ERROR(Core, "VR body: failed to generate the pose-copy wrapper ({}); no body",
                  e.what());
        return;
    }
    const u64 replacement = reinterpret_cast<u64>(stub);
    for (size_t i = 0; i < PoseCopySlotRvas.size(); ++i) {
        u64 found = 0;
        if (!Hook::SwapVtableSlot(module + PoseCopySlotRvas[i], original, replacement, found)) {
            for (size_t k = 0; k < i; ++k) {
                u64 ignored = 0;
                Hook::SwapVtableSlot(module + PoseCopySlotRvas[k], replacement, original, ignored);
            }
            LOG_WARNING(Core, "VR body: could not swap vtable slot {:#x} (holds {}); no body",
                        PoseCopySlotRvas[i], Rva(reinterpret_cast<const u8*>(found)));
            return;
        }
        LOG_INFO(Core, "VR body: vtable slot {:#x} +{:#x} -> wrapper {}", PoseCopySlotRvas[i],
                 PoseCopyRva, fmt::ptr(stub));
    }
    g_installed.store(true);
    LogInstalled(how);
}

void InstallTailPatch(u8* module, u64 size) {
    g_tried = true;
    if (!BodyWanted()) {
        return;
    }
    if (size < WorldChrManRva + sizeof(u64)) {
        LOG_INFO(Core, "VR body: eboot too small ({:#x}) for Bloodborne 1.09; no body", size);
        return;
    }
    u8* const site = module + PoseTailRva;
    // Live bytes: the static red-zone patcher ran before this.
    if (std::memcmp(module + PoseCopyRva, PoseCopySig.data(), PoseCopySig.size()) != 0 ||
        std::memcmp(site, PoseTailSig.data(), PoseTailSig.size()) != 0) {
        LOG_WARNING(Core, "VR body: pose copy tail signature MISMATCH ({} | {}); no body",
                    Hook::HexBytes(module + PoseCopyRva, PoseCopySig.size()),
                    Hook::HexBytes(site, PoseTailSig.size()));
        return;
    }
    if (!Hook::MakeWritable(site, PoseTailSig.size())) {
        LOG_ERROR(Core, "VR body: could not make the pose copy writable; no body");
        return;
    }
    CheckDmyPoly(module);
    CheckGateCode(module);
    CheckWeaponCode(module);
    CheckSoundCode(module);
    try {
        using namespace Xbyak::util;
        g_code = std::make_unique<Xbyak::CodeGenerator>(256);
        auto& c = *g_code;
        c.align(16);
        void* const stub = const_cast<u8*>(c.getCurr());
        // The replaced instructions, the copy as a call instead of a tail jump (rsp is 16-byte
        // aligned here), then the host function, then the original epilogue.
        c.add(r14, 0x38);
        c.mov(rdi, ptr[rbx + 0x10]);
        c.mov(rsi, r14);
        c.mov(rax, reinterpret_cast<u64>(module + SetPoseRva));
        c.call(rax);
        c.mov(rdi, ptr[rbx + 0x08]);
        c.mov(rsi, r14);
        c.mov(rax, reinterpret_cast<u64>(&OnPoseWrittenTail));
        c.call(rax);
        c.pop(rbx);
        c.pop(r14);
        c.pop(rbp);
        c.ret();
        Hook::WriteJump(site, PoseTailSig.size(), stub);
    } catch (const Xbyak::Error& e) {
        LOG_ERROR(Core, "VR body: failed to generate the tail patch ({}); no body", e.what());
        return;
    }
    g_installed.store(true);
    LogInstalled("from OnEbootLoaded");
}

} // Anonymous namespace

void BodyEbootLoaded(VAddr base, u64 size) {
    if (!VR::IsEnabled() || g_base != nullptr || base == 0) {
        return;
    }
    g_base = reinterpret_cast<u8*>(base);
    g_size = size;
    if (Method() == HookMethod::Code) {
        InstallTailPatch(g_base, size);
    }
}

void OnEbootRelocated(VAddr base, u64 size) {
    g_relocated = true;
    if (!VR::IsEnabled() || base == 0) {
        return;
    }
    if (g_base == nullptr) {
        g_base = reinterpret_cast<u8*>(base);
        g_size = size;
    }
    InstallSlots(reinterpret_cast<u8*>(base), size, "after relocation");
}

bool PoseHookInstalled() {
    return g_installed.load(std::memory_order_relaxed);
}

bool BodyGrabEye(s64 now, VR::GrabCamera::Sample& sample) {
    B::BodyStatus status{};
    (void)B::LatestBodyStatus(status);
    VR::Combat::VisceralHold hold{};
    const bool freehand = VisceralPullFreehand();
    if (freehand)
        (void)VR::Combat::VisceralHoldChannel().Load(hold);
    sample = VR::GrabCamera::SelectSample(status.grab_eye, hold, freehand, now);
    if (!sample.valid || (!sample.own_throw && !VR::GetSettings().vr_grab_attacks))
        return false;
    if (sample.own_throw) {
        // These are character transforms, not animated bones. Refresh them even when the
        // held stab has no new pose; its old eye sample must not end the camera hold.
        sample.victim_valid = false;
        sample.victim_character = nullptr;
        const u8* player = MainPlayer();
        std::array<float, 12> rows{};
        B::ChrFrame frame{};
        if (player != nullptr && ReadWorld(player, rows) && B::ChrFrameFromRows(rows, frame))
            sample.facing = B::RotateToWorld(frame, B::Vec3{0.0f, 0.0f, -1.0f});
        if (const u8* target = player != nullptr ? VisceralPullVictim(player) : nullptr;
            target != nullptr && ReadWorld(target, rows) && B::ChrFrameFromRows(rows, frame)) {
            sample.victim = frame.t;
            sample.victim_valid = true;
            sample.victim_character = target;
        }
    }
    return true;
}

bool BodyThrowPlacement(s64 now, float scale, VR::GrabCamera::ThrowPlacement& out) {
    B::BodyStatus status{};
    if (!B::LatestBodyStatus(status) || !status.driven) return false;
    out = status.throw_placement;
    return out.valid && out.scale == scale && now >= out.ns && now - out.ns <= 250'000'000;
}

bool BodyRootHeight(float& height, float* yaw) {
    if (g_base == nullptr || !PoseHookInstalled() || g_disabled.load(std::memory_order_relaxed)) {
        return false;
    }
    const u8* chr = MainPlayer();
    std::array<float, 12> rows{};
    B::ChrFrame frame{};
    if (chr == nullptr || !ReadWorld(chr, rows) || !B::ChrFrameFromRows(rows, frame)) {
        return false;
    }
    height = frame.t.y;
    if (yaw != nullptr) {
        *yaw = B::ChrYaw(frame);
    }
    return true;
}

void BodyRenderTick(const VR::CameraOverride* result, const float* position, bool fp_follow) {
    static VR::GrabCamera::ThrowBodyRig throw_rig;
    if (g_base == nullptr) {
        return;
    }
    if (C::Active() == nullptr) {
        ReadSkeleton();
    }
    if (!g_relocated && !g_tried) {
        // No post-relocation call came (Linker::Execute without the VR call): install here, on
        // the loop thread that dispatches the pose copy, so it cannot race with that dispatch.
        InstallSlots(g_base, g_size, "lazily from the render-camera hook");
    }
    if (!g_installed.load(std::memory_order_relaxed) ||
        g_disabled.load(std::memory_order_relaxed)) {
        return;
    }
    auto& r = Render();
    const s64 now = B::SteadyNowNs();
    if (!r.first_logged) {
        r.first_logged = true;
        LOG_INFO(Core,
                 "VR body: render-camera hook on thread '{}' (the pose copy must run on the same "
                 "one); character update jobs byte +{:#x} = {} (1: site A runs on job workers)",
                 Common::GetCurrentThreadName(), ChrUpdateJobsRva,
                 Read<u8>(g_base + ChrUpdateJobsRva));
    }
    u8* const chr = MainPlayer();
    std::array<float, 12> rows{};
    B::ChrFrame frame{};
    const bool w_ok = chr != nullptr && ReadWorld(chr, rows) && B::ChrFrameFromRows(rows, frame);
    B::Anchor anchor{};
    anchor.steady_ns = now;
    if (result != nullptr && position != nullptr && fp_follow && result->active && w_ok) {
        const B::Vec3 camera{position[0], position[1], position[2]};
        const B::Vec3 eye_offset{result->eye_offset.x, result->eye_offset.y, result->eye_offset.z};
        anchor.valid = true;
        anchor.record = result->record_id;
        anchor.eye = result->eye;
        anchor.aer = result->aer;
        // Stereo pairs (this tick used the live mode: it only switches between pairs). The second
        // eye's camera keeps this tick's position: the comfort camera when the next pose copy
        // comes within 1.5 game frames (pair_lock_position; the copy decides it and the camera
        // follows that decision), the old camera always (bloodborne_camera.cpp). DIBR and mono
        // start a pair every tick.
        const VR::StereoConfig stereo = VR::GetStereo();
        const auto& vs = VR::GetSettings();
        anchor.pair_first = result->pair_first;
        anchor.paired = stereo.stereo && (stereo.mode == 0 || stereo.mode == 1);
        anchor.lock_ns =
            !anchor.paired     ? 0
            : !vs.dodge_camera ? std::numeric_limits<s64>::max()
            : vs.pair_lock_position
                ? static_cast<s64>(1.5 * static_cast<double>(VR::GameFrameSeconds()) * 1e9)
                : 0;
        anchor.eye_world = camera - eye_offset;
        anchor.chr_t = frame.t;
        anchor.eye_rel = anchor.eye_world - frame.t;
        Common::SyncLog::Line("camw rec=%llu t=(%.4f %.4f %.4f) yaw=%.5f eye=(%.4f %.4f %.4f) "
                              "base_yaw=%.5f",
                              static_cast<unsigned long long>(result->record_id), frame.t.x,
                              frame.t.y, frame.t.z, B::ChrYaw(frame), anchor.eye_world.x,
                              anchor.eye_world.y, anchor.eye_world.z, result->base_yaw);
        const float forward = (vs.fp_forward_m + VR::FirstPersonAheadM(result->view_pitch)) *
                              result->world_scale;
        const B::Vec3 camera_offset{result->throw_offset.x, result->throw_offset.y,
                                    result->throw_offset.z};
        const B::Vec3 unheld_base{
            position[0] - result->render_offset.x - std::sin(result->view_yaw) * forward - camera_offset.x,
            position[1] - result->render_offset.y - camera_offset.y,
            position[2] - result->render_offset.z - std::cos(result->view_yaw) * forward - camera_offset.z};
        anchor.unheld_base = unheld_base;
        anchor.head_offset = {result->render_offset.x - eye_offset.x + std::sin(result->view_yaw) * forward,
                              result->render_offset.y - eye_offset.y,
                              result->render_offset.z - eye_offset.z + std::cos(result->view_yaw) * forward};
        if (result->throw_shared) {
            anchor.rig_offset = B::Vec3{result->throw_root.x, result->throw_root.y, result->throw_root.z} - frame.t;
            throw_rig.Reset();
        } else {
            anchor.rig_offset = throw_rig.Step(frame.t, unheld_base, camera_offset,
                                               result->throw_weight, result->world_scale, now);
        }
        anchor.rig_held = result->throw_holding;
        // The renderer's test for his own parts' draws (vk_rasterizer_vr_draws.cpp).
        VR::SetRecordPlayerPosition(result->record_id, {frame.t.x, frame.t.y, frame.t.z});
        anchor.head_k = {result->head_position.x, result->head_position.y, result->head_position.z};
        anchor.base_yaw = result->base_yaw;
        anchor.view_yaw = result->view_yaw;
        anchor.view_pitch = result->view_pitch;
        anchor.world_scale = result->world_scale;
        // The camera's base: the position minus your head's offset and the stereo eye
        // (render_offset) and the complete rig displacement, already applied to
        // Master. Only ordinary terrain/smoothing remains in the posture's dip.
        anchor.camera_dip = (position[1] - result->render_offset.y - anchor.rig_offset.y) -
                            (frame.t.y + vs.fp_eye_height_m * result->world_scale);
        if (result->world_scale > 0.0f) {
            // The late warp's floor under him (late_warp_motion, LateBodyShapes).
            VR::SetRecordFloorDip(result->record_id, anchor.camera_dip / result->world_scale);
        }
        ++r.anchors;
        r.eye_m = B::RotateToModel(frame, anchor.eye_rel);
        if (!r.anchor_logged) {
            r.anchor_logged = true;
            const auto& s = VR::GetSettings();
            LOG_INFO(Core,
                     "VR body: first anchor eye_m {} (expected about (0, {:.2f}, {:.2f}) plus your "
                     "head offset) chr {} yaw {:.1f} base_yaw {:.1f} eye {} aer {}",
                     Pos(r.eye_m),
                     VR::PlayerBody::StandingEyeM(VR::HeightEyeOffsetM(), s.fp_eye_height_m),
                     -s.fp_forward_m, Pos(frame.t),
                     B::ChrYaw(frame) / B::Deg, result->base_yaw / B::Deg, result->eye,
                     result->aer ? 1 : 0);
        }
    }
    if (!anchor.valid) throw_rig.Reset();
    B::PublishAnchor(anchor);
    ++r.ticks;
    if (LogLevel() == 0) {
        return;
    }
    if (w_ok) {
        Measure(r, chr, frame, now);
    }
    if (r.window_start == 0) {
        r.window_start = now;
    } else if (now - r.window_start >= WindowNs) {
        LogRender(r, now);
    }
}

bool ReadGripLabPose(GripLabPose& out) {
    u8* const chr = MainPlayer();
    if (!chr || !ReadWorld(chr, out.world)) return false;
    const char* why = "";
    const u8* pose = PoseNode(chr, why);
    if (!pose || Read<u8>(pose + OffsetPoseSync) == 0 ||
        Read<s32>(pose + 0x20) != C::BoneCount) return false;
    const auto* local = Read<const B::HkQsTransform*>(pose + OffsetPoseLocal);
    const auto* model = Read<const B::HkQsTransform*>(pose + 0x18);
    if (!Plausible(reinterpret_cast<const u8*>(local), 16) ||
        !Plausible(reinterpret_cast<const u8*>(model), 16)) return false;
    const auto copy = [](const B::HkQsTransform* from, auto& to) {
        for (int i = 0; i < C::BoneCount; ++i) {
            const auto x = B::FromHavok(from[i]);
            to[i] = {x.t.x, x.t.y, x.t.z, x.q.x, x.q.y, x.q.z, x.q.w,
                     x.s.x, x.s.y, x.s.z};
        }
    };
    copy(local, out.local);
    copy(model, out.model);
    return true;
}

} // namespace VR::Bloodborne
