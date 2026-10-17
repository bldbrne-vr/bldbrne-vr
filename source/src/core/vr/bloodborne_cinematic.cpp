// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

#include "common/logging/log.h"
#include "core/vr/bloodborne_cinematic.h"
#include "core/vr/bloodborne_layout.h"
#include "core/vr/cinematic_state.h"
#include "core/vr/hook_util.h"
#include "core/vr/quick_channel.h"
#include "core/vr/vr_session.h"

namespace VR::Bloodborne {

namespace {

namespace Cine = VR::Cinematic;
using Hook::Plausible;
using Hook::Read;
using namespace Layout;

// Engine code behind each signal (module-relative addresses in the 1.09 eboot). Not patched,
// only compared: a mismatch turns that signal off.
// SprjChrEventModule::IsEventAnim: ([[bag+8]+0x10] & 2) >> 1 while the behaviour module is ready.
constexpr u64 IsEventAnimRva = 0x1A59210;
constexpr std::array<u8, 41> IsEventAnimSig = {
    0x48, 0x8B, 0x47, 0x08,                   // mov rax, [rdi+0x08]
    0x48, 0x8B, 0x80, 0xB0, 0x03, 0x00, 0x00, // mov rax, [rax+0x3B0]
    0x48, 0x8B, 0x48, 0x30,                   // mov rcx, [rax+0x30]
    0x48, 0x85, 0xC9, 0x74, 0x15,             // test rcx, rcx; jz
    0x80, 0xB9, 0x84, 0x03, 0x00, 0x00, 0x00, // cmp byte [rcx+0x384], 0
    0x74, 0x0F,                               // jz
    0x48, 0x8B, 0x40, 0x08,                   // mov rax, [rax+0x08]
    0x8A, 0x40, 0x10,                         // mov al, [rax+0x10]
    0x24, 0x02, 0xD0, 0xE8, 0xC3,             // and al, 2; shr al, 1; ret
};
// HKS act 8 ("in event animation", the only writer of the bit; Act8Sig) is in
// bloodborne_layout.h, shared with the VR body's animation gate.
// HKS env 0x65 "in a throw?" (and env 0x07 at the second site): the whole module chain to the
// ThrowNode state. -1 = any byte.
constexpr u64 ThrowEnvRva = 0x1A14E44;
constexpr u64 ThrowEnv2Rva = 0x1A13972;
constexpr std::array<s16, 41> ThrowEnvSig = {
    0x49, 0x8B, 0x80, 0xB0, 0x03, 0x00, 0x00, // mov rax, [r8+0x3B0]
    0x48, 0x8B, 0x80, 0x88, 0x00, 0x00, 0x00, // mov rax, [rax+0x88]
    0x48, 0x8B, 0x40, 0x28,                   // mov rax, [rax+0x28]
    0x48, 0x85, 0xC0,                         // test rax, rax
    0x48, 0x8B, 0x3D, -1,   -1,   -1,   -1,   // mov rdi, [rip+disp]
    0x0F, 0x84, -1,   -1,   -1,   -1,         // jz
    0x80, 0x78, 0x20, 0x00,                   // cmp byte [rax+0x20], 0
    0x0F, 0x94, 0xC0,                         // setz al
};
// ChrThrowModule::IsInThrow (IsInThrowSig) is in bloodborne_layout.h.
// SprjChrEventModule::SetEventAnim: writes the request and the sticky copy [[chr+0x20]+4].
constexpr u64 SetEventAnimRva = 0x1A59190;
constexpr std::array<u8, 40> SetEventAnimSig = {
    0x48, 0x8B, 0x47, 0x08,                   // mov rax, [rdi+0x08]
    0x48, 0x8B, 0x88, 0xB0, 0x03, 0x00, 0x00, // mov rcx, [rax+0x3B0]
    0x48, 0x8B, 0x49, 0x30,                   // mov rcx, [rcx+0x30]
    0x48, 0x85, 0xC9, 0x74, 0x13,             // test rcx, rcx; jz
    0x80, 0xB9, 0x84, 0x03, 0x00, 0x00, 0x00, // cmp byte [rcx+0x384], 0
    0x74, 0x0A,                               // jz
    0x89, 0x77, 0x20,                         // mov [rdi+0x20], esi
    0x48, 0x8B, 0x40, 0x20,                   // mov rax, [rax+0x20]
    0x89, 0x70, 0x04, 0xC3,                   // mov [rax+4], esi; ret
};
// The TimeAct history push (TimeActPushSig) and HKS act 26 (Act26Sig, the ladder state byte) are
// in bloodborne_layout.h, shared with the dodge comfort camera's motion signals.
// HKS env 99 / env 100 (on the top / bottom rung?): the chain to the ladder object
// [[bag+0x58]+0x28] (through the ladder module's owner) and its rung fields. -1 = any byte.
constexpr u64 Env99Rva = 0x1A14D9C;
constexpr u64 Env100Rva = 0x1A14DF1;
constexpr std::array<s16, 68> LadderEnvSig = {
    0x41, 0x0F, 0xBE, 0x47, 0x60,             // movsx eax, byte [r15+0x60]
    0x83, 0xE0, 0xFE,                         // and eax, ~1
    0xBE, 0x00, 0x00, 0x00, 0x00,             // mov esi, 0
    0x83, 0xF8, 0x02,                         // cmp eax, 2 (not while stepping)
    0x48, 0x8B, 0x3D, -1,   -1,   -1,   -1,   // mov rdi, [rip+disp]
    0x0F, 0x84, -1,   -1,   -1,   -1,         // jz
    0x49, 0x8B, 0x80, 0xB0, 0x03, 0x00, 0x00, // mov rax, [r8+0x3B0]
    0x48, 0x8B, 0x40, 0x78,                   // mov rax, [rax+0x78]
    0x48, 0x8B, 0x40, 0x08,                   // mov rax, [rax+0x08]
    0x48, 0x8B, 0x80, 0xB0, 0x03, 0x00, 0x00, // mov rax, [rax+0x3B0]
    0x48, 0x8B, 0x40, 0x58,                   // mov rax, [rax+0x58]
    0x48, 0x8B, 0x40, 0x28,                   // mov rax, [rax+0x28]
    0x48, 0x85, 0xC0,                         // test rax, rax
    0x0F, 0x84, -1,   -1,   -1,   -1,         // jz
};
constexpr std::array<u8, 6> Env99Tail = {
    0x8B, 0x48, 0x04, // mov ecx, [rax+4]  (rung)
    0x3B, 0x48, 0x08, // cmp ecx, [rax+8]  (top)
};
constexpr std::array<u8, 4> Env100Tail = {
    0x83, 0x78, 0x04,
    0x00, // cmp dword [rax+4], 0
};

// Runtime layout (the module bag, the TimeAct ring, the ladder action state, the action flags,
// the throw module and HP are in bloodborne_layout.h). The modules' vtables and owners are checked
// before their signals are used.
constexpr u32 OffsetBackup = 0x20; // PlayerIns -> {ThrowAnimId, EventAnimId, loop} (sticky)
constexpr u32 OffsetBackupEventAnim = 0x04;
constexpr u32 OffsetThrowParamId = 0x18; // -1 when idle
constexpr u32 OffsetThrowPair = 0x24;    // partner ChrIns handle, -1 = none
constexpr u32 OffsetThrowAligning = 0x64;
constexpr u32 OffsetMaxHp = 0xFC; // ChrDataModule: the ladder damage threshold
// Ladders: the ladder state byte is in the HKS action state (bloodborne_layout.h); the ladder
// object (0x60 bytes, no vtable) holds the rung counter.
constexpr u64 EventModuleVtableRva = 0x53358B0; // SprjChrEventModule
constexpr u32 OffsetEventModule = 0x58;
constexpr u32 OffsetLadderObject = 0x28;      // EventModule -> ladder object
constexpr u32 OffsetLadderObjectState = 0x00; // 1 on the ladder, 2 getting on, 0 none
constexpr u32 OffsetLadderRung = 0x04;        // 0 = bottom
constexpr u32 OffsetLadderTop = 0x08;

u8* g_base = nullptr;
u64 g_size = 0;
// Signals still on (install checks, then the runtime checks).
bool g_event = false;
bool g_throw = false;
bool g_request_ids = false;    // [[PlayerIns+0x20]+4]
bool g_ring_ids = false;       // TimeAct entries
bool g_ladder = false;         // the ladder state byte (cinematic_ladder)
bool g_ladder_predict = false; // the ladder object (length prediction)
bool g_cancel = false;         // the cancel windows (action request module): control back
// cinematic_item: a canned item use through our own Square (the Shaman Bone Blade's stab), played
// while the action flags' item-use bit is on (Cine::ItemUseGate, armed by the presenter's
// VR::Quick::NoteCannedUse).
bool g_item = false;
constexpr u8 FlagItemUse = 0x40; // action flags: an item-use state (bloodborne_layout.h)
Cine::ItemUseGate g_item_gate{};
VR::Quick::CannedUse g_canned{}; // the latest arm seen
int g_log = 0;                 // VR_CINE_LOG: 1 = signal edges, 2 = also a state line every second
bool g_last_control = false;
// The cancel window as the main player's pose copy read it (NoteCancelWindow, another thread):
// at site A the module still reads empty during throws (the frame's TAE step comes later).
constexpr s64 CancelWindowMaxAgeNs = 150 * Cine::Millis;
std::atomic<u64> g_cw_allow{0};
std::atomic<bool> g_cw_move{false};
std::atomic<const u8*> g_cw_player{nullptr};
std::atomic<s64> g_cw_ns{0};

s64 SteadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
// Main-player update (site A: one update at a time, usually on a job worker thread, not the loop
// thread that places the camera; the machine's phase reaches the other threads through
// VR::UpdateCinematic's atomics).
bool g_checked = false; // the first-update checks ran
bool g_disabled_logged = false;
u8 g_last_flags = 0;
u8 g_last_ts = 0;
u8 g_last_align = 0;
bool g_last_event = false;
bool g_last_raw = false;
Cine::Phase g_last_phase = Cine::Phase::Off;
const u8* g_last_player = nullptr; // a new main player object ends any flat period at once
u64 g_updates = 0;
u64 g_logged_updates = 0;
std::chrono::steady_clock::time_point g_last_state{};
// Ladder checks and log (main-player update).
bool g_ladder_checked = false; // the first-update ladder checks ran
s8 g_last_ladder = -1;
s32 g_last_object_state = 0;
s32 g_last_rung = 0;
s32 g_last_top = 0;
s32 g_last_ladder_clip = -1;
s32 g_last_dir = 0;
bool g_last_armed = false;
bool g_last_ladder_flat = false;
bool g_last_mounting = false;
std::chrono::steady_clock::time_point g_ladder_push{}; // the last ladder clip push
std::chrono::steady_clock::time_point g_lead_exit{};   // the last exit ahead of the dismount
bool g_lead_pending = false; // a lead exit whose dismount clip has not been pushed yet

// One update's readings (zero / -1 where a pointer is not plausible).
struct Sample {
    u8* modules{};
    u8* action_flags{};
    u8* behavior{};
    u8* time_act{};
    u8* throw_module{};
    u8* node{};
    bool ready{false};
    u8 flags{0};
    u8 ts{0};
    u8 align{0};
    s32 throw_param{-1};
    s32 pair{-1};
    s32 requested{-1};
    bool ring{false};
    s32 write{-1};
    s32 read{-1};
    std::array<s32, TimeActRingSize> pending{};
    u32 pending_count{0};
    s32 newest{-1};
    bool hp_valid{false};
    s32 hp{0};
    s32 hp_max{0};
    // The cancel window (the requests the playing clip accepts this update).
    bool cancel{false};
    u64 allow{0};
    bool move{false};
    // Ladder.
    u8* ctrl{};
    u8* action_state{};
    u8* event_module{};
    u8* ladder_object{};
    s8 ladder{-1};
    s32 object_state{0};
    s32 rung{0};
    s32 top{0};
    s32 ladder_clip{-1}; // PickLadderClip of the pending entries
};

Sample ReadSample(const u8* player) {
    Sample s{};
    s.modules = Read<u8*>(player + OffsetChrModules);
    if (!Plausible(s.modules)) {
        s.modules = nullptr;
        return s;
    }
    const auto module = [&s](u32 offset) -> u8* {
        u8* const pointer = Read<u8*>(s.modules + offset);
        return Plausible(pointer) ? pointer : nullptr;
    };
    s.action_flags = module(OffsetActionFlagModule);
    s.behavior = module(OffsetBehaviorModule);
    s.time_act = module(OffsetTimeActModule);
    s.throw_module = module(OffsetThrowModule);
    if (s.behavior != nullptr) {
        s.ready = Read<u8>(s.behavior + OffsetBehaviorReady) != 0;
    }
    if (s.action_flags != nullptr) {
        s.flags = Read<u8>(s.action_flags + OffsetActionFlags);
    }
    if (s.throw_module != nullptr) {
        u8* const node = Read<u8*>(s.throw_module + OffsetThrowNode);
        s.node = Plausible(node) ? node : nullptr;
    }
    if (s.node != nullptr) {
        s.ts = Read<u8>(s.node + OffsetThrowState);
        s.align = Read<u8>(s.node + OffsetThrowAligning);
        s.throw_param = Read<s32>(s.node + OffsetThrowParamId);
        s.pair = Read<s32>(s.node + OffsetThrowPair);
    }
    if (u8* const backup = Read<u8*>(player + OffsetBackup); Plausible(backup, 4)) {
        s.requested = Read<s32>(backup + OffsetBackupEventAnim);
    }
    if (s.time_act != nullptr) {
        s.write = Read<s32>(s.time_act + OffsetTimeActWrite);
        s.read = Read<s32>(s.time_act + OffsetTimeActRead);
        if (s.write >= 0 && s.write < TimeActRingSize && s.read >= 0 && s.read < TimeActRingSize) {
            s.ring = true;
            const auto entry = [&s](s32 index) {
                return Read<s32>(s.time_act + OffsetTimeActRing + index * TimeActEntrySize);
            };
            for (s32 i = s.read; i != s.write; i = (i + 1) % TimeActRingSize) {
                s.pending[s.pending_count++] = entry(i);
            }
            s.newest = entry((s.write + TimeActRingSize - 1) % TimeActRingSize);
        }
    }
    if (u8* const data = module(OffsetDataModule); data != nullptr) {
        s.hp_valid = true;
        s.hp = Read<s32>(data + OffsetHp);
        s.hp_max = Read<s32>(data + OffsetMaxHp);
    }
    if (g_cancel) {
        const s64 noted = g_cw_ns.load(std::memory_order_acquire);
        if (noted != 0 && g_cw_player.load(std::memory_order_relaxed) == player &&
            SteadyNs() - noted <= CancelWindowMaxAgeNs) {
            s.cancel = true;
            s.allow = g_cw_allow.load(std::memory_order_relaxed);
            s.move = g_cw_move.load(std::memory_order_relaxed);
        }
    }
    if (g_ladder) {
        if (u8* const ctrl = Read<u8*>(player + OffsetChrCtrl); Plausible(ctrl)) {
            s.ctrl = ctrl;
            if (u8* const state = Read<u8*>(ctrl + OffsetActionState); Plausible(state)) {
                s.action_state = state;
                s.ladder = Read<s8>(state + OffsetLadderState);
            }
        }
        s.event_module = module(OffsetEventModule);
        if (s.event_module != nullptr) {
            u8* const object = Read<u8*>(s.event_module + OffsetLadderObject);
            s.ladder_object = Plausible(object, 4) ? object : nullptr;
        }
        if (s.ladder_object != nullptr) {
            s.object_state = Read<s32>(s.ladder_object + OffsetLadderObjectState);
            s.rung = Read<s32>(s.ladder_object + OffsetLadderRung);
            s.top = Read<s32>(s.ladder_object + OffsetLadderTop);
        }
        if (g_ring_ids) {
            s.ladder_clip = Cine::PickLadderClip(s.pending.data(), s.pending_count);
        }
    }
    return s;
}

// "+0x53342c0" for an address in the eboot, else the pointer.
std::string Rva(const u8* pointer) {
    if (pointer >= g_base && pointer < g_base + g_size) {
        return fmt::format("+{:#x}", static_cast<u64>(pointer - g_base));
    }
    return fmt::format("{}", fmt::ptr(pointer));
}

std::string Vtable(const u8* module) {
    return module != nullptr ? Rva(Read<u8*>(module)) : std::string{"none"};
}

std::string Owner(const u8* object, const u8* player) {
    if (object == nullptr) {
        return "none";
    }
    const u8* const owner = Read<u8*>(object + OffsetOwner);
    return owner == player ? std::string{"P"} : fmt::format("{}", fmt::ptr(owner));
}

std::string Describe(const Sample& s) {
    std::string ids;
    for (u32 i = 0; i < s.pending_count; ++i) {
        ids += fmt::format("{}{}", i == 0 ? "" : ",", s.pending[i]);
    }
    std::string ladder;
    if (g_ladder) {
        ladder = fmt::format(" ls={} L={} {}/{}", static_cast<s32>(s.ladder), s.object_state,
                             s.rung, s.top);
    }
    return fmt::format(
        "flags={:#04x} ts={} al={} tid={} pair={} evReq={} ta=[{}] hp={} cw={}{}",
        static_cast<u32>(s.flags), static_cast<u32>(s.ts), static_cast<u32>(s.align), s.throw_param,
        s.pair, s.requested, ids,
        s.hp_valid ? fmt::format("{}/{}", s.hp, s.hp_max) : std::string{"?"},
        s.cancel ? fmt::format("{:#x}/m{}", s.allow, s.move ? 1 : 0) : std::string{"?"}, ladder);
}

std::string MsSince(s64 from, s64 to) {
    return from != 0 && to != 0 ? std::to_string((to - from) / Cine::Millis) : std::string{"-"};
}

// The way back of a flat period (Returning -> Off), timed from the moment the game handed control
// back (its cancel window; without one, from the end of the request): when first-person VR showed
// and when it was fully up and the gameplay controls back, and the parts that took it.
void LogReturn(const Cine::ReturnTimes& t) {
    const auto& settings = VR::GetSettings();
    const s64 base = t.control != 0 ? t.control : t.request_end != 0 ? t.request_end : t.exit;
    const s64 full = t.projection != 0 ? t.projection + t.return_fade : 0;
    LOG_INFO(Core,
             "VR cine: RETURN cat={} ({}) from {}: VR shown +{} ms, full +{} ms, controls +{} ms "
             "| signal end lag {} ms, exit hold {} ms, fade back {} ms, first VR frame {} ms "
             "later, fade in {} ms | the animation's signals {} | flat {:.2f} s | release={} "
             "exit={:.0f} fade={:.0f} return_fade={:.0f}",
             Cine::CategoryName(t.source), t.why,
             t.control != 0 ? "control back (cancel window)" : "the request's end",
             MsSince(base, t.projection), MsSince(base, full), MsSince(base, t.off),
             MsSince(base, t.request_end), MsSince(t.request_end, t.exit),
             MsSince(t.exit, t.returning), MsSince(t.returning, t.projection),
             t.return_fade / Cine::Millis,
             t.signals_off != 0 ? fmt::format("off +{} ms", (t.signals_off - base) / Cine::Millis)
                                : std::string{"still on (ignored until they end)"},
             t.start != 0 && t.exit != 0 ? static_cast<double>(t.exit - t.start) * 1e-9 : 0.0,
             settings.cinematic_release ? 1 : 0, settings.cinematic_exit_ms,
             settings.cinematic_fade_ms, settings.cinematic_return_fade_ms);
}

// The animation id for the log: the event animation, or for a throw the clip it activated
// (100033xxx when the hunter throws, 9xxx when thrown).
s32 LoggedId(const Sample& s, const Cine::Step& step, const Cine::Signals& signals) {
    if (step.category == Cine::Category::Ladder) {
        return step.ladder.clip;
    }
    if (step.category == Cine::Category::Item) {
        return g_canned.clip; // the canned item's use clip (a00)
    }
    if (step.category != Cine::Category::Visceral && step.category != Cine::Category::Grabbed) {
        return signals.event_id;
    }
    return s.pending_count != 0 ? s.pending[0] : s.newest;
}

void LogDisabled(const char* reason) {
    if (!g_disabled_logged) {
        g_disabled_logged = true;
        LOG_WARNING(Core, "VR cine: DISABLED ({}); scripted animations stay in VR", reason);
    }
}

// The module vtables and owner fields (first update, then every rising edge). A mismatch turns
// that signal off for the session.
void CheckModules(const u8* player, const Sample& s) {
    if ((g_event || g_item) && s.action_flags != nullptr &&
        Read<u8*>(s.action_flags) != g_base + ActionFlagVtableRva) {
        g_event = g_item = false;
        LOG_WARNING(Core,
                    "VR cine: WARNING action-flag module {} has vtable {} (expected +{:#x}); "
                    "event animation and item use signals off",
                    fmt::ptr(s.action_flags), Vtable(s.action_flags), ActionFlagVtableRva);
    }
    if (g_throw && s.throw_module != nullptr) {
        const bool vtable_ok = Read<u8*>(s.throw_module) == g_base + ThrowVtableRva;
        const bool owner_ok = Read<u8*>(s.throw_module + OffsetOwner) == player;
        const bool node_ok = s.node == nullptr || Read<u8*>(s.node + OffsetOwner) == player;
        if (!vtable_ok || !owner_ok || !node_ok) {
            g_throw = false;
            LOG_WARNING(Core,
                        "VR cine: WARNING throw module {} vtable {} (expected +{:#x}) owner {} "
                        "node owner {}; throw signal off",
                        fmt::ptr(s.throw_module), Vtable(s.throw_module), ThrowVtableRva,
                        Owner(s.throw_module, player), Owner(s.node, player));
        }
    }
    if (g_ring_ids && s.time_act != nullptr && Read<u8*>(s.time_act) != g_base + TimeActVtableRva) {
        g_ring_ids = false;
        LOG_WARNING(Core,
                    "VR cine: WARNING TimeAct module {} has vtable {} (expected +{:#x}); its "
                    "animation ids are not used",
                    fmt::ptr(s.time_act), Vtable(s.time_act), TimeActVtableRva);
    }
    if (!g_event && !g_throw && !g_ladder) {
        LogDisabled("runtime module checks failed");
    }
}

// The ladder layout (the first update the action state is readable, then every time the hunter
// gets on a ladder). A wrong action state turns the ladder category off for the session, a wrong
// event module the length prediction.
void CheckLadder(const u8* player, const Sample& s) {
    if (g_ladder && s.action_state != nullptr) {
        const bool vtable_ok = Read<u8*>(s.action_state) == g_base + ActionStateVtableRva;
        const bool owner_ok = Read<u8*>(s.action_state + OffsetOwner) == s.ctrl;
        if (!vtable_ok || !owner_ok) {
            g_ladder = false;
            LOG_WARNING(Core,
                        "VR cine: WARNING ladder action state {} vtable {} (expected +{:#x}) "
                        "owner {} (ChrCtrl {}); ladder category off (act26@1A11A67)",
                        fmt::ptr(s.action_state), Vtable(s.action_state), ActionStateVtableRva,
                        fmt::ptr(Read<u8*>(s.action_state + OffsetOwner)), fmt::ptr(s.ctrl));
        }
    }
    if (g_ladder_predict && s.event_module != nullptr) {
        const bool vtable_ok = Read<u8*>(s.event_module) == g_base + EventModuleVtableRva;
        const bool owner_ok = Read<u8*>(s.event_module + OffsetOwner) == player;
        if (!vtable_ok || !owner_ok) {
            g_ladder_predict = false;
            LOG_WARNING(Core,
                        "VR cine: WARNING event module {} vtable {} (expected +{:#x}) owner {}; "
                        "ladder length prediction off (env99@1A14D9C)",
                        fmt::ptr(s.event_module), Vtable(s.event_module), EventModuleVtableRva,
                        Owner(s.event_module, player));
        }
    }
    if (!g_event && !g_throw && !g_ladder) {
        LogDisabled("runtime module checks failed");
    }
}

void LogLadderFirst(const u8* player, const Sample& s) {
    LOG_INFO(
        Core, "VR cine: ladder first X={} vt={} owner={} ev vt={} owner={} ls={} Ls={} rung={}/{}",
        fmt::ptr(s.action_state), Vtable(s.action_state),
        s.action_state != nullptr && Read<u8*>(s.action_state + OffsetOwner) == s.ctrl ? "ok"
                                                                                       : "MISMATCH",
        Vtable(s.event_module), Owner(s.event_module, player), static_cast<s32>(s.ladder),
        s.object_state, s.rung, s.top);
}

const char* DirName(s32 dir) {
    return dir > 0 ? "up" : dir < 0 ? "down" : "?";
}

std::string MsText(s64 ns) {
    return ns < 0 ? std::string{"?"} : std::to_string(ns / Cine::Millis);
}

// The ladder lines: enter / skip / exit always (rare, and the data the thresholds are tuned
// from), every change of the readings or the gate with VR_CINE_LOG.
void LogLadder(const Sample& s, const Cine::Step& step, Cine::Phase phase) {
    const Cine::LadderOut& l = step.ladder;
    const auto now = std::chrono::steady_clock::now();
    const auto since = [now](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - t).count();
    };
    const Cine::LadderClip pushed =
        l.clip_pushed ? Cine::ClassifyLadderClip(l.clip) : Cine::LadderClip::None;
    if (l.clip_pushed) {
        g_ladder_push = now;
    }
    if (l.unknown_state) {
        LOG_INFO(Core, "VR cine: LADDER unexpected ls={} (counted as idle) rung={}/{}",
                 static_cast<s32>(s.ladder), s.rung, s.top);
    }
    if (pushed == Cine::LadderClip::Unexpected) {
        LOG_INFO(Core, "VR cine: LADDER unexpected clip {} (no HKS state plays it) ls={}", l.clip,
                 static_cast<s32>(s.ladder));
    }
    if (l.bind) {
        LOG_INFO(Core,
                 "VR cine: LADDER damage bind: ls=-1 while on the ladder (Ls={} rung={}/{}) "
                 "clip={} hp {}->{}; the climb goes on{}",
                 s.object_state, s.rung, s.top, l.clip, l.hp_before, s.hp_valid ? s.hp : -1,
                 l.event == Cine::LadderEvent::Exit ? " (back to VR: combat exit)" : "");
    }
    if (l.drain && l.session && g_log >= 1) {
        LOG_INFO(Core,
                 "VR cine: LADDER hp {}->{} lost less than {} (cinematic_ladder_damage_pct of max "
                 "{}): a drain, not damage",
                 l.hp_before, s.hp, l.damage_min, s.hp_max);
    }
    if (l.mounting) {
        g_lead_pending = false; // a new climb (the last one ended without a dismount clip)
    }
    if (g_lead_pending && Cine::IsLadderEnd(pushed)) {
        g_lead_pending = false;
        LOG_INFO(Core, "VR cine: LADDER {} ({}) pushed {} ms after the lead exit, phase={}", l.clip,
                 Cine::LadderClipName(pushed), since(g_lead_exit), Cine::PhaseName(phase));
    }
    switch (l.event) {
    case Cine::LadderEvent::Enter:
        LOG_INFO(Core, "VR cine: LADDER enter t_climb={}ms pred={}ms rung={}/{} dir={} steps={}{}",
                 MsText(l.climb_ns), MsText(l.pred_ns), s.rung, s.top, DirName(l.dir), l.steps,
                 l.pred_ns < 0 ? " (length unknown)" : "");
        break;
    case Cine::LadderEvent::Skip: {
        const auto& settings = VR::GetSettings();
        LOG_INFO(Core, "VR cine: LADDER skip pred={}ms < min {:.0f} + lead {:.0f} (rung {}/{} {})",
                 MsText(l.pred_ns), settings.cinematic_ladder_min_flat_ms,
                 settings.cinematic_ladder_exit_lead_ms, s.rung, s.top, DirName(l.dir));
        break;
    }
    case Cine::LadderEvent::Exit: {
        const bool lead = !l.cut;
        g_lead_pending = lead;
        if (lead) {
            g_lead_exit = now;
        }
        LOG_INFO(Core,
                 "VR cine: LADDER exit reason={}{} clip={} ls={} rung={}/{} pred={}ms "
                 "lag_from_push={}ms phase={} hp={}",
                 l.reason,
                 std::strcmp(l.reason, "damage") == 0
                     ? fmt::format("(hp {}->{} min {})", l.hp_before, s.hp_valid ? s.hp : -1,
                                   l.damage_min)
                     : std::string{},
                 l.clip, static_cast<s32>(s.ladder), s.rung, s.top, MsText(l.pred_ns),
                 since(g_ladder_push), Cine::PhaseName(phase),
                 s.hp_valid ? std::to_string(s.hp) : std::string{"?"});
        break;
    }
    case Cine::LadderEvent::None:
        break;
    }
    if (g_log >= 1 &&
        (s.ladder != g_last_ladder || s.object_state != g_last_object_state ||
         s.rung != g_last_rung || s.top != g_last_top || l.clip != g_last_ladder_clip ||
         l.dir != g_last_dir || l.armed != g_last_armed || l.flat != g_last_ladder_flat ||
         l.mounting != g_last_mounting)) {
        LOG_INFO(Core,
                 "VR cine: LADDER ls={}->{} Ls={}->{} rung={}/{} dir={} clip={}{} ({}) {} "
                 "t_climb={}ms pred={}ms steps_left={} steps={} step={}ms{}{} hp={} phase={}",
                 static_cast<s32>(g_last_ladder), static_cast<s32>(s.ladder), g_last_object_state,
                 s.object_state, s.rung, s.top, DirName(l.dir), l.clip, l.clip_pushed ? " new" : "",
                 Cine::LadderClipName(Cine::ClassifyLadderClip(l.clip)),
                 l.mounting  ? "mount"
                 : l.flat    ? "flat"
                 : l.armed   ? "armed"
                 : l.session ? "idle"
                             : "off",
                 MsText(l.climb_ns), MsText(l.pred_ns), l.steps_left, l.steps, MsText(l.step_ns),
                 l.suppressed ? " suppressed" : "",
                 l.event == Cine::LadderEvent::None && l.reason[0] != '\0'
                     ? fmt::format(" disarmed={}", l.reason)
                     : std::string{},
                 s.hp_valid ? std::to_string(s.hp) : std::string{"?"}, Cine::PhaseName(phase));
    }
    g_last_ladder = s.ladder;
    g_last_object_state = s.object_state;
    g_last_rung = s.rung;
    g_last_top = s.top;
    g_last_ladder_clip = l.clip;
    g_last_dir = l.dir;
    g_last_armed = l.armed;
    g_last_ladder_flat = l.flat;
    g_last_mounting = l.mounting;
}

void LogFirst(const u8* player, const Sample& s) {
    LOG_INFO(Core,
             "VR cine: first P={} af vt={} ready={} thr vt={} owner={} node owner={} ts={} ta "
             "vt={} w/r={}/{} evReq={} hp={}",
             fmt::ptr(player), Vtable(s.action_flags), s.ready ? 1 : 0, Vtable(s.throw_module),
             Owner(s.throw_module, player), Owner(s.node, player), static_cast<u32>(s.ts),
             Vtable(s.time_act), s.write, s.read, s.requested,
             s.hp_valid ? std::to_string(s.hp) : std::string{"?"});
}

} // Anonymous namespace

