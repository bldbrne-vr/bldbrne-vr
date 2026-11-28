// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <fmt/format.h>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/file_sys/fs.h"
#include "core/vr/augur.h"
#include "core/vr/bloodborne_assets.h"
#include "core/vr/bloodborne_evasion.h"
#include "core/vr/bloodborne_melee.h"
#include "core/vr/bloodborne_sever.h"
#include "core/vr/hook_util.h"
#include "core/vr/combat_channel.h"
#include "core/vr/inframe.h"
#include "core/vr/item_audio.h"
#include "core/vr/readable_memory.h"
#include "core/vr/sever_cache.h"
#include "core/vr/sever_enemy.h"
#include "core/vr/sever_feel.h"
#include "core/vr/sever_flesh.h"
#include "core/vr/sever_jobs.h"
#include "core/vr/sever_model.h"
#include "core/vr/sever_reveal.h"
#include "core/vr/sever_rig.h"
#include "core/vr/vr_session.h"

namespace VR::Bloodborne {

std::atomic<std::uint32_t> g_sever_awake{0};

namespace {

namespace S = VR::Sever;

void SetAwake(std::uint32_t bit, bool on) {
    if (on) {
        g_sever_awake.fetch_or(bit, std::memory_order_release);
    } else {
        g_sever_awake.fetch_and(~bit, std::memory_order_release);
    }
}
namespace A = VR::Assets;
namespace F = VR::InFrame;
using Hook::Plausible;
using Hook::Read;

// A character and its pose, as core/vr/bloodborne_body.cpp reads the hunter's (the same classes
// for every character: ChrIns, its ChrCtrl and its behaviour module's hkaPose).
constexpr u32 OffsetHandle = 0x08;
constexpr u32 OffsetNpcParam = 0x70; // s32: its NpcParam row (its display mask)
constexpr u32 OffsetChrId = 0x74; // s32 (2000 = c2000)
constexpr u32 OffsetChrCtrl = 0x58;
constexpr u32 OffsetChrModules = 0x3B0;
constexpr u32 OffsetBehaviorModule = 0x30;
constexpr u32 OffsetModNode = 0x28;
constexpr u32 OffsetNodePose = 0x38;     // the hkaPose, inside the node
constexpr u32 OffsetPoseSkeleton = 0x00; // hkaSkeleton*
constexpr u32 OffsetPoseLocal = 0x08;    // hkQsTransform* (0x30 bytes each)
constexpr u32 OffsetPoseLocalSize = 0x10;
// Its model-space pose (hkaPose m_modelPose: what a ragdoll drives, the game's kill ragdoll and
// any later death fall), its bone flags and its "model in sync" / "local in sync" bools.
constexpr u32 OffsetPoseModel = 0x18;
constexpr u32 OffsetPoseModelSize = 0x20;
constexpr u32 OffsetPoseFlagsSize = 0x30;
constexpr u32 OffsetPoseModelInSync = 0x38;
constexpr u32 OffsetPoseLocalInSync = 0x39;
constexpr u32 OffsetCtrlRagdoll = 0x20; // ChrCtrl: its RagdollIns
constexpr u32 OffsetRagdollState = 0xC0; // RagdollIns: 2 = the full ragdoll
constexpr u32 OffsetSkelParents = 0x18; // s16*
constexpr u32 OffsetSkelParentCount = 0x20;
constexpr u32 OffsetSkelBones = 0x28; // {hkStringPtr name (bit 0: owned), lock translation}, 16 bytes
constexpr u32 SkelBoneBytes = 0x10;
constexpr std::size_t MaxNameChars = 63;
constexpr u32 OffsetSkelCount = 0x30;
constexpr u32 OffsetSkelPose = 0x38; // its reference pose, hkQsTransform*
constexpr u32 OffsetSkelPoseCount = 0x40;
constexpr u32 OffsetCtrlOwner = 0x08;
constexpr u32 OffsetCtrlWorld = 0x90; // three float4 rows {R row i, t_i}
constexpr u32 HkQsBytes = 0x30;
// VR_SEVER_TEST only: WorldChrMan's characters, as core/vr/bloodborne_corpse.cpp's Scan walks them,
// and the main player. A character's HP ([chr+0x3B0]+0x20 -> +0xF8): the test's and the hide's
// (a cut corpse alive again: SeverPose).
constexpr u64 WorldChrManRva = 0x553E878;
constexpr u32 OffsetMainPlayer = 0x60;
constexpr u32 OffsetMgrBlocks = 0x08;
constexpr u32 OffsetBlocksCount = 0x18;
constexpr u32 OffsetMgrChrSets = 0x850;
constexpr u32 OffsetSetEntries = 0x08;
constexpr u32 ChrEntrySize = 0x38;
constexpr s32 MaxBlocks = 64;
constexpr s32 MaxSetCount = 4096;
constexpr u32 OffsetDataModule = 0x20;
constexpr u32 OffsetHp = 0xF8;

constexpr std::size_t MaxQueued = 4; // kills waiting for the worker (more: not cut)
// The enemy models the worker keeps read, the most recent last: their meshes, their texture files
// and the textures decoded for their cuts (owned copies: nothing of a live enemy). At most
// MaxCached of them within CacheBudgetBytes, the newest kept whatever its size (it is being cut);
// all freed once nothing has used them for a while (WorkerRun: ReleaseAfterNs, ReleaseIdleNs).
// The user, 2026-11-16: "performance and memory safety is king".
constexpr std::size_t MaxCached = 4;
constexpr std::size_t CacheBudgetBytes = std::size_t{64} << 20;
constexpr std::size_t MaxPrefetch = 2; // models asked for ahead of a kill, waiting (more: dropped)
// The posed mesh against its bind pose (the frame check): its box's size within these factors,
// its middle moved by less than its bind box's size.
constexpr float MinPosedSize = 0.5f;
constexpr float MaxPosedSize = 2.0f;

// What the game thread copies at the kill: no guest pointer leaves it.
struct Capture {
    S::ActorKey key{};
    s32 chr{-1};
    const u8* addr{nullptr}; // the corpse: its pieces last while its handle still finds it
    u32 handle{0};
    s32 npc{-1};                         // its NpcParam row
    std::optional<std::uint32_t> disp{}; // ... its display mask (the worker's; none: every mesh)
    S::Site site{};
    bool heavy{false};   // too heavy to cut in half: the nearest limb or the head instead
    bool charged{false}; // a charged cut: more violent on every count (sever_feel.h)
    S::Feel::StrikeHands hands{S::Feel::StrikeHands::Weapon};
    std::vector<S::QsPose> local, reference;
    // Model space, when the game's is in sync (a ragdoll writes only that one: its local pose may
    // be behind); empty: the worker builds it from `local`.
    std::vector<S::QsPose> model;
    std::vector<std::int16_t> parents;
    std::vector<std::string> names; // the Havok bones' (the model's bones are matched by name)
    std::array<float, 12> world{};
    S::Vec3 contact{}; // world
    bool blade{false};
    S::Vec3 base{}, tip{}, tip_vel{}; // world
    s64 ns{0};
    // The worker's (steady ns): it took the kill, read what it needed (model, textures' file, the
    // looks), made the cut. The cut's timeline (Piece::times).
    s64 start_ns{0}, loaded_ns{0}, cut_ns{0};
    u32 feel{0}; // the cut's id for its feel (its bite at the blow, its tear as it comes off)
};

s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
double Ms(s64 from, s64 to) {
    return static_cast<double>(to - from) / 1e6;
}

const char* ErrorName(S::Error e) {
    switch (e) {
    case S::Error::None:
        return "none";
    case S::Error::InvalidInput:
        return "invalid input";
    case S::Error::Budget:
        return "over the cut's limits";
    case S::Error::NoIntersection:
        return "the ring misses every mesh";
    case S::Error::OnPlaneVertex:
        return "a vertex on the ring, nudged both ways";
    case S::Error::WrongRegion:
        return "wrong region";
    case S::Error::OpenOrMultipleContours:
        return "a flesh hole left open and nothing closed";
    case S::Error::UnsupportedContour:
        return "a ring with no extent";
    case S::Error::AllocationFailed:
        return "out of memory";
    }
    return "?";
}

// A corpse's name for the jobs: its address and its spawn handle (a respawned enemy at the same
// address is another corpse), and its model.
S::ActorKey KeyOf(const u8* chr) {
    return {(reinterpret_cast<std::uint64_t>(chr) << 16) ^ Read<u32>(chr + OffsetHandle),
            Read<s32>(chr + OffsetChrId)};
}

std::vector<S::QsPose> QsArray(const u8* p, s32 n) {
    std::vector<S::QsPose> out(static_cast<std::size_t>(n));
    for (s32 i = 0; i < n; ++i) {
        float f[12];
        std::memcpy(f, p + static_cast<std::size_t>(i) * HkQsBytes, sizeof(f));
        out[static_cast<std::size_t>(i)] = {{f[0], f[1], f[2]}, {f[4], f[5], f[6], f[7]},
                                            {f[8], f[9], f[10]}};
    }
    return out;
}

// A value read only where it can be (ReadPose).
template <typename T>
bool ReadIf(ReadableMemory& mem, const u8* at, T& out) {
    if (!mem.Has(at, sizeof(T))) {
        return false;
    }
    out = Read<T>(at);
    return true;
}

// The character's pose now, copied: nullptr, or why not (structure checks only; the worker checks
// the values). Every byte is checked readable before it is read (a code review, 2026-11-17: a
// stale or odd skeleton whose pointers pass Plausible may still point at nothing, and reading it
// would end the game): a plain kill instead. A few system queries a cutting kill (ReadableMemory:
// one per region the reads are in), on no other path.
const char* ReadPose(const u8* chr, Capture& c) {
    ReadableMemory mem;
    const u8* ctrl = nullptr;
    const u8* owner = nullptr;
    if (!ReadIf(mem, chr + OffsetChrCtrl, ctrl) || !Plausible(ctrl) ||
        !ReadIf(mem, ctrl + OffsetCtrlOwner, owner) || owner != chr ||
        !mem.Has(ctrl + OffsetCtrlWorld, sizeof(c.world))) {
        return "no ChrCtrl";
    }
    std::memcpy(c.world.data(), ctrl + OffsetCtrlWorld, sizeof(c.world));
    const u8* mods = nullptr;
    const u8* mod = nullptr;
    const u8* node = nullptr;
    if (!ReadIf(mem, chr + OffsetChrModules, mods) || !Plausible(mods) ||
        !ReadIf(mem, mods + OffsetBehaviorModule, mod) || !Plausible(mod) ||
        !ReadIf(mem, mod + OffsetModNode, node) || !Plausible(node)) {
        return "no pose node";
    }
    const u8* const pose = node + OffsetNodePose;
    const u8* skeleton = nullptr;
    const u8* local = nullptr;
    if (!ReadIf(mem, pose + OffsetPoseSkeleton, skeleton) ||
        !ReadIf(mem, pose + OffsetPoseLocal, local) || !Plausible(skeleton) ||
        !Plausible(local, 16)) {
        return "no pose";
    }
    s32 n = 0;
    s32 local_size = 0;
    s32 parent_count = 0;
    s32 pose_count = 0;
    if (!ReadIf(mem, skeleton + OffsetSkelCount, n) || n <= 0 ||
        static_cast<std::size_t>(n) > S::MaxBones ||
        !ReadIf(mem, pose + OffsetPoseLocalSize, local_size) || local_size != n ||
        !ReadIf(mem, skeleton + OffsetSkelParentCount, parent_count) || parent_count != n ||
        !ReadIf(mem, skeleton + OffsetSkelPoseCount, pose_count) || pose_count != n) {
        return "the pose's sizes";
    }
    const auto count = static_cast<std::size_t>(n);
    const u8* parents = nullptr;
    const u8* reference = nullptr;
    const u8* bones = nullptr;
    if (!ReadIf(mem, skeleton + OffsetSkelParents, parents) ||
        !ReadIf(mem, skeleton + OffsetSkelPose, reference) ||
        !ReadIf(mem, skeleton + OffsetSkelBones, bones) || !Plausible(parents, 2) ||
        !Plausible(reference, 16) || !Plausible(bones) ||
        !mem.Has(parents, count * sizeof(std::int16_t)) || !mem.Has(local, count * HkQsBytes) ||
        !mem.Has(reference, count * HkQsBytes) || !mem.Has(bones, count * SkelBoneBytes)) {
        return "the skeleton";
    }
    // The bones' names (the ChrCtrl's FLVER-to-Havok map counts other bones than the file's
    // FLVER: c1000's 153 against 218; the names match). Each read as far as it can be: a name that
    // runs into memory that cannot be read before its end is not one.
    c.names.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto at = Read<std::uint64_t>(bones + i * SkelBoneBytes) & ~std::uint64_t{1};
        const char* const name = reinterpret_cast<const char*>(at);
        if (!Plausible(reinterpret_cast<const u8*>(name), 1)) {
            return "a bone's name";
        }
        const std::size_t readable = mem.Span(name, MaxNameChars);
        std::string& out = c.names[i];
        std::size_t k = 0;
        for (; k < readable && name[k] != 0; ++k) {
            out.push_back(name[k]);
        }
        if (k == readable && readable < MaxNameChars) {
            return "a bone's name";
        }
    }
    c.parents.resize(count);
    std::memcpy(c.parents.data(), parents, count * sizeof(std::int16_t));
    c.local = QsArray(local, n);
    c.reference = QsArray(reference, n);
    u8 in_sync = 0;
    s32 model_size = 0;
    const u8* model = nullptr;
    if (ReadIf(mem, pose + OffsetPoseModelInSync, in_sync) && in_sync != 0 &&
        ReadIf(mem, pose + OffsetPoseModelSize, model_size) && model_size == n &&
        ReadIf(mem, pose + OffsetPoseModel, model) && Plausible(model, 16) &&
        mem.Has(model, count * HkQsBytes)) {
        c.model = QsArray(model, n);
    }
    return nullptr;
}

// The frame check: a mesh posed by the game's skeleton keeps about its size and stays near where
// it was (the box of its bind pose); a wrong frame or bone match throws it metres away or folds it.
bool Sane(std::span<const S::Vertex> bind, std::span<const S::Vertex> posed, float& size,
          float& moved) {
    const auto box = [](std::span<const S::Vertex> v, S::Vec3& lo, S::Vec3& hi) {
        lo = {INFINITY, INFINITY, INFINITY};
        hi = {-INFINITY, -INFINITY, -INFINITY};
        for (const S::Vertex& x : v) {
            lo = {std::min(lo.x, x.position.x), std::min(lo.y, x.position.y),
                  std::min(lo.z, x.position.z)};
            hi = {std::max(hi.x, x.position.x), std::max(hi.y, x.position.y),
                  std::max(hi.z, x.position.z)};
        }
    };
    if (bind.empty() || bind.size() != posed.size()) {
        return false;
    }
    S::Vec3 a0, a1, b0, b1;
    box(bind, a0, a1);
    box(posed, b0, b1);
    const auto diag = [](S::Vec3 lo, S::Vec3 hi) {
        return std::sqrt((hi.x - lo.x) * (hi.x - lo.x) + (hi.y - lo.y) * (hi.y - lo.y) +
                         (hi.z - lo.z) * (hi.z - lo.z));
    };
    const float da = diag(a0, a1), db = diag(b0, b1);
    const S::Vec3 ca{(a0.x + a1.x) / 2, (a0.y + a1.y) / 2, (a0.z + a1.z) / 2};
    const S::Vec3 cb{(b0.x + b1.x) / 2, (b0.y + b1.y) / 2, (b0.z + b1.z) / 2};
    size = da > 1e-6f ? db / da : INFINITY;
    moved = da > 1e-6f ? diag(ca, cb) / da : INFINITY;
    return size >= MinPosedSize && size <= MaxPosedSize && moved < 1.0f;
}

// ---- The hide ------------------------------------------------------------------------------------
// A cut corpse's piece shrunk in the corpse's own pose, as the hunter's head is hidden (body_solver.h
// HiddenScale): the detached set's root bones get a tiny local scale right after the game's pose
// copy (the NPC pose hook, bloodborne_body.cpp), so the game draws the body without it, its shadow
// too. A ring of the last cuts, matched by key. The key's handle is only the character's slot in
// its map block, so an enemy a lamp's rest brings back can have its corpse's address and handle:
// a character seen alive again (HideAliveGraceNs after its cut) is dropped from the ring. The
// grace counts from the killing blow, not from the worker's finish: a cut that lands late, after a
// death or a warp has reloaded the area, finds the enemy back in its corpse's slot alive past it,
// and is undone before it is ever drawn (a code review, 2026-11-13). No reload ends within a
// second of a kill.
constexpr float HideScale = 0.01f; // never 0: later inverses would divide by it
constexpr std::size_t HideRoots = 8;
constexpr s64 HideAliveGraceNs = 1'000'000'000;
struct Hidden {
    S::ActorKey key{};
    u32 reveal{0}; // 0: an already-cut corpse whose pieces have retired
    // Its reveal slot given back once its cut was drawn (RevealDone): `reveal` stays the cut's id.
    bool revealed{false};
    std::array<std::int16_t, HideRoots> havok{};
    std::uint8_t count{0};
    // The limbs the cut keeps that hang under a root in the game's skeleton (an arm under the
    // neck): scaled back to full size (1 / HideScale), so only the cut part vanishes.
    std::array<std::int16_t, HideRoots> keeps{};
    std::uint8_t keep_count{0};
    std::int16_t anchor{-1}; // the stump's bone (Havok): the drawn stump rides it (SeverPose)
    std::int16_t piece_bone{-1}; // the piece's (Havok): where it starts at the swap (Rebase)
    s64 ns{0};               // when it was cut (the killing blow: Capture::ns)
    // Its corpse (CorpseThere): checked for itself, pieces or none (CheckHiddenCorpses).
    const u8* addr{nullptr};
    u32 handle{0};
    s32 model{-1};
};
constexpr std::size_t HiddenSlots = 32; // cut corpses kept cut (2026-11-13: 32)
std::mutex g_hide_mutex;
std::array<Hidden, HiddenSlots> g_hidden{}; // under g_hide_mutex
std::atomic<std::uint32_t> g_hide_count{0}; // the pose hook's one test while nothing is cut
// The hidden corpses' actors (KeyOf's), written under g_hide_mutex: every character's two hooks
// look here first and take the lock only for a cut corpse (the review, 2026-11-13: after one cut
// every NPC took the lock twice a frame).
std::array<std::atomic<std::uint64_t>, HiddenSlots> g_hidden_actor{};
// ... and once a hook has hidden its part (ApplyHide): its pieces are drawn only from then on, so
// the piece and the part are never both seen, nor neither (the review, 2026-11-13).
std::array<std::atomic<std::uint32_t>, HiddenSlots> g_applied_cut{};

bool HideApplied(u32 reveal) {
    if (reveal == 0) return false;
    for (const auto& cut : g_applied_cut) {
        if (cut.load(std::memory_order_acquire) == reveal) {
            return true;
        }
    }
    return false;
}
// ... and before them one load: a bit a hidden actor (its hash's), so every other character's
// hooks stop at one test while a cut corpse lies somewhere (not 48 loads twice a frame each).
std::atomic<std::uint64_t> g_hidden_bits{0};

std::uint64_t HideBit(std::uint64_t actor) {
    return std::uint64_t{1} << ((actor * 0x9E3779B97F4A7C15ull) >> 58);
}

// (Under g_hide_mutex.) The first hidden corpse wakes the hooks and swaps the NPC update wrapper
// into the game's tables; the last one gone swaps it out and puts them back to sleep.
void CountHidden() {
    std::uint32_t n = 0;
    std::uint64_t bits = 0;
    for (const Hidden& h : g_hidden) {
        if (h.count > 0) {
            ++n;
            bits |= HideBit(h.key.actor);
        }
    }
    const bool was = g_hide_count.load(std::memory_order_relaxed) > 0;
    g_hidden_bits.store(bits, std::memory_order_release);
    g_hide_count.store(n, std::memory_order_release);
    if (n > 0 && !was) {
        SetAwake(SeverAwakeHidden, true);
        SeverNpcUpdateHook(true);
    } else if (n == 0 && was) {
        SeverNpcUpdateHook(false);
        SetAwake(SeverAwakeHidden, false);
    }
}

void Hide(const S::ActorKey& key, const std::array<std::int16_t, HideRoots>& havok,
          std::uint8_t count, const std::array<std::int16_t, HideRoots>& keeps,
          std::uint8_t keep_count, std::int16_t anchor, std::int16_t piece_bone, const u8* addr,
          u32 handle, s32 model, s64 cut_ns, u32 reveal) {
    std::scoped_lock lock{g_hide_mutex};
    // A free slot first; all taken (32 cut corpses lying about), the oldest cut's.
    std::size_t at = g_hidden.size();
    for (std::size_t i = 0; i < g_hidden.size() && at == g_hidden.size(); ++i) {
        if (g_hidden[i].count == 0) {
            at = i;
        }
    }
    if (at == g_hidden.size()) {
        at = 0;
        for (std::size_t i = 1; i < g_hidden.size(); ++i) {
            if (g_hidden[i].ns < g_hidden[at].ns) {
                at = i;
            }
        }
    }
    Hidden h{};
    h.key = key;
    h.reveal = reveal;
    h.havok = havok;
    h.count = count;
    h.keeps = keeps;
    h.keep_count = keep_count;
    h.anchor = anchor;
    h.piece_bone = piece_bone;
    h.ns = cut_ns;
    h.addr = addr;
    h.handle = handle;
    h.model = model;
    if (!g_hidden[at].revealed) {
        S::g_reveals.Release(g_hidden[at].reveal);
    }
    g_hidden[at] = h;
    g_applied_cut[at].store(0, std::memory_order_release);
    g_hidden_actor[at].store(count > 0 ? key.actor : 0, std::memory_order_release);
    CountHidden();
}

// Its pieces gone (a newer cut took their slots): the corpse stays cut, but nothing rides its
// bone any more, so its hooks only hide (no stump to place, no pieces' lock).
void DropAnchor(const S::ActorKey& key) {
    std::scoped_lock lock{g_hide_mutex};
    for (std::size_t i = 0; i < g_hidden.size(); ++i) {
        Hidden& h = g_hidden[i];
        if (h.count > 0 && h.key == key) {
            const bool claimed = h.revealed || S::g_reveals.Release(h.reveal);
            if (h.reveal != 0 && !claimed) {
                h = {}; // evicted before reveal: leave the original corpse whole
                g_hidden_actor[i].store(0, std::memory_order_release);
                g_applied_cut[i].store(0, std::memory_order_release);
            } else {
                h.anchor = -1;
                h.reveal = 0;
                h.revealed = false;
            }
        }
    }
    CountHidden();
}

// A character alive again under a cut corpse's key: not that corpse (its entry goes).
void Unhide(const S::ActorKey& key) {
    std::scoped_lock lock{g_hide_mutex};
    for (std::size_t i = 0; i < g_hidden.size(); ++i) {
        if (g_hidden[i].count > 0 && g_hidden[i].key == key) {
            if (!g_hidden[i].revealed) {
                S::g_reveals.Release(g_hidden[i].reveal);
            }
            g_hidden[i] = {};
            g_hidden_actor[i].store(0, std::memory_order_release);
            g_applied_cut[i].store(0, std::memory_order_release);
        }
    }
    CountHidden();
}

// Its cut drawn in every active eye (SeverRenderTick): its reveal slot goes back at once. Held
// until the corpse went, a cut with one half (or whose piece fell away) kept one for good, and
// 16 of them stopped all cutting in the area (the review, 2026-11-16). `reveal` stays the cut's
// id (HideApplied, the stump's follow); Visible implies the hide was claimed (DropAnchor).
void RevealDone(u32 token) {
    std::scoped_lock lock{g_hide_mutex};
    for (Hidden& h : g_hidden) {
        if (h.count > 0 && h.reveal == token && !h.revealed) {
            S::g_reveals.Release(token);
            h.revealed = true;
        }
    }
}

// ---- The worker ---------------------------------------------------------------------------------

const u8* g_base = nullptr; // the eboot (InstallSever)

std::mutex g_queue_mutex;
std::condition_variable g_wake;
std::deque<Capture> g_queue; // under g_queue_mutex
S::Jobs g_jobs;              // its own lock (Known from the game thread)
// Read ahead of a kill (SeverPrefetch): the characters whose models are wanted, waiting (under
// g_queue_mutex), and recent accepted requests; eviction or a failed read permits another try.
S::Cache::Wanted<MaxPrefetch> g_prefetch;
S::Cache::Recent<4> g_prefetch_asked;

// An enemy's model, read from its file (the worker's alone).
struct Loaded {
    s32 chr{-1};
    S::Model model;
    std::vector<S::BoneRef> refs; // names into model.bones (stable: never resized)
    bool logged{false}; // its skeleton's match logged, on its first cut
    // Its own texture file beside the chrbnd (c<id>_2.tpf.dcx, inflated; empty: none): where 24
    // characters keep their textures (c2630's: the tester's 2026-11-13 untextured limbs).
    std::vector<u8> more_textures;
    // The textures its cuts decoded (halved, as drawn; their blocks shared: ModelTexture::Share),
    // by name: the next cut's pieces share them instead of decoding again or holding a copy each.
    // Counted in `bytes` (once), never past CacheBudgetBytes.
    std::vector<std::pair<std::string, A::ModelTexture>> decoded;
    std::size_t bytes{0}; // all of the above, about
};
std::vector<std::unique_ptr<Loaded>> g_models; // the most recent last
// The models read (their characters; 0 none: c0000 is never cut): the hit hook's test for a
// read-ahead (SeverPrefetch). Written by the worker only.
std::array<std::atomic<s32>, MaxCached> g_cached_chr{};

// Where a model can never be cut, whatever its pose: a site whose limb bones it lacks, or every
// site (its file unreadable, its bones none of its skeleton's). The kill hook skips those (no
// bite for a cut that cannot come: the review, 2026-11-16; two of a mock's seven bites had none).
// 16 models, a word each (sever_cache.h Cache::Uncuttable: its character and its sites in one
// word, a code review, 2026-11-16). The worker writes, the game thread reads. 128 bytes.
S::Cache::Uncuttable<16> g_uncut;
constexpr u64 AllSites = S::Cache::Uncuttable<16>::AllSites;

static_assert((static_cast<unsigned>(S::Limb::Torso) - 1u) * 8u + 7u < 48u);
u64 SiteBit(const S::Site& s) {
    const auto limb = static_cast<unsigned>(s.limb), bone = static_cast<unsigned>(s.bone);
    return limb == 0 || bone > 7 ? 0 : u64{1} << ((limb - 1u) * 8u + bone);
}

void MarkUncuttable(s32 chr, u64 sites) {
    g_uncut.Mark(chr, sites);
}

bool Uncuttable(s32 chr, const S::Site& site) {
    return g_uncut.Has(chr, SiteBit(site));
}

// Where one struck body can never be cut while the model's others at that limb can (a creature's
// segment by its own bone's name, one head of many: g_uncut's bit would block those): that body
// alone, the same failures (no bones to cut through, no mesh blending into them in any look).
// c1190's 30 such bodies of 74 bit at every kill with no cut (2026-11-17). 64 bodies, 512 bytes.
S::Cache::FailedHits<64> g_failed_hits;

bool FailedHit(s32 chr, const S::Site& site) {
    return g_failed_hits.Has(S::StruckId(chr, site));
}

std::size_t BytesOf(const Loaded& l) {
    std::size_t n = sizeof(Loaded) + l.model.textures.capacity() + l.more_textures.capacity() +
                    l.model.bones.capacity() * sizeof(S::ModelBone) +
                    l.refs.capacity() * sizeof(S::BoneRef);
    for (const S::ModelMesh& m : l.model.meshes) {
        n += sizeof(m) + m.vertices.capacity() * sizeof(S::Vertex) +
             m.triangles.capacity() * sizeof(S::Triangle) + m.group.capacity() +
             m.material.capacity() + m.diffuse.capacity();
    }
    for (const auto& [name, t] : l.decoded) {
        n += name.capacity() + sizeof(t) + t.Bytes().capacity() +
             t.level_offset.capacity() * sizeof(u32);
    }
    return n;
}

std::size_t CachedBytes() {
    std::size_t n = 0;
    for (const auto& m : g_models) {
        n += m->bytes;
    }
    return n;
}

// The oldest models go until MaxCached and CacheBudgetBytes hold (never the newest: in use).
void TrimCache() {
    const std::size_t drop = S::Cache::TrimCount(g_models, MaxCached, CacheBudgetBytes,
                                                 [](const auto& m) { return m->bytes; });
    for (std::size_t i = 0; i < drop; ++i) {
        g_prefetch_asked.Forget(g_models[i]->chr);
    }
    g_models.erase(g_models.begin(), g_models.begin() + static_cast<std::ptrdiff_t>(drop));
    for (std::size_t i = 0; i < g_cached_chr.size(); ++i) {
        g_cached_chr[i].store(i < g_models.size() ? g_models[i]->chr : 0,
                              std::memory_order_relaxed);
    }
}
std::vector<s32> g_unreadable;                 // models that could not be read (not tried again)
std::vector<s32> g_mismatched;                 // logged once: another model than its file's
unsigned g_dumps = 0;

// The display mask of an NpcParam row (the game's gameparam, read once by the worker; none known:
// every mesh is drawn, as before). The game draws only its character's own look of a model.
std::optional<std::unordered_map<std::int32_t, std::uint32_t>> g_disp_masks; // ReleaseAll

std::optional<std::uint32_t> DispMask(s32 npc) {
    if (!g_disp_masks) {
        std::unordered_map<std::int32_t, std::uint32_t> masks;
        const char* const path = "/app0/dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx";
        auto* const mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
        const std::optional<std::vector<u8>> file = mnt->ReadFile(path);
        const S::ModelError e = file ? S::LoadNpcDispMasks(*file, masks) : S::ModelError::Read;
        if (e != S::ModelError::None) {
            g_disp_masks.emplace(); // A missing or unsupported table falls back to every mesh.
            LOG_WARNING(Core, "VR sever: NpcParam's display masks not read ({}): every mesh of a "
                              "model is cut and drawn",
                        S::ModelErrorName(e));
        } else {
            LOG_INFO(Core, "VR sever: NpcParam's display masks read ({} rows)", masks.size());
            g_disp_masks.emplace(std::move(masks)); // A thrown allocation leaves the cache unset.
        }
    }
    const auto it = g_disp_masks->find(npc);
    return it == g_disp_masks->end() ? std::nullopt : std::optional<std::uint32_t>{it->second};
}

Loaded* ModelOf(s32 chr, double& read_ms, bool ahead = false) {
    for (std::size_t i = 0; i < g_models.size(); ++i) {
        if (g_models[i]->chr == chr) {
            std::rotate(g_models.begin() + static_cast<std::ptrdiff_t>(i),
                        g_models.begin() + static_cast<std::ptrdiff_t>(i) + 1, g_models.end());
            return g_models.back().get();
        }
    }
    if (std::find(g_unreadable.begin(), g_unreadable.end(), chr) != g_unreadable.end()) {
        return nullptr;
    }
    const s64 t0 = NowNs();
    const std::string path = fmt::format("/app0/dvdroot_ps4/chr/c{:04d}.chrbnd.dcx", chr);
    auto* const mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    const std::optional<std::vector<u8>> file = mnt->ReadFile(path);
    auto loaded = std::make_unique<Loaded>();
    loaded->chr = chr;
    // Allocation failures reach the worker's per-request handler. Only a file error is cached.
    const S::ModelError e = file ? S::LoadModel(*file, loaded->model) : S::ModelError::Read;
    if (e != S::ModelError::None) {
        g_unreadable.push_back(chr);
        MarkUncuttable(chr, AllSites);
        LOG_WARNING(Core, "VR sever: c{:04d}: its model {} could not be read ({}): never cut", chr,
                    path, S::ModelErrorName(e));
        return nullptr;
    }
    const std::string more = fmt::format("/app0/dvdroot_ps4/chr/c{:04d}_2.tpf.dcx", chr);
    if (const std::optional<std::vector<u8>> tpf = mnt->ReadFile(more); tpf) {
        if (S::InflateDcx(*tpf, loaded->more_textures) != S::ModelError::None) {
            loaded->more_textures.clear();
        }
    }
    loaded->refs.reserve(loaded->model.bones.size());
    for (const S::ModelBone& b : loaded->model.bones) {
        loaded->refs.push_back({b.name, b.parent});
    }
    loaded->bytes = BytesOf(*loaded);
    read_ms = Ms(t0, NowNs());
    g_models.push_back(std::move(loaded));
    TrimCache();
    LOG_INFO(Core, "VR sever: c{:04d}: its model read{} in {:.1f} ms, {:.1f} MB kept ({} models, "
                   "{:.1f} of {} MB)",
             chr, ahead ? " ahead of its kill (on a hit)" : "", read_ms, static_cast<double>(g_models.back()->bytes) / 1048576.0,
             g_models.size(), static_cast<double>(CachedBytes()) / 1048576.0,
             CacheBudgetBytes >> 20);
    return g_models.back().get();
}

// VR_SEVER_DUMP: the cut as OBJ (the body side and the piece side, whole meshes posed too; caps
// tagged "cut" with their flesh uv; cloth "cloth"), and the plane. The user's machine only.
void WriteMesh(std::ofstream& out, std::size_t& base, const S::Mesh& m, bool cloth) {
    for (const S::Vertex& v : m.vertices) {
        out << "v " << v.position.x << ' ' << v.position.y << ' ' << v.position.z << '\n'
            << "vt " << v.uv[0] << ' ' << v.uv[1] << '\n'
            << "vn " << v.normal.x << ' ' << v.normal.y << ' ' << v.normal.z << '\n';
    }
    int last = -1;
    for (const S::Triangle& t : m.triangles) {
        const int kind = t.material == S::Cut{}.cap_material ? 1 : cloth ? 2 : 0;
        if (kind != last) {
            out << "usemtl " << (kind == 1 ? "cut" : kind == 2 ? "cloth" : "surface") << '\n';
            last = kind;
        }
        out << "f " << t.index[0] + base << ' ' << t.index[1] + base << ' ' << t.index[2] + base
            << '\n';
    }
    base += m.vertices.size();
}

void Dump(const Capture& c, const Loaded& l, std::span<const S::SkinTransform> skins,
          std::span<const std::uint8_t> det, const S::Plane& plane, const S::EnemyCut& cut) {
    const char* const root = std::getenv("VR_SEVER_DUMP");
    if (root == nullptr || *root == '\0') {
        return;
    }
    std::error_code ec;
    const std::filesystem::path dir =
        std::filesystem::path(root) / fmt::format("c{:04d}_{}", c.chr, ++g_dumps);
    std::filesystem::create_directories(dir, ec);
    std::ofstream body(dir / "body.obj"), piece(dir / "piece.obj");
    if (ec || !body || !piece) {
        LOG_WARNING(Core, "VR sever: could not write the dump to {}", dir.string());
        return;
    }
    std::size_t body_base = 1, piece_base = 1;
    std::vector<bool> cut_mesh(l.model.meshes.size(), false);
    for (const S::PartCut& p : cut.parts) {
        const bool cloth = S::ClothMaterial(l.model.meshes[p.mesh].material);
        cut_mesh[p.mesh] = true;
        WriteMesh(body, body_base, p.body, cloth);
        WriteMesh(piece, piece_base, p.piece, cloth);
    }
    for (std::size_t i = 0; i < l.model.meshes.size(); ++i) {
        const S::ModelMesh& m = l.model.meshes[i];
        if (cut_mesh[i] || m.vertices.empty() || m.simulated || !S::Drawn(m, c.disp)) {
            continue;
        }
        S::Mesh posed;
        if (!S::Pose(m.vertices, skins, posed.vertices)) {
            continue;
        }
        posed.triangles = m.triangles;
        const bool to_piece = S::Majority(m.vertices, det) == S::Side::Piece;
        WriteMesh(to_piece ? piece : body, to_piece ? piece_base : body_base, posed,
                  S::ClothMaterial(m.material));
    }
    WriteMesh(body, body_base, cut.body_cap, false);
    WriteMesh(piece, piece_base, cut.piece_cap, false);
    std::ofstream(dir / "cut.txt") << plane.origin.x << ' ' << plane.origin.y << ' '
                                   << plane.origin.z << ' ' << plane.normal.x << ' '
                                   << plane.normal.y << ' ' << plane.normal.z << '\n';
    LOG_INFO(Core, "VR sever: dumped to {}", dir.string());
}

bool MakePieces(const Capture& c, Loaded& l, std::span<const S::SkinTransform> skins,
                std::span<const std::uint8_t> det, const S::Plane& plane, const S::EnemyCut& cut,
                const S::QsPose& anchor_kill, const S::QsPose& piece_kill, S::StumpSection section,
                const std::array<std::int16_t, HideRoots>& roots, std::uint8_t nroots,
                const std::array<std::int16_t, HideRoots>& keeps, std::uint8_t nkeeps,
                std::int16_t anchor, std::int16_t piece_bone);

void Cut(Capture c) {
    const s64 t0 = NowNs();
    c.start_ns = t0;
    double read_ms = 0;
    Loaded* const l = ModelOf(c.chr, read_ms);
    if (l == nullptr) {
        return;
    }
    const S::Model& m = l->model;
    const char* limb = S::LimbName(c.site.limb);
    c.disp = DispMask(c.npc); // only the look the game draws
    c.loaded_ns = NowNs();
    // Each of the model's bones to the skeleton's of the same name (-1: none, it rides its parent).
    std::vector<std::int16_t> map(m.bones.size(), -1);
    std::size_t mapped = 0;
    for (std::size_t b = 0; b < m.bones.size(); ++b) {
        for (std::size_t h = 0; h < c.names.size(); ++h) {
            if (c.names[h] == m.bones[b].name) {
                map[b] = static_cast<std::int16_t>(h);
                ++mapped;
                break;
            }
        }
    }
    if (mapped == 0) {
        MarkUncuttable(c.chr, AllSites);
        if (std::find(g_mismatched.begin(), g_mismatched.end(), c.chr) == g_mismatched.end()) {
            g_mismatched.push_back(c.chr);
            LOG_WARNING(Core, "VR sever: c{:04d}: none of its model's {} bones is in its skeleton "
                              "({} bones): never cut",
                        c.chr, m.bones.size(), c.names.size());
        }
        return;
    }
    std::vector<S::QsPose> havok, reference;
    // The game's own model-space pose when it had it in sync (every value finite), else built.
    const bool model_ok =
        c.model.size() == c.local.size() &&
        std::all_of(c.model.begin(), c.model.end(),
                    [](const S::QsPose& x) { return S::Rig::Finite(x); });
    if (model_ok) {
        havok = c.model;
    }
    if (!S::HavokModel(c.reference, c.parents, reference)) {
        reference.clear(); // (only the log line below uses it)
    }
    if (!model_ok && !S::HavokModel(c.local, c.parents, havok)) {
        LOG_WARNING(Core, "VR sever: c{:04d}: its pose is not usable ({} bones): not cut", c.chr,
                    c.local.size());
        return;
    }
    if (!l->logged && !reference.empty()) {
        // The skeleton's reference pose against the model's bind joints: the same frame, often
        // not the same pose (c1000's head and tongue sit 0.9 m apart; a FLVER bone with no parent
        // keeps a local offset). Logged once; the posed mesh is what is checked (Sane below).
        l->logged = true;
        std::vector<float> gaps;
        for (std::size_t b = 0; b < m.bones.size(); ++b) {
            if (map[b] >= 0) {
                const S::Vec3 a = reference[static_cast<std::size_t>(map[b])].t, j = m.bones[b].joint;
                gaps.push_back(std::sqrt((a.x - j.x) * (a.x - j.x) + (a.y - j.y) * (a.y - j.y) +
                                         (a.z - j.z) * (a.z - j.z)));
            }
        }
        std::sort(gaps.begin(), gaps.end());
        LOG_INFO(Core, "VR sever: c{:04d}: {} of its model's {} bones in its skeleton ({}); "
                       "reference pose vs bind joints: median {:.1f} mm, largest {:.0f} mm",
                 c.chr, mapped, m.bones.size(), c.names.size(), gaps[gaps.size() / 2] * 1000.0f,
                 gaps.back() * 1000.0f);
    }
    std::vector<S::Matrix4> posed;
    std::vector<S::SkinTransform> skins;
    if (!S::FlverPose(m.bones, map, havok, posed) || !S::SkinsOf(m.bones, posed, skins)) {
        LOG_WARNING(Core, "VR sever: c{:04d}: its bones could not be posed: not cut", c.chr);
        return;
    }
    S::Vec3 contact{NAN, NAN, NAN}, edge{}, motion{};
    S::ToModel(c.world, c.contact, true, contact);
    // Too heavy to cut in half (S::MaxHalfKg): the head or the limb nearest the blow instead, by
    // the middle of its cut bone (the head without a known contact).
    if (c.heavy && c.site.limb == S::Limb::Torso) {
        S::Site best{};
        float best_d2 = INFINITY;
        for (const char* name : {"Head", "L UpperArm", "R UpperArm", "L Thigh", "R Thigh"}) {
            const S::Site site = S::SiteOf(name);
            const S::Chain ch = S::FindChain(site, l->refs);
            if (!ch.Valid()) {
                continue;
            }
            const S::Vec3 a = S::Joint(posed[static_cast<std::size_t>(ch.cut)]);
            const S::Vec3 b = S::Joint(posed[static_cast<std::size_t>(ch.child)]);
            const S::Vec3 mid{0.5f * (a.x + b.x) - contact.x, 0.5f * (a.y + b.y) - contact.y,
                              0.5f * (a.z + b.z) - contact.z};
            const float d2 = std::isfinite(contact.x)
                                 ? mid.x * mid.x + mid.y * mid.y + mid.z * mid.z
                                 : (site.limb == S::Limb::Head ? 0.0f : 1.0f);
            if (d2 < best_d2) {
                best = site;
                best_d2 = d2;
            }
        }
        LOG_INFO(Core, "VR sever: c{:04d}: too heavy to cut in half: its {} instead (the nearest)",
                 c.chr, S::LimbName(best.limb));
        c.site = best;
        limb = S::LimbName(c.site.limb);
    }
    const S::Chain chain = S::FindChain(c.site, l->refs);
    // A failure every kill here would repeat (no bones to cut through, no mesh blending in any
    // look): remembered for the model's limb and kind, or for this struck body alone (a creature's
    // segment by its own bone, one head of many: c1190's), as S::FailureScope says. The kill hook
    // then skips the bite there: no bite where no cut can come.
    const auto remember = [&] {
        switch (S::FailureScope(c.site, c.heavy, chain, l->refs)) {
        case S::FailScope::Model:
            MarkUncuttable(c.chr, SiteBit(c.site)); // (its model, whatever the pose)
            break;
        case S::FailScope::Body:
            g_failed_hits.Mark(S::StruckId(c.chr, c.site)); // (this body: another may cut)
            break;
        case S::FailScope::None:
            break;
        }
    };
    std::vector<std::uint8_t> det(m.bones.size());
    if (!chain.Valid() || !S::DetachedMask(c.site, l->refs, chain, det)) {
        LOG_INFO(Core, "VR sever: c{:04d}: no {} bones to cut through: not cut", c.chr, limb);
        remember();
        return;
    }
    // What the hide takes is a subtree of the game's own (Havok) skeleton, not of the model's: the
    // cut follows it (the review, 2026-11-13). A bone the model keeps loose that the skeleton hangs
    // under the cut part (c1000's L_Toe00 under the foot) goes with the piece; a limb the cut
    // keeps on purpose (under the cut bone in the model: an arm hanging off the neck) stays on the
    // corpse, scaled back (`keeps`). Roots and keeps are the skeleton's own.
    const std::size_t nh = c.parents.size();
    std::vector<std::int16_t> flver_of(nh, -1);
    for (std::size_t b = 0; b < m.bones.size(); ++b) {
        if (map[b] >= 0 && static_cast<std::size_t>(map[b]) < nh) {
            flver_of[static_cast<std::size_t>(map[b])] = static_cast<std::int16_t>(b);
        }
    }
    std::vector<std::uint8_t> below_cut(m.bones.size(), 0); // the model's subtree of the cut bone
    for (std::size_t b = 0; b < m.bones.size(); ++b) {
        std::int32_t at = static_cast<std::int32_t>(b);
        for (std::size_t steps = 0; at >= 0 && steps <= m.bones.size(); ++steps) {
            if (at == chain.cut) {
                below_cut[b] = 1;
                break;
            }
            at = m.bones[static_cast<std::size_t>(at)].parent;
        }
    }
    std::vector<std::uint8_t> hidden(nh, 0);
    std::size_t taken = 0;
    for (std::size_t h = 0; h < nh; ++h) {
        const std::int16_t f = flver_of[h];
        const std::int16_t up = c.parents[h];
        const bool under = up >= 0 && static_cast<std::size_t>(up) < h &&
                           hidden[static_cast<std::size_t>(up)];
        const bool cut_bone = f >= 0 && det[static_cast<std::size_t>(f)];
        const bool kept = f >= 0 && !cut_bone && below_cut[static_cast<std::size_t>(f)];
        if (cut_bone || (under && !kept)) {
            hidden[h] = 1;
            if (f >= 0 && !cut_bone) {
                det[static_cast<std::size_t>(f)] = 1;
                ++taken;
            }
        }
    }
    std::array<std::int16_t, HideRoots> roots{}, keeps{};
    std::uint8_t nroots = 0, nkeeps = 0;
    bool scattered = false;
    for (std::size_t h = 0; h < nh; ++h) {
        const std::int16_t up = c.parents[h];
        const bool parent_hidden = up >= 0 && static_cast<std::size_t>(up) < h &&
                                   hidden[static_cast<std::size_t>(up)];
        if (hidden[h] && !parent_hidden) {
            scattered |= nroots == roots.size();
            if (nroots < roots.size()) {
                roots[nroots++] = static_cast<std::int16_t>(h);
            }
        } else if (!hidden[h] && parent_hidden) {
            scattered |= nkeeps == keeps.size();
            if (nkeeps < keeps.size()) {
                keeps[nkeeps++] = static_cast<std::int16_t>(h);
            }
        }
    }
    if (scattered) {
        // (More than HideRoots roots or kept limbs: some of it could not be hidden or kept.)
        LOG_INFO(Core, "VR sever: c{:04d}: its {} is too scattered in its skeleton: not cut",
                 c.chr, limb);
        return;
    }
    if (taken > 0 || nkeeps > 0) {
        LOG_INFO(Core, "VR sever: c{:04d}: its skeleton's {}: {} more bones go with the piece, {} "
                       "limbs stay (scaled back)",
                 c.chr, limb, taken, nkeeps);
    }
    S::Slash slash{};
    const S::Vec3 blade{c.tip.x - c.base.x, c.tip.y - c.base.y, c.tip.z - c.base.z};
    if (c.blade && S::ToModel(c.world, blade, false, edge) &&
        S::ToModel(c.world, c.tip_vel, false, motion)) {
        slash = {true, edge, motion};
    }
    const S::Plane plane = S::CutPlane(S::Joint(posed[static_cast<std::size_t>(chain.cut)]),
                                       S::Joint(posed[static_cast<std::size_t>(chain.child)]),
                                       contact, c.site, slash);
    if (!plane.ok) {
        LOG_INFO(Core, "VR sever: c{:04d}: its {} bone is too short to cut: not cut", c.chr, limb);
        return;
    }
    // The meshes the cut crosses (the rest stay whole on their side).
    std::vector<std::size_t> mixed;
    std::uint32_t vertices = 0, triangles = 0;
    for (std::size_t i = 0; i < m.meshes.size(); ++i) {
        const S::ModelMesh& mesh = m.meshes[i];
        if (mesh.simulated || mesh.vertices.empty() || !S::Drawn(mesh, c.disp) ||
            S::Classify(mesh.vertices, det) != S::Side::Mixed) {
            continue;
        }
        if (mesh.vertices.size() > S::EnemyLimits.input_vertices ||
            mesh.triangles.size() > S::EnemyLimits.input_triangles) {
            LOG_INFO(Core, "VR sever: c{:04d}: mesh {} ({} vertices) is over a cut's limits: not cut",
                     c.chr, mesh.group, mesh.vertices.size());
            return;
        }
        mixed.push_back(i);
        vertices += static_cast<std::uint32_t>(mesh.vertices.size());
        triangles += static_cast<std::uint32_t>(mesh.triangles.size());
    }
    if (mixed.empty()) {
        LOG_INFO(Core, "VR sever: c{:04d}: no mesh blends into its {}: not cut", c.chr, limb);
        // In none of the model's looks either (the display mask is this one's): never at this
        // site (c1110's head: its own mesh, no blend into the body; else a bite every head kill).
        if (!c.heavy) {
            bool any_look = false;
            for (std::size_t i = 0; i < m.meshes.size() && !any_look; ++i) {
                const S::ModelMesh& mesh = m.meshes[i];
                any_look = !mesh.simulated && !mesh.vertices.empty() &&
                           S::Classify(mesh.vertices, det) == S::Side::Mixed;
            }
            if (!any_look) {
                remember();
            }
        }
        return;
    }
    const S::Admit admit = g_jobs.Reserve(c.key, c.site.limb, static_cast<std::uint32_t>(mixed.size()),
                                          vertices, triangles);
    // Thrown past here (an allocation): its slot freed (the review, 2026-11-13); a retired or
    // stale ticket's Retire does nothing.
    struct RetireIfThrown {
        S::Ticket ticket;
        int pending = std::uncaught_exceptions();
        ~RetireIfThrown() {
            if (std::uncaught_exceptions() > pending) {
                g_jobs.Retire(ticket);
            }
        }
    } retire_if_thrown{admit.ticket};
    if (admit.why != S::Refusal::None) {
        LOG_INFO(Core, "VR sever: c{:04d}: not cut ({})", c.chr, S::RefusalName(admit.why));
        return;
    }
    S::Snapshot snapshot;
    snapshot.detached_bones = det;
    snapshot.cut.origin = plane.origin;
    snapshot.cut.normal = plane.normal;
    snapshot.cut.bone_from = S::Joint(posed[static_cast<std::size_t>(chain.cut)]);
    snapshot.cut.bone_to = S::Joint(posed[static_cast<std::size_t>(chain.child)]);
    snapshot.cut.skin_tilt = S::SkinTiltPerM;
    const std::int32_t parent = m.bones[static_cast<std::size_t>(chain.cut)].parent;
    snapshot.cut.body_cap_bone = static_cast<std::uint16_t>(parent >= 0 ? parent : chain.cut);
    snapshot.cut.piece_cap_bone = static_cast<std::uint16_t>(chain.child);
    snapshot.parts.reserve(mixed.size());
    for (const std::size_t i : mixed) {
        const S::ModelMesh& mesh = m.meshes[i];
        S::Part part{static_cast<std::uint16_t>(i), S::ClothMaterial(mesh.material), {},
                     mesh.triangles, S::CutLayer(mesh.group, mesh.material)};
        float size = 0.0f, moved = 0.0f;
        if (!S::Pose(mesh.vertices, skins, part.vertices) ||
            !Sane(mesh.vertices, part.vertices, size, moved)) {
            // (A mantle or a vest: cloth the skeleton does not pose. The headset run, 2026-11-13,
            // lost whole cuts to them.) Left whole on the body: the rest is still cut.
            LOG_INFO(Core, "VR sever: c{:04d}: mesh {} posed wrong (its size x{:.2f}, its middle "
                           "moved {:.2f} of its size): left on the body, not cut",
                     c.chr, mesh.group, size, moved);
            continue;
        }
        snapshot.parts.push_back(std::move(part));
    }
    if (snapshot.parts.empty()) {
        g_jobs.Retire(admit.ticket);
        LOG_INFO(Core, "VR sever: c{:04d}: none of the meshes it crosses posed right: not cut", c.chr);
        return;
    }
    const s64 t_posed = NowNs();
    if (const S::Refusal r = g_jobs.Submit(admit.ticket, std::move(snapshot)); r != S::Refusal::None) {
        g_jobs.Retire(admit.ticket);
        LOG_INFO(Core, "VR sever: c{:04d}: not cut ({})", c.chr, S::RefusalName(r));
        return;
    }
    S::Error error = S::Error::None;
    g_jobs.RunNext([&error](const S::Snapshot& in) {
        S::EnemyCut r = S::CutEnemy(in);
        error = r.error;
        return r;
    });
    const s64 t_cut = NowNs();
    c.cut_ns = t_cut;
    const std::optional<S::EnemyCut> cut = g_jobs.Take(admit.ticket);
    if (!cut) {
        g_jobs.Retire(admit.ticket);
        LOG_INFO(Core, "VR sever: c{:04d} the {}: not cut ({}); {:.1f} ms", c.chr, limb,
                 ErrorName(error), Ms(t0, t_cut));
        return;
    }
    LOG_INFO(Core,
             "VR sever: c{:04d} the {} through the {}: CUT ({} meshes, {} rings, {} joined, {} holes "
             "filled, {} open), tilt {:.0f} deg at {:.0f}%{}; model read {:.1f} ms, pose {:.1f} ms, "
             "cut {:.1f} ms, {:.1f} ms after the kill",
             c.chr, limb, S::CutName(c.site), mixed.size(), cut->loops, cut->joined,
             cut->filled, cut->open_chains, plane.tilt_deg, plane.t * 100.0f,
             slash.known ? "" : " (no blade: square)", read_ms, Ms(t0, t_posed) - read_ms,
             Ms(t_posed, t_cut), Ms(c.ns, t_cut));
    Dump(c, *l, skins, det, plane, *cut);
    // The piece off the corpse: its roots in the skeleton (found above) shrunk from now on.
    std::int16_t anchor = -1;
    for (std::int32_t b = parent >= 0 ? parent : chain.cut; b >= 0 && anchor < 0;
         b = m.bones[static_cast<std::size_t>(b)].parent) {
        anchor = map[static_cast<std::size_t>(b)];
    }
    if (nroots == 0) {
        // (Its cut bone and every detached one missing from the skeleton: the corpse could not
        // be hidden; a piece flying off a whole corpse would be a twin. The review, 2026-11-13.)
        g_jobs.Retire(admit.ticket);
        LOG_INFO(Core, "VR sever: c{:04d}: none of its {} bones is in its skeleton: not cut", c.chr,
                 limb);
        return;
    }
    const S::QsPose anchor_kill =
        anchor >= 0 ? havok[static_cast<std::size_t>(anchor)] : S::QsPose{};
    // Where the corpse now ends: the hidden root's joint nearest the cut (model space, the kill).
    S::StumpSection section;
    std::int16_t stump_root = -1;
    float nearest = INFINITY;
    for (std::uint8_t i = 0; i < nroots; ++i) {
        const S::Vec3 j = havok[static_cast<std::size_t>(roots[i])].t;
        const float d = (j.x - plane.origin.x) * (j.x - plane.origin.x) +
                        (j.y - plane.origin.y) * (j.y - plane.origin.y) +
                        (j.z - plane.origin.z) * (j.z - plane.origin.z);
        if (std::isfinite(d) && d < nearest) {
            nearest = d;
            section.joint = j;
            stump_root = roots[i];
        }
    }
    // The piece's bone (Havok): the cut bone's own (a hidden root: its parent is the body's), else
    // the stump's (the body's bone at the cut). Never another hidden root: a skeleton without the cut
    // bone hides loose ones (cloth, twist: the 2026-11-16 mock's c9020 arm "turned 177 deg" in 52 ms
    // on one). The piece starts on it as the corpse holds it at the swap (Rebase).
    std::int16_t piece_bone = map[static_cast<std::size_t>(chain.cut)];
    if (stump_root >= 0 && stump_root == piece_bone) {
        const auto along = S::Detail::Sub(S::Joint(posed[static_cast<std::size_t>(chain.child)]),
                                          S::Joint(posed[static_cast<std::size_t>(chain.cut)]));
        S::Detail::Unit(along, S::MinBoneM, section.axis);
    }
    const bool own_bone = piece_bone >= 0 && static_cast<std::size_t>(piece_bone) < nh &&
                          hidden[static_cast<std::size_t>(piece_bone)];
    if (!own_bone) {
        piece_bone = anchor;
    }
    const S::QsPose piece_kill =
        own_bone ? havok[static_cast<std::size_t>(piece_bone)] : anchor_kill;
    // Stage both replacement halves; hide only after their GPU uploads are ready.
    if (MakePieces(c, *l, skins, det, plane, *cut, anchor_kill, piece_kill, section, roots,
                   nroots, keeps, nkeeps, anchor, piece_bone)) {
        LOG_INFO(Core, "VR sever: c{:04d}: its {} staged for cutting ({} root bones)", c.chr,
                 limb, nroots);
    } else {
        LOG_INFO(Core, "VR sever: c{:04d}: nothing of its {} could be drawn: not cut", c.chr, limb);
    }
    // The cut's meshes are copied into the drawn models: its slot is freed at once.
    g_jobs.Retire(admit.ticket);
}

// ---- The pieces, drawn (the in-frame pass, Producer::Sever) --------------------------------------
// A cut leaves two things to draw that the game has no model for: the piece that flies off (its
// own meshes, cut, with their own textures, and its flesh caps) and the body's stump caps. Each is
// put as runtime models (A::PutModel: one a texture, the flesh one more), centred on its middle,
// and placed every camera update: the piece by its flight (core/vr/sever_feel.h), the stump where
// the cut was. The corpse's own piece is hidden (Hide) as they appear.
// A piece's models: one a texture, the flesh's always kept (PutPiece); more (a hunter's outfit of
// many textures) are left out, logged.
constexpr std::size_t MaxPieceModels = 10;
// A piece and a stump a cut: the last eight cuts. They last as long as their corpse (the user,
// 2026-11-13: "is there a way to remember and keep it split"): until its handle no longer finds
// it (CorpseThere: the area left, a lamp's rest), it is alive again, or a newer cut needs its slot
// (the oldest cut's pair goes; its corpse stays cut, HiddenSlots of them). Each eye's model cache
// holds them (vr_inframe.cpp MaxModels 96, shared textures). Drawn only within MaxDrawM.
constexpr std::size_t MaxPieces = 16; // eight cuts (2026-11-13: "8 cuts")
static_assert(2 * MaxPieceModels <= S::RevealMailbox::MaxModels);
// A reveal slot is held only while its cut stages and first draws (RevealDone gives it back),
// never for the cut's life: a handful at once, whatever halves the cuts have.
static_assert(MaxPieces <= S::RevealMailbox::Slots);
constexpr float MaxDrawM = 40.0f;
constexpr s64 CorpseCheckNs = 250'000'000;
constexpr std::size_t ClearPoints = 128; // a piece's outermost vertices (its head clearance)
// A piece's bone's speed before the swap: from two samples this far apart (a frame at 60 fps or
// more: the two hooks of one frame read one pose), no farther (a stale one says nothing).
constexpr s64 StartVelMinNs = 12'000'000;
constexpr s64 StartVelMaxNs = 100'000'000;
constexpr float CarryMaxMps = 4.0f; // the body's speed a piece keeps at most (a knock, a fall)
struct Piece {
    bool on{false};
    bool still{false}; // the stump: where the cut was
    std::array<s32, MaxPieceModels> models{};
    std::uint8_t count{0};
    std::array<float, 9> lin{}; // world <- model, column vectors (ChrCtrl's W, row i = R_i)
    S::Feel::Flight flight{};
    S::Feel::SettledTransform settled{};
    float ground{0.0f};      // the floor under it (the map's, while it flies; the kill's feet first)
    float kill_ground{0.0f}; // the kill's feet: a piece fallen far below them is gone
    float ground_target{0.0f}; // the floor where it comes down (PlanStep): taken once it is above
    S::Vec3 cut_at{};          // its middle at the cut (world), before the head's clearance
    // Its flight against the map (logged as it comes to rest): casts made, their time.
    std::uint32_t casts{0};
    float cast_us{0.0f}, cast_us_max{0.0f};
    bool flight_logged{false};
    bool hide_seen{false}; // its corpse's part hidden at least once: drawn (and flying) from then
    u32 reveal{0};         // shared by piece + stump; unique across slot/actor reuse
    s64 visible_ns{0};     // first camera list after hiding; flight and blood start together
    bool gpu_seen{false};  // first complete draw recorded for every active eye
    // Its fall's plan: its arc flown ahead on the worker (MakePieces): where it starts (world),
    // where it comes down and its arc's top from there; then cast against the map in the camera
    // hook, a step a frame (PlanStep: 0 its clearance, 1 the wall on its way, 2 the floor, 3 done).
    S::Vec3 plan_start{}, plan_land{}, plan_top{};
    std::uint8_t plan_stage{0};
    float plan_keep{1.0f}; // of its level speed (a wall on the way)
    bool rechecked{false}; // the floor under where it came to rest checked (CheckRest)
    float reach{0.1f}; // its farthest vertex from its middle (the cull sphere)
    float rest{0.1f};  // its vertices' RMS distance from its middle: it lands on that
    s64 born_ns{0}, last_ns{0};
    // The stump: the corpse it is on, its middle (the kill's model space), its bone at the kill
    // (model space) and, once the corpse's pose is seen, world <- the kill's model space as the
    // bone has moved since (SeverPose: W now * bone now * bone at the kill^-1).
    S::ActorKey key{};
    const u8* addr{nullptr}; // its corpse (CorpseThere), the piece's and the stump's alike
    u32 handle{0};
    s32 model{-1};
    s64 checked_ns{0};
    S::Vec3 middle_model{};
    S::Affine anchor_kill_inverse{}; // computed once by the worker; immutable while following
    S::Affine follow{};
    bool following{false};
    s64 ragdoll_ns{0}; // the last follow from a ragdoll's drawn pose (after the update)
    // The piece (not the stump): its own bone at the kill (inverse, model space) and, while its
    // cut is not yet seen, world <- the kill's model space as that bone holds it now (SeverPose):
    // where it starts at the swap (Rebase), once. Its outermost vertices about its middle (world
    // axes at the cut): kept clear of the hunter's head from where it starts.
    S::Affine start_kill_inverse{};
    S::Affine start_follow{};
    bool start_follow_set{false};
    bool rebased{false}; // tried at the swap (with start_follow_set: put on its bone)
    bool start_own_bone{true}; // its own (the cut bone), else the body's at the cut (the stump's)
    // ... and how fast that bone carries it (world, m/s: two of the hook's samples a frame or more
    // apart), so it never seems to stop as it comes off; its launch's own inputs, aimed again from
    // where it is at the swap (the blade's motion, world, and the limb).
    S::Vec3 start_prev_at{};
    s64 start_prev_ns{0};
    S::Vec3 start_vel{};
    bool start_vel_set{false};
    S::Vec3 launch_tip_vel{};
    S::Limb launch_limb{S::Limb::None};
    bool replan{false};  // its fall to plan again from where it starts now (PlanStep)
    float rebase_moved{0.0f}, rebase_turn_deg{0.0f}, rebase_shift{0.0f}; // (the timeline's log)
    float rebase_carried{0.0f};   // m/s added along the body's own motion (the timeline's log)
    bool rebase_placed{false};    // put on its bone (the values above: what its bone did)
    bool rebase_refused{false};   // its bone moved more than a body can: where the kill had it
    std::array<S::Vec3, ClearPoints> clear{};
    std::uint8_t clear_count{0};
    // When (steady ns): the kill, the worker took it, its model read, the cut made, its pieces
    // built; and its part first hidden on the corpse. The cut's timeline, logged as it is seen.
    std::array<s64, 5> times{};
    s64 hidden_ns{0};
    // The feel: a charged cut's; announced (its haptics and its sound, once, as it is first
    // drawn); the cut's normal toward the piece (the kill's model space) and its cap's middle
    // there, where the blood leaves it (sever_feel.h StumpGushes: the stump's beats, the piece's
    // first burst).
    bool charged{false};
    S::Feel::StrikeHands hands{S::Feel::StrikeHands::Weapon};
    bool announced{false};
    u32 feel{0}; // its cut's (Capture::feel): the tear continues the bite of the same cut
    S::Vec3 normal_model{};
    S::Vec3 cap_model{};
    float wound{0.06f}; // its cap's size (CapRms): the gushes'
    bool has_cap{false};
    // Each gush leaves from where the cut was as it starts (world) and then flies free of the
    // piece and of the falling corpse (bit g of gush_set: gush g's start taken).
    std::array<S::Vec3, 8> gush_at{}, gush_dir{};
    std::uint8_t gush_set{0};
};
std::mutex g_piece_mutex;
std::array<Piece, MaxPieces> g_pieces{}; // under g_piece_mutex
std::atomic<std::uint32_t> g_piece_count{0}; // the render tick's one test while nothing flies

// (Under g_piece_mutex.) The count, and the awake bit with it.
void CountPieces() {
    const auto n = static_cast<std::uint32_t>(
        std::count_if(g_pieces.begin(), g_pieces.end(), [](const Piece& p) { return p.on; }));
    g_piece_count.store(n, std::memory_order_release);
    SetAwake(SeverAwakePieces, n > 0);
}
std::atomic<s32> g_next_model{A::RuntimeModelBase};
// The camera's world position at its last update (SeverRenderTick): a new piece starts clear of
// the hunter's head (MakePieces).
std::array<std::atomic<float>, 3> g_view{};
std::atomic<bool> g_view_set{false}; // stored every camera update while awake
constexpr float HeadClearM = 0.55f;
constexpr float PlanStepS = 1.0f / 60.0f;
constexpr int PlanSteps = 240; // 4 s at most

S::Vec3 MiddleNow(const S::Feel::Flight& f) {
    return {f.middle.x + f.offset.x, f.middle.y + f.offset.y, f.middle.z + f.offset.z};
}
constexpr float MaxClearShiftM = 2.5f;

// A piece's arc flown ahead from where it starts, never in a frame's own work (the worker's, or a
// fall job's after the swap): where it comes down and its top, both from where it starts; the
// camera hook casts them against the map (PlanStep). At most PlanSteps flight steps, once a cut.
struct Arc {
    S::Vec3 start{}, land{}, top{};
};
Arc PlanArc(S::Feel::Flight sim, float ground) {
    const S::Vec3 start = MiddleNow(sim);
    float top = start.y;
    for (int i = 0; i < PlanSteps; ++i) {
        if (S::Feel::StepFlight(sim, PlanStepS, ground) || sim.resting || sim.slumping) {
            break;
        }
        top = std::max(top, MiddleNow(sim).y);
    }
    const S::Vec3 land = MiddleNow(sim);
    return {start,
            {land.x - start.x, land.y - start.y, land.z - start.z},
            {0.0f, top - start.y, 0.0f}};
}

// The textures' share keys (ModelTexture::share_key: one GPU image for every model with one):
// the flesh tile, the dark stand-in, and an enemy's own texture by its model and name.
constexpr u64 FleshShareKey = 1;
constexpr u64 DarkShareKey = 2;
u64 TextureShareKey(s32 chr, const std::string& name) {
    const u64 key = (static_cast<u64>(static_cast<u32>(chr)) << 32) ^
                    static_cast<u32>(std::hash<std::string>{}(name));
    return key < 16 ? key + 16 : key;
}

// A part whose texture is not read (or that names none): a dark flesh-brown, never a white piece
// in a dark place (the review, 2026-11-13).
A::ModelTexture DarkTexture() {
    A::ModelTexture t;
    t.format = A::ModelTexture::Format::Rgba8;
    t.width = t.height = 4;
    t.levels = 1;
    t.level_offset = {0};
    t.blocks.resize(4 * 4 * 4);
    for (std::size_t i = 0; i < t.blocks.size(); i += 4) {
        t.blocks[i] = 34;
        t.blocks[i + 1] = 26;
        t.blocks[i + 2] = 24;
        t.blocks[i + 3] = 255;
    }
    t.share_key = DarkShareKey;
    return t;
}

// Half its resolution (its top level dropped) for a texture of 1024 px and more: a quarter of its
// memory and its upload; a flying, falling piece never shows its finest level (the review,
// 2026-11-13: a 2048 px BC7 chain is 5.3 MB, an eye).
void HalveTexture(A::ModelTexture& t) {
    if (t.levels < 2 || t.width < 1024 || t.height < 256 || t.level_offset.size() < t.levels ||
        t.level_offset[1] == 0 || t.level_offset[1] >= t.blocks.size()) {
        return;
    }
    const u32 cut = t.level_offset[1];
    std::vector<u8>(t.blocks.begin() + cut, t.blocks.end()).swap(t.blocks);
    t.level_offset.erase(t.level_offset.begin());
    t.level_offset.resize(t.levels - 1);
    for (u32& o : t.level_offset) {
        o -= cut;
    }
    t.width /= 2;
    t.height /= 2;
    t.levels -= 1;
}

// The flesh tile (the caps' texture), baked by a stretch's first read-ahead, else its first cut
// (the worker's; ReleaseAll).
std::optional<A::ModelTexture> g_flesh;
const A::ModelTexture& FleshTexture() {
    if (!g_flesh) {
        S::Flesh::Tile t = S::Flesh::Bake();
        A::ModelTexture out;
        out.format = A::ModelTexture::Format::Rgba8;
        out.width = out.height = t.size;
        out.levels = t.levels;
        out.level_offset = std::move(t.offsets);
        out.blocks = std::move(t.rgba);
        out.share_key = FleshShareKey;
        out.Share(); // (every cap of every piece: one buffer)
        g_flesh.emplace(std::move(out)); // Share can throw. Only publish a complete tile.
    }
    return *g_flesh;
}

// One model being built: its vertices about the piece's middle, a remap a source mesh.
struct Group {
    std::string texture; // its TPF name ("" none; "\x01" the flesh)
    A::Model model;
};
constexpr const char* FleshGroup = "\x01";

Group& GroupOf(std::vector<Group>& groups, const std::string& texture) {
    for (Group& g : groups) {
        if (g.texture == texture) {
            return g;
        }
    }
    groups.push_back({texture, {}});
    return groups.back();
}

// A mesh's triangles into the groups (caps to the flesh), its vertices about `middle`.
void AddMesh(std::vector<Group>& groups, const S::Mesh& mesh, const std::string& texture,
             S::Vec3 middle, bool caps_only) {
    std::vector<std::uint32_t> remap;
    for (const bool cap : {false, true}) {
        if (!cap && caps_only) {
            continue;
        }
        Group* g = nullptr;
        remap.assign(mesh.vertices.size(), 0xFFFFFFFFu);
        for (const S::Triangle& t : mesh.triangles) {
            if ((t.material == S::Cut{}.cap_material) != cap) {
                continue;
            }
            if (g == nullptr) {
                g = &GroupOf(groups, cap ? FleshGroup : texture);
            }
            for (const std::uint32_t i : t.index) {
                if (remap[i] == 0xFFFFFFFFu) {
                    const S::Vertex& v = mesh.vertices[i];
                    A::ModelVertex mv{};
                    mv.position[0] = v.position.x - middle.x;
                    mv.position[1] = v.position.y - middle.y;
                    mv.position[2] = v.position.z - middle.z;
                    mv.normal[0] = v.normal.x;
                    mv.normal[1] = v.normal.y;
                    mv.normal[2] = v.normal.z;
                    const auto uv = cap ? S::Flesh::TexCoord(v.uv[0], v.uv[1]) : v.uv;
                    mv.uv[0] = uv[0];
                    mv.uv[1] = uv[1];
                    remap[i] = static_cast<std::uint32_t>(g->model.vertices.size());
                    g->model.vertices.push_back(mv);
                }
                g->model.indices.push_back(remap[i]);
            }
        }
    }
}

// A texture of the enemy's as drawn (decoded from its TPF, else its own texture file, halved):
// its earlier cuts' (shared, no copy of the blocks), else decoded and kept for the next (within
// CacheBudgetBytes). Pieces keep a kept texture's blocks alive past a trim of the model, never
// longer than their own life. False: not in its files (`why`).
bool EnemyTexture(Loaded& l, const std::string& name, A::ModelTexture& out, std::string& why) {
    for (const auto& [kept, t] : l.decoded) {
        if (kept == name) {
            out = t;
            return true;
        }
    }
    if (!A::DecodeTpfTexture(l.model.textures, name, out, why) &&
        (l.more_textures.empty() || !A::DecodeTpfTexture(l.more_textures, name, out, why))) {
        return false;
    }
    HalveTexture(out);
    out.share_key = TextureShareKey(l.chr, name);
    out.blocks.shrink_to_fit();
    out.Share(); // (this cut's other groups and pieces of one texture share it too)
    const std::size_t more = name.capacity() + sizeof(out) + out.Bytes().size() +
                             out.level_offset.size() * sizeof(u32);
    if (l.bytes + more <= CacheBudgetBytes) {
        l.decoded.emplace_back(name, out);
        l.bytes += more;
    }
    return true;
}

// The groups' models put (their textures the enemy's: EnemyTexture), into one piece.
bool PutPiece(std::vector<Group>& groups, Loaded& l, Piece& piece, std::string& textures,
              std::size_t& dropped) {
    double squares = 0.0;
    std::size_t points = 0;
    // The flesh first: over the limit, a texture's group goes, never the cut's own look.
    std::stable_partition(groups.begin(), groups.end(),
                          [](const Group& g) { return g.texture == FleshGroup; });
    for (Group& g : groups) {
        if (g.model.indices.empty()) {
            continue;
        }
        if (piece.count >= MaxPieceModels) {
            ++dropped;
            continue;
        }
        if (g.texture == FleshGroup) {
            g.model.diffuse = FleshTexture();
            piece.has_cap = true;
        } else if (!g.texture.empty()) {
            std::string why;
            if (!EnemyTexture(l, g.texture, g.model.diffuse, why)) {
                g.model.diffuse = DarkTexture();
                textures += fmt::format(" {} ({})", g.texture, why);
            }
        } else {
            g.model.diffuse = DarkTexture();
        }
        for (int c = 0; c < 3; ++c) {
            g.model.min[c] = 1e9f;
            g.model.max[c] = -1e9f;
        }
        for (const A::ModelVertex& v : g.model.vertices) {
            for (int c = 0; c < 3; ++c) {
                g.model.min[c] = std::min(g.model.min[c], v.position[c]);
                g.model.max[c] = std::max(g.model.max[c], v.position[c]);
            }
            const float d2 = v.position[0] * v.position[0] + v.position[1] * v.position[1] +
                             v.position[2] * v.position[2];
            piece.reach = std::max(piece.reach, std::sqrt(d2));
            squares += d2;
            ++points;
        }
        const s32 id = g_next_model.fetch_add(1);
        g.model.id = id;
        A::PutModel(id, std::make_shared<const A::Model>(std::move(g.model)));
        piece.models[piece.count++] = id;
    }
    piece.rest = points > 0 ? static_cast<float>(std::sqrt(squares / points)) : 0.1f;
    return piece.count > 0;
}

S::Vec3 Middle(const std::vector<const S::Mesh*>& meshes, bool caps_only) {
    double x = 0, y = 0, z = 0;
    std::size_t n = 0;
    for (const S::Mesh* m : meshes) {
        for (const S::Triangle& t : m->triangles) {
            if (caps_only && t.material != S::Cut{}.cap_material) {
                continue;
            }
            for (const std::uint32_t i : t.index) {
                x += m->vertices[i].position.x;
                y += m->vertices[i].position.y;
                z += m->vertices[i].position.z;
                ++n;
            }
        }
    }
    return n == 0 ? S::Vec3{} : S::Vec3{float(x / n), float(y / n), float(z / n)};
}

// The wound's size: its cap vertices' RMS distance from `middle` (their middle; its blood's size).
float CapRms(const std::vector<const S::Mesh*>& meshes, S::Vec3 middle,
             S::StumpSection* section = nullptr) {
    double sum = 0;
    std::size_t n = 0;
    for (const S::Mesh* m : meshes) {
        for (const S::Triangle& t : m->triangles) {
            if (t.material != S::Cut{}.cap_material) {
                continue;
            }
            for (const std::uint32_t i : t.index) {
                const S::Vec3& v = m->vertices[i].position;
                if (section) section->Include(v);
                sum += double(v.x - middle.x) * (v.x - middle.x) +
                       double(v.y - middle.y) * (v.y - middle.y) +
                       double(v.z - middle.z) * (v.z - middle.z);
                ++n;
            }
        }
    }
    return n == 0 ? 0.06f : static_cast<float>(std::sqrt(sum / double(n)));
}

// Into a free slot (a corpse gone frees its own); all taken, the oldest cut's piece and stump
// go together (its corpse stays as cut, drawn without them: the oldest is the farthest, mostly).
void Place(Piece piece) {
    std::vector<s32> gone;
    std::optional<S::ActorKey> evicted;
    {
        std::scoped_lock lock{g_piece_mutex};
        std::size_t at = g_pieces.size();
        for (std::size_t i = 0; i < g_pieces.size() && at == g_pieces.size(); ++i) {
            if (!g_pieces[i].on) {
                at = i;
            }
        }
        if (at == g_pieces.size()) {
            at = 0;
            for (std::size_t i = 1; i < g_pieces.size(); ++i) {
                if (g_pieces[i].born_ns < g_pieces[at].born_ns) {
                    at = i;
                }
            }
            const S::ActorKey key = g_pieces[at].key;
            evicted = key;
            const s64 born = g_pieces[at].born_ns;
            for (Piece& p : g_pieces) {
                if (p.on && p.key == key && p.born_ns == born) {
                    gone.insert(gone.end(), p.models.begin(), p.models.begin() + p.count);
                    p = {};
                }
            }
        }
        g_pieces[at] = piece;
        CountPieces();
    }
    for (const s32 id : gone) {
        A::DropModel(id);
    }
    if (evicted && !(*evicted == piece.key)) {
        DropAnchor(*evicted);
    }
}

// The corpse is still in the world: its handle still finds it in WorldChrMan's block set (as the
// hit resolve reads it: bloodborne_deflect.cpp), the same character model. Game:Main.
bool CorpseThere(const u8* chr, u32 handle, s32 model) {
    if (g_base == nullptr || !Plausible(chr)) {
        return false;
    }
    const u8* const mgr = Read<const u8*>(g_base + WorldChrManRva);
    const u8* const blocks = Plausible(mgr) ? Read<const u8*>(mgr + OffsetMgrBlocks) : nullptr;
    if (!Plausible(blocks)) {
        return false;
    }
    const u32 block = (handle >> 14) & 0x3F;
    if (static_cast<s32>(block) >= std::min(Read<s32>(blocks + OffsetBlocksCount) + 2, MaxBlocks)) {
        return false;
    }
    const u8* const set = Read<const u8*>(mgr + OffsetMgrChrSets + 8 * block);
    const u32 slot = handle & 0x3FFF;
    if (!Plausible(set) || static_cast<s32>(slot) >= std::min(Read<s32>(set), MaxSetCount)) {
        return false;
    }
    const u8* const entries = Read<const u8*>(set + OffsetSetEntries);
    return Plausible(entries) && Read<const u8*>(entries + ChrEntrySize * slot) == chr &&
           Read<u32>(chr + OffsetHandle) == handle && Read<s32>(chr + OffsetChrId) == model;
}

// Every piece of this corpse goes (it is alive again). Its models are dropped.
void RetirePieces(const S::ActorKey& key) {
    std::vector<s32> gone;
    {
        std::scoped_lock lock{g_piece_mutex};
        for (Piece& p : g_pieces) {
            if (p.on && p.key == key) {
                gone.insert(gone.end(), p.models.begin(), p.models.begin() + p.count);
                p = {};
            }
        }
        CountPieces();
    }
    for (const s32 id : gone) {
        A::DropModel(id);
    }
}

// The models a build put (PutPiece), dropped again unless their piece is placed: an exception
// part way (out of memory) never leaves them registered with no piece to drop them (a code
// review, 2026-11-13).
struct Unplaced {
    std::array<const Piece*, 2> pieces{};
    Unplaced() = default;
    Unplaced(const Unplaced&) = delete;
    Unplaced& operator=(const Unplaced&) = delete;
    ~Unplaced() {
        for (const Piece* p : pieces) {
            for (std::uint8_t k = 0; p != nullptr && k < p->count; ++k) {
                A::DropModel(p->models[k]);
            }
        }
    }
};

// The worker, the cut done: the piece's and the stump's models, the piece's flight.
// Stage the piece and stump, then arm the pose hook. Nothing is hidden until both halves
// are uploaded for the active eyes. False: leave the original corpse whole.
bool MakePieces(const Capture& c, Loaded& l, std::span<const S::SkinTransform> skins,
                std::span<const std::uint8_t> det, const S::Plane& plane, const S::EnemyCut& cut,
                const S::QsPose& anchor_kill, const S::QsPose& piece_kill, S::StumpSection section,
                const std::array<std::int16_t, HideRoots>& roots, std::uint8_t nroots,
                const std::array<std::int16_t, HideRoots>& keeps, std::uint8_t nkeeps,
                std::int16_t anchor, std::int16_t piece_bone) {
    const S::Model& m = l.model;
    // The piece: the cut meshes' piece side, and every mesh wholly the piece's (posed).
    std::vector<const S::Mesh*> piece_meshes;
    std::vector<std::string> piece_textures;
    std::vector<bool> cut_mesh(m.meshes.size(), false);
    for (const S::PartCut& p : cut.parts) {
        cut_mesh[p.mesh] = true;
        piece_meshes.push_back(&p.piece);
        piece_textures.push_back(m.meshes[p.mesh].diffuse);
    }
    std::vector<S::Mesh> whole;
    whole.reserve(m.meshes.size());
    for (std::size_t i = 0; i < m.meshes.size(); ++i) {
        const S::ModelMesh& mesh = m.meshes[i];
        if (cut_mesh[i] || mesh.simulated || mesh.vertices.empty() || !S::Drawn(mesh, c.disp) ||
            S::Classify(mesh.vertices, det) != S::Side::Piece) {
            continue;
        }
        S::Mesh posed;
        if (!S::Pose(mesh.vertices, skins, posed.vertices)) {
            continue;
        }
        posed.triangles = mesh.triangles;
        whole.push_back(std::move(posed));
        piece_textures.push_back(mesh.diffuse);
    }
    for (const S::Mesh& w : whole) {
        piece_meshes.push_back(&w);
    }
    piece_meshes.push_back(&cut.piece_cap);
    piece_textures.push_back("");
    const S::Vec3 middle = Middle(piece_meshes, false);
    std::vector<Group> groups;
    for (std::size_t k = 0; k < piece_meshes.size(); ++k) {
        AddMesh(groups, *piece_meshes[k], piece_textures[k], middle, false);
    }
    // World: W's rows {R_i, t_i}.
    const auto& w = c.world;
    const std::array<float, 9> lin{w[0], w[1], w[2], w[4], w[5], w[6], w[8], w[9], w[10]};
    const auto to_world = [&](S::Vec3 v) {
        return S::Vec3{lin[0] * v.x + lin[1] * v.y + lin[2] * v.z + w[3],
                       lin[3] * v.x + lin[4] * v.y + lin[5] * v.z + w[7],
                       lin[6] * v.x + lin[7] * v.y + lin[8] * v.z + w[11]};
    };
    const auto dir_to_world = [&](S::Vec3 v) {
        return S::Vec3{lin[0] * v.x + lin[1] * v.y + lin[2] * v.z,
                       lin[3] * v.x + lin[4] * v.y + lin[5] * v.z,
                       lin[6] * v.x + lin[7] * v.y + lin[8] * v.z};
    };
    const s64 now = NowNs();
    std::string missing;
    std::size_t dropped = 0; // groups over MaxPieceModels
    bool have_piece = false, have_stump = false;
    Piece piece, stump; // both must outlive Unplaced, including exception unwinding
    piece.on = true;
    piece.lin = lin;
    piece.ground = w[7]; // the enemy's feet at the kill
    piece.kill_ground = w[7];
    piece.ground_target = w[7];
    piece.born_ns = piece.last_ns = now;
    piece.key = c.key;
    piece.addr = c.addr;
    piece.handle = c.handle;
    piece.model = c.chr;
    piece.charged = c.charged;
    piece.hands = c.hands;
    piece.normal_model = plane.normal;
    piece.middle_model = middle;
    Unplaced unplaced;
    unplaced.pieces[0] = &piece;
    if (PutPiece(groups, l, piece, missing, dropped)) {
        // A one-sided cut can produce a textured piece with no exposed flesh. Only
        // a real wound has a blood origin and needs a burst slot at reveal.
        if (piece.has_cap) {
            piece.cap_model = Middle(piece_meshes, true);
            piece.wound = CapRms(piece_meshes, piece.cap_model);
        }
        // Away from the hunter: their head (the camera), else the blade's base.
        const S::Vec3 at = to_world(middle);
        S::Vec3 head{NAN, NAN, NAN};
        if (g_view_set.load(std::memory_order_acquire)) {
            head = {g_view[0].load(std::memory_order_relaxed),
                    g_view[1].load(std::memory_order_relaxed),
                    g_view[2].load(std::memory_order_relaxed)};
        }
        const bool seen = std::isfinite(head.x) && std::isfinite(head.y) && std::isfinite(head.z);
        S::Vec3 away = seen      ? S::Vec3{at.x - head.x, 0.0f, at.z - head.z}
                       : c.blade ? S::Vec3{at.x - c.base.x, 0.0f, at.z - c.base.z}
                                 : S::Vec3{};
        // Clear of the head: an enemy killed mid-attack reaches for it, and its piece, frozen in
        // that pose, would start around the camera (the 2026-11-13 mock: a Scourge Beast's upper
        // body, arms out, over the whole view). Moved straight away from the head, level, as
        // little as it takes (its vertices sampled). Its outermost samples are kept: at the swap
        // it is cleared again from where its bone has taken it (Rebase).
        std::size_t total = 0;
        for (const S::Mesh* mesh : piece_meshes) {
            total += mesh->vertices.size();
        }
        const std::size_t stride = std::max<std::size_t>(1, total / 4096);
        std::vector<S::Vec3> samples; // about its middle, world axes at the cut
        samples.reserve(4096 + piece_meshes.size());
        for (const S::Mesh* mesh : piece_meshes) {
            for (std::size_t i = 0; i < mesh->vertices.size(); i += stride) {
                const S::Vec3& v = mesh->vertices[i].position;
                samples.push_back(dir_to_world({v.x - middle.x, v.y - middle.y, v.z - middle.z}));
            }
        }
        float shift = 0.0f;
        const float level = std::sqrt(away.x * away.x + away.z * away.z);
        if (seen && level > 1e-3f) {
            const S::Vec3 a{away.x / level, 0.0f, away.z / level};
            shift = S::Feel::ClearanceShift(samples, at, head, a, HeadClearM, MaxClearShiftM);
            if (shift > 0.0f) {
                LOG_INFO(Core, "VR sever: c{:04d}: the piece starts {:.2f} m further from the "
                               "hunter's head (it reached for it)",
                         c.chr, shift);
            }
            away = a;
        }
        const std::size_t outer = std::min(samples.size(), ClearPoints);
        std::nth_element(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(outer),
                         samples.end(), [](const S::Vec3& a, const S::Vec3& b) {
                             return a.x * a.x + a.y * a.y + a.z * a.z >
                                    b.x * b.x + b.y * b.y + b.z * b.z;
                         });
        std::copy_n(samples.begin(), outer, piece.clear.begin());
        piece.clear_count = static_cast<std::uint8_t>(outer);
        S::QsPose bone_kill = piece_kill;
        bone_kill.s = {1.0f, 1.0f, 1.0f};
        piece.start_kill_inverse = S::InverseOf(bone_kill);
        piece.start_own_bone = piece_bone != anchor;
        piece.launch_tip_vel = c.tip_vel;
        piece.launch_limb = c.site.limb;
        const S::Feel::Launch launch =
            S::Feel::PieceLaunch(dir_to_world(plane.normal), c.tip_vel, c.site.limb, c.charged,
                                 away, piece.rest);
        piece.flight = S::Feel::StartFlight(
            {at.x + away.x * shift, at.y, at.z + away.z * shift}, piece.rest, launch);
        piece.cut_at = at;
        // It lands on its box (world axes at the cut, about its middle), turned as it tumbles.
        S::Vec3 lo{INFINITY, INFINITY, INFINITY}, hi{-INFINITY, -INFINITY, -INFINITY};
        for (const S::Mesh* mesh : piece_meshes) {
            for (const S::Vertex& v : mesh->vertices) {
                const S::Vec3 d = dir_to_world({v.position.x - middle.x, v.position.y - middle.y,
                                                v.position.z - middle.z});
                lo = {std::min(lo.x, d.x), std::min(lo.y, d.y), std::min(lo.z, d.z)};
                hi = {std::max(hi.x, d.x), std::max(hi.y, d.y), std::max(hi.z, d.z)};
            }
        }
        S::Feel::SetBox(piece.flight, lo, hi);
        // Its own shape (a sample of its vertices): its outline to land on, its flattest side to
        // settle onto (a cut arm lies flat, its hand down).
        {
            std::size_t total = 0;
            for (const S::Mesh* mesh : piece_meshes) {
                total += mesh->vertices.size();
            }
            const std::size_t stride = std::max<std::size_t>(1, total / 1024);
            std::vector<S::Vec3> shape;
            shape.reserve(1024 + piece_meshes.size());
            for (const S::Mesh* mesh : piece_meshes) {
                for (std::size_t i = 0; i < mesh->vertices.size(); i += stride) {
                    const S::Vec3& v = mesh->vertices[i].position;
                    shape.push_back(dir_to_world({v.x - middle.x, v.y - middle.y, v.z - middle.z}));
                }
            }
            S::Feel::SetShape(piece.flight, shape);
        }
        // Its arc flown ahead (here, never in a frame): where it comes down and its top, from
        // where it starts; the camera hook casts those against the map (PlanStep).
        const Arc arc = PlanArc(piece.flight, piece.ground);
        piece.plan_start = arc.start;
        piece.plan_land = arc.land;
        piece.plan_top = arc.top;
        have_piece = true;
    }
    // The stump: only the body side's flesh caps. The game's corpse keeps its exterior;
    // there is no second named-texture decode here (FleshTexture is already cached).
    std::vector<const S::Mesh*> stump_meshes;
    for (const S::PartCut& p : cut.parts) {
        stump_meshes.push_back(&p.body);
    }
    stump_meshes.push_back(&cut.body_cap);
    const S::Vec3 stump_middle = Middle(stump_meshes, true);
    std::vector<Group> stump_groups;
    for (const S::Mesh* mesh : stump_meshes) {
        AddMesh(stump_groups, *mesh, "", stump_middle, true);
    }
    unplaced.pieces[1] = &stump;
    stump.on = true;
    stump.still = true;
    stump.lin = lin;
    stump.born_ns = stump.last_ns = now;
    stump.key = c.key;
    // Keep a wrist cut from hanging beyond the hidden joint, while preserving off-centre
    // contours and cuts whose flesh already spans the joint. Measured once at the kill.
    stump.wound = CapRms(stump_meshes, stump_middle,
                         S::Detail::Dot(section.axis, section.axis) > 0 ? &section : nullptr);
    const S::Vec3 stump_where = section.Place(stump_middle);
    stump.addr = c.addr;
    stump.handle = c.handle;
    stump.model = c.chr;
    stump.middle_model = stump_where;
    stump.anchor_kill_inverse = S::InverseOf(anchor_kill);
    stump.charged = c.charged;
    stump.hands = c.hands;
    // The cut's feel (haptics, sound) plays once, as its piece is first drawn; from the stump
    // only when there is no piece.
    stump.announced = have_piece;
    stump.normal_model = plane.normal;
    stump.cap_model = stump_where;
    if (PutPiece(stump_groups, l, stump, missing, dropped)) {
        stump.flight = S::Feel::StartFlight(to_world(stump_where), 0.1f, {});
        stump.flight.resting = true;
        have_stump = true;
    }
    // Either half is enough, as before the staged reveal (2026-11-16: never fewer
    // dismemberments): a piece whose body side got no flesh cap still flies off; a cap whose piece
    // could not be drawn still shows the cut. Only the halves that exist are staged, and the limb
    // hides once every one of their models is up (an unplaced half's models: Unplaced drops them).
    if (!have_piece && !have_stump) return false;
    const u32 staged = (have_piece ? u32(piece.count) : 0u) + (have_stump ? u32(stump.count) : 0u);
    struct RevealReservation {
        u32 token;
        ~RevealReservation() { S::g_reveals.Release(token); }
    } reservation{S::g_reveals.Reserve(staged)};
    if (reservation.token == 0) return false;
    // The cut's timeline so far (logged as it is first seen: SeverRenderTick).
    const std::array<s64, 5> times{c.ns, c.start_ns, c.loaded_ns, c.cut_ns, NowNs()};
    if (have_piece) {
        piece.reveal = reservation.token;
        piece.times = times;
        piece.feel = c.feel;
        Place(piece);
        unplaced.pieces[0] = nullptr;
    }
    if (have_stump) {
        stump.reveal = reservation.token;
        stump.times = times;
        stump.feel = c.feel;
        Place(stump);
        unplaced.pieces[1] = nullptr;
    }
    // The hooks are armed now, but do not shrink anything until all models are uploaded
    // for both active eyes. The pending camera lists only preload; their clock is stopped.
    Hide(c.key, roots, nroots, keeps, nkeeps, anchor, have_piece ? piece_bone : std::int16_t{-1},
         c.addr, c.handle, c.chr, c.ns, reservation.token);
    reservation.token = 0; // Hidden owns the reservation until retirement/unhide
    LOG_INFO(Core, "VR sever: c{:04d}: staged piece: {} models ({:.2f} m across), "
                   "stump: {}; launch speed {:.1f} m/s{}{}{}",
             c.chr, have_piece ? piece.count : 0, piece.reach * 2.0f, have_stump ? stump.count : 0,
             std::sqrt(piece.flight.velocity.x * piece.flight.velocity.x +
                       piece.flight.velocity.y * piece.flight.velocity.y +
                       piece.flight.velocity.z * piece.flight.velocity.z),
             dropped > 0 ? fmt::format("; {} textures' parts left out (over {} models)", dropped,
                                       MaxPieceModels)
                         : std::string{},
             missing.empty() ? "" : "; textures not read:", missing);
    return true;
}

// VR_SEVER_TEST=<a struck body's name, e.g. "Head", "L UpperArm">: test builds and mock runs
// only. Every 3 s, once VR_SEVER_TEST_AFTER_S (0) have passed since the hunter first stood in a
// world, the worker takes the nearest other character within 80 m that it has not cut and cuts
// it as if that limb had been struck by a lethal claw heavy (no blade: a square cut), up to
// VR_SEVER_TEST_COUNT (3) of them. It reads the game's character lists from this thread, so
// never in a play build.
// VR_SEVER_TEST may list several, in turn ("Head;L UpperArm;Spine"); VR_SEVER_TEST_EVERY_S keeps
// that long between cuts; VR_SEVER_TEST_KILL=1 kills each as it is cut (its HP 0, as a lethal hit
// leaves it), so it falls as a real claw kill's would.
const std::vector<S::Site>& TestSites() {
    static const std::vector<S::Site> sites = [] {
        std::vector<S::Site> out;
        const char* v = std::getenv("VR_SEVER_TEST");
        std::string all = v != nullptr ? v : "";
        for (std::size_t at = 0; at <= all.size();) {
            const std::size_t end = std::min(all.find(';', at), all.size());
            const S::Site site = S::SiteOf(std::string_view(all).substr(at, end - at));
            if (site.limb != S::Limb::None) {
                out.push_back(site);
            }
            at = end + 1;
        }
        return out;
    }();
    return sites;
}
S::Site TestSite() {
    static std::size_t next = 0; // (the worker's alone)
    const auto& sites = TestSites();
    return sites.empty() ? S::Site{} : sites[next++ % sites.size()];
}
// VR_SEVER_TEST_WEAKEN=1: instead of cutting, the nearest enemy is left at 1 HP, so the next hit
// kills it: a scripted Beast Claw heavy then runs the real path (the damage detour's Decide).
bool TestWeaken() {
    static const bool on = [] {
        const char* v = std::getenv("VR_SEVER_TEST_WEAKEN");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    return on;
}
bool TestOn() {
    return !TestSites().empty() || TestWeaken();
}

void TestTick() {
    static int left = [] {
        const char* v = std::getenv("VR_SEVER_TEST_COUNT");
        return v != nullptr ? std::max(0, std::atoi(v)) : 3;
    }();
    static const s64 every_ns = [] {
        const char* v = std::getenv("VR_SEVER_TEST_EVERY_S");
        return static_cast<s64>((v != nullptr ? std::max(0.0, std::atof(v)) : 0.0) * 1e9);
    }();
    static const bool kill = [] {
        const char* v = std::getenv("VR_SEVER_TEST_KILL");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    static s64 last_ns = 0;
    if (left <= 0 || g_base == nullptr || (last_ns != 0 && NowNs() - last_ns < every_ns)) {
        return;
    }
    const u8* const mgr = Read<const u8*>(g_base + WorldChrManRva);
    const u8* const blocks = Plausible(mgr) ? Read<const u8*>(mgr + OffsetMgrBlocks) : nullptr;
    const u8* const player = Plausible(mgr) ? Read<const u8*>(mgr + OffsetMainPlayer) : nullptr;
    if (!Plausible(blocks) || !Plausible(player)) {
        return;
    }
    const auto at = [](const u8* chr, S::Vec3& out) {
        const u8* const ctrl = Read<const u8*>(chr + OffsetChrCtrl);
        if (!Plausible(ctrl) || Read<const u8*>(ctrl + OffsetCtrlOwner) != chr) {
            return false;
        }
        std::array<float, 12> w{};
        std::memcpy(w.data(), ctrl + OffsetCtrlWorld, sizeof(w));
        out = {w[3], w[7], w[11]};
        return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
    };
    S::Vec3 me{};
    if (!at(player, me)) {
        return;
    }
    static const s64 after_ns = [] {
        const char* v = std::getenv("VR_SEVER_TEST_AFTER_S");
        return static_cast<s64>((v != nullptr ? std::max(0.0, std::atof(v)) : 0.0) * 1e9);
    }();
    static s64 first_ns = 0;
    if (first_ns == 0) {
        first_ns = NowNs();
    }
    if (NowNs() - first_ns < after_ns) {
        return;
    }
    const u8* best = nullptr;
    float best_d2 = 80.0f * 80.0f;
    const s32 nblocks = std::clamp(Read<s32>(blocks + OffsetBlocksCount) + 2, 0, MaxBlocks);
    for (s32 b = 0; b < nblocks; ++b) {
        const u8* const set = Read<const u8*>(mgr + OffsetMgrChrSets + 8 * static_cast<u32>(b));
        if (!Plausible(set)) {
            continue;
        }
        const s32 count = std::clamp(Read<s32>(set), 0, MaxSetCount);
        const u8* const entries = Read<const u8*>(set + OffsetSetEntries);
        if (!Plausible(entries)) {
            continue;
        }
        for (s32 i = 0; i < count; ++i) {
            const u8* const chr = Read<const u8*>(entries + ChrEntrySize * static_cast<u32>(i));
            if (!Plausible(chr) || chr == player || Read<s32>(chr + OffsetChrId) <= 0 ||
                g_jobs.Known(KeyOf(chr))) {
                continue;
            }
            S::Vec3 p{};
            if (!at(chr, p)) {
                continue;
            }
            const float d2 = (p.x - me.x) * (p.x - me.x) + (p.y - me.y) * (p.y - me.y) +
                             (p.z - me.z) * (p.z - me.z);
            if (d2 < best_d2) {
                best = chr;
                best_d2 = d2;
            }
        }
    }
    if (best == nullptr) {
        return;
    }
    const u8* const mods = Read<const u8*>(best + OffsetChrModules);
    u8* const data = Plausible(mods) ? Read<u8*>(mods + OffsetDataModule) : nullptr;
    if (TestWeaken()) {
        if (Plausible(data) && Read<s32>(data + OffsetHp) > 1) {
            const s32 one = 1;
            std::memcpy(data + OffsetHp, &one, sizeof(one));
            LOG_INFO(Core, "VR sever: TEST c{:04d} at {:.1f} m left at 1 HP: the next hit kills it",
                     Read<s32>(best + OffsetChrId), std::sqrt(best_d2));
            --left;
            last_ns = NowNs();
        }
        return;
    }
    const S::Site site = TestSite();
    LOG_INFO(Core, "VR sever: TEST c{:04d} at {:.1f} m (hp {}): cut as if its {} were struck{}",
             Read<s32>(best + OffsetChrId), std::sqrt(best_d2),
             Plausible(data) ? Read<s32>(data + OffsetHp) : -1, S::LimbName(site.limb),
             kill ? ", and killed" : "");
    MeleeHitInfo none{};
    if (SeverKill(best, site, {NAN, NAN, NAN}, none, false)) {
        --left;
        last_ns = NowNs();
        if (kill && Plausible(data)) {
            const s32 zero = 0;
            std::memcpy(data + OffsetHp, &zero, sizeof(zero)); // (its pose was copied first)
        }
    }
}

// Nothing cut is left: everything the worker read goes (the enemies' models and their textures,
// the display masks, the flesh tile, the models it could not read). The next cut reads afresh (on
// this thread, never a frame's). The flesh tile stays while the weapon is in the hand (1.4 MB):
// baked again, it would be in the next cut's path, a one-blow kill's (a code review, 2026-11-16:
// +28 ms after a minute's idle); it goes with the worker.
void ReleaseAll(bool keep_flesh) {
    const std::size_t models = g_models.size();
    const std::size_t bytes = CachedBytes();
    std::vector<std::unique_ptr<Loaded>>().swap(g_models);
    std::vector<s32>().swap(g_unreadable);
    std::vector<s32>().swap(g_mismatched);
    g_disp_masks.reset();
    if (!keep_flesh) {
        g_flesh.reset();
    }
    for (auto& chr : g_cached_chr) {
        chr.store(0, std::memory_order_relaxed);
    }
    g_prefetch_asked.Clear(); // (the next hit on them asks again)
    LOG_INFO(Core, "VR sever: unused for a while: what the cuts read is freed ({} enemy models, "
                   "{:.1f} MB){}",
             models, static_cast<double>(bytes) / 1048576.0,
             keep_flesh && g_flesh ? "; the flesh tile kept (the weapon in the hand)" : "");
}

// The worker thread: started as a dismembering weapon is equipped, in either form (or a cut is
// queued). Cuts first, then read-aheads (SeverPrefetch). It frees what it read once nothing has
// used it for ReleaseIdleNs with the weapon in the hand, ReleaseAfterNs without (cut corpses about
// or not: the next cut reads afresh, here, never in a frame), and with no such weapon and nothing
// cut left it frees it at once and ends. Joined at exit.
struct WorkerHost {
    std::thread thread;
    bool running{false}; // under g_queue_mutex
    bool stop{false};    // under g_queue_mutex
    WorkerHost() = default;
    WorkerHost(const WorkerHost&) = delete;
    WorkerHost& operator=(const WorkerHost&) = delete;
    ~WorkerHost() {
        {
            std::scoped_lock lock{g_queue_mutex};
            stop = true;
        }
        g_wake.notify_all();
        if (thread.joinable()) {
            thread.join();
        }
    }
};
WorkerHost g_worker;
constexpr s64 ReleaseAfterNs = 5'000'000'000; // no dismembering weapon in the hand
// The weapon in the hand: its next kill most likely the same enemies (2026-11-16: the
// 5 s release made nearly every cut read its model again, tens of ms between blow and cut).
constexpr s64 ReleaseIdleNs = 60'000'000'000;

// (Under g_queue_mutex.) No cut queued, none drawn, no corpse hidden.
bool NothingCut() {
    return g_queue.empty() && g_piece_count.load(std::memory_order_acquire) == 0 &&
           g_hide_count.load(std::memory_order_acquire) == 0;
}

void WorkerRun() {
    Common::SetCurrentThreadName("VR:Sever");
    const bool test = TestOn();
    bool held = false; // anything read and kept since the last release
    // The flesh tile baked now (the weapon just equipped: 28 ms at 512 px), never in a cut's
    // path; freed with the rest (ReleaseAll), baked again by the next read-ahead or cut.
    try {
        FleshTexture();
        held = true;
    } catch (const std::exception&) {
    }
    s64 last_use_ns = NowNs(); // the last cut or read-ahead
    bool bake_failed = false;  // the flesh tile's bake threw: not tried again until re-equipped
    for (;;) {
        std::optional<Capture> c;
        s32 prefetch = 0;
        bool release = false, leave = false, keep_flesh = false;
        {
            std::unique_lock lock{g_queue_mutex};
            const auto equipped = [] { return (SeverAwake() & SeverAwakeWeapon) != 0; };
            // The weapon in the hand and no flesh tile (freed while it was away; the worker kept
            // alive by cut corpses when it came back): work, baked at once, never in the next
            // cut (a code review, 2026-11-16: the equip's wake went back to sleep for 5 s).
            // g_flesh is this thread's own.
            const auto ready = [&] {
                return g_worker.stop || !g_queue.empty() || !g_prefetch.Empty() ||
                       (!g_flesh && !bake_failed && equipped());
            };
            if (test) {
                g_wake.wait_for(lock, std::chrono::seconds(3), ready);
            } else if (held || !NothingCut()) {
                // Something to free, or cut corpses to outlast: looked at every few seconds.
                g_wake.wait_for(lock, std::chrono::seconds(5), ready);
            } else {
                g_wake.wait(lock, [&] { return ready() || !equipped(); });
            }
            if (g_worker.stop) {
                return;
            }
            if (!g_queue.empty()) {
                c = std::move(g_queue.front());
                g_queue.pop_front();
            } else if (!g_prefetch.Take(prefetch)) {
                const bool weapon = equipped();
                leave = !test && !weapon && NothingCut(); // (a test's worker stays: it polls)
                release = held && (leave || NowNs() - last_use_ns >=
                                                (weapon ? ReleaseIdleNs : ReleaseAfterNs));
                keep_flesh = weapon && !leave;
            }
        }
        if (prefetch != 0) {
            // Read ahead of its kill (its model, its texture file, the looks): the cut finds them.
            held = true; // ModelOf may retain a model or a failed lookup before a later step throws.
            try {
                double read_ms = 0.0;
                if (ModelOf(prefetch, read_ms, true) != nullptr) {
                    DispMask(-1);
                    FleshTexture(); // (its bake off the cut's path too)
                }
            } catch (const std::exception& e) {
                g_prefetch_asked.Forget(prefetch);
                LOG_WARNING(Core, "VR sever: c{:04d}: reading ahead failed ({})", prefetch,
                            e.what());
            }
            last_use_ns = NowNs();
            continue;
        }
        if (release) {
            ReleaseAll(keep_flesh);
            held = false;
        }
        if (leave) {
            std::scoped_lock lock{g_queue_mutex};
            if (g_worker.stop) {
                return;
            }
            if (g_queue.empty() && g_prefetch.Empty() && (SeverAwake() & SeverAwakeWeapon) == 0) {
                g_flesh.reset(); // (kept through an idle release: it goes with the worker)
                g_worker.running = false; // (its last step: StartWorker joins it)
                return;
            }
            continue; // a cut, or an eligible weapon again, meanwhile
        }
        if (!c) {
            // Idle with the weapon in the hand and no flesh tile (freed while it was away, the
            // worker already running when it came back): baked now, never in the next cut.
            const bool weapon_now = (SeverAwake() & SeverAwakeWeapon) != 0;
            bake_failed = bake_failed && weapon_now; // (put away: tried again when it is back)
            if (!g_flesh && !bake_failed && weapon_now) {
                try {
                    FleshTexture();
                    held = true;
                } catch (const std::exception& e) {
                    bake_failed = true; // (never a loop of failing bakes)
                    LOG_WARNING(Core, "VR sever: the flesh tile could not be baked ({})", e.what());
                }
            }
            if (test) {
                TestTick(); // (test only: it may queue the next cut)
            }
            continue;
        }
        held = true;
        try {
            Cut(std::move(*c));
        } catch (const std::exception& e) {
            LOG_WARNING(Core, "VR sever: c{:04d}: the cut failed ({}): not cut", c->chr, e.what());
        }
        TrimCache(); // (its textures decoded may have grown it)
        last_use_ns = NowNs();
    }
}

// Caller holds g_queue_mutex. After clearing running under this same lock the old worker
// returns without taking it again. A failed start leaves the next request free to retry.
bool EnsureWorkerLocked() {
    if (g_worker.stop) {
        return false;
    }
    if (g_worker.running) {
        return true;
    }
    if (g_worker.thread.joinable()) {
        g_worker.thread.join();
    }
    try {
        g_worker.thread = std::thread{WorkerRun};
        g_worker.running = true;
        return true;
    } catch (const std::exception& e) {
        LOG_WARNING(Core, "VR sever: could not start the worker ({})", e.what());
        return false;
    }
}

void StartWorker() {
    {
        std::scoped_lock lock{g_queue_mutex};
        if (!EnsureWorkerLocked()) {
            return;
        }
    }
    g_wake.notify_all();
}

} // namespace

void InstallSever(VAddr base, u64 size) {
    (void)size;
    g_base = reinterpret_cast<const u8*>(base);
    if (!SeverEnabled()) {
        return;
    }
    LOG_INFO(Core, "VR sever: on (VR_SEVER=0 turns it off); lethal Beast Claw and transformed "
                   "Burial Blade swings cut by chance, the stronger the swing the surer{}",
             TestOn() ? "; TEST: the nearest characters are cut too" : "");
    if (TestOn()) {
        StartWorker(); // the worker polls for the test
    }
}

void SeverWeaponEquipped(bool on) {
    if (!SeverEnabled()) {
        return;
    }
    SetAwake(SeverAwakeWeapon, on);
    if (on) {
        StartWorker();
    } else {
        // (nothing cut left: the worker frees what it read and ends.) The lock taken between the
        // change and the notice: the worker sees one or the other, never neither.
        { std::scoped_lock lock{g_queue_mutex}; }
        g_wake.notify_all();
    }
}

bool SeverEnabled() {
    // On by default (2026-11-13: "Yes, on by default"); VR_SEVER=0 turns it off.
    static const bool on = [] {
        const char* v = std::getenv("VR_SEVER");
        return v == nullptr || std::strcmp(v, "0") != 0;
    }();
    return on;
}

// A cut corpse's piece hidden in its pose and its stump moved with it: after the pose copy (the
// behaviour's local pose: a death animation) and after the character's update (a ragdoll writes
// the model-space pose there, the one drawn: the game's kill ragdoll and every ragdoll fall).
static void ApplyHide(u8* chr, bool after_update) {
    if (g_hide_count.load(std::memory_order_acquire) == 0 || !Plausible(chr)) {
        return;
    }
    const S::ActorKey key = KeyOf(chr);
    if ((g_hidden_bits.load(std::memory_order_acquire) & HideBit(key.actor)) == 0) {
        return; // not a cut corpse (one test for nearly every character)
    }
    bool listed = false;
    for (const auto& actor : g_hidden_actor) {
        if (actor.load(std::memory_order_acquire) == key.actor) {
            listed = true;
            break;
        }
    }
    if (!listed) {
        return; // not a cut corpse: no lock
    }
    std::array<std::int16_t, HideRoots> roots{}, keeps{};
    std::uint8_t count = 0, keep_count = 0;
    std::int16_t anchor = -1, piece_bone = -1;
    s64 cut_ns = 0;
    u32 reveal = 0;
    bool revealed = false;
    std::size_t slot = 0;
    {
        std::scoped_lock lock{g_hide_mutex};
        for (std::size_t i = 0; i < g_hidden.size(); ++i) {
            const Hidden& h = g_hidden[i];
            if (h.count > 0 && h.key == key) {
                roots = h.havok;
                count = h.count;
                keeps = h.keeps;
                keep_count = h.keep_count;
                anchor = h.anchor;
                piece_bone = h.piece_bone;
                cut_ns = h.ns;
                reveal = h.reveal;
                revealed = h.revealed;
                slot = i;
                break;
            }
        }
    }
    if (count == 0) {
        return;
    }
    const u8* const mods = Read<const u8*>(chr + OffsetChrModules);
    const u8* const data = Plausible(mods) ? Read<const u8*>(mods + OffsetDataModule) : nullptr;
    if (Plausible(data) && Read<s32>(data + OffsetHp) > 0 && NowNs() - cut_ns > HideAliveGraceNs) {
        Unhide(key); // brought back (a lamp's rest): whole again, and it can be cut again
        g_jobs.Forget(key);
        RetirePieces(key);
        return;
    }
    const bool first_hide = reveal != 0 &&
        g_applied_cut[slot].load(std::memory_order_acquire) != reveal;
    if (first_hide && !S::g_reveals.Ready(reveal)) return;
    const u8* const mod = Plausible(mods) ? Read<const u8*>(mods + OffsetBehaviorModule) : nullptr;
    const u8* const node = Plausible(mod) ? Read<const u8*>(mod + OffsetModNode) : nullptr;
    if (!Plausible(node)) {
        return;
    }
    const u8* const pose = node + OffsetNodePose;
    u8* const local = Read<u8*>(pose + OffsetPoseLocal);
    const s32 n = Read<s32>(pose + OffsetPoseLocalSize);
    if (!Plausible(local, 16) || n <= 0 || static_cast<std::size_t>(n) > S::MaxBones) {
        return;
    }
    if (first_hide && !S::g_reveals.ClaimHide(reveal)) return;
    const float scale[3] = {HideScale, HideScale, HideScale};
    // (VR_SEVER_LOG) what the hook found before its writes: the roots' local and model scales.
    const float local_scale_before =
        count > 0 && roots[0] >= 0 && roots[0] < n
            ? Read<float>(local + static_cast<std::size_t>(roots[0]) * HkQsBytes + 0x20)
            : NAN;
    for (std::uint8_t i = 0; i < count; ++i) {
        if (roots[i] >= 0 && roots[i] < n) {
            std::memcpy(local + static_cast<std::size_t>(roots[i]) * HkQsBytes + 0x20, scale,
                        sizeof(scale));
        }
    }
    const float back[3] = {1.0f / HideScale, 1.0f / HideScale, 1.0f / HideScale};
    for (std::uint8_t i = 0; i < keep_count; ++i) {
        if (keeps[i] >= 0 && keeps[i] < n) {
            std::memcpy(local + static_cast<std::size_t>(keeps[i]) * HkQsBytes + 0x20, back,
                        sizeof(back));
        }
    }
    const u8* const skeleton = Read<const u8*>(pose + OffsetPoseSkeleton);
    const u8* const parents = Plausible(skeleton) ? Read<const u8*>(skeleton + OffsetSkelParents)
                                                  : nullptr;
    // A ragdoll drives the model-space pose, and the game draws from it: the cut part is shrunk
    // there too, every bone of it (model space has no parents to carry a root's scale) onto its
    // root's joint. Only while the model pose is in sync (a stale one is recomputed from the local
    // pose, whose roots are already shrunk).
    u8* const model = Read<u8*>(pose + OffsetPoseModel);
    const bool model_ok = Plausible(model, 16) && Read<s32>(pose + OffsetPoseModelSize) == n &&
                          Read<u8>(pose + OffsetPoseModelInSync) != 0 && Plausible(parents, 2) &&
                          Read<s32>(skeleton + OffsetSkelParentCount) == n;
    float model_scale_before = NAN;
    if (model_ok) {
        // Each bone's hidden root (-1: none; -2: a kept limb, its subtree untouched).
        std::array<std::int16_t, S::MaxBones> root_of;
        std::fill_n(root_of.begin(), n, std::int16_t{-1});
        for (std::uint8_t i = 0; i < count; ++i) {
            if (roots[i] >= 0 && roots[i] < n) {
                root_of[static_cast<std::size_t>(roots[i])] = roots[i];
            }
        }
        for (std::uint8_t i = 0; i < keep_count; ++i) {
            if (keeps[i] >= 0 && keeps[i] < n) {
                root_of[static_cast<std::size_t>(keeps[i])] = -2;
            }
        }
        model_scale_before =
            count > 0 && roots[0] >= 0 && roots[0] < n
                ? Read<float>(model + static_cast<std::size_t>(roots[0]) * HkQsBytes + 0x20)
                : NAN;
        // Havok orders a skeleton's parents before their children: one pass.
        for (s32 b = 0; b < n; ++b) {
            const std::int16_t parent = Read<std::int16_t>(parents + 2 * static_cast<std::size_t>(b));
            if (root_of[static_cast<std::size_t>(b)] == -1 && parent >= 0 && parent < b) {
                root_of[static_cast<std::size_t>(b)] = root_of[static_cast<std::size_t>(parent)];
            }
            const std::int16_t r = root_of[static_cast<std::size_t>(b)];
            if (r < 0) {
                continue;
            }
            u8* const at = model + static_cast<std::size_t>(b) * HkQsBytes;
            if (r != b) {
                float joint[3];
                std::memcpy(joint, model + static_cast<std::size_t>(r) * HkQsBytes, sizeof(joint));
                std::memcpy(at, joint, sizeof(joint));
            }
            std::memcpy(at + 0x20, scale, sizeof(scale));
        }
    }
    // Publish only after this hook finishes the hide and (when available) stump following.
    // Tokens prevent an old actor/slot acknowledgement from starting a different cut.
    struct AppliedPublication {
        std::size_t slot;
        u32 token;
        ~AppliedPublication() {
            if (token != 0 && g_applied_cut[slot].load(std::memory_order_relaxed) != token)
                g_applied_cut[slot].store(token, std::memory_order_release);
        }
    } applied{slot, first_hide ? reveal : 0};
    // The stump rides its bone: its model transform now (the model pose while it is in sync, the
    // ragdoll's; else up the local pose's parents), in the world now.
    const u8* const ctrl = Read<const u8*>(chr + OffsetChrCtrl);
    if (anchor < 0 || anchor >= n || !Plausible(parents, 2) || !Plausible(ctrl) ||
        Read<const u8*>(ctrl + OffsetCtrlOwner) != chr) {
        return;
    }
    // A bone's model transform now: up the local pose's parents (composed), or the model pose's
    // own while it is in sync (a ragdoll's: the one drawn).
    const auto local_composed = [&](std::int16_t from) {
        std::array<std::int16_t, 64> up{};
        std::size_t depth = 0;
        for (std::int16_t b = from; b >= 0 && b < n && depth < up.size();
             b = Read<std::int16_t>(parents + 2 * static_cast<std::size_t>(b))) {
            up[depth++] = b;
        }
        S::QsPose composed{};
        for (std::size_t k = depth; k-- > 0;) {
            float f[12];
            std::memcpy(f, local + static_cast<std::size_t>(up[k]) * HkQsBytes, sizeof(f));
            composed = S::Compose(
                composed, {{f[0], f[1], f[2]}, {f[4], f[5], f[6], f[7]}, {f[8], f[9], f[10]}});
        }
        return composed;
    };
    const auto bone_now = [&](std::int16_t b) {
        if (model_ok) {
            float f[12];
            std::memcpy(f, model + static_cast<std::size_t>(b) * HkQsBytes, sizeof(f));
            return S::QsPose{{f[0], f[1], f[2]}, {f[4], f[5], f[6], f[7]}, {f[8], f[9], f[10]}};
        }
        return local_composed(b);
    };
    const S::QsPose composed = local_composed(anchor);
    S::QsPose bone = model_ok ? bone_now(anchor) : composed;
    // The piece's own bone, only while its cut is not yet seen (Rebase at the swap): its turn and
    // place now (the hide's scale on it is ours: not its). A hidden root keeps both, in either
    // pose: only the scale of a root, and its children's places, are written above.
    S::QsPose piece_now{};
    bool piece_ok = false;
    if (reveal != 0 && !revealed && piece_bone >= 0 && piece_bone < n) {
        piece_now = bone_now(piece_bone);
        piece_now.s = {1.0f, 1.0f, 1.0f};
        piece_ok = S::Rig::Finite(piece_now);
    }
    S::Affine world{};
    std::memcpy(world.data(), ctrl + OffsetCtrlWorld, sizeof(world));
    // VR_SEVER_LOG: what the game's pose holds for a cut corpse (a few lines a corpse).
    static const bool log = std::getenv("VR_SEVER_LOG") != nullptr;
    if (log) {
        static std::mutex log_mutex;
        static std::array<std::pair<S::ActorKey, std::pair<s64, int>>, 8> seen{};
        static std::array<int, 8> calls{};
        std::scoped_lock log_lock{log_mutex};
        std::size_t k = 0;
        while (k < seen.size() && !(seen[k].first == key) && seen[k].second.second != 0) {
            ++k;
        }
        if (k < seen.size()) {
            if (!(seen[k].first == key)) {
                seen[k] = {key, {0, 0}};
                calls[k] = 0;
            }
            ++calls[k];
            const s64 t = NowNs();
            auto& [last, lines] = seen[k].second;
            if (lines < 16 && t - last > 300'000'000) {
                last = t;
                ++lines;
                const u8* const rag = Read<const u8*>(ctrl + OffsetCtrlRagdoll);
                LOG_INFO(Core,
                         "VR sever: c{:04d} after the {} x{}: bones {} (model {} in sync {} / local "
                         "in sync {}, flags {}), ragdoll state {}; root scale before: local {:.3f} "
                         "model {:.3f}; anchor: model ({:.2f}, {:.2f}, {:.2f}) local-composed "
                         "({:.2f}, {:.2f}, {:.2f}); W t ({:.2f}, {:.2f}, {:.2f})",
                         Read<s32>(chr + OffsetChrId), after_update ? "update" : "pose copy",
                         calls[k], n,
                         Read<s32>(pose + OffsetPoseModelSize),
                         Read<u8>(pose + OffsetPoseModelInSync),
                         Read<u8>(pose + OffsetPoseLocalInSync),
                         Read<s32>(pose + OffsetPoseFlagsSize),
                         Plausible(rag) ? Read<s32>(rag + OffsetRagdollState) : -1,
                         local_scale_before, model_scale_before, bone.t.x, bone.t.y, bone.t.z,
                         composed.t.x, composed.t.y, composed.t.z, world[3], world[7], world[11]);
                calls[k] = 0;
            }
        }
    }
    // A ragdoll's pose (after the update) is the one drawn: while it drives the corpse, the pose
    // copy's (its death animation's, not drawn) never moves the stump.
    const s64 follow_now = NowNs();
    // (The render tick holds this lock for its whole loop: an NPC update never waits on it; the
    // stump follows next frame instead.)
    std::unique_lock lock{g_piece_mutex, std::try_to_lock};
    if (!lock.owns_lock()) {
        return;
    }
    for (Piece& p : g_pieces) {
        // Retirement/re-cut can happen while the pose is computed outside this lock.
        // Its bone and cached inverse must belong to the same cut, not just the actor.
        if (!p.on || !(p.key == key) || p.reveal != reveal) {
            continue;
        }
        if (!p.still) {
            // The piece, not yet seen: where its bone holds it now (the latest hook's: after the
            // update, the ragdoll's drawn pose), for its start at the swap. Seen, it flies free.
            if (piece_ok && !p.rebased) {
                p.start_follow =
                    S::Mul(S::Mul(world, S::AffineOf(piece_now)), p.start_kill_inverse);
                p.start_follow_set = true;
                // Its speed from samples a frame or more apart (the two hooks of one frame read
                // one pose: never their difference).
                const S::Vec3 at = S::Apply(p.start_follow, p.middle_model);
                const s64 since = follow_now - p.start_prev_ns;
                if (p.start_prev_ns == 0 || since >= StartVelMinNs) {
                    if (p.start_prev_ns != 0 && since <= StartVelMaxNs) {
                        const float dt = static_cast<float>(since) * 1e-9f;
                        p.start_vel = {(at.x - p.start_prev_at.x) / dt,
                                       (at.y - p.start_prev_at.y) / dt,
                                       (at.z - p.start_prev_at.z) / dt};
                        p.start_vel_set = std::isfinite(p.start_vel.x) &&
                                          std::isfinite(p.start_vel.y) &&
                                          std::isfinite(p.start_vel.z);
                    }
                    p.start_prev_at = at;
                    p.start_prev_ns = follow_now;
                }
            }
            continue;
        }
        if (!after_update && follow_now - p.ragdoll_ns < 500'000'000) {
            continue;
        }
        p.follow = S::Mul(S::Mul(world, S::AffineOf(bone)), p.anchor_kill_inverse);
        p.following = true;
        if (after_update && model_ok) {
            p.ragdoll_ns = follow_now;
        }
    }
}

void SeverPoseHidden(u8* chr) {
    ApplyHide(chr, false);
}

void SeverAfterUpdateHidden(u8* chr) {
    ApplyHide(chr, true);
}

namespace {
// ---- Its fall against the map, planned once -------------------------------------------------------
// The user (2026-11-13): pieces should land on the real floor and stop at walls, "lightweight",
// never "stuck on the air, or in weird configurations". Each piece's fall is planned once, as it
// is first drawn: its arc flown ahead on the CPU (no casts) to where it comes down, then the
// game's own sphere cast against its static map (bloodborne_evasion.cpp SweepMapSphere, the layers
// its ragdolls collide with, from the render-camera hook as its follow camera casts): one along
// that path (a wall: it flies only that far and drops there), one straight down where it comes
// down (the floor it lands on: a table top, a step, a lower ledge), and one from where it was cut
// to where it starts when the head's clearance moved it. As it comes to rest, one more down under
// where it lies: no floor right under it, it falls on to the one there is. Three to five casts a
// cut, none a frame, never under g_piece_mutex (the review, 2026-11-13: a character's update hook
// takes that lock): the render tick copies what a plan needs, casts, then applies.
constexpr float WallRadiusMaxM = 0.15f; // the path's sphere: half its size, 5-15 cm
constexpr float FloorRadiusM = 0.05f;
constexpr float LostBelowM = 40.0f;     // fallen this far below the kill's feet (a chasm): gone
constexpr float FloorDropM = LostBelowM; // a floor this far below where it comes down, at most

// Its outline's lowest point now (world height).
float LowestNow(const S::Feel::Flight& f) {
    const auto turned = S::Feel::TurnedPoints(f);
    float low = 0.0f;
    for (std::size_t i = 0; i < f.point_count; ++i) {
        low = std::min(low, turned[i].y);
    }
    return f.middle.y + f.offset.y + (f.point_count > 0 ? low : -f.radius);
}

// A plan step's or a rest check's copy of its piece (taken under g_piece_mutex) and its answer.
struct FallJob {
    std::size_t slot{0};
    s64 born{0};
    bool rest{false}; // a rest check, else a plan step
    S::Feel::Flight flight{};
    float ground{0.0f}, kill_ground{0.0f}, size{0.1f};
    S::Vec3 cut_at{};
    std::uint8_t stage{0};
    S::Vec3 plan_start{}, plan_land{}, plan_top{};
    float keep{1.0f};
    bool replan{false}; // its arc to fly ahead again first (put on its bone at the swap: Rebase)
};
struct FallAnswer {
    std::size_t slot{0};
    s64 born{0};
    bool rest{false};
    bool planned{false};                    // its arc flown again (replan): these replace its plan
    S::Vec3 plan_start{}, plan_land{}, plan_top{};
    std::uint8_t stage{0}; // the plan's stage now (3: done)
    bool floored{false};   // the floor where it comes down found (stage 2 done)
    float floor{0.0f};
    float keep{1.0f};    // of its level speed (a wall on the way)
    S::Vec3 moved{};     // its start set back (a wall between the cut and its clearance)
    bool refall{false};  // a rest check: no floor right under it
    std::uint32_t casts{0};
    float us{0.0f}, us_max{0.0f};
};

MapSweep Cast(FallAnswer& out, S::Vec3 from, S::Vec3 to, float radius, VR::Body::Vec3& normal,
              float& fraction) {
    const s64 t0 = NowNs();
    const MapSweep state = SweepMapSphere({from.x, from.y, from.z}, {to.x, to.y, to.z}, radius,
                                          normal, fraction);
    const float us = static_cast<float>(NowNs() - t0) * 1e-3f;
    ++out.casts;
    out.us += us;
    out.us_max = std::max(out.us_max, us);
    return state;
}

// The floor under (x, z), cast down from `top`: its height; nothing within FloorDropM: far below
// (it falls on, and is gone past LostBelowM); a start inside the map, a slope too steep to stand
// on or no world: `fallback`.
float FloorAt(FallAnswer& out, const FallJob& job, float x, float top, float z, float fallback) {
    VR::Body::Vec3 n{};
    float f = 0.0f;
    const S::Vec3 from{x, top, z}, to{x, top - FloorDropM, z};
    const MapSweep state = Cast(out, from, to, FloorRadiusM, n, f);
    if (state == MapSweep::Clear) {
        return job.kill_ground - 2.0f * LostBelowM;
    }
    if (state == MapSweep::Hit && f > 1e-3f && n.y > 0.5f) {
        return from.y + (to.y - from.y) * f - FloorRadiusM;
    }
    return fallback;
}

// One step of its fall's plan, at most one cast a frame (its arc was flown ahead on the worker):
// 0 the head's clearance moved it from where it was cut (never through a wall); 1 a wall on its
// level path (it flies only that far and drops at the wall's foot); 2 the floor where it comes
// down, cast from just over its arc's top (its own path is open space: never from inside a step
// going up, nor from over a ceiling).
FallAnswer PlanStep(const FallJob& job) {
    FallAnswer out{job.slot, job.born, false};
    out.stage = job.stage;
    const float radius = std::clamp(0.5f * job.size, 0.05f, WallRadiusMaxM);
    S::Vec3 start = job.plan_start, plan_land = job.plan_land, plan_top = job.plan_top;
    if (job.replan) {
        // Put on its bone at the swap: the worker's plan was for where it was. Its arc is flown
        // again from where it is, and every cast starts over from the first.
        const Arc arc = PlanArc(job.flight, job.ground);
        start = out.plan_start = arc.start;
        plan_land = out.plan_land = arc.land;
        plan_top = out.plan_top = arc.top;
        out.planned = true;
        out.stage = 0;
    }
    if (out.stage == 0) {
        const S::Vec3 shift{start.x - job.cut_at.x, start.y - job.cut_at.y,
                            start.z - job.cut_at.z};
        const float shift_len =
            std::sqrt(shift.x * shift.x + shift.y * shift.y + shift.z * shift.z);
        if (shift_len > 0.01f && std::isfinite(shift_len)) {
            VR::Body::Vec3 n{};
            float f = 0.0f;
            if (Cast(out, job.cut_at, start, radius, n, f) == MapSweep::Hit && f > 1e-3f) {
                const float keep = std::max(0.0f, f - radius / shift_len);
                out.moved = {job.cut_at.x + shift.x * keep - start.x,
                             job.cut_at.y + shift.y * keep - start.y,
                             job.cut_at.z + shift.z * keep - start.z};
            }
        }
        out.stage = 1;
        if (out.casts > 0) {
            return out;
        }
    }
    if (out.stage == 1) {
        const float dx = plan_land.x, dz = plan_land.z;
        if (dx * dx + dz * dz > 1e-4f) {
            VR::Body::Vec3 n{};
            float f = 0.0f;
            if (Cast(out, start, {start.x + dx, start.y, start.z + dz}, radius, n, f) ==
                    MapSweep::Hit &&
                f > 1e-3f && n.y < 0.7f) {
                // Its level speed cut to that share: it comes down at the wall's foot.
                out.keep = std::max(0.0f, f - radius / std::sqrt(dx * dx + dz * dz));
            }
        }
        out.stage = 2;
        if (out.casts > 0) {
            return out;
        }
    }
    if (out.stage == 2) {
        const float keep = job.keep * out.keep;
        const float top = start.y + std::max(0.0f, plan_top.y) + 0.3f;
        out.floor = FloorAt(out, job, start.x + plan_land.x * keep, top,
                            start.z + plan_land.z * keep, job.ground);
        out.floored = true;
        out.stage = 3;
    }
    return out;
}

// Come to rest: the floor right under where it lies. None there (an edge, a gap): it falls on.
FallAnswer CheckRest(const FallJob& job) {
    FallAnswer out{job.slot, job.born, true};
    const S::Vec3 at = MiddleNow(job.flight);
    out.floor = FloorAt(out, job, at.x, at.y + 0.3f, at.z, job.ground);
    out.refall = out.floor < job.ground - 0.05f;
    return out;
}

// A gush's size: the Augur's splatter (made for a fist) at the wound's size and the gush's force.
float GushScale(float wound_rms, const S::Feel::Gush& g) {
    const float size = std::clamp(wound_rms / 0.06f, 0.8f, 2.2f);
    return size * (0.45f + 0.35f * g.strength + 0.08f * g.speed_mps);
}

// One burst of the Augur's splatter (vr_inframe.vert AugurSprayVertex): at `at` (world, from the
// camera), out along `dir` (unit), `scale` its size and speed, `age` seconds into it.
void AddGush(F::List& list, const float* rows, S::Vec3 at, S::Vec3 dir, float scale, float age,
             u32 group) {
    S::Vec3 x{dir.z, 0.0f, -dir.x}; // across it, level
    float lx = std::sqrt(x.x * x.x + x.z * x.z);
    if (lx < 1e-4f) {
        x = {1.0f, 0.0f, 0.0f};
        lx = 1.0f;
    }
    x = {x.x / lx, 0.0f, x.z / lx};
    const S::Vec3 z{x.y * dir.z - x.z * dir.y, x.z * dir.x - x.x * dir.z,
                    x.x * dir.y - x.y * dir.x};
    const auto view = [rows](S::Vec3 v) {
        return S::Vec3{rows[0] * v.x + rows[1] * v.y + rows[2] * v.z,
                       rows[4] * v.x + rows[5] * v.y + rows[6] * v.z,
                       rows[8] * v.x + rows[9] * v.y + rows[10] * v.z};
    };
    const S::Vec3 vx = view(x), vy = view(dir), vz = view(z), p = view(at),
                  down = view({0.0f, -scale, 0.0f});
    F::Item item{};
    item.mesh = F::MeshId::AugurSpray;
    item.group = group; // never show blood for a replacement group that cannot be drawn
    item.shade = F::Shade::Unlit;
    item.mv = {vx.x * scale, vy.x * scale, vz.x * scale, p.x, vx.y * scale, vy.y * scale,
               vz.y * scale, p.y, vx.z * scale, vy.z * scale, vz.z * scale, p.z};
    item.uv = {-2000.0f, age, 0.0f, 0.0f}; // the blood's style (the Augur's), age
    item.emissive = {down.x, down.y, down.z, 0.0f};
    item.cull_sphere = {p.x, p.y, p.z, 2.0f * scale};
    list.Add(item);
}
} // namespace

// Cut corpses no longer in the world (the area left, a lamp's rest, a death): their entries go,
// pieces of theirs or none (a newer cut may have taken their slots), so nothing of a cut stays
// listed. Four times a second while any is hidden.
static void CheckHiddenCorpses() {
    static s64 next_ns = 0;
    const s64 now = NowNs();
    if (now < next_ns) {
        return;
    }
    next_ns = now + CorpseCheckNs;
    struct Seen {
        S::ActorKey key;
        const u8* addr;
        u32 handle;
        s32 model;
    };
    std::array<Seen, HiddenSlots> seen{};
    std::size_t n = 0;
    {
        std::scoped_lock lock{g_hide_mutex};
        for (const Hidden& h : g_hidden) {
            if (h.count > 0) {
                seen[n++] = {h.key, h.addr, h.handle, h.model};
            }
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!CorpseThere(seen[i].addr, seen[i].handle, seen[i].model)) {
            Unhide(seen[i].key);
            g_jobs.Forget(seen[i].key);
            RetirePieces(seen[i].key);
        }
    }
}

// At the swap (its part hidden on the corpse, before its first complete draw): the piece put on its
// bone as the corpse holds it now (start_follow, from the hide's hook), turned and moved; cleared of
// the hunter's head again from there; its fall planned afresh, its casts started over (the old plan
// was for where it was). True once done; false while its bone is not yet seen (tried again next
// update), or when it is not sound (then it starts where the kill had it: p.rebased, moved < 0).
// Under g_piece_mutex; no allocation.
bool Rebase(Piece& p, const float* position) {
    if (!p.start_follow_set) {
        return false;
    }
    p.rebased = true;
    std::array<float, 9> now{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            now[i * 3 + j] = p.start_follow[i * 4 + j];
        }
    }
    const S::Vec3 at = S::Apply(p.start_follow, p.middle_model);
    const float moved = std::sqrt((at.x - p.cut_at.x) * (at.x - p.cut_at.x) +
                                  (at.y - p.cut_at.y) * (at.y - p.cut_at.y) +
                                  (at.z - p.cut_at.z) * (at.z - p.cut_at.z));
    // (A bone moved or turned more since the kill than a body can: not trusted, the piece starts
    // where the kill had it, as before: Feel::RebaseCheck.)
    std::array<float, 9> delta{};
    std::array<float, 4> turn{};
    if (!(moved < S::Feel::RebaseMaxM) || !S::Feel::MulInverse(now, p.lin, delta) ||
        !S::Feel::RotationOf(delta, turn)) {
        p.rebase_refused = true;
        p.rebase_moved = moved;
        return true;
    }
    const float turn_deg = S::Feel::TurnDegrees(turn);
    const float since = static_cast<float>(NowNs() - p.times[0]) * 1e-9f;
    if (S::Feel::RebaseCheck(moved, turn_deg, since) != S::Feel::Rebase::Place) {
        p.rebase_refused = true;
        p.rebase_moved = moved;
        p.rebase_turn_deg = turn_deg;
        return true;
    }
    S::Vec3 start = at;
    float shift = 0.0f;
    S::Vec3 away{}; // level, from the hunter's head to it (unit; zero: not known)
    if (position != nullptr) {
        const S::Vec3 head{position[0], position[1], position[2]};
        const S::Vec3 level_to{at.x - head.x, 0.0f, at.z - head.z};
        const float level = std::sqrt(level_to.x * level_to.x + level_to.z * level_to.z);
        if (level > 1e-3f && std::isfinite(level)) {
            away = {level_to.x / level, 0.0f, level_to.z / level};
            std::array<S::Vec3, ClearPoints> turned{};
            for (std::size_t i = 0; i < p.clear_count; ++i) {
                turned[i] = S::Feel::Detail::Rotate(turn, p.clear[i]);
            }
            shift = S::Feel::ClearanceShift(std::span{turned.data(), p.clear_count}, at, head,
                                            away, HeadClearM, MaxClearShiftM);
            start = {at.x + away.x * shift, at.y, at.z + away.z * shift};
        }
    }
    // Its launch aimed again from here: off its cut's face as the bone turns it now, away from the
    // hunter's head now; the blade's own motion stays the kill's. And never slower than the body
    // carried it along its own way (a knocked, falling corpse: the piece does not stop as it comes
    // off), without adding to a launch already that fast (no second push).
    const S::Vec3 nm = p.normal_model;
    const S::Vec3 normal{now[0] * nm.x + now[1] * nm.y + now[2] * nm.z,
                         now[3] * nm.x + now[4] * nm.y + now[5] * nm.z,
                         now[6] * nm.x + now[7] * nm.y + now[8] * nm.z};
    const S::Feel::Launch launch = S::Feel::PieceLaunch(normal, p.launch_tip_vel, p.launch_limb,
                                                        p.charged, away, p.rest);
    const S::Feel::Carry carry = p.start_vel_set
                                     ? S::Feel::CarryAlong(launch.velocity, p.start_vel, CarryMaxMps)
                                     : S::Feel::Carry{launch.velocity, 0.0f};
    // (Its clocks are held until its first complete draw: it has not turned or moved yet.)
    p.flight.velocity = carry.velocity;
    p.flight.spin = launch.spin;
    p.rebase_carried = carry.carried;
    p.flight.turn = turn;
    p.flight.middle = start;
    p.flight.offset = {};
    p.cut_at = at;
    p.plan_stage = 0;
    p.plan_keep = 1.0f;
    p.replan = true;
    p.rechecked = false;
    p.settled.Invalidate();
    p.rebase_placed = true;
    p.rebase_moved = moved;
    p.rebase_turn_deg = turn_deg;
    p.rebase_shift = shift;
    return true;
}

constexpr float SplatFullM = 3.0f;  // a landing this near: its splat at full volume
constexpr float SplatHearM = 15.0f; // ... none past this

void SeverRenderTick(u64 record_id, const float* rows, const float* position) {
    if (position != nullptr) {
        for (int i = 0; i < 3; ++i) {
            g_view[static_cast<std::size_t>(i)].store(position[i], std::memory_order_relaxed);
        }
        g_view_set.store(true, std::memory_order_release);
    }
    if ((SeverAwake() & SeverAwakeHidden) != 0) {
        CheckHiddenCorpses();
    }
    if (g_piece_count.load(std::memory_order_acquire) == 0 || rows == nullptr ||
        position == nullptr) {
        return;
    }
    VR::CameraRecord rec{};
    if (!VR::GetRecord(record_id, rec)) {
        return;
    }
    const s64 now = NowNs();
    F::List list{};
    list.record = record_id;
    list.steady_ns = now;
    list.producer = F::Producer::Sever;
    list.exposure = 1.0f;
    std::vector<s32> gone;
    std::vector<std::pair<s32, float>> gone_corpses; // (model, seconds kept), logged
    std::vector<std::pair<s32, bool>> slumps;        // (model, landed): a piece slumping over
    struct FlightLog {
        s32 model;
        float seconds;
        std::uint32_t casts;
        float us, us_max, height, top; // top: its highest point above the floor it lies on
    };
    std::vector<FlightLog> flights; // a piece come to rest: its flight against the map
    // The cuts first drawn this update: each its tear, in its own hand (two at once: both felt;
    // a code review, 2026-11-17, one hand missed its tear); one splash for them all.
    struct Tear {
        u32 feel;     // its cut's id (its bite's: the tear continues it)
        bool charged; // a charged cut's
        S::Feel::StrikeHands hands;
    };
    std::array<Tear, MaxPieces> tears{};
    std::size_t tear_count = 0;
    // Cuts first drawn this update: their reveal slots go back (RevealDone) after the lock.
    std::array<u32, MaxPieces> revealed{};
    std::size_t revealed_count = 0;
    // ... and their timelines (from the blow to the cut seen), logged after the lock too.
    struct RevealLog {
        s32 model;
        std::array<s64, 5> times; // the kill, the worker took it, read, cut, built
        s64 hidden, seen;
        bool stump_only;
        bool placed, refused;  // put on its bone; its bone not trusted (neither: not seen)
        float moved, turn_deg; // what its bone did since the kill (placed or refused)
        float shift, carried;  // placed: clear of the head (m), the body's speed kept (m/s)
        bool own_bone;         // its own bone, else the body's at the cut
    };
    std::vector<RevealLog> reveal_logs;
    float landing_m = INFINITY; // the nearest landing this update (its splat)
    // Plans and rest checks: copied under the lock, cast outside it (a few casts a cut).
    std::vector<FallJob> jobs;
    {
        std::scoped_lock lock{g_piece_mutex};
        for (std::size_t i = 0; i < g_pieces.size(); ++i) {
            Piece& p = g_pieces[i];
            if (!p.on || p.still) {
                continue;
            }
            if (!p.gpu_seen) {
                continue; // no collision planning before the first complete draw
            }
            const bool plan = p.plan_stage < 3;
            const bool rest = !plan && p.flight.resting && !p.rechecked;
            if (plan || rest) {
                p.rechecked = p.rechecked || rest;
                jobs.push_back({i, p.born_ns, rest, p.flight, p.ground, p.kill_ground, p.rest,
                                p.cut_at, p.plan_stage, p.plan_start, p.plan_land, p.plan_top,
                                p.plan_keep, !rest && p.replan});
            }
        }
    }
    std::vector<FallAnswer> answers;
    answers.reserve(jobs.size());
    for (const FallJob& job : jobs) {
        answers.push_back(job.rest ? CheckRest(job) : PlanStep(job));
    }
    std::vector<S::ActorKey> unhide; // corpses gone: their hide entries go too
    {
        std::scoped_lock lock{g_piece_mutex};
        for (const FallAnswer& ans : answers) {
            Piece& p = g_pieces[ans.slot];
            if (!p.on || p.born_ns != ans.born) {
                continue; // replaced meanwhile
            }
            p.settled.Invalidate(); // floor/path correction can move a resting piece
            p.casts += ans.casts;
            p.cast_us += ans.us;
            p.cast_us_max = std::max(p.cast_us_max, ans.us_max);
            if (ans.rest) {
                if (ans.refall) {
                    p.ground = p.ground_target = ans.floor;
                    p.flight.resting = false; // (slumped stays: it falls as it lies)
                }
                continue;
            }
            if (ans.planned) {
                p.plan_start = ans.plan_start;
                p.plan_land = ans.plan_land;
                p.plan_top = ans.plan_top;
                p.replan = false;
            }
            p.flight.offset = {p.flight.offset.x + ans.moved.x, p.flight.offset.y + ans.moved.y,
                               p.flight.offset.z + ans.moved.z};
            p.plan_start = {p.plan_start.x + ans.moved.x, p.plan_start.y + ans.moved.y,
                            p.plan_start.z + ans.moved.z};
            p.flight.velocity.x *= ans.keep;
            p.flight.velocity.z *= ans.keep;
            p.plan_keep *= ans.keep;
            p.plan_stage = ans.stage;
            if (ans.floored) {
                p.ground_target = ans.floor;
                if (ans.floor < p.ground) {
                    p.ground = ans.floor; // a lower floor: at once
                }
            }
        }
        S::RevealFrame reveal_frame;
        S::RevealFrame drawn_frame;
        const auto reveal_order = S::RevealOrder(g_pieces);
        for (const auto slot : reveal_order) {
            Piece& p = g_pieces[slot];
            if (!p.on) {
                continue;
            }
            // Kept while its corpse is there (checked four times a second).
            if (now - p.checked_ns >= CorpseCheckNs) {
                p.checked_ns = now;
                if (!CorpseThere(p.addr, p.handle, p.model)) {
                    if (!p.still) {
                        gone_corpses.push_back(
                            {p.model, static_cast<float>(now - p.born_ns) * 1e-9f});
                    }
                    unhide.push_back(p.key); // (either one: a stump may never have been placed)
                    gone.insert(gone.end(), p.models.begin(), p.models.begin() + p.count);
                    p = {};
                    continue;
                }
            }
            if (!p.gpu_seen && !S::g_reveals.Live(p.reveal)) {
                // Cancelled/overwritten staging: retire only this cut's CPU models.
                // Do not unhide by actor key, which could now belong to a newer cut.
                gone.insert(gone.end(), p.models.begin(), p.models.begin() + p.count);
                p = {};
                continue;
            }
            if (!p.hide_seen) {
                p.hide_seen = reveal_frame.Hidden(p.reveal, HideApplied);
                if (!p.hide_seen) {
                    // No flight, particles or haptics while staging. These requests are not
                    // culled: both halves and both eyes must upload before anything is hidden.
                    S::StageHalf(list, std::span{p.models.data(), p.count}, p.reveal, p.has_cap);
                    continue;
                }
                p.visible_ns = p.last_ns = now; // same start for piece + stump; first dt is zero
                p.hidden_ns = now;
            }
            if (!p.gpu_seen) {
                // On its bone as the corpse holds it now, before it is drawn (tried each update
                // until its bone is seen; the hook's lock rarely misses one).
                if (!p.still && !p.rebased) {
                    Rebase(p, position);
                }
                // A crowded combined item list can postpone the first complete draw.
                // Keep BOTH clocks at zero rather than flying/bleeding invisibly.
                p.visible_ns = p.last_ns = now;
                p.gpu_seen = drawn_frame.Hidden(p.reveal, [](u32 token) {
                    return S::g_reveals.Visible(token);
                });
                if (p.gpu_seen && !p.still && !p.rebased) {
                    p.rebased = true; // never on its bone: it went where the kill had it
                }
                if (p.gpu_seen && revealed_count < revealed.size() &&
                    std::find(revealed.begin(), revealed.begin() + revealed_count, p.reveal) ==
                        revealed.begin() + revealed_count) {
                    revealed[revealed_count++] = p.reveal;
                }
                if (p.gpu_seen && !p.announced) {
                    // The cut's timeline (its half that plays its feel: the piece, else the
                    // stump), logged after the lock.
                    reveal_logs.push_back({p.model, p.times, p.hidden_ns, now, p.still,
                                           p.rebase_placed, p.rebase_refused, p.rebase_moved,
                                           p.rebase_turn_deg, p.rebase_shift, p.rebase_carried,
                                           p.start_own_bone});
                }
            }
            const float dt = std::clamp(static_cast<float>(now - p.last_ns) * 1e-9f, 0.0f, 0.1f);
            p.last_ns = now;
            if (!p.still) {
                const bool was_slumping = p.flight.slumping;
                // A higher floor where it comes down (a table, a step up): taken once its lowest
                // point is above it, never snapping it up where it was cut (the review).
                if (p.ground_target > p.ground && !p.flight.resting && !p.flight.slumping &&
                    LowestNow(p.flight) >= p.ground_target) {
                    p.ground = p.ground_target;
                }
                if (S::Feel::StepFlight(p.flight, dt, p.ground)) {
                    const S::Vec3 m = MiddleNow(p.flight);
                    const float dx = m.x - position[0], dy = m.y - position[1],
                                dz = m.z - position[2];
                    landing_m = std::min(landing_m, std::sqrt(dx * dx + dy * dy + dz * dz));
                }
                if (MiddleNow(p.flight).y < p.kill_ground - LostBelowM) {
                    gone.insert(gone.end(), p.models.begin(), p.models.begin() + p.count);
                    p = {};
                    continue;
                }
                if (p.flight.resting && !p.flight_logged) {
                    p.flight_logged = true;
                    const auto turned = S::Feel::TurnedPoints(p.flight);
                    float top = 0.0f;
                    for (std::size_t i = 0; i < p.flight.point_count; ++i) {
                        top = std::max(top, p.flight.middle.y + p.flight.offset.y + turned[i].y -
                                                p.ground);
                    }
                    flights.push_back({p.model, static_cast<float>(now - p.visible_ns) * 1e-9f,
                                       p.casts, p.cast_us, p.cast_us_max,
                                       p.flight.middle.y + p.flight.offset.y - p.kill_ground, top});
                }
                if (p.flight.slumping != was_slumping || (was_slumping && p.flight.slumped)) {
                    slumps.push_back({p.model, p.flight.slumped});
                }
            }
            if (p.gpu_seen && !p.announced) {
                p.announced = true;
                if (tear_count < tears.size()) {
                    tears[tear_count++] = {p.feel, p.charged, p.hands};
                }
            }
            // World <- mesh: the flight's turn after W's (column vectors), at its middle moved;
            // a stump riding its corpse: as its bone has moved.
            std::array<float, 9> a{};
            float at[3]{};
            if (p.following) {
                p.settled.Invalidate(); // corpse-attached stumps always follow their bone
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) {
                        a[i * 3 + j] = p.follow[i * 4 + j];
                    }
                }
                const S::Vec3 c = S::Apply(p.follow, p.middle_model);
                at[0] = c.x - position[0];
                at[1] = c.y - position[1];
                at[2] = c.z - position[2];
            } else {
                p.settled.Update(p.flight, p.lin);
                a = p.settled.linear;
                at[0] = p.settled.middle.x - position[0];
                at[1] = p.settled.middle.y - position[1];
                at[2] = p.settled.middle.z - position[2];
            }
            std::array<float, 12> mv{};
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    mv[i * 4 + j] = rows[i * 4 + 0] * a[0 * 3 + j] + rows[i * 4 + 1] * a[1 * 3 + j] +
                                    rows[i * 4 + 2] * a[2 * 3 + j];
                }
                mv[i * 4 + 3] = rows[i * 4 + 0] * at[0] + rows[i * 4 + 1] * at[1] +
                                rows[i * 4 + 2] * at[2];
            }
            // Far off: not drawn (its flight still stepped; the in-frame cache kept for near ones).
            if (p.gpu_seen && at[0] * at[0] + at[1] * at[1] + at[2] * at[2] > MaxDrawM * MaxDrawM) {
                continue;
            }
            // The blood: the stump's gushes (the cut's burst, then the heart's last beats, down to
            // a dribble), the piece's first few off its own cap; the Augur's splatter, aimed out of
            // the cut.
            const float since = static_cast<float>(now - p.visible_ns) * 1e-9f;
            if (p.has_cap && !S::Feel::GushesExpired(since, p.still, VR::Augur::SpraySeconds)) {
                const S::Vec3 off{p.cap_model.x - p.middle_model.x, p.cap_model.y - p.middle_model.y,
                                  p.cap_model.z - p.middle_model.z};
                const S::Vec3 n = p.normal_model;
                const float sign = p.still ? 1.0f : -1.0f;
                S::Vec3 cap{at[0] + a[0] * off.x + a[1] * off.y + a[2] * off.z,
                            at[1] + a[3] * off.x + a[4] * off.y + a[5] * off.z,
                            at[2] + a[6] * off.x + a[7] * off.y + a[8] * off.z};
                S::Vec3 d{sign * (a[0] * n.x + a[1] * n.y + a[2] * n.z),
                          sign * (a[3] * n.x + a[4] * n.y + a[5] * n.z),
                          sign * (a[6] * n.x + a[7] * n.y + a[8] * n.z)};
                const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                const auto gushes = S::Feel::StumpGushes(p.charged);
                for (std::size_t g = 0; g < gushes.size() && len > 1e-6f; ++g) {
                    if (!p.still && g >= S::Feel::PieceGushes) {
                        break; // the piece: its first few
                    }
                    const float age = since - gushes[g].delay_s;
                    if (age < 0.0f || age >= VR::Augur::SpraySeconds || g >= p.gush_at.size()) {
                        continue;
                    }
                    const auto bit = static_cast<std::uint8_t>(1u << g);
                    if ((p.gush_set & bit) == 0) {
                        p.gush_set |= bit;
                        p.gush_at[g] = {cap.x + position[0], cap.y + position[1],
                                        cap.z + position[2]};
                        p.gush_dir[g] = {d.x / len, d.y / len, d.z / len};
                    }
                    AddGush(list, rows,
                            {p.gush_at[g].x - position[0], p.gush_at[g].y - position[1],
                             p.gush_at[g].z - position[2]},
                            p.gush_dir[g], GushScale(p.wound, gushes[g]), age, p.reveal);
                }
            }
            // (Its models drawn all or none: never a piece part by part as they upload, nor cut
            // off by a full list.)
            const auto group = p.reveal;
            if (list.count + p.count > F::MaxItems) {
                continue;
            }
            for (std::uint8_t k = 0; k < p.count; ++k) {
                F::Item item{};
                item.mesh = F::MeshId::Model;
                item.tex = F::Tex::Model;
                item.group = group;
                item.reveal_first = !p.gpu_seen;
                item.tex_key = static_cast<std::uint32_t>(p.models[k]);
                item.shade = F::Shade::Model;
                item.mv = mv;
                if (p.gpu_seen) item.cull_sphere = {mv[3], mv[7], mv[11], p.reach * 1.2f};
                list.Add(item);
            }
        }
        CountPieces();
    }
    for (const s32 id : gone) {
        A::DropModel(id);
    }
    for (std::size_t i = 0; i < revealed_count; ++i) {
        RevealDone(revealed[i]); // (the hide lock: never under the pieces' lock)
    }
    for (const RevealLog& r : reveal_logs) {
        const auto& t = r.times;
        std::string where = "the stump only";
        const char* bone = r.own_bone ? "its bone" : "the body's bone at the cut";
        if (!r.stump_only && r.placed) {
            where = fmt::format("the piece on {}: moved {:.2f} m, turned {:.0f} deg, {:.2f} m "
                                "clear of the head, the body's speed kept {:.1f} m/s",
                                bone, r.moved, r.turn_deg, r.shift, r.carried);
        } else if (!r.stump_only && r.refused) {
            where = fmt::format("the piece where the kill had it ({} moved {:.2f} m and turned "
                                "{:.0f} deg: more than a body can, not trusted)",
                                bone, r.moved, r.turn_deg);
        } else if (!r.stump_only) {
            where = "the piece where the kill had it (its bone not seen)";
        }
        LOG_INFO(Core,
                 "VR sever: c{:04d}: the cut seen {:.0f} ms after the blow: the worker took it at "
                 "{:.1f} ms, read {:.1f} ms, posed and cut {:.1f} ms, built {:.1f} ms, uploaded "
                 "and hidden {:.1f} ms, first drawn {:.1f} ms; {}",
                 r.model, Ms(t[0], r.seen), Ms(t[0], t[1]), Ms(t[1], t[2]), Ms(t[2], t[3]),
                 Ms(t[3], t[4]), Ms(t[4], r.hidden), Ms(r.hidden, r.seen), where);
    }
    for (const S::ActorKey& key : unhide) {
        // The corpse is gone: its hooks need not look for it, and a new enemy given its slot (its
        // address and handle) can be cut.
        Unhide(key);
        g_jobs.Forget(key);
    }
    for (const FlightLog& f : flights) {
        LOG_INFO(Core, "VR sever: c{:04d}: the piece rests {:.1f} s after the cut, its middle "
                       "{:.2f} m from the kill's feet, {:.2f} m high at most; {} map casts, {:.0f} "
                       "us in all (at most {:.1f} us one)",
                 f.model, f.seconds, f.height, f.top, f.casts, f.us, f.us_max);
    }
    for (const auto& [model, landed] : slumps) {
        LOG_INFO(Core, "VR sever: c{:04d}: the piece {}", model,
                 landed ? "has slumped over and lies on its side" : "stands on a narrow base: it slumps over");
    }
    for (const auto& [model, kept] : gone_corpses) {
        LOG_INFO(Core, "VR sever: c{:04d}: its corpse is gone: its pieces go (kept {:.0f} s)",
                 model, kept);
    }
    // The cut's feel, as its piece is first drawn: the claw hand's bite, tear and the weight
    // leaving it (the presenter's MeleeEventKind::Sever), the cut's wet tear (the game's blood
    // splash, louder and deeper); each landing of a piece, a wet splat.
    if (tear_count > 0) {
        bool charged = false;
        for (std::size_t i = 0; i < tear_count; ++i) {
            const Tear& t = tears[i];
            VR::Combat::PushMeleeEvent({VR::Combat::MeleeEventKind::Sever, t.feel,
                                        t.charged ? 1.0f : 0.0f, static_cast<float>(t.hands), now});
            charged = charged || t.charged;
        }
        VR::ItemAudio::Play(VR::ItemAudio::Splash, now, charged ? 2.0f : 1.7f,
                            charged ? 0.68f : 0.75f);
    } else if (landing_m < SplatHearM) {
        // Full within SplatFullM, fading out by SplatHearM (the sound is not placed in the world).
        const float vol =
            std::clamp(1.0f - (landing_m - SplatFullM) / (SplatHearM - SplatFullM), 0.0f, 1.0f);
        if (vol > 0.05f) {
            VR::ItemAudio::Play(VR::ItemAudio::Splash, now, vol, 1.12f);
        }
    }
    if (list.count > 0) {
        F::Submit(list);
    }
}

