// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "common/logging/log.h"
#include "core/vr/bloodborne_corpse.h"
#include "core/vr/bloodborne_ragdoll.h"
#include "core/vr/body_channel.h"
#include "core/vr/combat_channel.h"
#include "core/vr/corpse_hands.h"
#include "core/vr/hook_util.h"
#include "core/vr/seqlock.h"
#include "core/vr/vr_session.h"

namespace VR::Bloodborne {

namespace {

namespace K = VR::Corpse;
namespace B = VR::Body;
using Hook::Plausible;
using Hook::Read;
using K::Vec3;

// ---- Sites -----------------------------------------------------------------------------------------

// hknpWorld::applyBodyLinearImpulse(world, s32 body, const hkVector4* impulse) and
// hknpWorld::applyBodyImpulseAt(world, s32 body, const hkVector4* impulse, const hkVector4* point):
// return for a body that is not dynamic, activate it, then linVel += impulse * invMass (and, at a
// point, angVel += invInertia * (q^-1 ((point - com) x impulse)), in the motion's frame), and tell
// the body's attachments and the world's listeners. The prologues and the body lookup.
constexpr u64 ApplyLinearRva = 0xB68DB0;
constexpr std::array<u8, 41> ApplyLinearSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
    0x83, 0xEC, 0x28, 0x49, 0x89, 0xD4, 0x41, 0x89, 0xF7, 0x49, 0x89, 0xFD, 0x49, 0x63,
    0xC7, 0x4D, 0x8B, 0x75, 0x20, 0x48, 0x8D, 0x1C, 0xC0, 0x48, 0xC1, 0xE3, 0x04,
};
constexpr u64 ApplyAtRva = 0xB69150;
constexpr std::array<u8, 48> ApplyAtSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC,
    0x28, 0x49, 0x89, 0xCE, 0x49, 0x89, 0xD5, 0x89, 0x75, 0xB4, 0x48, 0x63, 0xC6, 0x4C, 0x8B, 0x7F,
    0x20, 0x48, 0x8D, 0x1C, 0xC0, 0x48, 0xC1, 0xE3, 0x04, 0x41, 0x8B, 0x44, 0x1F, 0x40, 0xA8, 0x02,
};
using LinearFn = void(PS4_SYSV_ABI*)(void* world, s32 body, const float* impulse);
using AtFn = void(PS4_SYSV_ABI*)(void* world, s32 body, const float* impulse, const float* point);

constexpr u64 WorldChrManRva = 0x553E878;
// WorldChrMan's characters (as bloodborne_deflect.cpp's ChrByHandle): the blocks below
// [[mgr+0x08]+0x18] + 2, each block's chr set [mgr + 0x850 + 8 x block] {s32 count, +0x08 entries
// of 0x38 bytes, each starting with its ChrIns}.
constexpr u32 OffsetMgrBlocks = 0x08;
constexpr u32 OffsetBlocksCount = 0x18;
constexpr u32 OffsetMgrChrSets = 0x850;
constexpr u32 OffsetSetEntries = 0x08;
constexpr u32 ChrEntrySize = 0x38;
constexpr s32 MaxBlocks = 64;     // a handle's block is 6 bits
constexpr s32 MaxSetCount = 4096; // ... its slot 14
constexpr u64 RagdollVtableRva = 0x538D440; // RagdollIns
constexpr u64 PhysWorldRva = 0x553D700;     // [[+0x28]+0x08] = the hknpWorld
constexpr u64 CapsuleVtableRva = 0x52B1450; // hknpCapsuleShape
constexpr u64 SphereVtableRva = 0x52AAF20;  // hknpSphereShape

// ChrIns
constexpr u32 OffsetHandle = 0x08;
constexpr u32 OffsetChrCtrl = 0x58;
constexpr u32 OffsetNpcParam = 0x70;
constexpr u32 OffsetChrId = 0x74;
constexpr u32 OffsetModules = 0x3B0;
constexpr u32 OffsetDataModule = 0x20;
constexpr u32 OffsetHp = 0xF8;
// ChrCtrl
constexpr u32 OffsetCtrlOwner = 0x08;
constexpr u32 OffsetRagdoll = 0x20;
constexpr u32 OffsetCtrlWorld = 0x90; // W: three float4 rows {R row i, t_i}
// RagdollIns
constexpr u32 OffsetRagdollOwner = 0x10;
constexpr u32 OffsetRagdollSystem = 0x38;
constexpr u32 OffsetSystemBodyIds = 0x20;
constexpr u32 OffsetSystemBodyCount = 0x28;
constexpr u32 OffsetRagdollState = 0xC0;
constexpr s32 RagdollStateDead = 2; // the full ragdoll: the bodies dynamic
// hknpWorld, hknpBody, hknpMotion, the shapes
constexpr u32 OffsetWorldBodies = 0x20;
constexpr u32 OffsetWorldMotions = 0xE0;
constexpr u32 BodyStride = 0x90;
constexpr u32 BodyTransform = 0x00; // rotation columns +0x00 / +0x10 / +0x20, translation +0x30
constexpr u32 BodyFlags = 0x40;
constexpr u32 BodyFlagDynamic = 0x2;
constexpr u32 BodyShape = 0x48;
constexpr u32 BodyMotionId = 0x68;
// Its place in the simulation (the active list's index or its sleeping island's id): -1 = not in
// it. Some of a ragdoll's parts are kept out of the simulation by the game (the mock, 2026-11-02:
// a few of a 60-part enemy's, flags 0x1402 against the others' 0x100A / 0xA, this -1, the
// motion's solver id 0x7FFFFFFF; the ragdoll itself stays simulated, its first body in it 40 s
// after the death). The world's activateBody (0xB57860, inside both impulse calls) returns at once
// for such a body and nothing moves it, so it counts as fixed (never pushed or held).
constexpr u32 BodyIsland = 0x6C;
constexpr u32 MotionStride = 0x80;
constexpr u32 MotionCom = 0x00;
constexpr u32 MotionOrientation = 0x10; // hkQuaternion x, y, z, w
constexpr u32 MotionInvInertia = 0x20;  // hkHalf[4]: x, y, z in its frame, w = the inverse mass
constexpr u32 MotionLinearVelocity = 0x40;
constexpr u32 MotionAngularVelocity = 0x50; // in the motion's frame
constexpr u32 ShapeRadius = 0x14;
constexpr u32 CapsuleA = 0x40;
constexpr u32 CapsuleB = 0x50;
constexpr u32 SphereCenter = 0x30;

