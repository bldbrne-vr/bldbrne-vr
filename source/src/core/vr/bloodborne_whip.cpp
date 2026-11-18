// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <miniz.h>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "common/scope_exit.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/file_sys/fs.h"
#include "core/vr/amygdalan_arm.h"
#include "core/vr/amygdalan_hook.h"
#include "core/vr/arm_ik.h"
#include "core/vr/bb_weapons.h"
#include "core/vr/bloodborne_layout.h"
#include "core/vr/bloodborne_melee.h"
#include "core/vr/bloodborne_ragdoll.h"
#include "core/vr/bloodborne_whip.h"
#include "core/vr/body_channel.h"
#include "core/vr/bow.h"
#include "core/vr/bow_channel.h"
#include "core/vr/c0000_skeleton.h"
#include "core/vr/combat_channel.h"
#include "core/vr/cutter_path.h"
#include "core/vr/hook_util.h"
#include "core/vr/kos_lash.h"
#include "core/vr/kos_grip.h"
#include "core/vr/melee_window.h"
#include "core/vr/seqlock.h"
#include "core/vr/tpf_patch.h"
#include "core/vr/vr_session.h"
#include "core/vr/whip_audio.h"
#include "core/vr/whip_chain.h"
#include "core/vr/whip_flick.h"
#include "core/vr/whip_idle.h"
#include "core/vr/whip_input.h"
#include "core/vr/whip_moves.h"
#include "core/vr/whip_settings.h"
#include "core/vr/whip_spin.h"

namespace VR::Bloodborne {

namespace {

namespace B = VR::Body;
namespace W = VR::Whip;
namespace M = VR::Melee;
namespace L = Layout;
using Hook::Plausible;
using Hook::Read;

// Module-relative addresses in the 1.09 eboot (patch file address - 0x400000).

// The weapon parts' animation update, `void(WeaponPart*, s32 id, bool force_default, f32 time, f32
// dt)` (every weapon part of every character, from PlayerIns vt+0x118 and the ChrAsm update). Its
// tail, after the part's clip was sampled into the hkaPose locals (accessUnsyncedPoseLocalSpace
// 0x6C5970 flagged every bone's model dirty): rbx = the pose node ([[part+0x158]]->player+0x20),
// `add rbx, 0x38; mov rdi, rbx; add rsp, 0x18; pop rbx r12 r13 r14 r15 rbp; jmp syncModelSpace`.
// The 26 bytes from 0x1CA73B5 are the detour's window (its first byte is the only branch target in
// it: `jz` / `jnz` 0x1CA73B5 from the dirty checks; the other exit is 0x1CA73CF).
constexpr u64 PartAnimRva = 0x1CA6E30;
constexpr std::array<u8, 48> PartAnimSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC,
    0x18, 0xC5, 0xFA, 0x11, 0x45, 0xD4, 0x41, 0x89, 0xF7, 0x48, 0x8B, 0x87, 0x58, 0x01, 0x00, 0x00,
    0x48, 0x85, 0xC0, 0x0F, 0x84, 0x76, 0x05, 0x00, 0x00, 0x4C, 0x8B, 0x20, 0x4D, 0x85, 0xE4, 0x0F,
};
constexpr u64 TailCtxRva = 0x1CA738F; // `mov rax, [r12]; mov rbx, [rax+0x20]; ...` up to the end
constexpr std::array<u8, 64> TailCtxSig = {
    0x49, 0x8B, 0x04, 0x24, 0x48, 0x8B, 0x58, 0x20, 0xF6, 0x43, 0x0C, 0x01, 0x74, 0x18, 0xB9, 0x01,
    0x00, 0x00, 0x00, 0x31, 0xC0, 0xF0, 0x0F, 0xB1, 0x4B, 0x10, 0x85, 0xC0, 0x75, 0x08, 0x48, 0x89,
    0xDF, 0xE8, 0x4B, 0x51, 0x09, 0x00, 0x48, 0x83, 0xC3, 0x38, 0x48, 0x89, 0xDF, 0x48, 0x83, 0xC4,
    0x18, 0x5B, 0x41, 0x5C, 0x41, 0x5D, 0x41, 0x5E, 0x41, 0x5F, 0x5D, 0xE9, 0x51, 0xDB, 0xA1, 0xFE,
};
constexpr u64 TailRva = 0x1CA73B5;
constexpr size_t TailLen = 26;
constexpr u64 SyncModelRva = 0x6C4F20; // hkaPose::syncModelSpace
constexpr std::array<u8, 24> SyncModelSig = {
    0x80, 0x7F, 0x38, 0x00, 0x0F, 0x85, 0x83, 0x01, 0x00, 0x00, 0x48, 0x8B,
    0x07, 0x44, 0x8B, 0x40, 0x30, 0x45, 0x85, 0xC0, 0x0F, 0x8E, 0x6F, 0x01,
};
// ChrIns::GetDmyPolyMtx, `s32(ChrIns*, Mtx34* out, const s32* id, u32 max)`.
constexpr u64 DmyPolyRva = 0x18C3BF0;
constexpr std::array<u8, 32> DmyPolySig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC,
    0x28, 0x41, 0x89, 0xCF, 0x48, 0x89, 0xD3, 0x48, 0x89, 0x75, 0xB0, 0x4C, 0x8B, 0x35, 0xCE, 0xFF,
};
constexpr u64 WorldChrManRva = 0x553E878;
constexpr u32 OffsetMainPlayer = 0x60;
// The ChrAsm model set [chr+0x4D0]: the weapon part objects at +0x28 + slot * 8 (slots 11-14: the
// right weapon's parts 0-3; part 2 is a whip's form B), vtable 0x539D100; a part on a body dummy
// has +0x138 = 2 and the dummy at +0x13C (WeaponPart::AttachToDmy 0x1CA5760).
constexpr u32 OffsetChrAsm = 0x4D0;
constexpr u32 OffsetAsmParts = 0x28;
constexpr s32 WhipPartSlot = 13;
// The Kos Parasite (its family's variation, core/vr/bb_weapons.h): the game draws its tendrils with
// a hidden weapon's four parts in the right weapon's slots (core/vr/kos_lash.h); form A's tendril
// is part 0 (B_Rarm_Bone_Chain_1-9 on the right hand).
constexpr s32 KosVariation = 101;
constexpr s32 KosFormAPartSlot = 11;
constexpr u64 WeaponPartVtableRva = 0x539D100;
constexpr u32 OffsetPartMode = 0x138;
constexpr u32 OffsetPartDummy = 0x13C;
// The part's animation: [part+0x158] -> [0] = the state 0x1CA6E30 keeps (r12): +0x00 the player
// (r9 / r14: its pose node at +0x20, the node's hkaPose at +0x38), +0x18 the id asked for, +0x1C
// the clip playing (999000000: the part's default, its idle).
constexpr u32 OffsetPartAnim = 0x158;
constexpr u32 OffsetStatePlayer = 0x00;
constexpr u32 OffsetStateAsked = 0x18;
constexpr u32 OffsetStatePlaying = 0x1C;
constexpr u32 OffsetPlayerNode = 0x20;
constexpr u32 OffsetNodePose = 0x38;
constexpr s32 IdleClip = 999000000;
// hkaPose: skeleton, local {data, size}; hkaSkeleton: parents {s16*, size} +0x18, bones {16 bytes,
// hkStringPtr name (bit 0 = owned)} +0x28, count +0x30.
constexpr u32 OffsetPoseSkeleton = 0x00;
constexpr u32 OffsetPoseLocal = 0x08;
constexpr u32 OffsetPoseLocalSize = 0x10;
constexpr u32 OffsetSkelParents = 0x18;
constexpr u32 OffsetSkelBones = 0x28;
constexpr u32 OffsetSkelCount = 0x30;
constexpr u32 OffsetEquip = 0x4B8; // +0x08 the weapon style (1 form A, 3-5 form B)
constexpr u32 OffsetEquipStyle = 0x08;
constexpr u32 OffsetCtrlWorld = 0x90; // ChrCtrl: W, three float4 rows {R row i, t_i}

constexpr s32 GripDummy = 300;
constexpr s32 TipDummy = 12100;
constexpr s32 MidDummy = 12130;
constexpr s32 RootBladeDummy = 12110;
constexpr int MaxBones = 64;

constexpr s64 Ms = 1'000'000;
constexpr s64 WindowFreshNs = 300 * Ms;
// The presenter's swing speed older than this: none (the hand is not tracked, a menu).
constexpr s64 SpeedFreshNs = 150 * Ms;
// A heavy / charged window's reach lasts this long after it closed (its lash's follow-through).
constexpr s64 HeavyHoldNs = 400 * Ms;
// The chain hurts while its tip was at HurtTipSpeed within this; a hit's speed: its fastest in it.
constexpr s64 FastRecentNs = 150 * Ms; // was 100: the lash that just hit the ground still counts
constexpr s64 TipRecentNs = 120 * Ms;
// The attack of a window that closed lives on while the chain is out and fast, this long at most.
constexpr s64 HoldAfterCloseNs = 300 * Ms;
// ... the Beast Cutter's heavy chain: 350 ms (2026-10-15: the tester, a little more forgiving).
constexpr s64 CutterHoldAfterCloseNs = 350 * Ms;
// ... the Amygdalan Arm's lash: its crack lands about as the hand stops (0.3 s into the swing, its
// window closed by then: it never hurt) and it carries on round 0.3 s more; it hurts while out with
// its tip this fast.
constexpr s64 ArmHoldAfterCloseNs = 400 * Ms;
constexpr float ArmHurtTipMps = 8.0f;
constexpr float BlendS = 0.15f; // the chain eases in / out over this

using DmyPolyFn = s32(PS4_SYSV_ABI*)(void* chr, void* out, const s32* ids, u32 max);

// ---- Install state ------------------------------------------------------------------------------

u8* g_base = nullptr;
bool g_checked = false;  // the sites matched
bool g_hooked = false;   // the detour is written (whip=physical)
bool g_physical = false; // whip=physical
DmyPolyFn g_dmy_poly = nullptr;
std::unique_ptr<Xbyak::CodeGenerator> g_code;
// VR_WHIP_LOG: 1 = state changes, each lash, the dummy check; 2 = + every frame of a lash.
int g_log = 0;
bool g_ref = false;      // VR_WHIP_REF: trace the game's own whip clips (the reference)
bool g_untested = false; // VR_WHIP_UNTESTED: the whips the mock has not swung too
// VR_WHIP_LEAD: the hand's frame one game frame ahead. The dummy 300 the hook reads is the last
// frame's: the part is placed with this frame's hand after the hook, so the whip is shown (and
// hits) one frame of hand motion ahead of the chain. Extrapolated, the lead puts the tip dummy
// within 10 cm of the chain in 80-90% of the mock's checks (60% without), but the extrapolated
// root jitters and the chain folded (bends past 90 degrees) in 4% of the frames instead of 0.5%:
// off by default (offline analysis; the clean fix is this frame's hand from the
// body solver).
bool g_lead = false;

// The node the stub compares rbx with (the pose tick writes it; 0 = none).
alignas(8) volatile u64 g_node_cmp = 0;

// ---- The rig (the pose tick finds it, the hook reads it) ---------------------------------------

struct Rig {
    u8* chr{nullptr};
    u8* part{nullptr};
    u8* state{nullptr};
    u8* node{nullptr};
    s32 variation{-1};
    s32 count{0};
    s32 n{0};          // chain bones
    s32 tip_blade{-1}; // the blade on the last chain bone (the tip dummy's)
    s32 mid_blade{-1}; // the blade the mid dummy 12130 hangs on (logs)
    s32 dummy{GripDummy}; // the hand dummy the part is attached to (its model space)
    std::array<s16, MaxBones> parents{};
    std::array<s16, W::MaxPoints> chain{};
    bool drive{false}; // the pose tick wants the chain driven
    bool arm_release{false}; // form A, with every other Arm drive safety gate still satisfied
    W::Frame grip{};
    s64 grip_ns{0}; // current body pose; zero means use the legacy attachment fallback
    B::Vec3 carry_position{};
    float carry_yaw{0.0f}; // tracking-space yaw, not the animated hunter's facing
    bool carry_valid{false};
};
SeqLock<Rig> g_rig;

// What the hook did (the pose tick reads: logs, the window hold).
struct HookOut {
    s64 ns{0};
    float weight{0.0f};
    float s{1.0f};
    bool compact{false}; // trigger-held Cutter; normal blade gates once fully closed
    float out{0.0f};
    float tip_speed{0.0f};
    float max_speed{0.0f};
    float bend{0.0f};
    float reach{0.0f}; // the last chain point from the grip (m)
    s32 variation{-1};
    B::Vec3 grip{}; // world-space hand at this chain pose; per-contact damage falloff
    std::array<float, 3> tip{};
    std::array<float, 3> mid{};
    // The chain's last point in the frames before (0 = this one): which of them the game's dummy
    // matches tells its lag behind the chain (logs).
    std::array<std::array<float, 3>, 4> hist{};
    s32 tip_index{-1};
    s32 mid_index{-1};
    u32 lash{0};
    bool driving{false};
    s64 fast_ns{0};         // the last frame the chain's tip was at HurtTipSpeed (it hurts)
    float tip_recent{0.0f}; // the tip's fastest over the last TipRecentNs (a hit's speed)
};
SeqLock<HookOut> g_out;
// The Amygdalan Arm's lash (the window hold): out (a strike, a jab, a hook or folding back), and
// the last frame its tip went ArmHurtTipMps; `lashing`: out but not holding an enemy (no stamina
// regen then: ArmLashNow); `grip`: the hand (world).
struct ArmOut {
    s64 ns{0};
    bool out{false};
    s64 fast_ns{0};
    bool lashing{false};
    B::Vec3 grip{};
};
SeqLock<ArmOut> g_arm_out;
std::atomic<u64> g_hook_calls{0};

s64 NowNs() {
    return B::SteadyNowNs();
}

u8* MainPlayer() {
    if (g_base == nullptr) {
        return nullptr;
    }
    u8* const world = Read<u8*>(g_base + WorldChrManRva);
    u8* const chr = Plausible(world) ? Read<u8*>(world + OffsetMainPlayer) : nullptr;
    return Plausible(chr) ? chr : nullptr;
}

bool DummyMtx(u8* chr, s32 id, std::array<float, 12>& out) {
    if (g_dmy_poly == nullptr || chr == nullptr) {
        return false;
    }
    alignas(16) std::array<float, 8 * 12> m{};
    if (g_dmy_poly(chr, m.data(), &id, 1) < 1) {
        return false;
    }
    std::copy_n(m.begin(), 12, out.begin());
    return std::isfinite(out[3]) && std::isfinite(out[7]) && std::isfinite(out[11]);
}

bool DummyAt(u8* chr, s32 id, B::Vec3& out) {
    std::array<float, 12> m{};
    if (!DummyMtx(chr, id, m)) {
        return false;
    }
    out = {m[3], m[7], m[11]};
    return true;
}

// The weapon part's frame (the right hand's dummy it is attached to, 300: the part's model space).
bool GripFrame(u8* chr, W::Frame& f, s32 dummy = GripDummy) {
    std::array<float, 12> m{};
    if (!DummyMtx(chr, dummy, m)) {
        return false;
    }
    // Rows of [R | t]: the columns are the dummy's axes.
    const B::Vec3 x = B::Normalize(B::Vec3{m[0], m[4], m[8]});
    const B::Vec3 y = B::Normalize(B::Vec3{m[1], m[5], m[9]});
    const B::Vec3 z = B::Normalize(B::Cross(x, y));
    if (B::LengthSq(x) == 0.0f || B::LengthSq(z) == 0.0f) {
        return false;
    }
    f.q = B::FromBasis(x, B::Cross(z, x), z);
    f.t = {m[3], m[7], m[11]};
    return B::IsFinite(f.t) && B::IsFinite(f.q);
}

std::string BoneName(const u8* bones, s32 i) {
    const u8* const p = Read<const u8*>(bones + 16 * static_cast<size_t>(i));
    const auto name = reinterpret_cast<const u8*>(reinterpret_cast<uintptr_t>(p) & ~uintptr_t{1});
    if (!Plausible(name, 1)) {
        return {};
    }
    std::string s;
    for (int k = 0; k < 48; ++k) {
        const char c = static_cast<char>(name[k]);
        if (c == 0) {
            break;
        }
        s += c;
    }
    return s;
}

s32 Style(const u8* chr) {
    const u8* const equip = Read<u8*>(chr + OffsetEquip);
    return Plausible(equip) ? Read<s32>(equip + OffsetEquipStyle) : -1;
}

// The right weapon's behaviour variation: its EquipParamWeapon id's family (equip data E =
// [chr+0x4B8], the right hand's active slot [E+0x10], its id at [E + 0x64 + 4 * (slot * 2 + 1)];
// PlayerIns vt+0x388's layout) in the weapons table.
s32 Variation(const u8* chr) {
    const u8* const equip = Read<u8*>(chr + OffsetEquip);
    if (!Plausible(equip)) {
        return -1;
    }
    const s32 slot = Read<s32>(equip + 0x10);
    if (slot < 0 || slot > 2) {
        return -1;
    }
    const s32 id = Read<s32>(equip + 0x64 + 4 * (slot * 2 + 1));
    const auto* const f = B::Weapons::FindFamily(B::Weapons::FamilyOf(id));
    return f != nullptr ? f->variation : -1;
}

// ---- The hook (Game:Main, inside the part update) -----------------------------------------------

// The hunter turning more than this in one game frame carries the chain round with him (a snap
// turn is 30 degrees at once; a smooth turn at 120 deg/s is 2 a frame).
constexpr float CarryTurnDeg = 12.0f;

// The Kos Parasite's swing read off your controller (KosSwingFrame, below): its last place, held
// a moment while the runtime loses it.
struct KosSwingHold {
    B::Vec3 rel{};  // the controller from your head (tracking space, m)
    B::Vec3 line{}; // its line (grip -Z)
    s64 ns{0};
    bool have{false};
};
constexpr s64 KosSwingGraceNs = 250 * Ms;

// A snap turn: the view's heading jumps and the hand with it, round your eye; the hunter's facing
// need not (MockXR 2026-11-09: no carry on his turn in four snaps), so the chain was dragged
// behind and sprang back (a review, 2026-11-09). In the frame the hand jumps the tendrils go with
// it rigidly, its last frame with it. The jump can reach the bones a frame or two after the
// heading: a window of SnapWindow frames, the first jump of SnapJumpM in it carried.
class KosSnap {
public:
    static constexpr int SnapWindow = 3;
    static constexpr float SnapJumpM = 0.05f;

