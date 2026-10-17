// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "core/vr/bloodborne_evasion.h"
#include "core/vr/body_channel.h"
#include "core/vr/combat_channel.h"
#include "core/vr/evasion.h"
#include "core/vr/evasion_hitgeom.h"
#include "core/vr/interaction_evasion.h"
#include "core/vr/head_room.h"
#include "core/vr/hook_util.h"
#include "core/vr/player_body.h"
#include "core/vr/seqlock.h"

namespace VR::Bloodborne {

namespace {

using Hook::Plausible;
using Hook::Read;
namespace E = VR::Evade;

// RagdollIns::KeyframeToAnimPose (0x180F450), state-0 path, per body: the target position is at
// [rbp-0x40] (vec4, world), right before `mov rdi, [rbp-0x90]; mov esi, r14d; lea rdx, [rbp-0x40]`
// set up the call to 0xB68A40 (world, body id, &pos, &quat, dt) that applies the hard keyframe.
// r13 = the ragdoll bone index (0-15), [rbp-0x88] = the RagdollIns.
constexpr u64 KeyframeFnRva = 0x180F450;
constexpr std::array<u8, 28> KeyframeFnSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
    0x81, 0xEC, 0x88, 0x00, 0x00, 0x00, 0x48, 0x89, 0xB5, 0x68, 0xFF, 0xFF, 0xFF, 0xC5,
};
constexpr u64 TargetSiteRva = 0x180F578;
constexpr std::array<u8, 14> TargetSiteSig = {
    0x48, 0x8B, 0xBD, 0x70, 0xFF, 0xFF, 0xFF, // mov rdi, [rbp-0x90]
    0x44, 0x89, 0xF6,                         // mov esi, r14d
    0x48, 0x8D, 0x55, 0xC0,                   // lea rdx, [rbp-0x40]
};
constexpr s32 FrameRagdoll = -0x88;
constexpr s32 FrameTarget = -0x40;
constexpr s32 FrameWorldFromModel = -0x98;

constexpr u64 WorldChrManRva = 0x553E878;
constexpr u64 RagdollVtableRva = 0x538D440;
constexpr u64 PhysWorldRva = 0x553D700; // [[[+0x28]+0x08]+0x20] = hknp body array, stride 0x90
constexpr u32 OffsetMainPlayer = 0x60;
constexpr u32 OffsetChrCtrl = 0x58;
constexpr u32 OffsetRagdoll = 0x20;       // ChrCtrl -> RagdollIns
constexpr u32 OffsetRagdollSystem = 0x38; // RagdollIns -> hknp ragdoll
constexpr u32 OffsetSystemBodyIds = 0x20; // hknp ragdoll: body ids (s32), count at +0x28
constexpr u32 OffsetSystemBodyCount = 0x28;
constexpr u32 OffsetSystemSkeleton = 0x40; // the ragdoll hkaSkeleton
constexpr u32 OffsetSkeletonBones = 0x28;  // hkArray<hkaBone>: data, size at +0x30
constexpr u32 OffsetSkeletonBoneCount = 0x30;
constexpr u32 BoneStride = 0x10; // hkaBone {hkStringPtr name, hkBool lock}
constexpr u32 BodyStride = 0x90;
constexpr u32 BodyTranslation = 0x30;
constexpr u32 BodyShape = 0x48;             // hknpBody -> hknpShape*
constexpr u64 CapsuleVtableRva = 0x52B1450; // hknpCapsuleShape
constexpr u32 ShapeRadius = 0x14;
constexpr u32 CapsuleA = 0x40;
constexpr u32 CapsuleB = 0x50;

constexpr s64 StatusMaxAgeNs = 250'000'000; // the body's status is fresh within this
constexpr u32 OffsetRagdollOwner = 0x10;    // RagdollIns -> its ChrIns
constexpr u32 OffsetCtrlOwner = 0x08;       // ChrCtrl -> its ChrIns
constexpr u32 OffsetCtrlWorld = 0x90;       // ChrCtrl: the character's world rows {R row i, t_i} x3

// ---- Wall clearance ------------------------------------------------------------------------
// The game's own collision queries, called from the render-camera hook on the loop thread: the
// follow camera makes six sphere casts through the same physics-world wrapper on the same thread
// about 0x80 bytes earlier in the same function (0x143AC60 -> 0x18090E0 -> hknpWorld::castShape),
// and the world is only stepped later (HavokWorldUpdate_Pre, after DamageStep), so this is the
// same context the game queries the world in. Read-only: no world change, the collector is on the
// stack, no locks.
constexpr u32 OffsetPhysWorldWrapper = 0x28; // [base+PhysWorldRva] -> FrpgPhysWorld (hknpWorld +8)
// Query layer of hkaDetectRagdollPenetration's rays (0x17FE7D0): static map layers {0, 18-21, 27,
// 29}, none of the character layers (the camera's layer 0x25 also hits characters: never used).
constexpr u32 LayerRagdollLandscape = 0x2F;
// The follow camera's sphere cast: bool (FrpgPhysWorld*, u32 filter info, const hkVector4* from,
// const hkVector4* delta, hkVector4* hit centre, hkVector4* hit normal, float* fraction, float
// radius), hit centre = from + delta * fraction. Vectors 16-byte aligned (vmovaps).
constexpr u64 SphereCastRva = 0x18090E0;
constexpr std::array<u8, 34> SphereCastSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
    0x53, 0x48, 0x81, 0xEC, 0x58, 0x01, 0x00, 0x00, 0x4C, 0x89, 0x8D, 0xA8,
    0xFE, 0xFF, 0xFF, 0x4C, 0x89, 0x85, 0xA0, 0xFE, 0xFF, 0xFF,
};
using SphereCastFn = u8(PS4_SYSV_ABI*)(void* phys_world, u32 filter_info, const float* from,
                                       const float* delta, float* hit_center, float* hit_normal,
                                       float* fraction, float radius);
// The ragdoll-vs-landscape ray (hkaRaycastInterface, primary vtable slot 0x538D168; `this` is
// never read): bool (this, const hkVector4* from, const hkVector4* to, hkVector4* hit position,
// hkVector4* hit normal). No allocation (the fallback, VR_EVADE_WALL=ray).
constexpr u64 RagdollRayRva = 0x17FE5A0;
constexpr std::array<u8, 32> RagdollRaySig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC,
    0xF8, 0x00, 0x00, 0x00, 0x4D, 0x89, 0xC6, 0x49, 0x89, 0xCF, 0x4C, 0x8B, 0x2D, 0x1F, 0x56, 0xBE,
};
using RagdollRayFn = u8(PS4_SYSV_ABI*)(void* unused, const float* from, const float* to,
                                       float* hit_pos, float* hit_normal);
enum class WallMethod { Off, Sphere, Ray };
// Casts from the character root along the lean: the chest (Spine02, r 0.144) and the head
// (Head01, r 0.094) as spheres a little larger than their capsules. Below 1.1 m nothing is cast,
// so you can lean over a low railing.
struct WallCast {
    float height_m;
    float radius_m;
};
constexpr std::array<WallCast, 2> WallCasts{{{1.25f, 0.15f}, {1.55f, 0.10f}}};
// Independent check (logs): from the Spine02 / Head01 hurtboxes' actual positions along the lean.
constexpr std::array<std::pair<E::Body, float>, 2> GapBodies{
    {{E::Spine02, 0.144f}, {E::Head01, 0.094f}}};
constexpr float GapCastLength = 0.6f;

// What the keyframe hook applies: per ragdoll bone index, the offset in game units.
struct Published {
    u64 ragdoll{};
    u64 player{}; // the ragdoll's owner (a reused ragdoll address after a load is not ours)
    u64 frame{};
    std::array<std::array<float, 3>, E::BodyCount> offset{}; // by ragdoll bone index
    std::array<s8, E::BodyCount> body_of_bone{};             // E::Body of each ragdoll bone
    float view_yaw{};                                        // the hit log's whoosh side
    // The UpperPos shift the offsets subtract (game units) and each bone's factor on it: the
    // keyframe hook swaps it for the shift in the pose it keyframes (the visual body eases its
    // shift, so the render tick's is a frame old). Its downward part as the ragdoll follows it
    // (E::FollowedShiftY; `unit` = world_scale, game units per metre).
    std::array<float, 3> shift{};
    std::array<float, E::BodyCount> shift_k{};
    float unit{1.0f};
    // The same for the trunk lean (body_posture=lean): the eye's move and each bone's share of it.
    std::array<float, 3> lean{};
    std::array<float, E::BodyCount> lean_k{};
};
SeqLock<Published> g_published;

// ---- Hit log (VR_EVADE_HITLOG=1|2, and the near-miss whoosh of evasion_feedback) -------------
// Read-only hooks in AttackHitManager::Update's per-attack cast (DamageStep, after this frame's
// keyframe targets were set and before the physics step moves the bodies onto them):
//  H1 0x18602B4, inside 0x185FBE0 right before it builds the castShape query: r12 = the
//     attacker's ragdoll (bullets resolved to their shooter; the same-owner filter), r13 = the
//     attack's shape wrapper ([+0x28] hknpShape: capsule a +0x40 / b +0x50 or sphere centre
//     +0x30, radius +0x14), [rbp-0xB88] the AttackHitIns entry, [rbp-0xBA0] / [rbp-0xBC0] the
//     sweep's start (last frame) and end, [rbp-0xAC0..0xAA0] the cast orientation (columns).
//     Replays `mov [rbp-0x818], r12; vmovups xmm7, [rbx+0x10]; vmovups xmm6, [rbx+0x20]`.
//  H2 0x1860FB0 entry (entry, ChrIns* target, const hkVector4* pos, normal, material, part):
//     called for every body the cast hit, before the hit lists and team checks: a geometric hit.
//     Replays the 20-byte prologue.
// Per attack near the player the log tests the swept attack shape against the hurtbox capsules
// where they are (S: should agree with the game's hit, G) and where they would be without the
// lean (V, from what the keyframe hook added one keyframe earlier). V && !S && !G = evaded;
// S && !G = ignored by the game (i-frames, a grab, its hit lists), never an evasion.
constexpr u64 AttackCastFnRva = 0x185FBE0;
constexpr std::array<u8, 23> AttackCastFnSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
    0x53, 0x48, 0x81, 0xEC, 0xA8, 0x0B, 0x00, 0x00, 0x48, 0x89, 0xFB,
};
constexpr u64 AttackCastSiteRva = 0x18602B4;
constexpr std::array<u8, 17> AttackCastSiteSig = {
    0x4C, 0x89, 0xA5, 0xE8, 0xF7, 0xFF, 0xFF, // mov [rbp-0x818], r12
    0xC5, 0xF8, 0x10, 0x7B, 0x10,             // vmovups xmm7, [rbx+0x10]
    0xC5, 0xF8, 0x10, 0x73, 0x20,             // vmovups xmm6, [rbx+0x20]
};
constexpr s32 FrameAttackEntry = -0xB88;
constexpr s32 FrameSweepFrom = -0xBA0;
constexpr s32 FrameSweepTo = -0xBC0;
constexpr s32 FrameCastOrient = -0xAC0; // three hkVector4 columns
constexpr u64 ChrHitFnRva = 0x1860FB0;
constexpr std::array<u8, 27> ChrHitFnSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
    0x81, 0xEC, 0x98, 0x02, 0x00, 0x00, 0x44, 0x89, 0x8D, 0x6C, 0xFD, 0xFF, 0xFF,
};
constexpr size_t ChrHitPrologue = 20;      // push rbp .. sub rsp, 0x298
constexpr u64 SphereVtableRva = 0x52AAF20; // hknpSphereShape
constexpr u32 SphereCenter = 0x30;
constexpr u32 WrapperShape = 0x28;
constexpr u32 EntryFixedTarget = 0x1C8;  // set: no cast, damage at the target's root
constexpr float HitNearM = 4.0f;         // attacks farther than this (plus their length) skipped
constexpr s64 HistoryGapNs = 5'000'000;  // keyframes closer than this are one game frame
constexpr s64 AttackEndNs = 300'000'000; // an attack with no cast this long is over
constexpr s64 WhooshCooldownNs = 600'000'000;
constexpr s64 WhooshDelayNs = 100'000'000;  // an attack this long without a cast has passed
constexpr float LeanedHitTolerance = 0.02f; // m: the model's own precision (S vs the game)

