// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstdlib>
#include <memory>
#include <mutex>
#include "common/logging/log.h"
#include "common/thread.h"
#include "core/vr/bloodborne_shoulder.h"
#include "core/vr/body_channel.h"
#include "core/vr/hook_util.h"
#include "core/vr/interaction_native_shoulder.h"
#include "core/vr/shoulder_equipment.h"
#include "core/vr/vr_session.h"

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
std::array<E::Remembered, 2> remembered{};
std::array<bool, 2> empty_selected{};
VR::Interactions::Module* request_module{};
bool request_fault{};
VR::Interactions::NativeShoulderRuntime request_policy;
void InitRequestDll() {
    if (!VR::Interactions::DllRequested() || request_module)
        return;
    request_module = new VR::Interactions::Module();
    request_module->EnableFaultBroadcast();
    request_policy.Bind(*request_module, request_fault);
    if (!request_module->LoadBesideExecutable()) {
        request_fault = true;
        request_module->ReportFault();
        LOG_ERROR(Core, "VR shoulder native: DLL unavailable: {}", request_module->Diagnostic());
    }
}

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

// Fault cleanup only: undo a previously owned inventory edit, never choose a new slot/item.
void RestoreOwnedOnFault(u8* equipment, u8* data, unsigned side) {
    auto& item = remembered[side];
    if (item.slot < 0 || item.slot > 1 || item.return_slot < 0 || item.return_slot > 1)
        return;
    const unsigned index = 2 * item.slot + side;
    if (Read<s32>(data + 0x2A4 + 4 * index) != E::BareFists) {
        item = {};
        return;
    }
    if (item.HadWeapon()) {
        if (!StillOwned(data, item) || EquippedElsewhere(data, index, item))
            return;
        set_equip(data + 0x1D0, index, &item.handle, item.inventory_index, 1);
        SyncSlot(equipment, data, index);
        if (Read<s32>(data + 0x2A4 + 4 * index) != item.id)
            return;
    }
    Select(equipment, data, side, item.return_slot);
    item = {};
}

