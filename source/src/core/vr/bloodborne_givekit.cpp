// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fmt/format.h>

#include "common/logging/log.h"
#include "common/thread.h"
#include "core/vr/bb_weapons.h"
#include "core/vr/bloodborne_givekit.h"
#include "core/vr/hook_util.h"
#include "core/vr/test_stats.h"
#include "core/vr/vr_session.h"

namespace VR::Bloodborne {

namespace {

using Hook::Plausible;
using Hook::Read;

// Module-relative addresses in the 1.09 eboot (patch file address - 0x400000). Every function is
// byte-identical in the original and the -mods eboot and untouched by the bb-vr patch presets.
// EquipGameData::AddItem(egd, category, id, qty, -, trophy notify, no auto quick slot): what the
// shop's purchase (0x1B5C8C0) and the game's debug "item get" (0x14D49F0) call. Returns the
// inventory index or -1 (status at egd+0x29C: 0 ok, 2 capped or already owned, 4 failed). Goods
// stack (clamped at their maximum); a weapon gets its own new item handle.
constexpr u64 AddItemRva = 0x14CDFD0;
constexpr std::array<u8, 32> AddItemSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC,
    0x18, 0x01, 0x00, 0x00, 0x41, 0x89, 0xCD, 0x41, 0x89, 0xD4, 0x49, 0x89, 0xFF, 0x48, 0x8B, 0x1D,
};
// The shop's room for an item (const u32* item): goods: the maximum carried (rune bonuses
// included, 0x1580FA0) minus the count owned, or 0 without a free inventory entry; a weapon: 1 if
// an inventory entry is free. Pure reads (GameDataMan's PlayerGameData).
constexpr u64 RoomRva = 0x1B5CA20;
constexpr std::array<u8, 32> RoomSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x49,
    0x89, 0xFC, 0x4C, 0x8B, 0x3D, 0xA7, 0x71, 0x88, 0x03, 0x49, 0x8B, 0x07, 0x48, 0x89, 0x45, 0xD8,
};
// Its `lea rax, [GameDataMan]` (0x553B130) proves the global the game's functions use.
constexpr u64 RoomLeaRva = 0x1B5CA58;
constexpr std::array<u8, 7> RoomLeaSig = {0x48, 0x8D, 0x05, 0xD1, 0xE6, 0x9D, 0x03};
constexpr u64 GameDataManRva = 0x553B130; // +0x08: PlayerGameData
// FindItemIndex(inventory, const u32* item): hash lookup, the index or -1. Pure.
constexpr u64 FindRva = 0x14D9F60;
constexpr std::array<u8, 32> FindSig = {
    0x8B, 0x06, 0x83, 0xF8, 0xFF, 0x0F, 0x84, 0xA6, 0x00, 0x00, 0x00, 0x89, 0xC1, 0x48, 0x69, 0xC9,
    0x15, 0x3F, 0xEF, 0x03, 0x48, 0xC1, 0xE9, 0x20, 0x89, 0xC2, 0x29, 0xCA, 0xD1, 0xEA, 0x01, 0xCA,
};
// The Equipment menu's equip action (reached from the menu command 0x1C18B60), `void(s32 slot,
// const ItemRef*)`; it reads only ref+0x48 (inventory index) and ref+0x4C (item). Slots: 0 = L1,
// 1 = R1, 2 = L2, 3 = R2 (weapons, through SetEquip 0x14CCB90; the hand shows it within frames),
// 16 + k = quick-item slot k. The item already in the target slot UNEQUIPS it (a toggle): never
// called then.
constexpr u64 MenuEquipRva = 0x1B63E00;
constexpr std::array<u8, 32> MenuEquipSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC,
    0x78, 0x41, 0x89, 0xFE, 0x4C, 0x8B, 0x2D, 0xC5, 0xFD, 0x87, 0x03, 0x49, 0x8B, 0x45, 0x00, 0x48,
};
// Its `mov rax, [MenuMan]`: MenuEquip stores into [MenuMan+0x150] without a null check.
constexpr u64 MenuManMovRva = 0x1B64289;
constexpr std::array<u8, 7> MenuManMovSig = {0x48, 0x8B, 0x05, 0xE8, 0xE5, 0x9F, 0x03};
constexpr u64 MenuManRva = 0x5562878;
// GoodsParamRow, `void(Out{s32 id; u32 pad; u8* row}*, s32 goods)`.
constexpr u64 GoodsRowRva = 0x1F1E3D0;
constexpr std::array<u8, 32> GoodsRowSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x41, 0x89, 0xF4, 0x49, 0x89,
    0xFE, 0x31, 0xC9, 0x45, 0x85, 0xE4, 0x0F, 0x88, 0xF2, 0x00, 0x00, 0x00, 0x4C, 0x8D, 0x3D, 0x4D,
};
constexpr u32 RowEquipByte = 0x44; // EquipParamGoods: bit 6 isEquip (quick-slot equippable)
// WeaponParamRow, `void(Out*, s32 weapon)` (what AddItem and the shop's room call): the
// EquipParamWeapon row of the id (upgrade level stripped) and its ReinforceParamWeapon row (that
// level). Pure lookups. A give_kit id without both rows is refused (AddItem would create it).
constexpr u64 WeaponRowRva = 0x1F29AA0;
constexpr std::array<u8, 32> WeaponRowSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50, 0x41, 0x89,
    0xF4, 0x49, 0x89, 0xFE, 0x45, 0x31, 0xC9, 0x41, 0xBF, 0xFF, 0xFF, 0xFF, 0xFF, 0x41, 0xBA, 0xFF,
};