bool SeverKnown(const u8* target) {
    if (target == nullptr) {
        return false;
    }
    const S::ActorKey key = KeyOf(target);
    if (!g_jobs.Known(key)) {
        return false;
    }
    if (g_jobs.Busy(key)) {
        return true; // its cut in flight (a second lethal hit in the same frame)
    }
    {
        std::scoped_lock lock{g_hide_mutex};
        for (const Hidden& h : g_hidden) {
            if (h.count > 0 && h.key == key) {
                return true;
            }
        }
    }
    // A past life's (its cut failed, or its corpse is gone): this one, alive until this blow, can
    // be cut (the review, 2026-11-13: a respawned enemy could never be cut again).
    g_jobs.Forget(key);
    return false;
}

bool SeverCutsCharacter(const u8* target) {
    if (!Plausible(target)) {
        return false;
    }
    // c0000 (the hunters' human body: an NPC hunter, a phantom): its look is its armour's parts,
    // not in its chrbnd, and its class may not be the hooked one: never cut (no bare twin).
    return Read<s32>(target + OffsetChrId) != 0 && SeverHookedCharacter(target);
}

void SeverPrefetch(const u8* target) {
    if (!SeverEnabled() || !Plausible(target)) {
        return;
    }
    const s32 chr = Read<s32>(target + OffsetChrId);
    if (chr <= 0) {
        return;
    }
    // Asked for lately, or read already: nothing more (every hit after the first on its kind).
    if (g_prefetch_asked.Has(chr)) {
        return;
    }
    for (const auto& cached : g_cached_chr) {
        if (cached.load(std::memory_order_relaxed) == chr) {
            return;
        }
    }
    if (!SeverHookedCharacter(target)) {
        return; // (never cut: nothing to read for it)
    }
    {
        std::scoped_lock lock{g_queue_mutex};
        if (!EnsureWorkerLocked() || !g_prefetch.Add(chr)) {
            return; // (waiting already, or MaxPrefetch are: its kill reads its own)
        }
        g_prefetch_asked.Note(chr);
    }
    g_wake.notify_all();
}