// The hook's record of the last targets it moved (logs only; relaxed).
struct Applied {
    std::atomic<u64> count{0};
    std::array<std::array<std::atomic<float>, 3>, E::BodyCount> target{};
    std::array<std::atomic<s32>, E::BodyCount> body_id{};
};
Applied g_applied;

u8* g_base = nullptr;
std::unique_ptr<Xbyak::CodeGenerator> g_code;
std::atomic<u64> g_main_ragdoll{0}; // read by the stub through its address
bool g_installed = false;
int g_log = 0;

// Loop thread only.
VR::Interactions::EvasionRuntime g_model;
VR::Interactions::Module* g_evasion_module{};
bool g_evasion_fault{};
void InitEvasionDll() {
    if (!VR::Interactions::DllRequested() || g_evasion_module)
        return;
    // Installed hooks retain this exclusive render-owner context through process exit.
    g_evasion_module = new VR::Interactions::Module();
    g_evasion_module->EnableFaultBroadcast();
    g_model.Bind(*g_evasion_module, g_evasion_fault);
    if (!g_evasion_module->LoadBesideExecutable()) {
        g_evasion_fault = true;
        g_evasion_module->ReportFault();
        LOG_ERROR(Core, "VR evade: interaction DLL unavailable: {}",
                  g_evasion_module->Diagnostic());
    }
}
u8* g_ragdoll = nullptr;
u8* g_player = nullptr; // its owner
bool g_map_ok = false;
std::array<s32, E::BodyCount> g_bone_of_body{}; // ragdoll bone index of each E::Body
std::array<s32, E::BodyCount> g_body_of_bone{}; // E::Body of each ragdoll bone index
u64 g_frame = 0;
u64 g_capsule_log_frame = 0; // log the hurtbox capsules once, a moment after a new ragdoll
s64 g_last_log_ns = 0;
u32 g_last_mark = 0;
s64 g_mark_until_ns = 0;
const char* g_last_reason = "";

// Wall clearance (loop thread only).
WallMethod g_wall_method = WallMethod::Off;
SphereCastFn g_sphere_cast = nullptr;
// CastMapSphere's (the weapon against the walls: core/vr/weapon_wall.h), installed whatever
// physical_evasion says.
u8* g_map_base = nullptr;
SphereCastFn g_map_cast = nullptr;
RagdollRayFn g_ragdoll_ray = nullptr;
struct WallStats {
    u64 probes{}; // ticks that cast
    u64 casts{};
    u64 hits{};   // probes with a wall within reach
    u64 inside{}; // casts that started inside geometry (no information)
    u64 no_world{};
    u64 limited{}; // ticks the wall limited the lean
    u64 closer{};  // probes where the hurtboxes' own casts were nearer than the root's
    float min_free{1e9f};
    s64 ns{};
    s64 max_ns{};
};
WallStats g_wall;
s64 g_last_wall_log_ns = 0;
float g_wall_dir[2]{}; // the lean direction of the last probe (world x, z)

// Hit log.
int g_hitlog = 0;         // VR_EVADE_HITLOG level
bool g_hit_hooks = false; // H1 / H2 installed (the log or the near-miss whoosh)
// What the keyframe hook added to each hurtbox at one keyframe, plus the UpperPos shift the pose
// already carried (the hurtbox's whole offset from the vanilla animation), by ragdoll bone.
struct AppliedFrame {
    s64 ns{};
    u64 ragdoll{};
    bool valid{false};
    std::array<E::Vec3, E::BodyCount> baseline{};
    std::array<bool, E::BodyCount> excluded{}; // arms on IK: your real hands, no vanilla place
    E::Vec3 head{};                            // Head01's offset (logs)
};
std::mutex g_history_mutex;
std::array<AppliedFrame, 8> g_history{};
u32 g_history_next = 0;
AppliedFrame g_building{}; // the keyframe hook's thread only (one main-player keyframe at a time)
// The keyframe hook's thread: the UpperPos shift in the pose being keyframed (the body's status
// from this frame's pose copy) and the arms' IK weights, read once per keyframe (bone 0).
std::array<float, 3> g_pose_shift{};
std::array<float, 3> g_pose_lean{};
std::array<float, 2> g_pose_ik{};
float g_shift_reach = E::Config{}.shift_reach; // VR_EVADE_SHIFT_REACH (tests)
float g_lean_reach = E::Config{}.lean_reach;   // VR_EVADE_LEAN_REACH (tests)
std::array<float, 3> g_building_shift{};
std::array<float, 3> g_building_lean{};
std::array<float, 2> g_building_ik{};

// The hook's own record (logs, the stub's bone order 0..15).
void RecordApplied(u64 bone, const std::array<float, 3>& applied, const Published& p, bool have) {
    if (bone == 0) {
        g_building = {};
        g_building.ns = VR::Body::SteadyNowNs();
        g_building.ragdoll = p.ragdoll;
        g_building.valid = have;
        g_building_shift = g_pose_shift;
        g_building_shift[1] = E::FollowedShiftY(g_building_shift[1], p.unit);
        g_building_lean = g_pose_lean;
        g_building_ik = g_pose_ik;
    }
    const s8 body = have ? p.body_of_bone[bone] : static_cast<s8>(-1);
    if (body < 0) {
        g_building.valid = false;
    } else {
        const auto b = static_cast<size_t>(body);
        E::Vec3 total{applied[0], applied[1], applied[2]};
        if (E::UnderUpperPos(b)) {
            const float k = g_shift_reach;
            const float kl = g_lean_reach * E::LeanReach(b);
            total = {total.x + k * g_building_shift[0] + kl * g_building_lean[0],
                     total.y + k * g_building_shift[1] + kl * g_building_lean[1],
                     total.z + k * g_building_shift[2] + kl * g_building_lean[2]};
        }
        g_building.baseline[bone] = total;
        g_building.excluded[bone] = (E::IsLeftArm(b) && g_building_ik[0] > 0.01f) ||
                                    (E::IsRightArm(b) && g_building_ik[1] > 0.01f);
        if (b == E::Head01) {
            g_building.head = total;
        }
    }
    if (bone + 1 == E::BodyCount && g_building.valid) {
        std::scoped_lock lock{g_history_mutex};
        g_history[g_history_next] = g_building;
        g_history_next = (g_history_next + 1) % g_history.size();
    }
}

void ReadPoseShift() {
    g_pose_shift = {};
    g_pose_lean = {};
    g_pose_ik = {};
    VR::Body::BodyStatus status{};
    if (VR::Body::LatestBodyStatus(status) &&
        VR::Body::SteadyNowNs() - status.steady_ns <= StatusMaxAgeNs && status.driven) {
        g_pose_shift = status.upper_shift;
        g_pose_lean = status.upper_lean;
        g_pose_ik = status.drawn_weight; // the arm as drawn
    }
}