void NoteCancelWindow(const u8* player, u64 allow, bool move) {
    g_cw_allow.store(allow, std::memory_order_relaxed);
    g_cw_move.store(move, std::memory_order_relaxed);
    g_cw_player.store(player, std::memory_order_relaxed);
    g_cw_ns.store(SteadyNs(), std::memory_order_release);
}

bool InitCinematic(VAddr base, u64 size, bool site_ok, bool main_ok) {
    auto* const module = reinterpret_cast<u8*>(base);
    g_base = module;
    g_size = size;
    if (size < ActionFlagVtableRva + sizeof(u64)) {
        LOG_INFO(Core, "VR cine: eboot too small ({:#x}) for Bloodborne 1.09; off", size);
        return false;
    }
    const auto matches = [module](u64 rva, const auto& signature) {
        return std::memcmp(module + rva, signature.data(), signature.size()) == 0;
    };
    const bool event_anim_ok = matches(IsEventAnimRva, IsEventAnimSig);
    const bool act8_ok = matches(Act8Rva, Act8Sig);
    const bool throw_env_ok = MatchMasked(module + ThrowEnvRva, ThrowEnvSig);
    const u32 throw_env_sites =
        (throw_env_ok ? 1 : 0) + (MatchMasked(module + ThrowEnv2Rva, ThrowEnvSig) ? 1 : 0);
    const bool in_throw_ok = matches(IsInThrowRva, IsInThrowSig);
    const bool set_event_ok = matches(SetEventAnimRva, SetEventAnimSig);
    const bool push_ok = matches(TimeActPushRva, TimeActPushSig);
    const bool act26_ok = matches(Act26Rva, Act26Sig);
    const bool env99_ok = MatchMasked(module + Env99Rva, LadderEnvSig) &&
                          std::memcmp(module + Env99Rva + LadderEnvSig.size(), Env99Tail.data(),
                                      Env99Tail.size()) == 0;
    const bool env100_ok = MatchMasked(module + Env100Rva, LadderEnvSig) &&
                           std::memcmp(module + Env100Rva + LadderEnvSig.size(), Env100Tail.data(),
                                       Env100Tail.size()) == 0;
    // The cancel windows (TAE JumpTable events 0 / 4 / 11 filling the action request module):
    // control back, for the early return.
    const bool cancel_ok = matches(JumpTableRva, JumpTableSig) &&
                           matches(JumpTable4Rva, JumpTable4Sig) &&
                           matches(JumpTable11Rva, JumpTable11Sig);
    const auto& settings = VR::GetSettings();
    const bool ladder_setting = settings.cinematic_ladder;
    const bool hook_ok = site_ok && main_ok;
    g_event = hook_ok && event_anim_ok && act8_ok;
    g_throw = hook_ok && throw_env_ok && in_throw_ok;
    g_request_ids = set_event_ok;
    g_ring_ids = push_ok;
    g_ladder = hook_ok && act26_ok && ladder_setting;
    g_ladder_predict = g_ladder && env99_ok;
    g_cancel = hook_ok && cancel_ok;
    g_item = hook_ok;
    if (const char* value = std::getenv("VR_CINE_LOG"); value != nullptr) {
        g_log = std::max(std::atoi(value), 1);
    }
    const auto verdict = [](bool ok) { return ok ? "ok" : "MISMATCH"; };
    const auto on = [](bool ok) { return ok ? "on" : "off"; };
    LOG_INFO(Core,
             "VR cine: siteA={} ismain={} isEventAnim@1A59210={} act8@1A11557={} throwEnv={}({}) "
             "isInThrow@1A61550={} setEventAnim@1A59190={} taPush@1A28173={} "
             "cancelWindows@1A2173A={} -> event={} throw={} ids={} release={}{} | exit {:.0f} ms, "
             "fade {:.0f} ms, return fade {:.0f} ms, min {:.0f} ms{}",
             verdict(site_ok), verdict(main_ok), verdict(event_anim_ok), verdict(act8_ok),
             verdict(throw_env_ok), throw_env_sites, verdict(in_throw_ok), verdict(set_event_ok),
             verdict(push_ok), verdict(cancel_ok), on(g_event), on(g_throw),
             on(g_request_ids || g_ring_ids), on(g_cancel && settings.cinematic_release),
             g_cancel && !settings.cinematic_release ? " (cinematic_release=0: timed only)" : "",
             settings.cinematic_exit_ms, settings.cinematic_fade_ms,
             settings.cinematic_return_fade_ms, settings.cinematic_min_ms,
             g_log != 0 ? fmt::format(" log={}", g_log) : std::string{});
    LOG_INFO(Core,
             "VR cine: ladder act26@1A11A67={} env99@1A14D9C={} env100@1A14DF1={} -> ladder={} "
             "predict={}{}",
             verdict(act26_ok), verdict(env99_ok), verdict(env100_ok), on(g_ladder),
             on(g_ladder_predict), ladder_setting ? "" : " (cinematic_ladder=0)");
    LOG_INFO(Core,
             "VR cine: canned item uses (the Shaman Bone Blade's stab from the arc): our Square's "
             "arm + the item-use bit {:#04x} -> item={}{}",
             static_cast<u32>(FlagItemUse), on(g_item),
             settings.cinematic_item ? "" : " (cinematic_item=0: shown in VR)");
    const auto dump = [module](const char* name, u64 rva, size_t length) {
        LOG_WARNING(Core, "VR cine: {} live bytes at {:#x}: {}", name, rva,
                    Hook::HexBytes(module + rva, length));
    };
    if (!event_anim_ok) {
        dump("IsEventAnim", IsEventAnimRva, IsEventAnimSig.size());
    }
    if (!act8_ok) {
        dump("act8", Act8Rva, Act8Sig.size());
    }
    if (!throw_env_ok) {
        dump("throwEnv", ThrowEnvRva, ThrowEnvSig.size());
    }
    if (!in_throw_ok) {
        dump("IsInThrow", IsInThrowRva, IsInThrowSig.size());
    }
    if (!set_event_ok) {
        dump("SetEventAnim", SetEventAnimRva, SetEventAnimSig.size());
    }
    if (!push_ok) {
        dump("TimeActPush", TimeActPushRva, TimeActPushSig.size());
    }
    if (!act26_ok) {
        dump("act26", Act26Rva, Act26Sig.size());
    }
    if (!env99_ok) {
        dump("env99", Env99Rva, LadderEnvSig.size() + Env99Tail.size());
    }
    if (!env100_ok) {
        dump("env100", Env100Rva, LadderEnvSig.size() + Env100Tail.size());
    }
    if (!cancel_ok) {
        dump("JumpTable", JumpTableRva, JumpTableSig.size());
        dump("JumpTable 4", JumpTable4Rva, JumpTable4Sig.size());
        dump("JumpTable 11", JumpTable11Rva, JumpTable11Sig.size());
    }
    if (!g_event && !g_throw && !g_ladder) {
        LogDisabled(hook_ok ? "no signal left" : "player update hook signature mismatch");
        return false;
    }
    if (!g_request_ids && !g_ring_ids) {
        LOG_WARNING(Core, "VR cine: no animation ids; every event animation counts as "
                          "cinematic_objects");
    }
    return true;
}