    // True when the hand's move from `prev` to `grip` this frame is a snap's.
    bool Step(const W::Frame& prev, const W::Frame& grip) {
        B::Anchor at{};
        if (B::LatestAnchor(at) && at.valid && std::isfinite(at.base_yaw)) {
            if (have_ && std::abs(std::remainder(at.base_yaw - yaw_, 2.0f * B::Pi)) >
                             CarryTurnDeg * B::Deg) {
                window_ = SnapWindow;
            }
            yaw_ = at.base_yaw;
            have_ = true;
        }
        if (window_ <= 0) {
            return false;
        }
        --window_;
        if (!(B::Length(grip.t - prev.t) >= SnapJumpM)) {
            return false;
        }
        window_ = 0;
        ++carries_;
        return true;
    }
    u32 Carries() const {
        return carries_;
    }

private:
    float yaw_{0.0f};
    bool have_{false};
    int window_{0};
    u32 carries_{0};
};

struct HookState {
    W::Chain chain{};
    W::ChainConfig cfg{};
    u8* node{nullptr};
    bool have_rest{false};
    std::array<B::Vec3, W::MaxPoints> rest{};   // the simulation's idle shape (RestToBlade)
    std::array<B::Quat, W::MaxPoints> rest_q{};
    std::array<B::Vec3, W::MaxPoints> rest_raw{}; // ... as the idle clip has it
    std::array<W::Kos::BoneScale, W::MaxPoints> kos_scale{};
    bool flipped{false};                        // turned to the blade side (the cane)
    float links{0.84f}; // the idle chain's length along its links (m)
    W::DriveState drive{}; // driven through a transform's clip (a flick into the whip)
    bool tried_builtin{false};
    s32 playing_logged{-1};
    W::Frame prev{};
    bool have_prev{false};
    W::PoseClock clock{};
    float weight{0.0f};
    // The lash in progress (logs).
    u32 lash{0};
    bool lashing{false};
    u32 lash_window{0};
    float lash_peak_s{1.0f};
    float lash_peak_tip{0.0f};
    float lash_reach{0.0f};
    float lash_bend{0.0f};
    u32 lash_frames{0};
    s64 lash_ns{0};
    s64 last_frame_log_ns{0};
    s64 last_bad_log_ns{0};
    std::array<std::array<float, 3>, 4> hist{};
    W::Frame raw{}; // the hand's frame as read last time (the lead)
    float dt_avg{1.0f / 60.0f}; // the game's frame time, averaged
    bool have_raw{false};
    W::RecallSoundGate recall_sound{};
    W::PullRecall recall{};
    W::CutterStrokeTurn turn{}; // the Beast Cutter's stroke turning back
    float throw_left{0.0f};     // s left of a Beast Cutter throw's front drive
    // The Beast Cutter's momentum length (whip_cutter_lab): the meter, the chain's front half's
    // speed around the hand last frame (and what it is measured from), a lockout after a recall.
    float meter{0.0f};
    float front{0.0f};
    float front_s{1.0f};
    float meter_lock{0.0f};
    float impact_left{0.0f}; // s the meter is held after the chain hit the ground
    float hand_speed{0.0f};  // m/s, the hand's own movement around the hunter (no wrist turn)
    bool have_front{false};
    B::Vec3 front_root{};
    std::array<B::Vec3, W::MaxPoints> front_prev{};
    W::CutterPath path{};  // the Beast Cutter's swing-path lash (whip_cutter_lab model=path)
    W::Kos::Recoil kos_recoil{}; // the Kos Parasite's tendril: its recoil, its sway, its return
    W::Kos::Sway kos_sway{};
    W::Kos::Return kos_return{};
    W::Kos::Pulse kos_pulse{};
    W::CutterLab kos_lab{}; // its dials this frame (the stroke's own return)
    bool kos_tracked{false}; // its path read off the controller (KosSwingFrame) last frame
    KosSwingHold kos_hold{};
    KosSnap kos_snap{};
    bool forced_in{false}; // this frame: the trigger, the flick back or a recall pull (the trace)
    W::WristFlick flick{}; // the Beast Cutter's flick recall (core/vr/whip_flick.h)
    W::Payout payout{}; // the extension the swing's speed asks for
    bool full{false};   // at the full payout (locked out)
    u32 locks{0};       // ... counted (the presenter's tick)
    u32 reels{0};       // reel-ins started (counted)
    s64 fast_ns{0};
    std::array<std::pair<s64, float>, 8> tips{}; // the tip's speed, the last frames
    u32 tip_next{0};
    // The hunter last frame (the chain is carried round with a snap turn).
    B::Vec3 body_pos{};
    B::Vec3 body_fwd{};
    bool have_body{false};
    s64 body_ns{0};
    u32 carries{0};
    u32 teleports{0}; // the hand's frame jumped (TeleportMs): the chain carried with it
    bool compact_logged{false}; // the Cutter's compact state, as last logged
};
HookState g_hs{};

// Opt-in, bounded recording of the ACTUAL game-side input and solver output. Replay this when
// headset feel disagrees with a scripted mock. Never enabled during normal play or benchmarks.
bool HunterFrame(const u8* chr, B::Vec3& pos, B::Vec3& fwd, B::Vec3& left); // (below)

// The recording also carries the head (its eye, forward and up in the world), the hunter (his feet
// and facing) and the chain's points, every frame: the offline replay is seen from the player's
// eye and checked against what the game's chain did.
void TraceMotion(const Rig& rig, const HookState& h, const W::Frame& grip,
                 const W::GazeAssist& gaze, s64 now, float dt, float speed, float target,
                 bool current_pose) {
    static FILE* const file = [] {
        const char* path = std::getenv("VR_WHIP_TRACE");
        FILE* f = path && *path ? std::fopen(path, "w") : nullptr;
        if (f) {
            std::fputs("ns,variation,pose_ns,current_pose,dt,px,py,pz,qx,qy,qz,qw,speed,target,"
                       "extension,floor,tip_x,tip_y,tip_z,tip_speed,bend,carries,gaze,"
                       "eye_x,eye_y,eye_z,forward_x,forward_y,forward_z,compact,"
                       "head_ok,head_x,head_y,head_z,head_fx,head_fy,head_fz,head_ux,head_uy,"
                       "head_uz,body_ok,body_x,body_y,body_z,body_fx,body_fz,chain_n", f);
            for (int i = 0; i < W::MaxPoints; ++i) std::fprintf(f, ",c%d_x,c%d_y,c%d_z", i, i, i);
            std::fputs(",forced\n", f);
            LOG_INFO(Core, "VR whip: recording up to 30000 live motion samples to {}", path);
        }
        return f;
    }();
    static unsigned count = 0;
    if (!file || count >= 30000 || !rig.drive || dt <= 0.0f) return;
    const auto tip = h.chain.Point(h.chain.Count() - 1);
    // The head (as the gaze assist reads it) and the hunter.
    B::HandsSample hands{};
    B::Anchor anchor{};
    B::Vec3 eye{}, fwd{}, up{};
    const bool head_ok = B::LatestHands(hands) && hands.head_valid && B::LatestAnchor(anchor) &&
                         anchor.valid && B::IsFinite(hands.head_q) &&
                         B::Dot(hands.head_q, hands.head_q) > 0.5f;
    if (head_ok) {
        const B::Quat hq = B::Normalize(hands.head_q);
        eye = anchor.eye_world + B::TrackingToWorld(hands.head_p - anchor.head_k, anchor.base_yaw,
                                                    anchor.world_scale);
        fwd = B::TrackingToWorld(B::Rotate(hq, {0.0f, 0.0f, -1.0f}), anchor.base_yaw, 1.0f);
        up = B::TrackingToWorld(B::Rotate(hq, {0.0f, 1.0f, 0.0f}), anchor.base_yaw, 1.0f);
    }
    B::Vec3 feet{}, facing{}, side{};
    const bool body_ok = HunterFrame(rig.chr, feet, facing, side) && B::IsFinite(feet);
    std::fprintf(file,
        "%lld,%d,%lld,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,"
        "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d",
        static_cast<long long>(now), rig.variation, static_cast<long long>(rig.grip_ns),
        int(current_pose), dt, grip.t.x, grip.t.y, grip.t.z, grip.q.x, grip.q.y, grip.q.z,
        grip.q.w, speed, target, h.chain.Extension(), h.cfg.floor_y, tip.x, tip.y, tip.z,
        h.chain.TipSpeed(), h.chain.MaxBend(), h.carries, int(gaze.active), gaze.eye.x,
        gaze.eye.y, gaze.eye.z, gaze.forward.x, gaze.forward.y, gaze.forward.z,
        int(h.cfg.lock_closed));
    std::fprintf(file, ",%d,%.6g,%.6g,%.6g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%d,%.6g,%.6g,%.6g,%.5g,%.5g,%d",
                 int(head_ok), eye.x, eye.y, eye.z, fwd.x, fwd.y, fwd.z, up.x, up.y, up.z,
                 int(body_ok), feet.x, feet.y, feet.z, facing.x, facing.z, h.chain.Count());
    for (int i = 0; i < W::MaxPoints; ++i) {
        const B::Vec3 c = i < h.chain.Count() ? h.chain.Point(i) : B::Vec3{};
        std::fprintf(file, ",%.6g,%.6g,%.6g", c.x, c.y, c.z);
    }
    std::fprintf(file, ",%d\n", int(h.forced_in));
    if (++count % 60 == 0) std::fflush(file);
}

// The idle shape of each whip as last seen (a new part object of the same whip, after a weapon
// switch or a load, drives at once instead of waiting for its idle clip: a flick into the whip as
// the first transform of a session).
struct RestCache {
    s32 variation{-1};
    s32 n{0};
    std::array<B::Vec3, W::MaxPoints> rest{};
    std::array<B::Quat, W::MaxPoints> rest_q{};
    std::array<W::Kos::BoneScale, W::MaxPoints> kos_scale{};
    std::array<B::Vec3, W::MaxPoints> raw{};
    bool flipped{false};
    float links{0.0f};
};
std::array<RestCache, 4> g_rest_cache{};

// The whips' idle lines from the game's own files (core/vr/whip_idle.h: the part's skeleton and
// the first frame of its idle clip a999), read once at install: the rest shape of a whip whose
// part was never seen in its idle clip yet. None: the hook waits for the idle clip.
std::array<std::optional<W::WhipRest>, W::WhipParts.size()> g_idle{};

// A part's form-B anibnd (parts/<model>.partsbnd.dcx -> <model>_2.anibnd), inflated; `anibnd` its
// span in the returned BND.
std::optional<std::vector<u8>> ReadPartBnd(std::string_view model, std::span<const u8>& anibnd,
                                           std::string& why) {
    auto* const mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    const std::string path = fmt::format("/app0/dvdroot_ps4/parts/{}.partsbnd.dcx", model);
    const auto file = mnt->ReadFile(path);
    if (!file) {
        why = path + " not readable";
        return std::nullopt;
    }
    const auto dcx = TpfPatch::ParseDcx(*file);
    if (!dcx || dcx->unpacked > (64u << 20)) {
        why = path + " is not a DCX";
        return std::nullopt;
    }
    std::vector<u8> bnd(dcx->unpacked);
    mz_ulong length = dcx->unpacked;
    if (mz_uncompress(bnd.data(), &length, file->data() + dcx->stream, dcx->packed) != MZ_OK ||
        length != dcx->unpacked) {
        why = path + ": the zlib stream does not inflate";
        return std::nullopt;
    }
    const std::string name = fmt::format("{}_2.anibnd", model);
    const auto inner = VR::Body::C0000::Rig::Bnd4Find(bnd, name);
    if (!inner) {
        why = fmt::format("{} holds no {}", path, name);
        return std::nullopt;
    }
    const auto offset = static_cast<std::size_t>(inner->data() - bnd.data());
    const auto size = inner->size();
    auto out = std::make_optional(std::move(bnd));
    anibnd = std::span<const u8>(out->data() + offset, size);
    return out;
}

std::optional<W::WhipRest> ReadIdle(const W::WhipPart& part, std::string& why) {
    std::span<const u8> anibnd;
    const auto bnd = ReadPartBnd(part.model, anibnd, why);
    if (!bnd) {
        return std::nullopt;
    }
    return W::Idle::FromAnibnd(part.variation, anibnd, why);
}

void LoadIdleLines() {
    const auto started = std::chrono::steady_clock::now();
    std::FILE* dump = nullptr;
    if (const char* path = std::getenv("VR_WHIP_REST_DUMP"); path != nullptr) {
        dump = std::fopen(path, "w");
    }
    for (std::size_t i = 0; i < W::WhipParts.size(); ++i) {
        const W::WhipPart& part = W::WhipParts[i];
        std::string why;
        g_idle[i] = ReadIdle(part, why);
        if (!g_idle[i]) {
            LOG_WARNING(Core,
                        "VR whip: the idle line of variation {} ({}) is not readable from the "
                        "game's files ({}): until its idle clip is seen, that whip's chain stays "
                        "the game's",
                        part.variation, part.model, why);
            continue;
        }
        const W::WhipRest& r = *g_idle[i];
        LOG_INFO(Core,
                 "VR whip: the idle line of variation {} ({}, a999 frame 0) read from the game's "
                 "files: {} chain bones of {}, links {:.4f} m, the last {:.4f} m from the grip",
                 part.variation, part.model, r.n, r.bones, r.links,
                 std::sqrt(r.t[r.n - 1][0] * r.t[r.n - 1][0] + r.t[r.n - 1][1] * r.t[r.n - 1][1] +
                           r.t[r.n - 1][2] * r.t[r.n - 1][2]));
        if (dump != nullptr) {
            // Diagnostics (VR_WHIP_REST_DUMP=<file>): the numbers, for a check against a reference.
            std::fprintf(dump, "whip %d bones %d n %d links %.9g\n", r.variation, r.bones, r.n,
                         static_cast<double>(r.links));
            for (int k = 0; k < r.n; ++k) {
                const auto& t = r.t[static_cast<std::size_t>(k)];
                const auto& q = r.q[static_cast<std::size_t>(k)];
                std::fprintf(dump, "%d %d %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n", r.variation, k,
                             static_cast<double>(t[0]), static_cast<double>(t[1]),
                             static_cast<double>(t[2]), static_cast<double>(q[0]),
                             static_cast<double>(q[1]), static_cast<double>(q[2]),
                             static_cast<double>(q[3]));
            }
        }
    }
    if (dump != nullptr) {
        std::fclose(dump);
    }
    LOG_INFO(Core, "VR whip: idle lines read in {:.0f} ms",
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                 .count());
}

const W::WhipRest* FindIdle(s32 variation) {
    for (std::size_t i = 0; i < W::WhipParts.size(); ++i) {
        if (W::WhipParts[i].variation == variation && g_idle[i]) {
            return &*g_idle[i];
        }
    }
    return nullptr;
}

float MaxExtension(const Rig& r, u8 kind) {
    if (r.variation == KosVariation) {
        // The game's own reach: 1.95 m swung, 2.7 m on a charged swing (core/vr/kos_lash.h), over
        // the tendril's rest length along its links.
        const float reach = kind == 2 ? W::Kos::ChargedReachM : W::Kos::ReachM;
        return std::max(reach * W::GetWhipSettings().reach_scale / std::max(g_hs.links, 0.05f),
                        1.0f);
    }
    const W::WhipMoves* const w = W::FindWhip(r.variation);
    if (w == nullptr) {
        return 1.0f;
    }
    const float scale = W::GetWhipSettings().reach_scale;
    if (r.variation == 2400) {
        // Final grip-to-tip caps in metres at scale 1: light, heavy, charged.
        // These replace the Cutter's stacked reach boosts.
        constexpr std::array<float, 3> reach{4.7f, 6.6f, 7.4f};
        return W::PayoutFor(reach, kind, scale, w->idle_tip, g_hs.links);
    }
    return W::ReachExtension(r.variation, W::PayoutFor(
        w->reach, kind, scale, w->idle_tip, g_hs.links),
        w->idle_tip, g_hs.links);
}

bool HunterFrame(const u8* chr, B::Vec3& pos, B::Vec3& fwd, B::Vec3& left); // (below)
// The chain lies on the ground at the hunter's feet, this far above them (its links' thickness).
constexpr float FloorLiftM = 0.03f;
// Faster than any hand moves or turns (the user's fastest real swings: 25 m/s, 5,400 deg/s): a
// jump of the hand's frame, carried rigidly instead of thrown through. The traces of 2026-11-08
// showed 107 cm / 67 deg in one frame as the drive started on the weapon bone and went over to
// the controller, and 62 cm / 115 deg the frame after a turnaround's carried one.
constexpr float TeleportMs = 40.0f;
constexpr float TeleportDegS = 8000.0f;

// ---- The Kos Parasite's form B: a whip on each hand, three tendrils each ----------------------
// (core/vr/kos_lash.h). With the Milkweed rune the game swaps in the hidden weapon whose parts 2
// (slot 13, on the right hand's dummy 304: B_RarmA, B and C, 9 joints each) and 3 (slot 14, on the
// left's 344: Finger0 and Finger2, 9 joints each, and the short Finger1, 3; their common root
// Finger0_00, the parent of every left tendril, stays the game's) are form B's tendrils. Each
// hand's swing throws its bundle on the Cutter's swing-path lash; each tendril has a mind of its
// own (its lag, curl, side, reach, recoil, sway, return and mood). None of the game's attack
// animations play. The pose tick finds the parts; the stub sends their pose nodes here.

constexpr std::array<s32, 2> KosBundleSlot{13, 14};
constexpr std::array<s32, 2> KosBundleDummy{304, 344};

// The two bundles' pose nodes (right, left) and the mouth tendril's (KosMouthTick) for the stub;
// 0 = none.
alignas(8) volatile u64 g_kos_node_cmp[3] = {0, 0, 0};

struct KosBundleRig {
    u8* chr{nullptr};
    std::array<u8*, 2> node{};
    std::array<u8*, 2> state{};
    std::array<s32, 2> dummy{};
    std::array<s32, 2> count{};
    std::array<std::array<s16, MaxBones>, 2> parents{};
    std::array<std::array<std::array<s16, W::MaxPoints>, 3>, 2> bones{};
    std::array<std::array<s32, 3>, 2> n{};
    bool drive{false};
};
SeqLock<KosBundleRig> g_kos_rig;

