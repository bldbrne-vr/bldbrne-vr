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
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "common/thread.h"
#include "core/vr/bloodborne_pickup.h"
#include "core/vr/hook_util.h"
#include "core/vr/pickup_gate.h"
#include "core/vr/vr_session.h"

namespace VR::Bloodborne {

namespace {

namespace P = VR::Pickup;
using Hook::Plausible;
using Hook::Read;

// Module-relative addresses in the 1.09 eboot (patch file address - 0x400000); byte-identical in
// the original and the -mods eboot, untouched by the bb-vr patch presets.
//
// The item checker (the ActionButton checker for items, vtable slot 0x531CC70), once per frame:
// enemy drops (uid 9, ItemDropMan list 2), world items (uid 8, list 1), treasure (uid 10, list 3).
constexpr u64 CheckerRva = 0x12FD880;
constexpr std::array<u8, 40> CheckerSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
    0x83, 0xEC, 0x58, 0x48, 0x89, 0x75, 0xA0, 0x48, 0x8B, 0x1D, 0x44, 0x63, 0x0E, 0x04,
    0x48, 0x8B, 0x03, 0x48, 0x89, 0x45, 0xD0, 0x48, 0x8D, 0x05, 0xCE, 0x0F,
};
constexpr size_t CheckerPrologueLen = 17; // push rbp .. sub rsp, 0x58 (position independent)
// Its `lea rax, [ItemDropMan]` (0x553D6E0) and `lea rax, [WorldChrMan]` (0x553E878).
constexpr u64 IdmLeaRva = 0x12FD8C3;
constexpr std::array<u8, 7> IdmLeaSig = {0x48, 0x8D, 0x05, 0x16, 0xFE, 0x23, 0x04};
constexpr u64 WorldLeaRva = 0x12FD8A3;
constexpr std::array<u8, 7> WorldLeaSig = {0x48, 0x8D, 0x05, 0xCE, 0x0F, 0x24, 0x04};
constexpr u64 ItemDropManRva = 0x553D6E0;
constexpr u64 WorldChrManRva = 0x553E878;
constexpr u64 GameDataManRva = 0x553B130; // +0x08: PlayerGameData
// ItemDropMan::PickUp(idm, const s32* handle): the whole pickup (removal, event flag, the lot
// into the inventory or storage, the pop-up and its sound); no animation.
constexpr u64 PickUpRva = 0x17D8570;
constexpr std::array<u8, 40> PickUpSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
    0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00, 0x49, 0x89, 0xF4, 0x49, 0x89, 0xFE, 0x48, 0x8B,
    0x05, 0x4F, 0xB6, 0xC0, 0x03, 0x48, 0x8B, 0x00, 0x48, 0x89, 0x45, 0xD0,
};
// The event pushes (ctx, 4010, n) and (ctx, 4010, 1001 + n, animation): to the event system
// (OnEvent_4010_1 / _2 / _1001_PlayAnim / _1002_ForcePlayAnim: the crouch) and, online, to the
// other players.
constexpr u64 EventRva = 0x132E960;
constexpr std::array<u8, 24> EventSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
    0x53, 0x48, 0x83, 0xEC, 0x48, 0x41, 0x89, 0xD7, 0x41, 0x89, 0xF4, 0x48,
};
constexpr u64 EventAnimRva = 0x132EAD0;
constexpr std::array<u8, 24> EventAnimSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
    0x53, 0x48, 0x83, 0xEC, 0x48, 0x41, 0x89, 0xCF, 0x41, 0x89, 0xD4, 0x41,
};
// FindItemIndex(inventory, const u32* item): hash lookup, the index or -1. Pure.
constexpr u64 FindRva = 0x14D9F60;
constexpr std::array<u8, 32> FindSig = {
    0x8B, 0x06, 0x83, 0xF8, 0xFF, 0x0F, 0x84, 0xA6, 0x00, 0x00, 0x00, 0x89, 0xC1, 0x48, 0x69, 0xC9,
    0x15, 0x3F, 0xEF, 0x03, 0x48, 0xC1, 0xE9, 0x20, 0x89, 0xC2, 0x29, 0xCA, 0xD1, 0xEA, 0x01, 0xCA,
};