constexpr s64 Ms = 1'000'000;
constexpr s64 ScanNs = 100 * Ms;      // a look for corpses near the hunter
// No pose this long: the hands are gone (all let go). Long enough for a hitch or a slow frame (the
// corpse's update may run before this frame's pose, on the last one's hands).
constexpr s64 HandsStaleNs = 400 * Ms;
constexpr float ReachM = 3.0f;        // a corpse watched within this of the hunter ...
constexpr float ReachUpM = 2.5f;      // ... (vertically)
constexpr float SpanM = 1.5f;         // its parts at most this far from its first (the pelvis)
constexpr float NearM = 3.0f;         // an update reads its parts with a bare hand this near that,
constexpr float NearMarginM = 0.3f;   // ... or once read, its parts' reach from it and this
constexpr s64 TouchTickNs = 250 * Ms; // a new touch's tick at most this often per hand
constexpr s64 LoadTickNs = 100 * Ms;  // the held weight's rumble
constexpr size_t Slots = 4;           // corpses watched at once
// A corpse the game does not move (the mock, before BodyIsland was known: a held part out of the
// simulation kept the velocity it was given and never moved; brought back in it would fly): its
// fastest dynamic part moving this fast for this many of its updates in a row without moving at
// all (a limb its joints hold back still moves a little: never frozen) ...
constexpr float FrozenSpeed = 0.3f;
constexpr int FrozenUpdates = 5;
constexpr float FrozenStillM = 1e-4f;
constexpr s64 FrozenNs = 3000 * Ms; // ... is left alone this long, its added motion taken back out
static_assert(Slots <= CorpseWatchSlots);

// ---- State -----------------------------------------------------------------------------------------

// The pose hook -> the corpses' updates (seqlock): the hands as they are now.
struct Hands {
    s64 ns{0};
    float scale{1.0f};
    int strength{};
    std::array<K::Hand, 2> hand{};
    std::array<s64, 2> squeeze_ns{}; // the squeeze past grab_on, while it lasts (0: none)
};
SeqLock<Hands> g_hands;
// The corpses' updates -> the pose hook (seqlock; stored under g_mutex: one writer at a time):
// where each holding hand's palm is on its corpse.
std::array<CorpseGrip, 2> g_grip{}; // under g_mutex
SeqLock<std::array<CorpseGrip, 2>> g_grips;
std::array<std::atomic<s64>, 2> g_grab_near_ns{};

struct Corpse {
    u8* chr{nullptr};
    u8* ctrl{nullptr};
    u32 handle{0};
    s32 chr_id{-1};
    s32 npc{-1};
    s64 last_ns{0};      // its last update
    s64 heavy_until{0};  // the extra gravity after a let-go
    std::array<bool, 2> touching{};
    std::array<s64, 2> touch_ns{};
    float span{0.0f}; // its parts' reach from its pelvis (0: not read yet)
    // The frozen check: its fastest part where the last update read it and how fast it went,
    // whether impulses went in then, and how many updates in a row it kept still while moving.
    s32 probe_part{-1};
    Vec3 probe{};
    float probe_speed{0.0f};
    bool pushed{false};
    int still{0};
    s64 frozen_until{0};
};

struct HandHold {
    K::Hold hold{};
    s32 slot{-1};          // the corpse's (g_corpses)
    u32 handle{0};
    s64 since{0};
    s64 used_squeeze{0};   // the squeeze that took it (one hold per squeeze)
    s64 load_ns{0};
    s64 log_ns{0};
    float load_max{0.0f};
    float part_mass{0.0f};
    float corpse_mass{0.0f};
    s32 parts{0};
    float start_y{0.0f};
    float top_y{0.0f};     // the held point's highest
};

u8* g_base = nullptr;
bool g_installed = false;
bool g_log = false; // VR_CORPSE_LOG: touches and the hold's numbers too
// VR_CORPSE_TEST=1 (the mock): a scripted right hand lifts, drags and shoves the nearest corpse;
// =2 first kills the nearest enemy with a ragdoll (its HP to 0) for one, and looks up to 45 m.
int g_test = 0;
constexpr float TestReachM = 45.0f;
LinearFn g_linear = nullptr;
AtFn g_at = nullptr;
std::mutex g_mutex; // g_corpses and g_hold (the pose hook and the corpses' own threads)
std::array<Corpse, Slots> g_corpses{};
std::array<HandHold, 2> g_hold{};
// A hand holds a corpse (written under g_mutex): the pose hook's check without the lock.
std::atomic<bool> g_holding{false};
// One corpse's parts and impulses, under g_mutex (the updates run on the game's threads, on their
// own small stacks: nothing of this size goes there).
struct Scratch {
    std::array<K::Part, K::MaxParts> parts{};
    std::array<s32, K::MaxParts> ids{};
    std::array<K::Impulse, 2 * K::MaxParts + 4> impulse{};
};
Scratch g_scratch{};

// VR_CORPSE_LOG: what it costs (the pose hook's part and the corpses' updates), every 10 s.
struct Cost {
    s64 since{0};
    u64 poses{0};
    s64 pose_ns{0};
    s64 pose_max{0};
    u64 scans{0};
    s64 scan_ns{0};
    u64 steps{0};
    u64 reads{0}; // the steps that read the parts (a bare hand near, or holding)
    s64 step_ns{0};
    s64 step_max{0};
    u64 impulses{0};
};
Cost g_cost{}; // the poses' and scans' under the pose hook, the steps' under g_mutex

void NoteCost(s64 now) {
    Cost& c = g_cost;
    if (c.since == 0) {
        c.since = now;
        return;
    }
    if (now - c.since < 10'000 * Ms) {
        return;
    }
    const auto us = [](s64 ns, u64 n) { return n > 0 ? static_cast<double>(ns) / n / 1e3 : 0.0; };
    LOG_INFO(Core,
             "VR corpse: cost {:.0f} s: pose hook {} calls avg {:.1f} us max {:.1f} us ({} scans "
             "avg {:.1f} us); corpse updates {} (parts read {}) avg {:.1f} us max {:.1f} us, {} "
             "impulses",
             static_cast<double>(now - c.since) * 1e-9, c.poses, us(c.pose_ns, c.poses),
             static_cast<double>(c.pose_max) / 1e3, c.scans, us(c.scan_ns, c.scans), c.steps,
             c.reads, us(c.step_ns, c.steps), static_cast<double>(c.step_max) / 1e3, c.impulses);
    c = {};
    c.since = now;
}

// The pose hook's own (one thread).
struct Pose {
    s64 last_ns{0};
    s64 scan_ns{0};
    std::array<Vec3, 2> palm{};
    std::array<Vec3, 2> v{};
    std::array<bool, 2> had{};
    std::array<float, 2> squeeze{};
    std::array<s64, 2> squeeze_ns{};
    bool watching{false};
};
Pose g_pose{};

const char* HandName(size_t h) {
    return h == 0 ? "left" : "right";
}

s64 NowNs() {
    return B::SteadyNowNs();
}

u8* Module(u8* chr, u32 offset) {
    u8* const mods = Plausible(chr) ? Read<u8*>(chr + OffsetModules) : nullptr;
    u8* const m = Plausible(mods) ? Read<u8*>(mods + offset) : nullptr;
    return Plausible(m) ? m : nullptr;
}

s32 Hp(u8* chr) {
    u8* const data = Module(chr, OffsetDataModule);
    return data != nullptr ? Read<s32>(data + OffsetHp) : 1;
}

// The chr's ChrCtrl when it has its own RagdollIns (out: it).
u8* RagdollCtrl(u8* chr, u8*& rag) {
    rag = nullptr;
    u8* const ctrl = Plausible(chr) ? Read<u8*>(chr + OffsetChrCtrl) : nullptr;
    if (!Plausible(ctrl) || Read<u8*>(ctrl + OffsetCtrlOwner) != chr) {
        return nullptr;
    }
    u8* const r = Read<u8*>(ctrl + OffsetRagdoll);
    if (!Plausible(r) || Read<u8*>(r) != g_base + RagdollVtableRva ||
        Read<u8*>(r + OffsetRagdollOwner) != chr) {
        return nullptr;
    }
    rag = r;
    return ctrl;
}

