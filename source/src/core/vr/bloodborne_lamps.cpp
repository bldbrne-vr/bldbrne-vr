// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include "common/logging/log.h"
#include "core/vr/bloodborne_lamps.h"
#include "core/vr/bloodborne_menu.h"
#include "core/vr/hook_util.h"
#include "core/vr/lamp_channel.h"
#include "core/vr/lamp_native.h"
#include "core/vr/lamp_sequence.h"
#include "core/vr/vr_session.h"
#include "core/vr/vial_refill.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace VR::Bloodborne {
namespace {
using Hook::Plausible;
using Hook::Read;
namespace P = VR::Lamps;
// Verified in the 1.09 executable.
constexpr u64 WorldChr = 0x553e878, GameData = 0x553b130, GameMan = 0x5556678;
constexpr u64 TalkMan = 0x5540508, MenuMan = 0x5562878, MenuSystem = 0x5562870;
constexpr u64 FlagMan = 0x553b100, ParamMan = 0x5540340;
constexpr u64 ObjectMan = 0x553b168; // the world's objects, by map block (as bloodborne_levers.cpp)
constexpr int LampMessage = 10010713;
using SetRespawnFn = void(PS4_SYSV_ABI*)(s32);
using AddEffectFn = u8(PS4_SYSV_ABI*)(void*, s32, void*, u32, u32, s32, float, float, float, float,
                                      float, s32);
using GetFlagFn = u32(PS4_SYSV_ABI*)(void*, u32);
using AddItemFn = s32(PS4_SYSV_ABI*)(u8*, u32, s32, s32, s32, s32, s32);
using RoomFn = s32(PS4_SYSV_ABI*)(const u32*);
using FindFn = s32(PS4_SYSV_ABI*)(u8*, const u32*);
using TransferFn = u8(PS4_SYSV_ABI*)(s32 index, u8* source, u8* destination, s32 quantity);
struct ReturnRow {
    u64 vtable;
    s32 id{-1};
    u32 pad{};
    u8* row{};
};
using RowFn = void(PS4_SYSV_ABI*)(ReturnRow*, s32);
u8* base{};
bool ready{};
u64 seen{}, offered_serial{};
u8* offered_player{};
u8* offered_dialog{};
P::Offer offer;
P::Request request;
enum class Phase { Idle, Offering, Responding, Warp };
P::Sequence sequence;
P::Pad last_pad{P::Pad::Neutral};
Phase phase{Phase::Idle};
s64 started{}, phase_start{}, last_receipt_ns{};
int default_choice{};
const char* refusal{};

template <class T> T Function(u64 rva) { return reinterpret_cast<T>(base + rva); }
u8* Global(u64 rva) { return Read<u8*>(base + rva); }
void Refuse(const char* reason) {
    VR::PublishLampBoxHidden(false); // the game's own box answers it
    if (reason != refusal) {
        LOG_WARNING(Core, "VR lamp: refused: {}", reason);
        refusal = reason;
    }
}
void Cancel(const char* reason) {
    VR::PublishLampBoxHidden(false);
    if (phase == Phase::Idle)
        return;
    LOG_INFO(Core, "VR lamp: cancelled serial {}: {}", offered_serial, reason);
    P::Mailbox().Cancel();
    phase = Phase::Idle;
    offered_player = nullptr;
}
bool Globals() {
    return Plausible(Global(WorldChr)) && Plausible(Global(GameData)) &&
           Plausible(Global(GameMan)) && Plausible(Global(TalkMan)) && Plausible(Global(MenuMan)) &&
           Plausible(Global(MenuSystem));
}
std::optional<bool> Flag(int id) {
    u8* man = Global(FlagMan);
    if (!Plausible(man) || !Read<u32>(man + 0x1c) || !Plausible(Read<u8*>(man + 0x38)))
        return {};
    return Function<GetFlagFn>(0x13cfb60)(man, id) != 0;
}
bool Row(int id, ReturnRow& out) {
    u8* params = Global(ParamMan);
    if (id < 0 || !Plausible(params) || !Read<u32>(params + 0xe80))
        return false;
    u8* table = Read<u8*>(params + 0xe88);
    if (!Plausible(table))
        return false;
    table = Read<u8*>(table + 0x70);
    if (!Plausible(table) || !Plausible(Read<u8*>(table + 0x70)))
        return false;
    out = {reinterpret_cast<u64>(base + 0x5320b40)};
    Function<RowFn>(0x1f26b40)(&out, id);
    return Plausible(out.row, 2) && Read<s32>(out.row + 4) == id;
}
const char* PlayerBlock(u8* player) {
    if (!Globals() || !Plausible(player) || Read<u8*>(Global(WorldChr) + 0x60) != player)
        return "player or managers unavailable";
    u8* vtable = Read<u8*>(player);
    if (!Plausible(vtable) || Read<u8*>(vtable + 0x3f0) != base + 0x18c6db0)
        return "player effect function changed";
    if (Read<s32>(player + 0x78) != 0)
        return "multiplayer guest";
    // Conservative until the active-peer predicate is runtime-verified: IsOnlineMode, registered
    // at 0x134337e, reads GameMan+0x1590. Never treat 'host' alone as single-player.
    if (Read<u8>(Global(GameMan) + 0x1590) != 0)
        return "online/multiplayer mode";
    u8* modules = Read<u8*>(player + 0x3b0);
    if (!Plausible(modules))
        return "player modules unavailable";
    u8* data = Read<u8*>(modules + 0x20);
    if (!Plausible(data) || Read<s32>(data + 0xf8) <= 0)
        return "player dead or HP unavailable";
    u8* pgd = Read<u8*>(Global(GameData) + 8);
    if (!Plausible(pgd) || Read<u8*>(player + 0x3c0) != pgd)
        return "player inventory changed";
    return nullptr;
}
std::optional<int> CountInventory(u8* inv, u32 item) {
    if (!Plausible(inv))
        return {};
    const u32 capacity = Read<u32>(inv + 0x10);
    if (!capacity || capacity > 65536 || Read<u32>(inv + 0x24) > 65536)
        return {};
    const int index = Function<FindFn>(0x14d9f60)(inv, &item);
    if (index == -1)
        return 0;
    const u32 highest = Read<u32>(inv + 0x88), key_cap = Read<u32>(inv + 0x24);
    if (index < 0 || highest > 65536 || static_cast<u32>(index) > highest || key_cap > 65536)
        return {};
    u8* list = Read<u8*>(inv + (static_cast<u32>(index) < key_cap ? 0x58 : 0x48));
    if (!Plausible(list))
        return {};
    u8* entry = list + 16 * (static_cast<u32>(index) < key_cap ? index : index - key_cap);
    if (!Read<u32>(entry) || Read<u32>(entry + 4) != item)
        return {};
    const int count = Read<s32>(entry + 8);
    return count >= 0 && count <= 999 ? std::optional<int>(count) : std::nullopt;
}
std::optional<int> Count(u8* egd, u32 item) {
    return CountInventory(egd + 0x158, item);
}

// 1.09 automatic replenishment (0x14dace0): storage at PlayerGameData+0x5b0
// transfers auto-replenishable goods to carried inventory, then reaches this one
// epilogue even when storage has no entries. Hook AFTER that transfer, not its loop:
// an empty storage has no vial entry to visit. The native loading caller is 0x19398de.
constexpr u64 RefillEndRva = 0x14dae7e;
constexpr std::array<u8, 15> RefillEndBytes{
    0x48, 0x83, 0xc4, 0x38, 0x5b, 0x41, 0x5c, 0x41, 0x5d, 0x41, 0x5e, 0x41, 0x5f, 0x5d, 0xc3};
constexpr std::array<u8, 21> RefillInventoryBytes{
    0x48, 0x8d, 0x05, 0x29, 0x04, 0x06, 0x04, 0x48, 0x8b, 0x00, 0x4c, 0x8b, 0x70, 0x08,
    0x4d, 0x8b, 0xbe, 0xb0, 0x05, 0x00, 0x00};
constexpr std::array<u8, 32> TransferBytes{
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec,
    0x68, 0x49, 0x89, 0xf6, 0x41, 0x89, 0xff, 0x48, 0x8b, 0x1d, 0x72, 0x94, 0xf0, 0x03, 0x48, 0x8b};
std::unique_ptr<Xbyak::CodeGenerator> refill_code;
bool refill_failed{};
bool vial_transfer_ready{};

void PS4_SYSV_ABI RefillFinished(u8* storage, void*) {
    if (!ready || refill_failed)
        return;
    u8* data = Global(GameData);
    u8* game = Global(GameMan);
    if (!Plausible(data) || !Plausible(game) || Read<u8>(game + 0x1590) != 0)
        return;
    u8* pgd = Read<u8*>(data + 8);
    if (!Plausible(pgd) || !Plausible(storage) || Read<u8*>(pgd + 0x5b0) != storage)
        return;
    constexpr u32 vial = 0x400003e8;
    u8* egd = pgd + 0x1d0;
    const auto carried = Count(egd, vial);
    const auto stored = CountInventory(storage, vial);
    // Do not call AddItem or disturb the ordinary full/partial refill unless both are empty.
    const int room = carried && stored && *carried == 0 && *stored == 0
                         ? Function<RoomFn>(0x1b5ca20)(&vial) : 0;
    const int grant = P::EmptyVialRefillGrant(carried, stored, room);
    if (!grant) {
        LOG_INFO(Core, "VR vial refill: replenishment finished, carried={} storage={} room={}, no grant",
                 carried.value_or(-1), stored.value_or(-1), room);
        return;
    }
    const int result = Function<AddItemFn>(0x14cdfd0)(egd, 0x40000000, 1000, grant, 1, 0, 1);
    const auto after = Count(egd, vial);
    const auto stock_after = CountInventory(storage, vial);
    LOG_INFO(Core, "VR vial refill: empty supplies, grant {}: carried 0->{} storage 0->{} result={}",
             grant, after.value_or(-1), stock_after.value_or(-1), result);
    if (result < 0 || after != std::optional<int>{grant} || stock_after != std::optional<int>{0}) {
        refill_failed = true; // no automatic retries after an unexpected native result
        LOG_ERROR(Core, "VR vial refill: verification failed; fallback disabled for this session");
    }
}

void InstallVialRefill(u64 size) {
    if (refill_code)
        return;
    // The lamp's ordinary storage transfer does not require the optional completion hook.
    vial_transfer_ready = 0x14da750 + TransferBytes.size() <= size &&
                          std::memcmp(base + 0x14da750, TransferBytes.data(),
                                      TransferBytes.size()) == 0;
    if (RefillEndRva + RefillEndBytes.size() > size ||
        std::memcmp(base + RefillEndRva, RefillEndBytes.data(), RefillEndBytes.size()) != 0 ||
        std::memcmp(base + 0x14dad00, RefillInventoryBytes.data(), RefillInventoryBytes.size()) != 0 ||
        !vial_transfer_ready) {
        LOG_WARNING(Core, "VR vial refill: OFF, native replenishment signature mismatch");
        return;
    }
    try {
        auto code = std::make_unique<Xbyak::CodeGenerator>(1024);
        // r15 is the storage inventory on both completion paths (including empty storage).
        // The replay contains the entire native epilogue and ret, with no relative operands.
        void* stub = Hook::EmitHostCallStub(*code, reinterpret_cast<void*>(&RefillFinished),
                                          RefillEndBytes.data(), RefillEndBytes.size(),
                                          reinterpret_cast<u64>(base + RefillEndRva +
                                                                RefillEndBytes.size()),
                                          Xbyak::util::r15);
        if (!Hook::MakeWritable(base + RefillEndRva, RefillEndBytes.size())) {
            LOG_ERROR(Core, "VR vial refill: OFF, could not make completion hook writable");
            return;
        }
        refill_code = std::move(code);
        Hook::WriteJump(base + RefillEndRva, RefillEndBytes.size(), stub);
        LOG_INFO(Core, "VR vial refill: installed, grant 3 only after native refill leaves carried "
                       "and storage empty (offline)");
    } catch (const Xbyak::Error& e) {
        LOG_ERROR(Core, "VR vial refill: OFF, could not generate completion hook: {}", e.what());
    }
}

bool Warp(u8* player) {
    const auto* dest = P::ByReturnPoint(request.destination);
    if (!dest || !P::Available(*dest, Flag)) {
        Refuse("destination no longer available");
        return false;
    }
    ReturnRow target{}, previous{};
    const int old = Read<s32>(Global(GameMan) + 0x1528);
    if (!Row(request.destination, target) || !Row(old, previous)) {
        Refuse("return-point parameter missing or unsupported");
        return false;
    }
    if (Read<u16>(target.row) != dest->area || Read<u16>(target.row + 2) != dest->block) {
        Refuse("return-point map disagrees with catalog");
        return false;
    }
    u8* egd = Read<u8*>(Global(GameData) + 8) + 0x1d0;
    constexpr std::array<u32, 2> items{0x400003e8, 0x40000384}; // goods 1000, 900
    std::array<int, 2> before{}, room{};
    for (size_t i = 0; i < items.size(); ++i) {
        auto qty = Count(egd, items[i]);
        if (!qty) {
            Refuse("inventory unavailable");
            return false;
        }
        room[i] = Function<RoomFn>(0x1b5ca20)(&items[i]);
        if (!qty || room[i] < 0 || room[i] > 99) {
            Refuse("inventory capacity unavailable");
            return false;
        }
        before[i] = *qty;
    }
    // VR Rest/Travel previously created a full vial stack directly. Use the same
    // native storage transfer as automatic replenishment, then the same empty-stock
    // fallback. Keep the existing bullet refill separate and unchanged.
    u8* storage = Read<u8*>(Global(GameData) + 8);
    storage = Read<u8*>(storage + 0x5b0);
    const auto stock = CountInventory(storage, items[0]);
    const auto plan = P::PlanVialRest(before[0], stock, room[0], !refill_failed);
    if (!vial_transfer_ready || !plan) {
        Refuse("vial replenishment unavailable");
        return false;
    }
    const int take = plan->take;
    const int index = take ? Function<FindFn>(0x14d9f60)(storage, &items[0]) : -1;
    if (take && index < 0) {
        Refuse("stored vial entry unavailable");
        return false;
    }
    if (take)
        Function<TransferFn>(0x14da750)(index, storage, egd + 0x158, take);
    // Verify the transfer BEFORE a failed transfer could be mistaken for empty supplies.
    const auto transferred = Count(egd, items[0]);
    const auto stock_left = CountInventory(storage, items[0]);
    if (transferred != std::optional<int>{before[0] + take} ||
        stock_left != std::optional<int>{*stock - take}) {
        Refuse("vial storage transfer verification failed; no warp and no retry");
        return false;
    }
    RefillFinished(storage, nullptr);
    const int grant = plan->grant;
    const auto vials_after = Count(egd, items[0]);
    LOG_INFO(Core, "VR lamp: vials {} -> {}, storage {} -> {}, transferred {}, free grant {}",
             before[0], vials_after.value_or(-1), *stock, *stock_left, take, grant);
    if (vials_after != std::optional<int>{plan->carried_after}) {
        Refuse("vial refill verification failed; no warp and no retry");
        return false;
    }
    for (size_t i = 1; i < items.size(); ++i) {
        const int id = items[i] & 0x0fffffff;
        const int result =
            room[i] ? Function<AddItemFn>(0x14cdfd0)(egd, 0x40000000, id, room[i], 1, 0, 1) : 0;
        const auto after = Count(egd, items[i]);
        LOG_INFO(Core, "VR lamp: refill goods {}: {} -> {} (room {}, result {})", id, before[i],
                 after.value_or(-1), room[i], result);
        if (result < 0 || !after || *after != before[i] + room[i]) {
            Refuse("refill verification failed; no warp and no automatic retry");
            return false;
        }
    }
    Function<SetRespawnFn>(0x156d110)(request.destination);
    LOG_INFO(Core, "VR lamp: native respawn setter {} -> {} (readback {})", old,
             request.destination, Read<s32>(Global(GameMan) + 0x1528));
    if (Read<s32>(Global(GameMan) + 0x1528) != request.destination) {
        Function<SetRespawnFn>(0x156d110)(old);
        Refuse("respawn readback mismatch");
        return false;
    }
    const auto added =
        Function<AddEffectFn>(0x18c6db0)(player, 2101, player, 0, 1, 0, 1.f, 1.f, 1.f, 1.f, 1.f, 0);
    LOG_INFO(Core, "VR lamp: AddSpEffect 2101 -> {} (destination {}, {})", added,
             request.destination, dest->name);
    if (!added) {
        Function<SetRespawnFn>(0x156d110)(old);
        Refuse("warp effect rejected; respawn restored");
        return false;
    }
    return true;
}
void Next(Phase value, s64 now) {
    phase = value;
    phase_start = now;
    P::Mailbox().Output(P::Pad::Neutral, true);
}
// A fault-safe read of game memory reached through world objects (a stale pointer never crashes).
bool Copy(const u8* address, void* out, size_t size) {
    const auto value = reinterpret_cast<uintptr_t>(address);
    if (value < 0x10000 || value >= 0x0000'8000'0000'0000ULL) {
        return false;
    }
#ifdef _WIN32
    SIZE_T copied{};
    return ReadProcessMemory(GetCurrentProcess(), address, out, size, &copied) && copied == size;
#else
    return false;
#endif
}
template <class T> std::optional<T> SafeRead(const u8* address) {
    T value{};
    if (!Copy(address, &value, sizeof(value)))
        return {};
    return value;
}

// The lamp object you stand at. Each map's event 7000 registers its lamps as objects with entity
// AABB1950 + index, the return point's spawn entity less 1000 (m24_01: 2411950.. for 2412950..).
// In a world object (ObjectMan: blocks of 0x2E0-byte records) +0x28 points to its map part:
// the position at +0x28 / +0x2C / +0x30, and after its name the entity id followed by -1
// (measured in the mock, 2026-10-14: Central Yharnam's at 0.56 m while at its box, the other
// m24_01 lamps 60..170 m away). The nearest catalog lamp within NearLampM, or nothing.
constexpr float NearLampM = 6.0f;
struct NearLamp {
    const P::Lamp* lamp{};
    float distance{};
    bool searched{}; // the player and the object list were readable
};
NearLamp FindNearLamp() {
    NearLamp out{};
    u8* const world = Global(WorldChr);
    u8* const player = Plausible(world) ? Read<u8*>(world + 0x60) : nullptr;
    u8* const ctrl = Plausible(player) ? Read<u8*>(player + 0x58) : nullptr;
    if (!Plausible(ctrl) || Read<u8*>(ctrl + 0x08) != player)
        return out;
    std::array<float, 12> rows{};
    if (!Copy(ctrl + 0x90, rows.data(), sizeof(rows)))
        return out;
    const float px = rows[3], py = rows[7], pz = rows[11];
    if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz))
        return out;
    u8* const man = Global(ObjectMan);
    const auto block_count = Plausible(man) ? SafeRead<s32>(man + 0x20) : std::nullopt;
    const auto blocks = Plausible(man) ? SafeRead<u8*>(man + 0x28) : std::nullopt;
    if (!block_count || *block_count <= 0 || *block_count > 256 || !blocks || !Plausible(*blocks))
        return out;
    out.searched = true;
    out.distance = NearLampM;
    for (s32 b = 0; b < *block_count; ++b) {
        u8* const entry = *blocks + b * 0x1F8;
        const auto count = SafeRead<s32>(entry + 0xC8);
        const auto objects = SafeRead<u8*>(entry + 0xD0);
        if (!count || *count <= 0 || *count > 0x100000 || !objects || !Plausible(*objects))
            continue;
        for (s32 i = 0; i < *count; ++i) {
            const auto part = SafeRead<u8*>(*objects + static_cast<size_t>(i) * 0x2E0 + 0x28);
            if (!part || !Plausible(*part))
                continue;
            std::array<u8, 0x180> data{};
            if (!Copy(*part, data.data(), data.size()))
                continue;
            float pos[3];
            std::memcpy(pos, data.data() + 0x28, sizeof(pos));
            const float dx = pos[0] - px, dy = pos[1] - py, dz = pos[2] - pz;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (!std::isfinite(distance) || distance >= out.distance)
                continue;
            for (size_t o = 0xD0; o + 8 <= data.size(); o += 4) {
                s32 id, next;
                std::memcpy(&id, data.data() + o, 4);
                std::memcpy(&next, data.data() + o + 4, 4);
                if (next != -1 || id % 10000 < 1950 || id % 10000 > 1959)
                    continue;
                if (const auto* lamp = P::ByReturnPoint(id + 1000)) {
                    out.lamp = lamp;
                    out.distance = distance;
                    break;
                }
            }
        }
    }
    return out;
}