// A bundle part's three tendrils by their bones' names, each joint the child of the one before.
bool ResolveBundle(const u8* skel, int hand, KosBundleRig& rig, std::string& why) {
    const s32 count = Read<s32>(skel + OffsetSkelCount);
    const u8* const parents = Read<const u8*>(skel + OffsetSkelParents);
    const u8* const bones = Read<const u8*>(skel + OffsetSkelBones);
    if (count <= 3 || count > MaxBones || !Plausible(parents, 2) || !Plausible(bones)) {
        why = fmt::format("skeleton not readable (count {})", count);
        return false;
    }
    rig.count[hand] = count;
    for (s32 i = 0; i < count; ++i) {
        rig.parents[hand][i] = Read<s16>(parents + 2 * i);
    }
    struct Want {
        const char* prefix;
        int first, last;
        bool pad; // two digits
    };
    static constexpr std::array<Want, 3> right{{{"B_RarmA_Bone_Chain_", 1, 9, false},
                                                {"B_RarmB_Bone_Chain_", 1, 9, false},
                                                {"B_RarmC_Bone_Chain_", 1, 9, false}}};
    static constexpr std::array<Want, 3> left{{{"B_Larm_Finger0_", 1, 9, true},
                                               {"B_Larm_Finger2_", 1, 9, true},
                                               {"B_Larm_Finger1_", 1, 3, true}}};
    const auto& wants = hand == 0 ? right : left;
    // (The game's own names may end in white space: the left part's B_Larm_Finger1_01 has a tab.)
    std::array<std::string, MaxBones> names{};
    for (s32 i = 0; i < count; ++i) {
        names[i] = BoneName(bones, i);
        while (!names[i].empty() && std::isspace(static_cast<unsigned char>(names[i].back()))) {
            names[i].pop_back();
        }
    }
    for (int t = 0; t < 3; ++t) {
        const Want& w = wants[t];
        s32 prev = -1;
        int n = 0;
        for (int k = w.first; k <= w.last; ++k) {
            const std::string name =
                std::string{w.prefix} + (w.pad && k < 10 ? "0" : "") + std::to_string(k);
            s32 found = -1;
            for (s32 i = 0; i < count; ++i) {
                if (names[i] == name) {
                    found = i;
                    break;
                }
            }
            if (found < 0 || (prev >= 0 && rig.parents[hand][found] != prev)) {
                why = fmt::format("no tendril ({} {})", name,
                                  found < 0 ? "missing" : "not under the one before");
                return false;
            }
            rig.bones[hand][t][n++] = static_cast<s16>(found);
            prev = found;
        }
        rig.n[hand][t] = n;
    }
    return true;
}

// ---- The Kos Parasite's mouth tendril: third person only --------------------------------------
// (2026-11-09: "Mouth tendril third person only"). The hidden weapon's part 1 (slot 12,
// on the face's dummy 7) plays in the ritual, form B's leap and its step R2; in first person it
// swept through your view. There its pose collapses to nothing (its root's scale, KosMouthHook).
constexpr s32 KosMouthSlot = 12;
constexpr s32 KosMouthDummy = 7;
constexpr float KosMouthHiddenScale = 1e-3f;

// The main player's pose update: the mouth tendril's pose node for the stub while `hide`.
void KosMouthTick(u8* chr, bool hide) {
    static u64 logged = 1; // (a change logs once)
    u8* node = nullptr;
    u8* const parts = hide && g_hooked ? Read<u8*>(chr + OffsetChrAsm) : nullptr;
    u8* const part =
        Plausible(parts) ? Read<u8*>(parts + OffsetAsmParts + 8 * KosMouthSlot) : nullptr;
    if (Plausible(part) && Read<u8*>(part) == g_base + WeaponPartVtableRva &&
        Read<s32>(part + OffsetPartMode) == 2 &&
        Read<s32>(part + OffsetPartDummy) == KosMouthDummy) {
        u8* const anim = Read<u8*>(part + OffsetPartAnim);
        u8* const state = Plausible(anim) ? Read<u8*>(anim) : nullptr;
        u8* const player = Plausible(state) ? Read<u8*>(state + OffsetStatePlayer) : nullptr;
        u8* const found = Plausible(player) ? Read<u8*>(player + OffsetPlayerNode) : nullptr;
        node = Plausible(found) ? found : nullptr;
    }
    g_kos_node_cmp[2] = reinterpret_cast<u64>(node);
    if ((node != nullptr) != (logged != 0)) {
        logged = node != nullptr ? 1 : 0;
        LOG_INFO(Core, "VR whip: the Kos Parasite's mouth tendril {}",
                 node != nullptr ? "hidden (first person)" : "the game's");
    }
}

// The mouth tendril's pose (Game:Main, inside its animation update): collapsed. True when `node`
// is it.
bool KosMouthHook(u8* node) {
    if (node == nullptr || reinterpret_cast<u64>(node) != g_kos_node_cmp[2]) {
        return false;
    }
    u8* const pose = node + OffsetNodePose;
    auto* const local = Read<B::HkQsTransform*>(pose + OffsetPoseLocal);
    const s32 count = Read<s32>(pose + OffsetPoseLocalSize);
    if (count < 1 || count > MaxBones || !Plausible(reinterpret_cast<u8*>(local), 16)) {
        return true;
    }
    B::QsTransform root = B::FromHavok(local[0]);
    root.s = {KosMouthHiddenScale, KosMouthHiddenScale, KosMouthHiddenScale};
    B::ToHavok(root, local[0]);
    return true;
}

// The main player's pose update: finds form B's two parts and publishes them for the hook.
void KosBundleTick(u8* chr, bool kos_b, bool body_on) {
    static std::array<u8*, 2> resolved{};
    static std::array<u8*, 2> failed{}; // a node whose bones were not found (tried once)
    static KosBundleRig rig{};
    static bool logged_drive = false;
    static const char* logged_why = "";
    const char* why = nullptr;
    std::array<u8*, 2> nodes{};
    std::array<u8*, 2> states{};
    std::array<s32, 2> dummies{};
    u8* const parts = kos_b ? Read<u8*>(chr + OffsetChrAsm) : nullptr;
    if (!kos_b) {
        why = "not the Kos Parasite's form B";
    } else if (!g_hooked || !g_physical) {
        why = "the whip hook is off";
    } else if (!Plausible(parts)) {
        why = "no ChrAsm";
    } else {
        for (int hand = 0; hand < 2 && why == nullptr; ++hand) {
            u8* const part = Read<u8*>(parts + OffsetAsmParts + 8 * KosBundleSlot[hand]);
            if (!Plausible(part) || Read<u8*>(part) != g_base + WeaponPartVtableRva) {
                why = "no tendril part (the Milkweed rune not worn?)";
                break;
            }
            const s32 dummy = Read<s32>(part + OffsetPartDummy);
            if (Read<s32>(part + OffsetPartMode) != 2 || dummy != KosBundleDummy[hand]) {
                why = "a tendril part not on its hand";
                break;
            }
            u8* const anim = Read<u8*>(part + OffsetPartAnim);
            u8* const state = Plausible(anim) ? Read<u8*>(anim) : nullptr;
            u8* const player = Plausible(state) ? Read<u8*>(state + OffsetStatePlayer) : nullptr;
            u8* const node = Plausible(player) ? Read<u8*>(player + OffsetPlayerNode) : nullptr;
            if (!Plausible(node)) {
                why = "a tendril part has no animation";
                break;
            }
            if (node == failed[hand]) {
                why = "a tendril part's bones not found";
                break;
            }
            if (node != resolved[hand]) {
                std::string err;
                const u8* const skel = Read<u8*>(node + OffsetNodePose + OffsetPoseSkeleton);
                if (!Plausible(skel) || !ResolveBundle(skel, hand, rig, err)) {
                    resolved[hand] = nullptr;
                    failed[hand] = node;
                    why = "a tendril part's bones not found";
                    LOG_INFO(Core, "VR whip: Kos form B part {}: {}", hand, err);
                    break;
                }
                resolved[hand] = node;
            }
            nodes[hand] = node;
            states[hand] = state;
            dummies[hand] = dummy;
        }
    }
    const bool drive = why == nullptr && body_on;
    if (why == nullptr && !body_on)
        why = "body off";
    if (nodes[0] != nullptr && nodes[1] != nullptr && why == nullptr) {
        rig.chr = chr;
        rig.node = nodes;
        rig.state = states;
        rig.dummy = dummies;
        rig.drive = drive;
        g_kos_rig.Store(rig);
        g_kos_node_cmp[0] = reinterpret_cast<u64>(nodes[0]);
        g_kos_node_cmp[1] = reinterpret_cast<u64>(nodes[1]);
    } else {
        g_kos_node_cmp[0] = g_kos_node_cmp[1] = 0;
        KosBundleRig off{};
        g_kos_rig.Store(off);
    }
    if (kos_b && (drive != logged_drive || (why != nullptr && logged_why != why))) {
        logged_drive = drive;
        logged_why = why != nullptr ? why : "";
        LOG_INFO(Core, "VR whip: Kos form B: {}",
                 drive ? std::string{"driving both hands' tendrils"}
                       : fmt::format("not driving ({})", why));
    }
}

// The Kos Parasite's swing, read off your own arm (2026-11-09: "when I walk around or
// run, or snap turn - I do not want the tentacle to swing or extend.. only when I swing my
// arms"): the controller and its line (a held stick's, grip -Z), from your head, in the tracking
// space turned to the game's heading; that frame (your eye, the heading) is the path's body
// frame, so his walk, a snap turn and his own turning never read as a swing. (His feet and facing
// did, MockXR 2026-11-09: 10 strokes with one swing; the hunter turns to run sideways while your
// hands stay with the view, and a snap turns them round your eye, not his feet.) A controller the
// runtime only infers (a fast swing behind you) still counts; one lost for a moment holds its last
// place (no swing) for KosSwingGraceNs before the path goes back to the hand bone, since each
// switch restarts it (a review, 2026-11-09: a flapping source wiped strokes mid-swing). False: no
// head or view, or no controller for that long; the path reads the hand bone and his frame.
bool KosSwingFrame(int xr_hand, s64 now, KosSwingHold& hold, W::PathFrame& pf) {
    B::Anchor at{};
    if (!pf.head_ok || !B::LatestAnchor(at) || !at.valid) {
        return false;
    }
    B::HandsSample hs{};
    if (B::LatestHands(hs) && hs.head_valid && now >= hs.steady_ns &&
        now - hs.steady_ns < 100 * Ms && B::IsFinite(hs.head_p) &&
        hs.hand[xr_hand].position_valid && hs.hand[xr_hand].orientation_valid) {
        const B::TrackedHand& c = hs.hand[xr_hand];
        const B::Vec3 rel = c.p - hs.head_p;
        const B::Vec3 line = B::Rotate(B::Normalize(c.q), {0.0f, 0.0f, -1.0f});
        if (B::IsFinite(rel) && B::IsFinite(line)) {
            hold = {rel, line, now, true};
        }
    }
    if (!hold.have || now - hold.ns > KosSwingGraceNs) {
        hold.have = false;
        return false;
    }
    pf.hand = pf.eye + B::TrackingToWorld(hold.rel, at.base_yaw, at.world_scale);
    pf.weapon = B::TrackingToWorld(hold.line, at.base_yaw, 1.0f);
    pf.feet = pf.eye;
    pf.facing = B::TrackingToWorld({0.0f, 0.0f, -1.0f}, at.base_yaw, 1.0f);
    pf.body_ok = true;
    return true;
}

struct KosTendrilState {
    W::Chain chain{};
    std::array<B::Vec3, W::MaxPoints> rest{};
    std::array<B::Quat, W::MaxPoints> rest_q{};
    std::array<W::Kos::BoneScale, W::MaxPoints> scale{};
    float links{0.0f};
    bool have_rest{false};
    W::Kos::Recoil recoil{};
    W::Kos::Sway sway{};
    W::Kos::Pulse pulse{};
    W::Kos::Return back{};
    W::Kos::Wiggle wiggle{};
    float peak_ext{1.0f}; // the last stroke's (logs)
    float peak_reach{0.0f};
    float peak_share{0.0f};
};
struct KosHandState {
    u8* node{nullptr};
    s64 stamp{0};
    float clock{0.0f};
    W::Frame prev{};
    bool have_prev{false};
    float weight{0.0f};
    W::CutterPath path{};
    bool path_tracked{false}; // the path read off the controller (KosSwingFrame) last frame
    KosSwingHold swing_hold{};
    KosSnap snap{};
    W::Kos::Bundle bundle{};
    std::array<KosTendrilState, 3> t{};
    s64 bad_log_ns{0};
};
std::array<KosHandState, 2> g_kos_hands{}; // (the hook's thread only)
W::Kos::GripFit<MaxBones> g_kos_right_fit{}; // bounded pose cache, right bundle only