u8* PhysWorld() {
    u8* const a = Read<u8*>(g_base + PhysWorldRva);
    u8* const b = Plausible(a) ? Read<u8*>(a + 0x28) : nullptr;
    u8* const w = Plausible(b) ? Read<u8*>(b + 0x08) : nullptr;
    return Plausible(w) ? w : nullptr;
}

float Half(const u8* p) {
    const u32 bits = static_cast<u32>(Read<u16>(p)) << 16;
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return std::isfinite(f) ? f : 0.0f;
}

Vec3 V3(const u8* p) {
    return {Read<float>(p), Read<float>(p + 4), Read<float>(p + 8)};
}

// The ragdoll's parts as the physics world has them now (at most K::MaxParts), with their body ids.
// A part whose shape is neither a capsule nor a sphere, or whose numbers do not read, is left out.
s32 ReadParts(u8* rag, u8* world, std::array<K::Part, K::MaxParts>& parts,
              std::array<s32, K::MaxParts>& ids) {
    u8* const system = Read<u8*>(rag + OffsetRagdollSystem);
    if (!Plausible(system) || world == nullptr) {
        return 0;
    }
    u8* const body_ids = Read<u8*>(system + OffsetSystemBodyIds);
    const s32 count = Read<s32>(system + OffsetSystemBodyCount);
    u8* const bodies = Read<u8*>(world + OffsetWorldBodies);
    u8* const motions = Read<u8*>(world + OffsetWorldMotions);
    if (!Plausible(body_ids, 4) || count <= 0 || count > K::MaxParts || !Plausible(bodies, 16) ||
        !Plausible(motions, 16)) {
        return 0;
    }
    s32 n = 0;
    for (s32 i = 0; i < count; ++i) {
        const s32 id = Read<s32>(body_ids + i * 4);
        if (id < 0 || id > 0x100000) {
            continue;
        }
        u8* const body = bodies + static_cast<size_t>(id) * BodyStride;
        const s32 motion_id = Read<s32>(body + BodyMotionId);
        u8* const shape = Read<u8*>(body + BodyShape);
        if (motion_id <= 0 || motion_id > 0x100000 || !Plausible(shape)) {
            continue;
        }
        u8* const vtable = Read<u8*>(shape);
        const bool sphere = vtable == g_base + SphereVtableRva;
        if (!sphere && vtable != g_base + CapsuleVtableRva) {
            continue;
        }
        std::array<float, 16> m{};
        std::memcpy(m.data(), body + BodyTransform, sizeof(m));
        const auto world_of = [&m](const u8* p) {
            const Vec3 l = V3(p);
            return Vec3{m[0] * l.x + m[4] * l.y + m[8] * l.z + m[12],
                        m[1] * l.x + m[5] * l.y + m[9] * l.z + m[13],
                        m[2] * l.x + m[6] * l.y + m[10] * l.z + m[14]};
        };
        u8* const motion = motions + static_cast<size_t>(motion_id) * MotionStride;
        K::Part& p = parts[static_cast<size_t>(n)];
        p.a = world_of(shape + (sphere ? SphereCenter : CapsuleA));
        p.b = world_of(shape + (sphere ? SphereCenter : CapsuleB));
        p.r = Read<float>(shape + ShapeRadius);
        p.com = V3(motion + MotionCom);
        p.q = B::Normalize(B::Quat{Read<float>(motion + MotionOrientation),
                                   Read<float>(motion + MotionOrientation + 4),
                                   Read<float>(motion + MotionOrientation + 8),
                                   Read<float>(motion + MotionOrientation + 12)});
        p.inv_inertia = {Half(motion + MotionInvInertia), Half(motion + MotionInvInertia + 2),
                         Half(motion + MotionInvInertia + 4)};
        const bool dynamic = (Read<u32>(body + BodyFlags) & BodyFlagDynamic) != 0 &&
                             Read<s32>(body + BodyIsland) != -1;
        p.inv_mass = dynamic ? Half(motion + MotionInvInertia + 6) : 0.0f;
        p.v = V3(motion + MotionLinearVelocity);
        p.w = B::Rotate(p.q, V3(motion + MotionAngularVelocity));
        if (!B::IsFinite(p.a) || !B::IsFinite(p.b) || !B::IsFinite(p.com) || !B::IsFinite(p.v) ||
            !B::IsFinite(p.w) || !(p.r > 0.0f && p.r < 3.0f) ||
            !(p.inv_mass >= 0.0f && p.inv_mass < 1e4f)) {
            continue;
        }
        ids[static_cast<size_t>(n)] = id;
        ++n;
    }
    return n;
}

// Where the ragdoll's first part (the pelvis) is, its centre of mass: the cheap look before its parts.
// False too when that part is out of the simulation (BodyIsland).
bool RootCom(u8* rag, u8* world, Vec3& out) {
    u8* const system = Read<u8*>(rag + OffsetRagdollSystem);
    u8* const body_ids = Plausible(system) ? Read<u8*>(system + OffsetSystemBodyIds) : nullptr;
    if (!Plausible(body_ids, 4) || Read<s32>(system + OffsetSystemBodyCount) <= 0 ||
        world == nullptr) {
        return false;
    }
    u8* const bodies = Read<u8*>(world + OffsetWorldBodies);
    u8* const motions = Read<u8*>(world + OffsetWorldMotions);
    const s32 id = Read<s32>(body_ids);
    if (!Plausible(bodies, 16) || !Plausible(motions, 16) || id < 0 || id > 0x100000) {
        return false;
    }
    u8* const body = bodies + static_cast<size_t>(id) * BodyStride;
    const s32 motion_id = Read<s32>(body + BodyMotionId);
    if (motion_id <= 0 || motion_id > 0x100000 || Read<s32>(body + BodyIsland) == -1) {
        return false;
    }
    out = V3(motions + static_cast<size_t>(motion_id) * MotionStride + MotionCom);
    return B::IsFinite(out);
}

float MassOf(const K::Part* parts, s32 n) {
    float m = 0.0f;
    for (s32 i = 0; i < n; ++i) {
        m += K::Dynamic(parts[i]) ? 1.0f / parts[i].inv_mass : 0.0f;
    }
    return m;
}

std::string Enemy(const Corpse& c) {
    return fmt::format("c{:04d} (NpcParam {})", c.chr_id, c.npc);
}

void Event(size_t hand, float what, float b) {
    VR::Combat::MeleeEvent e{};
    e.kind = VR::Combat::MeleeEventKind::Corpse;
    e.serial = static_cast<u32>(hand);
    e.a = what;
    e.b = b;
    e.ns = NowNs();
    VR::Combat::PushMeleeEvent(e);
}

void Unwatch(size_t slot) {
    g_corpses[slot] = {};
    WatchCorpse(slot, nullptr);
}