// PlayerIns.
constexpr u32 OffsetChrModules = 0x3B0; // +0x20 = ChrDataModule (HP +0xF8)
constexpr u32 OffsetPlayerGameData = 0x3C0;
constexpr u32 OffsetChrAsm = 0x4B8; // the equipment mirror the model follows
constexpr u32 OffsetHp = 0xF8;
// PlayerGameData -> EquipGameData.
constexpr u32 OffsetEgd = 0x1D0;
// EquipGameData.
constexpr u32 OffsetSlotIndex = 0x24;   // + 4s: inventory index of equip slot s (0..14)
constexpr u32 OffsetActiveLeft = 0x7C;  // active left weapon slot (0 or 1: slot 2x)
constexpr u32 OffsetActiveRight = 0x80; // active right weapon slot (0 or 1: slot 2x+1)
constexpr u32 OffsetSlotId = 0xD4;      // + 4s: item id of equip slot s
constexpr u32 OffsetMirror = 0x130;     // SetEquip's mirror (null in the test runs; logging only)
constexpr u32 OffsetInventory = 0x158;  // = PlayerGameData+0x328
constexpr u32 OffsetQuick = 0x21C;      // + 8k: quick slot k's inventory index (-1 empty), k 0..5
constexpr u32 OffsetQuick2 = 0x250;     // + 8j: the second 6-pair array MenuEquip also clears
constexpr u32 OffsetQuickSelected = 0x248;
constexpr u32 OffsetAddStatus = 0x29C;
// The player's equipment data [player+0x4B8] (what the model follows; the game copies the slots
// into it each frame, seen 3 s after the kit) and SetEquip's mirror: active left +0x0C, right
// +0x10, ids +0x64 + 4s.
constexpr u32 OffsetMirrorActiveLeft = 0x0C;
constexpr u32 OffsetMirrorActiveRight = 0x10;
constexpr u32 OffsetMirrorId = 0x64;
// Inventory: 16-byte entries {handle (0 = gone), item, qty, sort}; key items first.
constexpr u32 OffsetInvCapacity = 0x10;
constexpr u32 OffsetInvKeyCap = 0x24;
constexpr u32 OffsetInvNormalList = 0x48;
constexpr u32 OffsetInvKeyList = 0x58;
constexpr u32 OffsetInvHighest = 0x88;

constexpr u32 CategoryMask = 0xF0000000u;
constexpr u32 CategoryGoods = 0x40000000u; // weapons are category 0
constexpr s32 BareFists = 1000000;
constexpr s32 WeaponSlots = 15;
constexpr s32 QuickSlots = 6;
constexpr s32 QuickSlotBase = 16;  // MenuEquip slot of quick slot 0
constexpr s32 MaxGoodsId = 999999; // larger ids in a give_kit list are weapons
constexpr auto CheckDelay = std::chrono::seconds(3);

using AddItemFn = s32(PS4_SYSV_ABI*)(u8* egd, u32 category, s32 id, s32 qty, s32 unused,
                                     s32 trophy_notify, s32 no_auto_quick);
using RoomFn = s32(PS4_SYSV_ABI*)(const u32* item);
using FindFn = s32(PS4_SYSV_ABI*)(u8* inventory, const u32* item);
using MenuEquipFn = void(PS4_SYSV_ABI*)(s32 slot, const void* ref);
struct GoodsRowOut {
    s32 id{-1};
    u32 pad{0};
    u8* row{nullptr};
};
using GoodsRowFn = void(PS4_SYSV_ABI*)(GoodsRowOut* out, s32 goods);
struct WeaponRowOut {
    s32 id{-1};
    u32 pad0{0};
    u8* row{nullptr};
    s32 base{-1};
    u32 pad1{0};
    s32 reinforce_id{-1};
    u32 pad2{0};
    u8* reinforce{nullptr};
};
static_assert(sizeof(WeaponRowOut) == 0x28);
using WeaponRowFn = void(PS4_SYSV_ABI*)(WeaponRowOut* out, s32 weapon);

struct KitItem {
    u32 item; // goods: 0x40000000 | id; weapon: its id (category 0)
    s32 qty;  // goods: the top-up target; weapons: 1
};

// The default kit (goods top-up targets, then one of each weapon).
constexpr std::array<KitItem, 18> DefaultKit = {{
    {CategoryGoods | 1200, 10}, // Molotov Cocktail
    {CategoryGoods | 1240, 20}, // Throwing Knife
    {CategoryGoods | 1230, 5},  // Pebble
    {CategoryGoods | 1220, 5},  // Oil Urn
    {CategoryGoods | 1270, 3},  // Numbing Mist
    {CategoryGoods | 1100, 3},  // Antidote
    {CategoryGoods | 1101, 3},  // Sedative
    {CategoryGoods | 1300, 3},  // Fire Paper
    {CategoryGoods | 1320, 3},  // Bolt Paper
    {CategoryGoods | 1510, 3},  // Coldblood Dew (1)
    {CategoryGoods | 2090, 3},  // Shaman Bone Blade (held to use, its stab shown flat)
    {CategoryGoods | 1000, 20}, // Blood Vial
    {CategoryGoods | 900, 20},  // Quicksilver Bullets
    {7000000, 1},               // Saw Cleaver
    {5000000, 1},               // Hunter Axe
    {22000000, 1},              // Threaded Cane
    {14000000, 1},              // Hunter Pistol
    {6000000, 1},               // Hunter Blunderbuss
}};

// give_kit_equip: each hand's weapon slots that hold Bare Fists get these weapons, the ACTIVE slot
// (the weapon in the hand) first: right hand the Saw Cleaver, then the Hunter Axe as its second
// weapon; left hand the Hunter Pistol, then the Blunderbuss. The empty quick slots get these goods,
// in this order. The Blood Vial cannot go in a quick slot (its isEquip is 0: the Equipment menu
// never offers it; it has its own button): it is skipped with a log line and Fire Paper is the
// sixth.
constexpr std::array<s32, 2> RightWeapons = {7000000, 5000000}; // Saw Cleaver, Hunter Axe
constexpr std::array<s32, 2> LeftWeapons = {14000000, 6000000}; // Hunter Pistol, Blunderbuss
constexpr std::array<s32, 7> QuickCandidates = {1200, 1240, 1230, 1100, 1000, 1270, 1300};

const char* GoodsName(s32 id) {
    switch (id) {
    case 100:
        return "Hunter's Mark";
    case 900:
        return "Quicksilver Bullets";
    case 1000:
        return "Blood Vial";
    case 1100:
        return "Antidote";
    case 1101:
        return "Sedative";
    case 1110:
        return "Beast Blood Pellet";
    case 1120:
        return "Blue Elixir";
    case 1200:
        return "Molotov Cocktail";
    case 1210:
        return "Poison Knife";
    case 1220:
        return "Oil Urn";
    case 1230:
        return "Pebble";
    case 1240:
        return "Throwing Knife";
    case 1250:
        return "Rope Molotov Cocktail";
    case 1260:
        return "Pungent Blood Cocktail";
    case 1270:
        return "Numbing Mist";
    case 1300:
        return "Fire Paper";
    case 1320:
        return "Bolt Paper";
    case 1330:
        return "Bone Marrow Ash";
    case 1400:
        return "Bold Hunter's Mark";
    case 1410:
        return "Shining Coins";
    case 1411:
        return "Monocular";
    case 1412:
        return "Hand Lantern";
    case 1500:
        return "Madman's Knowledge";
    case 1510:
        return "Coldblood Dew (1)";
    case 2030:
        return "Lead Elixir";
    case 2090:
        return "Shaman Bone Blade";
    default:
        return nullptr;
    }
}