void CinematicUpdate(u8* player) {
    const Sample s = ReadSample(player);
    ++g_updates;
    // Layout checks on the first update with modules, then on every rising edge (a new
    // character object brings new modules).
    const bool event_bit = s.ready && (s.flags & FlagEventAnim) != 0;
    if (!g_checked) {
        if (s.modules != nullptr) {
            g_checked = true;
            LogFirst(player, s);
            CheckModules(player, s);
        }
    } else if ((event_bit && !g_last_event) || (s.ts != 0 && g_last_ts == 0)) {
        CheckModules(player, s);
    }
    // The ladder layout: the first update it is readable, then whenever the hunter gets on one
    // (also straight from a stale state: after a death on a ladder the next mount follows it
    // without a -1 in between).
    if (g_ladder && s.action_state != nullptr) {
        if (!g_ladder_checked) {
            g_ladder_checked = true;
            LogLadderFirst(player, s);
            CheckLadder(player, s);
        } else if ((s.ladder != -1 && g_last_ladder == -1) ||
                   (Cine::IsLadderMountState(s.ladder) &&
                    !Cine::IsLadderMountState(g_last_ladder))) {
            CheckLadder(player, s);
        }
    }

    // With every signal off the machine still runs (on empty signals), so a flat period always
    // ends.
    Cine::Signals signals{};
    signals.throw_valid = g_throw && s.node != nullptr;
    signals.throw_state = s.ts;
    signals.aligning = s.align != 0;
    signals.event_valid = g_event && s.action_flags != nullptr && s.ready;
    signals.event = (s.flags & FlagEventAnim) != 0;
    signals.ids_valid = g_request_ids || g_ring_ids;
    signals.event_id = Cine::PickEventId(s.pending.data(), g_ring_ids ? s.pending_count : 0,
                                         g_request_ids ? s.requested : -1);
    signals.hp_valid = s.hp_valid;
    signals.hp = s.hp;
    signals.hp_max = s.hp_max;
    signals.cancel_valid = g_cancel && s.cancel;
    signals.control = (s.allow & Cine::ControlAllowMask) != 0 || s.move;
    signals.new_player = g_last_player != nullptr && player != g_last_player;
    g_last_player = player;
    signals.ladder.valid = g_ladder && s.action_state != nullptr;
    signals.ladder.state = s.ladder;
    signals.ladder.object_valid = g_ladder_predict && s.ladder_object != nullptr;
    signals.ladder.object_state = s.object_state;
    signals.ladder.rung = s.rung;
    signals.ladder.top = s.top;
    signals.ladder.clip = s.ladder_clip;
    // cinematic_item: our Square for a canned item (the presenter's arm) and the game's item-use
    // state playing it (the action flags' bit; the item's clip among the pending entries).
    if (signals.new_player) {
        g_item_gate.Reset();
    }
    bool armed_now = false;
    if (VR::Quick::CannedUse canned{}; VR::Quick::LatestCannedUse(canned) && canned.serial != 0 &&
                                       canned.serial != g_canned.serial) {
        g_canned = canned;
        armed_now = true;
    }
    Cine::ItemSignals item{};
    item.valid = g_item && s.action_flags != nullptr;
    item.use = (s.flags & FlagItemUse) != 0;
    if (g_ring_ids && g_canned.clip >= 0) {
        for (u32 i = 0; i < s.pending_count; ++i) {
            item.clip = item.clip || s.pending[i] == g_canned.clip;
        }
    }
    item.arm_serial = g_canned.serial;
    item.arm_ns = g_canned.steady_ns;
    const Cine::ItemOut item_out = g_item_gate.Update(SteadyNs(), item);
    signals.item_valid = item.valid;
    signals.item = item_out.on;
    const Cine::Step step = VR::UpdateCinematic(signals);
    if (armed_now || item_out.event != Cine::ItemEvent::None) {
        // The canned use's own timeline (always: rare): armed, started (the game's item-use state
        // after our Square), ended (the bit off) or never started.
        const char* phase_now = Cine::PhaseName(step.changed ? step.to : g_last_phase);
        if (armed_now) {
            LOG_INFO(Core, "VR cine: ITEM armed: our Square for goods {} (clip {}), {} ms ago | {}",
                     g_canned.goods, g_canned.clip, (SteadyNs() - g_canned.steady_ns) / Cine::Millis,
                     Describe(s));
        }
        switch (item_out.event) {
        case Cine::ItemEvent::Start:
            LOG_INFO(Core,
                     "VR cine: ITEM start: goods {} use playing {} ms after our Square (by {}) "
                     "phase={}{} | {}",
                     g_canned.goods, item_out.arm_age_ns / Cine::Millis,
                     item_out.by_clip ? "its clip, the bit already on" : "the item-use bit",
                     phase_now,
                     VR::GetSettings().cinematic_item ? "" : " (cinematic_item=0: stays in VR)",
                     Describe(s));
            break;
        case Cine::ItemEvent::Rejoin:
            LOG_INFO(Core, "VR cine: ITEM rejoin: the item-use bit back after a gap ({} ms in) | {}",
                     item_out.use_age_ns / Cine::Millis, Describe(s));
            break;
        case Cine::ItemEvent::End:
        case Cine::ItemEvent::TimedOut:
            LOG_INFO(Core, "VR cine: ITEM {}: goods {} use played {} ms phase={} | {}",
                     Cine::ItemEventName(item_out.event), g_canned.goods,
                     item_out.use_age_ns / Cine::Millis, phase_now, Describe(s));
            break;
        case Cine::ItemEvent::Expired:
            LOG_INFO(Core,
                     "VR cine: ITEM expired: no item use {} ms after our Square for goods {} (the "
                     "game did not take it) | {}",
                     item_out.arm_age_ns / Cine::Millis, g_canned.goods, Describe(s));
            break;
        case Cine::ItemEvent::None:
            break;
        }
    }

    if (step.throw_stuck) {
        LOG_WARNING(Core,
                    "VR cine: throw state {} has lasted over {:.0f} s; ignored until it returns "
                    "to 0 | {}",
                    static_cast<u32>(s.ts), VR::GetSettings().cinematic_throw_timeout_s,
                    Describe(s));
    }
    if (step.changed) {
        // An aborted fade (FadeOut -> Returning) brightens again from the level it had reached.
        const bool aborted = step.from == Cine::Phase::FadeOut && step.to == Cine::Phase::Returning;
        LOG_INFO(Core, "VR cine: PHASE {}->{} after {:.2f} s ({}) cat={} id={}{}{} | {}",
                 Cine::PhaseName(step.from), Cine::PhaseName(step.to),
                 static_cast<double>(step.from_ns) * 1e-9, step.reason,
                 Cine::CategoryName(step.category), LoggedId(s, step, signals),
                 aborted ? fmt::format(" b0={:.2f}", step.b0) : std::string{},
                 step.resync ? " (heading re-synced to the head)" : "", Describe(s));
    }
    if (step.death_back) {
        LOG_INFO(Core,
                 "VR cine: death: HP 0 in the flat period, back to VR for the death screen "
                 "(cinematic_death_hold=0); the rest of this animation is ignored | {}",
                 Describe(s));
    }
    if (step.control_back) {
        LOG_INFO(Core,
                 "VR cine: control back: the game's cancel window stayed open {:.0f} ms (allow "
                 "{:#x} move {}); the rest of this animation is ignored | {}",
                 VR::GetSettings().cinematic_exit_ms, s.allow, s.move ? 1 : 0, Describe(s));
    }
    if (step.returned) {
        LogReturn(step.times);
    }
    if (step.latch_cleared) {
        LOG_INFO(Core,
                 "VR cine: the released animation's signals ended {} ms after control back (VR "
                 "came back that much sooner) | {}",
                 step.latch_age_ns / Cine::Millis, Describe(s));
    }
    const Cine::Phase phase = step.changed ? step.to : g_last_phase;
    if (g_ladder || step.ladder.session || step.ladder.event != Cine::LadderEvent::None) {
        LogLadder(s, step, phase);
    }
    if (g_log >= 1 &&
        (s.flags != g_last_flags || s.ts != g_last_ts || s.align != g_last_align ||
         step.raw != g_last_raw || phase != g_last_phase || signals.control != g_last_control)) {
        LOG_INFO(Core, "VR cine: EDGE {} cat={} raw={} phase={}", Describe(s),
                 Cine::CategoryName(step.category), step.raw ? 1 : 0, Cine::PhaseName(phase));
    }
    if (g_log >= 2) {
        const auto now = std::chrono::steady_clock::now();
        if (now - g_last_state >= std::chrono::seconds(1)) {
            const float seconds = g_last_state.time_since_epoch().count() == 0
                                      ? 1.0f
                                      : std::chrono::duration<float>(now - g_last_state).count();
            g_last_state = now;
            s64 age_ms{};
            const Cine::Phase published = VR::CinematicPhase(&age_ms);
            LOG_INFO(Core,
                     "VR cine: STATE phase={} ({} ms) raw={} cat={} event={} throw={} ids={} | "
                     "{} | upd/s={:.0f}",
                     Cine::PhaseName(published), age_ms, step.raw ? 1 : 0,
                     Cine::CategoryName(step.category), g_event ? "on" : "off",
                     g_throw ? "on" : "off", g_request_ids || g_ring_ids ? "on" : "off",
                     Describe(s), static_cast<float>(g_updates - g_logged_updates) / seconds);
            g_logged_updates = g_updates;
        }
    }
    g_last_flags = s.flags;
    g_last_ts = s.ts;
    g_last_align = s.align;
    g_last_event = event_bit;
    g_last_raw = step.raw;
    g_last_phase = phase;
    g_last_control = signals.control;
}

} // namespace VR::Bloodborne
