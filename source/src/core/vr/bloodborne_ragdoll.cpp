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
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "core/vr/amygdalan_hook.h"
#include "core/vr/bloodborne_corpse.h"
#include "core/vr/bloodborne_headshot.h"
#include "core/vr/bloodborne_melee.h"
#include "core/vr/bloodborne_ragdoll.h"
#include "core/vr/bloodborne_sever.h"
#include "core/vr/bloodborne_stagger.h"
#include "core/vr/bloodborne_tuning.h"
#include "core/vr/body_channel.h"
#include "core/vr/combat_channel.h"
#include "core/vr/hook_util.h"
#include "core/vr/ragdoll_getups.h"
#include "core/vr/ragdoll_kill.h"
#include "core/vr/sever_policy.h"
#include "core/vr/vr_session.h"

namespace VR::Bloodborne {

namespace {

namespace R = VR::Ragdoll;
using Hook::Plausible;
using Hook::Read;

// ---- Sites -----------------------------------------------------------------------------------------

// The ChrCtrl ragdoll update `void(ChrCtrl* rdi)` (called for every chr every frame from vt+0xF8
// 0x18BF460, on the chr's own CSChrThread): it turns the request +0x144 into the RagdollIns' (+0x20)
// state, then runs the RagdollIns update (0x180DF90: the game's ragdoll blast, the state switch, the
// pose). Its prologue through `test rax, rax` (16 position-independent bytes, no branch lands
// inside) is replayed after our compare; the rest of the signature proves the request (+0x144) and
// the applied one (+0x148).
constexpr u64 CtrlRagdollRva = 0x1515CC0;
constexpr std::array<u8, 30> CtrlRagdollSig = {
    0x55,                               // push rbp
    0x48, 0x89, 0xE5,                   // mov rbp, rsp
    0x53,                               // push rbx
    0x50,                               // push rax
    0x48, 0x89, 0xFB,                   // mov rbx, rdi
    0x48, 0x8B, 0x43, 0x20,             // mov rax, [rbx+0x20] (the RagdollIns)
    0x48, 0x85, 0xC0,                   // test rax, rax
    0x74, 0x46,                         // je 0x1515D18  <- resume
    0x8B, 0x8B, 0x44, 0x01, 0x00, 0x00, // mov ecx, [rbx+0x144]
    0x8B, 0x93, 0x48, 0x01, 0x00, 0x00, // mov edx, [rbx+0x148]
};
constexpr size_t CtrlRagdollPatchLen = 16;
// TAE event 228's handler: the ChrCtrl (`mov rax, [rax+0x58]`) and its full-ragdoll store (blend
// <= 0, every enemy death: args 0, 0): `mov dword [rax+0x144], 1; vsubss xmm0, xmm0, xmm1;
// vmovss [rax+0x14C], xmm0` (the weight 1 - blend). Compared only: our writes are the same.
constexpr u64 Ev228CtrlRva = 0x1A222CF;
constexpr std::array<u8, 4> Ev228CtrlSig = {0x48, 0x8B, 0x40, 0x58};
constexpr u64 FullRagdollStoreRva = 0x1A22A15;
constexpr std::array<u8, 22> FullRagdollStoreSig = {
    0xC7, 0x80, 0x44, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xC5,
    0xFA, 0x5C, 0xC1, 0xC5, 0xFA, 0x11, 0x80, 0x4C, 0x01, 0x00, 0x00,
};
// DamageStep's killer store `mov [rcx+0x1DC], eax` (the attacker's handle, once HP stays <= 0 past
// the immortality check). Compared only.
constexpr u64 KillerStoreRva = 0x1A2C176;
constexpr std::array<u8, 6> KillerStoreSig = {0x89, 0x81, 0xDC, 0x01, 0x00, 0x00};
// hknpWorld::applyBodyLinearImpulse(world, s32 body, const hkVector4* impulse): returns for a body
// that is not dynamic, activates it, linVel += impulse * invMass. The prologue and the body lookup
// (bodies [world+0x20], stride 0x90).
constexpr u64 ApplyLinearImpulseRva = 0xB68DB0;
constexpr std::array<u8, 41> ApplyLinearImpulseSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
    0x83, 0xEC, 0x28, 0x49, 0x89, 0xD4, 0x41, 0x89, 0xF7, 0x49, 0x89, 0xFD, 0x49, 0x63,
    0xC7, 0x4D, 0x8B, 0x75, 0x20, 0x48, 0x8D, 0x1C, 0xC0, 0x48, 0xC1, 0xE3, 0x04,
};
using ApplyImpulseFn = void(PS4_SYSV_ABI*)(void* world, s32 body_id, const float* impulse);

// ChrCtrl::SetRagdoll(bool), including the false branch. Unlike simply writing
// request -1 late in ChrCtrl::Update, this resets the requested state and collision
// filter even when the earlier request-history slot already contains -1.
constexpr u64 ToggleRagdollRva = 0x1515E40;
constexpr std::array<u8, 9> ToggleRagdollSig = {0x48, 0x8B, 0x47, 0x20, 0x40,
                                                0x84, 0xF6, 0x74, 0x2F};
constexpr u64 ResetRagdollRva = 0x1515E78;
constexpr std::array<u8, 57> ResetRagdollSig = {
    0x48, 0x85, 0xC0, 0x74, 0x14, 0xC7, 0x80, 0xD4, 0,    0,    0,    0,    0,    0,    0,
    0xC7, 0x80, 0xC4, 0,    0,    0,    0,    0,    0,    0,    0xC7, 0x87, 0x44, 0x01, 0,
    0,    0xFF, 0xFF, 0xFF, 0xFF, 0xC7, 0x87, 0x4C, 0x01, 0,    0,    0,    0,    0,    0,
    0x0F, 0xB7, 0xB7, 0x14, 0x01, 0,    0,    0xE9, 0x6F, 0x06, 0,    0};
using ToggleRagdollFn = void(PS4_SYSV_ABI*)(u8* ctrl, bool dynamic);
ToggleRagdollFn g_toggle_ragdoll{};

constexpr u64 RagdollVtableRva = 0x538D440; // RagdollIns
constexpr u64 PhysWorldRva = 0x553D700;     // [[+0x28]+0x08] = the hknpWorld

// ChrIns
constexpr u32 OffsetHandle = 0x08;
constexpr u32 OffsetChrCtrl = 0x58;
constexpr u32 OffsetNpcParam = 0x70; // s32 (100021 for the arena's Scourge Beast)
constexpr u32 OffsetChrId = 0x74;    // s32 (1000 = c1000)
constexpr u32 OffsetKiller = 0x1DC;  // the killing attacker's handle
constexpr u32 OffsetModules = 0x3B0;
constexpr u32 OffsetTimeActModule = 0x18;
constexpr u32 OffsetDataModule = 0x20;
constexpr u32 OffsetPhysicsModule = 0x68;
constexpr u32 OffsetThrowModule = 0x88;
constexpr u32 OffsetThrowNode = 0x28;
constexpr u32 OffsetThrowState = 0x20;
constexpr u32 OffsetHp = 0xF8;
constexpr u32 OffsetPhysicsPosition = 0x1E0;
// The TimeAct ring (core/vr/bloodborne_layout.h): 10 entries {s32 anim id, ...} of 16 bytes at
// +0x20, the next write at +0xC0.
constexpr u32 OffsetTimeActRing = 0x20;
constexpr u32 OffsetTimeActWrite = 0xC0;
constexpr s32 TimeActRingSize = 10;
constexpr u32 TimeActEntrySize = 16;
constexpr u32 OffsetTimeActEntryTime = 8; // f32: the clip's time
// EMEVD ForceAnimationPlayback's write (1.09 0x13CBF85..0x13CBFB0): the clip to the event
// module's request (+0x20) and the character's sticky copy ([chr+0x20]+4), only while the
// module at +0x30 is ready (+0x384).
constexpr u32 OffsetEventModule = 0x58;
constexpr u32 OffsetEventModuleOwner = 0x08;
constexpr u32 OffsetEventAnimRequest = 0x20;
constexpr u32 OffsetChrSticky = 0x20;
constexpr u32 OffsetStickyAnim = 0x04;
constexpr u32 OffsetAnimReadyModule = 0x30;
constexpr u32 OffsetAnimReady = 0x384;
// ChrCtrl
constexpr u32 OffsetCtrlOwner = 0x08;
constexpr u32 OffsetRagdoll = 0x20;
constexpr u32 OffsetRagdollRequest = 0x144; // 1 = full ragdoll (RagdollIns state 2)
constexpr u32 OffsetRagdollWeight = 0x14C;
// RagdollIns
constexpr u32 OffsetRagdollOwner = 0x10;
constexpr u32 OffsetRagdollSystem = 0x38;
constexpr u32 OffsetSystemBodyIds = 0x20;
constexpr u32 OffsetSystemBodyCount = 0x28;
constexpr u32 OffsetRagdollState = 0xC0;
constexpr s32 RagdollStateDead = 2; // the full ragdoll: the bodies dynamic
// hknpWorld
constexpr u32 OffsetWorldBodies = 0x20;
constexpr u32 OffsetWorldMotions = 0xE0;
constexpr u32 BodyStride = 0x90;
constexpr u32 BodyFlags = 0x40;
constexpr u32 BodyFlagDynamic = 0x2;
constexpr u32 BodyMotionId = 0x68;
constexpr u32 MotionStride = 0x80;
constexpr u32 MotionCom = 0x00;
constexpr u32 MotionInvMassHalf = 0x26; // hkHalf (the top 16 bits of a float)
constexpr u32 MotionLinearVelocity = 0x40;
// The damage result (the final-damage detour's rcx).
constexpr u32 ResCategory = 0x13E; // u8 BehaviorParam.category: 1 = the right weapon's attack
constexpr u32 ResHand = 0x44;
constexpr u32 ResAtkParam = 0x3C;
constexpr u32 ResBullet = 0x14C;
constexpr u32 ResHitPosition = 0x180;
// s32: the struck collision (ragdoll) body's index; proven for bullets by the headshots, logged
// for melee hits by SeverOnPlayerHit until a run shows the claw's hits fill it too.
constexpr u32 ResBodyIndex = 0x1B0;
constexpr u32 ResTarget = 0x1E8;

// The charged Stake Driver blast's push speed (m/s; it ignores the slow trigger-release hand):
// 8 -> 10 on 2026-11-02 with its stronger momentum and caps (ragdoll_kill.h AttackPushConfig).
constexpr float StakeBlastSpeed = 10.0f;
// After the switch (VR_RAGDOLL_LOG): the state lines this many updates after it.
constexpr s32 AfterLogUpdates = 30;
// After the push: the guard that keeps the corpse off the hunter (R::GuardConfig).
const R::GuardConfig Guard{};
// An armed slot nobody's update came back to (the enemy unloaded) is reused after this.
constexpr s64 StaleNs = 3'000'000'000;

// ---- State -----------------------------------------------------------------------------------------

enum class Phase : u8 {
    Free,
    Armed,    // killed: waiting for its death animation
    Switched, // the ragdoll requested: the push on its next update
    After,    // pushed: the guard (and with VR_RAGDOLL_LOG a few state lines)
};

struct Pending {
    Phase phase{Phase::Free};
    u8* target{};
    u8* ctrl{};
    u32 handle{};
    u32 hunter{}; // the hunter's handle (the killer the game must name)
    u8* player{}; // the hunter (the guard reads where he stands; checked by the handle)
    s32 chr_id{-1};
    s32 npc{-1};
    s32 ring_next{-1}; // the TimeAct ring's entries from here on are new since the kill
    s32 updates{0};
    s32 switched_at{0};
    s64 armed_ns{0};
    s64 pushed_ns{0};
    float guard_max{0.0f}; // the fastest motion toward the hunter the guard took out (m/s)
    s32 guard_bodies{0};   // body-updates it acted on
    R::Vec3 hit{};
    R::Vec3 hunter_pos{};
    R::Vec3 target_pos{};
    bool blade{false};
    R::Vec3 blade_vel{};
    bool stake_blast{false}; // exact retail charged blast, frozen at the killing hit
    bool charged{false};     // a charged attack's kill (always limp, its own push), frozen there
    R::PushAttack push_attack{R::PushAttack::Normal}; // frozen at the killing hit
    float tip_speed{0.0f};  // the presenter's tip speed for the swing
    float game_tip{0.0f};   // the game blade's tip speed at the last pose tick
    float hit_speed{0.0f};  // the blade's speed at the hit (R::HitSpeed)
    bool swing{false};
    bool transform{false};
    u32 serial{0};
    const char* kind{""};
    s32 atk{-1};
    s32 anim{-1};
    R::DeathAnim death{};
    R::VanillaRoll roll{}; // melee_ragdoll_vanilla_pct: this kill's one roll
};

u8* g_base = nullptr;
bool g_installed = false;
bool g_log = false;     // VR_RAGDOLL_LOG: the armed / state lines too
bool g_oneshot = false; // VR_RAGDOLL_ONESHOT (tests): a right-hand melee hit is lethal
bool g_oneshot_any = false; // VR_RAGDOLL_ONESHOT=any (tests): every hit of the hunter's is
ApplyImpulseFn g_apply = nullptr;
std::unique_ptr<Xbyak::CodeGenerator> g_code;
std::mutex g_mutex;
std::array<Pending, 4> g_pending{};
// The ChrCtrl pointers the entry stub compares with (0 = none): the armed kills' (written under
// g_mutex), then the corpses bare hands may touch (bloodborne_corpse.cpp, WatchCorpse).
constexpr size_t KillSlots = 4;
constexpr size_t HookSlot = KillSlots + CorpseWatchSlots;
alignas(64) std::array<std::atomic<u64>, HookSlot + 1> g_watch{};
static_assert(KillSlots == std::tuple_size_v<decltype(g_pending)>);
static_assert(sizeof(std::atomic<u64>) == sizeof(u64));
std::atomic<u64> g_kills{0};
std::atomic<u64> g_ragdolls{0};
// melee_ragdoll_vanilla_pct's die: seeded once at install, drawn under g_mutex (one roll per kill).
R::KillDice g_dice{};

struct RecoveryTest {
    u8* target{};
    u8* ctrl{};
    u32 handle{};
    s32 chr_id{}, hp{}, phase{};
    s64 start{}, phase_start{}, log_at{};
    R::Vec3 root{};
    bool dynamic{}, restored{};
};
std::mutex g_recovery_mutex;
RecoveryTest g_recovery{};

// Down: the get-up clip runs on fast under the still limp body (ArmHookAnimationRate) to where it
// lies and starts to get up; then Blend hands the body to it (2026-11-06: "when the
// ragdoll of the beast ends, it levitates and it looks stupid": the limp body blended over 0.6 s
// into the standing pose it was hooked in, rising with no legs under it).
// Lie: let go and settled, it lies there stunned LieNs before getting up (the headset, 2026-11-06:
// "when the ragdoll effect ended they attacked immediately").
enum class ArmPhase { Armed, Held, Coast, Lie, Down, Blend, Verify };
constexpr s64 LieNs = 1'200'000'000;
constexpr s64 CoastMaxNs = 3'000'000'000; // flung far: still flying this long at most
// The knockdown's clock while it runs on underneath: 0.17 s of it a 60 Hz frame (at 20x the mock's
// c1000 stopped at 2.00 s, past its get-up's start at 1.83).
constexpr float GetUpFastRate = 10.f;
constexpr s64 GetUpWaitNs = 600'000'000;         // never there by then: the plain blend
constexpr float GetUpLeadS = .05f;               // handed over just before the get-up starts
constexpr float GetUpBlendS = .35f;              // limp body -> the clip lying where it lies
struct ArmHold {
    u8* target{};
    u8* ctrl{};
    u8* rag{};
    u8* system{};
    u8* world{};
    u32 handle{};
    ArmPhase phase{ArmPhase::Armed};
    ArmHookInput input{};
    ArmHookStatus status{};
    s64 start{}, changed{}, tick{};
    s32 body{-1}, pelvis{-1};
    Body::Vec3 offset{};
    float length{};
    u32 pulls{};
    float impulse{};
    s32 getup{-1};   // the knockdown clip it gets up with (-1: none, the plain blend)
    float up_at{};   // s into it: lying, about to get up
    Body::Vec3 lever_tip{}, lever_v{}; // the lever's tip last update and its velocity, when swung
    s64 lever_ns{};
};
// Shared with the entry stub; zero when there is no living hook. No borrowed
// pointer is dereferenced on another thread merely because it is in this slot.
std::atomic<u64> g_arm_target{};
ArmHold g_arm{}; // g_recovery_mutex, shared with the mutually exclusive probe

void ClearArm() {
    g_arm_target.store(0, std::memory_order_release);
    g_watch[HookSlot].store(0, std::memory_order_release);
    g_arm = {};
}

s64 NowNs() {
    return VR::Body::SteadyNowNs();
}

u8* Module(u8* chr, u32 offset) {
    if (!Plausible(chr)) {
        return nullptr;
    }
    u8* const mods = Read<u8*>(chr + OffsetModules);
    if (!Plausible(mods)) {
        return nullptr;
    }
    u8* const m = Read<u8*>(mods + offset);
    return Plausible(m) ? m : nullptr;
}

s32 Hp(u8* chr) {
    u8* const data = Module(chr, OffsetDataModule);
    return data != nullptr ? Read<s32>(data + OffsetHp) : -1;
}

bool Position(u8* chr, R::Vec3& out) {
    u8* const phys = Module(chr, OffsetPhysicsModule);
    if (phys == nullptr) {
        return false;
    }
    out = {Read<float>(phys + OffsetPhysicsPosition), Read<float>(phys + OffsetPhysicsPosition + 4),
           Read<float>(phys + OffsetPhysicsPosition + 8)};
    return VR::Body::IsFinite(out);
}

bool InThrow(u8* chr) {
    u8* const throw_module = Module(chr, OffsetThrowModule);
    if (throw_module == nullptr) {
        return false;
    }
    u8* const node = Read<u8*>(throw_module + OffsetThrowNode);
    return Plausible(node) && Read<u8>(node + OffsetThrowState) != 0;
}

// The chr's ChrCtrl when it has its own RagdollIns.
u8* RagdollCtrl(u8* chr) {
    u8* const ctrl = Plausible(chr) ? Read<u8*>(chr + OffsetChrCtrl) : nullptr;
    if (!Plausible(ctrl) || Read<u8*>(ctrl + OffsetCtrlOwner) != chr) {
        return nullptr;
    }
    u8* const rag = Read<u8*>(ctrl + OffsetRagdoll);
    if (!Plausible(rag) || Read<u8*>(rag) != g_base + RagdollVtableRva ||
        Read<u8*>(rag + OffsetRagdollOwner) != chr) {
        return nullptr;
    }
    return ctrl;
}

s32 RingWrite(u8* chr) {
    u8* const ta = Module(chr, OffsetTimeActModule);
    if (ta == nullptr) {
        return -1;
    }
    const s32 write = Read<s32>(ta + OffsetTimeActWrite);
    return write >= 0 && write < TimeActRingSize ? write : -1;
}

// The clip the chr plays now (the TimeAct ring's newest, its full id) and its time.
bool NewestClip(u8* chr, s32& id, float& time) {
    u8* const ta = Module(chr, OffsetTimeActModule);
    const s32 write = RingWrite(chr);
    if (ta == nullptr || write < 0) {
        return false;
    }
    const u8* const entry =
        ta + OffsetTimeActRing + ((write + TimeActRingSize - 1) % TimeActRingSize) * TimeActEntrySize;
    id = Read<s32>(entry);
    time = Read<float>(entry + OffsetTimeActEntryTime);
    return std::isfinite(time);
}

// The game's own ForceAnimationPlayback on `chr` (OffsetEventModule): false when it is not ready
// for one, as the event's handler leaves it then.
bool ForceAnimation(u8* chr, s32 clip) {
    u8* const events = Module(chr, OffsetEventModule);
    u8* const ready = Module(chr, OffsetAnimReadyModule);
    u8* const sticky = Plausible(chr) ? Read<u8*>(chr + OffsetChrSticky) : nullptr;
    if (events == nullptr || Read<u8*>(events + OffsetEventModuleOwner) != chr || ready == nullptr ||
        Read<u8>(ready + OffsetAnimReady) == 0 || !Plausible(sticky)) {
        return false;
    }
    std::memcpy(events + OffsetEventAnimRequest, &clip, sizeof(clip));
    std::memcpy(sticky + OffsetStickyAnim, &clip, sizeof(clip));
    return true;
}

// The TimeAct ring's entries new since the last look, oldest first: the first death animation of
// the chr's own (ending in the ragdoll or animated) decides.
R::DeathAnim ScanDeath(Pending& p) {
    u8* const ta = Module(p.target, OffsetTimeActModule);
    if (ta == nullptr) {
        return {};
    }
    const s32 write = Read<s32>(ta + OffsetTimeActWrite);
    if (write < 0 || write >= TimeActRingSize) {
        return {};
    }
    if (p.ring_next < 0 || p.ring_next >= TimeActRingSize) {
        p.ring_next = (write + TimeActRingSize - 1) % TimeActRingSize; // the newest only
    }
    for (s32 n = 0; n < TimeActRingSize && p.ring_next != write; ++n) {
        const s32 id = Read<s32>(ta + OffsetTimeActRing + p.ring_next * TimeActEntrySize);
        p.ring_next = (p.ring_next + 1) % TimeActRingSize;
        const R::DeathAnim d = R::ClassifyDeath(p.chr_id, id);
        if (d.kind != R::Death::Unknown) {
            p.anim = id;
            return d;
        }
    }
    return {};
}

u8* PhysWorld() {
    u8* const a = Read<u8*>(g_base + PhysWorldRva);
    u8* const b = Plausible(a) ? Read<u8*>(a + 0x28) : nullptr;
    u8* const w = Plausible(b) ? Read<u8*>(b + 0x08) : nullptr;
    return Plausible(w) ? w : nullptr;
}

struct BodyRead {
    s32 id{-1};
    R::PushBody body{};
    Body::Quat rotation{};
};

// The RagdollIns' bodies (ids [[rag+0x38]+0x20], count +0x28) with their motions.
s32 ReadBodies(u8* rag, u8* world, std::array<BodyRead, R::MaxBodies>& out) {
    u8* const system = Read<u8*>(rag + OffsetRagdollSystem);
    if (!Plausible(system)) {
        return 0;
    }
    u8* const ids = Read<u8*>(system + OffsetSystemBodyIds);
    const s32 count = Read<s32>(system + OffsetSystemBodyCount);
    u8* const bodies = Read<u8*>(world + OffsetWorldBodies);
    u8* const motions = Read<u8*>(world + OffsetWorldMotions);
    if (!Plausible(ids, 4) || count <= 0 || count > R::MaxBodies || !Plausible(bodies) ||
        !Plausible(motions)) {
        return 0;
    }
    s32 n = 0;
    for (s32 i = 0; i < count; ++i) {
        const s32 id = Read<s32>(ids + i * 4);
        if (id < 0 || id > 0x100000) {
            continue;
        }
        u8* const body = bodies + static_cast<size_t>(id) * BodyStride;
        const s32 motion_id = Read<s32>(body + BodyMotionId);
        if (motion_id <= 0 || motion_id > 0x100000) {
            continue;
        }
        u8* const motion = motions + static_cast<size_t>(motion_id) * MotionStride;
        BodyRead& b = out[n++];
        b.id = id;
        b.rotation = Read<Body::Quat>(motion + 0x10);
        b.body.dynamic = (Read<u32>(body + BodyFlags) & BodyFlagDynamic) != 0;
        b.body.com = {Read<float>(motion + MotionCom), Read<float>(motion + MotionCom + 4),
                      Read<float>(motion + MotionCom + 8)};
        b.body.vel = {Read<float>(motion + MotionLinearVelocity),
                      Read<float>(motion + MotionLinearVelocity + 4),
                      Read<float>(motion + MotionLinearVelocity + 8)};
        const u32 bits = static_cast<u32>(Read<u16>(motion + MotionInvMassHalf)) << 16;
        float inv_mass;
        std::memcpy(&inv_mass, &bits, sizeof(inv_mass));
        b.body.mass = inv_mass > 1e-6f && std::isfinite(inv_mass) ? 1.0f / inv_mass : 0.0f;
    }
    return n;
}

std::string Enemy(const Pending& p) {
    return fmt::format("c{:04d} (NpcParam {})", p.chr_id, p.npc);
}

std::string Attack(const Pending& p) {
    if (p.swing) {
        return fmt::format("{} #{} (atk {})",
                           p.transform ? std::string{"a flick's transform attack"}
                                       : fmt::format("a physical {} swing", p.kind),
                           p.serial, p.atk);
    }
    return fmt::format("a right-hand attack (atk {})", p.atk);
}

std::string V3(R::Vec3 v) {
    return fmt::format("({:.2f},{:.2f},{:.2f})", v.x, v.y, v.z);
}

void Release(size_t i) {
    g_pending[i] = {};
    g_watch[i].store(0, std::memory_order_release);
}

void Push(Pending& p, u8* rag) {
    u8* const world = PhysWorld();
    std::array<BodyRead, R::MaxBodies> read{};
    const s32 n = world != nullptr ? ReadBodies(rag, world, read) : 0;
    std::array<R::PushBody, R::MaxBodies> bodies{};
    for (s32 i = 0; i < n; ++i) {
        bodies[i] = read[i].body;
    }
    const R::PushConfig cfg =
        R::AttackPushConfig(VR::GetSettings().melee_ragdoll_impulse, p.push_attack);
    R::PushIn in{};
    in.hit = p.hit;
    in.hunter = p.hunter_pos;
    in.target = p.target_pos;
    in.blade_ok = p.blade;
    in.blade_vel = p.blade_vel;
    in.hit_speed = p.hit_speed;
    in.fallback_speed = p.tip_speed;
    if (p.stake_blast) {
        // Explosive force is independent of a slow trigger-release hand movement.
        // Use the existing away-from-player direction and modest upward component.
        in.blade_ok = false;
        in.fallback_speed = StakeBlastSpeed;
    }
    const R::PushPlan plan = R::PlanPush(in, bodies.data(), n, cfg);
    s32 pushed = 0;
    if (g_apply != nullptr) {
        for (s32 i = 0; i < n; ++i) {
            const R::Vec3 j = plan.impulse[i];
            if (VR::Body::LengthSq(j) <= 0.0f || !VR::Body::IsFinite(j)) {
                continue;
            }
            alignas(16) const std::array<float, 4> impulse{j.x, j.y, j.z, 0.0f};
            g_apply(world, read[i].id, impulse.data());
            ++pushed;
        }
    }
    g_ragdolls.fetch_add(1, std::memory_order_relaxed);
    const std::string skipped =
        p.death.ragdoll_at_s > 0.0f
            ? fmt::format("a{} would have gone limp after {:.2f} s", p.anim, p.death.ragdoll_at_s)
            : fmt::format("a{}", p.anim);
    const std::string calmed =
        (plan.calmed_bodies > 0
             ? fmt::format(", {} bodies slowed to {:.0f} m/s (by up to {:.1f})", plan.calmed_bodies,
                           cfg.inherit_max, plan.calmed)
             : std::string{}) +
        (plan.cancelled > 0.0f
             ? fmt::format(", up to {:.1f} m/s toward you taken out", plan.cancelled)
             : std::string{});
    if (pushed == 0 || cfg.scale <= 0.0f) {
        LOG_INFO(Core, "VR ragdoll: {} killed by {} -> ragdoll at once ({}), no push ({}){}",
                 Enemy(p), Attack(p), skipped,
                 world == nullptr     ? "no physics world"
                 : n == 0             ? "no bodies read"
                 : cfg.scale <= 0.0f  ? "melee_ragdoll_impulse=0"
                                      : "no dynamic body",
                 calmed);
        return;
    }
    LOG_INFO(Core,
             "VR ragdoll: {} killed by {} -> ragdoll at once ({}), {} push {:.2f} m/s (+{:.2f} "
             "m/s on the {} bodies struck, {:.0f} kg), blade {:.1f} m/s{} along {} (at most {:.0f} "
             "deg off away from you, {:.2f} m off), {:.1f} kg over {} bodies{}",
             Enemy(p), Attack(p), skipped, R::PushAttackName(p.push_attack), plan.dv_all,
             plan.dv_local, plan.local, plan.local_mass,
             plan.speed, plan.from_blade ? " at the hit" : " (no blade: the default)",
             V3(plan.dir), plan.away_max_deg, plan.distance, plan.total_mass, plan.dynamic, calmed);
}

void LogState(const Pending& p, u8* rag, const char* what) {
    u8* const world = PhysWorld();
    std::array<BodyRead, R::MaxBodies> read{};
    const s32 n = world != nullptr ? ReadBodies(rag, world, read) : 0;
    float vmax = 0.0f;
    float mass = 0.0f;
    float closest = 1e9f;  // a body's horizontal distance to where the hunter stood at the hit
    float toward = 0.0f;   // the fastest horizontal motion toward him
    for (s32 i = 0; i < n; ++i) {
        const R::PushBody& b = read[i].body;
        vmax = std::max(vmax, VR::Body::Length(b.vel));
        mass += b.mass;
        const R::Vec3 d{p.hunter_pos.x - b.com.x, 0.0f, p.hunter_pos.z - b.com.z};
        const float dist = VR::Body::Length(d);
        closest = std::min(closest, dist);
        if (dist > 1e-3f) {
            toward = std::max(toward, VR::Body::Dot(R::Vec3{b.vel.x, 0.0f, b.vel.z}, d) / dist);
        }
    }
    R::Vec3 pos{};
    Position(p.target, pos);
    LOG_INFO(Core,
             "VR ragdoll: {} {} u{} hp={} state {} root {} | body0 {} v{} | max |v| {:.2f} m/s, "
             "toward you {:.2f} m/s, closest {:.2f} m from you, {:.1f} kg over {} bodies",
             what, Enemy(p), p.updates, Hp(p.target), Read<s32>(rag + OffsetRagdollState), V3(pos),
             n > 0 ? V3(read[0].body.com) : std::string{"-"},
             n > 0 ? V3(read[0].body.vel) : std::string{"-"}, vmax, toward, closest, mass, n);
}

// The guard (each update after the push, on the enemy's own thread like the push): the bodies within
// Guard.radius of where the hunter stands now lose their horizontal motion toward him.
void GuardStep(Pending& p, u8* rag) {
    if (g_apply == nullptr) {
        return;
    }
    R::Vec3 hunter = p.hunter_pos;
    if (Plausible(p.player) && Read<u32>(p.player + OffsetHandle) == p.hunter) {
        Position(p.player, hunter);
    }
    u8* const world = PhysWorld();
    std::array<BodyRead, R::MaxBodies> read{};
    const s32 n = world != nullptr ? ReadBodies(rag, world, read) : 0;
    for (s32 i = 0; i < n; ++i) {
        float removed = 0.0f;
        const R::Vec3 j = R::GuardImpulse(read[i].body, hunter, Guard.radius, &removed);
        if (removed <= 0.0f || !VR::Body::IsFinite(j)) {
            continue;
        }
        alignas(16) const std::array<float, 4> impulse{j.x, j.y, j.z, 0.0f};
        g_apply(world, read[i].id, impulse.data());
        p.guard_max = std::max(p.guard_max, removed);
        ++p.guard_bodies;
    }
}

// An armed enemy's ChrCtrl ragdoll update: false when none of the kills is this ChrCtrl's.
bool KillUpdate(u8* ctrl) {
    std::scoped_lock lock{g_mutex};
    size_t i = 0;
    while (i < g_pending.size() &&
           (g_pending[i].phase == Phase::Free || g_pending[i].ctrl != ctrl)) {
        ++i;
    }
    if (i == g_pending.size()) {
        for (size_t k = 0; k < KillSlots; ++k) {
            if (g_watch[k].load(std::memory_order_relaxed) == reinterpret_cast<u64>(ctrl)) {
                g_watch[k].store(0, std::memory_order_release); // a stale watch
            }
        }
        return false;
    }
    Pending& p = g_pending[i];
    if (!Plausible(p.target) || Read<u32>(p.target + OffsetHandle) != p.handle ||
        RagdollCtrl(p.target) != ctrl) {
        LOG_INFO(Core, "VR ragdoll: {} killed by {}: kept the death animation: the enemy is gone",
                 Enemy(p), Attack(p));
        Release(i);
        return true;
    }
    u8* const rag = Read<u8*>(ctrl + OffsetRagdoll);
    ++p.updates;
    switch (p.phase) {
    case Phase::Armed: {
        if (!RagdollWanted()) {
            // Switched off (the VR page) between the blow and its death: the game's own death.
            LOG_INFO(Core, "VR ragdoll: {} killed by {}: kept the death animation: "
                           "melee_kill_ragdoll=0 now",
                     Enemy(p), Attack(p));
            Release(i);
            return true;
        }
        const s32 hp = Hp(p.target);
        const bool killer = Read<u32>(p.target + OffsetKiller) == p.hunter;
        if (p.death.kind == R::Death::Unknown) {
            p.death = ScanDeath(p);
        }
        const R::Step step = R::DecideStep(p.updates, hp, killer, p.death.kind);
        if (step == R::Step::Wait) {
            return true;
        }
        if (step != R::Step::Ragdoll) {
            LOG_INFO(Core, "VR ragdoll: {} killed by {}: kept the death animation: {}{}", Enemy(p),
                     Attack(p), R::StepName(step),
                     p.anim >= 0 ? fmt::format(" (a{})", p.anim) : std::string{});
            Release(i);
            return true;
        }
        // It would go limp at once. melee_ragdoll_vanilla_pct: with that chance (one roll per kill)
        // it keeps the game's own death instead: nothing written, the death animation plays to its
        // end and the game's own event 228 hands it to the ragdoll there, no push. A charged
        // attack's kill and the stake's blast never roll: they always go limp here.
        if (!p.stake_blast && !p.charged &&
            R::RollVanilla(p.roll, VR::GetSettings().melee_ragdoll_vanilla_pct, g_dice)) {
            LOG_INFO(Core,
                     "VR ragdoll: {} killed by {} -> kept the death animation: the {}% chance "
                     "(melee_ragdoll_vanilla_pct={}): a{} plays to its end, the game's ragdoll "
                     "after {:.2f} s, no push",
                     Enemy(p), Attack(p), p.roll.pct, p.roll.pct, p.anim, p.death.ragdoll_at_s);
            Release(i);
            return true;
        }
        // What the death animation's event 228 writes at its end (args 0, 0): the ChrCtrl update
        // this call goes on into switches the RagdollIns to state 2 at once.
        const s32 request = 1;
        const float weight = 1.0f;
        std::memcpy(ctrl + OffsetRagdollRequest, &request, sizeof(request));
        std::memcpy(ctrl + OffsetRagdollWeight, &weight, sizeof(weight));
        p.phase = Phase::Switched;
        p.switched_at = p.updates;
        if (g_log) {
            LogState(p, rag, "requested");
        }
        return true;
    }
    case Phase::Switched: {
        const s32 state = Read<s32>(rag + OffsetRagdollState);
        if (state == RagdollStateDead) {
            if (g_log) {
                LogState(p, rag, "dynamic");
            }
            Push(p, rag);
            if (g_log) {
                LogState(p, rag, "pushed");
            }
            p.phase = Phase::After;
            p.pushed_ns = NowNs();
            return true;
        }
        if (p.updates - p.switched_at >= 3) {
            LOG_INFO(Core,
                     "VR ragdoll: {} killed by {}: the ragdoll was asked for but stayed in state "
                     "{}; no push (its death animation goes on)",
                     Enemy(p), Attack(p), state);
            Release(i);
        }
        return true;
    }
    case Phase::After: {
        const s32 since = p.updates - p.switched_at;
        const bool guarding =
            R::Guarding(Guard, since, static_cast<float>(NowNs() - p.pushed_ns) * 1e-9f);
        if (guarding) {
            GuardStep(p, rag);
        }
        if (g_log && (since == 3 || since == 5 || since == 10 || since == 20 ||
                      since >= AfterLogUpdates)) {
            LogState(p, rag, "after");
        }
        if (!guarding && (!g_log || since >= AfterLogUpdates)) {
            if (p.guard_bodies > 0) {
                LOG_INFO(Core,
                         "VR ragdoll: {}: kept off you: up to {:.2f} m/s toward you taken out "
                         "({} body-updates within {:.1f} m, over {} updates)",
                         Enemy(p), p.guard_max, p.guard_bodies, Guard.radius, since);
            }
            Release(i);
        }
        return true;
    }
    case Phase::Free:
        break;
    }
    return true;
}

// The entry stub's call (the ChrCtrl ragdoll update of an armed enemy or of a watched corpse, on
// its own thread): a kill first; a corpse while no kill of it is pending.
// This probe runs only on its target's own ChrCtrl update. The blend matches
// TAE 228: request 1 at weight 1, request 2 for intermediate weights, -1 at 0.
// It intentionally does not try to move the living enemy yet.
bool RecoveryTestStep(u8* ctrl) {
    if (g_watch[HookSlot].load(std::memory_order_acquire) != reinterpret_cast<u64>(ctrl))
        return false;
    std::scoped_lock lock{g_recovery_mutex};
    auto& p = g_recovery;
    if (p.ctrl != ctrl)
        return false; // the watch changed while this callback waited for the lock
    const auto clear = [&] {
        g_watch[HookSlot].store(0, std::memory_order_release);
        p = {};
    };
    u8* const owner = Read<u8*>(ctrl + OffsetCtrlOwner);
    if (p.ctrl != ctrl || owner != p.target || !Plausible(owner) ||
        Read<u32>(owner + OffsetHandle) != p.handle || RagdollCtrl(owner) != ctrl) {
        LOG_WARNING(Core, "VR Arm recovery TEST: target ownership changed; abandoned");
        clear();
        return false;
    }
    const s32 hp = Hp(owner);
    if (hp <= 0 || InThrow(owner)) {
        LOG_WARNING(Core, "VR Arm recovery TEST: stopped, hp {} or native throw; game owns it", hp);
        // Death keeps its dynamic body. A throw owns its animation/ragdoll request.
        clear();
        return false;
    }
    u8* const rag = Read<u8*>(ctrl + OffsetRagdoll);
    const s64 now = NowNs();
    const s32 state = Read<s32>(rag + OffsetRagdollState);
    const auto request = [&](s32 value, float weight) {
        std::memcpy(ctrl + OffsetRagdollRequest, &value, sizeof(value));
        std::memcpy(ctrl + OffsetRagdollWeight, &weight, sizeof(weight));
    };
    if (p.phase == 0) {
        if (state != 0 || Read<s32>(ctrl + OffsetRagdollRequest) != -1) {
            LOG_WARNING(Core, "VR Arm recovery TEST: refused initial state {}, request {}", state,
                        Read<s32>(ctrl + OffsetRagdollRequest));
            clear();
            return false;
        }
        p.start = p.phase_start = now;
        p.phase = 1;
        request(1, 1.f);
        LOG_INFO(Core, "VR Arm recovery TEST: c{} hp {} requesting dynamic", p.chr_id, hp);
    } else if (p.phase == 1) {
        p.dynamic |= state == 2;
        request(1, 1.f);
        if (now - p.start >= 2'000'000'000) {
            p.phase = 2;
            p.phase_start = now;
        }
    } else if (p.phase == 2) {
        const float blend =
            std::clamp(static_cast<float>(now - p.phase_start) * 1e-9f / .6f, 0.f, 1.f);
        request(blend < 1.f ? 2 : -1, 1.f - blend);
        if (blend >= 1.f) {
            g_toggle_ragdoll(ctrl, false);
            p.phase = 3;
            p.phase_start = now;
        }
    } else {
        p.restored |= state == 0;
        if (now - p.phase_start >= 5'000'000'000) {
            R::Vec3 root{};
            Position(owner, root);
            LOG_INFO(Core,
                     "VR Arm recovery TEST: COMPLETE c{} dynamic={} restored={} hp={}->{} "
                     "root drift {:.3f}m; no forces applied",
                     p.chr_id, p.dynamic, p.restored, p.hp, hp, VR::Body::Length(root - p.root));
            clear();
            return true;
        }
    }
    if (now >= p.log_at) {
        R::Vec3 root{};
        Position(owner, root);
        LOG_INFO(Core,
                 "VR Arm recovery TEST: phase {} state {} request {} hp {} root "
                 "({:.3f},{:.3f},{:.3f})",
                 p.phase, state, Read<s32>(ctrl + OffsetRagdollRequest), hp, root.x, root.y,
                 root.z);
        p.log_at = now + 250'000'000;
    }
    return true;
}

bool ArmHookStep(u8* ctrl) {
    if (g_watch[HookSlot].load(std::memory_order_acquire) != reinterpret_cast<u64>(ctrl) ||
        g_arm_target.load(std::memory_order_acquire) == 0)
        return false;
    std::scoped_lock lock{g_recovery_mutex};
    auto& p = g_arm;
    if (p.ctrl != ctrl)
        return false; // never clear a newer target on behalf of an old callback
    u8* const owner = Read<u8*>(ctrl + OffsetCtrlOwner);
    if (p.ctrl != ctrl || owner != p.target || !Plausible(owner) ||
        Read<u32>(owner + OffsetHandle) != p.handle || RagdollCtrl(owner) != ctrl ||
        Read<u8*>(ctrl + OffsetRagdoll) != p.rag) {
        ClearArm();
        return false;
    }
    // Never restore HP, mask death, or compete with the game's grab/visceral.
    if (Hp(owner) <= 0 || InThrow(owner)) {
        ClearArm();
        return false;
    }
    if (Read<u8*>(p.rag + OffsetRagdollSystem) != p.system || PhysWorld() != p.world) {
        // A rebuilt physics system invalidates every captured body ID. Restore
        // this still-live owner's animation, but never apply to the recycled IDs.
        g_toggle_ragdoll(ctrl, false);
        ClearArm();
        return true;
    }
    const s64 now = NowNs();
    const s32 state = Read<s32>(p.rag + OffsetRagdollState);
    const auto request = [&](s32 value, float weight) {
        std::memcpy(ctrl + OffsetRagdollRequest, &value, sizeof(value));
        std::memcpy(ctrl + OffsetRagdollWeight, &weight, sizeof(weight));
    };
    const auto phase = [&](ArmPhase next) {
        p.phase = next;
        p.changed = now;
    };
    const float dt = p.tick > 0 && now > p.tick
                         ? std::min(static_cast<float>(now - p.tick) * 1e-9f, .025f)
                         : 0.f;
    p.tick = now;
    p.status.ns = now;
    Position(owner, p.status.root);
    if (p.phase == ArmPhase::Armed) {
        if (!p.input.held || !p.input.clear || now < p.input.ns ||
            now - p.input.ns > AmygdalanHook::MaxAgeNs || state != 0 ||
            Read<s32>(ctrl + OffsetRagdollRequest) != -1) {
            ClearArm(); // did not own it: nothing to restore
            return false;
        }
        request(1, 1.f);
        phase(ArmPhase::Held);
        LOG_INFO(Core, "VR Arm hook: caught c{} handle {:#x}", Read<s32>(owner + OffsetChrId),
                 p.handle);
        // Reuse the existing hand-grip feedback pattern; this event has no damage,
        // hit-stop or stamina effects. The right hand gets one confirmed-catch tick.
        Combat::PushMeleeEvent({Combat::MeleeEventKind::Corpse, 1, 1.f, 0.f, now, "Arm hook"});
        return true;
    }
    if (p.phase == ArmPhase::Down) {
        // Limp on the ground while its knockdown runs on fast underneath to where it lies and
        // starts to get up (or GetUpWaitNs passes: the plain blend into whatever it plays).
        request(1, 1.f);
        s32 clip = -1;
        float time = 0.f;
        const bool there =
            NewestClip(owner, clip, time) && clip % 1000000 == p.getup && time >= p.up_at;
        if (there || now - p.changed > GetUpWaitNs) {
            LOG_INFO(Core, "VR Arm hook: {:#x} {} (clip {} at {:.2f} s)", p.handle,
                     there ? "gets up on its own knockdown" : "never reached its get-up: blended",
                     clip, time);
            if (!there)
                p.getup = -1;
            phase(ArmPhase::Blend);
        }
        return true;
    }
    if (p.phase == ArmPhase::Blend) {
        const float span = p.getup >= 0 ? GetUpBlendS : .6f;
        const float blend =
            std::clamp(static_cast<float>(now - p.changed) * 1e-9f / span, 0.f, 1.f);
        request(blend < 1.f ? 2 : -1, 1.f - blend);
        if (blend >= 1.f) {
            g_toggle_ragdoll(ctrl, false);
            phase(ArmPhase::Verify);
        }
        return true;
    }
    if (p.phase == ArmPhase::Verify) {
        request(-1, 0.f);
        if (state == 0 || now - p.changed > 1'000'000'000) {
            LOG_INFO(Core,
                     "VR Arm hook: released {:#x}, animation state {}, hp {}, {} pulls, "
                     "impulse {:.3f}Ns, root displacement {:.3f}m",
                     p.handle, state, Hp(owner), p.pulls, p.impulse,
                     Body::Length(p.status.root - p.status.original_root));
            ClearArm();
        }
        return true;
    }
    request(1, 1.f);
    std::array<BodyRead, R::MaxBodies> bodies{};
    u8* const world = PhysWorld();
    const s32 count = world ? ReadBodies(p.rag, world, bodies) : 0;
    BodyRead* caught = nullptr;
    BodyRead* pelvis = nullptr;
    for (s32 i = 0; i < count; ++i) {
        if (bodies[i].id == p.body)
            caught = &bodies[i];
        if (bodies[i].id == p.pelvis)
            pelvis = &bodies[i];
    }
    const bool physics = state == 2 && caught && pelvis && caught->body.dynamic &&
                         caught->body.mass > .01f && caught->body.mass < 10000.f &&
                         Body::IsFinite(caught->body.com) && Body::IsFinite(caught->body.vel) &&
                         Body::IsFinite(caught->rotation) && Body::IsFinite(pelvis->body.com);
    if (physics) {
        p.status.point =
            caught->body.com + Body::Rotate(Body::Normalize(caught->rotation), p.offset);
        p.status.pelvis = pelvis->body.com;
    }
    if (p.phase == ArmPhase::Held) {
        const auto& in = p.input;
        const bool fresh = in.ns > 0 && now >= in.ns && now - in.ns <= AmygdalanHook::MaxAgeNs;
        const bool keep =
            fresh && in.held && in.clear && in.target == p.handle &&
            now - p.start < 10'000'000'000 && physics &&
            Body::Length(p.status.point - in.grip) <=
                std::min(p.length + 1.2f, AmygdalanHook::MaxTetherM) * in.scale;
        if (!keep) {
            LOG_INFO(Core,
                     "VR Arm hook: letting go {:#x}, fresh={} held={} clear={} physics={} "
                     "velocity ({:.2f},{:.2f},{:.2f}) mass {:.2f}kg",
                     p.handle, fresh, in.held, in.clear, physics,
                     physics ? caught->body.vel.x : 0.f, physics ? caught->body.vel.y : 0.f,
                     physics ? caught->body.vel.z : 0.f, physics ? caught->body.mass : 0.f);
            p.status.attached = false;
            // Let go mid-swing (the trigger, not a broken line): thrown on at the lever tip's
            // speed (AmygdalanHook::Launch), its pelvis and the hooked part (the rest of the body
            // follows them). A slow hand adds nothing: native momentum only.
            if (fresh && !in.held && physics && p.lever_ns > 0 &&
                now - p.lever_ns <= AmygdalanHook::MaxAgeNs) {
                for (BodyRead* b : {caught, pelvis}) {
                    if (b == pelvis && pelvis == caught)
                        continue;
                    const Body::Vec3 j = AmygdalanHook::Launch(p.lever_v * (1.f / in.scale),
                                                               b->body.vel * (1.f / in.scale),
                                                               b->body.mass) *
                                         in.scale;
                    if (b->body.dynamic && Body::LengthSq(j) > 0.f) {
                        alignas(16) const float v[4]{j.x, j.y, j.z, 0};
                        g_apply(world, b->id, v);
                        p.impulse += Body::Length(j);
                    }
                }
            }
            phase(ArmPhase::Coast);
        } else {
            // The reel: the rope takes up the slack as the enemy comes nearer (never under
            // ReelMinM), so each pull brings it closer and it stays there.
            p.length = AmygdalanHook::Reel(p.length, Body::Length(p.status.point - in.grip) / in.scale);
            // Swung (AmygdalanHook::LeverWay): pulled toward the lever's tip, the rope's length
            // out from the hand the way the tentacle turned with it; else toward the hand.
            const bool lever = Body::LengthSq(in.lever) > .25f;
            const Body::Vec3 anchor = lever ? in.grip + in.lever * (p.length * in.scale) : in.grip;
            const Body::Vec3 anchor_velocity =
                lever && p.lever_ns > 0 && dt > 0.f ? (anchor - p.lever_tip) * (1.f / dt)
                                                    : in.velocity;
            p.lever_tip = anchor;
            p.lever_v = anchor_velocity;
            p.lever_ns = lever ? now : 0;
            const auto impulse =
                AmygdalanHook::PullImpulse(
                    {anchor * (1.f / in.scale), p.status.point * (1.f / in.scale),
                     anchor_velocity * (1.f / in.scale), caught->body.vel * (1.f / in.scale),
                     lever ? AmygdalanHook::LeverSlackM : p.length, caught->body.mass, dt,
                     in.strength, true, lever ? AmygdalanHook::LeverReachM : 1.5f,
                     lever ? AmygdalanHook::LeverForceScale : 1.f,
                     lever ? AmygdalanHook::LeverMaxMps : 7.f}) *
                in.scale;
            if (Body::LengthSq(impulse) > 0.f) {
                alignas(16) const float j[4]{impulse.x, impulse.y, impulse.z, 0};
                g_apply(world, caught->id, j);
                ++p.pulls;
                p.impulse += Body::Length(impulse);
            }
            // Swung, the body goes with it: its pelvis pulled the same way the hooked part has
            // to go (toward the lever's tip, as far off it as the pelvis is from the part), with
            // its own Strength-capped pull. On the part alone a swing dragged a 155 kg beast by a
            // 5 kg limb about a metre (the mock, 2026-11-06).
            if (lever && pelvis != caught && pelvis->body.dynamic && pelvis->body.mass > .01f &&
                pelvis->body.mass < 10000.f && Body::IsFinite(pelvis->body.vel)) {
                const Body::Vec3 pelvis_anchor = anchor + (p.status.pelvis - p.status.point);
                const auto drag =
                    AmygdalanHook::PullImpulse(
                        {pelvis_anchor * (1.f / in.scale), p.status.pelvis * (1.f / in.scale),
                         anchor_velocity * (1.f / in.scale), pelvis->body.vel * (1.f / in.scale),
                         AmygdalanHook::LeverSlackM, pelvis->body.mass, dt, in.strength, true,
                         AmygdalanHook::LeverReachM, AmygdalanHook::LeverForceScale,
                         AmygdalanHook::LeverMaxMps}) *
                    in.scale;
                if (Body::LengthSq(drag) > 0.f) {
                    alignas(16) const float j[4]{drag.x, drag.y, drag.z, 0};
                    g_apply(world, pelvis->id, j);
                    p.impulse += Body::Length(drag);
                }
            }
        }
    }
    if (p.phase == ArmPhase::Coast &&
        (now - p.changed > CoastMaxNs ||
         (physics && now - p.changed > 300'000'000 && Body::Length(caught->body.vel) < .6f))) {
        phase(ArmPhase::Lie); // landed: it lies there a moment, stunned
    }
    if (p.phase == ArmPhase::Lie && now - p.changed > LieNs) {
        // ChrCtrl's native request == 1 path (0x1514ED5..0x1515016) already follows
        // the ragdoll body with the character root. Keep that position; never
        // teleport a living enemy or restore its pre-catch location ourselves.
        // Its own knockdown, when it has one (ragdoll_getups.h), gets it up on its legs.
        const R::GetUp* const up = R::FindGetUp(Read<s32>(owner + OffsetChrId));
        if (up && ForceAnimation(owner, up->clip)) {
            p.getup = up->clip;
            p.up_at = up->up_ms * 1e-3f - GetUpLeadS;
            phase(ArmPhase::Down);
        } else {
            phase(ArmPhase::Blend);
        }
    }
    return true;
}

void PS4_SYSV_ABI CtrlRagdollHook(u8* ctrl, u8* saved_ymm) {
    (void)saved_ymm;
    if (ArmHookStep(ctrl))
        return;
    if (RecoveryTestStep(ctrl))
        return;
    if (KillUpdate(ctrl)) {
        return;
    }
    for (size_t k = KillSlots; k < HookSlot; ++k) {
        if (g_watch[k].load(std::memory_order_acquire) == reinterpret_cast<u64>(ctrl)) {
            CorpseStep(ctrl);
            return;
        }
    }
}

void Arm(u8* res, u8* player, u8* target) {
    g_kills.fetch_add(1, std::memory_order_relaxed);
    Pending p{};
    p.target = target;
    p.handle = Read<u32>(target + OffsetHandle);
    p.hunter = Read<u32>(player + OffsetHandle);
    p.player = player;
    p.chr_id = Read<s32>(target + OffsetChrId);
    p.npc = Read<s32>(target + OffsetNpcParam);
    p.atk = Read<s32>(res + ResAtkParam);
    MeleeHitInfo info{};
    bool swing = MeleeHitInfoOf(res, info);
    if (!swing) {
        // The right weapon's own left part (the transformed Beast Claw's left claw, a split
        // blade's second blade): its own swing, and no blade pose (that one is the right hand's).
        MeleeHitInfo left{};
        if (MeleeLeftHitInfoOf(res, left)) {
            info = left;
            swing = true;
        }
    }
    if (swing) {
        if (R::HeavyTwoHandMomentum(info.weapon_variation, info.form_b,
                                   info.two_handed, info.transform) > 1.0f) {
            p.push_attack = R::PushAttack::HeavyTwoHand;
        }
        // A charged attack's kill: its own push, the stronger one also for a two-handed heavy.
        p.charged = info.charged;
        if (p.charged) {
            p.push_attack = R::PushAttack::Charged;
        }
        p.swing = true;
        p.serial = info.serial;
        p.kind = info.kind;
        p.transform = info.transform;
        p.tip_speed = info.tip_speed;
    }
    p.stake_blast = R::IsStakeBlast(Read<s32>(res + 0x38), p.atk,
                                   Read<s32>(res + ResBullet), Read<s32>(res + ResHand),
                                   Read<u8>(res + ResCategory));
    if (p.stake_blast) p.push_attack = R::PushAttack::StakeBlast;
    p.hit = {Read<float>(res + ResHitPosition), Read<float>(res + ResHitPosition + 4),
             Read<float>(res + ResHitPosition + 8)};
    if (!Position(player, p.hunter_pos)) {
        p.hunter_pos = p.hit;
    }
    if (!Position(target, p.target_pos)) {
        p.target_pos = p.hit;
    }
    if (info.blade) {
        p.blade_vel = R::BladeVelocityAt(info.base, info.tip, info.base_vel, info.tip_vel, p.hit);
        p.game_tip = VR::Body::Length(info.tip_vel);
        p.hit_speed = R::HitSpeed(VR::Body::Length(p.blade_vel), p.game_tip, p.tip_speed);
        p.blade = VR::Body::IsFinite(p.blade_vel);
    }
    u8* const ctrl = RagdollCtrl(target);
    if (ctrl == nullptr || !R::ChrCanRagdoll(p.chr_id)) {
        LOG_INFO(Core, "VR ragdoll: {} killed by {}: kept the death animation: {}", Enemy(p),
                 Attack(p),
                 ctrl == nullptr ? "it has no ragdoll of its own"
                                 : "none of its deaths ends in the ragdoll (a boss, a dissolve, an "
                                   "NPC hunter, a scripted enemy)");
        return;
    }
    p.ctrl = ctrl;
    p.ring_next = RingWrite(target);
    p.armed_ns = NowNs();
    p.phase = Phase::Armed;
    std::scoped_lock lock{g_mutex};
    size_t slot = g_pending.size();
    for (size_t i = 0; i < g_pending.size() && slot == g_pending.size(); ++i) {
        if (g_pending[i].phase != Phase::Free && g_pending[i].target == target) {
            slot = i; // a second lethal hit in the same frame: the newest blow
        }
    }
    for (size_t i = 0; i < g_pending.size() && slot == g_pending.size(); ++i) {
        if (g_pending[i].phase == Phase::Free ||
            p.armed_ns - g_pending[i].armed_ns > StaleNs) {
            slot = i;
        }
    }
    if (slot == g_pending.size()) {
        LOG_INFO(Core, "VR ragdoll: {} killed by {}: kept the death animation: four kills pending",
                 Enemy(p), Attack(p));
        return;
    }
    if (g_pending[slot].phase != Phase::Free && g_pending[slot].target == target) {
        p.ring_next = g_pending[slot].ring_next; // keep looking from the first blow
        p.roll = g_pending[slot].roll;           // and never roll the same kill twice
    }
    g_pending[slot] = p;
    g_watch[slot].store(reinterpret_cast<u64>(ctrl), std::memory_order_release);
    if (g_log) {
        LOG_INFO(Core,
                 "VR ragdoll: armed {} killed by {} hit {} hunter {} enemy {} blade {} v {} "
                 "({:.1f} m/s; the game's tip {:.1f}, the controller's tip {:.1f} -> {:.1f} m/s "
                 "at the hit) ring {}",
                 Enemy(p), Attack(p), V3(p.hit), V3(p.hunter_pos), V3(p.target_pos),
                 p.blade ? "at the hit" : "unknown", V3(p.blade_vel),
                 VR::Body::Length(p.blade_vel), p.game_tip, p.tip_speed, p.hit_speed, p.ring_next);
    }
}

} // Anonymous namespace