// pickup_reach: how far an item on the ground offers its prompt. The checker asks for the nearest
// item whose ActionButtonParam region holds the hunter (0x17D9FB0 for enemy drops and world
// items, 0x17DA500 for treasure), through ActionButtonMan's region test (0x1CDE850): the row's
// regionType (+0x00) picks the test, 0 a cylinder (0x1CDBB40): the level distance under radius
// (+0x0C, metres) plus the item's own +0x44, height +0x1C above +0x20, angle +0x10 / +0x28. The
// rows: 4000 for world items (0x12FDADE), 4100 or 4110 for enemy drops (0x12FD8D1, 0x12FD915) and
// each treasure's own (its entry's +0x4C, read at 0x17DA5E1); a treasure without one (-1, most of
// them) takes 4300 with its flag 0x100, else 4200 (0x17DA5F5, 0x17DA60F). A row comes from 0x1F128A0
// (Out{const void* 0x531CBF0; s32 -1; u8* row}*, s32 id), as the region test asks for it. The
// rows are the game's, read on this same thread: their radius is scaled in place, once.
constexpr u64 AbRowRva = 0x1F128A0;
constexpr std::array<u8, 28> AbRowSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x41, 0x89, 0xF4,
    0x49, 0x89, 0xFE, 0x31, 0xC9, 0x45, 0x85, 0xE4, 0x0F, 0x88, 0xF2, 0x00, 0x00, 0x00,
};
constexpr u64 AbRowOutRva = 0x531CBF0;
// The region test's lookup (lea of the Out's first word ... call 0x1F128A0) and its dispatch on
// the row's first byte; the cylinder test's reads of radius, angle, height, base, angle check.
constexpr u64 AbLookupRva = 0x1CDE889;
constexpr std::array<u8, 37> AbLookupSig = {
    0x48, 0x8D, 0x05, 0x60, 0xE3, 0x63, 0x03, 0x48, 0x89, 0x45, 0xB8, 0xC7, 0x45,
    0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0x48, 0xC7, 0x45, 0xC8, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x8D, 0x7D, 0xB8, 0x89, 0xCE, 0xE8, 0xF2, 0x3F, 0x23, 0x00,
};
constexpr u64 AbDispatchRva = 0x1CDE8B7;
constexpr std::array<u8, 11> AbDispatchSig = {0x0F, 0xB6, 0x00, 0x83, 0xF8, 0x02,
                                              0x74, 0x30, 0x83, 0xF8, 0x01};
constexpr u64 AbCylinderRva = 0x1CDBC16;
constexpr std::array<u8, 23> AbCylinderSig = {
    0xC5, 0x7A, 0x10, 0x79, 0x0C, 0x44, 0x8B, 0x71, 0x10, 0xC5, 0x7A, 0x10,
    0x69, 0x1C, 0xC5, 0xFA, 0x10, 0x71, 0x20, 0x0F, 0xB6, 0x41, 0x24,
};
constexpr u64 DropRowRva = 0x12FD8D1; // mov r12d, 4100; mov edx, 2; mov ecx, 4100
constexpr std::array<u8, 16> DropRowSig = {0x41, 0xBC, 0x04, 0x10, 0x00, 0x00, 0xBA, 0x02,
                                           0x00, 0x00, 0x00, 0xB9, 0x04, 0x10, 0x00, 0x00};
constexpr u64 DropRowFlaggedRva = 0x12FD915; // mov eax, 4110
constexpr std::array<u8, 5> DropRowFlaggedSig = {0xB8, 0x0E, 0x10, 0x00, 0x00};
constexpr u64 WorldRowRva = 0x12FDADE; // mov ecx, 4000
constexpr std::array<u8, 5> WorldRowSig = {0xB9, 0xA0, 0x0F, 0x00, 0x00};
// mov r12d, [r14+0x4C]; cmp r12d, -1; jnz; mov eax, [r14+0x48]; bt eax, 8; jnb; mov r12d, 4300;
// jmp; mov ecx, 0x108; bextr eax, eax, ecx; mov r12d, -1; jnz; mov r12d, 4200
constexpr u64 TreasureRowRva = 0x17DA5E1;
constexpr std::array<u8, 52> TreasureRowSig = {
    0x45, 0x8B, 0x66, 0x4C, 0x41, 0x83, 0xFC, 0xFF, 0x75, 0x35, 0x41, 0x8B, 0x46,
    0x48, 0x0F, 0xBA, 0xE0, 0x08, 0x73, 0x08, 0x41, 0xBC, 0xCC, 0x10, 0x00, 0x00,
    0xEB, 0x23, 0xB9, 0x08, 0x01, 0x00, 0x00, 0xC4, 0xE2, 0x70, 0xF7, 0xC0, 0x41,
    0xBC, 0xFF, 0xFF, 0xFF, 0xFF, 0x75, 0x11, 0x41, 0xBC, 0x68, 0x10, 0x00, 0x00,
};
constexpr std::array<s32, 5> FixedItemRows = {4000, 4100, 4110, 4200, 4300};
constexpr u32 OffsetAbRadius = 0x0C;
constexpr u32 OffsetAbHeight = 0x1C;
constexpr u32 OffsetAbBase = 0x20;
constexpr u32 OffsetAbText = 0x30;

