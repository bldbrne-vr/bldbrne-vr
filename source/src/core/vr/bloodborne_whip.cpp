// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <miniz.h>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/file_sys/fs.h"
#include "core/vr/arm_ik.h"
#include "core/vr/bb_weapons.h"
#include "core/vr/bloodborne_layout.h"
#include "core/vr/bloodborne_melee.h"
#include "core/vr/bow.h"
#include "core/vr/bow_channel.h"
#include "core/vr/bloodborne_whip.h"
#include "core/vr/body_channel.h"
#include "core/vr/c0000_skeleton.h"
#include "core/vr/combat_channel.h"
#include "core/vr/hook_util.h"
#include "core/vr/melee_window.h"
#include "core/vr/seqlock.h"
#include "core/vr/tpf_patch.h"
#include "core/vr/vr_session.h"
#include "core/vr/whip_chain.h"
#include "core/vr/interaction_chain.h"
#include "core/vr/interaction_whip_control.h"
#include "core/vr/interaction_native_whip.h"
#include "core/vr/interaction_native_whip_pose.h"
#include "core/vr/interaction_native_whip_motion.h"
#include "core/vr/interaction_native_whip_rest.h"
#include "core/vr/interaction_native_hierarchy.h"
#include "core/vr/interaction_native_whip_frame.h"
#include "core/vr/whip_audio.h"
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
// The presenter's swing speed older than this: none (the hand is not tracked, a menu).
constexpr s64 SpeedFreshNs = 150 * Ms;
// The attack of a window that closed lives on while the chain is out and fast, this long at most.
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
// off by default (scratchpad/whip/investigate.md; the clean fix is this frame's hand from the
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
    W::Frame grip{};
    s64 grip_ns{0}; // current body pose; zero means use the legacy attachment fallback
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

bool NativeWhipFramePolicy(PiNativeWhipFrameInput& input, PiNativeWhipFrameOutput& output);