bool RagdollWanted() {
    return VR::GetSettings().melee_kill_ragdoll;
}

bool RagdollUpdateHooked() {
    return g_installed;
}

void WatchCorpse(size_t slot, u8* ctrl) {
    if (slot < CorpseWatchSlots) {
        g_watch[KillSlots + slot].store(reinterpret_cast<u64>(ctrl), std::memory_order_release);
    }
}

bool AmygdalanRecoveryTest(u8* target) {
    const char* test = std::getenv("VR_AMYGDALA_HOOK_TEST");
    if (!g_installed || !g_toggle_ragdoll || !test || std::strcmp(test, "1") != 0 ||
        !Plausible(target))
        return false;
    const s32 id = Read<s32>(target + OffsetChrId);
    u8* const ctrl = RagdollCtrl(target);
    const s32 hp = Hp(target);
    if (id <= 0 || !R::ChrCanRagdoll(id) || !ctrl || hp <= 0 || InThrow(target))
        return false;
    std::scoped_lock lock{g_recovery_mutex};
    if (g_recovery.ctrl || g_arm.ctrl)
        return false;
    RecoveryTest p{};
    p.target = target;
    p.ctrl = ctrl;
    p.handle = Read<u32>(target + OffsetHandle);
    p.chr_id = id;
    p.hp = hp;
    if (!Position(target, p.root))
        return false;
    g_recovery = p;
    g_watch[HookSlot].store(reinterpret_cast<u64>(ctrl), std::memory_order_release);
    LOG_INFO(Core, "VR Arm recovery TEST: watching c{} handle {:#x}, hp {}", id, p.handle, hp);
    return true;
}