float SeverRoll() {
    if (TestOn()) {
        return 0.0f;
    }
    // splitmix64 (any thread; the damage detour's alone in practice).
    static std::atomic<std::uint64_t> state{static_cast<std::uint64_t>(NowNs()) | 1};
    constexpr std::uint64_t Golden = 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state.fetch_add(Golden, std::memory_order_relaxed) + Golden;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<float>(z >> 40) * (1.0f / 16777216.0f);
}

bool SeverKill(const u8* target, const S::Site& site, const std::array<float, 3>& contact,
               const MeleeHitInfo& info, bool heavy) {
    const s64 t0 = NowNs();
    if (!SeverCutsCharacter(target)) {
        LOG_INFO(Core, "VR sever: c{:04d}: not one of the hooked enemy classes (or a human "
                       "body): not cut",
                 Plausible(target) ? Read<s32>(target + OffsetChrId) : -1);
        return false;
    }
    // A site its model can never be cut at (an earlier cut found out): a plain kill, no bite. (A
    // too-heavy torso blow takes another site on the worker: not known here.)
    if (const s32 chr = Read<s32>(target + OffsetChrId); !heavy && Uncuttable(chr, site)) {
        LOG_INFO(Core, "VR sever: c{:04d}: its {} cannot be cut (its model has no such bones): a "
                       "plain kill",
                 chr, S::LimbName(site.limb));
        return false;
    } else if (!heavy && FailedHit(chr, site)) {
        LOG_INFO(Core, "VR sever: c{:04d}: its {} cannot be cut there (an earlier kill there found "
                       "no cut): a plain kill",
                 chr, S::CutName(site));
        return false;
    }
    Capture c;
    c.heavy = heavy;
    c.charged = info.charged;
    using Hands = S::Feel::StrikeHands;
    const Hands hands = info.left         ? Hands::LeftClaw
                        : info.two_handed ? Hands::TwoHanded
                                          : Hands::Weapon;
    c.hands = hands;
    c.key = KeyOf(target);
    c.addr = target;
    c.handle = Read<u32>(target + OffsetHandle);
    c.npc = Read<s32>(target + OffsetNpcParam);
    c.chr = Read<s32>(target + OffsetChrId);
    c.site = site;
    c.contact = {contact[0], contact[1], contact[2]};
    c.blade = info.blade;
    c.base = {info.base.x, info.base.y, info.base.z};
    c.tip = {info.tip.x, info.tip.y, info.tip.z};
    c.tip_vel = {info.tip_vel.x, info.tip_vel.y, info.tip_vel.z};
    c.ns = t0;
    // Room for it, before its pose is copied (a code review, 2026-11-17: a busy multi-kill copied
    // a skeleton to throw it away); checked again as it goes in.
    const auto full = [&c] {
        if (g_queue.size() < MaxQueued) {
            return false;
        }
        LOG_INFO(Core, "VR sever: c{:04d}: {} kills already waiting: not cut", c.chr, MaxQueued);
        return true;
    };
    {
        std::scoped_lock lock{g_queue_mutex};
        if (full()) {
            return false;
        }
    }
    if (const char* why = ReadPose(target, c)) {
        LOG_WARNING(Core, "VR sever: c{:04d}: its pose could not be read ({}): not cut", c.chr, why);
        return false;
    }
    const std::size_t bones = c.local.size();
    // The cut's id for its feel: its bite now, its tear as it comes off (never 0).
    static std::atomic<u32> next_feel{0};
    u32 feel = next_feel.fetch_add(1, std::memory_order_relaxed) + 1;
    if (feel == 0) {
        feel = next_feel.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    c.feel = feel;
    {
        std::scoped_lock lock{g_queue_mutex};
        if (full() || !EnsureWorkerLocked()) {
            return false;
        }
        g_queue.push_back(std::move(c));
    }
    // The cut's hit-stop: the claw stays in the flesh a beat longer than a hit's (the right
    // claw's: the left hand has no bite hold).
    if (!info.left) {
        MeleeBiteAtLeast(S::Feel::Hitstop(info.charged));
    }
    // ... and its bite in the hand, with the blow (2026-11-16: felt as the limb came off,
    // about 0.1 s after it, it was a second, late hit). Its hit's hands: the left claw's, both for
    // a two-handed swing (the scythe's).
    VR::Combat::PushMeleeEvent({VR::Combat::MeleeEventKind::SeverBite, feel,
                                info.charged ? 1.0f : 0.0f, static_cast<float>(hands), t0});
    g_wake.notify_all();
    LOG_INFO(Core, "VR sever: c{:04d} the {}: its pose copied ({} bones, {:.0f} us), queued",
             Read<s32>(target + OffsetChrId), S::LimbName(site.limb), bones,
             Ms(t0, NowNs()) * 1000.0);
    return true;
}

} // namespace VR::Bloodborne