// Under g_mutex, after a hold began or ended: the flag, and the drawn hands let go of the ones that
// ended.
void NoteHolding() {
    g_holding.store(g_hold[0].hold.on || g_hold[1].hold.on, std::memory_order_release);
    bool changed = false;
    for (size_t h = 0; h < 2; ++h) {
        if (g_grip[h].on && !g_hold[h].hold.on) {
            g_grip[h].on = false;
            changed = true;
        }
    }
    if (changed) {
        g_grips.Store(g_grip);
    }
}

// Under g_mutex: hand h holds its corpse with the palm at `palm` now.
void NoteGrip(size_t h, Vec3 palm, s64 now) {
    if (h < 2 && B::IsFinite(palm)) {
        g_grab_near_ns[h].store(now, std::memory_order_relaxed);
        g_grip[h] = {now, true, palm};
        g_grips.Store(g_grip);
    }
}

// ---- The corpses near the hunter (the pose hook) ---------------------------------------------------

struct Found {
    u8* chr{nullptr};
    u8* ctrl{nullptr};
    float d2{0.0f};
};

// The dead enemies with a ragdoll of their own within reach of the hunter (their pelvis within
// ReachM + SpanM), nearest first (at most Slots). A few reads per character, none of its parts.
size_t Scan(const CorpseHandsIn& in, std::array<Found, Slots>& found) {
    u8* const mgr = Read<u8*>(g_base + WorldChrManRva);
    u8* const blocks = Plausible(mgr) ? Read<u8*>(mgr + OffsetMgrBlocks) : nullptr;
    u8* const world = PhysWorld();
    if (!Plausible(blocks) || world == nullptr) {
        return 0;
    }
    const s32 nblocks = std::clamp(Read<s32>(blocks + OffsetBlocksCount) + 2, 0, MaxBlocks);
    const float reach = (g_test == 2 ? TestReachM : ReachM + SpanM) * in.scale;
    const float up = (g_test == 2 ? TestReachM : ReachUpM + SpanM) * in.scale;
    size_t n = 0;
    for (s32 b = 0; b < nblocks; ++b) {
        u8* const set = Read<u8*>(mgr + OffsetMgrChrSets + 8 * static_cast<u32>(b));
        if (!Plausible(set)) {
            continue;
        }
        const s32 count = std::clamp(Read<s32>(set), 0, MaxSetCount);
        u8* const entries = Read<u8*>(set + OffsetSetEntries);
        if (!Plausible(entries)) {
            continue;
        }
        for (s32 i = 0; i < count; ++i) {
            u8* const chr = Read<u8*>(entries + ChrEntrySize * static_cast<u32>(i));
            if (!Plausible(chr) || chr == in.player || Hp(chr) > 0) {
                continue;
            }
            u8* rag = nullptr;
            u8* const ctrl = RagdollCtrl(chr, rag);
            Vec3 root{};
            if (ctrl == nullptr || Read<s32>(rag + OffsetRagdollState) != RagdollStateDead ||
                !RootCom(rag, world, root)) {
                continue;
            }
            const Vec3 d = root - in.feet;
            const float d2 = d.x * d.x + d.z * d.z;
            if (std::abs(d.y) > up || d2 > reach * reach) {
                continue;
            }
            // Insert, nearest first.
            size_t at = n;
            while (at > 0 && found[at - 1].d2 > d2) {
                --at;
            }
            if (at >= Slots) {
                continue;
            }
            for (size_t k = std::min(n, Slots - 1); k > at; --k) {
                found[k] = found[k - 1];
            }
            found[at] = {chr, ctrl, d2};
            n = std::min(n + 1, Slots);
        }
    }
    return n;
}

// Under g_mutex: the watched corpses become `found`, keeping the ones a hand holds where they are.
void Rewatch(const std::array<Found, Slots>& found, size_t n) {
    std::array<bool, Slots> keep{};
    for (const HandHold& hh : g_hold) {
        if (hh.hold.on && hh.slot >= 0) {
            keep[static_cast<size_t>(hh.slot)] = true;
        }
    }
    std::array<bool, Slots> placed{};
    for (size_t f = 0; f < n; ++f) {
        for (size_t s = 0; s < Slots; ++s) {
            if (g_corpses[s].ctrl == found[f].ctrl && g_corpses[s].chr == found[f].chr) {
                placed[f] = true;
                keep[s] = true;
            }
        }
    }
    for (size_t s = 0; s < Slots; ++s) {
        if (!keep[s] && g_corpses[s].ctrl != nullptr) {
            Unwatch(s);
        }
    }
    for (size_t f = 0; f < n; ++f) {
        if (placed[f]) {
            continue;
        }
        for (size_t s = 0; s < Slots; ++s) {
            if (g_corpses[s].ctrl == nullptr) {
                Corpse c{};
                c.chr = found[f].chr;
                c.ctrl = found[f].ctrl;
                c.handle = Read<u32>(c.chr + OffsetHandle);
                c.chr_id = Read<s32>(c.chr + OffsetChrId);
                c.npc = Read<s32>(c.chr + OffsetNpcParam);
                g_corpses[s] = c;
                WatchCorpse(s, c.ctrl);
                if (g_log) {
                    LOG_INFO(Core, "VR corpse: watching {} ({:.2f} m off)", Enemy(c),
                             std::sqrt(found[f].d2));
                }
                break;
            }
        }
    }
}

// ---- VR_CORPSE_TEST (the mock) ---------------------------------------------------------------------

// The right hand, scripted: on the nearest watched corpse's highest part, it lifts it 0.6 m and
// holds it (run 0), drags it 1 m sideways (run 1), then shoves it (run 2: no squeeze).
struct Test {
    int run{0};
    s64 start{0};
    Vec3 from{};
    Vec3 side{};
    bool done{false};
};
Test g_test_state{};
u8* g_test_killed = nullptr; // VR_CORPSE_TEST=2's kill (the test works on it once it is a corpse)

// VR_CORPSE_TEST=2: once, 20 s after the hands first show, the nearest living enemy with a ragdoll
// of its own (within TestReachM) gets HP 0: the game's own death follows, its ragdoll at its end.
void TestKill(const CorpseHandsIn& in) {
    static s64 first = 0;
    static bool done = false;
    if (done) {
        return;
    }
    if (first == 0) {
        first = in.now;
        return;
    }
    if (in.now - first < 15'000 * Ms) {
        return;
    }
    done = true;
    u8* const mgr = Read<u8*>(g_base + WorldChrManRva);
    u8* const blocks = Plausible(mgr) ? Read<u8*>(mgr + OffsetMgrBlocks) : nullptr;
    if (!Plausible(blocks)) {
        return;
    }
    const s32 nblocks = std::clamp(Read<s32>(blocks + OffsetBlocksCount) + 2, 0, MaxBlocks);
    u8* best = nullptr;
    float best_d = TestReachM * in.scale;
    for (s32 b = 0; b < nblocks; ++b) {
        u8* const set = Read<u8*>(mgr + OffsetMgrChrSets + 8 * static_cast<u32>(b));
        u8* const entries = Plausible(set) ? Read<u8*>(set + OffsetSetEntries) : nullptr;
        if (!Plausible(entries)) {
            continue;
        }
        const s32 count = std::clamp(Read<s32>(set), 0, MaxSetCount);
        for (s32 i = 0; i < count; ++i) {
            u8* const chr = Read<u8*>(entries + ChrEntrySize * static_cast<u32>(i));
            u8* rag = nullptr;
            if (!Plausible(chr) || chr == in.player || Read<s32>(chr + OffsetChrId) == 0 ||
                Hp(chr) <= 0 || RagdollCtrl(chr, rag) == nullptr) {
                continue;
            }
            const u8* const w = Read<u8*>(chr + OffsetChrCtrl) + OffsetCtrlWorld;
            const Vec3 at{Read<float>(w + 0x0C), Read<float>(w + 0x1C), Read<float>(w + 0x2C)};
            const float d = B::Length(at - in.feet);
            if (B::IsFinite(at) && d < best_d) {
                best_d = d;
                best = chr;
            }
        }
    }
    u8* const data = Module(best, OffsetDataModule);
    if (data == nullptr) {
        LOG_INFO(Core, "VR corpse test: no enemy with a ragdoll within {:.0f} m to kill", TestReachM);
        return;
    }
    LOG_INFO(Core, "VR corpse test: killing c{:04d} (NpcParam {}) {:.1f} m off (HP {} -> 0)",
             Read<s32>(best + OffsetChrId), Read<s32>(best + OffsetNpcParam), best_d,
             Read<s32>(data + OffsetHp));
    const s32 zero = 0;
    std::memcpy(data + OffsetHp, &zero, sizeof(zero));
    std::scoped_lock lock{g_mutex};
    g_test_killed = best;
}