// The three pickups: `call PickUp` (its arguments rdi = ItemDropMan, rsi = &handle and the handle
// at [rbp-0x38] are set just before), the event's argument setup (only callee-saved registers and
// constants: rbx / r15 / [rbp-0x60] = the checker's context), `call` the event push. No branch
// lands inside any of them (the whole code segment was scanned); the next instruction is the
// common continuation.
constexpr size_t CallLen = 5;
struct Site {
    u64 rva;
    P::Kind kind;
    std::array<u8, 31> bytes;
    size_t setup; // bytes between the two calls
    u64 event_rva;
};
constexpr std::array<Site, 3> Sites = {{
    {0x12FDAB6,
     P::Kind::Drop,
     {
         0xE8, 0xB5, 0xAA, 0x4D, 0x00, // call 0x17D8570 (PickUp)
         0xBE, 0xAA, 0x0F, 0x00, 0x00, // mov esi, 4010
         0x48, 0x8B, 0x7D, 0xA0,       // mov rdi, [rbp-0x60]
         0x89, 0xDA,                   // mov edx, ebx (1, or 2 in an event animation)
         0xE8, 0x95, 0x0E, 0x03, 0x00, // call 0x132E960
     },
     11,
     EventRva},
    {0x12FDBD6,
     P::Kind::World,
     {
         0xE8, 0x95, 0xA9, 0x4D, 0x00, // call 0x17D8570 (PickUp)
         0xBE, 0xAA, 0x0F, 0x00, 0x00, // mov esi, 4010
         0xBA, 0x01, 0x00, 0x00, 0x00, // mov edx, 1
         0x48, 0x8B, 0x7D, 0xA0,       // mov rdi, [rbp-0x60]
         0xE8, 0x72, 0x0D, 0x03, 0x00, // call 0x132E960
     },
     14,
     EventRva},
    {0x12FDDEF,
     P::Kind::Treasure,
     {
         0xE8, 0x7C, 0xA7, 0x4D, 0x00, // call 0x17D8570 (PickUp)
         0x0F, 0xB6, 0xD3,             // movzx edx, bl
         0x81, 0xC2, 0xE9, 0x03, 0x00, 0x00, // add edx, 1001
         0xBE, 0xAA, 0x0F, 0x00, 0x00,       // mov esi, 4010
         0x48, 0x8B, 0x7D, 0xA0,             // mov rdi, [rbp-0x60]
         0x44, 0x89, 0xF9,                   // mov ecx, r15d (the treasure's animation)
         0xE8, 0xC2, 0x0C, 0x03, 0x00,       // call 0x132EAD0
     },
     21,
     EventAnimRva},
}};
constexpr size_t SiteLen(const Site& site) {
    return CallLen + site.setup + CallLen;
}
static_assert(SiteLen(Sites[0]) >= 14 && SiteLen(Sites[1]) >= 14 && SiteLen(Sites[2]) >= 14);
static_assert(SiteLen(Sites[2]) <= Sites[2].bytes.size());

// ItemDropMan: list heads by handle & 0xF (1 world items +0x18, 2 enemy drops +0x38, 3 treasure
// +0x28; 0x17D81A0). Entry: +0x00 next, +0x20 handle, +0x30 position (float3), +0x48 u16 flags
// (0x40 can be picked up, 0x80 online), +0x4C ActionButton param, +0x50 animation (treasure),
// +0x54 item count, +0x58 + 16k: {lot ref, item, quantity, -} for k < 6.
constexpr u32 OffsetNext = 0x00;
constexpr u32 OffsetHandle = 0x20;
constexpr u32 OffsetPos = 0x30;
constexpr u32 OffsetFlags = 0x48;
constexpr u32 OffsetParam = 0x4C;
constexpr u32 OffsetAnim = 0x50;
constexpr u32 OffsetCount = 0x54;
constexpr u32 OffsetSlots = 0x58;
constexpr u32 SlotItem = 0x04;
constexpr u32 SlotQty = 0x08;
constexpr u16 FlagPickable = 0x40;
constexpr u16 FlagOnline = 0x80;
constexpr s32 MaxSlots = 6;
constexpr int MaxWalk = 1024;
// PlayerGameData -> EquipGameData (+0x1D0) -> inventory (+0x158): 16-byte entries {handle (0 =
// gone), item, quantity, sort}, key items first (core/vr/bloodborne_givekit.cpp).
constexpr u32 OffsetEgd = 0x1D0;
constexpr u32 OffsetInventory = 0x158;
constexpr u32 OffsetInvCapacity = 0x10;
constexpr u32 OffsetInvKeyCap = 0x24;
constexpr u32 OffsetInvNormalList = 0x48;
constexpr u32 OffsetInvKeyList = 0x58;
constexpr u32 OffsetInvHighest = 0x88;
// WorldChrMan +0x60 = the main PlayerIns; its ChrCtrl (+0x58) +0x90 = its world matrix
// (position at +0x0C / +0x1C / +0x2C).
constexpr u32 OffsetMainPlayer = 0x60;
constexpr u32 OffsetChrCtrl = 0x58;
constexpr u32 OffsetChrFrame = 0x90;

using FindFn = s32(PS4_SYSV_ABI*)(u8* inventory, const u32* item);

u8* g_base = nullptr;
FindFn g_find = nullptr;
std::unique_ptr<Xbyak::CodeGenerator> g_code;
bool g_installed = false;
int g_env_setting = -1; // VR_PICKUP_INSTANT: -1 = vr.ini
std::atomic<u32> g_instant{0};
u32 g_pickups = 0; // the checker's thread only

bool SettingOn() {
    return g_env_setting >= 0 ? g_env_setting != 0 : VR::GetSettings().pickup_instant;
}