std::string ItemName(u32 item) {
    if (item == 0xFFFFFFFFu || item == 0x0FFFFFFFu) {
        return "none";
    }
    const s32 id = static_cast<s32>(item & ~CategoryMask);
    if ((item & CategoryMask) == CategoryGoods) {
        const char* name = GoodsName(id);
        return name != nullptr ? std::string{name} : fmt::format("goods {}", id);
    }
    if ((item & CategoryMask) != 0) {
        return fmt::format("item {:#x}", item);
    }
    const auto* family = Body::Weapons::FindFamily(Body::Weapons::FamilyOf(id));
    const s32 level = (id % 10000) / 100;
    if (family == nullptr) {
        return fmt::format("weapon {}", id);
    }
    return level > 0 ? fmt::format("{} +{}", family->name, level) : std::string{family->name};
}

const char* SlotName(s32 slot) {
    static constexpr std::array<const char*, 4> Names = {
        "L1 (left weapon 1)", "R1 (right weapon 1)", "L2 (left weapon 2)", "R2 (right weapon 2)"};
    return slot >= 0 && slot < 4 ? Names[static_cast<size_t>(slot)] : "?";
}

enum class Phase { Init, Waiting, Check, Done, Off };

struct State {
    Phase phase{Phase::Init};
    std::string spec;
    std::vector<KitItem> items;
    bool equip{true};
    bool equip_weapons{true}; // VR_GIVE_KIT_EQUIP=quick: the empty quick slots only
    u8* base{nullptr};
    AddItemFn add{nullptr};
    RoomFn room{nullptr};
    FindFn find{nullptr};
    MenuEquipFn menu_equip{nullptr};
    GoodsRowFn goods_row{nullptr};
    WeaponRowFn weapon_row{nullptr};
    u64 updates{0};
    const char* last_wait{nullptr};
    std::chrono::steady_clock::time_point applied_at{};
};
State g;

std::string Trim(std::string text) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
}

void AddKitItem(std::vector<KitItem>& items, KitItem item) {
    for (KitItem& existing : items) {
        if (existing.item == item.item) {
            existing.qty = item.qty;
            return;
        }
    }
    items.push_back(item);
}

// "0" / "" = off; "1" / "kit" = the default kit; else "<id>[:<qty>],..." (may include "kit").
std::vector<KitItem> ParseKit(const std::string& raw, std::string& error) {
    std::string spec = Trim(raw);
    std::transform(spec.begin(), spec.end(), spec.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::vector<KitItem> items;
    if (spec.empty() || spec == "0" || spec == "off" || spec == "false" || spec == "no") {
        return items;
    }
    if (spec == "1" || spec == "on" || spec == "true" || spec == "yes") {
        spec = "kit";
    }
    for (char& c : spec) {
        if (c == ';' || std::isspace(static_cast<unsigned char>(c))) {
            c = ',';
        }
    }
    size_t at = 0;
    while (at <= spec.size()) {
        const size_t end = std::min(spec.find(',', at), spec.size());
        const std::string token = spec.substr(at, end - at);
        at = end + 1;
        if (token.empty()) {
            continue;
        }
        if (token == "kit") {
            for (const KitItem& item : DefaultKit) {
                AddKitItem(items, item);
            }
            continue;
        }
        char* rest = nullptr;
        const long id = std::strtol(token.c_str(), &rest, 10);
        long qty = 1;
        if (rest != nullptr && *rest == ':') {
            qty = std::strtol(rest + 1, &rest, 10);
        }
        if (id <= 0 || id > 0x0FFFFFFF || qty <= 0 || rest == nullptr || *rest != '\0') {
            error += fmt::format("{}'{}'", error.empty() ? "" : ", ", token);
            continue;
        }
        if (id > MaxGoodsId) {
            AddKitItem(items, {static_cast<u32>(id), 1});
        } else {
            AddKitItem(items, {CategoryGoods | static_cast<u32>(id),
                               static_cast<s32>(std::min(qty, 999L))});
        }
    }
    return items;
}

bool EnvFlag(const char* name, bool fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return fallback;
    }
    const std::string v = Trim(value);
    return !(v.empty() || v == "0" || v == "off" || v == "false" || v == "no");
}

void Init(u8* module) {
    g.phase = Phase::Off;
    const auto& settings = VR::GetSettings();
    const char* env = std::getenv("VR_GIVE_KIT");
    g.spec = env != nullptr ? std::string{env} : settings.give_kit;
    std::string error;
    g.items = ParseKit(g.spec, error);
    if (!error.empty()) {
        LOG_WARNING(Core, "VR kit: give_kit '{}': ignored entries {}", g.spec, error);
    }
    if (g.items.empty()) {
        return; // off (the default): nothing logged
    }
    g.equip = EnvFlag("VR_GIVE_KIT_EQUIP", settings.give_kit_equip);
    const char* equip_env = std::getenv("VR_GIVE_KIT_EQUIP");
    g.equip_weapons = !(equip_env != nullptr && Trim(equip_env) == "quick");
    const auto matches = [module](u64 rva, const auto& signature) {
        return std::memcmp(module + rva, signature.data(), signature.size()) == 0;
    };
    const bool add_ok = matches(AddItemRva, AddItemSig);
    const bool room_ok = matches(RoomRva, RoomSig) && matches(RoomLeaRva, RoomLeaSig);
    const bool find_ok = matches(FindRva, FindSig);
    const bool equip_ok =
        matches(MenuEquipRva, MenuEquipSig) && matches(MenuManMovRva, MenuManMovSig);
    const bool row_ok = matches(GoodsRowRva, GoodsRowSig) && matches(WeaponRowRva, WeaponRowSig);
    if (!add_ok || !room_ok || !find_ok || !equip_ok || !row_ok) {
        LOG_WARNING(Core,
                    "VR kit: TEST KIT NOT APPLIED: signature mismatch (additem {} room {} find {} "
                    "menuequip {} param rows {}); not Bloodborne 1.09?",
                    add_ok ? "ok" : "MISMATCH", room_ok ? "ok" : "MISMATCH",
                    find_ok ? "ok" : "MISMATCH", equip_ok ? "ok" : "MISMATCH",
                    row_ok ? "ok" : "MISMATCH");
        return;
    }
    g.weapon_row = reinterpret_cast<WeaponRowFn>(module + WeaponRowRva);
    g.base = module;
    g.add = reinterpret_cast<AddItemFn>(module + AddItemRva);
    g.room = reinterpret_cast<RoomFn>(module + RoomRva);
    g.find = reinterpret_cast<FindFn>(module + FindRva);
    g.menu_equip = reinterpret_cast<MenuEquipFn>(module + MenuEquipRva);
    g.goods_row = reinterpret_cast<GoodsRowFn>(module + GoodsRowRva);
    g.phase = Phase::Waiting;
    LOG_WARNING(Core,
                "VR kit: TEST/DEBUG KIT ON (give_kit '{}'{}: {} entries, equip {}): applied once, "
                "when the hunter is first in the world, through the game's own item and equip "
                "functions (the game saves it). Set give_kit=0 to stop.",
                g.spec, env != nullptr ? " from VR_GIVE_KIT" : "", g.items.size(),
                !g.equip ? "off" : g.equip_weapons ? "on" : "quick slots only");
}