void TestHand(CorpseHandsIn& in) {
    Test& t = g_test_state;
    static s64 first = 0;
    if (first == 0) {
        first = in.now;
    }
    static const s64 after = [] {
        const char* v = std::getenv("VR_CORPSE_TEST_AFTER_S");
        return static_cast<s64>((v != nullptr ? std::atof(v) : 30.0) * 1e9);
    }();
    if (t.done || in.now - first < after) {
        return; // the arrival's shaders first (VR_CORPSE_TEST_AFTER_S, 30)
    }
    if (t.start == 0) {
        // The nearest watched corpse's highest part: the palm right on top of it.
        std::scoped_lock lock{g_mutex};
        u8* ctrl = nullptr;
        u8* chr = nullptr;
        u8* const world = PhysWorld();
        float best = 0.0f;
        for (const Corpse& c : g_corpses) {
            u8* r = nullptr;
            Vec3 root{};
            if (c.ctrl == nullptr || RagdollCtrl(c.chr, r) != c.ctrl ||
                !RootCom(r, world, root)) {
                continue;
            }
            const float d = B::Length(root - in.feet) - (c.chr == g_test_killed ? 1e4f : 0.0f);
            if (ctrl == nullptr || d < best) {
                best = d;
                ctrl = c.ctrl;
                chr = c.chr;
            }
        }
        u8* rag = nullptr;
        if (ctrl == nullptr || RagdollCtrl(chr, rag) != ctrl || world == nullptr ||
            // =2: its own kill, or another corpse right by the hunter
            (g_test == 2 && chr != g_test_killed && best > 3.0f * in.scale)) {
            in.bare[1] = true; // keep looking
            in.palm[1] = in.feet + Vec3{0.0f, 1.0f, 0.0f};
            in.squeeze[1] = 0.0f;
            return;
        }
        auto& parts = g_scratch.parts;
        const s32 n = ReadParts(rag, world, parts, g_scratch.ids);
        s32 top = -1;
        float top_score = 0.0f;
        for (s32 i = 0; i < n; ++i) {
            const K::Part& q = parts[static_cast<size_t>(i)];
            const float score = t.run == 0 ? B::Length(q.com - parts[0].com) : q.com.y;
            if (K::Dynamic(q) && q.inv_mass <= 1.0f && (top < 0 || score > top_score)) { // >= 1 kg
                top = i;
                top_score = score;
            }
        }
        if (top < 0) {
            return;
        }
        const K::Part& p = parts[static_cast<size_t>(top)];
        const K::Config cfg{};
        const float hand_r = cfg.hand_r * in.scale;
        const Vec3 away = B::Normalize(Vec3{p.com.x - in.feet.x, 0.0f, p.com.z - in.feet.z},
                                       {0.0f, 0.0f, 1.0f});
        t.side = B::Normalize(B::Cross({0.0f, 1.0f, 0.0f}, away), {1.0f, 0.0f, 0.0f});
        t.start = in.now;
        // Lift and drag: the palm on top of the part (1 cm into it); the shove: just off its side.
        const Vec3 axis = K::ClosestOnSegment(p.a, p.b, p.com);
        t.from = t.run < 2 ? axis + Vec3{0.0f, p.r + hand_r - 0.01f * in.scale, 0.0f}
                           : axis - t.side * (p.r + hand_r + 0.05f * in.scale);
        LOG_INFO(Core, "VR corpse test: run {} ({}) on part {} of {} at ({:.2f},{:.2f},{:.2f}), "
                       "{:.1f} kg of {:.1f} kg, the corpse {:.1f} m from the hunter",
                 t.run, t.run == 0 ? "lift its farthest limb" : t.run == 1 ? "drag" : "shove", top,
                 n, p.com.x, p.com.y, p.com.z, 1.0f / std::max(p.inv_mass, 1e-6f),
                 MassOf(parts.data(), n), best + (chr == g_test_killed ? 1e4f : 0.0f));
    }
    const float s = static_cast<float>(in.now - t.start) * 1e-9f;
    in.bare[1] = true;
    in.q[1] = {};
    Vec3 at = t.from;
    float squeeze = 0.0f;
    if (t.run == 0) { // lift 0.6 m over 1.2 s, hold 2 s, let go
        squeeze = s >= 0.3f && s < 3.5f ? 1.0f : 0.0f;
        at.y += 0.6f * in.scale * std::clamp((s - 0.4f) / 1.2f, 0.0f, 1.0f);
    } else if (t.run == 1) { // drag 1 m sideways over 2 s, let go
        squeeze = s >= 0.3f && s < 2.8f ? 1.0f : 0.0f;
        at += t.side * (1.0f * in.scale * std::clamp((s - 0.4f) / 2.0f, 0.0f, 1.0f));
    } else { // shove: the open palm into it at 1 m/s for 0.5 s
        at += t.side * (1.0f * in.scale * std::clamp(s - 0.3f, 0.0f, 0.5f));
    }
    in.palm[1] = at;
    in.squeeze[1] = squeeze;
    if (s > 5.0f) {
        t.start = 0;
        t.done = ++t.run > 2;
        if (t.done) {
            LOG_INFO(Core, "VR corpse test: done");
        }
    }
}

} // Anonymous namespace

