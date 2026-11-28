// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/logging/log.h"
#include "core/vr/anim_classes.h"
#include "core/vr/bloodborne_heal.h"
#include "core/vr/bloodborne_layout.h"
#include "core/vr/bloodborne_pose.h"
#include "core/vr/hook_util.h"
#include "core/vr/live_tune.h"
#include "core/vr/pose_channel.h"
#include "core/vr/pose_gestures.h"
#include "core/vr/vr_session.h"

namespace VR::Bloodborne {

namespace {

namespace L = VR::Bloodborne::Layout;
namespace P = VR::PoseGesture;
using Hook::Plausible;
using Hook::Read;

// Module-relative addresses in the 1.09 eboot (patch file address - 0x400000).
// ChrIns::AddSpEffect (vt+0x3F0 of ChrIns, EnemyIns and both PlayerIns vtables): (chr, id, source
// chr, u8, u8 local_only, s32, 5 floats, s32 on the stack) -> u8 added. The HKS act that adds a
// SpEffect (0x1A248D0) calls it with the character as its own source, local_only = 1, zeros and
// 1.0f: with local_only the main player's add never goes through the network path (0x18C7080) or
// the remote request (vt+0x400). The add itself (0x18C69D0) is what UseGoods' item effect uses.
constexpr u64 AddSpEffectRva = 0x18C6DB0;
constexpr std::array<u8, 32> AddSpEffectSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC,
    0x28, 0x45, 0x89, 0xCC, 0x44, 0x89, 0xC3, 0xC5, 0xFA, 0x11, 0x65, 0xD4, 0xC5, 0xFA, 0x11, 0x5D,
};
// SprjEventFlagMan::GetEventFlag(this, flag) -> 0/1: a leaf (no call, no store) that finds the
// flag's block in the manager's tree ([+0x38], block size [+0x1C]) and tests its bit. The game
// mostly inlines it; this copy is the one its function table points at.
constexpr u64 GetEventFlagRva = 0x13CFB60;
constexpr std::array<u8, 24> GetEventFlagSig = {
    0x44, 0x8B, 0x4F, 0x1C, 0x31, 0xD2, 0x89, 0xF0, 0x41, 0xF7, 0xF1, 0x89,
    0xC1, 0x41, 0x89, 0xC8, 0x45, 0x0F, 0xAF, 0xC1, 0x4C, 0x8B, 0x57, 0x38,
};
// The action-button check's read of SprjEventFlagMan (its invalidFlag): lea rax, [rip+disp];
// mov r8, [rax]; test r8, r8; jnz: proves the global.
constexpr u64 FlagManLeaRva = 0x1CD93AE;
constexpr std::array<u8, 15> FlagManLeaSig = {
    0x48, 0x8D, 0x05, 0x4B, 0x1D, 0x86, 0x03, 0x4C, 0x8B, 0x00, 0x4D, 0x85, 0xC0, 0x75, 0x31,
};
constexpr size_t FlagManLeaDisp = 3;
constexpr size_t FlagManLeaEnd = 7;
constexpr u64 EventFlagManRva = 0x553B100;
constexpr u32 OffsetFlagBlockSize = 0x1C; // the divisor GetEventFlag divides by (0: not set up)
constexpr u32 OffsetFlagTree = 0x38;
// Both PlayerIns vtables (bloodborne_heal.cpp) hold AddSpEffect at +0x3F0.
constexpr u64 PlayerVtableRva = 0x538F310;
constexpr u64 PlayerVtableRva2 = 0x538FA80;
constexpr u32 OffsetVtableAddSpEffect = 0x3F0;
// GestureGameData ([PlayerGameData + 0x5B8], vtable 0x53259C0): 30 u32 at +0x10 (pose_gestures.h
// LearnedMask).
constexpr u64 GestureDataVtableRva = 0x53259C0;
constexpr u32 OffsetPlayerGameData = 0x3C0;
constexpr u32 OffsetGestureData = 0x5B8;
constexpr u32 OffsetGestureEntries = 0x10;
constexpr int GestureEntries = 30;
constexpr u32 OffsetChrType = 0x78; // s32: 0 = the host (a phantom is not)
constexpr u32 OffsetHp = 0xF8;      // ChrDataModule
constexpr u32 OffsetCtrlWorld = 0x90; // ChrCtrl: the world matrix, three float4 rows {R, t}
// SpEffect list (bloodborne_heal.cpp): first node +0x08, flags +0x1C, id +0x40, next +0x58.
constexpr u32 OffsetSpEffects = 0x1C8;
constexpr u32 OffsetEffectHead = 0x08;
constexpr u32 OffsetNodeFlags = 0x1C;
constexpr u32 OffsetNodeId = 0x40;
constexpr u32 OffsetNodeNext = 0x58;
constexpr u32 NodeSkipMask = 0x800C0003;

using AddSpEffectFn = u8(PS4_SYSV_ABI*)(void* chr, s32 id, void* source, u32 a3, u32 local_only,
                                       s32 a5, float f0, float f1, float f2, float f3, float f4,
                                       s32 stack0);
using GetEventFlagFn = u32(PS4_SYSV_ABI*)(void* manager, u32 flag);

u8* g_base = nullptr;
bool g_ready = false;
bool g_disabled = false;
bool g_test_brain = false; // VR_POSE_TEST=brain: the Brain's flag and region forced (tests only)
AddSpEffectFn g_add = nullptr;
GetEventFlagFn g_get_flag = nullptr;

struct SiteState {
    P::Zone zone{P::Zone::None}; // the zone shown (it ends 1 s after the game's conditions)
    s64 none_since_ns{0};        // the conditions have been off since (0: they hold)
    s64 next_poll_ns{0};
    s64 next_learned_ns{0};
    u32 learned{0};
    bool learned_ok{false};
    const char* block{nullptr};
    bool block_logged{false};
    P::Trigger trigger{};
    u32 fired{0};
    s32 fired_marker{-1};
    u32 wait_serial{0};
    const char* wait_why{nullptr};
    // The newest marker's watch (logs when it is first active on the hunter and when it leaves:
    // the row's own 0.1 s).
    s32 watch_id{-1};
    s64 watch_from_ns{0};
    s64 watch_seen_ns{0}; // first seen active (0: not yet)
};
SiteState g_site{};

s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

u8* Module(const u8* chr, u32 offset) {
    u8* const bag = Read<u8*>(chr + L::OffsetChrModules);
    if (!Plausible(bag)) {
        return nullptr;
    }
    u8* const m = Read<u8*>(bag + offset);
    return Plausible(m) ? m : nullptr;
}

P::V3 Position(const u8* chr) {
    u8* const ctrl = Read<u8*>(chr + L::OffsetChrCtrl);
    if (!Plausible(ctrl)) {
        return {1e9f, 1e9f, 1e9f};
    }
    const u8* const w = ctrl + OffsetCtrlWorld;
    return {Read<float>(w + 0x0C), Read<float>(w + 0x1C), Read<float>(w + 0x2C)};
}

// The game's flag (false when the manager is not set up yet).
bool Flag(u32 id) {
    u8* const manager = Read<u8*>(g_base + EventFlagManRva);
    if (!Plausible(manager) || Read<u32>(manager + OffsetFlagBlockSize) == 0 ||
        !Plausible(Read<u8*>(manager + OffsetFlagTree))) {
        return false;
    }
    return g_get_flag(manager, id) != 0;
}

// VR_SET_FLAGS="9802=1,9800=0" (tests only): the game's flags forced to these values, written the
// way GetEventFlag reads them (the block of flag / size in the tree at +0x38; its bits at [node +
// 0x30] (kind 2) or manager +0x28 + [+0x20] * [node + 0x30] (kind 1), most significant bit first).
struct ForcedFlag {
    u32 id;
    bool on;
    u32 writes;
};
std::vector<ForcedFlag> g_forced;
bool g_forced_read = false;
// VR_SET_FLAGS_DIE=1 (tests only): the hunter dies 3 s after the flags first hold, so the map
// reloads with them (the time of day is read at a load). =<seconds> (more than 3): that long after
// they hold, after a warp as well: a warp to a lamp, then a death there (2026-11-21: a death
// reloads none of its own area's files, so the Fog row's switch waits for another area).
bool g_forced_die = false;
s64 g_forced_die_ns = 3'000'000'000;
bool g_die_after_warp = false;
// VR_SET_FLAGS_WARP=<return point> (tests only): instead of a death, the lamp travel's own two calls
// (the respawn setter, then warp effect 2101) once the flags hold 3 s. A list ("<a>,<b>,...") is a
// tour: each next warp VR_SET_FLAGS_WARP_EVERY seconds (default 45) after the hunter is in the
// world again, round and round (a long session through many areas: the memory stress test).
s32 g_forced_warp = -1;
s64 g_forced_held_ns = 0;
std::vector<s32> g_warp_tour;
size_t g_warp_next = 0;
s64 g_warp_every_ns = 45'000'000'000;
s64 g_warp_first_ns = 3'000'000'000;

u8* FlagByte(u8* manager, u32 id, u32& bit) {
    const u32 size = Read<u32>(manager + OffsetFlagBlockSize);
    const u32 block = id / size;
    u8* const root = Read<u8*>(manager + OffsetFlagTree);
    u8* best = root;
    // At most 64 steps down the tree (a damaged one never loops).
    int steps = 0;
    for (u8* n = Read<u8*>(root + 8); Plausible(n) && Read<u8>(n + 0x19) == 0 && steps++ < 64;) {
        if (Read<u32>(n + 0x20) < block) {
            n = Read<u8*>(n + 0x10);
        } else {
            best = n;
            n = Read<u8*>(n);
        }
    }
    if (best == root || Read<u32>(best + 0x20) != block) {
        return nullptr;
    }
    const u32 kind = Read<u32>(best + 0x28);
    u8* data = nullptr;
    if (kind == 2) {
        data = Read<u8*>(best + 0x30);
    } else if (kind == 1) {
        data = Read<u8*>(manager + 0x28) + Read<u32>(manager + 0x20) * Read<u32>(best + 0x30);
    }
    const u32 off = id - block * size;
    if (!Plausible(data, 1) || !Plausible(data + (off >> 3), 1)) {
        return nullptr;
    }
    bit = 7 - (off & 7);
    return data + (off >> 3);
}

void ForceFlags(u8* player) {
    if (!g_forced_read) {
        g_forced_read = true;
        const char* spec = std::getenv("VR_SET_FLAGS");
        // "<id>=<0|1>" pairs, comma-separated; anything else ends the list (a malformed "9802="
        // read past the string's end before: its value, then two characters on).
        for (const char* p = spec; p != nullptr && *p != 0;) {
            char* end = nullptr;
            const unsigned long id = std::strtoul(p, &end, 10);
            if (end == p || end[0] != '=' || (end[1] != '0' && end[1] != '1') ||
                (end[2] != ',' && end[2] != '\0')) {
                break;
            }
            g_forced.push_back({static_cast<u32>(id), end[1] == '1', 0});
            p = end[2] == ',' ? end + 3 : end + 2;
        }
        g_forced_die = std::getenv("VR_SET_FLAGS_DIE") != nullptr;
        if (const char* die = std::getenv("VR_SET_FLAGS_DIE"); die != nullptr && std::atoll(die) > 3) {
            g_forced_die_ns = std::atoll(die) * 1'000'000'000LL;
            g_die_after_warp = true;
        }
        if (const char* warp = std::getenv("VR_SET_FLAGS_WARP"); warp != nullptr) {
            for (const char* p = warp; *p != 0;) {
                char* end = nullptr;
                const long point = std::strtol(p, &end, 10);
                if (end == p) {
                    break;
                }
                g_warp_tour.push_back(static_cast<s32>(point));
                p = *end == ',' ? end + 1 : end;
            }
            g_forced_warp = g_warp_tour.empty() ? -1 : g_warp_tour.front();
            if (const char* every = std::getenv("VR_SET_FLAGS_WARP_EVERY"); every && *every) {
                g_warp_every_ns = std::max(10LL, std::atoll(every)) * 1'000'000'000LL;
            }
            // VR_SET_FLAGS_WARP_FIRST=<s>: the first warp's wait (default 3 s).
            if (const char* first = std::getenv("VR_SET_FLAGS_WARP_FIRST"); first && *first) {
                g_warp_first_ns = std::max(3LL, std::atoll(first)) * 1'000'000'000LL;
            }
        }
        if (!g_forced.empty()) {
            LOG_INFO(Core, "VR flags: TEST forcing {} flags{}", g_forced.size(),
                     g_forced_die ? ", then a death" : "");
        }
    }
    // (A warp alone, no flags: 2026-11-12, a save moved to the Grand Cathedral as it is, its time
    // of day untouched.)
    if (g_forced.empty() && g_forced_warp < 0) {
        return;
    }
    u8* const manager = Read<u8*>(g_base + EventFlagManRva);
    if (!Plausible(manager) || Read<u32>(manager + OffsetFlagBlockSize) == 0 ||
        !Plausible(Read<u8*>(manager + OffsetFlagTree))) {
        return;
    }
    for (ForcedFlag& f : g_forced) {
        if ((g_get_flag(manager, f.id) != 0) == f.on || f.writes >= 20) {
            continue;
        }
        u32 bit = 0;
        u8* const byte = FlagByte(manager, f.id, bit);
        if (byte == nullptr) {
            if (f.writes == 0) {
                f.writes = 20;
                LOG_WARNING(Core, "VR flags: TEST {}: its block was not found", f.id);
            }
            continue;
        }
        *byte = static_cast<u8>(f.on ? *byte | (1u << bit) : *byte & ~(1u << bit)); // test only
        ++f.writes;
        LOG_INFO(Core, "VR flags: TEST {} set to {} (now reads {})", f.id, f.on ? 1 : 0,
                 g_get_flag(manager, f.id));
    }
    if (!g_forced_die && g_forced_warp < 0) {
        return;
    }
    const bool hold = std::ranges::all_of(
        g_forced, [&](const ForcedFlag& f) { return (g_get_flag(manager, f.id) != 0) == f.on; });
    const s64 now = NowNs();
    if (!hold) {
        g_forced_held_ns = 0;
        return;
    }
    if (g_forced_held_ns == 0) {
        g_forced_held_ns = now;
    }
    const s64 wait_ns = g_warp_next == 0 ? g_warp_first_ns : g_warp_every_ns;
    if (g_forced_warp >= 0 && now - g_forced_held_ns > wait_ns) {
        using SetRespawnFn = void(PS4_SYSV_ABI*)(s32);
        reinterpret_cast<SetRespawnFn>(g_base + 0x156D110)(g_forced_warp);
        const u8 added = g_add(player, 2101, player, 0, 1, 0, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0);
        LOG_INFO(Core, "VR flags: TEST warp to return point {} (effect 2101 added {}; tour stop {})",
                 g_forced_warp, added, g_warp_next + 1);
        // A tour goes on from the next map: the wait restarts when the hunter is in it.
        ++g_warp_next;
        g_forced_warp = g_warp_tour.size() > 1 ? g_warp_tour[g_warp_next % g_warp_tour.size()] : -1;
        g_forced_held_ns = 0;
        g_forced_die = g_forced_die && g_die_after_warp;
        return;
    }
    u8* const data = Module(player, L::OffsetDataModule);
    if (g_forced_die && now - g_forced_held_ns > g_forced_die_ns && data != nullptr &&
        Read<s32>(data + OffsetHp) > 0) {
        const s32 zero = 0;
        std::memcpy(data + OffsetHp, &zero, sizeof(zero)); // test only
        g_forced_die = false;
        LOG_INFO(Core, "VR flags: TEST the hunter dies so the map reloads with the flags");
    }
}

bool HasEffect(const u8* player, s32 id) {
    u8* const list = Read<u8*>(player + OffsetSpEffects);
    if (!Plausible(list)) {
        return false;
    }
    u8* node = Read<u8*>(list + OffsetEffectHead);
    for (u32 i = 0; i < 512 && Plausible(node); ++i) {
        if ((Read<u32>(node + OffsetNodeFlags) & NodeSkipMask) == 0 &&
            Read<s32>(node + OffsetNodeId) == id) {
            return true;
        }
        node = Read<u8*>(node + OffsetNodeNext);
    }
    return false;
}

// The learned gestures (G(idx) bits); false when the list is not where it should be.
bool ReadLearned(const u8* player, u32& mask) {
    u8* const pgd = Read<u8*>(player + OffsetPlayerGameData);
    u8* const ggd = Plausible(pgd) ? Read<u8*>(pgd + OffsetGestureData) : nullptr;
    if (!Plausible(ggd) || Read<u8*>(ggd) != g_base + GestureDataVtableRva) {
        return false;
    }
    std::array<u32, GestureEntries> entries{};
    std::memcpy(entries.data(), ggd + OffsetGestureEntries, sizeof(entries));
    mask = P::LearnedMask(entries.data(), GestureEntries);
    return true;
}

std::string LearnedNames(u32 mask) {
    std::string out;
    for (int i = 1; i < P::KindCount; ++i) {
        const auto k = static_cast<P::Kind>(i);
        if ((P::GesturesOf(k) & mask) != 0) {
            out += fmt::format("{}{}", out.empty() ? "" : ", ", P::KindName(k));
        }
    }
    return out.empty() ? std::string{"none"} : out;
}

// The newest animation is idle or locomotion (not attacking, rolling, an item, an event animation
// such as a ladder, a hit, a gesture of the pad's own).
bool AnimFree(const u8* player, s32& newest) {
    newest = -1;
    u8* const ta = Module(player, L::OffsetTimeActModule);
    if (ta == nullptr || Read<u8*>(ta) != g_base + L::TimeActVtableRva) {
        return false;
    }
    const s32 write = Read<s32>(ta + L::OffsetTimeActWrite);
    if (write < 0 || write >= L::TimeActRingSize) {
        return false;
    }
    const s32 index = (write + L::TimeActRingSize - 1) % L::TimeActRingSize;
    newest = Read<s32>(ta + L::OffsetTimeActRing + index * L::TimeActEntrySize);
    const auto c = VR::Body::ClassifyAnim(newest);
    return c == VR::Body::AnimClass::Idle || c == VR::Body::AnimClass::Locomotion;
}

// On a ladder: PlayerIns -> ChrCtrl -> HKS action state -> s8 (-1 = off).
bool OnLadder(const u8* player) {
    u8* const ctrl = Read<u8*>(player + L::OffsetChrCtrl);
    u8* const state = Plausible(ctrl) ? Read<u8*>(ctrl + L::OffsetActionState) : nullptr;
    if (!Plausible(state) || Read<u8*>(state) != g_base + L::ActionStateVtableRva) {
        return false;
    }
    return Read<s8>(state + L::OffsetLadderState) != -1;
}

// Applies one marker with the game's own add (the vtable slot is checked first: a different
// character class never gets a call).
bool ApplyMarker(u8* player, s32 id) {
    u8* const vtable = Read<u8*>(player);
    u8* const slot = Plausible(vtable) ? Read<u8*>(vtable + OffsetVtableAddSpEffect) : nullptr;
    if ((vtable != g_base + PlayerVtableRva && vtable != g_base + PlayerVtableRva2) ||
        slot != g_base + AddSpEffectRva) {
        g_disabled = true;
        LOG_WARNING(Core,
                    "VR pose: DISABLED: the player's vtable {} (+0x3F0 = {}) is not the expected "
                    "one; no marker is applied",
                    fmt::ptr(vtable), fmt::ptr(slot));
        return false;
    }
    const u8 added = g_add(player, id, player, 0, 1, 0, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0);
    return added != 0;
}

void Publish(const SiteState& s, s64 now) {
    P::ZoneState z{};
    z.zone = static_cast<u8>(s.zone);
    z.blocked = s.block != nullptr;
    const bool all = VR::Tune::Get("pose.all", P::GetPoseSettings().all ? 1.0f : 0.0f) != 0.0f;
    z.available = s.zone == P::Zone::None ? 0 : P::AvailableKinds(s.learned, all);
    z.fired = s.fired;
    z.fired_marker = s.fired_marker;
    z.ns = now;
    P::ZoneChannel().Store(z);
}

void WatchMarker(const u8* player, s64 now) {
    auto& s = g_site;
    if (s.watch_id < 0) {
        return;
    }
    const double ms = static_cast<double>(now - s.watch_from_ns) * 1e-6;
    const bool on = HasEffect(player, s.watch_id);
    if (on && s.watch_seen_ns == 0) {
        s.watch_seen_ns = now;
        LOG_INFO(Core, "VR pose: marker {} active on the hunter {:.0f} ms after the add", s.watch_id,
                 ms);
    } else if (!on) {
        LOG_INFO(Core, "VR pose: marker {} {} {:.0f} ms after the add", s.watch_id,
                 s.watch_seen_ns != 0 ? "left the hunter" : "NEVER active on the hunter; gone",
                 ms);
        s.watch_id = -1;
    } else if (ms > 3000.0) {
        LOG_INFO(Core, "VR pose: marker {} still on the hunter after {:.0f} ms", s.watch_id, ms);
        s.watch_id = -1;
    }
}

} // Anonymous namespace