// ---- The player's data -------------------------------------------------------------------------

u8* EntryOf(u8* egd, s32 index) {
    u8* const inv = egd + OffsetInventory;
    if (index < 0 || Read<u32>(inv + OffsetInvCapacity) == 0 ||
        static_cast<u32>(index) > Read<u32>(inv + OffsetInvHighest)) {
        return nullptr;
    }
    const u32 key_cap = Read<u32>(inv + OffsetInvKeyCap);
    const u32 i = static_cast<u32>(index);
    u8* const list = Read<u8*>(inv + (i < key_cap ? OffsetInvKeyList : OffsetInvNormalList));
    if (!Plausible(list)) {
        return nullptr;
    }
    return list + static_cast<size_t>(i < key_cap ? i : i - key_cap) * 16;
}

// The live entry's item (0 = none or gone).
u32 ItemAt(u8* egd, s32 index) {
    u8* const e = EntryOf(egd, index);
    return e != nullptr && Read<u32>(e) != 0 ? Read<u32>(e + 4) : 0;
}

s32 QtyAt(u8* egd, s32 index) {
    u8* const e = EntryOf(egd, index);
    return e != nullptr && Read<u32>(e) != 0 ? Read<s32>(e + 8) : 0;
}

s32 Find(u8* egd, u32 item) {
    return g.find(egd + OffsetInventory, &item);
}

// A weapon of `weapon`'s family (any upgrade, Uncanny / Lost) in the inventory: the exact id
// first. -1 = none.
s32 FindWeapon(u8* egd, s32 weapon) {
    const s32 exact = Find(egd, static_cast<u32>(weapon));
    if (exact >= 0) {
        return exact;
    }
    const s32 family = Body::Weapons::FamilyOf(weapon);
    const u32 highest = std::min(Read<u32>(egd + OffsetInventory + OffsetInvHighest), 4096u);
    for (u32 i = 0; i <= highest; ++i) {
        const u32 item = ItemAt(egd, static_cast<s32>(i));
        if (item != 0 && (item & CategoryMask) == 0 &&
            Body::Weapons::FamilyOf(static_cast<s32>(item)) == family) {
            return static_cast<s32>(i);
        }
    }
    return -1;
}

s32 WeaponSlotOf(u8* egd, s32 index) {
    for (s32 s = 0; s < WeaponSlots; ++s) {
        if (Read<s32>(egd + OffsetSlotIndex + 4 * s) == index) {
            return s;
        }
    }
    return -1;
}

// Quick slot (0..5, or 6..11 for the second array) that holds `index`, else -1.
s32 QuickSlotOf(u8* egd, s32 index) {
    for (s32 k = 0; k < QuickSlots; ++k) {
        if (Read<s32>(egd + OffsetQuick + 8 * k) == index) {
            return k;
        }
        if (Read<s32>(egd + OffsetQuick2 + 8 * k) == index) {
            return QuickSlots + k;
        }
    }
    return -1;
}

std::string WeaponSlots4(u8* egd) {
    return fmt::format(
        "L1 {} | R1 {} | L2 {} | R2 {} (active L{} R{})", ItemName(Read<u32>(egd + OffsetSlotId)),
        ItemName(Read<u32>(egd + OffsetSlotId + 4)), ItemName(Read<u32>(egd + OffsetSlotId + 8)),
        ItemName(Read<u32>(egd + OffsetSlotId + 12)), Read<s32>(egd + OffsetActiveLeft) + 1,
        Read<s32>(egd + OffsetActiveRight) + 1);
}

std::string QuickSlotsText(u8* egd) {
    std::string text;
    for (s32 k = 0; k < QuickSlots; ++k) {
        const s32 index = Read<s32>(egd + OffsetQuick + 8 * k);
        const u32 item = ItemAt(egd, index);
        text += fmt::format("{}{}:{}", k == 0 ? "" : " ", k + 1,
                            index < 0   ? std::string{"-"}
                            : item == 0 ? fmt::format("(gone idx {})", index)
                                        : fmt::format("{} x{}", ItemName(item), QtyAt(egd, index)));
    }
    const s32 selected = Read<s32>(egd + OffsetQuickSelected);
    text += fmt::format(" | selected {}",
                        selected < 0 ? std::string{"none"} : ItemName(ItemAt(egd, selected)));
    return text;
}

void MenuEquip(s32 slot, s32 index, u32 item) {
    alignas(16) std::array<u8, 0x50> ref{};
    std::memcpy(ref.data() + 0x48, &index, sizeof(index));
    std::memcpy(ref.data() + 0x4C, &item, sizeof(item));
    g.menu_equip(slot, ref.data());
}

// ---- The kit -----------------------------------------------------------------------------------

void TopUpGoods(u8* egd, const KitItem& kit) {
    const std::string name = ItemName(kit.item);
    const s32 id = static_cast<s32>(kit.item & ~CategoryMask);
    const s32 before = QtyAt(egd, Find(egd, kit.item));
    if (before >= kit.qty) {
        LOG_INFO(Core, "VR kit: = {} ({}): {} (target {}), nothing given", name, id, before,
                 kit.qty);
        return;
    }
    const s32 room = std::max(g.room(&kit.item), 0);
    const s32 give = std::min(kit.qty - before, room);
    if (give <= 0) {
        LOG_INFO(Core,
                 "VR kit: ! {} ({}): {} (target {}), no room (its maximum, or the inventory "
                 "is full)",
                 name, id, before, kit.qty);
        return;
    }
    const s32 result = g.add(egd, kit.item & CategoryMask, id, give, 1, 0, 1);
    const s32 status = Read<s32>(egd + OffsetAddStatus);
    const s32 index = Find(egd, kit.item);
    const s32 after = QtyAt(egd, index);
    LOG_INFO(Core, "VR kit: + {} ({}): {} -> {} (target {}, gave {}, room {}) inventory index {}{}",
             name, id, before, after, kit.qty, give, room, index,
             result < 0 || after != before + give
                 ? fmt::format(" (AddItem returned {}, status {}: UNEXPECTED)", result, status)
                 : std::string{});
}

