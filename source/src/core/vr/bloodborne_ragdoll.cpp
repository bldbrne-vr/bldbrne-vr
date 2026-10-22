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
#include "core/vr/bloodborne_melee.h"
#include "core/vr/bloodborne_ragdoll.h"
#include "core/vr/bloodborne_tuning.h"
#include "core/vr/body_channel.h"
#include "core/vr/hook_util.h"
#include "core/vr/ragdoll_kill.h"
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
constexpr u32 ResTarget = 0x1E8;

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
// The ChrCtrl pointers the entry stub compares with (0 = none); written under g_mutex.
alignas(64) std::array<std::atomic<u64>, 4> g_watch{};
static_assert(sizeof(std::atomic<u64>) == sizeof(u64));
std::atomic<u64> g_kills{0};
std::atomic<u64> g_ragdolls{0};
// melee_ragdoll_vanilla_pct's die: seeded once at install, drawn under g_mutex (one roll per kill).
R::KillDice g_dice{};

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
        in.fallback_speed = 8.0f;
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
             "VR ragdoll: {} killed by {} -> ragdoll at once ({}), push {:.2f} m/s (+{:.2f} m/s "
             "on the {} bodies struck, {:.0f} kg), blade {:.1f} m/s{} along {} (at most {:.0f} deg "
             "off away from you, {:.2f} m off), {:.1f} kg over {} bodies{}",
             Enemy(p), Attack(p), skipped, plan.dv_all, plan.dv_local, plan.local, plan.local_mass,
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

// The entry stub's call (the ChrCtrl ragdoll update of an armed enemy, on its own thread).
void PS4_SYSV_ABI CtrlRagdollHook(u8* ctrl, u8* saved_ymm) {
    (void)saved_ymm;
    std::scoped_lock lock{g_mutex};
    size_t i = 0;
    while (i < g_pending.size() &&
           (g_pending[i].phase == Phase::Free || g_pending[i].ctrl != ctrl)) {
        ++i;
    }
    if (i == g_pending.size()) {
        for (auto& w : g_watch) {
            if (w.load(std::memory_order_relaxed) == reinterpret_cast<u64>(ctrl)) {
                w.store(0, std::memory_order_release); // a stale watch
            }
        }
        return;
    }
    Pending& p = g_pending[i];
    if (!Plausible(p.target) || Read<u32>(p.target + OffsetHandle) != p.handle ||
        RagdollCtrl(p.target) != ctrl) {
        LOG_INFO(Core, "VR ragdoll: {} killed by {}: kept the death animation: the enemy is gone",
                 Enemy(p), Attack(p));
        Release(i);
        return;
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
            return;
        }
        const s32 hp = Hp(p.target);
        const bool killer = Read<u32>(p.target + OffsetKiller) == p.hunter;
        if (p.death.kind == R::Death::Unknown) {
            p.death = ScanDeath(p);
        }
        const R::Step step = R::DecideStep(p.updates, hp, killer, p.death.kind);
        if (step == R::Step::Wait) {
            return;
        }
        if (step != R::Step::Ragdoll) {
            LOG_INFO(Core, "VR ragdoll: {} killed by {}: kept the death animation: {}{}", Enemy(p),
                     Attack(p), R::StepName(step),
                     p.anim >= 0 ? fmt::format(" (a{})", p.anim) : std::string{});
            Release(i);
            return;
        }
        // It would go limp at once. melee_ragdoll_vanilla_pct: with that chance (one roll per kill)
        // it keeps the game's own death instead: nothing written, the death animation plays to its
        // end and the game's own event 228 hands it to the ragdoll there, no push.
        if (!p.stake_blast && R::RollVanilla(p.roll, VR::GetSettings().melee_ragdoll_vanilla_pct, g_dice)) {
            LOG_INFO(Core,
                     "VR ragdoll: {} killed by {} -> kept the death animation: the {}% chance "
                     "(melee_ragdoll_vanilla_pct={}): a{} plays to its end, the game's ragdoll "
                     "after {:.2f} s, no push",
                     Enemy(p), Attack(p), p.roll.pct, p.roll.pct, p.anim, p.death.ragdoll_at_s);
            Release(i);
            return;
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
        return;
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
            return;
        }
        if (p.updates - p.switched_at >= 3) {
            LOG_INFO(Core,
                     "VR ragdoll: {} killed by {}: the ragdoll was asked for but stayed in state "
                     "{}; no push (its death animation goes on)",
                     Enemy(p), Attack(p), state);
            Release(i);
        }
        return;
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
        return;
    }
    case Phase::Free:
        break;
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
    if (MeleeHitInfoOf(res, info)) {
        if (R::HeavyTwoHandMomentum(info.weapon_variation, info.form_b,
                                   info.two_handed, info.transform) > 1.0f) {
            p.push_attack = R::PushAttack::HeavyTwoHand;
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
        for (size_t i = 0; i < g_watch.size(); ++i) {
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