void ArmHookHooksReady() {
    const bool ready = g_installed && g_toggle_ragdoll && IsStaggerSpeedInstalled();
    AmygdalanHook::backend_ready.store(ready, std::memory_order_release);
    if (AmygdalanHook::Enabled())
        LOG_INFO(Core, "VR Arm hook: experimental hook {} (native recovery and frame-time hooks)",
                 ready ? "ready" : "unavailable; normal trigger retained");
}

bool BeginArmHook(u8* target, const ArmHookInput& in, Body::Vec3 contact, float rope) {
    if (!AmygdalanHook::Active() || !g_installed || !g_toggle_ragdoll ||
        !IsStaggerSpeedInstalled() || !Plausible(target) || !in.held || !in.clear || !in.player ||
        !Body::IsFinite(contact) || !Body::IsFinite(in.grip) || !Body::IsFinite(in.velocity) ||
        !std::isfinite(in.scale) || in.scale < .25f || in.scale > 4.f)
        return false;
    const s64 now = NowNs();
    if (in.ns <= 0 || now < in.ns || now - in.ns > AmygdalanHook::MaxAgeNs)
        return false;
    const s32 model = Read<s32>(target + OffsetChrId);
    // Any enemy whose death ends in its ragdoll (78 chrs); the adapter latches the heavy ones and
    // those with none instead (core/vr/amygdalan_hook.h AnchoredKg), never making them limp.
    if (!R::ChrCanRagdoll(model) ||
        Read<u32>(target + OffsetHandle) != in.target || Hp(target) <= 0 || InThrow(target))
        return false;
    u8* const ctrl = RagdollCtrl(target);
    if (!ctrl)
        return false;
    u8* const rag = Read<u8*>(ctrl + OffsetRagdoll);
    if (Read<s32>(rag + OffsetRagdollState) != 0 || Read<s32>(ctrl + OffsetRagdollRequest) != -1)
        return false;
    std::scoped_lock lock{g_recovery_mutex};
    if (g_recovery.ctrl || g_arm.ctrl)
        return false;
    u8* const world = PhysWorld();
    std::array<BodyRead, R::MaxBodies> bodies{};
    const s32 count = world ? ReadBodies(rag, world, bodies) : 0;
    if (count < 2)
        return false;
    s32 nearest = -1;
    float best = .65f * in.scale;
    for (s32 i = 0; i < count; ++i) {
        const float distance = Body::Length(bodies[i].body.com - contact);
        if (Body::IsFinite(bodies[i].rotation) && std::isfinite(distance) && distance < best) {
            nearest = i;
            best = distance;
        }
    }
    if (nearest < 0)
        return false;
    ArmHold p{};
    p.target = target;
    p.ctrl = ctrl;
    p.rag = rag;
    p.system = Read<u8*>(rag + OffsetRagdollSystem);
    p.world = world;
    p.handle = in.target;
    p.input = in;
    p.body = bodies[nearest].id;
    p.pelvis = bodies[0].id;
    p.offset = Body::Rotate(Body::Conjugate(Body::Normalize(bodies[nearest].rotation)),
                            contact - bodies[nearest].body.com);
    p.length = rope >= 0.f ? rope : Body::Length(contact - in.grip) / in.scale;
    if (!std::isfinite(p.length) || p.length < .3f || p.length > AmygdalanHook::MaxRopeM ||
        !Position(target, p.status.original_root))
        return false;
    p.start = p.changed = now;
    p.status.ns = now;
    p.status.player = in.player;
    p.status.target = in.target;
    p.status.root = p.status.original_root;
    p.status.point = contact;
    p.status.pelvis = bodies[0].body.com;
    p.status.attached = true;
    g_arm = p;
    g_watch[HookSlot].store(reinterpret_cast<u64>(ctrl), std::memory_order_release);
    g_arm_target.store(reinterpret_cast<u64>(target), std::memory_order_release);
    return true;
}