void InstallCorpseHands(VAddr base, u64 size) {
    if (g_installed) {
        return;
    }
    auto* const module = reinterpret_cast<u8*>(base);
    g_base = module;
    g_log = std::getenv("VR_CORPSE_LOG") != nullptr;
    if (const char* test = std::getenv("VR_CORPSE_TEST")) {
        g_test = std::atoi(test);
        g_log = g_log || g_test != 0;
    }
    const auto matches = [&](u64 rva, const auto& sig) {
        return size > rva + sig.size() && std::memcmp(module + rva, sig.data(), sig.size()) == 0;
    };
    const bool linear_ok = matches(ApplyLinearRva, ApplyLinearSig);
    const bool at_ok = matches(ApplyAtRva, ApplyAtSig);
    const bool tables_ok = size > WorldChrManRva + 8 && size > PhysWorldRva + 8;
    if (!RagdollUpdateHooked() || !linear_ok || !at_ok || !tables_ok) {
        LOG_WARNING(Core,
                    "VR corpse: not installed (the ChrCtrl ragdoll update's stub {}, "
                    "applyBodyLinearImpulse {}, applyBodyImpulseAt {}, tables {}): bare hands "
                    "pass through corpses",
                    RagdollUpdateHooked() ? "in" : "NOT IN (see VR ragdoll)",
                    linear_ok ? "ok" : "MISMATCH", at_ok ? "ok" : "MISMATCH",
                    tables_ok ? "ok" : "out of range");
        return;
    }
    g_linear = reinterpret_cast<LinearFn>(module + ApplyLinearRva);
    g_at = reinterpret_cast<AtFn>(module + ApplyAtRva);
    g_installed = true;
    const K::Config cfg{};
    LOG_INFO(Core,
             "VR corpse: installed (applyBodyImpulseAt {:#x}, applyBodyLinearImpulse {:#x}: sig "
             "ok): bare hands push corpses ({:.0f} N) and hold them with a squeeze ({:.0f} to "
             "{:.0f} N per hand with Strength), the rest of a held corpse x{:.1f} gravity{}{}{}",
             ApplyAtRva, ApplyLinearRva, cfg.push_n, cfg.hold_n, K::HoldForce(99), 1.0f + cfg.extra_g,
             VR::GetSettings().corpse_hands ? "" : " (corpse_hands=0 now: off)",
             g_log ? ", VR_CORPSE_LOG" : "", g_test != 0 ? ", VR_CORPSE_TEST (scripted hand)" : "");
}

void CorpseHands(const CorpseHandsIn& in_) {
    if (!g_installed) {
        return;
    }
    const s64 t0 = g_log ? NowNs() : 0;
    struct Timed {
        s64 t0;
        ~Timed() {
            if (t0 != 0) {
                const s64 ns = NowNs() - t0;
                g_cost.poses += 1;
                g_cost.pose_ns += ns;
                g_cost.pose_max = std::max(g_cost.pose_max, ns);
            }
        }
    } timed{t0};
    CorpseHandsIn in = in_;
    const bool on = VR::GetSettings().corpse_hands;
    if (!on) {
        in.bare = {};
    }
    if (g_test == 2 && on) {
        TestKill(in);
    }
    if (g_test != 0 && on && g_pose.watching) {
        TestHand(in);
    }
    const K::Config cfg{};
    Pose& p = g_pose;
    const bool any = in.bare[0] || in.bare[1] || (g_test != 0 && on);
    const bool held = g_holding.load(std::memory_order_acquire);
    if (!any && !held) {
        // Armed (the usual case): nothing but this, once the corpses are let go of.
        if (p.watching) {
            std::scoped_lock lock{g_mutex};
            for (size_t s = 0; s < Slots; ++s) {
                Unwatch(s);
            }
            p.watching = false;
        }
        p.had = {};
        p.squeeze_ns = {};
        p.last_ns = 0;
        return;
    }
    // The palms' velocities (the hunter's own motion in them): from where they were last pose, or
    // none after a gap or a jump (a snap turn, a recentre).
    const float dt = p.last_ns != 0 ? static_cast<float>(in.now - p.last_ns) * 1e-9f : 0.0f;
    p.last_ns = in.now;
    Hands out{};
    out.ns = in.now;
    out.scale = in.scale;
    // Same v1.09 PlayerGameData and third attribute as the verified level-up routine in givekit.
    if (Plausible(in.player)) {
        const auto* data = Read<const u8*>(in.player + 0x3C0);
        if (Plausible(data)) out.strength = Read<s32>(data + 0x58);
    }
    bool edge = false;
    for (size_t h = 0; h < 2; ++h) {
        K::Hand& hand = out.hand[h];
        hand.bare = in.bare[h] && B::IsFinite(in.palm[h]);
        hand.p = in.palm[h];
        hand.q = B::Normalize(in.q[h]);
        hand.squeeze = std::isfinite(in.squeeze[h]) ? std::clamp(in.squeeze[h], 0.0f, 1.0f) : 0.0f;
        Vec3 v{};
        if (hand.bare && p.had[h] && dt > 1e-4f && dt <= 0.1f) {
            const Vec3 d = hand.p - p.palm[h];
            if (B::Length(d) < 0.5f * in.scale) {
                v = d * (1.0f / dt);
                v = v * 0.6f + p.v[h] * 0.4f;
            }
        }
        hand.v = B::IsFinite(v) ? v : Vec3{};
        p.v[h] = hand.v;
        p.palm[h] = hand.p;
        p.had[h] = hand.bare;
        // The squeeze past grab_on, while it lasts: the corpses' updates take hold within
        // grab_window_s of it.
        if (!hand.bare || hand.squeeze < cfg.grab_off) {
            p.squeeze_ns[h] = 0;
        } else if (hand.squeeze >= cfg.grab_on && p.squeeze[h] < cfg.grab_on &&
                   p.squeeze_ns[h] == 0) {
            p.squeeze_ns[h] = in.now;
            edge = true;
        }
        p.squeeze[h] = hand.bare ? hand.squeeze : 0.0f;
        out.squeeze_ns[h] = p.squeeze_ns[h];
    }
    g_hands.Store(out);
    if (edge || p.scan_ns == 0 || in.now - p.scan_ns >= ScanNs || !p.watching) {
        p.scan_ns = in.now;
        const s64 s0 = g_log ? NowNs() : 0;
        std::array<Found, Slots> found{};
        const size_t n = Scan(in, found);
        std::scoped_lock lock{g_mutex};
        Rewatch(found, n);
        p.watching = true;
        if (g_log) {
            g_cost.scans += 1;
            g_cost.scan_ns += NowNs() - s0;
            NoteCost(in.now);
        }
    }
}

bool LatestCorpseGrips(std::array<CorpseGrip, 2>& out) {
    return g_installed && g_grips.Load(out, 4);
}

bool CorpseGrabAvailable(unsigned hand, s64 now) {
    if (!g_installed || hand >= g_grab_near_ns.size()) return false;
    const s64 seen = g_grab_near_ns[hand].load(std::memory_order_relaxed);
    return K::GrabHintFresh(seen, now);
}