void GiveWeapon(u8* egd, const KitItem& kit) {
    const s32 weapon = static_cast<s32>(kit.item);
    const std::string name = ItemName(kit.item);
    const s32 owned = FindWeapon(egd, weapon);
    if (owned >= 0) {
        LOG_INFO(Core, "VR kit: = {} ({}): owned ({}, inventory index {}), nothing given", name,
                 weapon, ItemName(ItemAt(egd, owned)), owned);
        return;
    }
    const s32 room = g.room(&kit.item);
    if (room < 1) {
        LOG_INFO(Core, "VR kit: ! {} ({}): no free inventory entry", name, weapon);
        return;
    }
    const s32 result = g.add(egd, 0, weapon, 1, 1, 0, 1);
    const s32 status = Read<s32>(egd + OffsetAddStatus);
    const s32 index = Find(egd, kit.item);
    u8* const entry = EntryOf(egd, index);
    LOG_INFO(Core, "VR kit: + {} ({}): 0 -> {} inventory index {} handle {:#x}{}", name, weapon,
             index >= 0 ? 1 : 0, index, entry != nullptr ? Read<u32>(entry) : 0u,
             result < 0 || index < 0
                 ? fmt::format(" (AddItem returned {}, status {}: UNEXPECTED)", result, status)
                 : std::string{});
}

// One hand: its slots `hand` (0 = left: slots 0 / 2, 1 = right: 1 / 3), the active one first; each
// that holds Bare Fists gets the next of `weapons` that is owned and not equipped anywhere.
void EquipHand(u8* egd, s32 hand, const std::array<s32, 2>& weapons) {
    const s32 active =
        Read<s32>(egd + (hand == 0 ? OffsetActiveLeft : OffsetActiveRight)) == 1 ? 1 : 0;
    const std::array<s32, 2> slots = {2 * active + hand, 2 * (1 - active) + hand};
    size_t next = 0;
    for (const s32 slot : slots) {
        const s32 current = Read<s32>(egd + OffsetSlotId + 4 * slot);
        if (current != BareFists && current != -1 && current != 0x0FFFFFFF) {
            LOG_INFO(Core, "VR kit: = slot {}{} holds {}: kept", SlotName(slot),
                     slot == slots[0] ? " (in the hand)" : "", ItemName(static_cast<u32>(current)));
            continue;
        }
        while (next < weapons.size()) {
            const s32 weapon = weapons[next++];
            const s32 index = FindWeapon(egd, weapon);
            if (index < 0) {
                LOG_INFO(Core, "VR kit: ! {} not owned, not equipped",
                         ItemName(static_cast<u32>(weapon)));
                continue;
            }
            const u32 item = ItemAt(egd, index);
            if (const s32 other = WeaponSlotOf(egd, index); other >= 0) {
                LOG_INFO(Core, "VR kit: = {} already equipped in slot {}, kept", ItemName(item),
                         SlotName(other));
                continue;
            }
            MenuEquip(slot, index, item);
            const s32 now = Read<s32>(egd + OffsetSlotId + 4 * slot);
            LOG_INFO(Core, "VR kit: equip slot {}{}: {} -> {} (inventory index {}){}",
                     SlotName(slot), slot == slots[0] ? " (in the hand)" : "",
                     ItemName(static_cast<u32>(current)), ItemName(static_cast<u32>(now)), index,
                     static_cast<u32>(now) == item ? "" : " UNEXPECTED: the slot did not change");
            break;
        }
    }
}

void EquipWeapons(u8* egd) {
    EquipHand(egd, 1, RightWeapons);
    EquipHand(egd, 0, LeftWeapons);
}

void FillQuickSlots(u8* egd) {
    for (const s32 goods : QuickCandidates) {
        s32 empty = -1;
        for (s32 k = 0; k < QuickSlots && empty < 0; ++k) {
            if (Read<s32>(egd + OffsetQuick + 8 * k) == -1) {
                empty = k;
            }
        }
        const u32 item = CategoryGoods | static_cast<u32>(goods);
        const std::string name = ItemName(item);
        if (empty < 0) {
            LOG_INFO(Core, "VR kit: quick slots full; {} and the rest not placed", name);
            return;
        }
        const s32 index = Find(egd, item);
        if (index < 0 || QtyAt(egd, index) <= 0) {
            LOG_INFO(Core, "VR kit: ! quick slot: {} not owned, skipped", name);
            continue;
        }
        GoodsRowOut out{};
        g.goods_row(&out, goods);
        if (Plausible(out.row, 1) && (Read<u8>(out.row + RowEquipByte) & 0x40) == 0) {
            LOG_INFO(Core,
                     "VR kit: ! quick slot: {} is not quick-slot equippable (isEquip 0: the "
                     "Equipment menu never offers it), skipped",
                     name);
            continue;
        }
        if (const s32 slot = QuickSlotOf(egd, index); slot >= 0) {
            LOG_INFO(Core, "VR kit: = quick slot {}: {} already there, kept", slot + 1, name);
            continue;
        }
        MenuEquip(QuickSlotBase + empty, index, item);
        const s32 now = Read<s32>(egd + OffsetQuick + 8 * empty);
        LOG_INFO(Core, "VR kit: equip quick slot {}: empty -> {} x{} (inventory index {}){}",
                 empty + 1, name, QtyAt(egd, index), index,
                 now == index ? "" : " UNEXPECTED: the slot did not change");
    }
}