// The lamp the box belongs to: the conversation's talk id, else the lamp object you stand at,
// else (no object list) the respawn point. The respawn point alone was wrong whenever you last
// rested elsewhere (the 2026-10-14 run: at Central Yharnam's box with respawn 2412952, Rest took
// you to the Great Bridge).
const P::Lamp* CurrentLamp(int talk_id, bool& verified) {
    verified = true;
    if (const auto* lamp = P::ByTalk(talk_id))
        return lamp;
    const NearLamp found = FindNearLamp();
    const int respawn = Read<s32>(Global(GameMan) + 0x1528);
    if (found.lamp) {
        LOG_INFO(Core, "VR lamp: no talk id: the lamp object {:.2f} m away ({}; respawn {})",
                 found.distance, found.lamp->name, respawn);
        return found.lamp;
    }
    if (found.searched) {
        LOG_WARNING(Core, "VR lamp: no talk id and no lamp object within {:.0f} m; the respawn "
                          "point {} as before",
                    NearLampM, respawn);
    } else {
        LOG_WARNING(Core, "VR lamp: no talk id and no object list; the respawn point {}", respawn);
    }
    verified = false; // The last respawn point may belong to a different map.
    return P::ByReturnPoint(respawn);
}
const P::Lamp* offered_lamp{};
bool offered_lamp_verified{};
P::CleanupArrival cleanup_arrival;
// A lantern warp in flight (VR::PublishLampWarp): the player that left; the new one arrives.
u8* warp_player{};
} // namespace