// ---- Reading the game's data (the checker's own thread, where the game reads it too) ---------

u8* FindEntry(u8* idm, s32 handle) {
    u32 head = 0;
    switch (handle & 0xF) {
    case 1:
        head = 0x18;
        break;
    case 2:
        head = 0x38;
        break;
    case 3:
        head = 0x28;
        break;
    default:
        return nullptr;
    }
    u8* e = Read<u8*>(idm + head);
    for (int i = 0; i < MaxWalk && Plausible(e); ++i) {
        if (Read<s32>(e + OffsetHandle) == handle) {
            return e;
        }
        e = Read<u8*>(e + OffsetNext);
    }
    return nullptr;
}

u8* Inventory() {
    u8* const game_data = Read<u8*>(g_base + GameDataManRva);
    u8* const pgd = Plausible(game_data) ? Read<u8*>(game_data + 0x08) : nullptr;
    if (!Plausible(pgd)) {
        return nullptr;
    }
    u8* const inventory = pgd + OffsetEgd + OffsetInventory;
    return Read<u32>(inventory + OffsetInvCapacity) != 0 ? inventory : nullptr;
}

// The quantity of `item` in the inventory (weapons and armour: 1 if one is owned), -1 = unknown.
s32 InventoryCount(u8* inventory, u32 item) {
    if (inventory == nullptr || g_find == nullptr || P::IsNoItem(item)) {
        return -1;
    }
    const s32 index = g_find(inventory, &item);
    if (index < 0) {
        return 0;
    }
    if (static_cast<u32>(index) > Read<u32>(inventory + OffsetInvHighest)) {
        return -1;
    }
    const u32 key_cap = Read<u32>(inventory + OffsetInvKeyCap);
    const u32 i = static_cast<u32>(index);
    u8* const list = Read<u8*>(inventory + (i < key_cap ? OffsetInvKeyList : OffsetInvNormalList));
    if (!Plausible(list)) {
        return -1;
    }
    const u8* const entry = list + static_cast<size_t>(i < key_cap ? i : i - key_cap) * 16;
    return Read<u32>(entry) != 0 ? Read<s32>(entry + 8) : 0;
}

struct Lot {
    s32 count{0};
    std::array<u32, MaxSlots> item{};
    std::array<s32, MaxSlots> qty{};
};

Lot ReadLot(const u8* entry) {
    Lot lot{};
    lot.count = std::clamp(Read<s32>(entry + OffsetCount), 0, MaxSlots);
    for (s32 k = 0; k < lot.count; ++k) {
        const u8* const slot = entry + OffsetSlots + 16 * k;
        lot.item[k] = Read<u32>(slot + SlotItem);
        lot.qty[k] = Read<s32>(slot + SlotQty);
    }
    return lot;
}

std::array<float, 3> Position(const u8* entry) {
    return {Read<float>(entry + OffsetPos), Read<float>(entry + OffsetPos + 4),
            Read<float>(entry + OffsetPos + 8)};
}

// ---- The pickup (the checker's thread, inside its own pickup) ---------------------------------

struct Pending {
    bool valid{false};
    P::Kind kind{};
    s32 handle{0};
    u16 flags{0};
    s32 param{-1};
    s32 anim{-1};
    std::array<float, 3> pos{};
    Lot lot{};
    std::array<s32, MaxSlots> before{};
};
Pending g_pending; // the item checker runs on one thread at a time

// Right before PickUp: what the glow holds and how many of each the inventory has.
void PS4_SYSV_ABI BeforePickUp(u8* idm, const s32* handle, s32 kind) {
    g_pending = {};
    g_pending.kind = static_cast<P::Kind>(kind);
    if (!Plausible(idm) || handle == nullptr) {
        return;
    }
    g_pending.handle = *handle;
    const u8* const entry = FindEntry(idm, g_pending.handle);
    if (entry == nullptr) {
        g_pending.valid = true; // logged as "glow not found" (PickUp then does nothing either)
        g_pending.flags = 0;
        return;
    }
    g_pending.valid = true;
    g_pending.flags = Read<u16>(entry + OffsetFlags);
    g_pending.param = Read<s32>(entry + OffsetParam);
    g_pending.anim = Read<s32>(entry + OffsetAnim);
    g_pending.pos = Position(entry);
    g_pending.lot = ReadLot(entry);
    u8* const inventory = Inventory();
    for (s32 k = 0; k < g_pending.lot.count; ++k) {
        g_pending.before[k] = InventoryCount(inventory, g_pending.lot.item[k]);
    }
}