// One bundle part's pose (Game:Main, inside its animation update). True when `node` is one of them.
bool KosBundleHook(u8* node) {
    KosBundleRig r{};
    if (node == nullptr || !g_kos_rig.Load(r)) {
        return false;
    }
    const int hand = r.node[0] == node ? 0 : r.node[1] == node ? 1 : -1;
    if (hand < 0) {
        return false;
    }
    KosHandState& h = g_kos_hands[hand];
    const s64 now = NowNs();
    const auto& looks = hand == 0 ? W::Kos::RightLooks : W::Kos::LeftLooks;
    if (node != h.node) {
        if (hand == 0) g_kos_right_fit = {};
        h = {};
        h.node = node;
        h.bundle.Seed(static_cast<unsigned>(now) ^ (hand != 0 ? 0x5BD1E995u : 0u));
        for (int i = 0; i < 3; ++i) {
            h.t[i].back.Seed(static_cast<unsigned>(now) +
                             7919u * static_cast<unsigned>(i + 1 + 3 * hand));
            h.t[i].wiggle.Seed(static_cast<unsigned>(now) ^
                               (104729u * static_cast<unsigned>(i + 1 + 3 * hand)));
            h.t[i].wiggle.Stroke();
            h.t[i].recoil.hz = looks[i].hz;
            h.t[i].recoil.amp = looks[i].amp;
        }
    }
    u8* const pose = node + OffsetNodePose;
    auto* const local = Read<B::HkQsTransform*>(pose + OffsetPoseLocal);
    const s32 count = Read<s32>(pose + OffsetPoseLocalSize);
    if (!Plausible(reinterpret_cast<u8*>(local), 16) || count != r.count[hand] ||
        count <= 0 || count > MaxBones) {
        return true;
    }
    const std::span<B::HkQsTransform> fit_pose{local, static_cast<std::size_t>(count)};
    if (hand == 0) g_kos_right_fit.Restore(fit_pose);
    const s32 playing = Read<s32>(r.state[hand] + OffsetStatePlaying);
    const bool idle = playing == IdleClip;
    std::array<B::QsTransform, MaxBones> loc{};
    std::array<B::QsTransform, MaxBones> mod{};
    for (s32 i = 0; i < count; ++i) {
        loc[i] = B::FromHavok(local[i]);
    }
    if (!B::LocalToModel(loc.data(), r.parents[hand].data(), count, mod.data())) {
        return true;
    }
    W::Frame grip{};
    if (!GripFrame(r.chr, grip, r.dummy[hand])) {
        return true;
    }
    // Keep cached rest shapes and chain simulation in their original units. The
    // approved +6% fit is applied once to the finished right pose, including its
    // stationary base and native clips. The left hand and form A are unchanged.
    SCOPE_EXIT {
        if (hand == 0)
            g_kos_right_fit.Apply(fit_pose,
                std::span<const s16>{r.parents[hand].data(), static_cast<std::size_t>(count)});
    };
    // Its rest shapes, from the idle clip (the tendrils as the game holds them at rest).
    bool rest_ok = true;
    for (int i = 0; i < 3; ++i) {
        KosTendrilState& t = h.t[i];
        const s32 n = r.n[hand][i];
        if (!t.have_rest && idle && n >= 3) {
            float links = 0.0f;
            bool scales_ok = true;
            for (s32 k = 0; k < n; ++k) {
                t.rest[k] = mod[r.bones[hand][i][k]].t;
                t.rest_q[k] = B::Normalize(mod[r.bones[hand][i][k]].q);
                if (k > 0)
                    links += B::Length(t.rest[k] - t.rest[k - 1]);
            }
            for (s32 k = 0; k < n; ++k) {
                const s32 j = std::min(k + 1, n - 1);
                scales_ok = t.scale[k].Set(mod[r.bones[hand][i][k]].s,
                    B::Length(t.rest[j] - t.rest[j - 1])) && scales_ok;
            }
            t.links = links;
            t.have_rest = scales_ok && t.chain.Init(t.rest.data(), n);
        }
        rest_ok = rest_ok && t.have_rest;
    }
    const float dt =
        h.stamp != 0 ? std::clamp(static_cast<float>(now - h.stamp) * 1e-9f, 0.0f, 1.0f) : 0.0f;
    h.stamp = now;
    h.clock += dt;
    const bool want = r.drive && idle && rest_ok;
    h.weight = std::clamp(h.weight + (want ? 1.0f : -1.0f) * dt / BlendS, 0.0f, 1.0f);
    if (h.weight <= 0.0f || !rest_ok || dt <= 0.0f) {
        // Not driven: the clip's pose; the chains wait at it.
        for (int i = 0; i < 3 && rest_ok; ++i) {
            std::array<B::Vec3, W::MaxPoints> pts{};
            for (s32 k = 0; k < r.n[hand][i]; ++k) {
                pts[k] = W::Apply(grip, mod[r.bones[hand][i][k]].t);
            }
            h.t[i].chain.ResetTo(pts.data(), 1.0f);
        }
        // ... and the swing starts afresh (across the gap a hand's move read as one frame's swing).
        h.path.Reset();
        h.path_tracked = false;
        h.bundle.Reset();
        for (auto& t : h.t) {
            t.recoil.Reset();
            t.sway.Reset();
            t.pulse.Reset();
            t.back.Reset();
        }
        h.prev = grip;
        h.have_prev = true;
        return true;
    }
    // The hand's swing (the Cutter's path model on this hand): one share for its bundle.
    W::PathFrame pf{};
    pf.dt = dt;
    pf.hand = grip.t;
    const auto& lead = h.t[0];
    pf.weapon =
        B::Rotate(B::Normalize(grip.q), B::Normalize(lead.rest[r.n[hand][0] - 1] - lead.rest[0]));
    pf.impact = lead.chain.Impact();
    pf.cap = 2.0f;
    B::Vec3 feet{}, fwd{}, left{};
    const bool hunter =
        HunterFrame(r.chr, feet, fwd, left) && B::IsFinite(feet) && B::IsFinite(fwd);
    pf.body_ok = hunter;
    pf.feet = feet;
    pf.facing = fwd;
    B::HandsSample head{};
    B::Anchor at{};
    if (B::LatestHands(head) && head.head_valid && B::LatestAnchor(at) && at.valid &&
        B::IsFinite(head.head_q) && B::Dot(head.head_q, head.head_q) > 0.5f) {
        pf.eye =
            at.eye_world + B::TrackingToWorld(head.head_p - at.head_k, at.base_yaw, at.world_scale);
        pf.forward = B::TrackingToWorld(B::Rotate(B::Normalize(head.head_q), {0.0f, 0.0f, -1.0f}),
                                        at.base_yaw, 1.0f);
        pf.head_ok = true;
    }
    // (A change of source restarts the path: one frame across two frames is no swing.)
    const bool tracked = KosSwingFrame(hand == 0 ? 1 : 0, now, h.swing_hold, pf);
    if (tracked != h.path_tracked) {
        h.path.Reset();
        h.path_tracked = tracked;
    }
    W::CutterLab hand_lab = W::Kos::Lab();
    hand_lab.pkeep = 0.5f;
    hand_lab.pturn = W::Kos::BundleHoldS;
    const float share = std::clamp(h.path.Step(pf, hand_lab) - 1.0f, 0.0f, 1.0f);
    h.bundle.Step(dt, share, 0);
    // A heavy swing (the right hand's R2 rows) reaches the game's R2.
    Combat::MeleeWindow w{};
    const bool heavy = hand == 0 && Combat::LatestMeleeWindow(w) && now - w.ns < WindowFreshNs &&
                       w.committed && !w.unpaid && w.kind > 0 &&
                       (w.open || (w.close_ns != 0 && now - w.close_ns < HeavyHoldNs));
    const float floor_y = hunter ? feet.y + FloorLiftM : W::ChainConfig{}.floor_y;
    if (h.have_prev && h.snap.Step(h.prev, grip)) {
        const B::Quat rq =
            B::Normalize(B::Normalize(grip.q) * B::Conjugate(B::Normalize(h.prev.q)));
        for (auto& t : h.t) {
            if (t.have_rest)
                t.chain.Carry(rq, h.prev.t, grip.t);
        }
        if (g_log >= 1) {
            LOG_INFO(Core,
                     "VR whip: Kos form B {}: a snap turn: its tendrils carried round with the "
                     "hand (#{})",
                     hand == 0 ? "right" : "left", h.snap.Carries());
        }
        h.prev = grip;
    }
    for (int i = 0; i < 3; ++i) {
        KosTendrilState& t = h.t[i];
        const auto& look = looks[i];
        const s32 n = r.n[hand][i];
        const float cap =
            std::max((heavy ? look.heavy : look.reach) / std::max(t.links, 0.05f), 1.01f);
        const float out = std::clamp((t.chain.Extension() - 1.0f) / (cap - 1.0f), 0.0f, 1.0f);
        const bool stroke = t.back.Step(out);
        if (stroke)
            t.wiggle.Stroke();
        t.wiggle.Step(dt, look.hz);
        if (stroke && g_log >= 1) {
            LOG_INFO(
                Core,
                "VR whip: Kos form B {} tendril {}: returns in {:.0f} ms, {}, writhing at x{:.2f} "
                "(the last stroke: x{:.2f} of x{:.2f}, {:.2f} m from the hand, the hand's share "
                "{:.2f})",
                hand == 0 ? "right" : "left", i, t.back.Seconds() * 1000.0f,
                W::Kos::MoodName(t.back.Current()),
                t.wiggle.Rate() * look.hz / W::Kos::Wiggle::BaseHz, t.peak_ext, cap, t.peak_reach,
                t.peak_share);
            t.peak_ext = 1.0f;
            t.peak_reach = t.peak_share = 0.0f;
        }
        if (t.chain.Ready()) { // (not its points before its first step)
            t.peak_ext = std::max(t.peak_ext, t.chain.Extension());
            t.peak_reach = std::max(t.peak_reach, B::Length(t.chain.Point(n - 1) - grip.t));
        }
        t.peak_share = std::max(t.peak_share, h.bundle.Share(i));
        W::CutterLab lab = W::Kos::Lab();
        t.back.Apply(lab);
        lab.pkeep = 0.5f;
        lab.pdelay = look.delay * W::Kos::BundleCurlK;
        lab.pcurl *= look.curl;
        lab.tout = h.bundle.PayoutTime(i, 0.12f);
        const bool turned = t.recoil.Step(dt, out);
        t.sway.Step(dt, turned, out);
        t.pulse.Step(dt, turned, out);
        const float s = std::clamp(1.0f + (cap - 1.0f) * h.bundle.Share(i) +
                                       t.recoil.Offset() / std::max(t.links, 0.05f),
                                   1.0f, cap);
        W::ChainConfig cfg = W::ConfigFor(2400);
        W::ApplyLabChain(lab, cfg);
        h.path.Apply(lab, cfg);
        const W::Kos::MoodGain gain = W::Kos::GainOf(t.back.Current());
        cfg.sway = t.sway.Amplitude(out, gain) * t.wiggle.Gain(out);
        cfg.sway_phase = look.phase;
        cfg.sway_clock = t.wiggle.Clock();
        cfg.fan = W::Kos::FanAngle(out, t.recoil.Rising());
        cfg.fan_roll = look.side + 0.6f * std::sin(0.8f * h.clock + look.phase);
        cfg.floor_y = floor_y;
        t.chain.Step(h.have_prev ? h.prev : grip, grip, dt, s, cfg);
        // Its bones: positions and rotations from the chain, scales thickening and pulsing.
        std::array<B::Vec3, W::MaxPoints> p{};
        for (s32 k = 0; k < n; ++k)
            p[k] = W::ApplyInverse(grip, t.chain.Point(k));
        std::array<B::Quat, W::MaxPoints> q{};
        W::ChainRotations(p.data(), t.rest_q.data(), n, q.data());
        std::array<B::QsTransform, W::MaxPoints> clip{};
        std::array<B::Vec3, W::MaxPoints> want_s{}, clip_s{}, local_s{};
        const float girth = W::Kos::Girth(t.chain.Extension());
        for (s32 k = 0; k < n; ++k) {
            const s32 b = r.bones[hand][i][k];
            clip[k] = loc[b];
            const s32 j = std::min(k + 1, n - 1);
            const float g = girth * t.pulse.At(static_cast<float>(k) / (n - 1), gain.pulse);
            want_s[k] = t.scale[k].At(B::Length(p[j] - p[j - 1]), g);
            clip_s[k] = mod[b].s;
        }
        const s32 parent = r.parents[hand][r.bones[hand][i][0]];
        if (parent < 0 || parent >= count)
            continue;
        W::Kos::LocalScales(want_s.data(), clip_s.data(), mod[parent].s, n, h.weight,
                            local_s.data());
        for (s32 k = 0; k < n; ++k)
            clip[k].s = local_s[k];
        std::array<B::QsTransform, W::MaxPoints> outs{};
        W::ChainLocals(mod[parent], p.data(), q.data(), clip.data(), n, h.weight, outs.data());
        // Each link against its own resting length stretched (a tendril's links are uneven: the
        // left's longest failed against the average every few strokes, a flicker).
        const float stretch = std::max(t.chain.Extension(), 1.0f) * 1.5f;
        const char* bad = nullptr;
        for (s32 k = 0; k < n && bad == nullptr; ++k) {
            if (!B::IsFinite(outs[k])) {
                bad = "not finite";
            } else if (k > 0 && B::Length(outs[k].t) >
                                    std::max(B::Length(t.rest[k] - t.rest[k - 1]) * stretch + 0.02f,
                                             B::Length(clip[k].t) * 1.5f + 0.02f)) {
                bad = "a link too long";
            }
        }
        if (bad != nullptr) {
            if (g_log >= 1 && now - h.bad_log_ns > 1000 * Ms) {
                h.bad_log_ns = now;
                LOG_WARNING(Core, "VR whip: Kos form B {} tendril {}: pose not written ({})",
                            hand == 0 ? "right" : "left", i, bad);
            }
            continue;
        }
        for (s32 k = 0; k < n; ++k)
            B::ToHavok(outs[k], local[r.bones[hand][i][k]]);
    }
    h.prev = grip;
    h.have_prev = true;
    return true;
}

