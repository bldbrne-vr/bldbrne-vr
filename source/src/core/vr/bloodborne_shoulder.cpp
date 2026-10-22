// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include "core/vr/bloodborne_shoulder.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "core/vr/body_channel.h"
#include "core/vr/hook_util.h"
#include "core/vr/shoulder_equipment.h"
#include "core/vr/vr_session.h"
#include <array>
#include <cstdlib>
#include <memory>
#include <mutex>

namespace VR::Bloodborne {
namespace {
using Hook::Plausible;
using Hook::Read;
namespace E = VR::ShoulderEquipment;
// 1.09 PlayerIns equipment update: the complete saved equipment has just been
// copied into r12. The code below compares old/new IDs, reloads models and updates
// the HKS weapon/style mirror. r13 = player, r15 = PlayerGameData.
// Pending requests execute once here on the game thread; idle frames only observe.
constexpr u64 RefreshRva = 0x18F5249;
constexpr std::array<u8, 14> RefreshSig = {
    0x41, 0x8B, 0x87, 0x20, 0x03, 0x00, 0x00, // mov eax,[r15+0x320]
    0x41, 0x89, 0x85, 0xF8, 0x04, 0x00, 0x00, // mov [r13+0x4F8],eax
};
constexpr u64 CopyRva = 0x18F5220;
constexpr std::array<u8, 13> CopySig = {
    0x41, 0x8B, 0x84, 0x1F, 0xA4, 0x02, 0x00, 0x00, 0x41, 0x89, 0x44, 0x1C, 0x64,
};
constexpr u64 SelectRva = 0x18F51CF;
constexpr std::array<u8, 24> SelectSig = {
    0x41, 0x8B, 0x87, 0x4C, 0x02, 0x00, 0x00, 0x41, 0x89, 0x44, 0x24, 0x0C,
    0x41, 0x8B, 0x87, 0x50, 0x02, 0x00, 0x00, 0x41, 0x89, 0x44, 0x24, 0x10,
};
// Use the game's equipment transactions: a bare hand has a real inventory
// entry/reference. Never fabricate an ID/handle pair or override the save copy.
constexpr u64 UnequipRva = 0x14CD1C0, SetEquipRva = 0x14CCB90, CopyRefRva = 0x1A8BD80;
constexpr std::array<u8, 24> UnequipSig = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56,
                                           0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC,
                                           0x98, 0,    0,    0,    0x89, 0xF3, 0x49, 0x89};
constexpr std::array<u8, 25> SetEquipSig = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                            0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x28, 0x89,
                                            0xCB, 0x49, 0x89, 0xD4, 0x41, 0x89, 0xF7};
constexpr std::array<u8, 16> CopyRefSig = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56,
                                           0x53, 0x50, 0x49, 0x89, 0xF6, 0x48, 0x89, 0xFB};
using UnequipFn = void(PS4_SYSV_ABI*)(u8*, s32);
using SetEquipFn = void(PS4_SYSV_ABI*)(u8*, s32, const s32*, s32, s32);
using CopyRefFn = void(PS4_SYSV_ABI*)(u8*, const u8*);
UnequipFn unequip{};
SetEquipFn set_equip{};
CopyRefFn copy_ref{};
constexpr u64 WorldRva = 0x553E878;
constexpr u64 MainRva = 0x18C2D00;
constexpr std::array<u8, 17> MainSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x48, 0x89,
    0xFB, 0x4C, 0x8D, 0x35, 0x67, 0xBB, 0xC7, 0x03,
};
u8* base{};
std::unique_ptr<Xbyak::CodeGenerator> code;
std::mutex mutex;
bool installed{};
u8* current_player{};
u8* current_data{};
s64 last_tick{};
std::array<E::Remembered, 2> remembered{};
std::array<bool, 2> empty_selected{};
struct Request {
    bool pending{}, unarmed{}, cycle{};
    u8* player{};
    s64 expires{};
};
std::array<Request, 2> requests{};

// Verify the exact owned instance, including upgrades/gems through its handle.
// An item sold or replaced while unarmed must never restore an unrelated instance.
bool StillOwned(const u8* data, const E::Remembered& item) {
    const s32 index = item.inventory_index;
    if (index < 0 || index > Read<s32>(data + 0x3B0))
        return false;
    const s32 key_capacity = Read<s32>(data + 0x34C);
    if (key_capacity < 0)
        return false;
    const bool key = index < key_capacity;
    const s32 entry = key ? index : index - key_capacity;
    if (!key && entry >= Read<s32>(data + 0x338))
        return false;
    const auto list = Read<u8*>(data + (key ? 0x380 : 0x370));
    if (!Plausible(list))
        return false;
    const auto p = list + 16ull * entry;
    return Read<s32>(p) == item.handle && Read<s32>(p + 4) == item.id && Read<s32>(p + 8) > 0;
}

void Select(u8* equipment, u8* data, unsigned side, int slot) {
    E::Write(data + E::SavedSelected + 4 * side, slot);
    E::Write(equipment + E::Selected + 4 * side, slot);
    // The native operation owns style; copy its result after selecting the hand.
    E::Write(equipment + 8, Read<s32>(data + 0x248));
}

