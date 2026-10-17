// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <optional>
#include "common/logging/log.h"
#include "core/vr/bloodborne_lamps.h"
#include "core/vr/bloodborne_menu.h"
#include "core/vr/hook_util.h"
#include "core/vr/lamp_channel.h"
#include "core/vr/lamp_native.h"
#include "core/vr/lamp_sequence.h"
#include "core/vr/interaction_world.h"
#include "core/vr/interaction_lamp_catalog.h"
#include "core/vr/interaction_lamp_search.h"
#include "core/vr/vr_session.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace VR::Bloodborne {
namespace {
using Hook::Plausible;
using Hook::Read;
namespace P = VR::Lamps;
// Verified in the 1.09 executable; see documents/lamp-travel.md.
constexpr u64 WorldChr = 0x553e878, GameData = 0x553b130, GameMan = 0x5556678;
constexpr u64 TalkMan = 0x5540508, MenuMan = 0x5562878, MenuSystem = 0x5562870;
constexpr u64 FlagMan = 0x553b100, ParamMan = 0x5540340;
constexpr u64 ObjectMan = 0x553b168; // the world's objects, by map block (as bloodborne_levers.cpp)
using SetRespawnFn = void(PS4_SYSV_ABI*)(s32);
using AddEffectFn = u8(PS4_SYSV_ABI*)(void*, s32, void*, u32, u32, s32, float, float, float, float,
                                      float, s32);
using GetFlagFn = u32(PS4_SYSV_ABI*)(void*, u32);
using AddItemFn = s32(PS4_SYSV_ABI*)(u8*, u32, s32, s32, s32, s32, s32);
using RoomFn = s32(PS4_SYSV_ABI*)(const u32*);
using FindFn = s32(PS4_SYSV_ABI*)(u8*, const u32*);
struct ReturnRow {
    u64 vtable;
    s32 id{-1};
    u32 pad{};
    u8* row{};
};
using RowFn = void(PS4_SYSV_ABI*)(ReturnRow*, s32);
u8* base{};
bool ready{};
u8* offered_player{};
u8* offered_dialog{};
P::Offer offer;
P::Request request;
VR::Interactions::Module* sequence_module{};
bool sequence_fault{};
VR::Interactions::WorldRuntime sequence_runtime;
VR::Interactions::LampSequence sequence{sequence_runtime};
VR::Interactions::LampLifecycle lifecycle{sequence_runtime};
P::Pad last_pad{P::Pad::Neutral};
s64 last_receipt_ns{};
const char* refusal{};

template <class T>
T Function(u64 rva) {
    return reinterpret_cast<T>(base + rva);
}
u8* Global(u64 rva) {
    return Read<u8*>(base + rva);
}
void Refuse(const char* reason) {
    VR::PublishLampBoxHidden(false); // the game's own box answers it
    if (reason != refusal) {
        LOG_WARNING(Core, "VR lamp: refused: {}", reason);
        refusal = reason;
    }
}
void Cancel(const char* reason) {
    VR::PublishLampBoxHidden(false);
    const auto serial = lifecycle.State().offered_serial;
    if (!lifecycle.Cancel())
        return;
    LOG_INFO(Core, "VR lamp: cancelled serial {}: {}", serial, reason);
    P::Mailbox().Cancel();
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
// Event-flag reads stay native; the copied known/value masks do not unlock unknown flags.
void ReadLampFlags(PiLampCatalogInput& input, size_t index) {
    const auto& lamp = P::Table[index];
    const auto lit = Flag(lamp.Lit());
    const auto blocked = Flag(lamp.Blocked());
    const auto bit = u64{1} << index;
    if (lit) {
        input.lit_known |= bit;
        if (*lit)
            input.lit |= bit;
    }
    if (blocked) {
        input.blocked_known |= bit;
        if (*blocked)
            input.blocked |= bit;
    }
}
bool LampAvailable(const P::Lamp& lamp) {
    PiLampCatalogInput input{};
    input.operation = PI_LCAT_AVAILABLE;
    input.destination = lamp.ReturnPoint();
    const int index = PhysicalInteractions::LampCatalogPackets::Index(input.destination);
    if (index < 0)
        return false;
    ReadLampFlags(input, static_cast<size_t>(index));
    PiLampCatalogOutput output{};
    return VR::Interactions::LampCatalog(sequence_module, input, output) && output.available;
}
std::optional<std::vector<P::TravelPage>> LampPages() {
    PiLampCatalogInput input{};
    input.operation = PI_LCAT_PAGES;
    // Preserve TravelPages' group/table ordering and both flag reads for every lamp.
    for (int group = 0; group < 5; ++group)
        for (size_t i = 0; i < P::Table.size(); ++i)
            if (static_cast<int>(P::Table[i].group) == group)
                ReadLampFlags(input, i);
    PiLampCatalogOutput output{};
    if (!VR::Interactions::LampCatalog(sequence_module, input, output))
        return {};
    std::vector<P::TravelPage> pages;
    pages.reserve(output.page_count);
    for (uint32_t i = 0; i < output.page_count; ++i) {
        const auto& page = output.page[i];
        std::vector<int> ids(page.destination, page.destination + page.rows);
        pages.push_back({static_cast<P::Group>(page.group), page.number, page.count, std::move(ids)});
    }
    return pages;
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
PiLampCatalogOutput LampNativeDecision(PiLampCatalogInput input) {
    PiLampCatalogOutput output{};
    if (!VR::Interactions::LampCatalog(sequence_module, input, output)) {
        sequence_fault = true;
        output = {};
        output.player_reason = PI_LCP_FAULT;
        output.dialog_state = static_cast<uint32_t>(P::DialogState::Changed);
    }
    return output;
}
const char* LampPlayerReason(uint32_t reason) {
    static constexpr const char* text[]{nullptr, "player or managers unavailable",
        "player effect function changed", "multiplayer guest", "online/multiplayer mode",
        "player modules unavailable", "player dead or HP unavailable", "player inventory changed",
        "interaction module fault"};
    return reason <= PI_LCP_FAULT ? text[reason] : text[PI_LCP_FAULT];
}
const char* PlayerBlock(u8* player) {
    PiLampCatalogInput input{};
    input.operation = PI_LCAT_PLAYER;
    auto& facts = input.player;
    const auto decide = [&]() { return LampPlayerReason(LampNativeDecision(input).player_reason); };
    facts.globals = Globals();
    if (!facts.globals || !Plausible(player)) return decide();
    facts.current_player = Read<u8*>(Global(WorldChr) + 0x60) == player;
    if (!facts.current_player) return decide();
    u8* vtable = Read<u8*>(player);
    facts.effect_function = Plausible(vtable) && Read<u8*>(vtable + 0x3f0) == base + 0x18c6db0;
    if (!facts.effect_function) return decide();
    // Copy read-only gameplay facts after the native player/function proofs. The DLL
    // decides guest/online/HP precedence before any lamp action can be admitted.
    facts.guest = Read<s32>(player + 0x78);
    facts.online = Read<u8>(Global(GameMan) + 0x1590);
    u8* modules = Read<u8*>(player + 0x3b0);
    facts.modules = Plausible(modules);
    if (!facts.modules) return decide();
    u8* data = Read<u8*>(modules + 0x20);
    facts.data = Plausible(data);
    if (facts.data) facts.hp = Read<s32>(data + 0xf8);
    u8* pgd = Read<u8*>(Global(GameData) + 8);
    facts.inventory = Plausible(pgd) && Read<u8*>(player + 0x3c0) == pgd;
    return decide();
}
std::optional<int> Count(u8* egd, u32 item) {
    u8* inv = egd + 0x158;
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
bool Warp(u8* player) {
    if (VR::Interactions::ProcessFaultLatch().Tripped())
        return false;
    const auto decide = [](const PiLampWarpInput& input) {
        const auto output = VR::Interactions::LampWarp(sequence_runtime, input);
        if (!output.success && !output.restore)
            Refuse(VR::Interactions::LampWarpReason(output.reason));
        return output;
    };
    PiLampWarpInput facts{};
    facts.command = PI_LW_DESTINATION;
    facts.destination = request.destination;
    const auto* dest = P::ByReturnPoint(request.destination);
    facts.flags = dest && LampAvailable(*dest) ? PI_LWF_AVAILABLE : 0u;
    if (!decide(facts).success || !dest)
        return false;
    ReturnRow target{}, previous{};
    const int old = Read<s32>(Global(GameMan) + 0x1528);
    facts.command = PI_LW_ROWS;
    const bool rows = Row(request.destination, target) && Row(old, previous);
    facts.flags = rows ? PI_LWF_ROWS : 0u;
    if (!decide(facts).success || !rows)
        return false;
    facts.command = PI_LW_MAP;
    facts.area = Read<u16>(target.row);
    // Preserve the native short circuit: the block is not read after an area mismatch.
    facts.block = facts.area == dest->area ? Read<u16>(target.row + 2) : 0;
    if (!decide(facts).success)
        return false;
    u8* egd = Read<u8*>(Global(GameData) + 8) + 0x1d0;
    constexpr std::array<u32, 2> items{0x400003e8, 0x40000384}; // goods 1000, 900
    std::array<int, 2> before{}, room{};
    for (size_t i = 0; i < items.size(); ++i) {
        const auto qty = Count(egd, items[i]);
        facts.command = PI_LW_COUNT;
        facts.flags = qty ? PI_LWF_HAVE_COUNT : 0u;
        if (!decide(facts).success || !qty)
            return false;
        room[i] = Function<RoomFn>(0x1b5ca20)(&items[i]);
        facts.command = PI_LW_CAPACITY;
        facts.room = room[i];
        if (!decide(facts).success)
            return false;
        before[i] = *qty;
    }
    for (size_t i = 0; i < items.size(); ++i) {
        facts.command = PI_LW_REFILL_BEGIN;
        facts.room = room[i];
        const auto fill = decide(facts);
        if (!fill.success)
            return false;
        const int id = items[i] & 0x0fffffff;
        const int result =
            fill.invoke ? Function<AddItemFn>(0x14cdfd0)(egd, 0x40000000, id, fill.count, 1, 0, 1)
                        : 0;
        const auto after = Count(egd, items[i]);
        LOG_INFO(Core, "VR lamp: refill goods {}: {} -> {} (room {}, result {})", id, before[i],
                 after.value_or(-1), room[i], result);
        facts.command = PI_LW_REFILL_RESULT;
        facts.before = before[i];
        facts.native_result = result;
        facts.flags = after ? PI_LWF_HAVE_AFTER : 0u;
        facts.after = after.value_or(-1);
        if (!decide(facts).success)
            return false;
    }
    Function<SetRespawnFn>(0x156d110)(request.destination);
    LOG_INFO(Core, "VR lamp: native respawn setter {} -> {} (readback {})", old,
             request.destination, Read<s32>(Global(GameMan) + 0x1528));
    facts.command = PI_LW_RESPAWN_RESULT;
    facts.after = Read<s32>(Global(GameMan) + 0x1528);
    const auto respawn = decide(facts);
    if (respawn.restore)
        Function<SetRespawnFn>(0x156d110)(old);
    if (!respawn.success) {
        if (respawn.restore)
            Refuse(VR::Interactions::LampWarpReason(respawn.reason));
        return false;
    }
    const auto added =
        Function<AddEffectFn>(0x18c6db0)(player, 2101, player, 0, 1, 0, 1.f, 1.f, 1.f, 1.f, 1.f, 0);
    LOG_INFO(Core, "VR lamp: AddSpEffect 2101 -> {} (destination {}, {})", added,
             request.destination, dest->name);
    facts.command = PI_LW_EFFECT_RESULT;
    facts.effect_result = added;
    const auto effect = decide(facts);
    if (effect.restore) {
        Function<SetRespawnFn>(0x156d110)(old);
        Refuse(VR::Interactions::LampWarpReason(effect.reason));
    }
    return effect.success;
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
    PiLampSearchInput input{};
    input.player[0] = px; input.player[1] = py; input.player[2] = pz;
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
            input.best_distance = out.distance;
            std::memcpy(input.position, data.data() + 0x28, sizeof(input.position));
            std::memcpy(input.entity_words, data.data() + 0xD0, sizeof(input.entity_words));
            PiLampSearchOutput selected{};
            if (!VR::Interactions::LampSearch(sequence_module, input, selected)) return {};
            if (selected.found) {
                out.lamp = P::ByReturnPoint(selected.return_point);
                out.distance = selected.distance;
            }
        }
    }
    return out;
}

// The lamp the box belongs to: the conversation's talk id, else the lamp object you stand at,
// else (no object list) the respawn point. The respawn point alone was wrong whenever you last
// rested elsewhere (the 2026-10-14 run: at Central Yharnam's box with respawn 2412952, Rest took
// you to the Great Bridge).
const P::Lamp* CurrentLamp(int talk_id) {
    PiLampCatalogInput input{};
    input.operation = PI_LCAT_CURRENT;
    input.current.stage = PI_LCC_TALK;
    input.current.talk_id = talk_id;
    auto selected = LampNativeDecision(input);
    if (!selected.query_near) return P::ByReturnPoint(selected.return_point);
    const NearLamp found = FindNearLamp();
    const int respawn = Read<s32>(Global(GameMan) + 0x1528);
    input.current.stage = PI_LCC_RESOLVED;
    input.current.have_near = found.lamp != nullptr;
    input.current.near_return = found.lamp ? found.lamp->ReturnPoint() : 0;
    input.current.searched = found.searched;
    input.current.respawn = respawn;
    selected = LampNativeDecision(input);
    const auto* lamp = P::ByReturnPoint(selected.return_point);
    if (selected.source == PI_LCS_NEAR && lamp) {
        LOG_INFO(Core, "VR lamp: no talk id: the lamp object {:.2f} m away ({}; respawn {})",
                 found.distance, lamp->name, respawn);
    } else if (found.searched) {
        LOG_WARNING(Core, "VR lamp: no talk id and no lamp object within {:.0f} m; the respawn "
                          "point {} as before", NearLampM, respawn);
    } else {
        LOG_WARNING(Core, "VR lamp: no talk id and no object list; the respawn point {}", respawn);
    }
    return lamp;
}
const P::Lamp* offered_lamp{};
// A lantern warp in flight (VR::PublishLampWarp): the player that left; the new one arrives.
u8* warp_player{};
} // namespace

void LampsEbootLoaded(VAddr address, u64 size) {
    base = reinterpret_cast<u8*>(address);
    ready = false;
    lifecycle.Reset();
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
    if (VR::Interactions::DllRequested() && !sequence_module) {
        sequence_module = new VR::Interactions::Module();
        sequence_module->EnableFaultBroadcast();
        if(!sequence_module->LoadBesideExecutable()){sequence_fault=true;LOG_ERROR(Core,"VR lamp: DLL sequence unavailable: {}",sequence_module->Diagnostic());return;}
        sequence_runtime.Bind(*sequence_module,sequence_fault);
    }
    if(sequence_fault)return;
    ready = true;
    LOG_INFO(Core, "VR lamp: signatures verified; lamp_travel={} (offline only; no save-file IO)",
             P::Enabled());
}
void LampsSiteA(u8* player) {
    if (warp_player && player != warp_player) {
        warp_player = nullptr;
        VR::PublishLampWarp(true);
        LOG_INFO(Core, "VR lamp: arrived (a new player after the warp)");
    }
    if (!ready || !P::Enabled() || !VR::GetSettings().menu_mode) {
        Cancel("disabled");
        return;
    }
    if(VR::Interactions::ProcessFaultLatch().Tripped()){sequence_fault=true;Cancel("interaction module fault");return;}
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
    PiLampCatalogInput dialog_input{};
    dialog_input.operation = PI_LCAT_DIALOG;
    dialog_input.dialog = {modal, modal && MenuIsMessageBox(serial), Plausible(dialog),
                          dialog == offered_dialog, message, 0,
                          lifecycle.State().offered_serial, serial};
    const auto dialog_policy = LampNativeDecision(dialog_input);
    if (sequence_fault) { Cancel("interaction module fault"); return; }
    const bool lamp_box = dialog_policy.lamp_box;
    if (lifecycle.State().phase == PI_LP_IDLE) {
        PiLampLifecycleInput facts{};
        facts.command = PI_LL_OBSERVE;
        facts.serial = serial;
        facts.flags = (lamp_box ? PI_LF_LAMP_BOX : 0u) | (modal ? PI_LF_MODAL : 0u) |
                      (dialog_input.dialog.message_box ? PI_LF_MESSAGE_BOX : 0u);
        const auto observed = lifecycle.Call(facts, now);
        if (observed.reason) {
            Cancel(VR::Interactions::LampLifecycleReason(observed.reason));
            return;
        }
        if (observed.flags & PI_LI_NONLAMP) {
            VR::PublishLampBoxHidden(false);
            LOG_INFO(Core, "VR lamp: message box serial {} is not the lantern's (message {})",
                     serial, message);
        }
        if (!(observed.flags & PI_LI_INSPECT))
            return;
        LOG_INFO(Core, "VR lamp: dialog serial {} talk {} message {} ({} {}) respawn {}", serial,
                 talk_id, message, Read<s32>(dialog + 4), Read<s32>(dialog + 8),
                 Read<s32>(Global(GameMan) + 0x1528));
        const auto* lamp = CurrentLamp(talk_id);
        offered_lamp = lamp;
        facts = {};
        facts.command = PI_LL_OFFER;
        facts.serial = serial;
        facts.current = lamp ? lamp->ReturnPoint() : 0;
        if (lamp && LampAvailable(*lamp)) {
            facts.flags |= PI_LF_AVAILABLE;
            if (Read<s32>(dialog + 0x18) == -1) {
                facts.flags |= PI_LF_RESULT_CLEAR;
                facts.selection = Read<s32>(man + 0x184);
            }
        }
        const auto admitted = lifecycle.Call(facts, now);
        if (admitted.reason) {
            Refuse(VR::Interactions::LampLifecycleReason(admitted.reason));
            return;
        }
        if (!(admitted.flags & PI_LI_ACCEPT))
            return;
        auto pages = LampPages();
        if (!pages) { Cancel("lamp catalog module fault"); return; }
        offer = {serial, lamp->ReturnPoint(), std::move(*pages)};
        facts = {};
        facts.command = PI_LL_DESTINATIONS;
        facts.serial = serial;
        for (const auto& page : offer.pages)
            for (int id : page.destinations)
                for (size_t i = 0; i < P::Table.size(); ++i)
                    if (P::Table[i].ReturnPoint() == id)
                        facts.destinations |= u64{1} << i;
        const auto destinations = lifecycle.Call(facts, now);
        if (destinations.reason) {
            Cancel(VR::Interactions::LampLifecycleReason(destinations.reason));
            return;
        }
        offered_player = player;
        offered_dialog = dialog;
        refusal = nullptr;
        P::Mailbox().Publish(offer);
        VR::PublishLampBoxHidden(true);
        LOG_INFO(Core, "VR lamp: offer {} ({} travel pages)", lamp->name, offer.pages.size());
        return;
    }
    PiLampLifecycleInput facts{};
    facts.command = PI_LL_ACTIVE;
    facts.flags = player == offered_player ? PI_LF_SAME_PLAYER : 0u;
    if (player == offered_player && P::Mailbox().Active(lifecycle.State().offered_serial))
        facts.flags |= PI_LF_ACTIVE;
    const auto active = lifecycle.Call(facts, now);
    if (active.reason) {
        Cancel(VR::Interactions::LampLifecycleReason(active.reason));
        return;
    }
    if (active.flags & PI_LI_HIDE)
        VR::PublishLampBoxHidden(true);
    if (lifecycle.State().phase == PI_LP_OFFERING) {
        facts = {};
        facts.command = PI_LL_OFFERING;
        facts.serial = serial;
        PiLampCatalogInput same_input{};
        same_input.operation = PI_LCAT_CURRENT;
        same_input.current.stage = PI_LCC_SAME;
        same_input.current.talk_id = talk_id;
        same_input.current.offered_known = offered_lamp != nullptr;
        same_input.current.offered_return = offered_lamp ? offered_lamp->ReturnPoint() : 0;
        same_input.current.offer_current = offer.current;
        const auto same_lamp = LampNativeDecision(same_input).same_lamp;
        facts.flags =
            (P::Mailbox().Presented(lifecycle.State().offered_serial) ? PI_LF_PRESENTED : 0u) |
            (lamp_box ? PI_LF_LAMP_BOX : 0u);
        if (same_lamp)
            facts.flags |= PI_LF_SAME_LAMP;
        const auto offering = lifecycle.Call(facts, now);
        if (offering.reason) {
            Cancel(VR::Interactions::LampLifecycleReason(offering.reason));
            return;
        }
        if (!(offering.flags & PI_LI_TAKE_REQUEST))
            return;
        P::Mailbox().Output(P::Pad::Neutral);
        if (auto pending = P::Mailbox().TakeRequest()) {
            facts = {};
            facts.command = PI_LL_REQUEST;
            facts.request_serial = pending->serial;
            facts.action = static_cast<int>(pending->action);
            facts.destination = pending->destination;
            const auto accepted = lifecycle.Call(facts, now);
            if (accepted.reason) {
                Cancel(VR::Interactions::LampLifecycleReason(accepted.reason));
                return;
            }
            if (!(accepted.flags & PI_LI_ACCEPT))
                return;
            request = {accepted.offered_serial, static_cast<P::Action>(accepted.action),
                       accepted.destination};
            sequence.Start(request.action, accepted.selection, now);
            last_pad = P::Pad::Neutral;
            last_receipt_ns = 0;
            P::Mailbox().Output(P::Pad::Neutral, true);
            LOG_INFO(Core, "VR lamp: action {} destination {} serial {}",
                     static_cast<int>(request.action), request.destination, request.serial);
        }
        return;
    }
    if (lifecycle.State().phase == PI_LP_RESPONDING) {
        const bool talking = Read<u8>(talk + 0x3ff8) || Read<s32>(talk + 0x3ffc) != -1 ||
                             Read<float>(talk + 0x400c) > 0.f;
        const bool same_dialog = dialog_policy.same_box;
        const auto state = static_cast<P::DialogState>(dialog_policy.dialog_state);
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
            warp_player = player;
            VR::PublishLampWarp(false);
            facts = {};
            facts.command = PI_LL_WARP_RESULT;
            facts.flags = PI_LF_WARP_OK;
            const auto warped = lifecycle.Call(facts, now);
            if (warped.reason) {
                Cancel(VR::Interactions::LampLifecycleReason(warped.reason));
                return;
            }
            if (warped.flags & PI_LI_NEUTRAL)
                P::Mailbox().Output(P::Pad::Neutral, true);
            break;
        }
    } else if (lifecycle.State().phase == PI_LP_WARP) {
        facts = {};
        facts.command = PI_LL_WARP_TICK;
        const auto warped = lifecycle.Call(facts, now);
        if (warped.flags & PI_LI_NEUTRAL)
            P::Mailbox().Output(P::Pad::Neutral);
        if (warped.reason)
            Cancel(VR::Interactions::LampLifecycleReason(warped.reason));
    }
}
} // namespace VR::Bloodborne