void PS4_SYSV_ABI WhipHook(u8* node) {
    MeleeBowPartPose(node); // native bowstring shares this signature-checked pose tail
    g_hook_calls.fetch_add(1, std::memory_order_relaxed);
    if (KosMouthHook(node)) {
        return;
    }
    if (KosBundleHook(node)) {
        return; // the Kos Parasite's form B (its own rig)
    }
    Rig r{};
    if (!g_rig.Load(r) || r.node != node || r.n < 3) {
        return;
    }
    HookState& h = g_hs;
    const s64 now = NowNs();
    u8* const pose = node + OffsetNodePose;
    auto* const local = Read<B::HkQsTransform*>(pose + OffsetPoseLocal);
    const s32 count = Read<s32>(pose + OffsetPoseLocalSize);
    if (!Plausible(reinterpret_cast<u8*>(local), 16) || count != r.count || count > MaxBones) {
        return;
    }
    const s32 playing = Read<s32>(r.state + OffsetStatePlaying);
    const bool idle = playing == IdleClip;
    std::array<B::QsTransform, MaxBones> loc{};
    std::array<B::QsTransform, MaxBones> mod{};
    for (s32 i = 0; i < count; ++i) {
        loc[i] = B::FromHavok(local[i]);
    }
    if (!B::LocalToModel(loc.data(), r.parents.data(), count, mod.data())) {
        return;
    }
    W::Frame grip{};
    const bool pose_grip = r.grip_ns > 0 && now >= r.grip_ns && now - r.grip_ns < 100 * Ms;
    if (pose_grip) {
        grip = r.grip;
    } else if (!GripFrame(r.chr, grip, r.dummy)) {
        return;
    }
    // The Arm's tentacle and bladed tip have fixed lengths. Reuse the checked
    // part-animation tail so skinning and native hit dummies see the same pose.
    static AmygdalanArm::Motion arm;
    static u8* arm_node = nullptr;
    static s64 arm_stamp = 0;
    static s32 arm_clip = -1;
    static std::array<B::QsTransform, 3> arm_bones{}; // the lash stretches links 2 and 3 too
    static bool arm_cached = false, arm_hunter = false;
    static B::Vec3 arm_position{};
    static float arm_yaw{};
    static u8* arm_chr = nullptr;
    if (r.variation == 2500) {
        if (arm_node != node || arm_chr != r.chr) {
            arm.Clear(); // a new part or hunter: its neutral pose again
            arm_node = node;
            arm_chr = r.chr;
            arm_stamp = 0;
            arm_cached = arm_hunter = false;
        }
        const bool closing = playing == AmygdalanArm::ClosingClip;
        if ((!r.drive && !(r.arm_release && closing)) || !pose_grip || count != 4) {
            // A gap: the chain starts again after it, the neutral pose kept (Motion::Pause).
            if (arm_stamp != 0) arm.Pause();
            arm_stamp = 0;
            arm_cached = arm_hunter = false;
            return;
        }
        const s64 stamp = r.grip_ns;
        // A duplicate update must not advance physics or reset it.
        if (stamp == arm_stamp && closing == (arm_clip == AmygdalanArm::ClosingClip) && arm_cached) {
            loc[1] = arm_bones[0];
            loc[2] = arm_bones[1];
            loc[3] = arm_bones[2];
            B::ToHavok(loc[1], local[1]);
            B::ToHavok(loc[2], local[2]);
            B::ToHavok(loc[3], local[3]);
            return;
        }
        const float dt = arm_stamp ? static_cast<float>(stamp - arm_stamp) * 1e-9f : 0.0f;
        arm_stamp = stamp;
        arm_clip = playing;
        // The right trigger held in form B (core/vr/amygdalan_hook.h OwnsTrigger): the tentacle
        // lashes out, taking the arm over from an attack's clip too (AmygdalanArm::Motion).
        const auto intent = W::LatestSwingInput(now, SpeedFreshNs);
        const bool lash = !closing && intent.arm_lash;
        // Ours while out (a strike, a jab, a hook or folding back), a clip started or not.
        // Opening must first reach idle: never capture a transform's pose as the permanent rest.
        const bool ours = !closing && (idle || arm.HasNeutralPose());
        float floor_y = -1e9f;
        if (ours) {
            // These values were captured alongside grip, from the same body pose. Reading the
            // live character matrix here mixed two frames and interpreted walking turns as swings.
            const B::Vec3 position = r.carry_position;
            const bool hunter = r.carry_valid;
            if (hunter && arm_hunter && dt > 0.0f && dt <= 0.1f)
                arm.Carry(B::AxisAngle({0, 1, 0}, std::remainder(r.carry_yaw - arm_yaw, 2 * B::Pi)),
                          arm_position, position);
            arm_hunter = hunter;
            arm_position = position;
            arm_yaw = r.carry_yaw;
            if (hunter) floor_y = position.y + FloorLiftM; // the lash never goes through it
        } else {
            arm_hunter = false; // an attack's clip: the chain restarts from its pose, not carried
        }
        // Latched on (the enemy still on its feet) or caught (limp, pulled): the tentacle held out
        // to it, the tip on it. The hand's turn goes to the caught enemy's lever.
        B::Vec3 hooked{};
        bool on_enemy = false;
        if (AmygdalanHook::Enabled() && B::IsFinite(grip.q))
            AmygdalanHook::hand_turn.Store({now, reinterpret_cast<u64>(r.chr), grip.q});
        if (AmygdalanHook::Enabled() && !closing) {
            const auto fresh = [&](s64 ns, u64 player) {
                return ns > 0 && now >= ns && now - ns <= AmygdalanHook::MaxAgeNs &&
                       player == reinterpret_cast<u64>(r.chr);
            };
            const auto hook = ArmHookNow();
            const auto latch = AmygdalanHook::LatchNow();
            if (hook.attached && fresh(hook.ns, hook.player)) {
                hooked = hook.point;
                on_enemy = true;
            } else if (latch.attached && fresh(latch.ns, latch.player)) {
                hooked = latch.point;
                on_enemy = true;
            }
        }
        std::array<B::QsTransform, 4> clip{}, out{};
        std::copy_n(loc.begin(), 4, clip.begin());
        // Once initialized at idle, controller motion owns form B, including native attack clips.
        // Closing still blends back to the game's actual transformation.
        const bool was_out = arm.Out();
        AmygdalanArm::LaunchInput launch{};
        if (r.carry_valid) {
            launch.direction = B::TrackingToWorld(
                {intent.arm_direction[0], intent.arm_direction[1], intent.arm_direction[2]},
                r.carry_yaw, 1.0f);
            launch.velocity = B::TrackingToWorld(
                {intent.arm_velocity[0], intent.arm_velocity[1], intent.arm_velocity[2]},
                r.carry_yaw, 1.0f);
            // The hunter's facing (the Beast Cutter's throw reads the same): a free throw goes only
            // ahead of him, a wind-up behind keeps the limb coiled (AmygdalanArm::ThrowAhead).
            B::Vec3 feet{}, facing{}, side{};
            if (HunterFrame(r.chr, feet, facing, side) && B::IsFinite(facing))
                launch.heading = facing;
        }
        arm_cached = closing ? arm.Close(clip, grip, dt, out)
                     : ours
                         ? arm.Update(clip, grip, dt, out, lash, floor_y,
                                      on_enemy ? &hooked : nullptr, intent.arm_speed, &launch)
                         : arm.Attack(clip, grip, dt, out);
        if (g_log >= 1 && was_out != arm.Out())
            LOG_INFO(Core, "VR Arm motion: {} (controller {:.2f} m/s, trigger {})",
                     arm.Out() ? "out" : "folded", intent.arm_speed, lash);
        if (arm_cached) {
            if (on_enemy) AmygdalanArm::AimTip(out, W::ApplyInverse(grip, hooked));
            arm_bones = {out[1], out[2], out[3]};
            B::ToHavok(out[1], local[1]);
            B::ToHavok(out[2], local[2]);
            B::ToHavok(out[3], local[3]);
        }
        {
            static B::Vec3 arm_tip{};
            static bool arm_have_tip = false;
            static s64 arm_fast_ns = 0;
            const B::Vec3 tip = W::Apply(grip, AmygdalanArm::TipOf(arm_cached ? out : clip));
            if (arm_have_tip && dt > 0.0f && dt <= 0.1f &&
                B::Length(tip - arm_tip) >= ArmHurtTipMps * dt)
                arm_fast_ns = now;
            arm_tip = tip;
            arm_have_tip = B::IsFinite(tip);
            g_arm_out.Store({now, arm.Out(), arm_fast_ns, arm.Out() && !on_enemy, grip.t});
        }
        g_out.Store(HookOut{}); // never inherit the previous whip's damage-window hold
        return;
    }
    if (arm_node != nullptr) {
        arm.Clear();
        arm_node = nullptr;
        arm_stamp = 0;
        arm_chr = nullptr;
        arm_cached = arm_hunter = false;
    }
    if (node != h.node) {
        h = {}; // another part (a weapon switch, a load): start again
        h.node = node;
        h.kos_return.Seed(static_cast<unsigned>(now));
        for (const RestCache& c : g_rest_cache) {
            if (c.variation == r.variation && c.n == r.n && r.variation >= 0) {
                std::copy_n(c.rest.begin(), c.n, h.rest.begin());
                std::copy_n(c.rest_q.begin(), c.n, h.rest_q.begin());
                if (r.variation == KosVariation) h.kos_scale = c.kos_scale;
                std::copy_n(c.raw.begin(), c.n, h.rest_raw.begin());
                h.flipped = c.flipped;
                h.links = c.links;
                h.have_rest = h.chain.Init(h.rest.data(), r.n);
                if (g_log >= 1) {
                    LOG_INFO(Core, "VR whip: rest shape of variation {} from the cache ({})",
                             r.variation, h.have_rest ? "ok" : "REJECTED");
                }
                break;
            }
        }
    }
    if (idle && !h.have_rest) {
        // Capture once. A repeated part evaluation can still contain our previous output;
        // recapturing it would feed the simulated bend back as a new rest shape and reset it.
        bool changed = !h.have_rest;
        float links = 0.0f;
        for (s32 k = 0; k < r.n; ++k) {
            const B::Vec3 p = mod[r.chain[k]].t;
            changed = changed || B::Length(p - h.rest_raw[k]) > 0.002f;
            h.rest_raw[k] = p;
            h.rest[k] = p;
            h.rest_q[k] = B::Normalize(mod[r.chain[k]].q);
            if (k > 0) {
                links += B::Length(h.rest[k] - h.rest[k - 1]);
            }
        }
        if (changed) {
            if (r.variation == KosVariation) {
                for (s32 k = 0; k < r.n; ++k) {
                    const s32 j = std::min(k + 1, r.n - 1);
                    if (!h.kos_scale[k].Set(mod[r.chain[k]].s,
                            B::Length(h.rest[j] - h.rest[j - 1]))) return;
                }
            }
            h.flipped = W::RestToBlade(h.rest.data(), h.rest_q.data(), r.n);
            h.have_rest = h.chain.Init(h.rest.data(), r.n);
            h.links = links;
            h.have_prev = false;
            if (h.have_rest && r.variation >= 0) {
                // Remembered for the next part object of this whip.
                RestCache* slot = &g_rest_cache[0];
                for (RestCache& c : g_rest_cache) {
                    if (c.variation == r.variation || c.variation < 0) {
                        slot = &c;
                        break;
                    }
                }
                slot->variation = r.variation;
                slot->n = r.n;
                std::copy_n(h.rest.begin(), r.n, slot->rest.begin());
                std::copy_n(h.rest_q.begin(), r.n, slot->rest_q.begin());
                if (r.variation == KosVariation) slot->kos_scale = h.kos_scale;
                std::copy_n(h.rest_raw.begin(), r.n, slot->raw.begin());
                slot->flipped = h.flipped;
                slot->links = links;
            }
            if (g_log >= 1) {
                LOG_INFO(Core,
                         "VR whip: rest shape: {} chain bones, links {:.3f} m, the last {:.3f} m "
                         "from the grip ({}{})",
                         r.n, links, B::Length(h.rest[r.n - 1]), h.have_rest ? "ok" : "REJECTED",
                         h.flipped ? "; the idle clip holds it back along the forearm: turned to "
                                     "the blade side"
                                   : "");
            }
        }
    }
    if (!h.have_rest && !h.tried_builtin) {
        // Not seen in its idle clip yet (the part appears at a transform's switch: a flick into
        // the whip as the first transform of the session): the idle line from the game's files.
        h.tried_builtin = true;
        const W::WhipRest* const b = FindIdle(r.variation);
        if (b != nullptr && b->n == r.n && b->bones == r.count) {
            for (s32 k = 0; k < r.n; ++k) {
                h.rest[k] = {b->t[k][0], b->t[k][1], b->t[k][2]};
                h.rest_q[k] = B::Normalize(B::Quat{b->q[k][0], b->q[k][1], b->q[k][2], b->q[k][3]});
                h.rest_raw[k] = h.rest[k];
            }
            h.flipped = W::RestToBlade(h.rest.data(), h.rest_q.data(), r.n);
            h.links = b->links;
            h.have_rest = h.chain.Init(h.rest.data(), r.n);
        }
        if (g_log >= 1) {
            LOG_INFO(Core, "VR whip: rest shape of variation {} from the game's files ({}{})",
                     r.variation,
                     b == nullptr ? "none" : !h.have_rest ? "REJECTED (another skeleton)" : "ok",
                     h.flipped ? ", turned to the blade side" : "");
        }
    }
    if (!h.have_rest) {
        return;
    }
    // RestToBlade already corrects the Cane's simulated basis. Correct the animation's
    // chain root too, before capturing its handoff pose or blending it with physics.
    // Child locals retain the original coil/uncoil animation and the handle stays put.
    if (r.variation == 2200 && h.flipped && (idle || W::IsTransformClip(playing))) {
        const s32 root = r.chain[0];
        const auto corrected = W::ClipRootToBlade(mod[r.parents[root]], loc[root]);
        if (!B::IsFinite(corrected)) return;
        loc[root] = corrected;
        if (!B::LocalToModel(loc.data(), r.parents.data(), count, mod.data())) return;
        B::ToHavok(corrected, local[root]);
    }
    const float dt = h.clock.Step(pose_grip ? r.grip_ns : now);
    // VR_WHIP_LEAD: the hand's frame as the part will be placed with it (the dummy 300 read here
    // is one game frame behind the placement: the last frame's motion applied once more).
    const W::Frame raw = grip;
    if (!pose_grip && g_lead && h.have_raw && dt > 0.0f && dt < 0.1f) {
        // One typical frame of the last frame's motion (a hitch's long frame is not repeated:
        // repeated whole, the 0.12-0.13 s frames of an eye dump folded the chain).
        const float k = std::clamp(h.dt_avg / dt, 0.0f, 1.0f);
        grip.t = raw.t + (raw.t - h.raw.t) * k;
        grip.q = B::Normalize(
            B::ScaleRotation(B::Normalize(raw.q * B::Conjugate(h.raw.q)), k) * raw.q);
    }
    if (dt > 0.0f) {
        h.dt_avg += (std::min(dt, 0.05f) - h.dt_avg) * 0.1f;
    }
    if (dt > 0.0f || !h.have_raw) {
        h.raw = raw;
        h.have_raw = true;
    }
    // The swing's speed pays the chain out (core/vr/whip_spin.h Payout: a flail under centrifugal
    // force): toward the light move's reach, or a heavy / charged window's while it is open and
    // just after; it stays out while the speed is kept up and reels in after whip_hold_s of slow
    // motion. Out, it also takes the chain over from a transform's clip in the whip form (a flick
    // into the whip lashes it).
    Combat::MeleeWindow w{};
    const bool have_w = Combat::LatestMeleeWindow(w) && now - w.ns < WindowFreshNs;
    const auto& ws = W::GetWhipSettings();
    const bool heavy_w = have_w && w.committed && !w.unpaid && w.kind > 0 &&
                         (w.open || (w.close_ns != 0 && now - w.close_ns < HeavyHoldNs));
    const u8 kind = heavy_w ? w.kind : 0;
    W::PayoutConfig pc{};
    // The Beast Cutter's chain is thrown (whip_chain.h); the Kos Parasite's tendril too, on the
    // Cutter's swing-path lash with its own dials (core/vr/kos_lash.h).
    const bool kos = r.variation == KosVariation;
    const bool thrown = r.variation == 2400 || kos;
    pc.v_full = thrown && ws.cutter_full > 0.0f ? ws.cutter_full : ws.payout_speed;
    pc.v_start = ws.payout_speed * (thrown ? ws.cutter_start : W::PayoutStartShare);
    pc.v_keep = ws.payout_speed * (thrown ? ws.cutter_keep : W::PayoutKeepShare);
    pc.hold_s = thrown && ws.cutter_hold_s > 0.0f ? ws.cutter_hold_s
                                                  : W::HoldFor(r.variation, ws.hold_s);
    const W::SwingInput input = W::LatestSwingInput(now, SpeedFreshNs);
    const float speed = r.drive ? input.speed : 0.0f;
    const bool compact = W::CompactCutter(r.variation, true, r.drive, input.compact);
    if (compact != h.compact_logged) {
        // Always logged: a Cutter whose chain never pays out is held compact, or is not driven
        // (the tick's "not driving" line).
        h.compact_logged = compact;
        LOG_INFO(Core, "VR whip: Beast Cutter {}", compact
                     ? "held compact (the right trigger): no payout"
                     : "released from compact: pays out again");
    }
    const float cap = MaxExtension(r, kind);
    const float light_cap = MaxExtension(r, 0);
    bool recall = false;
    float throw_share = 1.0f; // of the swing's speed that may start a payout
    bool turning = false;     // a stroke turning back: the chain comes in for the next
    if (r.variation == 2400 && r.drive && !compact && h.have_prev && h.chain.Ready()) {
        B::Vec3 feet{}, fwd{}, left{};
        const bool body = HunterFrame(r.chr, feet, fwd, left) && B::IsFinite(feet) &&
                          B::IsFinite(fwd) && h.have_body && now - h.body_ns < 100 * Ms;
        const float turn = body ? B::Angle(fwd, h.body_fwd) : B::Pi;
        if (body && turn < CarryTurnDeg * B::Deg && dt > 0.0f && dt <= 0.05f) {
            const auto axis = B::Normalize(h.rest[1] - h.rest[0]);
            const auto probe = h.rest[0] + axis * 0.25f;
            const auto delta = W::Apply(grip, probe) - W::Apply(h.prev, probe) -
                               (feet - h.body_pos);
            const auto velocity = delta * (1.0f / dt);
            const auto to_tip = B::Normalize(h.chain.Point(r.n - 1) - h.chain.Point(0));
            const float pull = std::min(B::Dot(velocity, to_tip),
                                        B::Dot(velocity, B::Rotate(grip.q, axis)));
            recall = h.recall.Step(pull, B::Length(velocity), speed,
                                   h.chain.Extension(), dt, pc);
            h.hand_speed = B::Length(grip.t - h.prev.t - (feet - h.body_pos)) / dt;
            // The weapon's far end (its idle line's tip) relative to the hunter, and its line.
            const auto& far_end = h.rest[r.n - 1];
            const auto far_v = (W::Apply(grip, far_end) - W::Apply(h.prev, far_end) -
                                (feet - h.body_pos)) * (1.0f / dt);
            turning = h.turn.Step(far_v, pc.v_full, dt) && ws.cutter_turn;
            const auto line = B::Rotate(grip.q, B::Normalize(far_end - h.rest[0]));
            throw_share = ws.cutter_throw ? W::CutterThrowShare(line, fwd) : 1.0f;
        } else {
            recall = h.recall.Step(0, 0, 0, h.chain.Extension(), 0, pc);
            turning = h.turn.Step({}, pc.v_full, dt) && ws.cutter_turn;
            h.hand_speed = 0.0f;
        }
    } else {
        h.recall.Reset();
        h.turn.Reset();
    }
    // Beast Cutter only (the Threaded Cane has none since 2026-10-15): a snap of the wrist with
    // the chain out reels it straight in (the passive reel-in, now: the whip's own t_in). The
    // grip's turn and speed relative to the hunter, his own turn and walk since the last frame
    // taken out as the carry below does. A crack is the same wrist snap thrown out: only a snap
    // lifting the weapon back recalls (core/vr/whip_flick.h; whip_flick_recall=any: any snap).
    bool flick = false;
    if (r.variation == 2400 && r.drive && !compact && ws.flick_recall && h.have_prev &&
        h.chain.Ready()) {
        const W::FlickConfig fc = ws.flick_any ? W::AnyDirectionFlick() : W::FlickConfig{};
        W::FlickInput in{};
        in.dt = dt;
        in.extension = h.chain.Extension();
        in.asked_out = h.payout.Out();
        in.valid = false;
        B::Vec3 feet{}, fwd{}, left{};
        if (dt > 0.0f && HunterFrame(r.chr, feet, fwd, left) && B::IsFinite(feet) &&
            B::IsFinite(fwd) && h.have_body && now - h.body_ns < 100 * Ms) {
            const float turn = std::atan2(h.body_fwd.z * fwd.x - h.body_fwd.x * fwd.z,
                                          B::Dot(h.body_fwd, fwd));
            if (std::abs(turn) < CarryTurnDeg * B::Deg) {
                const B::Quat rq = B::AxisAngle({0.0f, 1.0f, 0.0f}, turn);
                const W::Frame carried{feet + B::Rotate(rq, h.prev.t - h.body_pos),
                                       B::Normalize(rq * h.prev.q)};
                in.omega = B::AngleBetween(carried.q, B::Normalize(grip.q)) / dt;
                in.hand_speed = B::Length(grip.t - carried.t) / dt;
                // The weapon's axis (its idle line, grip to tip) then and now: the far end lifting.
                const B::Vec3 stick = B::Normalize(h.rest[r.n - 1] - h.rest[0]);
                const B::Vec3 from = B::Rotate(carried.q, stick);
                const B::Vec3 to = B::Rotate(B::Normalize(grip.q), stick);
                in.lift = W::FlickLift({from.x, from.y, from.z}, {to.x, to.y, to.z},
                                       {fwd.x, fwd.y, fwd.z}, dt);
                in.valid = true;
            }
        }
        flick = h.flick.Step(in, fc);
        if (h.flick.Fired()) {
            const auto snap = h.flick.Last();
            LOG_INFO(Core,
                     "VR whip: Beast Cutter flick recall #{} (the wrist {:.0f} deg in {:.0f} ms "
                     "above {:.0f} deg/s, {:.0f}% of it lifting the far end; the hand {:.2f} m/s "
                     "at most; the chain x{:.2f})",
                     h.flick.Flicks(), snap.angle / B::Deg, snap.fast_s * 1000.0f,
                     fc.omega / B::Deg,
                     snap.angle > 0.0f ? 100.0f * snap.lift / snap.angle : 0.0f, snap.hand,
                     in.extension);
        } else if (h.flick.Dropped() != nullptr) {
            LOG_INFO(Core, "VR whip: Beast Cutter flick recall: not this lift ({})",
                     h.flick.Dropped());
        }
    } else {
        h.flick.Reset();
    }
    // Do not let the speed of the recall flick immediately pay the chain out again. A payout
    // starts only on a throw ahead (W::CutterThrowShare); out, the swing's own speed keeps it.
    const bool closed = h.payout.Target() <= 1.05f;
    const float pay_speed = closed ? speed * throw_share : speed;
    // The Beast Cutter's own flight keeps it out (whip_cutter_tip_keep): it comes back once its
    // momentum is spent, not as the hand stops.
    if (thrown && ws.cutter_tip_keep > 0.0f && h.chain.Ready() &&
        h.chain.TipSpeed() > ws.cutter_tip_keep) {
        h.payout.KeepOut();
    }
    // (The swing-path lash has no pull-back recall: every turn of a stroke pulls back a little and
    // snapped the chain in; its length follows the momentum, the trigger and the flick still bring
    // it in at once.)
    const float kos_out =
        kos ? std::clamp((h.chain.Extension() - 1.0f) / std::max(cap - 1.0f, 0.01f), 0.0f, 1.0f)
            : 0.0f;
    if (kos) {
        // The Kos's dials, its reel set per stroke, and its mood (core/vr/kos_lash.h Return).
        if (h.kos_return.Step(kos_out) && g_log >= 1) {
            LOG_INFO(Core, "VR whip: Kos stroke: returns in {:.0f} ms, {}",
                     h.kos_return.Seconds() * 1000.0f, W::Kos::MoodName(h.kos_return.Current()));
        }
        h.kos_lab = W::Kos::Lab();
        h.kos_return.Apply(h.kos_lab);
    }
    const W::CutterLab& lab = kos ? h.kos_lab : ws.cutter_lab;
    if (thrown && lab.on && lab.path_model) recall = false;
    const bool forced_in = compact || recall || flick || turning;
    float s_target = h.payout.Step(pay_speed, cap, dt, pc, forced_in);
    h.forced_in = forced_in;
    if (thrown && lab.on && lab.path_model) {
        // The swing-path lash: its length from the swing's momentum (core/vr/cutter_path.h; the
        // head and the hunter read as the trace records them, so a replay sees the same).
        W::PathFrame pf{};
        pf.dt = dt;
        pf.hand = grip.t;
        pf.weapon = B::Rotate(B::Normalize(grip.q), B::Normalize(h.rest[r.n - 1] - h.rest[0]));
        pf.forced_in = forced_in;
        pf.impact = h.chain.Impact();
        pf.cap = cap;
        B::Vec3 feet{}, fwd{}, left{};
        pf.body_ok = HunterFrame(r.chr, feet, fwd, left) && B::IsFinite(feet) && B::IsFinite(fwd);
        pf.feet = feet;
        pf.facing = fwd;
        B::HandsSample head{};
        B::Anchor at{};
        if (B::LatestHands(head) && head.head_valid && B::LatestAnchor(at) && at.valid &&
            B::IsFinite(head.head_q) && B::Dot(head.head_q, head.head_q) > 0.5f) {
            pf.eye = at.eye_world + B::TrackingToWorld(head.head_p - at.head_k, at.base_yaw,
                                                       at.world_scale);
            pf.forward = B::TrackingToWorld(B::Rotate(B::Normalize(head.head_q), {0.0f, 0.0f, -1.0f}),
                                            at.base_yaw, 1.0f);
            pf.head_ok = true;
        }
        if (kos) {
            const bool tracked = KosSwingFrame(1, now, h.kos_hold, pf);
            if (tracked != h.kos_tracked) {
                h.path.Reset();
                h.kos_tracked = tracked;
            }
        }
        s_target = h.path.Step(pf, lab);
        if (kos) {
            // The Kos's recoil and sway (core/vr/kos_lash.h): its stroke turning back in kicks
            // both; the recoil rides on the length asked for.
            const bool turned = h.kos_recoil.Step(dt, kos_out);
            h.kos_sway.Step(dt, turned, kos_out);
            h.kos_pulse.Step(dt, turned, kos_out);
            s_target = std::clamp(s_target + h.kos_recoil.Offset() / std::max(h.links, 0.05f),
                                  1.0f, std::max(cap, 1.0f));
        }
        h.payout.SetTarget(s_target);
    } else if (thrown && lab.on && lab.mompay) {
        // The length follows the chain's momentum, which only the hand's swing gives it: while
        // the hand swings (vact) the meter follows the chain's front speed; the hand stopped, it
        // halves every mhold (the chain comes back, its own swinging never keeping it out); the
        // chain just hit the ground, it is held hithold first (a smash keeps its force a moment).
        // Fully out above mout, in below min. The trigger and the flick back bring it in, and
        // the swing that brought it in does not throw it out again for 0.3 s.
        if (forced_in) {
            h.meter = 0.0f;
            h.meter_lock = 0.3f;
            h.impact_left = 0.0f;
        } else {
            const float step = std::max(dt, 0.0f);
            h.meter_lock = std::max(0.0f, h.meter_lock - step);
            h.impact_left = h.chain.Impact() > 2.0f ? lab.hithold
                                                     : std::max(0.0f, h.impact_left - step);
            // The hand's own movement, not the controller's far end (a wrist turn swings that).
            const bool swung = h.hand_speed >= lab.vact && h.meter_lock <= 0.0f;
            if (swung) {
                h.meter = std::max(h.front, h.meter * std::exp2(-step / 0.15f));
            } else if (h.impact_left <= 0.0f) {
                h.meter *= std::exp2(-step / std::max(lab.mhold, 0.02f));
            }
            const float f = std::clamp((h.meter - lab.min) / std::max(lab.mout - lab.min, 0.1f),
                                       0.0f, 1.0f);
            s_target = 1.0f + (cap - 1.0f) * f * f * (3.0f - 2.0f * f);
            h.payout.SetTarget(s_target);
        }
    }
    // A payout from closed is a throw: its front follows the hand for CutterThrowS.
    h.throw_left = r.variation == 2400 && closed && s_target > 1.05f
                       ? W::CutterThrowS
                       : std::max(0.0f, h.throw_left - std::max(dt, 0.0f));
    if (h.payout.Reeled()) {
        ++h.reels;
    }
    const bool swing = h.payout.Out();
    const bool was_taken = h.drive.taken;
    const bool want = W::StepDrive(h.drive, r.drive, playing, swing || compact);
    if (g_log >= 1 && h.drive.taken && !was_taken) {
        LOG_INFO(Core,
                 "VR whip: window #{} lashes during the transform clip {}: the chain is the "
                 "simulation's for the rest of it",
                 w.serial, playing);
    }
    if (g_log >= 1 && playing != h.playing_logged) {
        LOG_INFO(Core, "VR whip: the part plays {} -> {} ({})", h.playing_logged, playing,
                 want ? "driven" : "the clip's pose");
        h.playing_logged = playing;
    }
    h.weight = std::clamp(h.weight + (want ? 1.0f : -1.0f) * dt / BlendS, 0.0f, 1.0f);
    if (!want && h.weight <= 0.0f) {
        // Not driven: the chain follows the clip's pose (a seamless start when it takes over).
        std::array<B::Vec3, W::MaxPoints> pts{};
        for (s32 k = 0; k < r.n; ++k) {
            pts[k] = W::Apply(grip, mod[r.chain[k]].t);
        }
        h.chain.ResetTo(pts.data(), 1.0f);
        h.prev = grip;
        h.have_prev = true;
        h.payout.Reset();
        h.recall.Reset();
        h.turn.Reset();
        h.throw_left = 0.0f;
        h.meter = 0.0f;
        h.have_front = false;
        h.path.Reset();
        h.flick.Reset();
        h.recall_sound.Reset();
        W::PublishRecallAudio({}, false, 0, now);
        HookOut o{};
        o.ns = now;
        g_out.Store(o);
        W::PublishWhipFeel({now, 0.0f, 0.0f, h.locks, h.reels, false, h.lash, false});
        return;
    }
    if (!h.have_prev || !h.chain.Ready()) {
        h.chain.Reset(grip);
        h.prev = grip;
        h.have_prev = true;
    }
    // The payout: the chain to the extension the swing's speed asks for.
    const bool lash = want && swing;
    h.cfg = W::ConfigFor(kos ? 2400 : r.variation);
    if (r.variation == 2400 && lash && !recall && !compact && !turning && !lab.on) {
        W::TuneCutterFlight(speed, pc.v_keep, pc.v_full, h.cfg);
    }
    if (thrown && lab.on) {
        // The lab's free chain (Cutter Chain Lab; whip_cutter_lab), or its swing-path lash.
        W::ApplyLabChain(lab, h.cfg);
        if (lab.path_model) h.path.Apply(lab, h.cfg);
        if (kos) {
            h.cfg.sway = h.kos_sway.Amplitude(
                std::clamp((h.chain.Extension() - 1.0f) / std::max(cap - 1.0f, 0.01f), 0.0f, 1.0f),
                W::Kos::GainOf(h.kos_return.Current()));
            h.cfg.sway_phase = 1.3f;
        }
        // The arm's line, chest to hand (the head 35 cm down), for the drive and the guide.
        B::HandsSample head{};
        B::Anchor at{};
        if (!lab.path_model && lab.aim > 0.0f && B::LatestHands(head) && head.head_valid &&
            B::LatestAnchor(at) &&
            at.valid && now - at.steady_ns < 100 * Ms) {
            const B::Vec3 eye = at.eye_world + B::TrackingToWorld(head.head_p - at.head_k,
                                                                  at.base_yaw, at.world_scale);
            const B::Vec3 arm = grip.t - (eye - B::Vec3{0.0f, 0.35f, 0.0f});
            if (B::IsFinite(arm) && B::LengthSq(arm) > 0.01f) {
                h.cfg.aim = B::Normalize(arm);
                h.cfg.aim_share = lab.aim;
            }
        }
    }
    if (thrown && !kos && ws.cutter_reel_s > 0.0f) h.cfg.t_in = ws.cutter_reel_s;
    if (recall) h.cfg.t_in = std::min(h.cfg.t_in, 0.24f);
    if (turning) h.cfg.t_in = std::min(h.cfg.t_in, W::CutterStrokeTurn::ReelS);
    if (r.variation == 2400 && h.throw_left > 0.0f && lash && !recall && !turning &&
        !h.flick.Lifting() && ws.cutter_follow > 0.0f) {
        W::CutterThrowDrive(h.cfg, ws.cutter_follow);
    }
    if (thrown && !kos) {
        h.cfg.spine = ws.cutter_spine;
        h.cfg.unfold = ws.cutter_unfold;
    }
    if (compact) { h.cfg.t_in = 0.18f; h.cfg.lock_closed = true; }
    h.cfg.release = W::ReleaseForSpeed(speed, pc.v_keep, h.cfg); // slow: the chain hangs
    h.cfg.side_damp = W::SideDampForSpeed(speed, pc.v_keep, h.cfg); // ... and stops circling
    if (thrown && lab.on) h.cfg.side_damp = 0.0f; // the lab's chain keeps its own swing
    // The ground at the hunter's feet: the chain out and slow lies on it instead of hanging
    // through the floor (an approximation: a flat floor at his feet; walls still pass).
    {
        B::Vec3 feet{}, fwd{}, left{};
        const bool body =
            HunterFrame(r.chr, feet, fwd, left) && B::IsFinite(feet) && B::IsFinite(fwd);
        h.cfg.floor_y = body ? feet.y + FloorLiftM : W::ChainConfig{}.floor_y;
        // The hunter turned more than CarryTurnDeg in one frame (a snap turn, the game's own
        // quick turns): the chain goes round with him rigidly instead of being left behind and
        // springing back (its last frame's hand with it, so the step sees no jump).
        if (body && h.have_body && now - h.body_ns < 100 * Ms) {
            const float turn = std::atan2(h.body_fwd.z * fwd.x - h.body_fwd.x * fwd.z,
                                          B::Dot(h.body_fwd, fwd));
            if (std::abs(turn) > CarryTurnDeg * B::Deg) {
                const B::Quat rq = B::AxisAngle({0.0f, 1.0f, 0.0f}, turn);
                h.chain.Carry(rq, h.body_pos, feet);
                h.have_front = false; // the carry is no swing of the chain
                h.prev = {feet + B::Rotate(rq, h.prev.t - h.body_pos),
                          B::Normalize(rq * h.prev.q)};
                ++h.carries;
                if (g_log >= 1) {
                    LOG_INFO(Core, "VR whip: the hunter turned {:.0f} deg in a frame: the chain "
                                   "carried round with him (#{})",
                             turn / B::Deg, h.carries);
                }
            }
        }
        h.body_pos = feet;
        h.body_fwd = fwd;
        h.have_body = body;
        h.body_ns = now;
    }
    // The Kos Parasite's tendril on a snap turn (KosSnap): rigidly round with the hand.
    if (kos && h.have_prev && h.chain.Ready() && h.kos_snap.Step(h.prev, grip)) {
        const B::Quat rq =
            B::Normalize(B::Normalize(grip.q) * B::Conjugate(B::Normalize(h.prev.q)));
        h.chain.Carry(rq, h.prev.t, grip.t);
        h.prev = grip;
        h.have_front = false;
        if (g_log >= 1) {
            LOG_INFO(Core, "VR whip: Kos tendril: a snap turn: carried round with the hand (#{})",
                     h.kos_snap.Carries());
        }
    }
    W::GazeAssist gaze{};
    B::HandsSample hands{};
    B::Anchor anchor{};
    if (r.variation == 2400 && lash && speed >= pc.v_keep && !ws.cutter_lab.on &&
        B::LatestHands(hands) &&
        hands.head_valid && now >= hands.steady_ns && now - hands.steady_ns < 100 * Ms &&
        B::LatestAnchor(anchor) && anchor.valid && now >= anchor.steady_ns &&
        now - anchor.steady_ns < 100 * Ms && B::IsFinite(hands.head_q) &&
        B::Dot(hands.head_q, hands.head_q) > 0.5f) {
        gaze.active = true;
        gaze.eye = anchor.eye_world + B::TrackingToWorld(hands.head_p - anchor.head_k,
                                                         anchor.base_yaw, anchor.world_scale);
        gaze.forward = B::TrackingToWorld(
            B::Rotate(B::Normalize(hands.head_q), {0.0f, 0.0f, -1.0f}), anchor.base_yaw, 1.0f);
    }
    if (r.variation == 2400 && h.have_prev && h.chain.Ready() && dt > 0.0f) {
        const float moved = B::Length(grip.t - h.prev.t) / dt;
        const float turned = B::AngleBetween(h.prev.q, B::Normalize(grip.q)) / B::Deg / dt;
        // (The swing-path lash: also every frame the chain itself would restart closed on - a
        // swing's fastest frames and wrist snaps at a turn, 15 in 3.5 min on 2026-11-08.)
        const bool lash_jump = thrown && ws.cutter_lab.on && ws.cutter_lab.path_model &&
                               W::HandJump(h.prev, grip, dt);
        if (moved > TeleportMs || turned > TeleportDegS || lash_jump) {
            // A jump of the hand's frame (a source change, a turn not carried above): the chain
            // goes with it rigidly, then steps from there.
            const float jump_cm = B::Length(grip.t - h.prev.t) * 100.0f;
            const B::Quat r = B::Normalize(B::Normalize(grip.q) * B::Conjugate(B::Normalize(h.prev.q)));
            h.chain.Carry(r, h.prev.t, grip.t);
            h.prev = grip;
            h.have_front = false;
            ++h.teleports;
            if (g_log >= 1 || h.teleports <= 5) {
                LOG_INFO(Core, "VR whip: the hand jumped {:.0f} cm and {:.0f} deg in {:.1f} ms: the "
                               "chain carried with it (#{})",
                         jump_cm, turned * dt, dt * 1000.0f,
                         h.teleports);
            }
        }
    }
    h.chain.Step(h.prev, grip, dt, want ? s_target : 1.0f, h.cfg, gaze);
    TraceMotion(r, h, grip, gaze, now, dt, speed, want ? s_target : 1.0f, pose_grip);
    if (thrown && lab.on) {
        // The momentum meter's input: the front half's speed around the hand, the payout's own
        // growth taken out (it would keep itself out).
        const int half = r.n / 2;
        const B::Vec3 root = h.chain.Point(0);
        const float s_now = h.chain.Extension();
        if (h.have_front && dt > 0.0f && dt < 0.1f && h.front_s > 0.0f) {
            const float k = s_now / h.front_s;
            float sum = 0.0f;
            for (s32 i = half; i < r.n; ++i) {
                const B::Vec3 was = (h.front_prev[i - half] - h.front_root) * k;
                sum += B::Length((h.chain.Point(i) - root) - was);
            }
            h.front = sum / static_cast<float>(r.n - half) / dt;
        } else {
            h.front = 0.0f;
        }
        for (s32 i = half; i < r.n; ++i) h.front_prev[i - half] = h.chain.Point(i);
        h.front_root = root;
        h.front_s = s_now;
        h.have_front = true;
    }
    h.prev = grip;
    // Locked out (the full payout reached: the presenter's tick), and free again below half.
    const float ext = h.chain.Extension();
    if (dt > 0.0f) {
        const bool audible = r.variation == 2400 && r.drive && want;
        // The clink waits for the links to settle into the closed blade (the chain is at `grip`).
        const float shape = audible && ext <= 1.001f ? h.chain.ClosedDeviation(grip) : 0.0f;
        const auto sound = h.recall_sound.Step(audible, s_target <= 1.001f, ext, shape, dt);
        float pan = 0.0f;
        if (audible && B::LatestHands(hands) && hands.head_valid &&
            B::LatestAnchor(anchor) && anchor.valid && now >= hands.steady_ns &&
            now - hands.steady_ns < 100 * Ms && now >= anchor.steady_ns &&
            now - anchor.steady_ns < 100 * Ms) {
            const auto eye = anchor.eye_world + B::TrackingToWorld(
                hands.head_p - anchor.head_k, anchor.base_yaw, anchor.world_scale);
            const auto right = B::TrackingToWorld(B::Rotate(hands.head_q, {1, 0, 0}),
                                                  anchor.base_yaw, 1.0f);
            pan = std::clamp(B::Dot(B::Normalize(grip.t - eye), right), -0.85f, 0.85f);
        }
        W::PublishRecallAudio(sound, audible, pan, now);
        if (sound.clink && g_log >= 1) LOG_INFO(Core, "VR whip: Cutter recall closed (chain clink)");
    }
    if (!h.full && cap > 1.5f && ext >= 1.0f + 0.95f * (cap - 1.0f)) {
        h.full = true;
        ++h.locks;
    } else if (h.full && ext < 1.0f + 0.5f * (cap - 1.0f)) {
        h.full = false;
    }
    // The bone frames in the part's space, written into the clip's locals (blended by the weight).
    std::array<B::Vec3, W::MaxPoints> p{};
    for (s32 k = 0; k < r.n; ++k) {
        p[k] = W::ApplyInverse(grip, h.chain.Point(k));
    }
    std::array<B::Quat, W::MaxPoints> q{};
    W::ChainRotations(p.data(), h.rest_q.data(), r.n, q.data());
    // The locals, all checked before any is written. Every rotation is renormalised: the clip's
    // decompressed quaternions are not exactly unit, and LocalFromModel / Compose down 26 bones
    // square that error at every bone (a 0.2% error became a garbage pose in the first test: a
    // tip dummy kilometres away, a capsule across the whole map).
    std::array<B::QsTransform, W::MaxPoints> clip{};
    for (s32 k = 0; k < r.n; ++k) {
        clip[k] = loc[r.chain[k]];
    }
    if (r.variation == KosVariation && r.n >= 2) {
        // The Kos's tendril thickens as it stretches, as the game's own attacks make it, and
        // pulses as it comes home: each bone's model scale, turned into locals and blended with
        // the clip's as the pose is (core/vr/kos_lash.h LocalScales).
        const float girth = W::Kos::Girth(h.chain.Extension());
        std::array<B::Vec3, W::MaxPoints> want{}, clip_model{}, local{};
        for (s32 k = 0; k < r.n; ++k) {
            const s32 j = std::min(k + 1, r.n - 1);
            const float g = girth * h.kos_pulse.At(static_cast<float>(k) / (r.n - 1),
                                                   W::Kos::GainOf(h.kos_return.Current()).pulse);
            want[k] = h.kos_scale[k].At(B::Length(p[j] - p[j - 1]), g);
            clip_model[k] = mod[r.chain[k]].s;
        }
        W::Kos::LocalScales(want.data(), clip_model.data(), mod[r.parents[r.chain[0]]].s, r.n,
                            h.weight, local.data());
        for (s32 k = 0; k < r.n; ++k) {
            clip[k].s = local[k];
        }
    }
    std::array<B::QsTransform, W::MaxPoints> out{};
    const B::QsTransform end = W::ChainLocals(mod[r.parents[r.chain[0]]], p.data(), q.data(),
                                              clip.data(), r.n, h.weight, out.data());
    const float link_max = h.links / static_cast<float>(std::max(r.n - 1, 1)) *
                               std::max(h.chain.Extension(), 1.0f) * 1.5f +
                           0.02f;
    const char* bad = nullptr;
    for (s32 k = 0; k < r.n && bad == nullptr; ++k) {
        if (!B::IsFinite(out[k])) {
            bad = "not finite";
        } else if (k > 0 &&
                   B::Length(out[k].t) > std::max(link_max, B::Length(clip[k].t) * 1.5f + 0.02f)) {
            bad = "a link too long"; // (while blending out to a clip: its own links may be long)
        }
    }
    if (bad == nullptr && h.weight >= 1.0f && B::Length(end.t - p[r.n - 1]) > 0.01f) {
        bad = "the chain's end off its point";
    }
    if (bad != nullptr) {
        // Never hand the game a broken pose: the clip's stays this frame.
        if (g_log >= 1 && now - h.last_bad_log_ns > 1000 * Ms) {
            h.last_bad_log_ns = now;
            LOG_WARNING(Core,
                        "VR whip: pose not written ({}; the part plays {}, weight {:.2f}, x{:.2f}); "
                        "the clip's pose kept",
                        bad, playing, h.weight, h.chain.Extension());
        }
        HookOut o{};
        o.ns = now;
        g_out.Store(o);
        return;
    }
    for (s32 k = 0; k < r.n; ++k) {
        const s32 b = r.chain[k];
        const B::QsTransform& blended = out[k];
        if (g_log >= 3 && h.lashing && (k == 0 || k == r.n - 1)) {
            LOG_INFO(Core,
                     "VR whip: write bone {} (chain {}) local t ({:.4f},{:.4f},{:.4f}) q "
                     "({:.4f},{:.4f},{:.4f},{:.4f}) s ({:.3f},{:.3f},{:.3f}) | clip's t "
                     "({:.4f},{:.4f},{:.4f}) | model ({:.3f},{:.3f},{:.3f}) w {:.2f}",
                     b, k, blended.t.x, blended.t.y, blended.t.z, blended.q.x, blended.q.y,
                     blended.q.z, blended.q.w, blended.s.x, blended.s.y, blended.s.z, loc[b].t.x,
                     loc[b].t.y, loc[b].t.z, p[k].x, p[k].y, p[k].z, h.weight);
        }
        B::ToHavok(blended, local[b]);
    }
    // The lash's statistics and logs (a lash: the chain asked out by the swing's speed, until it
    // is back on its stick).
    const float tip_speed = h.chain.TipSpeed();
    const float reach = B::Length(p[r.n - 1]);
    if (tip_speed >= W::HurtTipSpeed) {
        h.fast_ns = now; // the chain hurts (bloodborne_melee.cpp: WhipChainNow)
    }
    h.tips[h.tip_next++ % h.tips.size()] = {now, tip_speed};
    if (lash && !h.lashing) {
        h.lashing = true;
        ++h.lash;
        h.lash_window = w.serial;
        h.lash_peak_s = 1.0f;
        h.lash_peak_tip = 0.0f;
        h.lash_reach = 0.0f;
        h.lash_bend = 0.0f;
        h.lash_frames = 0;
        h.lash_ns = now;
        h.last_frame_log_ns = now;
        if (g_log >= 1) {
            LOG_INFO(Core,
                     "VR whip: lash #{} pays out (the swing {:.1f} m/s; toward x{:.2f} of x{:.2f}: "
                     "the {} reach {:.2f} m x{:.2f}; window #{}{})",
                     h.lash, speed, s_target, cap, M::MoveKindName(static_cast<M::MoveKind>(kind)),
                     W::FindWhip(r.variation) != nullptr
                         ? W::FindWhip(r.variation)->reach[std::min<std::size_t>(kind, 2)]
                         : 0.0f,
                     ws.reach_scale, w.serial,
                     !have_w        ? " none"
                     : w.open       ? " open"
                     : w.unpaid     ? " not registered"
                                    : " closed");
        }
    }
    if (h.lashing) {
        ++h.lash_frames;
        h.lash_peak_s = std::max(h.lash_peak_s, ext);
        h.lash_peak_tip = std::max(h.lash_peak_tip, tip_speed);
        h.lash_reach = std::max(h.lash_reach, reach);
        h.lash_bend = std::max(h.lash_bend, h.chain.MaxBend());
        if (g_log >= 2) {
            const B::Vec3 tip = h.chain.Point(r.n - 1);
            LOG_INFO(Core,
                     "VR whip: #{} f{} t={:.3f} s x{:.2f} (target x{:.2f}, swing {:.1f} m/s, slow "
                     "{:.2f} s) tip ({:.3f},{:.3f},{:.3f}) {:.2f} m from the grip, {:.1f} m/s, "
                     "bend {:.0f} deg, dt {:.1f} ms",
                     h.lash, h.lash_frames, static_cast<double>(now - h.lash_ns) * 1e-9, ext,
                     s_target, speed, h.payout.Slow(), tip.x, tip.y, tip.z, reach, tip_speed,
                     h.chain.MaxBend() / B::Deg, dt * 1000.0f);
        } else if (g_log >= 1 && now - h.last_frame_log_ns >= 400 * Ms) {
            // A long lash (a spin): where the chain is every 0.4 s.
            h.last_frame_log_ns = now;
            const B::Vec3 d = h.chain.Point(r.n - 1) - grip.t;
            LOG_INFO(Core,
                     "VR whip: lash #{} out {:.2f} s: x{:.2f} (target x{:.2f}), the swing {:.1f} "
                     "m/s, the chain's end {:.2f} m from the grip ({:+.2f} m above it), tip {:.1f} "
                     "m/s, bend {:.0f} deg",
                     h.lash, static_cast<double>(now - h.lash_ns) * 1e-9, ext, s_target, speed,
                     reach, d.y, tip_speed, h.chain.MaxBend() / B::Deg);
        }
        if (!lash && ext <= 1.001f) {
            h.lashing = false;
            if (g_log >= 1) {
                LOG_INFO(Core,
                         "VR whip: lash #{} done after {:.0f} ms, {} frames: out x{:.2f}, the "
                         "chain's end {:.2f} m from the grip at most, tip {:.1f} m/s peak, bend "
                         "{:.0f} deg max",
                         h.lash, static_cast<double>(now - h.lash_ns) * 1e-6, h.lash_frames,
                         h.lash_peak_s, h.lash_reach, h.lash_peak_tip, h.lash_bend / B::Deg);
            }
        }
    }
    HookOut o{};
    o.ns = now;
    o.weight = h.weight;
    o.s = ext;
    o.compact = compact;
    o.out = h.chain.Out();
    o.tip_speed = tip_speed;
    o.max_speed = h.chain.MaxSpeed();
    o.bend = h.chain.MaxBend();
    o.reach = reach;
    o.variation = r.variation;
    o.grip = grip.t;
    o.fast_ns = h.fast_ns;
    for (const auto& [ns, v] : h.tips) {
        if (now - ns <= TipRecentNs) {
            o.tip_recent = std::max(o.tip_recent, v);
        }
    }
    const B::Vec3 tip = h.chain.Point(r.n - 1);
    o.tip = {tip.x, tip.y, tip.z};
    for (std::size_t i = h.hist.size() - 1; i > 0; --i) {
        h.hist[i] = h.hist[i - 1];
    }
    h.hist[0] = o.tip;
    o.hist = h.hist;
    o.tip_index = r.n - 1;
    o.lash = h.lash;
    o.driving = want;
    g_out.Store(o);
    W::PublishWhipFeel({now, light_cap > 1.0f ? (ext - 1.0f) / (light_cap - 1.0f) : 0.0f,
                        tip_speed, h.locks, h.reels, want, h.lash, lash,
                        lash && h.flick.Lifting(), r.variation,
                        h.fast_ns != 0 && now - h.fast_ns <= FastRecentNs});
}