void SyncSlot(u8* equipment, u8* data, unsigned index) {
    E::Write(equipment + E::Ids + 4 * index, Read<s32>(data + 0x2A4 + 4 * index));
    copy_ref(equipment + E::Handles + 4 * index, data + 0x264 + 4 * index);
}

bool EquippedElsewhere(const u8* data, unsigned target, const E::Remembered& item) {
    // Match the Equipment menu's uniqueness check over its 15 equipment slots.
    for (unsigned slot = 0; slot < 15; ++slot) {
        if (slot != target && Read<s32>(data + 0x1F4 + 4 * slot) == item.inventory_index)
            return true;
    }
    return false;
}

bool ResumeSwitching(u8* equipment, u8* data, unsigned side) {
    auto& item = remembered[side];
    const int other = 1 - item.slot;
    Select(equipment, data, side, other);
    item = {};
    LOG_INFO(Core, "VR shoulder: remembered {} weapon unavailable or moved; selected other slot",
             side == 0 ? "left" : "right");
    return true; // a real slot change, with no inventory mutation
}

bool Restore(u8* equipment, u8* data, unsigned side) {
    auto& item = remembered[side];
    if (item.slot < 0)
        return false;
    const unsigned index = 2 * item.slot + side;
    if (Read<s32>(data + 0x2A4 + 4 * index) != E::BareFists) {
        item = {}; // a manual equipment edit wins
        return false;
    }
    if (item.HadWeapon()) {
        if (!StillOwned(data, item) || EquippedElsewhere(data, index, item))
            return ResumeSwitching(equipment, data, side);
        set_equip(data + 0x1D0, index, &item.handle, item.inventory_index, 1);
        SyncSlot(equipment, data, index);
        if (Read<s32>(data + 0x2A4 + 4 * index) != item.id)
            return ResumeSwitching(equipment, data, side);
    }
    Select(equipment, data, side, item.return_slot);
    item = {};
    return true;
}

bool Sheathe(u8* equipment, u8* data, unsigned side) {
    auto& item = remembered[side];
    if (item.slot >= 0) {
        Select(equipment, data, side, item.slot);
        return true;
    }
    const auto active = Read<s32>(equipment + E::Selected + 4 * side);
    const auto slot = 1 - active;
    const unsigned index = 2 * slot + side;
    E::Remembered next{slot, slot, Read<s32>(data + 0x2A4 + 4 * index),
                       Read<s32>(data + 0x264 + 4 * index), Read<s32>(data + 0x1F4 + 4 * index)};
    if (next.HadWeapon() && !StillOwned(data, next))
        return false;
    // If the next slot was already empty, draw the previously held weapon on
    // the next tap. Native unequip also normalizes legacy invalid empty entries.
    if (!next.HadWeapon())
        next.return_slot = active;
    unequip(data + 0x1D0, index);
    SyncSlot(equipment, data, index);
    if (Read<s32>(data + 0x2A4 + 4 * index) != E::BareFists)
        return false;
    item = next;
    if (side == 1)
        E::Write(data + 0x248, 1);
    Select(equipment, data, side, slot);
    return true;
}

// The other weapon slot of the hand, when it holds a weapon (not bare fists), else -1.
int OtherWeaponSlot(const u8* equipment, const u8* data, unsigned side) {
    const s32 active = Read<s32>(equipment + E::Selected + 4 * side);
    if (active != 0 && active != 1)
        return -1;
    const int other = 1 - active;
    const s32 id = Read<s32>(data + 0x2A4 + 4 * (2 * other + side));
    return id >= 0 && id != E::BareFists ? other : -1;
}

bool Cycle(u8* equipment, u8* data, unsigned side) {
    const int other = OtherWeaponSlot(equipment, data, side);
    if (other < 0)
        return false;
    // The right weapon comes out in its normal form, as after the game's own switch (and as
    // Sheathe leaves it).
    if (side == 1)
        E::Write(data + 0x248, 1);
    Select(equipment, data, side, other);
    return true;
}