// Right after PickUp: true = leave the animation event out (an instant pickup).
bool PS4_SYSV_ABI AfterPickUp(s32 kind) {
    const P::GateInputs in{
        .setting = SettingOn(),
        .vr_enabled = VR::IsEnabled(),
        .display_auto = VR::GetDisplayMode() == VR::DisplayMode::Auto,
        .immersive = VR::IsImmersive(),
        .first_person = VR::IsFirstPerson(),
        .flat = VR::CinematicFlat(),
    };
    const P::Reason reason = P::Decide(in);
    const bool instant = reason == P::Reason::Instant;
    ++g_pickups;
    if (instant) {
        g_instant.fetch_add(1, std::memory_order_relaxed);
    }
    const Pending& p = g_pending;
    std::string items;
    std::string glow;
    if (!p.valid || p.flags == 0) {
        glow = "glow not found (nothing given)";
    } else {
        u8* const inventory = Inventory();
        for (s32 k = 0; k < p.lot.count; ++k) {
            const s32 after = InventoryCount(inventory, p.lot.item[k]);
            items += fmt::format("{}{} x{} [inventory {} -> {}{}]", k == 0 ? "" : ", ",
                                 P::ItemText(p.lot.item[k]), p.lot.qty[k], p.before[k], after,
                                 after >= 0 && p.before[k] >= 0 && after == p.before[k]
                                     ? ": not added (storage, or its maximum)"
                                     : "");
        }
        u8* const idm = Read<u8*>(g_base + ItemDropManRva);
        const u8* const still = Plausible(idm) ? FindEntry(idm, p.handle) : nullptr;
        const bool pickable = still != nullptr && (Read<u16>(still + OffsetFlags) & FlagPickable);
        if ((p.flags & FlagPickable) != 0 && !pickable) {
            // Publish on the pickup's own frame; waiting for the next MenuUpdate
            // lets the first faint confirmation reach the large rest quad.
            VR::PublishItemPickup(true);
        }
        glow = (p.flags & FlagOnline) != 0 ? "online: asked the host (granted when it answers)"
               : !(p.flags & FlagPickable) ? "glow not pickable (nothing given)"
               : pickable                  ? "glow STILL THERE"
                                           : "glow removed";
    }
    LOG_INFO(Core,
             "VR pickup: {} #{} handle {:#x} at ({:.2f},{:.2f},{:.2f}) param {}{}: {} item{}: "
             "{}; {}; {}{} (thread '{}')",
             P::KindName(static_cast<P::Kind>(kind)), g_pickups, static_cast<u32>(p.handle),
             p.pos[0], p.pos[1], p.pos[2], p.param,
             static_cast<P::Kind>(kind) == P::Kind::Treasure ? fmt::format(" anim {}", p.anim)
                                                             : std::string{},
             p.lot.count, p.lot.count == 1 ? "" : "s", items.empty() ? "-" : items, glow,
             P::ReasonText(reason), instant ? " (event 4010 left out)" : "",
             Common::GetCurrentThreadName());
    return instant;
}

// call PickUp exactly as the game does (its arguments are set), then the gate; the event push
// (the original setup, the call made absolute) only when the gate keeps the animation. At the site
// rsp is 16-byte aligned for a call (reached by a jump), and stays so (two pushes) for each call;
// only callee-saved registers and the frame are live after PickUp in the game's own code.
void* EmitSite(Xbyak::CodeGenerator& c, const Site& site, u8* module) {
    using namespace Xbyak::util;
    c.align(16);
    void* const entry = const_cast<u8*>(c.getCurr());
    Xbyak::Label skip;
    c.push(rdi);
    c.push(rsi);
    c.mov(edx, static_cast<u32>(site.kind));
    c.mov(rax, reinterpret_cast<u64>(&BeforePickUp));
    c.call(rax);
    c.pop(rsi);
    c.pop(rdi);
    c.mov(rax, reinterpret_cast<u64>(module + PickUpRva));
    c.call(rax);
    c.mov(edi, static_cast<u32>(site.kind));
    c.mov(rax, reinterpret_cast<u64>(&AfterPickUp));
    c.call(rax);
    c.test(al, al);
    c.jnz(skip, Xbyak::CodeGenerator::T_NEAR);
    for (size_t i = 0; i < site.setup; ++i) {
        c.db(site.bytes[CallLen + i]);
    }
    c.mov(rax, reinterpret_cast<u64>(module + site.event_rva));
    c.call(rax);
    c.L(skip);
    c.db(0xFF); // jmp qword [rip+0]; dq back
    c.db(0x25);
    c.dd(0);
    c.dq(reinterpret_cast<u64>(module + site.rva + SiteLen(site)));
    return entry;
}

// ---- VR_PICKUP_SCAN (tests): the active glows near the hunter, at the checker's entry ---------

std::vector<u64> g_scan_last; // handle << 16 | flags, sorted
s64 g_scan_ns = 0;
u64 g_scan_calls = 0;
bool g_scan_logged = false;

s64 SteadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ---- pickup_reach: the items' rows, scaled at the checker's entry ------------------------------

struct AbRowOut {
    const void* first; // the region test's own first word (AbRowOutRva)
    s32 id;
    u32 pad;
    u8* row;
};
static_assert(sizeof(AbRowOut) == 0x18 && offsetof(AbRowOut, row) == 0x10);
using AbRowFn = void(PS4_SYSV_ABI*)(AbRowOut*, s32 id);