bool InstallHook(u8* module) {
    using namespace Xbyak::util;
    auto& c = *g_code;
    c.align(16);
    auto* const entry = const_cast<u8*>(c.getCurr());
    Xbyak::Label original, override_pose;
    c.mov(rax, reinterpret_cast<u64>(&g_node_cmp));
    c.cmp(rbx, ptr[rax]);
    c.je(override_pose, Xbyak::CodeGenerator::T_NEAR);
    c.mov(rax, reinterpret_cast<u64>(&g_kos_node_cmp[0])); // the Kos's form B bundles
    c.cmp(rbx, ptr[rax]);
    c.je(override_pose, Xbyak::CodeGenerator::T_NEAR);
    c.cmp(rbx, ptr[rax + 8]);
    c.je(override_pose, Xbyak::CodeGenerator::T_NEAR);
    c.cmp(rbx, ptr[rax + 16]); // ... and its mouth tendril
    c.je(override_pose, Xbyak::CodeGenerator::T_NEAR);
    c.mov(rax, reinterpret_cast<u64>(&Combat::BowPartNode()));
    c.cmp(rbx, ptr[rax]);
    c.jne(original, Xbyak::CodeGenerator::T_NEAR);
    c.L(override_pose);
    c.lea(rsp, ptr[rsp - 128]); // keep the guest red zone intact
    c.pushf();
    for (const auto& reg : {rax, rcx, rdx, rbx, rsi, rdi, r8, r9, r10, r11, rbp}) {
        c.push(reg);
    }
    c.mov(rdi, rbx); // the pose node
    c.mov(rbp, rsp);
    c.and_(rsp, -32);
    c.sub(rsp, 32 * 16);
    for (int i = 0; i < 16; ++i) {
        c.vmovdqu(ptr[rsp + 32 * i], Xbyak::Ymm(i));
    }
    c.mov(rax, reinterpret_cast<u64>(&WhipHook));
    c.call(rax);
    for (int i = 0; i < 16; ++i) {
        c.vmovdqu(Xbyak::Ymm(i), ptr[rsp + 32 * i]);
    }
    c.mov(rsp, rbp);
    for (const auto& reg : {rbp, r11, r10, r9, r8, rdi, rsi, rbx, rdx, rcx, rax}) {
        c.pop(reg);
    }
    c.popf();
    c.lea(rsp, ptr[rsp + 128]);
    c.L(original);
    // The window's own instructions, with the tail jump made absolute.
    c.add(rbx, 0x38);
    c.mov(rdi, rbx);
    c.add(rsp, 0x18);
    c.pop(rbx);
    c.pop(r12);
    c.pop(r13);
    c.pop(r14);
    c.pop(r15);
    c.pop(rbp);
    c.db(0xFF); // jmp qword [rip+0]; dq syncModelSpace
    c.db(0x25);
    c.dd(0);
    c.dq(reinterpret_cast<u64>(module + SyncModelRva));
    c.ready();
    u8* const site = module + TailRva;
    if (!Hook::MakeWritable(site, TailLen)) {
        LOG_ERROR(Core, "VR whip: could not make {:#x} writable", TailRva);
        return false;
    }
    Hook::WriteJump(site, TailLen, entry);
    return true;
}