// VR_KIT_QUICK=<goods>,... (tests only, with the kit on): quick slots 1.. get exactly these owned
// goods through the Equipment menu's own equip action (the slot's item is replaced; a slot
// already holding it is kept), so a held-item test reaches items the kit save keeps out of its six
// slots (Oil Urn, Sedative, Bolt Paper, Coldblood Dew).
void SetQuickSlots(u8* egd) {
    const char* const env = std::getenv("VR_KIT_QUICK");
    if (env == nullptr || env[0] == 0) {
        return;
    }
    const std::string list{env};
    size_t at = 0;
    for (s32 k = 0; k < QuickSlots && at < list.size(); ++k) {
        const size_t end = std::min(list.find(',', at), list.size());
        const s32 goods = std::atoi(list.substr(at, end - at).c_str());
        at = end + 1;
        const u32 item = CategoryGoods | static_cast<u32>(goods);
        const std::string name = ItemName(item);
        const s32 index = Find(egd, item);
        if (goods <= 0 || index < 0 || QtyAt(egd, index) <= 0) {
            LOG_INFO(Core, "VR kit: ! quick slot {}: {} ({}) not owned, kept as it is", k + 1, name,
                     goods);
            continue;
        }
        if (Read<s32>(egd + OffsetQuick + 8 * k) == index) {
            continue;
        }
        if (const s32 other = QuickSlotOf(egd, index); other >= 0 && other < QuickSlots) {
            MenuEquip(QuickSlotBase + other, index, item); // the toggle: out of its old slot
        }
        MenuEquip(QuickSlotBase + k, index, item);
        LOG_INFO(Core, "VR kit: TEST quick slot {} -> {} (inventory index {}){}", k + 1, name,
                 index,
                 Read<s32>(egd + OffsetQuick + 8 * k) == index ? "" : " UNEXPECTED: unchanged");
    }
}

// VR_KIT_RIGHT=<weapon>[,<weapon>] (tests only, with the kit on): the right hand's slots get
// exactly these owned weapons (any upgrade level of the id's family), the slot in the hand first,
// through the Equipment menu's own equip action (the slot's weapon is replaced; a slot already
// holding it is kept), e.g. the Threaded Cane in the hand for a transform test (the kit save keeps
// it in the inventory). Give a weapon the kit lacks with VR_GIVE_KIT=kit,<id>.
void SetRightWeapons(u8* egd) {
    const char* const env = std::getenv("VR_KIT_RIGHT");
    if (env == nullptr || env[0] == 0) {
        return;
    }
    const std::string list{env};
    const s32 active = Read<s32>(egd + OffsetActiveRight) == 1 ? 1 : 0;
    const std::array<s32, 2> slots = {2 * active + 1, 2 * (1 - active) + 1};
    size_t at = 0;
    for (const s32 slot : slots) {
        if (at >= list.size()) {
            break;
        }
        const size_t end = std::min(list.find(',', at), list.size());
        const s32 weapon = std::atoi(list.substr(at, end - at).c_str());
        at = end + 1;
        const s32 index = weapon > 0 ? FindWeapon(egd, weapon) : -1;
        if (index < 0) {
            LOG_INFO(Core, "VR kit: ! right slot {}: {} not owned, kept as it is", SlotName(slot),
                     weapon);
            continue;
        }
        const u32 item = ItemAt(egd, index);
        if (Read<s32>(egd + OffsetSlotIndex + 4 * slot) == index) {
            continue;
        }
        if (const s32 other = WeaponSlotOf(egd, index); other >= 0) {
            MenuEquip(other, index, item); // the toggle: out of its old slot
        }
        MenuEquip(slot, index, item);
        LOG_INFO(Core, "VR kit: TEST right slot {}{} -> {} (inventory index {}){}", SlotName(slot),
                 slot == slots[0] ? " (in the hand)" : "", ItemName(item), index,
                 Read<s32>(egd + OffsetSlotIndex + 4 * slot) == index ? ""
                                                                      : " UNEXPECTED: unchanged");
    }
}

// VR_KIT_ATTIRE=<protector id>[,...] (tests only, with the kit on; the wrist gauge's fit on every
// sleeve): every listed armour piece is given (AddItem, category 0x10000000: EquipParamProtector
// ids, a set's head / body / arms / legs at +0 / +1000 / +2000 / +3000, e.g. 72000 Hunter Gloves,
// 42000 Bone Ash Gauntlets, 152000 Executioner Gauntlets) and equipped in turn, one every
// VR_KIT_ATTIRE_S seconds (8), looping, into the equip slot that holds that body part now, through
// the Equipment menu's own equip action. Each change is logged ("VR kit: ATTIRE ...").
constexpr u32 CategoryProtector = 0x10000000u;
struct AttireCycle {
    bool init{false};
    std::vector<std::vector<s32>> groups; // each step's pieces ("71000+72000": a garb and gloves)
    size_t next{0};
    double period_s{8.0};
    std::chrono::steady_clock::time_point last{};
};
AttireCycle g_attire{};

// The equip slot of protector `part` (0 head, 1 body, 2 arms, 3 legs): slots 8 .. 11 (the slots
// hold the raw EquipParamProtector id, no category: the kit save's 230000 .. 233000, the Foreign
// set), checked against the id there; -1 = not as expected.
constexpr s32 FirstProtectorSlot = 8;
s32 ProtectorSlot(u8* egd, s32 part) {
    const s32 slot = FirstProtectorSlot + part;
    const u32 id = Read<u32>(egd + OffsetSlotId + 4 * slot) & ~CategoryMask;
    if (part < 0 || part > 3 || id == 0x0FFFFFFFu || id < 1000 ||
        (static_cast<s32>(id) % 10000) / 1000 != part) {
        return -1;
    }
    return slot;
}

std::string SlotIds(u8* egd) {
    std::string text;
    for (s32 s = 0; s < WeaponSlots; ++s) {
        text += fmt::format("{}{}:{:#x}", s == 0 ? "" : " ", s, Read<u32>(egd + OffsetSlotId + 4 * s));
    }
    return text;
}

void EquipProtector(u8* egd, s32 id, size_t step) {
    const s32 part = (id % 10000) / 1000;
    const u32 item = CategoryProtector | static_cast<u32>(id);
    s32 index = Find(egd, item);
    if (index < 0) {
        const s32 result = g.add(egd, CategoryProtector, id, 1, 1, 0, 1);
        index = Find(egd, item);
        LOG_INFO(Core, "VR kit: ATTIRE give protector {} -> result {} status {} index {}", id,
                 result, Read<s32>(egd + OffsetAddStatus), index);
    }
    const s32 slot = ProtectorSlot(egd, part);
    if (index < 0 || slot < 0) {
        LOG_WARNING(Core, "VR kit: ATTIRE ! protector {} (part {}): index {} slot {}; slots {}", id,
                    part, index, slot, SlotIds(egd));
        return;
    }
    if (Read<s32>(egd + OffsetSlotIndex + 4 * slot) != index) {
        MenuEquip(slot, index, item);
    }
    LOG_INFO(Core, "VR kit: ATTIRE #{} protector {} (part {}) into slot {}: now {:#x}{}", step, id,
             part, slot, Read<u32>(egd + OffsetSlotId + 4 * slot),
             Read<s32>(egd + OffsetSlotIndex + 4 * slot) == index ? "" : " UNEXPECTED: unchanged");
}