void LampsEbootLoaded(VAddr address, u64 size) {
    base = reinterpret_cast<u8*>(address);
    ready = false;
    phase = Phase::Idle;
    seen = 0;
    cleanup_arrival = {};
    offered_lamp_verified = false;
    P::Mailbox().Cancel();
    if (size <= MenuMan + 8)
        return;
    for (const auto& site : P::Native::Sites) {
        if (site.rva + site.bytes.size() > size ||
            std::memcmp(base + site.rva, site.bytes.data(), site.bytes.size()) != 0) {
            LOG_WARNING(Core, "VR lamp: OFF: signature mismatch {} at {:#x}", site.name, site.rva);
            return;
        }
    }
    ready = true;
    InstallVialRefill(size);
    LOG_INFO(Core, "VR lamp: signatures verified; lamp_travel={} (offline only; no save-file IO)",
             P::Enabled());
}
void LampsSiteA(u8* player) {
    if (cleanup_arrival.Update(reinterpret_cast<uintptr_t>(player), P::NowNs())) {
        VR::RequestTransitionTextureCleanup(true);
        LOG_INFO(Core, "Texture transition: confirmed cross-map lamp arrival");
    }
    if (warp_player && player != warp_player) {
        warp_player = nullptr;
        VR::PublishLampWarp(true);
        LOG_INFO(Core, "VR lamp: arrived (a new player after the warp)");
    }
    if (!ready || !P::Enabled() || !VR::GetSettings().menu_mode) {
        Cancel("disabled");
        return;
    }
    const s64 now = P::NowNs();
    if (const char* reason = PlayerBlock(player)) {
        Refuse(reason);
        Cancel(reason);
        return;
    }
    u8* system = Global(MenuSystem);
    u8* man = Global(MenuMan);
    u8* talk = Global(TalkMan);
    if (Read<u8*>(system) != base + 0x539c110 || Read<u8*>(man) != base + 0x532b490) {
        Refuse("menu manager type changed");
        Cancel("menu manager type changed");
        return;
    }
    const bool modal = Read<u64>(system + 0xa88) != 0;
    const u64 serial = Read<u64>(system + 0xb18);
    u8* dialog = Read<u8*>(man + 0xd08);
    const int talk_id = Read<s32>(talk + 0x3ffc);
    const int message = Plausible(dialog) ? Read<s32>(dialog) : -1;
    // The message alone identifies the lantern's box: every lamp's talk script asks 10010713
    // ("Return to the Hunter's Dream?"), and the lamp is then checked by its talk id. (The two
    // words after it are not 1 / 2: the scripts open it with 3, 4, 2; logged below.)
    const bool lamp_box =
        modal && MenuIsMessageBox(serial) && Plausible(dialog) && message == LampMessage;
    if (phase == Phase::Idle) {
        if (serial == seen)
            return;
        if (!lamp_box) {
            if (modal && MenuIsMessageBox(serial)) {
                VR::PublishLampBoxHidden(false);
                seen = serial; // once per box: a message box that is not the lantern's
                LOG_INFO(Core, "VR lamp: message box serial {} is not the lantern's (message {})",
                         serial, message);
            }
            return;
        }
        seen = serial;
        LOG_INFO(Core, "VR lamp: dialog serial {} talk {} message {} ({} {}) respawn {}", serial,
                 talk_id, message, Read<s32>(dialog + 4), Read<s32>(dialog + 8),
                 Read<s32>(Global(GameMan) + 0x1528));
        const auto* lamp = CurrentLamp(talk_id, offered_lamp_verified);
        offered_lamp = lamp;
        if (!lamp || !P::Available(*lamp, Flag)) {
            Refuse("unrecognised, unlit or blocked current lamp");
            return;
        }
        if (Read<s32>(dialog + 0x18) != -1) {
            Refuse("lamp dialog already has a result");
            return;
        }
        default_choice = Read<s32>(man + 0x184);
        if (default_choice < 0 || default_choice > 1) {
            Refuse("unrecognised dialog selection");
            return;
        }
        offer = {serial, lamp->ReturnPoint(), P::TravelPages(Flag)};
        offered_serial = serial;
        offered_player = player;
        offered_dialog = dialog;
        refusal = nullptr;
        P::Mailbox().Publish(offer);
        VR::PublishLampBoxHidden(true);
        phase = Phase::Offering;
        started = phase_start = now;
        LOG_INFO(Core, "VR lamp: offer {} ({} travel pages)", lamp->name, offer.pages.size());
        return;
    }
    if (player != offered_player) {
        Cancel("player changed");
        return;
    }
    if (!P::Mailbox().Active(offered_serial)) {
        Cancel("page cancelled or site-A heartbeat expired");
        return;
    }
    if (phase == Phase::Offering || phase == Phase::Responding)
        VR::PublishLampBoxHidden(true); // the page answers the box: the game's own stays hidden
    if (phase == Phase::Offering) {
        if (!P::Mailbox().Presented(offered_serial) && now - started > 5'000'000'000) {
            Cancel("lamp page was not visibly submitted");
            return;
        }
        // The lamp found at the offer (the object search runs once per box); a talk id that
        // appears later and names another lamp is a different box.
        const auto* by_talk = P::ByTalk(talk_id);
        if (!lamp_box || serial != offered_serial || !offered_lamp ||
            offered_lamp->ReturnPoint() != offer.current ||
            (by_talk && by_talk->ReturnPoint() != offer.current)) {
            Cancel("lamp dialog changed");
            return;
        }
        P::Mailbox().Output(P::Pad::Neutral);
        if (auto pending = P::Mailbox().TakeRequest()) {
            if (!P::ValidRequest(offer, *pending)) {
                Cancel("stale action");
                return;
            }
            request = *pending;
            sequence.Start(request.action, default_choice, now);
            last_pad = P::Pad::Neutral;
            last_receipt_ns = 0;
            Next(Phase::Responding, now);
            LOG_INFO(Core, "VR lamp: action {} destination {} serial {}",
                     static_cast<int>(request.action), request.destination, request.serial);
        }
        return;
    }
    if (phase == Phase::Responding) {
        const bool talking = Read<u8>(talk + 0x3ff8) || Read<s32>(talk + 0x3ffc) != -1 ||
                             Read<float>(talk + 0x400c) > 0.f;
        const bool same_dialog =
            dialog == offered_dialog && Plausible(dialog) && message == LampMessage;
        const auto state =
            P::DialogFor(offered_serial, serial, modal, same_dialog && (!modal || lamp_box));
        const int result = same_dialog ? Read<s32>(dialog + 0x18) : -1;
        const auto command = sequence.Command();
        const auto receipt = P::Mailbox().Delivery();
        if (receipt && receipt->applied_ns != last_receipt_ns) {
            last_receipt_ns = receipt->applied_ns;
            LOG_INFO(Core, "VR lamp: pad applied {}", static_cast<int>(receipt->pad));
        }
        const auto step = sequence.Tick(P::NowNs(), state, result, talking, receipt);
        if (step.pad != last_pad) {
            LOG_INFO(Core, "VR lamp: dialog input requested {}",
                     step.pad == P::Pad::Back   ? "Back"
                     : step.pad == P::Pad::Yes  ? "Confirm"
                     : step.pad == P::Pad::Left ? "Left (Yes)"
                                                : "released");
            last_pad = step.pad;
        }
        P::Mailbox().Output(step.pad, command != sequence.Command());
        switch (step.result) {
        case P::Sequence::Result::Waiting:
            break;
        case P::Sequence::Result::Cancelled:
            LOG_INFO(Core, "VR lamp: stopped with the box's native result {} (dialog {})", result,
                     static_cast<int>(state));
            Cancel(step.reason);
            break;
        case P::Sequence::Result::Finished:
            cleanup_arrival.Begin(reinterpret_cast<uintptr_t>(player),
                                  P::ChangesMapForCleanup(request.action, offered_lamp,
                                                          offered_lamp_verified), now);
            Cancel("vanilla response delivered");
            break;
        case P::Sequence::Result::Warp:
            LOG_INFO(Core,
                     "VR lamp: delivered Back, native result {}, dialog and conversation clear; "
                     "preflight",
                     result);
            if (!Warp(player)) {
                Cancel("warp preflight or native call failed");
                return;
            }
            cleanup_arrival.Begin(
                reinterpret_cast<uintptr_t>(player),
                P::ChangesMapForCleanup(request.action, offered_lamp, offered_lamp_verified,
                                       P::ByReturnPoint(request.destination)), now);
            warp_player = player;
            VR::PublishLampWarp(false);
            Next(Phase::Warp, now);
            break;
        }
    } else if (phase == Phase::Warp) {
        P::Mailbox().Output(P::Pad::Neutral);
        if (now - phase_start > 2'000'000'000)
            Cancel("warp issued; returning input to game");
    }
}
} // namespace VR::Bloodborne