// ---- The pose tick (loop thread) ----------------------------------------------------------------

struct TickState {
    const u8* chr{nullptr};
    u8* part{nullptr};
    u8* node{nullptr};
    Rig rig{};
    bool rig_ok{false};
    const char* why{"not read yet"};
    const char* logged_why{nullptr};
    bool logged_drive{false};
    s64 logged_ns{0};
    // The reference trace (VR_WHIP_REF).
    s32 ref_clip{-1};
    s64 ref_start_ns{0};
    u32 ref_frames{0};
    u32 checks{0};
};
TickState g_tick{};

// The chain of `skel` (an hkaSkeleton): B_blade_root's child that has children, then on through the
// child that has children; the blade on the last one.
bool ResolveChain(const u8* skel, Rig& r, std::string& why) {
    const s32 count = Read<s32>(skel + OffsetSkelCount);
    const u8* const parents = Read<const u8*>(skel + OffsetSkelParents);
    const u8* const bones = Read<const u8*>(skel + OffsetSkelBones);
    if (count <= 3 || count > MaxBones || !Plausible(parents, 2) || !Plausible(bones)) {
        why = fmt::format("skeleton not readable (count {})", count);
        return false;
    }
    r.count = count;
    for (s32 i = 0; i < count; ++i) {
        r.parents[i] = Read<s16>(parents + 2 * i);
        if (r.parents[i] >= i) {
            why = "the skeleton's parents are not in order";
            return false;
        }
    }
    if (r.variation == 2500) {
        constexpr std::array<std::string_view, 4> names{"handle", "Dummy001", "Dummy002", "Dummy003"};
        if (count != 4) { why = "unexpected Amygdalan Arm bone count"; return false; }
        for (int i = 0; i < 4; ++i) {
            if (r.parents[i] != i - 1 || BoneName(bones, i) != names[i]) {
                why = "unexpected Amygdalan Arm hierarchy";
                return false;
            }
        }
        r.n = 3;
        r.chain = {1, 2, 3};
        return true;
    }
    if (r.variation == KosVariation) {
        // The Kos Parasite's form-A tendril (the hidden weapon's part 0): B_Rarm_Bone_Chain_1-9,
        // each the child of the one before.
        s32 prev = -1;
        r.n = 0;
        for (s32 k = 1; k <= 9; ++k) {
            const std::string name = fmt::format("B_Rarm_Bone_Chain_{}", k);
            s32 found = -1;
            for (s32 i = 0; i < count; ++i) {
                if (BoneName(bones, i) == name) {
                    found = i;
                    break;
                }
            }
            if (found < 0 || (prev >= 0 && r.parents[found] != prev)) {
                why = fmt::format("no Kos tendril ({} {})", name,
                                  found < 0 ? "missing" : "not under the one before");
                return false;
            }
            r.chain[r.n++] = static_cast<s16>(found);
            prev = found;
        }
        r.tip_blade = -1;
        r.mid_blade = -1;
        return true;
    }
    s32 root = -1;
    for (s32 i = 0; i < count; ++i) {
        if (BoneName(bones, i) == "B_blade_root") {
            root = i;
            break;
        }
    }
    if (root < 0) {
        why = "no B_blade_root (not a whip part)";
        return false;
    }
    const auto has_children = [&](s32 b) {
        for (s32 i = b + 1; i < count; ++i) {
            if (r.parents[i] == b) {
                return true;
            }
        }
        return false;
    };
    s32 cur = root;
    r.n = 0;
    while (r.n < W::MaxPoints) {
        s32 next = -1;
        for (s32 i = cur + 1; i < count; ++i) {
            if (r.parents[i] == cur && has_children(i)) {
                next = i;
                break;
            }
        }
        if (next < 0) {
            break;
        }
        r.chain[r.n++] = static_cast<s16>(next);
        cur = next;
    }
    r.tip_blade = -1;
    for (s32 i = cur + 1; i < count; ++i) {
        if (r.parents[i] == cur) {
            r.tip_blade = i;
            break;
        }
    }
    if (r.n < 8) {
        why = fmt::format("a chain of {} bones only", r.n);
        return false;
    }
    return true;
}

// The hunter's frame: position, forward and left (world, horizontal).
bool HunterFrame(const u8* chr, B::Vec3& pos, B::Vec3& fwd, B::Vec3& left) {
    u8* const ctrl = Read<u8*>(chr + L::OffsetChrCtrl);
    if (!Plausible(ctrl)) {
        return false;
    }
    const u8* w = ctrl + OffsetCtrlWorld;
    pos = {Read<float>(w + 0x0C), Read<float>(w + 0x1C), Read<float>(w + 0x2C)};
    // c0000 model space: -Z forward, +X the character's left; W's columns are its axes.
    fwd = B::Normalize(B::Vec3{-Read<float>(w + 0x08), 0.0f, -Read<float>(w + 0x28)});
    left = B::Normalize(B::Vec3{Read<float>(w + 0x00), 0.0f, Read<float>(w + 0x20)});
    return B::LengthSq(fwd) > 0.0f && B::LengthSq(left) > 0.0f;
}

std::string Rel(B::Vec3 p, B::Vec3 pos, B::Vec3 fwd, B::Vec3 left) {
    const B::Vec3 d = p - pos;
    return fmt::format("(fwd {:.2f} left {:.2f} up {:.2f})", B::Dot(d, fwd), B::Dot(d, left), d.y);
}

// VR_WHIP_REF: while the whip part plays one of the game's own clips (an attack: the canned R1 /
// R2 with whip=canned), its hit dummies every update relative to the hunter and the grip.
void RefTrace(u8* chr, TickState& t, s32 playing, s64 now) {
    if (!g_ref) {
        return;
    }
    if (playing == IdleClip || playing < 100'000'000) {
        if (t.ref_clip >= 0) {
            LOG_INFO(Core, "VR whip ref: clip {} over after {} frames, {:.3f} s", t.ref_clip,
                     t.ref_frames, static_cast<double>(now - t.ref_start_ns) * 1e-9);
        }
        t.ref_clip = -1;
        return;
    }
    if (playing != t.ref_clip) {
        t.ref_clip = playing;
        t.ref_start_ns = now;
        t.ref_frames = 0;
    }
    ++t.ref_frames;
    B::Vec3 pos{}, fwd{}, left{}, grip{}, tip{}, mid{}, rootb{};
    if (!HunterFrame(chr, pos, fwd, left) || !DummyAt(chr, GripDummy, grip) ||
        !DummyAt(chr, TipDummy, tip)) {
        return;
    }
    const bool have_mid = DummyAt(chr, MidDummy, mid);
    const bool have_root = DummyAt(chr, RootBladeDummy, rootb);
    LOG_INFO(Core,
             "VR whip ref: clip {} f{} t={:.3f} tip {} {:.2f} m from the grip, {:.2f} m from "
             "the hunter | mid {} | root blade {} | grip {}",
             playing, t.ref_frames, static_cast<double>(now - t.ref_start_ns) * 1e-9,
             Rel(tip, pos, fwd, left), B::Length(tip - grip),
             B::Length(B::Vec3{tip.x - pos.x, 0.0f, tip.z - pos.z}),
             have_mid ? Rel(mid, pos, fwd, left) : std::string{"-"},
             have_root ? Rel(rootb, pos, fwd, left) : std::string{"-"}, Rel(grip, pos, fwd, left));
}

} // Anonymous namespace

void InstallWhip(VAddr base, u64 size) {
    if (g_base != nullptr) {
        return;
    }
    auto* const module = reinterpret_cast<u8*>(base);
    // The presenter reads the setting (the swing speed it publishes, a spin's revolutions, the
    // cracks): on every way out it says what is installed (VR_WHIP, a failed check: canned).
    struct Settle {
        ~Settle() {
            W::GetWhipSettings().physical = g_physical && g_hooked;
        }
    } settle;
    g_physical = W::GetWhipSettings().physical;
    if (const char* env = std::getenv("VR_WHIP"); env != nullptr) {
        g_physical = std::string_view{env} == "physical" || std::string_view{env} == "1";
    }
    if (const char* value = std::getenv("VR_WHIP_LOG"); value != nullptr) {
        g_log = std::max(std::atoi(value), 1);
    }
    g_ref = std::getenv("VR_WHIP_REF") != nullptr;
    g_untested = std::getenv("VR_WHIP_UNTESTED") != nullptr;
    g_lead = std::getenv("VR_WHIP_LEAD") != nullptr;
    if (size <= WorldChrManRva + 8) {
        LOG_INFO(Core, "VR whip: eboot too small ({:#x}) for Bloodborne 1.09; whips canned", size);
        return;
    }
    if (!g_physical && !g_ref && !VR::Bow::GetBowSettings().enabled) {
        LOG_INFO(Core, "VR whip: whip=canned: a whip form's swing presses R1 / R2");
        return;
    }
    struct Site {
        const char* name;
        u64 rva;
        const u8* sig;
        size_t length;
    };
    const std::array<Site, 4> sites = {{
        {"part animation update", PartAnimRva, PartAnimSig.data(), PartAnimSig.size()},
        {"its tail", TailCtxRva, TailCtxSig.data(), TailCtxSig.size()},
        {"syncModelSpace", SyncModelRva, SyncModelSig.data(), SyncModelSig.size()},
        {"GetDmyPolyMtx", DmyPolyRva, DmyPolySig.data(), DmyPolySig.size()},
    }};
    bool ok = true;
    std::string bad;
    for (const Site& s : sites) {
        if (std::memcmp(module + s.rva, s.sig, s.length) != 0) {
            ok = false;
            bad += fmt::format(" {} at {:#x} (live {})", s.name, s.rva,
                               Hook::HexBytes(module + s.rva, std::min<size_t>(s.length, 24)));
        }
    }
    if (ok) {
        // The tail's jump really goes to syncModelSpace.
        s32 disp{};
        std::memcpy(&disp, module + TailRva + TailLen - 4, sizeof(disp));
        if (TailRva + TailLen + static_cast<s64>(disp) != SyncModelRva) {
            ok = false;
            bad += " the tail's jump target";
        }
    }
    if (!ok) {
        LOG_WARNING(Core, "VR whip: physical whip NOT installed:{}; whips canned", bad);
        return;
    }
    g_base = module;
    g_checked = true;
    g_dmy_poly = reinterpret_cast<DmyPolyFn>(module + DmyPolyRva);
    if (g_physical || VR::Bow::GetBowSettings().enabled) {
        try {
            g_code = std::make_unique<Xbyak::CodeGenerator>(1024);
            g_hooked = InstallHook(module);
        } catch (const Xbyak::Error& error) {
            LOG_ERROR(Core, "VR whip: code emission failed ({}); whips canned", error.what());
            g_hooked = false;
        }
        if (!g_hooked) {
            g_physical = false;
        } else if (g_physical) {
            LoadIdleLines();
        }
    }
    LOG_INFO(Core,
             "VR whip: {} (the part update's tail at {:#x}, syncModelSpace ok, GetDmyPolyMtx ok) | "
             "{}{}{}{}{}",
             g_physical ? "physical whip installed" : "OBSERVING ONLY (whip=canned)", TailRva,
             W::WhipSettingsText(), g_log > 0 ? fmt::format(" log={}", g_log) : std::string{},
             g_ref ? " ref trace" : "", g_untested ? " +untested whips" : "",
             g_lead ? " lead" : "");
}

// VR_KOS_PROBE (diagnostics): the Kos Parasite's parts. The game swaps a hidden weapon's model
// (40000000, wp_a_1711: four parts) in for it; this logs the right weapon's part slots 11-14 (and
// the left's 15-18): the dummy each hangs on, its clip, and its skeleton's bones (once per node).
void KosProbe(u8* chr, s32 variation, s64 now) {
    static s64 last_ns = 0;
    static std::array<u8*, 8> seen{};
    static std::array<s32, 8> seen_clip{};
    u8* const parts = Read<u8*>(chr + OffsetChrAsm);
    if (!Plausible(parts)) {
        return;
    }
    const u8* const equip = Read<u8*>(chr + OffsetEquip);
    const s32 hand = Plausible(equip) ? Read<s32>(equip + 0x10) : -1;
    const s32 id = Plausible(equip) && hand >= 0 && hand <= 2
                       ? Read<s32>(equip + 0x64 + 4 * (hand * 2 + 1))
                       : -1;
    bool changed = false;
    std::array<u8*, 8> nodes{};
    std::array<s32, 8> clips{};
    std::string line;
    for (s32 k = 0; k < 8; ++k) {
        u8* const part = Read<u8*>(parts + OffsetAsmParts + 8 * (11 + k));
        if (!Plausible(part) || Read<u8*>(part) != g_base + WeaponPartVtableRva) {
            line += fmt::format(" | {}: -", 11 + k);
            continue;
        }
        u8* const anim = Read<u8*>(part + OffsetPartAnim);
        u8* const state = Plausible(anim) ? Read<u8*>(anim) : nullptr;
        u8* const player = Plausible(state) ? Read<u8*>(state + OffsetStatePlayer) : nullptr;
        u8* const node = Plausible(player) ? Read<u8*>(player + OffsetPlayerNode) : nullptr;
        nodes[k] = Plausible(node) ? node : nullptr;
        clips[k] = Plausible(state) ? Read<s32>(state + OffsetStatePlaying) : -1;
        const u8* const skel = nodes[k] ? Read<u8*>(nodes[k] + OffsetNodePose + OffsetPoseSkeleton)
                                        : nullptr;
        const s32 count = Plausible(skel) ? Read<s32>(skel + OffsetSkelCount) : -1;
        line += fmt::format(" | {}: mode {} dummy {} clip {} asked {} bones {}", 11 + k,
                            Read<s32>(part + OffsetPartMode), Read<s32>(part + OffsetPartDummy),
                            clips[k], Plausible(state) ? Read<s32>(state + OffsetStateAsked) : -1,
                            count);
        if (nodes[k] != seen[k] && Plausible(skel) && count > 0 && count <= 128) {
            const u8* const bones = Read<const u8*>(skel + OffsetSkelBones);
            const s16* const parents = Read<const s16*>(skel + OffsetSkelParents);
            std::string names;
            for (s32 i = 0; i < count && Plausible(bones); ++i) {
                names += fmt::format(" {}:{}<{}", i, BoneName(bones, i),
                                     Plausible(reinterpret_cast<const u8*>(parents)) ? parents[i]
                                                                                     : -9);
            }
            LOG_INFO(Core, "VR kos probe: slot {} node {} skeleton of {}:{}", 11 + k,
                     fmt::ptr(nodes[k]), count, names);
        }
        changed = changed || nodes[k] != seen[k] || clips[k] != seen_clip[k];
    }
    if (changed || now - last_ns > 1500 * Ms) {
        last_ns = now;
        seen = nodes;
        seen_clip = clips;
        LOG_INFO(Core, "VR kos probe: weapon {} variation {} style {}{}", id, variation, Style(chr),
                 line);
    }
}