// The weapon part's frame (the right hand's dummy it is attached to, 300: the part's model space).
bool GripFrame(u8* chr, W::Frame& f, s32 dummy = GripDummy) {
    std::array<float, 12> m{};
    if (!DummyMtx(chr, dummy, m)) {
        return false;
    }
    PiNativeWhipFrameInput input{};
    input.operation = PI_WF_DUMMY;
    std::copy(m.begin(), m.end(), input.matrix);
    PiNativeWhipFrameOutput output{};
    if (!NativeWhipFramePolicy(input, output) || !output.valid)
        return false;
    f = PhysicalInteractions::ChainPackets::Unpack(output.frame);
    return true;
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

// Guest detours remain installed until process exit. Retain this module/context for that
// lifetime rather than unloading code while a callback could still be executing. It is
// allocated once before installing the hook, and only Game:Main mutates its context.
VR::Interactions::Module* g_chain_module{};
bool g_chain_fault{};
bool g_chain_dll{};

struct HookState {
    VR::Interactions::ChainRuntime chain{};
    W::ChainConfig cfg{};
    u8* node{nullptr};
    bool have_rest{false};
    std::array<B::Vec3, W::MaxPoints> rest{};   // the simulation's idle shape (RestToBlade)
    std::array<B::Quat, W::MaxPoints> rest_q{};
    bool flipped{false};                        // turned to the blade side (the cane)
    float links{0.84f}; // the idle chain's length along its links (m)
    VR::Interactions::ChainDrive drive{}; // driven through a transform's clip (a flick into the whip)
    s32 playing_logged{-1};
    VR::Interactions::NativeWhipMotion motion{};
    VR::Interactions::ChainPoseClock clock{};
    float weight{0.0f};
    s64 last_bad_log_ns{0}; // host diagnostic throttling only
    VR::Interactions::ChainRecallSound recall_sound{};
    VR::Interactions::ChainPullRecall recall{};
    VR::Interactions::ChainWristFlick flick{}; // the Beast Cutter's flick recall (core/vr/whip_flick.h)
    VR::Interactions::ChainPayout payout{}; // the extension the swing's speed asks for

    bool compact_logged{false}; // diagnostic transition only
};
HookState g_hs{};

bool WhipHierarchy(const Rig& r, const B::QsTransform* local, B::QsTransform* model,
                   s32 playing = 0, bool flipped = false, B::QsTransform* root_out = nullptr,
                   bool* corrected = nullptr) {
    if (corrected)
        *corrected = false;
    if (r.count < 1 || r.count > MaxBones ||
        (g_chain_dll &&
         (!g_chain_module || g_chain_fault || VR::Interactions::ProcessFaultLatch().Tripped())))
        return false;
    namespace S = PhysicalInteractions::NativeSkeletonPackets;
    PiNativeHierarchyInput p{};
    p.operation = root_out ? PI_NH_WHIP_ROOT : PI_NH_MODEL;
    p.count = r.count;
    p.variation = r.variation;
    p.playing = playing;
    p.flipped = flipped;
    p.root = r.chain[0];
    for (s32 i = 0; i < r.count; ++i) {
        p.local[i] = S::Pack(local[i]);
        p.parent[i] = r.parents[i];
    }
    if (root_out && p.root >= 0 && p.root < r.count && p.parent[p.root] >= 0 &&
        p.parent[p.root] < r.count)
        p.root_parent = S::Pack(model[p.parent[p.root]]);
    PiNativeHierarchyOutput o{};
    if (!VR::Interactions::NativeHierarchy(g_chain_module, p, o) || !o.valid)
        return false;
    if (p.operation == PI_NH_MODEL || o.corrected)
        for (s32 i = 0; i < r.count; ++i)
            model[i] = S::Unpack(o.model[i]);
    if (o.corrected) {
        *root_out = S::Unpack(o.root_local);
        if (corrected)
            *corrected = true;
    }
    return true;
}

// Opt-in, bounded recording of the ACTUAL game-side input and solver output. Replay this when
// headset feel disagrees with a scripted mock. Never enabled during normal play or benchmarks.
void TraceMotion(const Rig& rig, const HookState& h, const W::Frame& grip,
                 const W::GazeAssist& gaze, s64 now, float dt, float speed, float target,
                 bool current_pose) {
    static FILE* const file = [] {
        const char* path = std::getenv("VR_WHIP_TRACE");
        FILE* f = path && *path ? std::fopen(path, "w") : nullptr;
        if (f) {
            std::fputs("ns,variation,pose_ns,current_pose,dt,px,py,pz,qx,qy,qz,qw,speed,target,"
                       "extension,floor,tip_x,tip_y,tip_z,tip_speed,bend,carries,gaze,"
                       "eye_x,eye_y,eye_z,forward_x,forward_y,forward_z,compact\n",
                       f);
            LOG_INFO(Core, "VR whip: recording up to 9000 live motion samples to {}", path);
        }
        return f;
    }();
    static unsigned count = 0;
    if (!file || count >= 9000 || !rig.drive || dt <= 0.0f) return;
    const auto tip = h.chain.Point(h.chain.Count() - 1);
    std::fprintf(file,
        "%lld,%d,%lld,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,"
        "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d\n",
        static_cast<long long>(now), rig.variation, static_cast<long long>(rig.grip_ns),
        int(current_pose), dt, grip.t.x, grip.t.y, grip.t.z, grip.q.x, grip.q.y, grip.q.z,
        grip.q.w, speed, target, h.chain.Extension(), h.cfg.floor_y, tip.x, tip.y, tip.z,
        h.chain.TipSpeed(), h.chain.MaxBend(), h.motion.Carries(), int(gaze.active), gaze.eye.x,
        gaze.eye.y, gaze.eye.z, gaze.forward.x, gaze.forward.y, gaze.forward.z,
        int(h.cfg.lock_closed));
    if (++count % 60 == 0) std::fflush(file);
}

// Rest policy/cache outlives native part switches. The DLL owns acknowledged cache entries;
// the host keeps only copied current shape buffers and native asset access.
VR::Interactions::NativeWhipRest g_rest_policy{};
PiNativeWhipRestInput RestInput(const Rig& r, uint32_t operation) {
    PiNativeWhipRestInput p{};
    p.operation = operation;
    p.variation = r.variation;
    p.count = r.n;
    p.bones = r.count;
    return p;
}
void ApplyRestShape(HookState& h, const Rig& r, const PiNativeWhipRestOutput& shape) {
    if (!shape.found)
        return;
    namespace C = PhysicalInteractions::ChainPackets;
    for (s32 k = 0; k < r.n; ++k) {
        h.rest[k] = C::Vec(shape.rest[k]);
        h.rest_q[k] = C::Quat(shape.rest_q[k]);
    }
    h.flipped = shape.flipped;
    h.links = shape.links;
    h.have_rest = h.chain.Init(h.rest.data(), r.n);
}

// The whips' idle lines from the game's own files (core/vr/whip_idle.h: the part's skeleton and
// the first frame of its idle clip a999), read once at install: the rest shape of a whip whose
// part was never seen in its idle clip yet. None: the hook waits for the idle clip.
std::array<std::optional<W::WhipRest>, W::WhipParts.size()> g_idle{};

std::optional<W::WhipRest> ReadIdle(const W::WhipPart& part, std::string& why) {
    auto* const mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    const std::string path = fmt::format("/app0/dvdroot_ps4/parts/{}.partsbnd.dcx", part.model);
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
    const std::string anibnd = fmt::format("{}_2.anibnd", part.model);
    const auto inner = VR::Body::C0000::Rig::Bnd4Find(bnd, anibnd);
    if (!inner) {
        why = fmt::format("{} holds no {}", path, anibnd);
        return std::nullopt;
    }
    return W::Idle::FromAnibnd(part.variation, *inner, why);
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

bool NativeWhipPolicy(PiNativeWhipInput& input, PiNativeWhipOutput& output);
bool WhipSpacePolicy(PiNativeWhipInput& input, uint32_t operation, const B::HandsSample& hands,
                     bool have_hands, const B::Anchor& anchor, bool have_anchor, B::Vec3 grip,
                     s64 now, PiNativeWhipOutput& output) {
    namespace C = PhysicalInteractions::ChainPackets;
    input.operation = operation;
    input.now_ns = now;
    auto& f = input.space;
    f.have_hands = have_hands;
    f.have_anchor = have_anchor;
    f.head_valid = hands.head_valid;
    f.anchor_valid = anchor.valid;
    f.hands_ns = hands.steady_ns;
    f.anchor_ns = anchor.steady_ns;
    C::Pack(f.head_p, hands.head_p);
    C::Pack(f.head_q, hands.head_q);
    C::Pack(f.eye_world, anchor.eye_world);
    C::Pack(f.head_k, anchor.head_k);
    f.base_yaw = anchor.base_yaw;
    f.world_scale = anchor.world_scale;
    C::Pack(f.grip, grip);
    return NativeWhipPolicy(input, output);
}

bool HunterFrame(const u8* chr, B::Vec3& pos, B::Vec3& fwd, B::Vec3& left); // (below)

void PS4_SYSV_ABI WhipHook(u8* node) {
    MeleeBowPartPose(node); // native bowstring shares this signature-checked pose tail
    g_hook_calls.fetch_add(1, std::memory_order_relaxed);
    Rig r{};
    if (!g_rig.Load(r) || r.node != node || r.n < 3) {
        return;
    }
    if (g_chain_dll &&
        (!g_chain_module || g_chain_fault || VR::Interactions::ProcessFaultLatch().Tripped())) {
        g_out.Store(HookOut{});
        W::PublishWhipFeel({});
        W::PublishRecallAudio({}, false, 0, NowNs());
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
    std::array<B::QsTransform, MaxBones> loc{};
    std::array<B::QsTransform, MaxBones> mod{};
    for (s32 i = 0; i < count; ++i) {
        loc[i] = B::FromHavok(local[i]);
    }
    if (!WhipHierarchy(r, loc.data(), mod.data())) {
        return;
    }
    W::Frame grip{};
    PiNativeWhipFrameInput source{};
    source.operation = PI_WF_SELECT;
    source.now_ns = now;
    source.pose_ns = r.grip_ns;
    source.pose = PhysicalInteractions::ChainPackets::Pack(r.grip);
    PiNativeWhipFrameOutput selected_pose{};
    if (!NativeWhipFramePolicy(source, selected_pose) || !selected_pose.valid)
        return;
    const bool pose_grip = selected_pose.use_pose;
    if (pose_grip)
        grip = PhysicalInteractions::ChainPackets::Unpack(selected_pose.frame);
    else if (!GripFrame(r.chr, grip, r.dummy))
        return;
    if (node != h.node) {
        h = {}; // another part (a weapon switch, a load): start again
        h.node = node;
        if (g_chain_module) {
            h.chain.Bind(*g_chain_module, g_chain_fault);
            h.motion.Bind(*g_chain_module, g_chain_fault);
            g_rest_policy.Bind(*g_chain_module, g_chain_fault);
            h.payout.Bind(h.chain);
            h.recall.Bind(h.chain);
            h.flick.Bind(h.chain);
            h.drive.Bind(h.chain);
            h.recall_sound.Bind(h.chain);
            h.clock.Bind(h.chain);
        }
        const auto shape = g_rest_policy.Call(RestInput(r, PI_WR_LOOKUP));
        if (shape.found) {
            ApplyRestShape(h, r, shape);
            if (g_log >= 1)
                LOG_INFO(Core, "VR whip: rest shape of variation {} from the cache ({})",
                         r.variation, h.have_rest ? "ok" : "REJECTED");
        }
    }
    // A copied successful solver init skips acquisition; missing-rest policy is DLL-owned.
    bool capture_rest = false;
    if (!h.have_rest) {
        auto plan = RestInput(r, PI_WR_PLAN_CAPTURE);
        plan.playing = playing; plan.have_rest = h.have_rest;
        capture_rest = g_rest_policy.Call(plan).capture_rest != 0;
    }
    if (capture_rest) {
        // Capture once: repeated evaluation must never recapture our simulated pose.
        auto p = RestInput(r, PI_WR_CAPTURE);
        for (s32 k = 0; k < r.n; ++k)
            p.model[k] = PhysicalInteractions::NativeSkeletonPackets::Pack(mod[r.chain[k]]);
        const auto shape = g_rest_policy.Call(p);
        ApplyRestShape(h, r, shape);
        h.motion.ClearPrevious();
        if (shape.cacheable) {
            auto ack = RestInput(r, PI_WR_ACK);
            ack.token = shape.token;
            ack.accepted = h.have_rest;
            g_rest_policy.Call(ack);
        }
        if (g_log >= 1) {
            LOG_INFO(
                Core,
                "VR whip: rest shape: {} chain bones, links {:.3f} m, the last {:.3f} m "
                "from the grip ({}{})",
                r.n, h.links, B::Length(h.rest[r.n - 1]), h.have_rest ? "ok" : "REJECTED",
                h.flipped
                    ? "; the idle clip holds it back along the forearm: turned to the blade side"
                    : "");
        }
    }
    bool try_builtin = false;
    if (!h.have_rest) {
        auto plan = RestInput(r, PI_WR_PLAN_BUILTIN);
        plan.have_rest = h.have_rest;
        try_builtin = g_rest_policy.Call(plan).try_builtin != 0;
    }
    if (try_builtin) {
        // Not seen in its idle clip yet (the part appears at a transform's switch: a flick into
        // the whip as the first transform of the session): the idle line from the game's files.
        const W::WhipRest* const b = FindIdle(r.variation);
        auto p = RestInput(r, PI_WR_BUILTIN);
        if (b != nullptr) {
            p.have_asset = 1;
            p.asset_count = b->n;
            p.asset_bones = b->bones;
            p.links = b->links;
            // Bounds guard for host-owned asset arrays; DLL checks skeleton compatibility.
            if (b->n >= 0 && static_cast<std::size_t>(b->n) <= b->t.size() && b->n <= W::MaxPoints)
                for (s32 k = 0; k < b->n; ++k) {
                    p.model[k] = {{b->t[k][0], b->t[k][1], b->t[k][2]},
                                  {b->q[k][0], b->q[k][1], b->q[k][2], b->q[k][3]},
                                  {1, 1, 1}};
                }
            else
                p.have_asset = 0;
        }
        ApplyRestShape(h, r, g_rest_policy.Call(p));
        if (g_log >= 1) {
            LOG_INFO(Core, "VR whip: rest shape of variation {} from the game's files ({}{})",
                     r.variation,
                     b == nullptr   ? "none"
                     : !h.have_rest ? "REJECTED (another skeleton)"
                                    : "ok",
                     h.flipped ? ", turned to the blade side" : "");
        }
    }
    if (g_chain_dll && (g_chain_fault || VR::Interactions::ProcessFaultLatch().Tripped())) {
        g_out.Store(HookOut{});
        W::PublishWhipFeel({});
        W::PublishRecallAudio({}, false, 0, now);
        return;
    }
    if (!h.have_rest) {
        return;
    }
    // RestToBlade already corrects the Cane's simulated basis. Correct the animation's
    // chain root too, before capturing its handoff pose or blending it with physics.
    // Child locals retain the original coil/uncoil animation and the handle stays put.
    B::QsTransform corrected_root{};
    bool root_corrected = false;
    if (!WhipHierarchy(r, loc.data(), mod.data(), playing, h.flipped, &corrected_root, &root_corrected)) return;
    if (root_corrected) {
        const s32 root = r.chain[0];
        loc[root] = corrected_root;
        B::ToHavok(corrected_root, local[root]);
    }
    const float dt = h.clock.Step(pose_grip ? r.grip_ns : now);
    grip = h.motion.Lead(grip, dt, pose_grip, g_lead);
    if (h.chain.Faulted()) {
        g_out.Store(HookOut{});
        W::PublishWhipFeel({});
        return;
    }
    // The swing's speed pays the chain out (core/vr/whip_spin.h Payout: a flail under centrifugal
    // force): toward the light move's reach, or a heavy / charged window's while it is open and
    // just after; it stays out while the speed is kept up and reels in after whip_hold_s of slow
    // motion. Out, it also takes the chain over from a transform's clip in the whip form (a flick
    // into the whip lashes it).
    Combat::MeleeWindow w{};
    const bool have_window = Combat::LatestMeleeWindow(w);
    const auto& ws = W::GetWhipSettings();
    const W::SwingInput input = W::LatestSwingInput(now, SpeedFreshNs);
    PiNativeWhipInput setup_input{};
    setup_input.operation = PI_NWH_SETUP;
    setup_input.variation = r.variation;
    setup_input.now_ns = now;
    auto& setup = setup_input.setup;
    setup.window_ns = w.ns;
    setup.close_ns = w.close_ns;
    setup.have_window = have_window;
    setup.committed = w.committed;
    setup.unpaid = w.unpaid;
    setup.kind = w.kind;
    setup.open = w.open;
    setup.drive = r.drive;
    setup.trigger = input.compact;
    setup.swing_speed = input.speed;
    setup.payout_speed = ws.payout_speed;
    setup.hold_s = ws.hold_s;
    setup.reach_scale = ws.reach_scale;
    setup.links = h.links;
    setup_input.gates.have_previous = h.motion.HavePrevious();
    setup_input.gates.chain_ready = h.chain.Ready();
    setup_input.gates.flick_recall = ws.flick_recall;
    setup_input.gates.dt = dt;
    PiNativeWhipOutput setup_output{};
    if (!NativeWhipPolicy(setup_input, setup_output)) {
        g_out.Store(HookOut{});
        W::PublishWhipFeel({});
        return;
    }
    const bool have_w = setup_output.have_window;
    const u8 kind = static_cast<u8>(setup_output.kind);
    const auto pc = PhysicalInteractions::ChainPackets::Unpack(setup_output.payout);
    const float speed = setup_output.speed;
    const bool compact = setup_output.compact;
    const float cap = setup_output.cap;
    const float light_cap = setup_output.light_cap;
    if (compact != h.compact_logged) {
        // Always logged: a Cutter whose chain never pays out is held compact, or is not driven
        // (the tick's "not driving" line).
        h.compact_logged = compact;
        LOG_INFO(Core, "VR whip: Beast Cutter {}", compact
                     ? "held compact (the right trigger): no payout"
                     : "released from compact: pays out again");
    }
    bool recall = false;
    if (setup_output.pull_allowed) {
        B::Vec3 feet{}, fwd{}, left{};
        const bool body =
            HunterFrame(r.chr, feet, fwd, left) && B::IsFinite(feet) && B::IsFinite(fwd);
        const auto motion =
            h.motion.Motion(PI_WM_PULL, grip, feet, fwd, body, now, dt, h.rest[1] - h.rest[0],
                            h.chain.Point(r.n - 1) - h.chain.Point(0), h.rest[0]);
        recall = motion.motion_valid ? h.recall.Step(motion.radial, motion.gesture, speed,
                                                     h.chain.Extension(), dt, pc)
                                     : h.recall.Step(0, 0, 0, h.chain.Extension(), 0, pc);
    } else {
        h.recall.Reset();
    }
    // Beast Cutter only (the Threaded Cane has none since 2026-10-15): a snap of the wrist with
    // the chain out reels it straight in (the passive reel-in, now: the whip's own t_in). The
    // grip's turn and speed relative to the hunter, his own turn and walk since the last frame
    // taken out as the carry below does. A crack is the same wrist snap thrown out: only a snap
    // lifting the weapon back recalls (core/vr/whip_flick.h; whip_flick_recall=any: any snap).
    bool flick = false;
    if (setup_output.flick_allowed) {
        const W::FlickConfig fc = ws.flick_any ? W::AnyDirectionFlick() : W::FlickConfig{};
        W::FlickInput in{};
        in.dt = dt;
        in.extension = h.chain.Extension();
        in.asked_out = h.payout.Out();
        in.valid = false;
        B::Vec3 feet{}, fwd{}, left{};
        const bool body = dt > 0.0f && HunterFrame(r.chr, feet, fwd, left) && B::IsFinite(feet) &&
                          B::IsFinite(fwd);
        const auto motion = h.motion.Motion(PI_WM_FLICK, grip, feet, fwd, body, now, dt,
                                            h.rest[r.n - 1] - h.rest[0]);
        in.omega = motion.omega;
        in.hand_speed = motion.hand_speed;
        in.lift = motion.lift;
        in.valid = motion.motion_valid;
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
    // Do not let the speed of the recall flick immediately pay the chain out again.
    const float s_target = h.payout.Step(speed, cap, dt, pc, compact || recall || flick);
    if (h.payout.Reeled()) {
        h.motion.Reel();
    }
    const bool swing = h.payout.Out();
    const bool was_taken = h.drive.taken;
    const bool want = h.drive.Step(r.drive, playing, swing || compact);
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
    h.weight = h.chain.Blend(want, dt, BlendS);
    setup_input.gates.have_previous = h.motion.HavePrevious();
    setup_input.gates.chain_ready = h.chain.Ready();
    setup_input.gates.want = want;
    setup_input.gates.swing = swing;
    setup_input.gates.weight = h.weight;
    setup_input.gates.target_extension = s_target;
    setup_input.operation = PI_NWH_HOOK_CONFIG;
    setup.recall = recall;
    if (!NativeWhipPolicy(setup_input, setup_output)) {
        g_out.Store(HookOut{}); W::PublishWhipFeel({}); return;
    }
    const bool lash = setup_output.hook_lash;
    setup_input.gates.lash = lash; // approved fact for subsequent gaze/audio queries
    const float solver_extension = setup_output.solver_extension;
    h.cfg = PhysicalInteractions::ChainPackets::Unpack(setup_output.config);
    if (setup_output.clip_follow) {
        // Not driven: the chain follows the clip's pose (a seamless start when it takes over).
        PiNativeWhipPoseInput clip{};
        clip.count = r.n;
        clip.clip_follow = 1;
        clip.grip = PhysicalInteractions::ChainPackets::Pack(grip);
        for (s32 k = 0; k < r.n; ++k)
            clip.clip[k] = PhysicalInteractions::NativeSkeletonPackets::Pack(mod[r.chain[k]]);
        PiNativeWhipPoseOutput followed{};
        if (!VR::Interactions::NativeWhipPose(g_chain_module, clip, followed) || !followed.write) {
            g_out.Store(HookOut{}); W::PublishWhipFeel({});
            W::PublishRecallAudio({}, false, 0, now);
            return;
        }
        std::array<B::Vec3, W::MaxPoints> pts{};
        for (s32 k = 0; k < r.n; ++k)
            pts[k] = PhysicalInteractions::ChainPackets::Vec(followed.points[k]);
        h.chain.ResetTo(pts.data(), 1.0f);
        h.motion.Commit(grip);
        h.payout.Reset();
        h.recall.Reset();
        h.flick.Reset();
        h.recall_sound.Reset();
        W::PublishRecallAudio({}, false, 0, now);
        HookOut o{};
        o.ns = now;
        g_out.Store(o);
        const auto& feedback = h.motion.FeedbackState();
        W::PublishWhipFeel(
            {now, 0.0f, 0.0f, feedback.locks, feedback.reels, false, feedback.lash, false});
        return;
    }
    if (setup_output.reset_chain) {
        h.chain.Reset(grip);
        h.motion.Commit(grip);
    }
    // The ground at the hunter's feet: the chain out and slow lies on it instead of hanging
    // through the floor (an approximation: a flat floor at his feet; walls still pass).
    {
        B::Vec3 feet{}, fwd{}, left{};
        const bool body =
            HunterFrame(r.chr, feet, fwd, left) && B::IsFinite(feet) && B::IsFinite(fwd);
        const auto motion = h.motion.Motion(PI_WM_BODY, grip, feet, fwd, body, now);
        h.cfg.floor_y = motion.floor_y;
        if (motion.carry) {
            namespace C = PhysicalInteractions::ChainPackets;
            h.chain.Carry(C::Quat(motion.rotation), C::Vec(motion.carry_from),
                          C::Vec(motion.carry_to));
            if (g_log >= 1) {
                LOG_INFO(Core,
                         "VR whip: the hunter turned {:.0f} deg in a frame: the chain "
                         "carried round with him (#{})",
                         motion.turn / B::Deg, motion.carries);
            }
        }
    }
    W::GazeAssist gaze{};
    B::HandsSample hands{};
    B::Anchor anchor{};
    if (setup_output.gaze_possible) {
        const bool have_hands = B::LatestHands(hands), have_anchor = B::LatestAnchor(anchor);
        PiNativeWhipOutput spatial{};
        if (!WhipSpacePolicy(setup_input, PI_NWH_NATIVE_GAZE, hands, have_hands, anchor,
                             have_anchor, grip.t, now, spatial)) {
            g_out.Store(HookOut{});
            W::PublishWhipFeel({});
            W::PublishRecallAudio({}, false, 0, now);
            return;
        }
        gaze.active = spatial.gaze_active;
        gaze.eye = PhysicalInteractions::ChainPackets::Vec(spatial.eye);
        gaze.forward = PhysicalInteractions::ChainPackets::Vec(spatial.forward);
    }
    if (h.chain.Step(h.motion.Previous(), grip, dt, solver_extension, h.cfg, gaze) < 0) {
        g_out.Store(HookOut{});
        W::PublishWhipFeel({});
        W::PublishRecallAudio({}, false, 0, NowNs());
        return;
    }
    if (h.chain.Faulted()) {
        g_out.Store(HookOut{});
        W::PublishWhipFeel({});
        return;
    }
    TraceMotion(r, h, grip, gaze, now, dt, speed, want ? s_target : 1.0f, pose_grip);
    h.motion.Commit(grip);
    // Locked out (the full payout reached: the presenter's tick), and free again below half.
    const float ext = h.chain.Extension();
    // Query/dispatch approved audio intent; geometry remains in the chain DLL.
    bool have_audio_hands = false, have_audio_anchor = false;
    if (setup_output.audio_possible) {
        have_audio_hands = B::LatestHands(hands);
        have_audio_anchor = B::LatestAnchor(anchor);
    }
    setup_input.extension = ext;
    PiNativeWhipOutput audio{};
    if (!WhipSpacePolicy(setup_input, PI_NWH_RECALL_AUDIO, hands, have_audio_hands, anchor,
                         have_audio_anchor, grip.t, now, audio)) {
        g_out.Store(HookOut{});
        W::PublishWhipFeel({});
        W::PublishRecallAudio({}, false, 0, now);
        return;
    }
    if (audio.audio_step) {
        const float shape = audio.shape_needed ? h.chain.ClosedDeviation(grip) : 0.f;
        const auto sound = h.recall_sound.Step(audio.audible, audio.closed, ext, shape, dt);
        W::PublishRecallAudio(sound, audio.audible, audio.pan, now);
        if (sound.clink && g_log >= 1)
            LOG_INFO(Core, "VR whip: Cutter recall closed (chain clink)");
    }
    h.motion.Lock(cap, ext);
    // Copied animation and chain facts; the DLL builds and approves the complete pose
    // before the host writes any native bone. Keep identifiers and Havok storage here.
    PiNativeWhipPoseInput pose_input{};
    pose_input.count = r.n;
    pose_input.grip = PhysicalInteractions::ChainPackets::Pack(grip);
    pose_input.parent =
        PhysicalInteractions::NativeSkeletonPackets::Pack(mod[r.parents[r.chain[0]]]);
    pose_input.weight = h.weight;
    pose_input.links = h.links;
    pose_input.extension = h.chain.Extension();
    for (s32 k = 0; k < r.n; ++k) {
        PhysicalInteractions::ChainPackets::Pack(pose_input.points[k], h.chain.Point(k));
        PhysicalInteractions::ChainPackets::Pack(pose_input.rest_q[k], h.rest_q[k]);
        pose_input.clip[k] = PhysicalInteractions::NativeSkeletonPackets::Pack(loc[r.chain[k]]);
    }
    PiNativeWhipPoseOutput pose_output{};
    if (!VR::Interactions::NativeWhipPose(g_chain_module, pose_input, pose_output)) {
        g_out.Store(HookOut{});
        W::PublishWhipFeel({});
        return;
    }
    const char* bad = pose_output.write                              ? nullptr
                      : pose_output.reason == PI_WHIP_POSE_LONG_LINK ? "a link too long"
                      : pose_output.reason == PI_WHIP_POSE_END ? "the chain's end off its point"
                                                               : "not finite";
    if (bad != nullptr) {
        // Never hand the game a broken pose: the clip's stays this frame.
        if (g_log >= 1 && now - h.last_bad_log_ns > 1000 * Ms) {
            h.last_bad_log_ns = now;
            LOG_WARNING(
                Core,
                "VR whip: pose not written ({}; the part plays {}, weight {:.2f}, x{:.2f}); "
                "the clip's pose kept",
                bad, playing, h.weight, h.chain.Extension());
        }
        HookOut o{};
        o.ns = now;
        g_out.Store(o);
        return;
    }
    std::array<B::Vec3, W::MaxPoints> p{};
    for (s32 k = 0; k < r.n; ++k)
        p[k] = PhysicalInteractions::ChainPackets::Vec(pose_output.points[k]);
    for (s32 k = 0; k < r.n; ++k) {
        const s32 b = r.chain[k];
        const auto blended =
            PhysicalInteractions::NativeSkeletonPackets::Unpack(pose_output.local[k]);
        if (g_log >= 3 && h.motion.FeedbackState().lashing && (k == 0 || k == r.n - 1)) {
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
    const float reach = pose_output.reach;
    PiWhipFeedbackFacts feedback_facts{};
    feedback_facts.lash = lash;
    feedback_facts.window = w.serial;
    feedback_facts.log_level = static_cast<uint32_t>(std::max(g_log, 0));
    feedback_facts.extension = ext;
    feedback_facts.light_cap = light_cap;
    feedback_facts.tip_speed = tip_speed;
    feedback_facts.reach = reach;
    feedback_facts.bend = h.chain.MaxBend();
    PhysicalInteractions::ChainPackets::Pack(feedback_facts.tip, h.chain.Point(r.n - 1));
    const auto feedback = h.motion.Feedback(feedback_facts, now);
    if (h.chain.Faulted()) {
        g_out.Store(HookOut{});
        W::PublishWhipFeel({});
        return;
    }
    if (feedback.started) {
        if (g_log >= 1) {
            LOG_INFO(Core,
                     "VR whip: lash #{} pays out (the swing {:.1f} m/s; toward x{:.2f} of x{:.2f}: "
                     "the {} reach {:.2f} m x{:.2f}; window #{}{})",
                     feedback.lash, speed, s_target, cap,
                     M::MoveKindName(static_cast<M::MoveKind>(kind)),
                     W::FindWhip(r.variation) != nullptr
                         ? W::FindWhip(r.variation)->reach[std::min<std::size_t>(kind, 2)]
                         : 0.0f,
                     ws.reach_scale, w.serial,
                     !have_w    ? " none"
                     : w.open   ? " open"
                     : w.unpaid ? " not registered"
                                : " closed");
        }
    }
    if (feedback.active) {
        if (g_log >= 2) {
            const B::Vec3 tip = h.chain.Point(r.n - 1);
            LOG_INFO(Core,
                     "VR whip: #{} f{} t={:.3f} s x{:.2f} (target x{:.2f}, swing {:.1f} m/s, slow "
                     "{:.2f} s) tip ({:.3f},{:.3f},{:.3f}) {:.2f} m from the grip, {:.1f} m/s, "
                     "bend {:.0f} deg, dt {:.1f} ms",
                     feedback.lash, feedback.frames,
                     static_cast<double>(now - feedback.lash_ns) * 1e-9, ext, s_target, speed,
                     h.payout.Slow(), tip.x, tip.y, tip.z, reach, tip_speed,
                     h.chain.MaxBend() / B::Deg, dt * 1000.0f);
        } else if (feedback.frame_log) {
            // A long lash (a spin): where the chain is every 0.4 s.
            const B::Vec3 d = h.chain.Point(r.n - 1) - grip.t;
            LOG_INFO(Core,
                     "VR whip: lash #{} out {:.2f} s: x{:.2f} (target x{:.2f}), the swing {:.1f} "
                     "m/s, the chain's end {:.2f} m from the grip ({:+.2f} m above it), tip {:.1f} "
                     "m/s, bend {:.0f} deg",
                     feedback.lash, static_cast<double>(now - feedback.lash_ns) * 1e-9, ext,
                     s_target, speed, reach, d.y, tip_speed, h.chain.MaxBend() / B::Deg);
        }
        if (feedback.ended) {
            if (g_log >= 1) {
                LOG_INFO(Core,
                         "VR whip: lash #{} done after {:.0f} ms, {} frames: out x{:.2f}, the "
                         "chain's end {:.2f} m from the grip at most, tip {:.1f} m/s peak, bend "
                         "{:.0f} deg max",
                         feedback.lash, static_cast<double>(now - feedback.lash_ns) * 1e-6,
                         feedback.frames, feedback.peak_extension, feedback.reach,
                         feedback.peak_tip, feedback.bend / B::Deg);
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
    o.fast_ns = feedback.fast_ns;
    o.tip_recent = feedback.tip_recent;
    const B::Vec3 tip = h.chain.Point(r.n - 1);
    o.tip = {tip.x, tip.y, tip.z};
    for (std::size_t i = 0; i < o.hist.size(); ++i)
        o.hist[i] = {feedback.history[i][0], feedback.history[i][1], feedback.history[i][2]};
    o.tip_index = r.n - 1;
    o.lash = feedback.lash;
    o.driving = want;
    g_out.Store(o);
    W::PublishWhipFeel(
        {now, feedback.feel, tip_speed, feedback.locks, feedback.reels, want, feedback.lash, lash,
         lash && h.flick.Lifting()});
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
    if (g_chain_dll &&
        (!g_chain_module || g_chain_fault || VR::Interactions::ProcessFaultLatch().Tripped())) {
        why = "interaction DLL unavailable";
        return false;
    }
    r.count = count;
    PiNativeHierarchyInput input{};
    input.operation = PI_NH_PARENTS;
    // Validate each copied prefix before reading the next native parent, retaining the
    // original fail-stop read order without a host-side hierarchy decision.
    for (s32 i = 0; i < count; ++i) {
        r.parents[i] = Read<s16>(parents + 2 * i);
        input.parent[i] = r.parents[i];
        input.count = i + 1;
        PiNativeHierarchyOutput checked{};
        if (!VR::Interactions::NativeHierarchy(g_chain_module, input, checked) || !checked.valid) {
            why = "the skeleton's parents are not in order";
            return false;
        }
    }
    input.root = -1;
    for (s32 i = 0; i < count; ++i)
        if (BoneName(bones, i) == "B_blade_root") {
            input.root = i;
            break;
        }
    input.operation = PI_NH_WHIP_CHAIN;
    input.count = count;
    PiNativeHierarchyOutput resolved{};
    if (!VR::Interactions::NativeHierarchy(g_chain_module, input, resolved)) {
        why = "interaction DLL unavailable";
        return false;
    }
    r.n = static_cast<s32>(resolved.chain_count);
    r.tip_blade = resolved.tip_blade;
    for (s32 i = 0; i < r.n; ++i)
        r.chain[i] = static_cast<s16>(resolved.chain[i]);
    if (!resolved.valid) {
        why = resolved.reason == PI_NH_CHAIN_NO_ROOT ? "no B_blade_root (not a whip part)"
              : resolved.reason == PI_NH_CHAIN_PARENTS
                  ? "the skeleton's parents are not in order"
                  : fmt::format("a chain of {} bones only", r.n);
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
    PiNativeWhipFrameInput input{};
    input.operation = PI_WF_HUNTER;
    input.matrix[3] = Read<float>(w + 0x0C);
    input.matrix[7] = Read<float>(w + 0x1C);
    input.matrix[11] = Read<float>(w + 0x2C);
    input.matrix[2] = Read<float>(w + 0x08);
    input.matrix[10] = Read<float>(w + 0x28);
    input.matrix[0] = Read<float>(w + 0x00);
    input.matrix[8] = Read<float>(w + 0x20);
    PiNativeWhipFrameOutput output{};
    if (!NativeWhipFramePolicy(input, output) || !output.valid)
        return false;
    namespace C = PhysicalInteractions::ChainPackets;
    pos = C::Vec(output.position);
    fwd = C::Vec(output.forward);
    left = C::Vec(output.left);
    return true;
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
    g_chain_dll = VR::Interactions::DllRequested();
    // Stateless row/hold policy is needed even when chain physics is disabled.
    if (g_chain_dll && !g_chain_module) {
        g_chain_module = new VR::Interactions::Module();
        g_chain_module->EnableFaultBroadcast();
        if (!g_chain_module->LoadBesideExecutable()) {
            LOG_ERROR(Core, "VR whip: DLL unavailable: {}", g_chain_module->Diagnostic());
            g_chain_fault = true;
        }
    }
    if (g_chain_dll && (g_chain_fault || VR::Interactions::ProcessFaultLatch().Tripped()))
        g_physical = false;
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

void WhipPoseTick(u8* chr, const MeleeBody& body) {
    if (!g_checked || chr == nullptr) {
        return;
    }
    auto& t = g_tick;
    const s64 now = NowNs();
    if (chr != t.chr) {
        t = {};
        t.chr = chr;
    }
    // The right weapon's form-B part and its animation.
    const char* why = nullptr;
    u8* part = nullptr;
    u8* state = nullptr;
    u8* node = nullptr;
    s32 playing = -1;
    u8* const parts = Read<u8*>(chr + OffsetChrAsm);
    if (!Plausible(parts)) {
        why = "no ChrAsm";
    } else {
        part = Read<u8*>(parts + OffsetAsmParts + 8 * WhipPartSlot);
        if (!Plausible(part) || Read<u8*>(part) != g_base + WeaponPartVtableRva) {
            why = "no form-B part";
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
    if (node != nullptr && node != t.node) {
        t.node = node;
        t.part = part;
        t.rig = {};
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
            LOG_INFO(Core, "VR whip: right form-B part {} (anim node {}): {}", fmt::ptr(part),
                     fmt::ptr(node),
                     t.rig_ok ? fmt::format("a chain of {} bones of {} (the tip blade {})", t.rig.n,
                                            t.rig.count, t.rig.tip_blade)
                              : fmt::format("no whip chain ({})", err));
        }
    }
    if (node == nullptr) {
        t.node = nullptr;
        t.rig_ok = false;
    }
    RefTrace(chr, t, playing, now);
    // Driven: whip=physical, a whip with rows in the table, its form B in the right hand.
    const s32 variation = Variation(chr);
    const W::WhipMoves* const whip = W::FindWhip(variation);
    const s32 style = Style(chr);
    PiNativeWhipInput acquisition{};
    acquisition.operation = PI_NWH_ACQUIRE;
    acquisition.variation = variation;
    auto& facts = acquisition.acquire;
    facts.native_failure = why != nullptr;
    facts.physical = g_physical;
    facts.allow_untested = g_untested;
    facts.style = style;
    facts.rig_ok = t.rig_ok;
    facts.body_on = bool(body.body);
    facts.hooked = g_hooked;
    PiNativeWhipOutput decision{};
    bool policy_ok = NativeWhipPolicy(acquisition, decision);
    for (int step = 0; policy_ok && decision.next_read && step < 3; ++step) {
        facts.phase = decision.next_read;
        switch (decision.next_read) {
        case PI_WA_READ_MODE:
            facts.mode = Read<s32>(part + OffsetPartMode);
            break;
        case PI_WA_READ_LOW:
            facts.dummy_low = Read<s32>(part + OffsetPartDummy);
            break;
        case PI_WA_READ_HIGH:
            facts.dummy_high = Read<s32>(part + OffsetPartDummy);
            break;
        }
        acquisition.operation = PI_NWH_ATTACH;
        policy_ok = NativeWhipPolicy(acquisition, decision);
    }
    if (policy_ok && decision.next_read) {
        policy_ok = false;
        if (g_chain_module)
            g_chain_module->ReportFault();
    }
    static constexpr const char* reasons[] = {nullptr,
                                              nullptr,
                                              "whip=canned",
                                              "not a whip",
                                              "an untested whip (VR_WHIP_UNTESTED)",
                                              "not in form B",
                                              "no whip chain on the part",
                                              "body off",
                                              "the part is not in the right hand"};
    if (!policy_ok)
        why = "interaction DLL unavailable";
    else if (decision.acquire_reason != PI_WA_NATIVE)
        why = reasons[decision.acquire_reason];
    const bool drive = policy_ok && decision.drive;
    if (t.rig_ok) {
        t.rig.variation = variation;
        t.rig.drive = policy_ok && decision.rig_drive;
        // The part's model space is the hand dummy it hangs on (300; the Threaded Cane's too).
        PiNativeWhipInput attachment{};
        attachment.operation = PI_NWH_DUMMY;
        attachment.acquire.dummy = Read<s32>(part + OffsetPartDummy);
        PiNativeWhipOutput selected{};
        if (!NativeWhipPolicy(attachment, selected)) {
            t.rig.drive = false;
            g_rig.Store(t.rig);
            g_node_cmp = 0;
            g_out.Store(HookOut{});
            W::PublishWhipFeel({});
            return;
        }
        t.rig.dummy = selected.dummy;
        t.rig.grip_ns = 0;
        // The body has just written this pose; its synchronized dummy is still one frame old.
        // Copy only the attachment frame into the seqlock, never the borrowed pose pointers.
        const auto* skeleton = B::C0000::Active();
        if (skeleton && body.pose_locals && body.pose_frame && body.pose_ns > 0 &&
            t.rig.dummy == skeleton->right_weapon_base.id) {
            const auto& d = skeleton->right_weapon_base;
            const auto bone =
                B::ModelOf(body.pose_locals, B::C0000::Parents.data(), B::C0000::BoneCount, d.bone);
            if (bone) {
                namespace C = PhysicalInteractions::ChainPackets;
                PiNativeWhipFrameInput input{};
                input.operation = PI_WF_ATTACHMENT;
                input.character_valid = body.pose_frame->valid;
                std::copy(body.pose_frame->R.begin(), body.pose_frame->R.end(), input.character_r);
                C::Pack(input.character_t, body.pose_frame->t);
                C::Pack(input.character_q, body.pose_frame->q);
                input.bone = PhysicalInteractions::NativeSkeletonPackets::Pack(*bone);
                for (int i = 0; i < 3; ++i) {
                    input.offset[i] = d.position[i];
                    input.forward[i] = d.forward[i];
                    input.up[i] = d.up[i];
                }
                PiNativeWhipFrameOutput output{};
                if (NativeWhipFramePolicy(input, output) && output.valid) {
                    t.rig.grip = C::Unpack(output.frame);
                    t.rig.grip_ns = body.pose_ns;
                }
            }
        }
        g_rig.Store(t.rig);
    }
    g_node_cmp = g_hooked && t.rig_ok ? reinterpret_cast<u64>(t.node) : 0;
    // Always logged while a whip is in the hand (a chain that never pays out says why; at most
    // 4 a second, a start or stop of the drive at once); every weapon with VR_WHIP_LOG.
    if ((g_log >= 1 || whip != nullptr) && (drive != t.logged_drive || why != t.logged_why) &&
        (g_log >= 1 || drive != t.logged_drive || now - t.logged_ns >= 250 * Ms)) {
        t.logged_drive = drive;
        t.logged_why = why;
        t.logged_ns = now;
        LOG_INFO(Core, "VR whip: {} (variation {} style {}, part on dummy {}, hooked {}, thread '{}')",
                 drive ? fmt::format("driving the {}'s chain", whip != nullptr ? whip->name : "?")
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

namespace {
bool NativeWhipFramePolicy(PiNativeWhipFrameInput& input, PiNativeWhipFrameOutput& output) {
    if (g_chain_dll &&
        (!g_chain_module || g_chain_fault || VR::Interactions::ProcessFaultLatch().Tripped())) {
        output = {};
        return false;
    }
    return VR::Interactions::NativeWhipFrame(g_chain_module, input, output);
}
bool NativeWhipPolicy(PiNativeWhipInput& input, PiNativeWhipOutput& output) {
    if (g_chain_dll &&
        (!g_chain_module || g_chain_fault || VR::Interactions::ProcessFaultLatch().Tripped())) {
        output = {};
        return false;
    }
    return VR::Interactions::NativeWhip(g_chain_module, input, output);
}
} // namespace
const VR::Melee::WeaponMoves* WhipMovesFor(s32 variation, const VR::Melee::WeaponMoves* base) {
    PiNativeWhipInput input{};
    input.operation = PI_NWH_ROWS;
    input.variation = variation;
    input.hooked = g_hooked;
    input.physical = g_physical;
    input.have_base = base != nullptr;
    input.allow_untested = g_untested;
    PiNativeWhipOutput output{};
    if (!NativeWhipPolicy(input, output))
        return nullptr;
    if (!output.use)
        return base;
    // Host-side immutable storage memoization; eligibility and exact row values are DLL-owned.
    static std::array<VR::Melee::WeaponMoves, W::Whips.size()> built{};
    static std::array<bool, W::Whips.size()> made{};
    const auto index = static_cast<size_t>(output.index);
    if (!made[index]) {
        if (!base)
            return nullptr;
        built[index] = *base;
        for (int i = 0; i < 3; ++i)
            built[index].rows[3 + i] =
                PhysicalInteractions::NativeWhipPackets::Unpack(output.rows[i]);
        made[index] = true;
    }
    return &built[index];
}

bool IsWhipRow(const VR::Melee::MoveRow* row) {
    PiNativeWhipInput input{};
    input.operation = PI_NWH_MATCH;
    input.have_row = row != nullptr;
    if (row)
        input.row = PhysicalInteractions::NativeWhipPackets::Pack(*row);
    PiNativeWhipOutput output{};
    return NativeWhipPolicy(input, output) && output.match;
}

bool WhipHoldsWindow(const Combat::MeleeWindow& w, s64 now_ns) {
    PiNativeWhipInput input{};
    input.operation = PI_NWH_HOLD;
    input.hooked = g_hooked;
    input.window_open = w.open;
    input.close_ns = w.close_ns;
    input.now_ns = now_ns;
    HookOut output{};
    // Preserve the original short-circuit shared-output read before supplying copied facts.
    if (g_hooked && !w.open && w.close_ns != 0 && g_out.Load(output)) {
        input.have_output = 1;
        input.driving = output.driving;
        input.compact = output.compact;
        input.output_ns = output.ns;
        input.fast_ns = output.fast_ns;
        input.extension = output.s;
    }
    PiNativeWhipOutput decision{};
    return NativeWhipPolicy(input, decision) && decision.hold;
}

bool WhipWipePoints(u8* chr, std::span<B::Vec3> points, int& count) {
    count = 0;
    if (!g_hooked || !g_physical || !chr || W::FindWhip(Variation(chr)) == nullptr ||
        Style(chr) < 3 || Style(chr) > 5)
        return false;
    // Read the final bone locals, including animation/physics blending, at the same
    // render tick as the paper and grip. Simulation points alone can be a frame ahead.
    Rig r{};
    if (!g_rig.Load(r) || r.chr != chr || r.variation != Variation(chr) || r.n < 2 ||
        r.n > static_cast<int>(points.size()) || r.n > static_cast<int>(r.chain.size()) ||
        r.count < 1 || r.count > MaxBones)
        return true;
    const auto* parts = Read<const u8*>(chr + OffsetChrAsm);
    if (!Plausible(parts))
        return true;
    const auto* part = Read<const u8*>(parts + OffsetAsmParts + 8 * WhipPartSlot);
    if (part != r.part || !Plausible(part) ||
        Read<const u8*>(part) != g_base + WeaponPartVtableRva ||
        Read<s32>(part + OffsetPartMode) != 2)
        return true;
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
    // One copied packet owns both FK and the final world-space contact path. Keep native
    // reads here; do not use simulation points from a different tick for rendered contact.
    if (g_chain_dll &&
        (!g_chain_module || g_chain_fault || VR::Interactions::ProcessFaultLatch().Tripped()))
        return true;
    namespace S = PhysicalInteractions::NativeSkeletonPackets;
    namespace C = PhysicalInteractions::ChainPackets;
    PiNativeHierarchyInput input{};
    input.operation = PI_NH_WHIP_POINTS;
    input.count = r.count;
    input.point_count = r.n;
    input.grip = C::Pack(grip);
    for (int i = 0; i < r.count; ++i) {
        input.local[i] = S::Pack(B::FromHavok(local[i]));
        input.parent[i] = r.parents[i];
    }
    for (int i = 0; i < r.n; ++i) input.point_bone[i] = r.chain[i];
    PiNativeHierarchyOutput output{};
    if (!VR::Interactions::NativeHierarchy(g_chain_module, input, output) || !output.valid)
        return true;
    for (uint32_t i = 0; i < output.point_count; ++i) points[i] = C::Vec(output.points[i]);
    count = static_cast<int>(output.point_count);
    return true;
}

WhipChainState WhipChainNow(s64 now_ns) {
    PiNativeWhipInput input{};
    input.operation = PI_NWH_CHAIN_SNAPSHOT;
    input.hooked = g_hooked;
    input.now_ns = now_ns;
    HookOut source{};
    if (g_hooked) input.have_output = g_out.Load(source);
    if (input.have_output) {
        input.output_ns = source.ns;
        input.fast_ns = source.fast_ns;
        input.driving = source.driving;
        input.compact = source.compact;
        input.extension = source.s;
        input.variation = source.variation;
        input.contact = {source.tip_recent, {source.grip.x, source.grip.y, source.grip.z}};
    }
    PiNativeWhipOutput output{};
    if (!NativeWhipPolicy(input, output)) return {};
    const auto& c = output.chain;
    return {c.applies != 0, c.fast != 0, c.tip, c.variation,
            {c.grip[0], c.grip[1], c.grip[2]}};
}

} // namespace VR::Bloodborne