void UpdateArmHook(const ArmHookInput& in) {
    if (!g_arm_target.load(std::memory_order_acquire))
        return;
    std::scoped_lock lock{g_recovery_mutex};
    if (!g_arm.ctrl)
        return;
    AmygdalanHook::ContinueHold(g_arm.input, in, NowNs());
}

ArmHookStatus ArmHookNow() {
    if (!g_arm_target.load(std::memory_order_acquire))
        return {};
    std::scoped_lock lock{g_recovery_mutex};
    return g_arm.status;
}

void ForgetArmHook(u32 missing_handle) {
    std::scoped_lock lock{g_recovery_mutex};
    if (g_arm.handle == missing_handle)
        ClearArm();
}

const std::atomic<u64>* ArmHookTargetSlot() {
    return &g_arm_target;
}

float ArmHookAnimationRate(u8* chr) {
    if (g_arm_target.load(std::memory_order_acquire) != reinterpret_cast<u64>(chr))
        return 1.f;
    std::scoped_lock lock{g_recovery_mutex};
    const auto& p = g_arm;
    if (p.target != chr || Read<u32>(chr + OffsetHandle) != p.handle || Hp(chr) <= 0 ||
        InThrow(chr))
        return 1.f;
    const s64 now = NowNs();
    if (p.tick && (now < p.tick || now - p.tick > 1'000'000'000))
        return 1.f;
    return p.phase == ArmPhase::Down                                   ? GetUpFastRate
           : p.phase == ArmPhase::Blend || p.phase == ArmPhase::Verify ? 1.f
                                                                       : 0.f;
}

float RagdollOnPlayerHit(u8* res, u8* player, float damage) {
    if (!g_installed || res == nullptr || player == nullptr || !RagdollWanted()) {
        return damage;
    }
    u8* const target = Read<u8*>(res + ResTarget);
    if (!Plausible(target) || target == player) {
        return damage;
    }
    R::BlowIn b{};
    b.attacker_is_player = true; // the detour hands over the main player's hits only
    b.target_is_player = false;
    b.bullet = Read<s32>(res + ResBullet);
    b.hand = Read<s32>(res + ResHand);
    b.category = Read<u8>(res + ResCategory);
    b.hp = Hp(target);
    b.target_in_throw = InThrow(target);
    b.damage = damage;
    float out = damage;
    const bool melee = b.bullet == R::NoBullet && b.hand == R::HandRight &&
                       b.category == R::CategoryRightWeapon && !b.target_in_throw;
    if (g_oneshot && (melee || g_oneshot_any) && b.hp > 0 && std::isfinite(damage) &&
        damage > 0.0f) {
        out = std::max(damage, static_cast<float>(b.hp) + 1.0f);
        b.damage = out;
    }
    const R::Blow blow = R::ClassifyBlow(b);
    if (blow == R::Blow::Kill) {
        Arm(res, player, target);
    } else if (b.hp > 0 && R::HitDamage(b.damage) >= b.hp) {
        LOG_INFO(Core, "VR ragdoll: c{:04d} (NpcParam {}) killed by {} (atk {}): kept the death "
                       "animation",
                 Read<s32>(target + OffsetChrId), Read<s32>(target + OffsetNpcParam),
                 R::BlowName(blow), Read<s32>(res + ResAtkParam));
    }
    return out;
}

void SeverOnPlayerHit(u8* res, u8* player, float damage) {
    // No dismembering weapon in the hand: one test of a word per hit and nothing else.
    if ((SeverAwake() & SeverAwakeWeapon) == 0) {
        return;
    }
    // Read once (thread-safe statics); both off, two tests of a bool per hit and nothing else.
    static const bool log = std::getenv("VR_SEVER_LOG") != nullptr;
    const bool cut = SeverEnabled();
    if ((!log && !cut) || res == nullptr || player == nullptr) {
        return;
    }
    u8* const target = Read<u8*>(res + ResTarget);
    if (!Plausible(target) || target == player) {
        return;
    }
    MeleeHitInfo info{};
    if (!(MeleeHitInfoOf(res, info) || MeleeLeftHitInfoOf(res, info))) {
        return;
    }
    // Any hit of a dismembering weapon, either form: its enemy's model read ahead of its kill
    // (once per enemy type: the cut then never waits for its files).
    if (VR::Sever::CanWake(info.weapon_variation)) {
        SeverPrefetch(target);
    }
    // The eligible right-hand swing, or the transformed Beast Claw's own left claw.
    if (!VR::Sever::CanSever(info.weapon_variation, info.form_b, info.chikage_draw) ||
        (info.left && info.weapon_variation != VR::Sever::BeastClawVariation)) {
        return;
    }
    R::BlowIn b{};
    b.attacker_is_player = true;
    b.bullet = Read<s32>(res + ResBullet);
    b.hand = Read<s32>(res + ResHand);
    b.category = Read<u8>(res + ResCategory);
    if (info.left) {
        // The claw's left claw (its attack matched by MeleeLeftHitInfoOf): the right weapon's own
        // blow, whatever hand and category the game's result names (the review, 2026-11-13: not
        // seen in a run yet).
        b.hand = R::HandRight;
        b.category = R::CategoryRightWeapon;
    }
    b.hp = Hp(target);
    b.target_in_throw = InThrow(target);
    b.damage = damage;
    VR::Sever::Hit h{};
    h.weapon_variation = info.weapon_variation;
    h.swing = info.swing;
    h.heavy = info.heavy;
    h.judged_light = info.judged_light;
    h.transform = info.transform;
    h.kill = R::ClassifyBlow(b) == R::Blow::Kill;
    h.tip_mps = info.tip_peak;
    h.arc_deg = info.arc_deg;
    h.form_b = info.form_b;
    h.charged = info.charged;
    h.chikage_draw = info.chikage_draw;
    // Only a kill matters (and, logged, a heavy too).
    if (!h.kill && !(log && h.heavy)) {
        return;
    }
    // Ordinary nonlethal hits need no random draw (or shared atomic update).
    h.roll = SeverRoll();
    // Everything but where it was struck first (its class, its corpse, the roll): the struck
    // body's name (a dozen fault-safe reads) and the weighing only for a blow that will cut (the
    // review, 2026-11-13: every claw kill and heavy paid 10-50 us for them).
    h.supported = cut && SeverCutsCharacter(target);
    h.severed = h.supported && SeverKnown(target);
    if (!log) {
        VR::Sever::Hit any_limb = h;
        any_limb.site.limb = VR::Sever::Limb::Head;
        if (VR::Sever::Decide(any_limb) != VR::Sever::Verdict::Sever) {
            return;
        }
    }
    std::array<char, 64> body{};
    const bool named = HitBodyName(res, target, body);
    h.site = named ? VR::Sever::SiteOf(body.data()) : VR::Sever::Site{};
    // In half: a small enemy only (its hit capsules as flesh, as the Amygdalan Arm weighs one).
    float kg = -1.0f;
    if (h.site.limb == VR::Sever::Limb::Torso) {
        static EnemyCapsules caps; // Game:Main, as the melee code's own
        R::Vec3 at{};
        Position(target, at);
        kg = 0.0f;
        if (GatherEnemyCapsules({at.x, at.y, at.z}, 8.0f, 1.0f, caps) > 0) {
            const u32 handle = Read<u32>(target + OffsetHandle);
            for (size_t i = 0; i < caps.count; ++i) {
                if (caps.handle[i] != handle) {
                    continue;
                }
                const auto& c = caps.capsules[i];
                const float r = c.radius, l = VR::Body::Length(c.b - c.a);
                kg += (3.14159265f * r * r * l + 4.0f / 3.0f * 3.14159265f * r * r * r) *
                      VR::AmygdalanHook::FleshKgPerM3;
            }
        }
        // (heavier, or not weighed: the worker takes its head or the nearest limb instead)
    }
    // Every hooked enemy is tried (core/vr/bloodborne_sever.cpp checks its skeleton and model
    // before a cut); each corpse once.
    const VR::Sever::Verdict verdict = VR::Sever::Decide(h);
    const R::Vec3 at{Read<float>(res + ResHitPosition), Read<float>(res + ResHitPosition + 4),
                     Read<float>(res + ResHitPosition + 8)};
    if (cut && verdict == VR::Sever::Verdict::Sever) {
        SeverKill(target, h.site, {at.x, at.y, at.z}, info,
                  h.site.limb == VR::Sever::Limb::Torso &&
                      (!(kg > 0.0f) || kg > VR::Sever::MaxHalfKg));
    }
    // At most 64 lines a session.
    static std::atomic<u32> lines{0};
    if (!log || lines.fetch_add(1, std::memory_order_relaxed) >= 64) {
        return;
    }
    LOG_INFO(Core,
             "VR sever: c{:04d} (NpcParam {}) {} {}{} {}{} on body #{} '{}' (hp {} dmg "
             "{:.1f}{}): {}{}; contact ({:.2f}, {:.2f}, {:.2f}), tip {:.1f} m/s (peak {:.1f}), "
             "arc {:.0f} deg: chance {:.0f}%, rolled {:.0f}{}",
             Read<s32>(target + OffsetChrId), Read<s32>(target + OffsetNpcParam),
             info.weapon_variation == VR::Sever::BurialBladeVariation ? "Burial Blade"
             : info.weapon_variation == VR::Sever::ChikageVariation   ? "Chikage's quick draw"
                                                                      : "Beast Claw",
             info.form_b ? "form B" : "form A",
             info.left ? fmt::format(" left claw (the game's hand {}, category {})",
                                     Read<s32>(res + ResHand), Read<u8>(res + ResCategory))
                       : std::string{},
             info.kind,
             info.judged_light ? " (judged light)" : "",
             Read<s32>(res + ResBodyIndex), named ? body.data() : "?", b.hp, damage,
             h.kill ? ", a kill" : "",
             h.site.limb == VR::Sever::Limb::None
                 ? std::string{"no limb"}
                 : fmt::format("the {} through the {}", VR::Sever::LimbName(h.site.limb),
                               VR::Sever::CutName(h.site)),
             verdict == VR::Sever::Verdict::Sever
                 ? std::string{cut ? ": severed" : ": would sever"} +
                       (kg >= 0.0f ? fmt::format(" ({:.0f} kg{})", kg,
                                                 kg > VR::Sever::MaxHalfKg || !(kg > 0.0f)
                                                     ? ": too heavy to halve"
                                                     : "")
                                   : std::string{})
                 : fmt::format(" -> {}", VR::Sever::VerdictName(verdict)),
             at.x, at.y, at.z, info.tip_speed, info.tip_peak, info.arc_deg,
             100.0f * VR::Sever::HitChance(h), 100.0f * h.roll,
             info.blade ? "" : " (no blade pose)");
}

void InstallRagdoll(VAddr base, u64 size) {
    if (g_code) {
        return;
    }
    auto* const module = reinterpret_cast<u8*>(base);
    g_base = module;
    g_log = std::getenv("VR_RAGDOLL_LOG") != nullptr;
    const char* const oneshot = std::getenv("VR_RAGDOLL_ONESHOT");
    g_oneshot = oneshot != nullptr;
    g_oneshot_any = oneshot != nullptr && std::string{oneshot} == "any";
    // melee_ragdoll_vanilla_pct's die, seeded once.
    u64 seed = static_cast<u64>(NowNs());
    try {
        std::random_device rd;
        seed ^= (static_cast<u64>(rd()) << 32) ^ rd();
    } catch (...) {
        // the clock alone
    }
    g_dice.Seed(seed);
    // Installed whatever melee_kill_ragdoll says at the start: the VR page (Assists > Instant
    // ragdoll) switches it live, and the damage detour reads it at every hit (RagdollOnPlayerHit).
    // Off, nothing is armed: the entry's compares find no watched ChrCtrl.
    if (!IsOffhandDamageInstalled()) {
        LOG_WARNING(Core, "VR ragdoll: not installed (the final-damage detour is not in): every "
                          "death the game's own");
        return;
    }
    const auto matches = [&](u64 rva, const auto& sig) {
        return size > rva + sig.size() && std::memcmp(module + rva, sig.data(), sig.size()) == 0;
    };
    const bool ctrl_ok = matches(CtrlRagdollRva, CtrlRagdollSig);
    const bool ev_ok = matches(Ev228CtrlRva, Ev228CtrlSig) &&
                       matches(FullRagdollStoreRva, FullRagdollStoreSig);
    const bool killer_ok = matches(KillerStoreRva, KillerStoreSig);
    const bool impulse_ok = matches(ApplyLinearImpulseRva, ApplyLinearImpulseSig);
    const bool vtable_ok = size > RagdollVtableRva + 8 && size > PhysWorldRva + 8;
    const auto verdict = [](bool ok) { return ok ? "ok" : "MISMATCH"; };
    if (!ctrl_ok || !ev_ok || !killer_ok || !impulse_ok || !vtable_ok) {
        LOG_WARNING(Core,
                    "VR ragdoll: not installed (ChrCtrl ragdoll update {}, event 228 store {}, "
                    "killer store {}, applyBodyLinearImpulse {}, tables {}); live bytes at {:#x}: "
                    "{}: every death the game's own",
                    verdict(ctrl_ok), verdict(ev_ok), verdict(killer_ok), verdict(impulse_ok),
                    verdict(vtable_ok), CtrlRagdollRva,
                    size > CtrlRagdollRva + CtrlRagdollSig.size()
                        ? Hook::HexBytes(module + CtrlRagdollRva, CtrlRagdollSig.size())
                        : std::string("out of range"));
        return;
    }
    u8* const site = module + CtrlRagdollRva;
    void* entry = nullptr;
    try {
        using namespace Xbyak::util;
        g_code = std::make_unique<Xbyak::CodeGenerator>(1024);
        auto& c = *g_code;
        void* const host = Hook::EmitHostCallStub(
            c, reinterpret_cast<void*>(&CtrlRagdollHook), CtrlRagdollSig.data(),
            CtrlRagdollPatchLen, reinterpret_cast<u64>(site + CtrlRagdollPatchLen), rdi);
        // The entry: one compare per watched ChrCtrl (rax is dead at the function's entry: the
        // replayed prologue loads it), else the prologue and back.
        c.align(16);
        entry = const_cast<u8*>(c.getCurr());
        Xbyak::Label hit;
        c.mov(rax, reinterpret_cast<u64>(g_watch.data()));
        const char* probe = std::getenv("VR_AMYGDALA_HOOK_TEST");
        const size_t watches = AmygdalanHook::Enabled() || (probe && std::strcmp(probe, "1") == 0)
                                   ? g_watch.size()
                                   : HookSlot;
        for (size_t i = 0; i < watches; ++i) {
            c.cmp(rdi, ptr[rax + static_cast<u32>(i * sizeof(u64))]);
            c.je(hit, Xbyak::CodeGenerator::T_NEAR);
        }
        for (size_t i = 0; i < CtrlRagdollPatchLen; ++i) {
            c.db(CtrlRagdollSig[i]);
        }
        c.db(0xFF); // jmp qword [rip+0]
        c.db(0x25);
        c.dd(0);
        c.dq(reinterpret_cast<u64>(site + CtrlRagdollPatchLen));
        c.L(hit);
        c.jmp(host, Xbyak::CodeGenerator::T_NEAR);
        c.ready();
    } catch (const Xbyak::Error& error) {
        LOG_ERROR(Core, "VR ragdoll: code emission failed ({}); not installed", error.what());
        g_code.reset();
        return;
    }
    if (!Hook::MakeWritable(site, CtrlRagdollPatchLen)) {
        LOG_ERROR(Core, "VR ragdoll: could not make {:#x} writable; not installed",
                  CtrlRagdollRva);
        return;
    }
    g_apply = reinterpret_cast<ApplyImpulseFn>(module + ApplyLinearImpulseRva);
    if (matches(ToggleRagdollRva, ToggleRagdollSig) && matches(ResetRagdollRva, ResetRagdollSig)) {
        g_toggle_ragdoll = reinterpret_cast<ToggleRagdollFn>(module + ToggleRagdollRva);
    } else {
        LOG_WARNING(Core, "VR Arm hook: native ragdoll reset signature mismatch; hook unavailable");
    }
    g_installed = true;
    Hook::WriteJump(site, CtrlRagdollPatchLen, entry);
    LOG_INFO(Core,
             "VR ragdoll: installed at {:#x} (ChrCtrl ragdoll update, event 228 store, killer "
             "store, applyBodyLinearImpulse: sig ok): a right-hand melee kill cuts {} death "
             "animations short, push x{:.2f}, the game's own death on {}% of them{}{}{}",
             CtrlRagdollRva, R::RagdollDeaths.size(), VR::GetSettings().melee_ragdoll_impulse,
             std::clamp(VR::GetSettings().melee_ragdoll_vanilla_pct, 0, 100),
             RagdollWanted() ? ""
                             : " (melee_kill_ragdoll=0 now: every death the game's own until "
                               "the VR page turns it on)",
             g_log ? ", VR_RAGDOLL_LOG" : "", g_oneshot_any ? ", VR_RAGDOLL_ONESHOT=any (tests)"
             : g_oneshot   ? ", VR_RAGDOLL_ONESHOT (tests)"
                           : "");
}

} // namespace VR::Bloodborne