void PS4_SYSV_ABI OnBodyTarget(u8* frame, u64 bone) {
    if (bone >= E::BodyCount) {
        return;
    }
    if (bone == 0) {
        ReadPoseShift();
    }
    Published p;
    bool have =
        g_published.Load(p, 4) && p.ragdoll == Read<u64>(frame + FrameRagdoll) && p.ragdoll != 0;
    // An area load can free the player's ragdoll and hand its address to another character
    // before the next render tick refreshes the pointer: never move someone else's bodies.
    have =
        have && Read<u64>(reinterpret_cast<const u8*>(p.ragdoll) + OffsetRagdollOwner) == p.player;
    std::array<float, 3> applied{};
    if (have) {
        auto* const target = reinterpret_cast<float*>(frame + FrameTarget);
        applied = p.offset[bone];
        // The offset was computed against the render tick's shift and lean; this pose has its
        // own.
        const float k = p.shift_k[bone];
        const float kl = p.lean_k[bone];
        std::array<float, 3> shift = g_pose_shift;
        shift[1] = E::FollowedShiftY(shift[1], p.unit);
        for (int i = 0; i < 3; ++i) {
            applied[i] -= k * (shift[i] - p.shift[i]) + kl * (g_pose_lean[i] - p.lean[i]);
        }
        target[0] += applied[0];
        target[1] += applied[1];
        target[2] += applied[2];
        if (g_log > 0) {
            for (int k = 0; k < 3; ++k) {
                g_applied.target[bone][k].store(target[k], std::memory_order_relaxed);
            }
            g_applied.count.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (g_hit_hooks) {
        RecordApplied(bone, applied, p, have);
    }
}

// Gated stub: a compare per body for every ragdoll; the host call only for the main player's.
void* EmitStub(Xbyak::CodeGenerator& c, u8* site) {
    using namespace Xbyak::util;
    c.align(16);
    void* entry = const_cast<u8*>(c.getCurr());
    Xbyak::Label skip;
    c.lea(rsp, ptr[rsp - 128]); // keep the guest red zone intact
    c.pushf();
    c.push(rax);
    c.mov(rax, reinterpret_cast<u64>(&g_main_ragdoll));
    c.mov(rax, ptr[rax]);
    c.cmp(rax, ptr[rbp + FrameRagdoll]);
    c.pop(rax); // pop keeps the flags
    c.jne(skip, Xbyak::CodeGenerator::T_NEAR);

    for (const auto& reg : {rax, rcx, rdx, rbx, rsi, rdi, r8, r9, r10, r11, rbp}) {
        c.push(reg);
    }
    c.mov(rdi, rbp); // the guest frame
    c.mov(rsi, r13); // the ragdoll bone index
    c.mov(rbp, rsp);
    c.and_(rsp, -32);
    c.sub(rsp, 32 * 16);
    for (int i = 0; i < 16; ++i) {
        c.vmovdqu(ptr[rsp + 32 * i], Xbyak::Ymm(i));
    }
    c.mov(rax, reinterpret_cast<u64>(&OnBodyTarget));
    c.call(rax);
    for (int i = 0; i < 16; ++i) {
        c.vmovdqu(Xbyak::Ymm(i), ptr[rsp + 32 * i]);
    }
    c.mov(rsp, rbp);
    for (const auto& reg : {rbp, r11, r10, r9, r8, rdi, rsi, rbx, rdx, rcx, rax}) {
        c.pop(reg);
    }

    c.L(skip);
    c.popf();
    c.lea(rsp, ptr[rsp + 128]);
    for (const u8 byte : TargetSiteSig) {
        c.db(byte);
    }
    // jmp qword [rip+0]; dq resume
    c.db(0xFF);
    c.db(0x25);
    c.dd(0);
    c.dq(reinterpret_cast<u64>(site + TargetSiteSig.size()));
    c.ready();
    return entry;
}

// The main player's ragdoll (and the player: `player`), or nullptr.
u8* MainRagdoll(u8** player_out = nullptr) {
    u8* const world = Read<u8*>(g_base + WorldChrManRva);
    if (!Plausible(world)) {
        return nullptr;
    }
    u8* const player = Read<u8*>(world + OffsetMainPlayer);
    if (!Plausible(player)) {
        return nullptr;
    }
    u8* const ctrl = Read<u8*>(player + OffsetChrCtrl);
    if (!Plausible(ctrl)) {
        return nullptr;
    }
    u8* const ragdoll = Read<u8*>(ctrl + OffsetRagdoll);
    if (!Plausible(ragdoll) || Read<u8*>(ragdoll) != g_base + RagdollVtableRva ||
        Read<u8*>(ragdoll + OffsetRagdollOwner) != player) {
        return nullptr;
    }
    if (player_out != nullptr) {
        *player_out = player;
    }
    return ragdoll;
}

// The character root (world position the hurtboxes are keyframed around: ChrCtrl's W rows).
bool ReadRoot(float out[3]) {
    u8* const world = Read<u8*>(g_base + WorldChrManRva);
    u8* const player = Plausible(world) ? Read<u8*>(world + OffsetMainPlayer) : nullptr;
    u8* const ctrl = Plausible(player) ? Read<u8*>(player + OffsetChrCtrl) : nullptr;
    if (!Plausible(ctrl) || Read<u8*>(ctrl + OffsetCtrlOwner) != player) {
        return false;
    }
    float rows[12];
    std::memcpy(rows, ctrl + OffsetCtrlWorld, sizeof(rows));
    out[0] = rows[3];
    out[1] = rows[7];
    out[2] = rows[11];
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

// The physics world wrapper the game's queries take (FrpgPhysWorld; hknpWorld at +8), or null.
u8* PhysWorldWrapper() {
    u8* const mgr = Read<u8*>(g_base + PhysWorldRva);
    u8* const world = Plausible(mgr) ? Read<u8*>(mgr + OffsetPhysWorldWrapper) : nullptr;
    return Plausible(world) && Plausible(Read<u8*>(world + 0x08)) ? world : nullptr;
}

// One cast of a sphere of `radius` (game units) from `from` along the horizontal unit (ux, uz)
// for `length`: the free travel before it touches the map, `length` when nothing is in the way,
// -2 when it starts inside geometry (no information), -1 on no world.
float CastFree(u8* world, const float from_xyz[3], float ux, float uz, float length, float radius) {
    alignas(16) float from[4] = {from_xyz[0], from_xyz[1], from_xyz[2], 1.0f};
    if (g_wall_method == WallMethod::Sphere) {
        alignas(16) float delta[4] = {ux * length, 0.0f, uz * length, 0.0f};
        alignas(16) float hit[4]{};
        alignas(16) float normal[4]{};
        alignas(16) float fraction[4] = {1.0f};
        if (g_sphere_cast(world, LayerRagdollLandscape, from, delta, hit, normal, fraction,
                          radius) == 0) {
            return length;
        }
        if (!(fraction[0] > 1e-3f)) {
            return -2.0f;
        }
        return std::clamp(fraction[0], 0.0f, 1.0f) * length;
    }
    // The ray: as long as the sphere's travel plus its radius; free = hit distance - radius.
    const float reach = length + radius;
    alignas(16) float to[4] = {from[0] + ux * reach, from[1], from[2] + uz * reach, 1.0f};
    alignas(16) float hit[4]{};
    alignas(16) float normal[4]{};
    if (g_ragdoll_ray(nullptr, from, to, hit, normal) == 0) {
        return length;
    }
    const float distance = std::hypot(hit[0] - from[0], hit[2] - from[2]);
    if (distance < 1e-3f) {
        return -2.0f;
    }
    return std::max(distance - radius, 0.0f);
}

// Free travel (m) of the torso from the root along (ux, uz) up to `want_m`; -1 = clear (or no
// usable cast). `scale` = world_scale (game units per metre).
float WallFree(const float root[3], float ux, float uz, float want_m, float scale) {
    u8* const world = PhysWorldWrapper();
    if (world == nullptr) {
        ++g_wall.no_world;
        return -1.0f;
    }
    ++g_wall.probes;
    float free_m = -1.0f;
    for (const WallCast& w : WallCasts) {
        const float from[3] = {root[0], root[1] + w.height_m * scale, root[2]};
        const float d = CastFree(world, from, ux, uz, want_m * scale, w.radius_m * scale);
        ++g_wall.casts;
        g_wall.inside += d == -2.0f ? 1 : 0;
        if (d < 0.0f || d >= want_m * scale - 1e-5f) {
            continue; // inside (no information) or clear
        }
        const float m = d / scale;
        free_m = free_m < 0.0f ? m : std::min(free_m, m);
    }
    if (free_m >= 0.0f) {
        ++g_wall.hits;
        g_wall.min_free = std::min(g_wall.min_free, free_m);
    }
    return free_m;
}

// Maps the ragdoll skeleton's bones to E::Body by name. False (with one log line) when the
// skeleton is not the hunter's 16-body ragdoll.
bool ResolveBones(u8* ragdoll) {
    u8* const system = Read<u8*>(ragdoll + OffsetRagdollSystem);
    u8* const skeleton = Plausible(system) ? Read<u8*>(system + OffsetSystemSkeleton) : nullptr;
    u8* const bones = Plausible(skeleton) ? Read<u8*>(skeleton + OffsetSkeletonBones) : nullptr;
    const s32 count = Plausible(skeleton) ? Read<s32>(skeleton + OffsetSkeletonBoneCount) : -1;
    if (!Plausible(bones) || count != static_cast<s32>(E::BodyCount)) {
        LOG_WARNING(Core,
                    "VR evade: ragdoll {} skeleton {} has {} bones (expected 16); evasion off",
                    fmt::ptr(ragdoll), fmt::ptr(skeleton), count);
        return false;
    }
    g_bone_of_body.fill(-1);
    g_body_of_bone.fill(-1);
    std::string order;
    for (s32 bone = 0; bone < count; ++bone) {
        auto* const name = reinterpret_cast<const char*>(
            Read<u64>(bones + static_cast<u32>(bone) * BoneStride) & ~u64{1});
        if (!Plausible(reinterpret_cast<const u8*>(name), 1)) {
            LOG_WARNING(Core, "VR evade: ragdoll bone {} has no name; evasion off", bone);
            return false;
        }
        std::string text(name, strnlen(name, 64));
        order += (bone == 0 ? "" : " ") + text;
        // The game's ragdoll skeleton prefixes the HKX body names ("Ragdoll_Pelvis01").
        if (constexpr std::string_view Prefix = "Ragdoll_"; text.starts_with(Prefix)) {
            text.erase(0, Prefix.size());
        }
        for (size_t b = 0; b < E::BodyCount; ++b) {
            if (text == E::BodyNames[b]) {
                g_bone_of_body[b] = bone;
                g_body_of_bone[static_cast<size_t>(bone)] = static_cast<s32>(b);
            }
        }
    }
    for (size_t b = 0; b < E::BodyCount; ++b) {
        if (g_bone_of_body[b] < 0) {
            LOG_WARNING(Core, "VR evade: ragdoll bone {} not found (bones: {}); evasion off",
                        E::BodyNames[b], order);
            return false;
        }
    }
    LOG_INFO(Core, "VR evade: ragdoll {} bones: {}", fmt::ptr(ragdoll), order);
    return true;
}

// The counted duck (evasion_duck_m) stays within the camera's own room below the recentered head
// (core/vr/head_room.h: positional_limit_down_m), as the lean stays within positional_limit_m.
float DuckCap(const VR::Settings& s) {
    // The camera's standing eye: the hunter's own + your height's offset (player_body.h).
    return VR::HeadRoom::DuckCap(s.evasion_duck_m, s.positional_limit_m, s.positional_limit_down_m,
                                 VR::PlayerBody::StandingEyeM(VR::HeightEyeOffsetM(),
                                                              s.fp_eye_height_m));
}

// The hknp body positions the hurtboxes are at now (logs).
bool ReadBodyPosition(u8* ragdoll, size_t bone, std::array<float, 3>& out) {
    u8* const system = Read<u8*>(ragdoll + OffsetRagdollSystem);
    if (!Plausible(system) || Read<s32>(system + OffsetSystemBodyCount) != E::BodyCount) {
        return false;
    }
    u8* const ids = Read<u8*>(system + OffsetSystemBodyIds);
    u8* const world = Read<u8*>(g_base + PhysWorldRva);
    u8* const a = Plausible(world) ? Read<u8*>(world + 0x28) : nullptr;
    u8* const b = Plausible(a) ? Read<u8*>(a + 0x08) : nullptr;
    u8* const bodies = Plausible(b) ? Read<u8*>(b + 0x20) : nullptr;
    if (!Plausible(ids, 4) || !Plausible(bodies, 16)) {
        return false;
    }
    const s32 id = Read<s32>(ids + bone * 4);
    if (id < 0 || id > 0x100000) {
        return false;
    }
    std::memcpy(out.data(), bodies + static_cast<u64>(id) * BodyStride + BodyTranslation,
                sizeof(float) * 3);
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

// A ragdoll body's capsule in world space (game units), from the hknp body: transform (rotation
// columns +0x00/+0x10/+0x20, translation +0x30) and shape (+0x48: hknpCapsuleShape, radius +0x14,
// a +0x40, b +0x50, body-local).
struct Capsule {
    std::array<float, 3> a{}, b{};
    float r{};
    std::array<float, 3> origin{}; // the body's translation
};
bool ReadCapsule(u8* ragdoll, size_t bone, Capsule& out) {
    u8* const system = Read<u8*>(ragdoll + OffsetRagdollSystem);
    if (!Plausible(system) || Read<s32>(system + OffsetSystemBodyCount) != E::BodyCount) {
        return false;
    }
    u8* const ids = Read<u8*>(system + OffsetSystemBodyIds);
    u8* const world = Read<u8*>(g_base + PhysWorldRva);
    u8* const a = Plausible(world) ? Read<u8*>(world + 0x28) : nullptr;
    u8* const b = Plausible(a) ? Read<u8*>(a + 0x08) : nullptr;
    u8* const bodies = Plausible(b) ? Read<u8*>(b + 0x20) : nullptr;
    if (!Plausible(ids, 4) || !Plausible(bodies, 16)) {
        return false;
    }
    const s32 id = Read<s32>(ids + bone * 4);
    if (id < 0 || id > 0x100000) {
        return false;
    }
    const u8* const body = bodies + static_cast<u64>(id) * BodyStride;
    float m[16];
    std::memcpy(m, body, sizeof(m));
    u8* const shape = Read<u8*>(body + BodyShape);
    if (!Plausible(shape) || Read<u8*>(shape) != g_base + CapsuleVtableRva) {
        return false;
    }
    float la[4], lb[4];
    std::memcpy(la, shape + CapsuleA, sizeof(la));
    std::memcpy(lb, shape + CapsuleB, sizeof(lb));
    out.r = Read<float>(shape + ShapeRadius);
    for (int k = 0; k < 3; ++k) {
        out.origin[k] = m[12 + k];
        out.a[k] = m[k] * la[0] + m[4 + k] * la[1] + m[8 + k] * la[2] + m[12 + k];
        out.b[k] = m[k] * lb[0] + m[4 + k] * lb[1] + m[8 + k] * lb[2] + m[12 + k];
    }
    for (int k = 0; k < 3; ++k) {
        if (!std::isfinite(out.a[k]) || !std::isfinite(out.b[k])) {
            return false;
        }
    }
    return std::isfinite(out.r) && out.r > 0.0f && out.r < 1.0f;
}

// Once per ragdoll (logs): each hurtbox capsule's radius and length (the HKX has Pelvis01 0.123,
// Spine02 0.144, Head01 0.094 ...), and which way the body rotation is stored: a capsule's far
// end should meet the child body's origin (thigh -> calf -> foot, upper arm -> forearm).
void LogCapsules(u8* ragdoll) {
    std::string text;
    int bad = 0;
    for (size_t b = 0; b < E::BodyCount; ++b) {
        Capsule c;
        if (!ReadCapsule(ragdoll, static_cast<size_t>(g_bone_of_body[b]), c)) {
            ++bad;
            text += fmt::format(" {} -", E::BodyNames[b]);
            continue;
        }
        text += fmt::format(" {} r {:.3f} len {:.3f}", E::BodyNames[b], c.r,
                            std::hypot(c.a[0] - c.b[0], c.a[1] - c.b[1], c.a[2] - c.b[2]));
    }
    std::string joints;
    for (const auto& [parent, child] :
         {std::pair{E::L_Thigh01, E::L_Calf01}, std::pair{E::L_Calf01, E::L_Foot01},
          std::pair{E::R_UpperArm01, E::R_Forearm01}}) {
        Capsule p, c;
        if (!ReadCapsule(ragdoll, static_cast<size_t>(g_bone_of_body[parent]), p) ||
            !ReadCapsule(ragdoll, static_cast<size_t>(g_bone_of_body[child]), c)) {
            continue;
        }
        const auto dist = [&](const std::array<float, 3>& e) {
            return std::hypot(e[0] - c.origin[0], e[1] - c.origin[1], e[2] - c.origin[2]);
        };
        joints += fmt::format(" {}->{} {:.3f} m", E::BodyNames[parent], E::BodyNames[child],
                              std::min(dist(p.a), dist(p.b)) - p.r);
    }
    LOG_INFO(Core,
             "VR evade: hurtbox capsules{} ({} unreadable); far end to child origin minus "
             "radius (about 0 = rotation columns read right):{}",
             text, bad, joints);
}

// Independent check of the clamp (logs): the skin gap of the Spine02 and Head01 hurtboxes, from
// their capsules where they are now (both ends), to the map along the last probe's direction.
std::string GapText(float scale) {
    u8* const world = PhysWorldWrapper();
    if (world == nullptr || g_ragdoll == nullptr || !g_map_ok) {
        return {};
    }
    std::string text = " gap";
    for (const auto& [body, radius] : GapBodies) {
        const auto bone = static_cast<size_t>(g_bone_of_body[body]);
        Capsule c;
        std::array<std::array<float, 3>, 2> ends{};
        float r = radius * scale;
        size_t count = 2;
        if (ReadCapsule(g_ragdoll, bone, c)) {
            ends = {c.a, c.b};
            r = c.r;
        } else if (ReadBodyPosition(g_ragdoll, bone, ends[0])) {
            count = 1;
        } else {
            text += fmt::format(" {} -", E::BodyNames[body]);
            continue;
        }
        float gap = GapCastLength * scale;
        bool inside = false;
        for (size_t i = 0; i < count; ++i) {
            const float d = CastFree(world, ends[i].data(), g_wall_dir[0], g_wall_dir[1],
                                     GapCastLength * scale, r);
            inside |= d == -2.0f;
            if (d >= 0.0f) {
                gap = std::min(gap, d);
            }
        }
        text += fmt::format(" {} {}", E::BodyNames[body],
                            inside ? std::string{"INSIDE"}
                            : gap >= GapCastLength * scale - 1e-5f
                                ? std::string{">60 cm"}
                                : fmt::format("{:.1f} cm", 100.0f * gap / scale));
    }
    return text;
}

// Native collision facts only; profile conversion, delayed-head compensation and
// root/closed-loop response selection belong to the DLL's captured evasion frame.
void ReadClosedLoop(float ux, float uz, float reach_m, float scale, uint32_t query_mask,
                    PiEvasionWallFacts& facts) {
    u8* const world = PhysWorldWrapper();
    if (world == nullptr || g_ragdoll == nullptr || !g_map_ok)
        return;
    const float reach = reach_m * scale;
    for (size_t body = 0; body < E::BodyCount; ++body) {
        if (!(query_mask & (1u << body)))
            continue;
        Capsule c;
        if (!ReadCapsule(g_ragdoll, static_cast<size_t>(g_bone_of_body[body]), c))
            continue;
        facts.body_mask |= 1u << static_cast<uint32_t>(body);
        const std::array<std::array<float, 3>, 2> ends{c.a, c.b};
        for (size_t i = 0; i < ends.size(); ++i) {
            facts.ends[body][i] = CastFree(world, ends[i].data(), ux, uz, reach, c.r);
            ++g_wall.casts;
        }
    }
}
const char* EvasionReason(uint32_t reason, const char* block) {
    switch (reason) {
    case PI_ER_ON:
        return "on";
    case PI_ER_RAGDOLL:
        return "no ragdoll";
    case PI_ER_FIRST_PERSON:
        return "not first person";
    case PI_ER_NECK:
        return "no neck";
    case PI_ER_BODY:
        return "body not driven";
    case PI_ER_BLOCKED:
        return block ? block : "body gate";
    default:
        return "interaction DLL fault";
    }
}

void Log(const E::Output& out, const E::Input& in, const char* reason, float scale, s64 now) {
    const u32 mark = VR::Body::MarkCount();
    if (mark != g_last_mark) {
        g_last_mark = mark;
        g_mark_until_ns = now + 2'000'000'000;
    }
    const bool every = g_log >= 3 || (g_log >= 2 && now < g_mark_until_ns);
    const bool edge = std::strcmp(reason, g_last_reason) != 0;
    if (!every && !edge && now - g_last_log_ns < 1'000'000'000) {
        return;
    }
    g_last_log_ns = now;
    g_last_reason = reason;
    std::string bodies;
    if (g_ragdoll != nullptr && g_map_ok) {
        for (const E::Body b : {E::Head01, E::Spine02, E::Pelvis01, E::L_Foot01}) {
            const auto bone = static_cast<size_t>(g_bone_of_body[b]);
            std::array<float, 3> now_pos{};
            const bool have = ReadBodyPosition(g_ragdoll, bone, now_pos);
            const float tx = g_applied.target[bone][0].load(std::memory_order_relaxed);
            const float ty = g_applied.target[bone][1].load(std::memory_order_relaxed);
            const float tz = g_applied.target[bone][2].load(std::memory_order_relaxed);
            bodies +=
                fmt::format(" | {} off ({:+.3f},{:+.3f},{:+.3f}) target ({:.3f},{:.3f},{:.3f})"
                            " body {}",
                            E::BodyNames[b], out.offset[b].x * scale, out.offset[b].y * scale,
                            out.offset[b].z * scale, tx, ty, tz,
                            have ? fmt::format("({:.3f},{:.3f},{:.3f}) err {:.1f} cm", now_pos[0],
                                               now_pos[1], now_pos[2],
                                               100.0f * std::hypot(now_pos[0] - tx, now_pos[1] - ty,
                                                                   now_pos[2] - tz))
                                 : std::string{"-"});
        }
    }
    const float gain = std::max(VR::GetSettings().evasion_gain, 0.0f);
    std::string wall;
    if (g_wall_method != WallMethod::Off && VR::GetSettings().evasion_wall) {
        wall = fmt::format(" | wall free {} limit {} head {:.3f} of {:.3f}{}{}",
                           in.wall_m >= 0.0f ? fmt::format("{:.3f}", in.wall_m) : "-",
                           out.wall_limit_m >= 0.0f ? fmt::format("{:.3f}", out.wall_limit_m) : "-",
                           gain * out.lean_m, gain * out.lean_want_m,
                           out.wall_limited ? " LIMITED" : "",
                           out.wall_limited || g_log >= 2 ? GapText(scale) : std::string{});
    }
    LOG_INFO(Core,
             "VR evade: frame {} t {:.3f} {} w={:.2f} neck ({:+.3f},{:+.3f},{:+.3f}) counted "
             "({:+.3f},{:+.3f},{:+.3f}) lean {:.3f} duck {:.3f} settled {:.3f} held {:.2f} s{}{} "
             "shift ({:+.3f},{:+.3f},{:+.3f}) body lean ({:+.3f},{:+.3f},{:+.3f}) ik R {:.2f} L "
             "{:.2f} cw {:.2f}{} applied {}{}{}",
             g_frame, in.t, reason, out.weight, in.neck.x, in.neck.y, in.neck.z, out.counted.x,
             out.counted.y, out.counted.z, out.lean_m, out.duck_m, out.settled_m, out.held_s,
             out.hold_settling ? " SETTLING" : "", out.glitch ? " GLITCH" : "", in.pose_shift.x,
             in.pose_shift.y, in.pose_shift.z, in.pose_lean.x, in.pose_lean.y, in.pose_lean.z,
             in.ik_right, in.ik_left, out.comp_weight, in.look_only ? " LOOK-ONLY" : "",
             g_applied.count.load(std::memory_order_relaxed), wall, bodies);
    if (g_wall.probes > 0 && now - g_last_wall_log_ns >= 1'000'000'000) {
        g_last_wall_log_ns = now;
        LOG_INFO(Core,
                 "VR evade: wall casts: probes {} casts {} hits {} inside {} limited {} (by the "
                 "hurtboxes' own casts {}) no world {} min free {} avg {:.1f} us max {:.1f} us",
                 g_wall.probes, g_wall.casts, g_wall.hits, g_wall.inside, g_wall.limited,
                 g_wall.closer, g_wall.no_world,
                 g_wall.min_free < 1e8f ? fmt::format("{:.3f} m", g_wall.min_free) : "-",
                 1e-3 * static_cast<double>(g_wall.ns) / static_cast<double>(g_wall.probes),
                 1e-3 * static_cast<double>(g_wall.max_ns));
    }
}

// ---- Hit log -------------------------------------------------------------------------------

E::Vec3 ReadVec(const u8* address) {
    float v[3];
    std::memcpy(v, address, sizeof(v));
    return {v[0], v[1], v[2]};
}
E::Vec3 ToVec(const std::array<float, 3>& v) {
    return {v[0], v[1], v[2]};
}

// One attack cast against the hurtboxes (H1), completed by the game's hits on the player (H2).
struct PendingCast {
    bool valid{false};
    u64 entry{};
    u64 owner{};
    s64 ns{};
    float r{};
    float len{};
    float travel{};
    bool s_hit{false};
    bool v_hit{false};
    bool v_known{false};
    float s_clear{1e9f};
    float v_clear{1e9f};
    s8 s_body{-1};
    s8 v_body{-1};
    E::Vec3 v_point{}; // the attack's closest point to the vanilla body
    E::Vec3 spine{};   // the leaned Spine02's centre
    E::Vec3 head{};    // the lean on Head01 at the keyframe the bodies are at
    s64 history_age{};
    std::array<bool, E::BodyCount> excluded{};
    std::array<s8, E::BodyCount> body_of_bone{};
    bool g{false};         // the game hit the player (any body)
    bool g_counted{false}; // ... a body the model covers (not an arm on IK)
    s32 g_part{-1};
    E::Vec3 g_pos{};
    float aim_root{-1.0f};  // level 2: degrees between the attacker's facing and the root / the
    float aim_spine{-1.0f}; // leaned Spine02
};

// An attack (an AttackHitIns entry) over its casts; classified when it stops casting.
struct AttackTrack {
    u64 entry{};
    u64 owner{};
    s64 first_ns{};
    s64 last_ns{};
    u32 casts{};
    bool any_s{false};
    bool any_v{false};
    bool any_g{false};
    bool v_unknown{false};
    float min_s{1e9f};
    float min_v{1e9f};
    s8 min_s_body{-1};
    s8 min_v_body{-1};
    s32 g_part{-1};
    E::Vec3 head{}; // the lean at the first contact (vanilla or game), else at the closest pass
    bool contact{false};
    float r{};
    float len{};
    bool whoosh{false};
    s32 side{-1}; // where a blow through the vanilla body passed the leaned one (C0000 side)
    float aim_root{-1.0f};
    float aim_spine{-1.0f};
};

struct HitStats {
    u64 casts{};    // attack casts by others
    u64 tested{};   // ... within reach of the player
    u64 unknown{};  // ... with a shape the log does not read
    u64 game{};     // casts the game hit the player with
    u64 compared{}; // casts where S and G were compared (the model's agreement)
    u64 agree{};
    u64 fixed{};     // fixed-target hits (no cast: cannot be leaned out of)
    u64 unmatched{}; // a hit on the player with no cast before it
    u64 no_history{};
    u64 evaded{};
    u64 traded{};
    u64 both{};
    u64 missed{};  // neither (an attack that came close)
    u64 ignored{}; // the leaned body was hit, the game did not count it (i-frames, grabs)
    u64 s_only{};  // casts: the model hit, the game did not
    u64 g_only{};  // casts: the game hit, the model did not
    u64 whooshes{};
    s64 age_ns{};
    u64 age_n{};
    s64 test_ns{}; // time spent testing the casts near the player
    s64 test_max_ns{};
};

std::mutex g_hit_mutex; // H1 / H2 (DamageStep) and the render tick
PendingCast g_pending;
std::array<AttackTrack, 16> g_tracks{};
HitStats g_hits;
u64 g_hits_logged_casts = ~u64{0};
s64 g_last_whoosh_ns = 0;
s64 g_last_hit_log_ns = 0;
int g_unknown_logged = 0;
bool g_hit_first_logged = false;

const char* BodyName(s8 body) {
    return body >= 0 && body < static_cast<s8>(E::BodyCount)
               ? E::BodyNames[static_cast<size_t>(body)]
               : "-";
}

std::string Cm(float m) {
    return m > 1e8f ? std::string{"-"} : fmt::format("{:+.0f} cm", 100.0f * m);
}

// The keyframe the bodies are at now: the one before the newest game frame's (H1 runs after this
// frame's keyframe and before the physics step that moves the bodies onto it).
bool PickHistory(u64 ragdoll, s64 now, AppliedFrame& out) {
    std::scoped_lock lock{g_history_mutex};
    const AppliedFrame* newest = nullptr;
    for (const AppliedFrame& f : g_history) {
        if (f.valid && f.ragdoll == ragdoll && f.ns <= now &&
            (newest == nullptr || f.ns > newest->ns)) {
            newest = &f;
        }
    }
    if (newest == nullptr) {
        return false;
    }
    const AppliedFrame* before = nullptr;
    for (const AppliedFrame& f : g_history) {
        if (f.valid && f.ragdoll == ragdoll && newest->ns - f.ns > HistoryGapNs &&
            (before == nullptr || f.ns > before->ns)) {
            before = &f;
        }
    }
    if (before == nullptr) {
        return false;
    }
    out = *before;
    return true;
}

AttackTrack& TrackFor(u64 entry, s64 now) {
    AttackTrack* oldest = &g_tracks[0];
    for (AttackTrack& t : g_tracks) {
        if (t.entry == entry) {
            return t;
        }
    }
    for (AttackTrack& t : g_tracks) {
        if (t.entry == 0) {
            oldest = &t;
            break;
        }
        if (t.last_ns < oldest->last_ns) {
            oldest = &t;
        }
    }
    *oldest = {};
    oldest->entry = entry;
    oldest->first_ns = now;
    return *oldest;
}

// g_hit_mutex held. The previous cast is complete: its hits came right after it.
void FinalizePendingLocked(s64 now) {
    if (!g_pending.valid) {
        return;
    }
    PendingCast& c = g_pending;
    c.valid = false;
    if (c.g) {
        ++g_hits.game;
    }
    ++g_hits.compared;
    if (c.s_hit == c.g_counted) {
        ++g_hits.agree;
    } else if (c.s_hit) {
        ++g_hits.s_only;
    } else {
        ++g_hits.g_only;
    }
    if (c.v_known) {
        g_hits.age_ns += c.history_age;
        ++g_hits.age_n;
    } else {
        ++g_hits.no_history;
    }
    AttackTrack& t = TrackFor(c.entry, c.ns);
    t.owner = c.owner;
    t.last_ns = c.ns;
    ++t.casts;
    t.r = c.r;
    t.len = c.len;
    t.any_s |= c.s_hit;
    t.any_v |= c.v_hit;
    t.any_g |= c.g;
    t.v_unknown |= !c.v_known;
    if (c.s_clear < t.min_s) {
        t.min_s = c.s_clear;
        t.min_s_body = c.s_body;
    }
    if (c.v_known && c.v_clear < t.min_v) {
        t.min_v = c.v_clear;
        t.min_v_body = c.v_body;
        if (!t.contact) {
            t.head = c.head;
            t.aim_root = c.aim_root;
            t.aim_spine = c.aim_spine;
        }
    }
    if (!t.contact && (c.v_hit || c.g)) {
        t.contact = true;
        t.head = c.head;
        t.aim_root = c.aim_root;
        t.aim_spine = c.aim_spine;
    }
    if (c.g && t.g_part < 0) {
        t.g_part = c.g_part;
    }
    if (g_hitlog >= 2) {
        const char* part = c.g_part >= 0 && c.g_part < static_cast<s32>(E::BodyCount)
                               ? BodyName(c.body_of_bone[static_cast<size_t>(c.g_part)])
                               : "?";
        LOG_INFO(Core,
                 "VR evade cast: attack {:#x} r {:.2f} len {:.2f} sweep {:.2f} | game {} | "
                 "leaned {} ({}) {} | vanilla {} ({}) {}{} | lean head ({:+.2f},{:+.2f}) "
                 "history {:.1f} ms{}",
                 c.entry, c.r, c.len, c.travel,
                 c.g ? fmt::format("HIT {} at ({:.2f},{:.2f},{:.2f})", part, c.g_pos.x, c.g_pos.y,
                                   c.g_pos.z)
                     : std::string{"miss"},
                 Cm(c.s_clear), BodyName(c.s_body), c.s_hit ? "HIT" : "miss", Cm(c.v_clear),
                 BodyName(c.v_body),
                 !c.v_known ? "?"
                 : c.v_hit  ? "HIT"
                            : "miss",
                 c.s_hit != c.g_counted ? " MISMATCH" : "", c.head.x, c.head.z,
                 1e-6 * static_cast<double>(c.history_age),
                 c.aim_root >= 0.0f ? fmt::format(" | aim root {:.0f} deg leaned spine {:.0f} deg",
                                                  c.aim_root, c.aim_spine)
                                    : std::string{});
    }
    // The side a blow that went through the vanilla body passed you on (the whoosh, played once
    // the attack is over and never touched you): game right of the view is (cos y, 0, -sin y).
    if (c.v_hit && t.side < 0) {
        Published p;
        const float yaw = g_published.Load(p, 4) ? p.view_yaw : 0.0f;
        const float side =
            (c.v_point.x - c.spine.x) * std::cos(yaw) - (c.v_point.z - c.spine.z) * std::sin(yaw);
        t.side = side > 0.0f ? 1 : 0;
    }
}

// H1: an attack's cast (DamageStep).
void PS4_SYSV_ABI OnAttackCast(u8* frame, u8* owner_ragdoll, u8* wrapper) {
    const s64 now = VR::Body::SteadyNowNs();
    const u64 main = g_main_ragdoll.load(std::memory_order_acquire);
    std::scoped_lock lock{g_hit_mutex};
    FinalizePendingLocked(now);
    if (main == 0 || reinterpret_cast<u64>(owner_ragdoll) == main) {
        return; // the player's own attacks and bullets
    }
    ++g_hits.casts;
    Published p;
    if (!g_published.Load(p, 4) || p.ragdoll != main) {
        return;
    }
    // The attack's shape: a capsule (a, b) or a sphere, in the cast's orientation at `from`.
    u8* const shape = Plausible(wrapper) ? Read<u8*>(wrapper + WrapperShape) : nullptr;
    u8* const vtable = Plausible(shape) ? Read<u8*>(shape) : nullptr;
    E::Vec3 la{};
    E::Vec3 lb{};
    if (vtable == g_base + CapsuleVtableRva) {
        la = ReadVec(shape + CapsuleA);
        lb = ReadVec(shape + CapsuleB);
    } else if (vtable == g_base + SphereVtableRva) {
        la = ReadVec(shape + SphereCenter);
        lb = la;
    } else {
        ++g_hits.unknown;
        if (g_hitlog > 0 && g_unknown_logged < 3) {
            ++g_unknown_logged;
            LOG_INFO(Core,
                     "VR evade hits: attack shape with vtable {:#x} (not a capsule or sphere): "
                     "not tested",
                     vtable != nullptr ? static_cast<u64>(vtable - g_base) : 0);
        }
        return;
    }
    const float r = Read<float>(shape + ShapeRadius);
    if (!(r >= 0.0f && r < 20.0f)) {
        ++g_hits.unknown;
        return;
    }
    const E::Vec3 from = ReadVec(frame + FrameSweepFrom);
    const E::Vec3 to = ReadVec(frame + FrameSweepTo);
    const E::Vec3 c0 = ReadVec(frame + FrameCastOrient);
    const E::Vec3 c1 = ReadVec(frame + FrameCastOrient + 0x10);
    const E::Vec3 c2 = ReadVec(frame + FrameCastOrient + 0x20);
    const auto place = [&](const E::Vec3& v) { return from + c0 * v.x + c1 * v.y + c2 * v.z; };
    const E::Vec3 a = place(la);
    const E::Vec3 b = place(lb);
    const E::Vec3 sweep = to - from;
    float root[3];
    if (!ReadRoot(root)) {
        return;
    }
    // Near the player at all (the root's vertical, 2 m)?
    const E::Vec3 r0{root[0], root[1], root[2]};
    const E::Vec3 r1{root[0], root[1] + 2.0f, root[2]};
    const float near2 =
        std::min({E::SegSegDist2(a, b, r0, r1), E::SegSegDist2(a + sweep, b + sweep, r0, r1),
                  E::SegSegDist2(a, a + sweep, r0, r1), E::SegSegDist2(b, b + sweep, r0, r1)});
    if (!std::isfinite(near2) || std::sqrt(near2) - r > HitNearM) {
        return;
    }
    std::array<Capsule, E::BodyCount> caps{};
    for (size_t bone = 0; bone < E::BodyCount; ++bone) {
        if (!ReadCapsule(reinterpret_cast<u8*>(main), bone, caps[bone])) {
            ++g_hits.unknown;
            return;
        }
    }
    if (!g_hit_first_logged) {
        g_hit_first_logged = true;
        LOG_INFO(Core,
                 "VR evade hits: first attack near the player (radius {:.2f}, length {:.2f}, "
                 "sweep {:.2f}): tested on this thread",
                 r, std::sqrt(E::Dot(b - a, b - a)), std::sqrt(E::Dot(sweep, sweep)));
    }
    ++g_hits.tested;
    AppliedFrame base{};
    const bool have_base = PickHistory(main, now, base);
    PendingCast c{};
    c.valid = true;
    c.entry = Read<u64>(frame + FrameAttackEntry);
    c.owner = reinterpret_cast<u64>(owner_ragdoll);
    c.ns = now;
    c.r = r;
    c.len = std::sqrt(E::Dot(b - a, b - a));
    c.travel = std::sqrt(E::Dot(sweep, sweep));
    c.v_known = have_base;
    c.history_age = have_base ? now - base.ns : 0;
    c.head = base.head;
    c.body_of_bone = p.body_of_bone;
    for (size_t bone = 0; bone < E::BodyCount; ++bone) {
        const Capsule& cap = caps[bone];
        const s8 body = p.body_of_bone[bone];
        const bool excluded = have_base && base.excluded[bone];
        c.excluded[bone] = excluded;
        if (body == static_cast<s8>(E::Spine02)) {
            c.spine = (ToVec(cap.a) + ToVec(cap.b)) * 0.5f;
        }
        if (excluded) {
            continue;
        }
        const E::Sweep s = E::SweptCapsule(a, b, r, sweep, ToVec(cap.a), ToVec(cap.b), cap.r);
        if (s.clearance < c.s_clear) {
            c.s_clear = s.clearance;
            c.s_body = body;
        }
        if (have_base) {
            const E::Vec3 off = base.baseline[bone];
            const E::Sweep v =
                E::SweptCapsule(a, b, r, sweep, ToVec(cap.a) - off, ToVec(cap.b) - off, cap.r);
            if (v.clearance < c.v_clear) {
                c.v_clear = v.clearance;
                c.v_body = body;
                c.v_point = v.point;
            }
        }
    }
    c.s_hit = c.s_clear < 0.0f;
    c.v_hit = have_base && c.v_clear < 0.0f;
    const s64 spent = VR::Body::SteadyNowNs() - now;
    g_hits.test_ns += spent;
    g_hits.test_max_ns = std::max(g_hits.test_max_ns, spent);
    if (g_hitlog >= 2 && Plausible(owner_ragdoll)) {
        // Where the attacker faces (ChrCtrl's W rows; forward = -third column) against the
        // directions to the root and to the leaned chest: AI aiming at the root shows as a
        // smaller root angle while you lean.
        u8* const attacker = Read<u8*>(owner_ragdoll + OffsetRagdollOwner);
        u8* const ctrl = Plausible(attacker) ? Read<u8*>(attacker + OffsetChrCtrl) : nullptr;
        if (Plausible(ctrl) && Read<u8*>(ctrl + OffsetCtrlOwner) == attacker) {
            float w[12];
            std::memcpy(w, ctrl + OffsetCtrlWorld, sizeof(w));
            const float fx = -w[2];
            const float fz = -w[10];
            const auto angle = [&](float x, float z) {
                const float dx = x - w[3];
                const float dz = z - w[11];
                const float n = std::hypot(fx, fz) * std::hypot(dx, dz);
                return n > 1e-6f
                           ? std::acos(std::clamp((fx * dx + fz * dz) / n, -1.0f, 1.0f)) * 57.29578f
                           : -1.0f;
            };
            c.aim_root = angle(root[0], root[2]);
            c.aim_spine = angle(c.spine.x, c.spine.z);
        }
    }
    g_pending = c;
}

// H2: a cast hit a character's body (DamageStep), before the hit lists.
void PS4_SYSV_ABI OnChrHit(u8* entry, u8* target, const float* pos, s64 part) {
    Published p;
    if (!g_published.Load(p, 4) || p.player == 0 || reinterpret_cast<u64>(target) != p.player) {
        return;
    }
    std::scoped_lock lock{g_hit_mutex};
    const s32 part32 = static_cast<s32>(part);
    if (g_pending.valid && g_pending.entry == reinterpret_cast<u64>(entry)) {
        if (!g_pending.g) {
            g_pending.g_part = part32;
            if (pos != nullptr) {
                g_pending.g_pos = {pos[0], pos[1], pos[2]};
            }
        }
        g_pending.g = true;
        const bool covered = part32 >= 0 && part32 < static_cast<s32>(E::BodyCount) &&
                             !g_pending.excluded[static_cast<size_t>(part32)];
        g_pending.g_counted |= covered;
    } else if (Plausible(entry) && Read<u64>(entry + EntryFixedTarget) != 0) {
        ++g_hits.fixed;
    } else {
        ++g_hits.unmatched;
    }
}

// Render tick: attacks that stopped casting are classified (and logged); the 1 Hz summary.
void HitLogTick(s64 now) {
    std::scoped_lock lock{g_hit_mutex};
    if (g_pending.valid && now - g_pending.ns > 4'000'000) {
        FinalizePendingLocked(now);
    }
    for (AttackTrack& t : g_tracks) {
        // The whoosh: the blow went through where you would have been, the attack has stopped
        // casting and it never touched you.
        if (t.entry != 0 && t.any_v && !t.any_g && t.min_s >= -LeanedHitTolerance && !t.whoosh &&
            t.side >= 0 && now - t.last_ns >= WhooshDelayNs && VR::GetSettings().evasion_feedback &&
            now - g_last_whoosh_ns > WhooshCooldownNs) {
            VR::Combat::PushEvadeEvent(
                {VR::Combat::EvadeEventKind::NearMiss, t.side, t.min_v, now});
            t.whoosh = true;
            g_last_whoosh_ns = now;
            ++g_hits.whooshes;
        }
        if (t.entry == 0 || now - t.last_ns < AttackEndNs) {
            continue;
        }
        const E::HitClass cls = E::Classify(t.any_v, t.min_s < -LeanedHitTolerance, t.any_g);
        switch (cls) {
        case E::HitClass::Evaded:
            ++g_hits.evaded;
            break;
        case E::HitClass::Traded:
            ++g_hits.traded;
            break;
        case E::HitClass::Both:
            ++g_hits.both;
            break;
        case E::HitClass::Ignored:
            ++g_hits.ignored;
            break;
        case E::HitClass::None:
            ++g_hits.missed;
            break;
        }
        if (g_hitlog > 0 && (t.any_v || t.any_g || t.any_s || t.min_v < 0.5f || t.min_s < 0.5f)) {
            LOG_INFO(Core,
                     "VR evade hit: attack {:#x} by {:#x} r {:.2f} len {:.2f}, {} casts over {} "
                     "ms: {}{} | game {} | leaned min {} ({}) | vanilla min {} ({}){} | lean head "
                     "({:+.2f},{:+.2f}) m{}{}",
                     t.entry, t.owner, t.r, t.len, t.casts, (t.last_ns - t.first_ns) / 1'000'000,
                     E::HitClassName(cls), t.v_unknown ? " (some casts without history)" : "",
                     t.any_g ? fmt::format("HIT part {}", t.g_part) : std::string{"miss"},
                     Cm(t.min_s), BodyName(t.min_s_body), Cm(t.min_v), BodyName(t.min_v_body),
                     t.any_s != t.any_g ? " (model and game disagree)" : "", t.head.x, t.head.z,
                     t.whoosh ? " whoosh" : "",
                     t.aim_root >= 0.0f
                         ? fmt::format(" | aim root {:.0f} deg leaned spine {:.0f} deg", t.aim_root,
                                       t.aim_spine)
                         : std::string{});
        }
        t = {};
    }
    if (g_hitlog > 0 && now - g_last_hit_log_ns >= 1'000'000'000 &&
        g_hits.casts != g_hits_logged_casts) {
        g_last_hit_log_ns = now;
        g_hits_logged_casts = g_hits.casts;
        LOG_INFO(
            Core,
            "VR evade hits: casts {} tested {} | attacks evaded {} traded {} both {} missed {} "
            "ignored {} | game hits {} (casts), model agrees with the game on {} of {} (model "
            "only {}, game only {}) | fixed {} unmatched {} unknown {} no history {} | "
            "history age {:.1f} ms | whooshes {} | test avg {:.1f} us max {:.1f} us",
            g_hits.casts, g_hits.tested, g_hits.evaded, g_hits.traded, g_hits.both, g_hits.missed,
            g_hits.ignored, g_hits.game, g_hits.agree, g_hits.compared, g_hits.s_only,
            g_hits.g_only, g_hits.fixed, g_hits.unmatched, g_hits.unknown, g_hits.no_history,
            g_hits.age_n > 0
                ? 1e-6 * static_cast<double>(g_hits.age_ns) / static_cast<double>(g_hits.age_n)
                : 0.0,
            g_hits.whooshes,
            g_hits.tested > 0
                ? 1e-3 * static_cast<double>(g_hits.test_ns) / static_cast<double>(g_hits.tested)
                : 0.0,
            1e-3 * static_cast<double>(g_hits.test_max_ns));
    }
}

// The hit log's two read-only hooks (VR_EVADE_HITLOG, or evasion_feedback's near-miss whoosh).
void InstallHitLog(u8* module, u64 size) {
    const bool fn_ok =
        size > ChrHitFnRva + ChrHitFnSig.size() &&
        std::memcmp(module + AttackCastFnRva, AttackCastFnSig.data(), AttackCastFnSig.size()) == 0;
    const bool site_ok = fn_ok && std::memcmp(module + AttackCastSiteRva, AttackCastSiteSig.data(),
                                              AttackCastSiteSig.size()) == 0;
    const bool hit_ok =
        fn_ok && std::memcmp(module + ChrHitFnRva, ChrHitFnSig.data(), ChrHitFnSig.size()) == 0;
    if (!fn_ok || !site_ok || !hit_ok) {
        LOG_WARNING(Core,
                    "VR evade hits: not installed (cast fn {}, cast site {}, hit fn {}); live "
                    "bytes cast site: {} hit fn: {}",
                    fn_ok ? "ok" : "MISMATCH", site_ok ? "ok" : "MISMATCH",
                    hit_ok ? "ok" : "MISMATCH",
                    Hook::HexBytes(module + AttackCastSiteRva, AttackCastSiteSig.size()),
                    Hook::HexBytes(module + ChrHitFnRva, ChrHitFnSig.size()));
        return;
    }
    u8* const cast_site = module + AttackCastSiteRva;
    u8* const hit_site = module + ChrHitFnRva;
    if (!Hook::MakeWritable(cast_site, AttackCastSiteSig.size()) ||
        !Hook::MakeWritable(hit_site, ChrHitPrologue)) {
        LOG_ERROR(Core, "VR evade hits: could not make the sites writable; not installed");
        return;
    }
    void* const cast_stub = Hook::EmitHostCallStubWith(
        *g_code, reinterpret_cast<void*>(&OnAttackCast), AttackCastSiteSig.data(),
        AttackCastSiteSig.size(), reinterpret_cast<u64>(cast_site + AttackCastSiteSig.size()),
        [](Xbyak::CodeGenerator& c) {
            using namespace Xbyak::util;
            c.mov(rdi, rbp); // the cast's frame
            c.mov(rsi, r12); // the attacker's ragdoll
            c.mov(rdx, r13); // the attack's shape wrapper
        });
    void* const hit_stub = Hook::EmitHostCallStubWith(
        *g_code, reinterpret_cast<void*>(&OnChrHit), ChrHitFnSig.data(), ChrHitPrologue,
        reinterpret_cast<u64>(hit_site + ChrHitPrologue), [](Xbyak::CodeGenerator& c) {
            using namespace Xbyak::util;
            c.mov(rcx, r9); // the part (rdi entry, rsi target, rdx position stay)
        });
    g_code->ready();
    g_hit_hooks = true;
    Hook::WriteJump(cast_site, AttackCastSiteSig.size(), cast_stub);
    Hook::WriteJump(hit_site, ChrHitPrologue, hit_stub);
    LOG_INFO(Core, "VR evade hits: installed at {:#x} (attack cast) and {:#x} (chr hit){}{}",
             AttackCastSiteRva, ChrHitFnRva,
             g_hitlog > 0 ? fmt::format(" log {}", g_hitlog) : std::string{},
             VR::GetSettings().evasion_feedback ? " near-miss whoosh on" : "");
}

// The wall casts: the camera's sphere cast, else the ragdoll ray (VR_EVADE_WALL=sphere|ray|off).
void InstallWall(u8* module, u64 size) {
    const char* env = std::getenv("VR_EVADE_WALL");
    const std::string_view want = env != nullptr ? env : "sphere";
    if (want == "off") {
        LOG_INFO(Core, "VR evade: VR_EVADE_WALL=off (no wall clearance)");
        return;
    }
    const bool sphere_ok =
        size > SphereCastRva + SphereCastSig.size() &&
        std::memcmp(module + SphereCastRva, SphereCastSig.data(), SphereCastSig.size()) == 0;
    const bool ray_ok =
        size > RagdollRayRva + RagdollRaySig.size() &&
        std::memcmp(module + RagdollRayRva, RagdollRaySig.data(), RagdollRaySig.size()) == 0;
    if (sphere_ok && want != "ray") {
        g_sphere_cast = reinterpret_cast<SphereCastFn>(module + SphereCastRva);
        g_wall_method = WallMethod::Sphere;
    } else if (ray_ok) {
        g_ragdoll_ray = reinterpret_cast<RagdollRayFn>(module + RagdollRayRva);
        g_wall_method = WallMethod::Ray;
    }
    if (!sphere_ok || !ray_ok) {
        LOG_WARNING(Core,
                    "VR evade: wall casts: sphere cast {} ray {}; live bytes sphere: {} ray: {}",
                    sphere_ok ? "ok" : "MISMATCH", ray_ok ? "ok" : "MISMATCH",
                    Hook::HexBytes(module + SphereCastRva, SphereCastSig.size()),
                    Hook::HexBytes(module + RagdollRayRva, RagdollRaySig.size()));
    }
    if (g_wall_method == WallMethod::Off) {
        LOG_WARNING(Core, "VR evade: no wall clearance (no usable cast)");
    }
}

} // Anonymous namespace

bool CastMapSphere(const float from[3], const float to[3], float radius, float hit[3],
                   float normal[3], float& fraction) {
    if (g_map_cast == nullptr) {
        return false;
    }
    u8* const mgr = Read<u8*>(g_map_base + PhysWorldRva);
    u8* const world = Plausible(mgr) ? Read<u8*>(mgr + OffsetPhysWorldWrapper) : nullptr;
    if (!Plausible(world) || !Plausible(Read<u8*>(world + 0x08))) {
        return false;
    }
    alignas(16) float a[4] = {from[0], from[1], from[2], 1.0f};
    alignas(16) float d[4] = {to[0] - from[0], to[1] - from[1], to[2] - from[2], 0.0f};
    alignas(16) float h[4]{};
    alignas(16) float n[4]{};
    alignas(16) float f[4] = {1.0f};
    if (g_map_cast(world, LayerRagdollLandscape, a, d, h, n, f, radius) == 0 ||
        !(f[0] > 1e-3f)) {
        return false; // nothing in the way, or it starts inside the map (no information)
    }
    for (int i = 0; i < 3; ++i) {
        hit[i] = h[i];
        normal[i] = n[i];
    }
    fraction = std::clamp(f[0], 0.0f, 1.0f);
    return true;
}

void EvasionEbootLoaded(VAddr base, u64 size) {
    if (g_map_cast == nullptr) {
        // The map cast alone (CastMapSphere), whatever physical_evasion says.
        auto* const module = reinterpret_cast<u8*>(base);
        if (size > SphereCastRva + SphereCastSig.size() &&
            std::memcmp(module + SphereCastRva, SphereCastSig.data(), SphereCastSig.size()) == 0) {
            g_map_base = module;
            g_map_cast = reinterpret_cast<SphereCastFn>(module + SphereCastRva);
        } else {
            LOG_WARNING(Core, "VR map cast: the sphere cast does not match (no weapon_wall)");
        }
    }
    if (g_code || !VR::GetSettings().physical_evasion) {
        if (!VR::GetSettings().physical_evasion) {
            LOG_INFO(Core, "VR evade: physical_evasion=0 (not installed)");
        }
        return;
    }
    if (const char* hook = std::getenv("VR_EVADE_HOOK");
        hook != nullptr && std::strcmp(hook, "off") == 0) {
        LOG_INFO(Core, "VR evade: VR_EVADE_HOOK=off (not installed)");
        return;
    }
    if (const char* level = std::getenv("VR_EVADE_LOG")) {
        g_log = std::clamp(std::atoi(level), 1, 3);
    }
    if (const char* level = std::getenv("VR_EVADE_HITLOG")) {
        g_hitlog = std::clamp(std::atoi(level), 0, 2);
    }
    if (const char* reach = std::getenv("VR_EVADE_SHIFT_REACH")) {
        g_shift_reach = std::clamp(static_cast<float>(std::atof(reach)), 0.0f, 1.0f);
        LOG_INFO(Core, "VR evade: VR_EVADE_SHIFT_REACH={:.2f}", g_shift_reach);
    }
    if (const char* reach = std::getenv("VR_EVADE_LEAN_REACH")) {
        g_lean_reach = std::clamp(static_cast<float>(std::atof(reach)), 0.0f, 2.0f);
        LOG_INFO(Core, "VR evade: VR_EVADE_LEAN_REACH={:.2f}", g_lean_reach);
    }
    auto* const module = reinterpret_cast<u8*>(base);
    if (size <= WorldChrManRva + 8) {
        LOG_INFO(Core, "VR evade: eboot too small ({:#x}); not installed", size);
        return;
    }
    u8* const site = module + TargetSiteRva;
    const bool fn_ok =
        std::memcmp(module + KeyframeFnRva, KeyframeFnSig.data(), KeyframeFnSig.size()) == 0;
    const bool site_ok = std::memcmp(site, TargetSiteSig.data(), TargetSiteSig.size()) == 0;
    const bool vtable_ok = size > RagdollVtableRva + 8;
    if (!fn_ok || !site_ok || !vtable_ok) {
        LOG_WARNING(Core,
                    "VR evade: not installed (keyframe fn {}, site {}); live bytes fn: {} site: {}",
                    fn_ok ? "ok" : "MISMATCH", site_ok ? "ok" : "MISMATCH",
                    Hook::HexBytes(module + KeyframeFnRva, KeyframeFnSig.size()),
                    Hook::HexBytes(site, TargetSiteSig.size()));
        return;
    }
    if (!Hook::MakeWritable(site, TargetSiteSig.size())) {
        LOG_ERROR(Core, "VR evade: could not make the keyframe site writable; not installed");
        return;
    }
    try {
        g_code = std::make_unique<Xbyak::CodeGenerator>(4096);
        void* const stub = EmitStub(*g_code, site);
        g_base = module;
        Hook::WriteJump(site, TargetSiteSig.size(), stub);
        InitEvasionDll();
        g_installed = true;
        const auto& s = VR::GetSettings();
        InstallWall(module, size);
        if (g_hitlog > 0 || s.evasion_feedback) {
            InstallHitLog(module, size);
        }
        LOG_INFO(Core,
                 "VR evade: installed at {:#x} (keyframe fn ok, site ok) gain {:.2f} max {:.2f} m "
                 "lean {:.2f} m duck {:.2f} m hold {} ms (tau {}) settle {} ms wall {} margin "
                 "{:.2f} m{}",
                 TargetSiteRva, s.evasion_gain, std::min(s.evasion_max_m, s.positional_limit_m),
                 s.evasion_lean_m, DuckCap(s), s.evasion_hold_ms, s.evasion_hold_tau_ms,
                 s.evasion_settle_ms,
                 !s.evasion_wall                       ? "off (evasion_wall=0)"
                 : g_wall_method == WallMethod::Sphere ? "sphere casts"
                 : g_wall_method == WallMethod::Ray    ? "rays"
                                                       : "OFF",
                 s.evasion_wall_margin_m,
                 g_log > 0 ? fmt::format(" log {}", g_log) : std::string{});
    } catch (const Xbyak::Error& error) {
        LOG_ERROR(Core, "VR evade: code emission failed ({}); not installed", error.what());
        g_code.reset();
    }
}

void EvasionRenderTick(const VR::CameraOverride* result, bool fp_follow) {
    if (!g_installed) {
        return;
    }
    ++g_frame;
    const auto& settings = VR::GetSettings();
    const s64 now = VR::Body::SteadyNowNs();

    // The main player's ragdoll (it changes on load, death and area change).
    u8* player = nullptr;
    u8* const ragdoll = MainRagdoll(&player);
    g_player = player;
    if (ragdoll != g_ragdoll) {
        g_ragdoll = ragdoll;
        g_map_ok = ragdoll != nullptr && ResolveBones(ragdoll);
        g_model.Reset();
        g_main_ragdoll.store(g_map_ok ? reinterpret_cast<u64>(ragdoll) : 0,
                             std::memory_order_release);
        g_capsule_log_frame = g_map_ok ? g_frame + 30 : 0;
    }
    if (g_capsule_log_frame != 0 && g_frame >= g_capsule_log_frame) {
        g_capsule_log_frame = 0;
        LogCapsules(ragdoll);
    }

    VR::Body::BodyStatus status{};
    PiEvasionRawFacts raw{};
    raw.have_body = VR::Body::LatestBodyStatus(status);
    raw.body_ns = status.steady_ns;
    raw.driven = status.driven;
    raw.evade_ok = status.evade_ok;
    raw.ragdoll = g_map_ok;
    raw.camera = result != nullptr;
    raw.fp_follow = fp_follow;
    const float scale = result != nullptr ? result->world_scale : 1.0f;
    raw.scale = scale;
    if (result) {
        raw.neck_valid = result->neck_valid;
        raw.head_tracked = result->head_tracked;
        raw.neck[0] = result->neck_offset.x;
        raw.neck[1] = result->neck_offset.y;
        raw.neck[2] = result->neck_offset.z;
    }
    std::copy(status.upper_shift.begin(), status.upper_shift.end(), raw.upper_shift);
    std::copy(status.upper_lean.begin(), status.upper_lean.end(), raw.upper_lean);
    raw.ik_left = status.drawn_weight[0];
    raw.ik_right = status.drawn_weight[1];
    raw.look_only = status.look_only;
    raw.body_offset_m = VR::HeightBodyOffsetM();
    raw.eye_offset_m = VR::HeightEyeOffsetM();
    raw.fp_eye_m = settings.fp_eye_height_m;
    raw.gain = settings.evasion_gain;
    raw.max_m = settings.evasion_max_m;
    raw.lean_m = settings.evasion_lean_m;
    raw.duck_m = settings.evasion_duck_m;
    raw.wall_margin_m = settings.evasion_wall_margin_m;
    raw.wall_rise_mps = settings.evasion_wall_rise_mps;
    raw.hold_ms = settings.evasion_hold_ms;
    raw.hold_tau_ms = settings.evasion_hold_tau_ms;
    raw.settle_ms = settings.evasion_settle_ms;
    raw.settle_tau_ms = settings.evasion_settle_tau_ms;
    raw.pelvis = settings.evasion_pelvis;
    raw.shift_reach = g_shift_reach;
    raw.lean_reach = g_lean_reach;
    raw.positional_m = settings.positional_limit_m;
    raw.positional_down_m = settings.positional_limit_down_m;
    raw.feedback = settings.evasion_feedback;
    raw.wall_enabled = settings.evasion_wall;
    raw.wall_available = g_wall_method != WallMethod::Off;
    E::Output out = g_model.Frame(raw, now);
    if (g_model.Publication().frame.cast_required) {
        const auto& plan = g_model.Publication().frame;
        PiEvasionWallFacts facts{};
        facts.token = plan.token;
        facts.root_free_m = -1.0f;
        float root[3];
        if (ReadRoot(root)) {
            const float ux = plan.cast_dir[0], uz = plan.cast_dir[1];
            g_wall_dir[0] = ux;
            g_wall_dir[1] = uz;
            const s64 t0 = VR::Body::SteadyNowNs();
            facts.root_free_m = WallFree(root, ux, uz, plan.root_reach_m, scale);
            if (facts.root_free_m >= 0.0f)
                ReadClosedLoop(ux, uz, plan.closed_reach_m, scale, plan.body_query_mask, facts);
            const s64 spent = VR::Body::SteadyNowNs() - t0;
            g_wall.ns += spent;
            g_wall.max_ns = std::max(g_wall.max_ns, spent);
        }
        out = g_model.Finish(facts, now);
    }
    const auto& publication = g_model.Publication();
    if (publication.frame.wall_closer)
        ++g_wall.closer;
    if (out.wall_limited) {
        ++g_wall.limited;
    }
    if (publication.wall_knock) {
        VR::Combat::PushEvadeEvent({VR::Combat::EvadeEventKind::Wall, -1,
                                    publication.wall_knock_m, now});
    }

    const bool publish_live = g_map_ok && publication.frame.reason != PI_ER_FAULT;
    Published p{};
    p.ragdoll = publish_live ? reinterpret_cast<u64>(ragdoll) : 0;
    p.player = publish_live ? reinterpret_cast<u64>(g_player) : 0;
    p.frame = g_frame;
    p.view_yaw = publish_live && result != nullptr ? result->view_yaw : 0.0f;
    for (size_t bone = 0; bone < E::BodyCount; ++bone) {
        p.body_of_bone[bone] = g_map_ok ? static_cast<s8>(g_body_of_bone[bone]) : s8{-1};
    }
    p.unit = publish_live && scale > 1e-4f ? scale : 1.0f;
    std::copy_n(publication.frame.world_shift, 3, p.shift.begin());
    std::copy_n(publication.frame.world_lean, 3, p.lean.begin());
    if (publish_live) {
        for (size_t b = 0; b < E::BodyCount; ++b) {
            const auto bone = static_cast<size_t>(g_bone_of_body[b]);
            p.offset[bone] = {out.offset[b].x * scale, out.offset[b].y * scale,
                              out.offset[b].z * scale};
            p.shift_k[bone] = publication.shift_k[b];
            p.lean_k[bone] = publication.lean_k[b];
        }
    }
    g_published.Store(p);

    if (g_log > 0) {
        const auto& f = publication.frame;
        E::Input in{};
        in.t = double(now) * 1e-9;
        in.enabled = f.enabled;
        in.active = f.active;
        in.tracked = f.tracked;
        in.look_only = f.look_only;
        in.neck = PhysicalInteractions::EvasionPackets::Get(f.neck);
        in.pose_shift = PhysicalInteractions::EvasionPackets::Get(f.pose_shift);
        in.pose_lean = PhysicalInteractions::EvasionPackets::Get(f.pose_lean);
        in.ik_left = f.ik_left;
        in.ik_right = f.ik_right;
        in.wall_m = f.input_wall_m;
        Log(out, in, EvasionReason(f.reason, status.evade_block), scale, now);
    }
    if (g_hit_hooks) {
        HitLogTick(now);
    }
}

bool IsEvasionInstalled() {
    return g_installed;
}

} // namespace VR::Bloodborne