void InstallPose(VAddr base, u64 size) {
    const auto& st = P::GetPoseSettings();
    if (!st.enabled) {
        LOG_INFO(Core, "VR pose: pose_gestures=0, physical gestures off");
        return;
    }
    auto* const module = reinterpret_cast<u8*>(base);
    if (size < EventFlagManRva + 8) {
        LOG_INFO(Core, "VR pose: eboot too small ({:#x}) for Bloodborne 1.09; not installed", size);
        return;
    }
    const auto matches = [module](u64 rva, const auto& signature) {
        return std::memcmp(module + rva, signature.data(), signature.size()) == 0;
    };
    const bool add_ok = matches(AddSpEffectRva, AddSpEffectSig);
    const bool flag_ok = matches(GetEventFlagRva, GetEventFlagSig);
    bool man_ok = matches(FlagManLeaRva, FlagManLeaSig);
    if (man_ok) {
        s32 displacement{};
        std::memcpy(&displacement, module + FlagManLeaRva + FlagManLeaDisp, sizeof(displacement));
        man_ok = FlagManLeaRva + FlagManLeaEnd + static_cast<s64>(displacement) == EventFlagManRva;
    }
    const bool hook_ok = IsPlayerHookInstalled();
    const auto verdict = [](bool ok) { return ok ? "ok" : "MISMATCH"; };
    if (!add_ok || !flag_ok || !man_ok || !hook_ok) {
        LOG_WARNING(Core,
                    "VR pose: AddSpEffect {} GetEventFlag {} EventFlagMan {} site A {}: physical "
                    "gestures NOT installed",
                    verdict(add_ok), verdict(flag_ok), verdict(man_ok),
                    hook_ok ? "ok" : "not installed");
        if (!add_ok) {
            LOG_WARNING(Core, "VR pose: AddSpEffect live bytes: {}",
                        Hook::HexBytes(module + AddSpEffectRva, AddSpEffectSig.size()));
        }
        if (!flag_ok) {
            LOG_WARNING(Core, "VR pose: GetEventFlag live bytes: {}",
                        Hook::HexBytes(module + GetEventFlagRva, GetEventFlagSig.size()));
        }
        return;
    }
    g_base = module;
    g_add = reinterpret_cast<AddSpEffectFn>(module + AddSpEffectRva);
    g_get_flag = reinterpret_cast<GetEventFlagFn>(module + GetEventFlagRva);
    if (const char* test = std::getenv("VR_POSE_TEST"); test != nullptr) {
        g_test_brain = std::string{test} == "brain";
    }
    g_ready = true;
    LOG_INFO(Core,
             "VR pose: installed (AddSpEffect, GetEventFlag, EventFlagMan ok): the Brain of "
             "Mensis and the Doll; pose_gestures_all={} pose_contact_hold_s={:.1f}{}",
             st.all ? 1 : 0, st.contact_hold_s,
             g_test_brain ? " | TEST VR_POSE_TEST=brain: the Brain's flag and region forced" : "");
}