void CorpseStep(u8* ctrl) {
    if (!g_installed) {
        return;
    }
    const s64 now = NowNs();
    Hands hands{};
    const bool fresh = g_hands.Load(hands, 8) && now - hands.ns <= HandsStaleNs;
    if (!fresh) {
        hands.hand = {};
    }
    K::Config cfg{};
    cfg.hold_n = K::HoldForce(hands.strength);
    const float scale = std::max(hands.scale, 1e-3f);
    u8* const world = PhysWorld();
    std::scoped_lock lock{g_mutex};
    struct Timed {
        s64 t0;
        ~Timed() {
            if (t0 != 0) {
                const s64 ns = NowNs() - t0;
                g_cost.steps += 1;
                g_cost.step_ns += ns;
                g_cost.step_max = std::max(g_cost.step_max, ns);
            }
        }
    } timed{g_log ? now : 0};
    size_t slot = 0;
    while (slot < Slots && g_corpses[slot].ctrl != ctrl) {
        ++slot;
    }
    if (slot == Slots) {
        return; // a stale watch
    }
    Corpse& c = g_corpses[slot];
    u8* rag = nullptr;
    Vec3 root{};
    static constexpr const char* NotLimp = "its ragdoll is no longer limp";
    const char* gone = !Plausible(c.chr) || Read<u32>(c.chr + OffsetHandle) != c.handle
                           ? "unloaded"
                       : RagdollCtrl(c.chr, rag) != ctrl ? "its ragdoll is gone"
                       : Hp(c.chr) > 0                   ? "alive again"
                       : Read<s32>(rag + OffsetRagdollState) != RagdollStateDead ? NotLimp
                       : !RootCom(rag, world, root) ? "its pelvis is out of the physics"
                                                    : nullptr;
    if (gone != nullptr) {
        if (g_log) {
            LOG_INFO(Core, "VR corpse: no longer watching {}: {}{}", Enemy(c), gone,
                     gone == NotLimp && rag != nullptr
                         ? fmt::format(" (state {})", Read<s32>(rag + OffsetRagdollState))
                         : std::string{});
        }
        for (size_t h = 0; h < 2; ++h) {
            if (g_hold[h].hold.on && g_hold[h].slot == static_cast<s32>(slot)) {
                LOG_INFO(Core, "VR corpse: the {} hand let go of {}: it is no longer a corpse",
                         HandName(h), Enemy(c));
                g_hold[h] = {};
            }
        }
        NoteHolding();
        Unwatch(slot);
        return;
    }
    const float dt =
        c.last_ns != 0 ? std::clamp(static_cast<float>(now - c.last_ns) * 1e-9f, 0.0f, 0.1f)
                       : 1.0f / 30.0f;
    c.last_ns = now;
    // The cheap look: no hand holds it, none bare near it, nothing falling heavier: done.
    const float reach = c.span > 0.0f ? c.span + NearMarginM * scale : NearM * scale;
    bool close_by = now < c.heavy_until;
    for (size_t h = 0; h < 2 && !close_by; ++h) {
        const K::Hand& hand = hands.hand[h];
        close_by = (g_hold[h].hold.on && g_hold[h].slot == static_cast<s32>(slot)) ||
                   (hand.bare && B::LengthSq(hand.p - root) <= reach * reach);
    }
    if (!close_by) {
        c.touching = {};
        c.pushed = false; // the frozen check starts over when a hand is back
        c.still = 0;
        return;
    }
    auto& parts = g_scratch.parts;
    auto& ids = g_scratch.ids;
    auto& impulse = g_scratch.impulse;
    const int room = static_cast<int>(impulse.size());
    int pending = 0;
    if (now < c.frozen_until) {
        return;
    }
    const s32 n = ReadParts(rag, world, parts, ids);
    if (n == 0 || dt <= 0.0f) {
        return;
    }
    g_cost.reads += g_log ? 1 : 0;
    c.span = 0.0f;
    for (s32 i = 0; i < n; ++i) {
        const K::Part& p = parts[static_cast<size_t>(i)];
        c.span = std::max(c.span, std::max(B::Length(p.a - root), B::Length(p.b - root)) + p.r);
    }
    // Frozen: pushed last update, its fastest dynamic part then moving, yet it has not moved at all
    // since. Let go, take the motion out.
    s32 quick = 0;
    float quickest = 0.0f;
    for (s32 i = 0; i < n; ++i) {
        const float v = B::Length(parts[static_cast<size_t>(i)].v);
        if (K::Dynamic(parts[static_cast<size_t>(i)]) && v > quickest) {
            quickest = v;
            quick = i;
        }
    }
    const bool still = c.pushed && c.probe_part >= 0 && c.probe_part < n &&
                       c.probe_speed > FrozenSpeed &&
                       B::Length(parts[static_cast<size_t>(c.probe_part)].com - c.probe) <
                           FrozenStillM * scale;
    c.still = still ? c.still + 1 : 0;
    c.probe_part = quick;
    c.probe = parts[static_cast<size_t>(quick)].com;
    c.probe_speed = quickest;
    c.pushed = false;
    if (c.still >= FrozenUpdates) {
        c.still = 0;
        c.frozen_until = now + FrozenNs;
        for (size_t h = 0; h < 2; ++h) {
            if (g_hold[h].hold.on && g_hold[h].slot == static_cast<s32>(slot)) {
                const s64 used = g_hold[h].used_squeeze;
                g_hold[h] = {};
                g_hold[h].used_squeeze = used;
                Event(h, 2.0f, 0.0f);
            }
        }
        NoteHolding();
        LOG_INFO(Core,
                 "VR corpse: {} is frozen (the game does not move it: {:.1f} m/s for {} updates "
                 "without moving): let go, its motion taken out, left alone {:.0f} s",
                 Enemy(c), quickest, FrozenUpdates, static_cast<float>(FrozenNs) * 1e-9f);
        for (s32 i = 0; i < n; ++i) {
            const K::Part& p = parts[static_cast<size_t>(i)];
            if (K::Dynamic(p) && B::LengthSq(p.v) > 0.0f) {
                const Vec3 j = p.v * (-1.0f / p.inv_mass);
                alignas(16) const std::array<float, 4> undo{j.x, j.y, j.z, 0.0f};
                g_linear(world, ids[static_cast<size_t>(i)], undo.data());
            }
        }
        return;
    }
    std::array<int, 2> held{-1, -1};
    Vec3 drag{};
    bool let_go = false;
    for (size_t h = 0; h < 2; ++h) {
        HandHold& hh = g_hold[h];
        const K::Hand& hand = hands.hand[h];
        if (hh.hold.on && hh.slot == static_cast<s32>(slot)) {
            const char* why = !fresh                        ? "the hands are gone"
                              : !hand.bare                  ? "the hand is armed or busy"
                              : hand.squeeze < cfg.grab_off ? nullptr
                                                            : "";
            if (why == nullptr || *why != '\0') {
                LOG_INFO(Core,
                         "VR corpse: the {} hand let go of {}{} after {:.1f} s (its held point up "
                         "{:.2f} m at most; the grip's strength {:.0f}% at most)",
                         HandName(h), Enemy(c),
                         why != nullptr ? fmt::format(" ({})", why) : std::string{},
                         static_cast<float>(now - hh.since) * 1e-9f,
                         (hh.top_y - hh.start_y) / scale, hh.load_max * 100.0f);
                Event(h, 4.0f, 0.0f);
                hh = {};
                hh.used_squeeze = hands.squeeze_ns[h];
                let_go = true;
                continue;
            }
            K::Impulse imp{};
            const s32 part = hh.hold.part;
            const K::HoldStep step =
                K::StepHold(cfg, scale, dt, hand, parts.data(), n, hh.hold, imp);
            if (step != K::HoldStep::Held) {
                LOG_INFO(Core,
                         "VR corpse: {} {} the {} hand after {:.1f} s ({}; its held point up "
                         "{:.2f} m at most)",
                         Enemy(c), step == K::HoldStep::Slipped ? "slipped out of" : "was lost to",
                         HandName(h), static_cast<float>(now - hh.since) * 1e-9f,
                         step == K::HoldStep::Slipped
                             ? fmt::format("too heavy or caught: the hand {:.0f} cm ahead for "
                                           "{:.2f} s",
                                           cfg.slip_m * 100.0f, cfg.slip_s)
                             : std::string{"its part is gone"},
                         (hh.top_y - hh.start_y) / scale);
                Event(h, 2.0f, 0.0f);
                const s64 used = hh.used_squeeze;
                hh = {};
                hh.used_squeeze = used;
                let_go = true;
                continue;
            }
            held[h] = part;
            drag += hh.hold.drag;
            hh.load_max = std::max(hh.load_max, hh.hold.load);
            const Vec3 at = K::HeldPoint(parts[static_cast<size_t>(part)], hh.hold);
            hh.top_y = std::max(hh.top_y, at.y);
            // The drawn hand on it: the palm that holds this point (the controller's turn).
            NoteGrip(h, at - B::Rotate(hand.q, hh.hold.grip), now);
            if (imp.part >= 0 && pending < room) {
                impulse[static_cast<size_t>(pending++)] = imp;
            }
            if (now - hh.load_ns >= LoadTickNs) {
                hh.load_ns = now;
                Event(h, 3.0f, hh.hold.load);
            }
            if (g_log && now - hh.log_ns >= 250 * Ms) {
                hh.log_ns = now;
                float low = 1e9f;
                float high = -1e9f;
                float fastest = 0.0f;
                for (s32 i = 0; i < n; ++i) {
                    const K::Part& p = parts[static_cast<size_t>(i)];
                    low = std::min(low, p.com.y);
                    high = std::max(high, p.com.y);
                    fastest = std::max(fastest, B::Length(p.v));
                }
                const Vec3 to = K::GripPoint(hand, hh.hold);
                LOG_INFO(Core,
                         "VR corpse: {} held by the {} hand {:.2f} s: part {} at y {:.2f} (the "
                         "grip {:.2f}, {:.0f} cm off), pull {:.0f} N, slide {:.0f} N, strength "
                         "{:.0f}%; the corpse's parts y {:.2f}..{:.2f}, the fastest {:.2f} m/s",
                         Enemy(c), HandName(h), static_cast<float>(now - hh.since) * 1e-9f, part,
                         at.y, to.y, B::Length(to - at) * 100.0f / scale, B::Length(hh.hold.pull),
                         B::Length(hh.hold.drag), hh.hold.load * 100.0f, low, high, fastest);
            }
            continue;
        }
        // Reuse the contact query for both input ownership and the actual grab.
        // Publish only an eligible dynamic part, never just a corpse's bounding radius.
        K::Hold candidate{};
        const bool reachable = !hh.hold.on && hand.bare &&
                               K::TryGrab(cfg, scale, hand, parts.data(), n, candidate);
        if (reachable) g_grab_near_ns[h].store(now, std::memory_order_relaxed);
        // A new squeeze near this corpse takes hold.
        const s64 squeeze = hands.squeeze_ns[h];
        if (!hh.hold.on && hand.bare && squeeze != 0 && squeeze != hh.used_squeeze &&
            hand.squeeze >= cfg.grab_on &&
            static_cast<float>(now - squeeze) * 1e-9f <= cfg.grab_window_s &&
            reachable) {
            hh.hold = candidate;
            const K::Part& part = parts[static_cast<size_t>(hh.hold.part)];
            hh.slot = static_cast<s32>(slot);
            hh.handle = c.handle;
            hh.since = now;
            hh.used_squeeze = squeeze;
            hh.load_ns = now;
            hh.log_ns = now;
            hh.part_mass = 1.0f / part.inv_mass;
            hh.corpse_mass = MassOf(parts.data(), n);
            hh.parts = n;
            hh.start_y = hh.top_y = K::HeldPoint(part, hh.hold).y;
            held[h] = hh.hold.part;
            NoteGrip(h, hand.p, now);
            LOG_INFO(Core,
                     "VR corpse: the {} hand took hold of {} by its part {} of {} ({:.1f} kg of "
                     "{:.1f} kg; {:.0f} N a hand)",
                     HandName(h), Enemy(c), hh.hold.part, n, hh.part_mass, hh.corpse_mass,
                     cfg.hold_n);
            Event(h, 1.0f, hh.corpse_mass);
            continue;
        }
        // Otherwise a bare hand shoves what it touches (a hand holding another corpse too).
        if (!hand.bare) {
            c.touching[h] = false;
            continue;
        }
        float fastest = 0.0f;
        const int before = pending;
        // Closing (past push_open, or within the grab window of a squeeze): no shove.
        const bool closing =
            (hand.squeeze > cfg.push_open && hand.squeeze < cfg.grab_on) ||
            (squeeze != 0 && static_cast<float>(now - squeeze) * 1e-9f <= cfg.grab_window_s);
        const int touching = K::Push(cfg, scale, dt, hand, parts.data(), n, impulse.data(), room,
                                     pending, fastest, closing);
        if (touching > 0 && !c.touching[h] && now - c.touch_ns[h] >= TouchTickNs) {
            c.touch_ns[h] = now;
            Event(h, 0.0f, fastest);
            if (g_log) {
                LOG_INFO(Core,
                         "VR corpse: the {} hand touched {} ({} parts, {:.2f} m/s into it, {} "
                         "shoves)",
                         HandName(h), Enemy(c), touching, fastest, pending - before);
            }
        }
        c.touching[h] = touching > 0;
    }
    NoteHolding();
    K::Drag(cfg, dt, drag, parts.data(), n, impulse.data(), room, pending);
    if (let_go) {
        c.heavy_until = now + static_cast<s64>(cfg.heavy_after_s * 1e9f);
        K::Calm(cfg, parts.data(), n, impulse.data(), room, pending);
    }
    if (held[0] >= 0 || held[1] >= 0 || now < c.heavy_until) {
        K::Heavier(cfg, dt, parts.data(), n, held[0], held[1], impulse.data(), room, pending);
    }
    c.pushed = pending > 0;
    // Into the game's physics, on this corpse's own update (as the kill ragdoll's push): the world
    // returns for a body that is no longer dynamic.
    for (int i = 0; i < pending; ++i) {
        const K::Impulse& imp = impulse[static_cast<size_t>(i)];
        if (imp.part < 0 || imp.part >= n || !B::IsFinite(imp.j) || B::LengthSq(imp.j) <= 0.0f) {
            continue;
        }
        const s32 id = ids[static_cast<size_t>(imp.part)];
        g_cost.impulses += g_log ? 1 : 0;
        alignas(16) const std::array<float, 4> j{imp.j.x, imp.j.y, imp.j.z, 0.0f};
        if (imp.point && B::IsFinite(imp.at)) {
            alignas(16) const std::array<float, 4> at{imp.at.x, imp.at.y, imp.at.z, 1.0f};
            g_at(world, id, j.data(), at.data());
        } else {
            g_linear(world, id, j.data());
        }
    }
}

} // namespace VR::Bloodborne