void PS4_SYSV_ABI Refresh(u8* player, void*, u8* player_data) {
    const auto world = Read<u8*>(base + WorldRva);
    if (!Plausible(world) || Read<u8*>(world + 0x60) != player)
        return;
    if (!Plausible(player_data) || Read<u8*>(player + 0x3C0) != player_data)
        return;
    auto* equipment = Read<u8*>(player + 0x4B8);
    if (!Plausible(equipment) || !E::Valid(equipment))
        return;
    const s64 now = Body::SteadyNowNs();
    const auto& settings = GetSettings();
    const bool enabled = settings.shoulder_switch && !settings.controls_classic && IsImmersive();
    Body::BodyStatus body{};
    const bool fresh = Body::LatestBodyStatus(body) && now >= body.steady_ns &&
                       now - body.steady_ns < 250'000'000 && body.driven;
    std::lock_guard lock(mutex);
    if (player != current_player || player_data != current_data) {
        remembered = {};
        empty_selected = {};
        requests = {};
    }
    current_player = player;
    current_data = player_data;
    last_tick = now;
    for (unsigned side = 0; side < 2; ++side) {
        auto& item = remembered[side];
        if (item.slot >= 0 &&
            Read<s32>(player_data + 0x2A4 + 4 * (2 * item.slot + side)) != E::BareFists)
            item = {};
        const auto request = requests[side];
        requests[side] = {};
        if (!enabled) {
            Restore(equipment, player_data, side);
        } else if (request.pending && request.player == player && now <= request.expires && fresh &&
                   body.arm_weight[side] > .9f && !body.daze && !body.facing_block &&
                   !CinematicActive() && !MenuPanelActive()) {
            const bool changed = request.cycle     ? Cycle(equipment, player_data, side)
                                 : request.unarmed ? Sheathe(equipment, player_data, side)
                                                   : Restore(equipment, player_data, side);
            LOG_INFO(Core, "VR shoulder: {} hand {} {}", side == 0 ? "left" : "right",
                     request.cycle     ? "native weapon cycle (no switch clip)"
                     : request.unarmed ? "native unequip"
                                       : "restore remembered weapon",
                     changed ? "applied" : "refused");
        }
        empty_selected[side] =
            item.slot >= 0 && Read<s32>(equipment + E::Selected + 4 * side) == item.slot &&
            Read<s32>(equipment + E::Ids + 4 * (2 * item.slot + side)) == E::BareFists;
    }
}
} // namespace

bool RequestShoulderEquipment(unsigned side, bool unarmed) {
    if (side > 1)
        return false;
    const auto now = Body::SteadyNowNs();
    std::lock_guard lock(mutex);
    if (!installed || !current_player || now < last_tick || now - last_tick >= 250'000'000 ||
        requests[side].pending)
        return false;
    requests[side] = {true, unarmed, false, current_player, now + 250'000'000};
    return true;
}

bool RequestShoulderCycle(unsigned side) {
    static const bool enabled = [] {
        const char* value = std::getenv("VR_SHOULDER_NATIVE_CYCLE");
        return !(value && value[0] == '0');
    }();
    if (side > 1 || !enabled)
        return false;
    const auto now = Body::SteadyNowNs();
    std::lock_guard lock(mutex);
    if (!installed || !current_player || !current_data || now < last_tick ||
        now - last_tick >= 250'000'000 || requests[side].pending || empty_selected[side])
        return false;
    auto* equipment = Read<u8*>(current_player + 0x4B8);
    if (!Plausible(equipment) || !E::Valid(equipment) ||
        OtherWeaponSlot(equipment, current_data, side) < 0)
        return false;
    requests[side] = {true, false, true, current_player, now + 250'000'000};
    return true;
}

void CancelShoulderEquipmentRequests() {
    std::lock_guard lock(mutex);
    requests = {};
}

bool ShoulderUnarmed(unsigned side) {
    std::lock_guard lock(mutex);
    return side < 2 && empty_selected[side];
}

int ShoulderRestoreSlot(unsigned side) {
    std::lock_guard lock(mutex);
    return side < 2 && empty_selected[side] ? remembered[side].return_slot : -1;
}

void InstallShoulderEquipment(VAddr address, u64 size) {
    auto* module = reinterpret_cast<u8*>(address);
    if (installed || size <= WorldRva + 8)
        return;
    const auto matches = [module](u64 rva, const auto& sig) {
        return std::memcmp(module + rva, sig.data(), sig.size()) == 0;
    };
    if (!matches(RefreshRva, RefreshSig) || !matches(CopyRva, CopySig) ||
        !matches(SelectRva, SelectSig) || !matches(MainRva, MainSig) ||
        !matches(UnequipRva, UnequipSig) || !matches(SetEquipRva, SetEquipSig) ||
        !matches(CopyRefRva, CopyRefSig)) {
        LOG_WARNING(Core, "VR shoulder: unarmed unavailable (equipment signature mismatch)");
        return;
    }
    try {
        auto stub = std::make_unique<Xbyak::CodeGenerator>(4096);
        using namespace Xbyak::util;
        void* entry = Hook::EmitHostCallStub(
            *stub, reinterpret_cast<void*>(&Refresh), RefreshSig.data(), RefreshSig.size(),
            reinterpret_cast<u64>(module + RefreshRva + RefreshSig.size()), r13, &r15);
        stub->ready();
        if (!Hook::MakeWritable(module + RefreshRva, RefreshSig.size()))
            return;
        unequip = reinterpret_cast<UnequipFn>(module + UnequipRva);
        set_equip = reinterpret_cast<SetEquipFn>(module + SetEquipRva);
        copy_ref = reinterpret_cast<CopyRefFn>(module + CopyRefRva);
        base = module;
        code = std::move(stub);
        Hook::WriteJump(module + RefreshRva, RefreshSig.size(), entry);
        {
            std::lock_guard lock(mutex);
            installed = true;
        }
        LOG_INFO(Core, "VR shoulder: native remembered-slot unequip installed");
    } catch (const Xbyak::Error& error) {
        LOG_ERROR(Core, "VR shoulder: unarmed hook emission failed: {}", error.what());
    }
}
} // namespace VR::Bloodborne