void PoseSiteA(u8* player) {
    if (!g_ready || g_disabled || player == nullptr) {
        return;
    }
    ForceFlags(player);
    auto& s = g_site;
    const s64 now = NowNs();
    WatchMarker(player, now);
    // Outside a zone: one look a second (two flags and the position).
    if (s.zone == P::Zone::None && now < s.next_poll_ns) {
        return;
    }
    s.next_poll_ns = now + 1'000'000'000;
    const P::V3 pos = Position(player);
    const bool doll = Flag(P::FlagDollObserve);
    const bool brain = g_test_brain || Flag(P::FlagBrainFallen);
    const bool near_brain = g_test_brain || P::NearBrain(pos);
    // The zone as the game's conditions say now (`live`: what a marker needs) and as shown (`zone`:
    // it ends after 1 s without them, so the Doll's flag flickering at the edge of her view keeps a
    // held pose).
    const P::Zone live = doll ? P::Zone::Doll
                              : (brain && near_brain ? P::Zone::Brain : P::Zone::None);
    if (live == P::Zone::None && s.zone != P::Zone::None) {
        if (s.none_since_ns == 0) {
            s.none_since_ns = now;
        }
        if (now - s.none_since_ns >= 1'000'000'000) {
            LOG_INFO(Core, "VR pose: zone {} off (at ({:.2f}, {:.2f}, {:.2f}))", P::ZoneLabel(s.zone),
                     pos.x, pos.y, pos.z);
            s.zone = P::Zone::None;
            s.none_since_ns = 0;
            s.block = nullptr;
            s.wait_serial = 0;
            s.wait_why = nullptr;
            Publish(s, now);
            return;
        }
    } else {
        s.none_since_ns = 0;
    }
    if (live != P::Zone::None && live != s.zone) {
        s.zone = live;
        u32 mask = 0;
        s.learned_ok = ReadLearned(player, mask);
        s.learned = s.learned_ok ? mask : 0;
        s.next_learned_ns = now + 1'000'000'000;
        s.block_logged = false;
        LOG_INFO(Core,
                 "VR pose: zone {} on (flags 12100118={} 12600027={}{}; at ({:.2f}, {:.2f}, "
                 "{:.2f}), {:.1f} m from the Brain's centre; chrType {}; learned: {} ({:#x}){})",
                 P::ZoneLabel(live), doll ? 1 : 0, brain ? 1 : 0, g_test_brain ? " forced" : "",
                 pos.x, pos.y, pos.z, P::BrainHorizontal(pos), Read<s32>(player + OffsetChrType),
                 s.learned_ok ? LearnedNames(s.learned) : std::string{"(list not found: none)"},
                 s.learned, P::GetPoseSettings().all ? "; pose_gestures_all=1: every gesture counts" : "");
    }
    if (s.zone == P::Zone::None) {
        return;
    }
    if (now >= s.next_learned_ns) {
        s.next_learned_ns = now + 1'000'000'000;
        u32 mask = 0;
        s.learned_ok = ReadLearned(player, mask);
        s.learned = s.learned_ok ? mask : 0;
    }
    // The gates.
    P::GateInput g{};
    g.host = Read<s32>(player + OffsetChrType) == 0;
    const u8* const data = Module(player, L::OffsetDataModule);
    g.alive = data != nullptr && Read<s32>(data + OffsetHp) > 0;
    s32 anim = -1;
    g.free = AnimFree(player, anim) && !OnLadder(player);
    g.menu = VR::IsGameMenuOpen();
    g.cinematic = VR::CinematicActive();
    const char* const block = P::GateBlock(g);
    if (block != s.block || !s.block_logged) {
        if (s.block_logged || block != nullptr) {
            LOG_INFO(Core, "VR pose: {} (anim {})", block != nullptr ? block : "free again",
                     anim);
        }
        s.block = block;
        s.block_logged = true;
    }
    // The presenter's pose (fresh: written within the last 0.3 s).
    P::PoseState ps{};
    const bool fresh = P::PoseChannel().Load(ps) && now - ps.ns < 300'000'000;
    const auto& st = P::GetPoseSettings();
    P::TriggerInput in{};
    in.zone = live; // a marker needs the game's conditions now
    in.in_region = g_test_brain || P::InBrainRegion(pos);
    in.pose = fresh ? static_cast<P::Kind>(ps.pose) : P::Kind::None;
    in.held_s = ps.held_s;
    in.serial = ps.serial;
    in.block = block;
    in.learned = s.learned;
    in.all = VR::Tune::Get("pose.all", st.all ? 1.0f : 0.0f) != 0.0f;
    in.contact_hold_s = VR::Tune::Get("pose.hold_s", st.contact_hold_s);
    in.now_s = static_cast<double>(now) * 1e-9;
    const P::TriggerOutput out = s.trigger.Step(in);
    if (out.count > 0) {
        std::string applied;
        bool first_added = false;
        for (int i = 0; i < out.count; ++i) {
            const s32 id = out.markers[i];
            const bool added = ApplyMarker(player, id);
            if (g_disabled) {
                return;
            }
            first_added = first_added || (i == 0 && added);
            // (A new entry turns active in the game's next SpEffect step: WatchMarker logs it.)
            applied += fmt::format("{}{} {}", i == 0 ? "" : ", ", id, added ? "added" : "NOT added");
        }
        if (first_added) { // the presenter's pulse (164: the Brain answered)
            ++s.fired;
            s.fired_marker = out.markers[0];
            s.watch_id = out.markers[0];
            s.watch_from_ns = now;
            s.watch_seen_ns = 0;
        }
        LOG_INFO(Core, "VR pose: {} held {:.2f} s at {}: marker {}{}", P::KindName(in.pose),
                 in.held_s, P::ZoneLabel(live), applied,
                 live == P::Zone::Brain ? fmt::format(" ({:.1f} m from the region's centre)",
                                                      P::BrainHorizontal(pos))
                                        : std::string{});
    } else if (out.waiting != nullptr && (in.serial != s.wait_serial || out.waiting != s.wait_why)) {
        s.wait_serial = in.serial;
        s.wait_why = out.waiting;
        if (std::strcmp(out.waiting, "holding") != 0 || live == P::Zone::Brain) {
            LOG_INFO(Core, "VR pose: {} held {:.2f} s at {}: no marker yet ({}{})",
                     P::KindName(in.pose), in.held_s, P::ZoneLabel(live), out.waiting,
                     std::strcmp(out.waiting, "holding") == 0 && live == P::Zone::Brain
                         ? fmt::format(": 164 at {:.1f} s", in.contact_hold_s)
                         : std::string{});
        }
    }
    Publish(s, now);
}

} // namespace VR::Bloodborne