bool g_reach = false; // the rows can be found and scaled (every signature matched)
bool g_scan = false;  // VR_PICKUP_SCAN
s64 g_reach_ns = 0;
// The rows scaled (the checker's thread only): the radius written, so a row is scaled once, and
// again from its new value if the game ever loads its params anew. `skip`: left as it is (not a
// cylinder, or an implausible radius; logged once).
struct Reached {
    s32 id;
    const u8* row;
    float written;
    bool skip;
};
std::vector<Reached> g_reached;

void Reach(s32 id, float scale) {
    AbRowOut out{g_base + AbRowOutRva, -1, 0, nullptr};
    reinterpret_cast<AbRowFn>(g_base + AbRowRva)(&out, id);
    u8* const row = out.row;
    if (!Plausible(row, 4)) {
        return;
    }
    auto it = std::find_if(g_reached.begin(), g_reached.end(),
                           [&](const Reached& r) { return r.id == id && r.row == row; });
    const float radius = Read<float>(row + OffsetAbRadius);
    if (it != g_reached.end() && (it->skip || radius == it->written)) {
        return;
    }
    const u8 region = Read<u8>(row);
    if (region != 0 || !(radius > 0.05f && radius < 20.0f)) {
        LOG_INFO(Core, "VR pickup: reach row {}: region type {}, radius {:.2f}: left as it is", id,
                 region, radius);
        if (it == g_reached.end()) {
            g_reached.push_back({id, row, radius, true});
        } else {
            it->skip = true;
        }
        return;
    }
    const float scaled = radius * scale;
    std::memcpy(row + OffsetAbRadius, &scaled, sizeof(scaled));
    LOG_INFO(Core,
             "VR pickup: reach row {} radius {:.2f} -> {:.2f} m (x{:.2f}; height {:.2f} from "
             "{:+.2f}, text {}){}",
             id, radius, scaled, scale, Read<float>(row + OffsetAbHeight),
             Read<float>(row + OffsetAbBase), Read<s32>(row + OffsetAbText),
             it != g_reached.end() ? " again (the row was loaded anew)" : "");
    if (it == g_reached.end()) {
        g_reached.push_back({id, row, scaled, false});
    } else {
        it->written = scaled;
    }
}

// Twice a second: the fixed rows, and every treasure's own row (a new map's treasure is scaled
// within half a second of its list filling, before the hunter can walk to it).
void ReachTick() {
    const float scale = VR::GetSettings().pickup_reach;
    if (!g_reach || std::abs(scale - 1.0f) < 1e-3f) {
        return;
    }
    const s64 now = SteadyNs();
    if (now - g_reach_ns < 500'000'000) {
        return;
    }
    g_reach_ns = now;
    for (const s32 id : FixedItemRows) {
        Reach(id, scale);
    }
    u8* const idm = Read<u8*>(g_base + ItemDropManRva);
    if (!Plausible(idm)) {
        return;
    }
    std::array<s32, 16> seen{};
    size_t seen_count = 0;
    u8* e = Read<u8*>(idm + 0x28); // the treasure list
    for (int i = 0; i < MaxWalk && Plausible(e); ++i, e = Read<u8*>(e + OffsetNext)) {
        const s32 id = Read<s32>(e + OffsetParam);
        if (id <= 0 || std::find(seen.begin(), seen.begin() + seen_count, id) !=
                           seen.begin() + seen_count) {
            continue;
        }
        if (seen_count < seen.size()) {
            seen[seen_count++] = id;
        }
        Reach(id, scale);
    }
}