void AttireTick(u8* player) {
    auto& a = g_attire;
    if (!a.init) {
        a.init = true;
        const char* const env = std::getenv("VR_KIT_ATTIRE");
        if (env == nullptr || env[0] == 0) {
            return;
        }
        const std::string list{env};
        size_t at = 0;
        while (at <= list.size()) {
            const size_t end = std::min(list.find(',', at), list.size());
            const std::string token = list.substr(at, end - at);
            std::vector<s32> group;
            size_t p = 0;
            while (p <= token.size()) {
                const size_t q = std::min(token.find('+', p), token.size());
                if (const s32 id = std::atoi(token.substr(p, q - p).c_str()); id > 0) {
                    group.push_back(id);
                }
                p = q + 1;
            }
            if (!group.empty()) {
                a.groups.push_back(group);
            }
            at = end + 1;
        }
        if (const char* period = std::getenv("VR_KIT_ATTIRE_S"); period != nullptr) {
            a.period_s = std::clamp(std::atof(period), 1.0, 120.0);
        }
        LOG_WARNING(Core, "VR kit: ATTIRE test cycle: {} steps, every {:.0f} s", a.groups.size(),
                    a.period_s);
    }
    if (a.groups.empty()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (a.next > 0 && std::chrono::duration<double>(now - a.last).count() < a.period_s) {
        return;
    }
    u8* const pgd = Read<u8*>(player + OffsetPlayerGameData);
    if (!Plausible(pgd) || VR::IsGameMenuOpen() || !Plausible(Read<u8*>(g.base + MenuManRva))) {
        return;
    }
    u8* const egd = pgd + OffsetEgd;
    a.last = now;
    const auto& group = a.groups[a.next % a.groups.size()];
    ++a.next;
    for (const s32 id : group) {
        EquipProtector(egd, id, a.next);
    }
}

// Why the kit must wait, or nullptr when it can run now.
const char* WaitReason(u8* module, u8* player, u8*& egd) {
    u8* const pgd = Read<u8*>(player + OffsetPlayerGameData);
    if (!Plausible(pgd)) {
        return "no player game data";
    }
    u8* const game_data = Read<u8*>(module + GameDataManRva);
    if (!Plausible(game_data) || Read<u8*>(game_data + 8) != pgd) {
        return "the player's game data is not GameDataMan's";
    }
    egd = pgd + OffsetEgd;
    if (Read<u32>(egd + OffsetInventory + OffsetInvCapacity) == 0) {
        return "inventory not ready";
    }
    u8* const modules = Read<u8*>(player + OffsetChrModules);
    u8* const data = Plausible(modules) ? Read<u8*>(modules + 0x20) : nullptr;
    if (!Plausible(data) || Read<s32>(data + OffsetHp) <= 0) {
        return "the hunter is not alive";
    }
    if (VR::IsGameMenuOpen()) {
        return "a game menu is open";
    }
    if (!Plausible(Read<u8*>(module + MenuManRva))) {
        return "no MenuMan";
    }
    return nullptr;
}

// Explicit one-shot test-save grant, independent of give_kit. The launcher backs up the save
// before setting VR_MAX_STATS=1. Run on the main player's update, as the inventory kit does.
void MaxStatsTick(u8* module, u8* player) {
    namespace S = VR::TestStats;
    static bool done = false;
    static const bool enabled = [] {
        const char* env = std::getenv("VR_MAX_STATS");
        return env != nullptr && std::strcmp(env, "1") == 0;
    }();
    if (!enabled || done || !Plausible(module) || !Plausible(player)) {
        return;
    }
    constexpr u64 apply_rva = 0x1C13D00;
    // Entry, GameDataMan access, every attribute/level store, and WorldChrMan access.
    // These bytes also verify the request and PlayerGameData field offsets.
    constexpr std::array<u8, 104> signature = {
        0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83,
        0xEC, 0x28, 0x4C, 0x8B, 0x35, 0xC8, 0xFE, 0x7C, 0x03, 0x49, 0x8B, 0x06, 0x48, 0x89, 0x45,
        0xD0, 0x48, 0x8D, 0x05, 0x0A, 0x74, 0x92, 0x03, 0x48, 0x8B, 0x00, 0x4C, 0x8B, 0x68, 0x08,
        0x8B, 0x47, 0x0C, 0x41, 0x89, 0x45, 0x40, 0x8B, 0x47, 0x10, 0x41, 0x89, 0x45, 0x48, 0x8B,
        0x47, 0x14, 0x41, 0x89, 0x45, 0x58, 0x8B, 0x47, 0x18, 0x41, 0x89, 0x45, 0x60, 0x8B, 0x47,
        0x1C, 0x41, 0x89, 0x45, 0x68, 0x8B, 0x47, 0x20, 0x41, 0x89, 0x45, 0x70, 0x8B, 0x47, 0x08,
        0x41, 0x89, 0x85, 0x90, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x1D, 0x10, 0xAB, 0x92, 0x03,
    };
    static bool checked = false;
    if (!checked) {
        checked = true;
        if (std::memcmp(module + apply_rva, signature.data(), signature.size()) != 0) {
            done = true;
            LOG_ERROR(Core, "VR max stats: refused; level-up routine signature mismatch");
            return;
        }
        LOG_WARNING(Core, "VR max stats: TEST SAVE grant armed; waiting for a living hunter");
    }
    u8* egd = nullptr;
    if (WaitReason(module, player, egd) != nullptr) {
        return;
    }
    u8* const world = Read<u8*>(module + 0x553E878);
    if (!Plausible(world) || Read<u8*>(world + 0x60) != player) {
        return;
    }
    u8* const modules = Read<u8*>(player + OffsetChrModules);
    u8* const data = Read<u8*>(modules + 0x20); // checked by WaitReason
    if (!Plausible(Read<u8*>(modules + 0x28)) || Read<u8*>(data + 8) != player ||
        !Plausible(Read<u8*>(player + 0x1C8))) {
        return;
    }
    u8* const pgd = Read<u8*>(player + OffsetPlayerGameData);
    const auto read_stats = [pgd] {
        S::Stats values{};
        for (size_t i = 0; i < values.size(); ++i) {
            values[i] = Read<s32>(pgd + S::Offsets[i]);
        }
        return values;
    };
    const S::Stats before = read_stats();
    const s32 level = Read<s32>(pgd + S::LevelOffset);
    if (!S::Valid(before, level)) {
        done = true;
        LOG_ERROR(Core, "VR max stats: refused unexpected stats {}/{}/{}/{}/{}/{} level {}",
                  before[0], before[1], before[2], before[3], before[4], before[5], level);
        return;
    }
    done = true; // also prevents re-entry through the game's recalculation calls
    if (S::Maxed(before, level)) {
        LOG_INFO(Core, "VR max stats: already 99 in all six attributes, level 544");
        return;
    }
    LOG_WARNING(Core,
                "VR max stats: applying TEST grant, {}/{}/{}/{}/{}/{} level {} -> all 99, "
                "level 544 via the game's level-up handler",
                before[0], before[1], before[2], before[3], before[4], before[5], level);
    const S::Request request{};
    using ApplyStatsFn = void(PS4_SYSV_ABI*)(const S::Request*);
    reinterpret_cast<ApplyStatsFn>(module + apply_rva)(&request);
    const auto after = read_stats();
    if (S::Maxed(after, Read<s32>(pgd + S::LevelOffset))) {
        LOG_WARNING(Core,
                    "VR max stats: VERIFIED all six 99, level 544; HP {}/{}. "
                    "Use Quit Game to persist through the game's save system.",
                    Read<s32>(data + OffsetHp), Read<s32>(data + 0xFC));
    } else {
        LOG_ERROR(Core, "VR max stats: verification failed; no retry this session");
    }
}

void Apply(u8* player, u8* egd) {
    const u32 highest = Read<u32>(egd + OffsetInventory + OffsetInvHighest);
    LOG_WARNING(Core,
                "VR kit: applying the TEST KIT on thread '{}' (main-player update {}, inventory "
                "highest index {}); before: {}; quick {}",
                Common::GetCurrentThreadName(), g.updates, highest, WeaponSlots4(egd),
                QuickSlotsText(egd));
    for (const KitItem& kit : g.items) {
        const s32 id = static_cast<s32>(kit.item & ~CategoryMask);
        if ((kit.item & CategoryMask) == CategoryGoods) {
            GoodsRowOut row{};
            g.goods_row(&row, id);
            if (!Plausible(row.row, 1)) {
                LOG_WARNING(
                    Core, "VR kit: ! goods {}: no such item (no EquipParamGoods row), skipped", id);
                continue;
            }
            TopUpGoods(egd, kit);
        } else if ((kit.item & CategoryMask) == 0) {
            WeaponRowOut row{};
            g.weapon_row(&row, id);
            if (!Plausible(row.row, 1) || !Plausible(row.reinforce, 1)) {
                LOG_WARNING(Core,
                            "VR kit: ! weapon {}: no such weapon or upgrade level (param rows {} / "
                            "{}), skipped",
                            id, fmt::ptr(row.row), fmt::ptr(row.reinforce));
                continue;
            }
            GiveWeapon(egd, kit);
        }
    }
    if (g.equip) {
        if (g.equip_weapons) {
            EquipWeapons(egd);
        }
        FillQuickSlots(egd);
    }
    SetQuickSlots(egd);
    SetRightWeapons(egd);
    LOG_WARNING(Core, "VR kit: done; after: {}; quick {}; equip slots {}", WeaponSlots4(egd),
                QuickSlotsText(egd), SlotIds(egd));
    (void)player;
}

// A few seconds later: the slots, the player's mirror (what the model follows) and the quick
// slots, as the game now has them.
void Check(u8* player, u8* egd) {
    u8* const mirror = Read<u8*>(egd + OffsetMirror);
    u8* const chr_asm = Read<u8*>(player + OffsetChrAsm);
    const auto describe = [](const char* what, const u8* e) {
        if (!Plausible(e, 4)) {
            return fmt::format("{} {} (not readable)", what, fmt::ptr(e));
        }
        return fmt::format("{} {} (active L{} R{}): L1 {} R1 {} L2 {} R2 {}", what, fmt::ptr(e),
                           Read<s32>(e + OffsetMirrorActiveLeft) + 1,
                           Read<s32>(e + OffsetMirrorActiveRight) + 1,
                           Read<s32>(e + OffsetMirrorId), Read<s32>(e + OffsetMirrorId + 4),
                           Read<s32>(e + OffsetMirrorId + 8), Read<s32>(e + OffsetMirrorId + 12));
    };
    const std::string mirror_text =
        mirror == chr_asm
            ? describe("mirror [egd+0x130] == [player+0x4B8]", mirror)
            : describe("mirror [egd+0x130]", mirror) + "; " + describe("[player+0x4B8]", chr_asm);
    LOG_INFO(Core, "VR kit: check after {} s: {}; {}; quick {}",
             std::chrono::duration_cast<std::chrono::seconds>(CheckDelay).count(),
             WeaponSlots4(egd), mirror_text, QuickSlotsText(egd));
}

} // Anonymous namespace

void GiveKitSiteA(u8* module, u8* player) {
    MaxStatsTick(module, player);
    if (g.phase == Phase::Done && player != nullptr) {
        AttireTick(player);
        return;
    }
    if (g.phase == Phase::Done || g.phase == Phase::Off || module == nullptr || player == nullptr) {
        return;
    }
    if (g.phase == Phase::Init) {
        Init(module);
        if (g.phase != Phase::Waiting) {
            return;
        }
    }
    if (g.phase == Phase::Check) {
        u8* const pgd = Read<u8*>(player + OffsetPlayerGameData);
        if (std::chrono::steady_clock::now() - g.applied_at >= CheckDelay && Plausible(pgd)) {
            Check(player, pgd + OffsetEgd);
            g.phase = Phase::Done;
        }
        return;
    }
    ++g.updates;
    u8* egd = nullptr;
    const char* const wait = WaitReason(g.base, player, egd);
    if (wait != nullptr) {
        if (wait != g.last_wait) {
            g.last_wait = wait;
            LOG_INFO(Core, "VR kit: waiting ({})", wait);
        }
        return;
    }
    Apply(player, egd);
    g.applied_at = std::chrono::steady_clock::now();
    g.phase = Phase::Check;
}

} // namespace VR::Bloodborne