bool ExecuteEquipment(u8* equipment, u8* data, unsigned side, uint32_t action) {
    auto& item = remembered[side];
    PiShoulderInput packet{};
    packet.operation = PI_SHOULDER_EQUIP_PLAN;
    packet.side = side;
    packet.now = Body::SteadyNowNs();
    auto& facts = packet.equipment;
    facts.action = action;
    facts.active_slot = Read<s32>(equipment + E::Selected + 4 * side);
    facts.remembered_slot = item.slot;
    facts.return_slot = item.return_slot;
    facts.remembered_goods = item.id;
    // Address selection copies raw slot facts only; the DLL chooses the actual action/return.
    const int snapshot_slot = action == PI_NSC_RESTORE ? item.slot : 1 - facts.active_slot;
    if (snapshot_slot >= 0 && snapshot_slot <= 1) {
        const unsigned index = 2 * snapshot_slot + side;
        facts.slot_goods = Read<s32>(data + 0x2A4 + 4 * index);
        if (action == PI_NSC_RESTORE) {
            facts.slot_handle = item.handle;
            facts.slot_inventory = item.inventory_index;
            if (facts.slot_goods == E::BareFists && item.HadWeapon()) {
                facts.owned = StillOwned(data, item);
                facts.elsewhere = EquippedElsewhere(data, index, item);
            }
        } else {
            facts.slot_handle = Read<s32>(data + 0x264 + 4 * index);
            facts.slot_inventory = Read<s32>(data + 0x1F4 + 4 * index);
            E::Remembered candidate{snapshot_slot, snapshot_slot, facts.slot_goods,
                                    facts.slot_handle, facts.slot_inventory};
            facts.owned = !candidate.HadWeapon() || StillOwned(data, candidate);
        }
    }
    auto plan = request_policy.Equipment(packet);
    if (!plan.applied)
        return false;
    if (plan.flags & (PI_SE_SET | PI_SE_UNEQUIP)) {
        if (plan.index < 0 || plan.index > 3 || (unsigned(plan.index) & 1u) != side)
            return false;
        packet.operation = PI_SHOULDER_EQUIP_RECEIPT;
        packet.request_id = plan.plan_id;
        packet.equipment.write_attempted = 0;
        if (plan.flags & PI_SE_SET) {
            // A copied policy fact never replaces the final native exact-instance proof.
            if (StillOwned(data, item) && !EquippedElsewhere(data, plan.index, item) &&
                Read<s32>(data + 0x2A4 + 4 * plan.index) == E::BareFists) {
                set_equip(data + 0x1D0, plan.index, &item.handle, item.inventory_index, 1);
                SyncSlot(equipment, data, plan.index);
                packet.equipment.write_attempted = 1;
            }
        } else {
            E::Remembered candidate{plan.remember_slot, plan.return_slot, plan.id, plan.handle,
                                    plan.inventory_index};
            if (Read<s32>(data + 0x2A4 + 4 * plan.index) == plan.id &&
                (!candidate.HadWeapon() || StillOwned(data, candidate))) {
                unequip(data + 0x1D0, plan.index);
                SyncSlot(equipment, data, plan.index);
                packet.equipment.write_attempted = 1;
                // Preserve the rollback proof even if the DLL faults during the guest call.
                if (Read<s32>(data + 0x2A4 + 4 * plan.index) == E::BareFists)
                    item = candidate;
            }
        }
        packet.equipment.after_goods = Read<s32>(data + 0x2A4 + 4 * plan.index);
        plan = request_policy.Equipment(packet);
        if (!plan.applied)
            return false;
    }
    if (plan.flags & PI_SE_REMEMBER)
        item = {plan.remember_slot, plan.return_slot, plan.id, plan.handle, plan.inventory_index};
    if (plan.flags & PI_SE_STYLE)
        E::Write(data + 0x248, 1);
    if (plan.flags & PI_SE_SELECT)
        Select(equipment, data, side, plan.select_slot);
    if (plan.flags & PI_SE_DROP)
        item = {};
    if (plan.flags & PI_SE_FALLBACK)
        LOG_INFO(Core,
                 "VR shoulder: remembered {} weapon unavailable or moved; selected other slot",
                 side == 0 ? "left" : "right");
    return (plan.flags & PI_SE_CHANGED) != 0;
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
    const bool have_body = Body::LatestBodyStatus(body);
    std::lock_guard lock(mutex);
    const bool changed_owner = player != current_player || player_data != current_data;
    if (changed_owner) {
        remembered = {};
        empty_selected = {};
    }
    current_player = player;
    current_data = player_data;
    PiShoulderInput frame{};
    frame.operation = PI_SHOULDER_NATIVE_FRAME;
    frame.now = now;
    auto& facts = frame.native;
    facts.flags = (enabled ? PI_NSF_ENABLED : 0u) | (changed_owner ? PI_NSF_OWNER_CHANGED : 0u) |
                  (have_body ? PI_NSF_BODY : 0u) | (body.driven ? PI_NSF_DRIVEN : 0u) |
                  (body.facing_block ? PI_NSF_FACING : 0u) |
                  (CinematicActive() ? PI_NSF_CINEMATIC : 0u) |
                  (MenuPanelActive() ? PI_NSF_PANEL : 0u);
    facts.body_ns = body.steady_ns;
    facts.daze = body.daze;
    for (unsigned side = 0; side < 2; ++side)
        facts.arm_weight[side] = body.arm_weight[side];
    const auto decision = request_policy.Call(frame);
    if (!decision.applied) {
        // Undo already-owned equipment edits through the same inventory proofs as disable.
        for (unsigned side = 0; side < 2; ++side)
            RestoreOwnedOnFault(equipment, player_data, side);
        empty_selected = {};
        return;
    }
    for (unsigned side = 0; side < 2; ++side) {
        auto& item = remembered[side];
        if (item.slot >= 0 &&
            Read<s32>(player_data + 0x2A4 + 4 * (2 * item.slot + side)) != E::BareFists)
            item = {};
        const auto command = decision.command[side];
        if (command) {
            const bool changed = ExecuteEquipment(equipment, player_data, side, command);
            if (enabled)
                LOG_INFO(Core, "VR shoulder: {} hand {} {}", side == 0 ? "left" : "right",
                         command == PI_NSC_CYCLE     ? "native weapon cycle (no switch clip)"
                         : command == PI_NSC_SHEATHE ? "native unequip"
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
    PiShoulderInput p{};
    p.operation = PI_SHOULDER_NATIVE_REQUEST;
    p.now = now;
    p.side = side;
    p.native.flags = (installed ? PI_NSF_INSTALLED : 0u) | (current_player ? PI_NSF_PLAYER : 0u);
    p.native.unarmed = unarmed;
    return request_policy.Call(p).accepted != 0;
}

bool RequestShoulderCycle(unsigned side) {
    static const bool enabled = [] {
        const char* value = std::getenv("VR_SHOULDER_NATIVE_CYCLE");
        return !(value && value[0] == '0');
    }();
    if (side > 1)
        return false;
    const auto now = Body::SteadyNowNs();
    std::lock_guard lock(mutex);
    PiShoulderInput p{};
    p.operation = PI_SHOULDER_NATIVE_REQUEST;
    p.now = now;
    p.side = side;
    auto& facts = p.native;
    facts.cycle = 1;
    facts.flags = (installed ? PI_NSF_INSTALLED : 0u) | (current_player ? PI_NSF_PLAYER : 0u) |
                  (current_data ? PI_NSF_DATA : 0u) | (enabled ? PI_NSF_CYCLE : 0u) |
                  (empty_selected[side] ? PI_NSF_EMPTY : 0u);
    // Preserve the former freshness/pending gate before dereferencing a retained player.
    // The mutex spans plan, copied equipment reads and request commit.
    p.operation=PI_SHOULDER_NATIVE_CYCLE_PLAN;
    if(!request_policy.Call(p).accepted) return false;
    p.operation=PI_SHOULDER_NATIVE_REQUEST;
    auto* equipment = current_player ? Read<u8*>(current_player + 0x4B8) : nullptr;
    if (current_data && Plausible(equipment) && E::Valid(equipment)) {
        facts.flags |= PI_NSF_EQUIPMENT;
        facts.active_slot = Read<s32>(equipment + E::Selected + 4 * side);
        facts.other_goods =
            Read<s32>(current_data + 0x2A4 + 4 * (2 * (1 - facts.active_slot) + side));
    }
    return request_policy.Call(p).accepted != 0;
}

void CancelShoulderEquipmentRequests() {
    std::lock_guard lock(mutex);
    PiShoulderInput p{};
    p.operation = PI_SHOULDER_NATIVE_CANCEL;
    p.now = Body::SteadyNowNs();
    request_policy.Call(p);
}

bool ShoulderUnarmed(unsigned side) {
    std::lock_guard lock(mutex);
    return !request_policy.Faulted() && side < 2 && empty_selected[side];
}

int ShoulderRestoreSlot(unsigned side) {
    std::lock_guard lock(mutex);
    return !request_policy.Faulted() && side < 2 && empty_selected[side]
               ? remembered[side].return_slot
               : -1;
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
        InitRequestDll();
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