void PS4_SYSV_ABI ScanHook(u8* /*checker*/, u8* /*saved_ymm*/) {
    ReachTick();
    if (!g_scan) {
        return;
    }
    ++g_scan_calls;
    const s64 now = SteadyNs();
    if (now - g_scan_ns < 1'000'000'000) {
        return;
    }
    g_scan_ns = now;
    u8* const idm = Read<u8*>(g_base + ItemDropManRva);
    u8* const world = Read<u8*>(g_base + WorldChrManRva);
    u8* const player = Plausible(world) ? Read<u8*>(world + OffsetMainPlayer) : nullptr;
    u8* const ctrl = Plausible(player) ? Read<u8*>(player + OffsetChrCtrl) : nullptr;
    if (!Plausible(idm) || !Plausible(ctrl)) {
        return;
    }
    const u8* const w = ctrl + OffsetChrFrame;
    const std::array<float, 3> me = {Read<float>(w + 0x0C), Read<float>(w + 0x1C),
                                     Read<float>(w + 0x2C)};
    struct Glow {
        float dist;
        const char* list;
        const u8* e;
    };
    std::vector<Glow> glows;
    std::vector<u64> keys;
    static constexpr std::array<std::pair<u32, const char*>, 3> Lists = {
        {{0x38, "drop"}, {0x18, "world"}, {0x28, "treasure"}}};
    for (const auto& [head, name] : Lists) {
        u8* e = Read<u8*>(idm + head);
        for (int i = 0; i < MaxWalk && Plausible(e); ++i, e = Read<u8*>(e + OffsetNext)) {
            const u16 flags = Read<u16>(e + OffsetFlags);
            const auto at = Position(e);
            glows.push_back({std::hypot(at[0] - me[0], at[2] - me[2]), name, e});
            keys.push_back((static_cast<u64>(static_cast<u32>(Read<s32>(e + OffsetHandle))) << 16) |
                           flags);
        }
    }
    // Explicit private-save regression trigger. Uses the actual pickup path (and
    // hence its real Scaleform confirmation), never a fabricated HUD overlay.
    if (const char* path = std::getenv("VR_PICKUP_TEST_FILE")) {
        std::ifstream input(path);
        std::string command;
        if (input >> command) {
            input.close();
            std::remove(path);
            const s32 handle = static_cast<s32>(std::strtol(command.c_str(), nullptr, 0));
            const auto found = std::find_if(glows.begin(), glows.end(), [&](const Glow& g) {
                return Read<s32>(g.e + OffsetHandle) == handle &&
                       (Read<u16>(g.e + OffsetFlags) & FlagPickable) != 0;
            });
            if (found != glows.end() &&
                std::memcmp(g_base + PickUpRva, PickUpSig.data(), PickUpSig.size()) == 0) {
                const s32 kind = std::strcmp(found->list, "treasure") == 0 ? 2 :
                                 std::strcmp(found->list, "world") == 0 ? 1 : 0;
                BeforePickUp(idm, &handle, kind);
                using PickUpFn = void(PS4_SYSV_ABI*)(u8*, const s32*);
                reinterpret_cast<PickUpFn>(g_base + PickUpRva)(idm, &handle);
                AfterPickUp(kind);
                return; // PickUp removed the entry: do not reuse the scanned pointers.
            }
            LOG_WARNING(Core, "VR pickup: test handle {:#x} not pickable", handle);
        }
    }
    std::sort(keys.begin(), keys.end());
    if (g_scan_logged && keys == g_scan_last) {
        return;
    }
    g_scan_logged = true;
    g_scan_last = keys;
    std::sort(glows.begin(), glows.end(),
              [](const Glow& a, const Glow& b) { return a.dist < b.dist; });
    const auto pickable = std::count_if(glows.begin(), glows.end(), [](const Glow& g) {
        return (Read<u16>(g.e + OffsetFlags) & FlagPickable) != 0;
    });
    LOG_INFO(Core,
             "VR pickup: SCAN hunter ({:.2f},{:.2f},{:.2f}): {} glow(s), {} pickable (checker call "
             "{}, thread '{}')",
             me[0], me[1], me[2], glows.size(), pickable, g_scan_calls,
             Common::GetCurrentThreadName());
    for (size_t i = 0; i < glows.size() && i < 40; ++i) {
        const u8* const e = glows[i].e;
        const auto at = Position(e);
        const Lot lot = ReadLot(e);
        std::string items;
        for (s32 k = 0; k < lot.count; ++k) {
            items += fmt::format("{}{} x{}", k == 0 ? "" : ", ", P::ItemText(lot.item[k]),
                                 lot.qty[k]);
        }
        LOG_INFO(Core,
                 "VR pickup: SCAN   {} handle {:#x} flags {:#x} param {} anim {} at "
                 "({:.2f},{:.2f},{:.2f}) {:.1f} m (dy {:+.1f}): {}",
                 glows[i].list, static_cast<u32>(Read<s32>(e + OffsetHandle)),
                 Read<u16>(e + OffsetFlags), Read<s32>(e + OffsetParam), Read<s32>(e + OffsetAnim),
                 at[0], at[1], at[2], glows[i].dist, at[1] - me[1], items.empty() ? "-" : items);
    }
}

bool EnvFlag(const char* name, int& out) {
    const char* const value = std::getenv(name);
    if (value == nullptr || value[0] == 0) {
        return false;
    }
    out = (value[0] == '0' || value[0] == 'n' || value[0] == 'N' || value[0] == 'f' ||
           value[0] == 'F')
              ? 0
              : 1;
    return true;
}

} // Anonymous namespace