void WhipPoseTick(u8* chr, const MeleeBody& body) {
    if (!g_checked || chr == nullptr) {
        return;
    }
    auto& t = g_tick;
    const s64 now = NowNs();
    const s32 variation = Variation(chr);
    static const bool kos_probe = std::getenv("VR_KOS_PROBE") != nullptr;
    if (kos_probe) {
        KosProbe(chr, variation, now);
    }
    if (chr != t.chr) {
        t = {};
        t.chr = chr;
    }
    // The right weapon's form-B part and its animation (the Kos Parasite in form A: its tendril).
    const bool kos = variation == KosVariation;
    const bool kos_a = kos && Style(chr) == 1;
    KosBundleTick(chr, kos && Style(chr) >= 3 && Style(chr) <= 5, body.body);
    KosMouthTick(chr, kos && VR::IsFirstPerson());
    const char* why = nullptr;
    u8* part = nullptr;
    u8* state = nullptr;
    u8* node = nullptr;
    s32 playing = -1;
    u8* const parts = Read<u8*>(chr + OffsetChrAsm);
    if (!Plausible(parts)) {
        why = "no ChrAsm";
    } else {
        part = Read<u8*>(parts + OffsetAsmParts + 8 * (kos_a ? KosFormAPartSlot : WhipPartSlot));
        if (!Plausible(part) || Read<u8*>(part) != g_base + WeaponPartVtableRva) {
            why = kos_a ? "no Kos tendril part" : "no form-B part";
        } else {
            u8* const anim = Read<u8*>(part + OffsetPartAnim);
            state = Plausible(anim) ? Read<u8*>(anim) : nullptr;
            u8* const player = Plausible(state) ? Read<u8*>(state + OffsetStatePlayer) : nullptr;
            node = Plausible(player) ? Read<u8*>(player + OffsetPlayerNode) : nullptr;
            if (!Plausible(node)) {
                why = "the part has no animation";
                node = nullptr;
            } else {
                playing = Read<s32>(state + OffsetStatePlaying);
            }
        }
    }
    if (kos && !kos_a) {
        // Out of form A the Kos's tendrils are form B's parts, not this chain (its rest shape is
        // cached per variation: theirs would become form A's).
        node = nullptr;
        why = "the Kos Parasite out of form A";
    }
    if (node != nullptr && (node != t.node || variation != t.rig.variation)) {
        t.node = node;
        t.part = part;
        t.rig = {};
        t.rig.variation = variation;
        t.rig_ok = false;
        u8* const skel = Read<u8*>(node + OffsetNodePose + OffsetPoseSkeleton);
        std::string err;
        if (Plausible(skel) && ResolveChain(skel, t.rig, err)) {
            t.rig_ok = true;
            t.rig.chr = chr;
            t.rig.part = part;
            t.rig.state = state;
            t.rig.node = node;
        }
        if (g_log >= 1) {
            LOG_INFO(Core,
                     "VR whip: right form-B part {} (anim node {}): {}",
                     fmt::ptr(part), fmt::ptr(node),
                     t.rig_ok ? fmt::format("a chain of {} bones of {} (the tip blade {})",
                                            t.rig.n, t.rig.count, t.rig.tip_blade)
                              : fmt::format("no whip chain ({})", err));
        }
    }
    if (node == nullptr) {
        t.node = nullptr;
        t.rig_ok = false;
    }
    RefTrace(chr, t, playing, now);
    // Driven: whip=physical, a whip with rows in the table, its form B in the right hand.
    const W::WhipMoves* const whip = W::FindWhip(variation);
    const bool arm = variation == 2500;
    const s32 style = Style(chr);
    if (why == nullptr) {
        why = !g_physical                         ? "whip=canned"
              : whip == nullptr && !arm && !kos   ? "not a whip"
              : !arm && !kos && !(whip->tested || g_untested)
                  ? "an untested whip (VR_WHIP_UNTESTED)"
              : !kos && (style < 3 || style > 5)  ? "not in form B"
              : !t.rig_ok                         ? "no whip chain on the part"
              : !body.body                        ? "body off"
              : arm && !body.driven               ? "arm not driven"
              : Read<s32>(part + OffsetPartMode) != 2 ||
                        Read<s32>(part + OffsetPartDummy) < 300 ||
                        Read<s32>(part + OffsetPartDummy) >= 340
                  ? "the part is not in the right hand"
                  : nullptr;
    }
    const bool drive = why == nullptr;
    if (t.rig_ok) {
        t.rig.variation = variation;
        t.rig.drive = drive && g_hooked;
        // Relax only the form-B requirement, and only for the Arm's closing clip
        // (checked again in the hook). Body/tracking/physics/attachment stops stay hard.
        t.rig.arm_release = arm && style == 1 && g_physical && g_hooked &&
            body.body && body.driven && Read<s32>(part + OffsetPartMode) == 2 &&
            Read<s32>(part + OffsetPartDummy) >= 300 &&
            Read<s32>(part + OffsetPartDummy) < 340;
        // The part's model space is the hand dummy it hangs on (300; the Threaded Cane's too).
        const s32 dummy = Read<s32>(part + OffsetPartDummy);
        t.rig.dummy = dummy >= 300 && dummy < 340 ? dummy : GripDummy;
        t.rig.grip_ns = 0;
        t.rig.carry_valid = false;
        // The body has just written this pose; its synchronized dummy is still one frame old.
        // Copy only the attachment frame into the seqlock, never the borrowed pose pointers.
        const auto* skeleton = B::C0000::Active();
        if (skeleton && body.pose_locals && body.pose_frame && body.pose_ns > 0 &&
            t.rig.dummy == skeleton->right_weapon_base.id) {
            const auto& d = skeleton->right_weapon_base;
            const auto bone = B::ModelOf(body.pose_locals, B::C0000::Parents.data(),
                                         B::C0000::BoneCount, d.bone);
            if (bone && W::AttachmentFrame(*body.pose_frame, *bone,
                    {d.position[0], d.position[1], d.position[2]},
                    {d.forward[0], d.forward[1], d.forward[2]},
                    {d.up[0], d.up[1], d.up[2]}, t.rig.grip)) {
                t.rig.grip_ns = body.pose_ns;
                t.rig.carry_position = body.pose_frame->t;
                t.rig.carry_yaw = body.base_yaw;
                t.rig.carry_valid = B::IsFinite(t.rig.carry_position) &&
                                    std::isfinite(t.rig.carry_yaw);
            }
        }
        g_rig.Store(t.rig);
    }
    g_node_cmp = g_hooked && t.rig_ok ? reinterpret_cast<u64>(t.node) : 0;
    // Always logged while a whip is in the hand (a chain that never pays out says why; at most
    // 4 a second, a start or stop of the drive at once); every weapon with VR_WHIP_LOG.
    if ((g_log >= 1 || whip != nullptr || kos) &&
        (drive != t.logged_drive || why != t.logged_why) &&
        (g_log >= 1 || drive != t.logged_drive || now - t.logged_ns >= 250 * Ms)) {
        t.logged_drive = drive;
        t.logged_why = why;
        t.logged_ns = now;
        LOG_INFO(Core, "VR whip: {} (variation {} style {}, part on dummy {}, hooked {}, thread '{}')",
                 drive ? fmt::format("driving the {}'s chain",
                                     whip != nullptr ? whip->name
                                     : kos           ? "Kos Parasite's tendril"
                                                     : "?")
                       : fmt::format("not driving ({})", why),
                 variation, style, t.rig_ok ? t.rig.dummy : -1, g_hooked,
                 Common::GetCurrentThreadName());
    }
    // The check that the hit dummies ride the simulated chain: the tip dummy against the chain's
    // last point (a constant offset: the tip blade), every third update of a lash.
    if (g_log >= 1 && drive) {
        HookOut o{};
        if (g_out.Load(o) && o.driving && o.s > 1.05f && now - o.ns < 100 * Ms &&
            (t.checks++ % 3) == 0) {
            B::Vec3 tip{}, grip{};
            if (DummyAt(chr, TipDummy, tip) && DummyAt(chr, GripDummy, grip)) {
                const B::Vec3 last{o.tip[0], o.tip[1], o.tip[2]};
                std::string lag;
                for (const auto& hp : o.hist) {
                    lag += fmt::format(" {:.1f}",
                                       B::Length(tip - B::Vec3{hp[0], hp[1], hp[2]}) * 100.0f);
                }
                LOG_INFO(Core,
                         "VR whip: check lash #{} x{:.2f}: dummy 12100 {:.1f} cm from the chain's "
                         "last bone, {:.2f} m from the grip (the chain's end {:.2f} m); from the "
                         "last bone 0/1/2/3 hook frames before (cm):{}",
                         o.lash, o.s, B::Length(tip - last) * 100.0f, B::Length(tip - grip),
                         o.reach, lag);
                if (g_log >= 3 && t.rig_ok) {
                    // Where the error comes in: the part's hkaPose model space (after the sync)
                    // against the dummy's matrix.
                    const u8* const pose = t.node + OffsetNodePose;
                    const auto* const model = Read<const B::HkQsTransform*>(pose + 0x18);
                    W::Frame g{};
                    std::string text;
                    if (Plausible(reinterpret_cast<const u8*>(model), 16) && GripFrame(chr, g)) {
                        for (const s32 b : {static_cast<s32>(t.rig.chain[0]),
                                            static_cast<s32>(t.rig.chain[t.rig.n / 2]),
                                            static_cast<s32>(t.rig.chain[t.rig.n - 1]),
                                            t.rig.tip_blade}) {
                            if (b < 0) {
                                continue;
                            }
                            const B::QsTransform m = B::FromHavok(model[b]);
                            const B::Vec3 w = W::Apply(g, m.t);
                            text += fmt::format(" | bone {} model ({:.3f},{:.3f},{:.3f}) q "
                                                "({:.3f},{:.3f},{:.3f},{:.3f}) s ({:.2f},{:.2f},"
                                                "{:.2f}) world ({:.3f},{:.3f},{:.3f})",
                                                b, m.t.x, m.t.y, m.t.z, m.q.x, m.q.y, m.q.z, m.q.w,
                                                m.s.x, m.s.y, m.s.z, w.x, w.y, w.z);
                        }
                    }
                    std::array<float, 12> dm{};
                    const bool have_dm = DummyMtx(chr, TipDummy, dm);
                    LOG_INFO(Core,
                             "VR whip: check detail: sync {} {} | dummy 12100 rows {}{}",
                             Plausible(pose) ? Read<u8>(pose + 0x38) : 0xFF,
                             Plausible(pose) ? Read<u8>(pose + 0x39) : 0xFF,
                             have_dm ? fmt::format("[{:.3f} {:.3f} {:.3f} {:.3f}] [{:.3f} {:.3f} "
                                                   "{:.3f} {:.3f}] [{:.3f} {:.3f} {:.3f} {:.3f}]",
                                                   dm[0], dm[1], dm[2], dm[3], dm[4], dm[5], dm[6],
                                                   dm[7], dm[8], dm[9], dm[10], dm[11])
                                    : std::string{"-"},
                             text);
                }
            }
        }
    }
}

const VR::Melee::WeaponMoves* WhipMovesFor(s32 variation, const VR::Melee::WeaponMoves* base) {
    if (!g_hooked || !g_physical || base == nullptr) {
        return base;
    }
    const W::WhipMoves* const w = W::FindWhip(variation);
    if (w == nullptr || !(w->tested || g_untested)) {
        return base;
    }
    // Built once per whip (the table is fixed).
    static std::array<VR::Melee::WeaponMoves, W::Whips.size()> built{};
    static std::array<bool, W::Whips.size()> made{};
    const std::size_t i = static_cast<std::size_t>(w - W::Whips.data());
    if (!made[i]) {
        built[i] = W::WithWhipRows(*base, *w);
        made[i] = true;
    }
    return &built[i];
}

bool IsWhipRow(const VR::Melee::MoveRow* row) {
    if (row == nullptr) {
        return false;
    }
    const auto same = [row](const M::MoveRow& r) {
        return r.judge == row->judge && r.dmy1 == row->dmy1 && r.dmy2 == row->dmy2 &&
               r.stamina == row->stamina && r.mv == row->mv;
    };
    for (const W::WhipMoves& w : W::Whips) {
        for (const M::MoveRow& r : w.rows) {
            if (same(r)) {
                return true;
            }
        }
        // The flick into the whip: its transform attack's capsules hang on the chain too.
        if (const M::MoveRow* tr = W::WhipTransformRow(w.variation); tr != nullptr && same(*tr)) {
            return true;
        }
    }
    return false;
}

bool WhipHoldsWindow(const Combat::MeleeWindow& w, s64 now_ns) {
    if (!g_hooked || w.open || w.close_ns == 0) {
        return false;
    }
    // The Amygdalan Arm's lash: out and fast (its own state; the chain's is empty then).
    ArmOut a{};
    if (g_arm_out.Load(a) && now_ns >= a.ns && now_ns - a.ns <= 100 * Ms) {
        return a.out && now_ns - w.close_ns < ArmHoldAfterCloseNs &&
               now_ns - a.fast_ns <= FastRecentNs;
    }
    HookOut o{};
    if (!g_out.Load(o) || !o.driving || o.compact || now_ns - o.ns > 100 * Ms) {
        return false;
    }
    // Still out after the window (the lash's follow-through) and still fast enough to hurt.
    const s64 hold = o.variation == 2400 ? CutterHoldAfterCloseNs : HoldAfterCloseNs;
    if (W::ContactHeldWhip(o.variation) &&
        !W::WhipContactMotion(W::LatestWhipFeel(), now_ns, o.variation)) {
        return false; // a cancelled/recalling chain must not revive the attack through the tail
    }
    return now_ns - w.close_ns < hold && o.s > 1.3f &&
           now_ns - o.fast_ns <= FastRecentNs;
}

bool WhipWipePoints(u8* chr, std::span<B::Vec3> points, int& count) {
    count = 0;
    if (!g_hooked || !g_physical || !chr || W::FindWhip(Variation(chr)) == nullptr ||
        Style(chr) < 3 || Style(chr) > 5) return false;
    // Read the final bone locals, including animation/physics blending, at the same
    // render tick as the paper and grip. Simulation points alone can be a frame ahead.
    Rig r{};
    if (!g_rig.Load(r) || r.chr != chr || r.variation != Variation(chr) ||
        r.n < 2 || r.n > static_cast<int>(points.size()) || r.count < 1 || r.count > MaxBones)
        return true;
    const auto* parts = Read<const u8*>(chr + OffsetChrAsm);
    if (!Plausible(parts)) return true;
    const auto* part = Read<const u8*>(parts + OffsetAsmParts + 8 * WhipPartSlot);
    if (part != r.part || !Plausible(part) || Read<const u8*>(part) != g_base + WeaponPartVtableRva ||
        Read<s32>(part + OffsetPartMode) != 2) return true;
    const auto* anim = Read<const u8*>(part + OffsetPartAnim);
    const auto* state = Plausible(anim) ? Read<const u8*>(anim) : nullptr;
    const auto* player = Plausible(state) ? Read<const u8*>(state + OffsetStatePlayer) : nullptr;
    const auto* node = Plausible(player) ? Read<const u8*>(player + OffsetPlayerNode) : nullptr;
    if (!Plausible(node) || node != r.node) return true;
    const s32 dummy = Read<s32>(part + OffsetPartDummy);
    W::Frame grip{};
    if (dummy < 300 || dummy >= 340 || !GripFrame(chr, grip, dummy)) return true;
    const auto* pose = r.node + OffsetNodePose;
    const auto* local = Read<const B::HkQsTransform*>(pose + OffsetPoseLocal);
    if (Read<s32>(pose + OffsetPoseLocalSize) != r.count ||
        !Plausible(reinterpret_cast<const u8*>(local), 16))
        return true;
    std::array<B::QsTransform, MaxBones> loc{}, mod{};
    for (int i = 0; i < r.count; ++i) loc[i] = B::FromHavok(local[i]);
    if (!B::LocalToModel(loc.data(), r.parents.data(), r.count, mod.data())) return true;
    for (int i = 0; i < r.n; ++i) {
        if (r.chain[i] < 0 || r.chain[i] >= r.count) return true;
        points[i] = W::Apply(grip, mod[r.chain[i]].t);
        if (!B::IsFinite(points[i])) return true;
    }
    count = r.n;
    return true;
}

ArmLashState ArmLashNow(s64 now_ns) {
    ArmOut a{};
    if (!g_hooked || !g_arm_out.Load(a) || now_ns < a.ns || now_ns - a.ns > 100 * Ms ||
        !B::IsFinite(a.grip)) {
        return {};
    }
    return {true, a.lashing, a.grip};
}

WhipChainState WhipChainNow(s64 now_ns) {
    WhipChainState st{};
    HookOut o{};
    if (!g_hooked || !g_out.Load(o) || !o.driving || now_ns < o.ns || now_ns - o.ns > 150 * Ms) {
        return st;
    }
    if (o.compact && o.s <= 1.02f) return st; // compact club uses normal blade-speed gates
    st.applies = true;
    st.fast = o.fast_ns != 0 && now_ns - o.fast_ns <= FastRecentNs;
    st.tip = o.tip_recent;
    st.variation = o.variation;
    st.grip = o.grip;
    return st;
}

} // namespace VR::Bloodborne