void InstallPickup(VAddr base, u64 size) {
    if (g_installed || base == 0) {
        return;
    }
    auto* const module = reinterpret_cast<u8*>(base);
    g_base = module;
    EnvFlag("VR_PICKUP_INSTANT", g_env_setting);
    if (size < WorldChrManRva + sizeof(u64)) {
        LOG_INFO(Core, "VR pickup: eboot too small for Bloodborne 1.09; the game's own pickups");
        return;
    }
    const auto matches = [module](u64 rva, const auto& sig, size_t length) {
        return std::memcmp(module + rva, sig.data(), length) == 0;
    };
    std::string bad;
    if (!matches(CheckerRva, CheckerSig, CheckerSig.size())) {
        bad += " item checker";
    }
    if (!matches(IdmLeaRva, IdmLeaSig, IdmLeaSig.size()) ||
        !matches(WorldLeaRva, WorldLeaSig, WorldLeaSig.size())) {
        bad += " globals";
    }
    if (!matches(PickUpRva, PickUpSig, PickUpSig.size())) {
        bad += " PickUp";
    }
    if (!matches(EventRva, EventSig, EventSig.size()) ||
        !matches(EventAnimRva, EventAnimSig, EventAnimSig.size())) {
        bad += " event push";
    }
    if (!matches(FindRva, FindSig, FindSig.size())) {
        bad += " inventory lookup";
    }
    for (const Site& site : Sites) {
        if (!matches(site.rva, site.bytes, SiteLen(site))) {
            bad += fmt::format(" site {:#x} ({})", site.rva,
                               Hook::HexBytes(module + site.rva, SiteLen(site)));
        }
    }
    if (!bad.empty()) {
        LOG_WARNING(Core,
                    "VR pickup: signature MISMATCH ({}); items are picked up with the game's own "
                    "animation",
                    bad.substr(1));
        return;
    }
    g_find = reinterpret_cast<FindFn>(module + FindRva);
    const bool scan = std::getenv("VR_PICKUP_SCAN") != nullptr;
    g_scan = scan;
    // pickup_reach: the row lookup, the region test and the rows the checker passes, all as read
    // above; otherwise the game's own radius.
    std::string reach_bad;
    const auto need = [&](u64 rva, const auto& sig, const char* what) {
        if (!matches(rva, sig, sig.size())) {
            reach_bad += fmt::format(" {} {:#x} ({})", what, rva,
                                     Hook::HexBytes(module + rva, sig.size()));
        }
    };
    need(AbRowRva, AbRowSig, "row lookup");
    need(AbLookupRva, AbLookupSig, "region test");
    need(AbDispatchRva, AbDispatchSig, "region type");
    need(AbCylinderRva, AbCylinderSig, "cylinder");
    need(DropRowRva, DropRowSig, "drop row");
    need(DropRowFlaggedRva, DropRowFlaggedSig, "drop row 4110");
    need(WorldRowRva, WorldRowSig, "world row");
    need(TreasureRowRva, TreasureRowSig, "treasure row");
    const float reach = VR::GetSettings().pickup_reach;
    if (!reach_bad.empty()) {
        LOG_WARNING(Core, "VR pickup: reach signature MISMATCH ({}); the game's pickup radius",
                    reach_bad.substr(1));
    }
    g_reach = reach_bad.empty() && std::abs(reach - 1.0f) >= 1e-3f;
    const bool entry = scan || g_reach; // the checker's entry hook (ScanHook: ReachTick, the scan)
    for (const Site& site : Sites) {
        if (!Hook::MakeWritable(module + site.rva, SiteLen(site))) {
            LOG_ERROR(Core, "VR pickup: could not make {:#x} writable; the game's own pickups",
                      site.rva);
            return;
        }
    }
    if (entry && !Hook::MakeWritable(module + CheckerRva, CheckerPrologueLen)) {
        LOG_ERROR(Core, "VR pickup: could not make the checker writable; no scan, the game's "
                        "pickup radius");
        g_reach = false;
    }
    try {
        g_code = std::make_unique<Xbyak::CodeGenerator>(1024);
        std::array<void*, Sites.size()> stubs{};
        for (size_t i = 0; i < Sites.size(); ++i) {
            stubs[i] = EmitSite(*g_code, Sites[i], module);
        }
        void* scan_stub = nullptr;
        if (scan || g_reach) {
            using Xbyak::util::rdi;
            scan_stub = Hook::EmitHostCallStub(
                *g_code, reinterpret_cast<void*>(&ScanHook), CheckerSig.data(), CheckerPrologueLen,
                reinterpret_cast<u64>(module + CheckerRva + CheckerPrologueLen), rdi);
        }
        for (size_t i = 0; i < Sites.size(); ++i) {
            Hook::WriteJump(module + Sites[i].rva, SiteLen(Sites[i]), stubs[i]);
        }
        if (scan_stub != nullptr) {
            Hook::WriteJump(module + CheckerRva, CheckerPrologueLen, scan_stub);
        }
    } catch (const Xbyak::Error& e) {
        LOG_ERROR(Core, "VR pickup: failed to generate the stubs ({}); the game's own pickups",
                  e.what());
        return;
    }
    g_installed = true;
    LOG_INFO(Core,
             "VR pickup: installed at {:#x} / {:#x} / {:#x} (enemy drops, world items, treasure): "
             "pickup_instant {}{} - in first-person VR an item is granted with the game's pop-up "
             "and no pickup animation; pickup_reach x{:.2f}{}{}",
             Sites[0].rva, Sites[1].rva, Sites[2].rva, SettingOn() ? 1 : 0,
             g_env_setting >= 0 ? " (VR_PICKUP_INSTANT)" : "", reach,
             g_reach ? " (the radius of item rows 4000 / 4100 / 4110 / 4200 / 4300 and each "
                       "treasure's own)"
                     : " (the game's radius)",
             scan ? "; VR_PICKUP_SCAN: the item glows near the hunter are logged" : "");
}

u32 InstantPickups() {
    return g_instant.load(std::memory_order_relaxed);
}

} // namespace VR::Bloodborne
