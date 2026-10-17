// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "common/logging/log.h"
#include "core/vr/bloodborne_assets.h"
#include "core/vr/bloodborne_body.h"
#include "core/vr/bloodborne_gun.h"
#include "core/vr/bloodborne_heal.h"
#include "core/vr/bloodborne_held.h"
#include "core/vr/body_channel.h"
#include "core/vr/held_item.h"
#include "core/vr/hook_util.h"
#include "core/vr/inframe.h"
#include "core/vr/interaction_native_vial_visual.h"
#include "core/vr/lever_channel.h"
#include "core/vr/music_box.h"
#include "core/vr/quick_channel.h"
#include "core/vr/quick_items.h"
#include "core/vr/quick_settings.h"
#include "core/vr/torch_lift.h"
#include "core/vr/vial_drain.h"
#include "core/vr/vr_session.h"

namespace VR::Bloodborne {

namespace {

namespace H = VR::Held;
using Hook::Plausible;
using Hook::Read;

VR::Interactions::Module* g_held_visual_module{};
bool g_held_visual_fault{};
VR::Interactions::NativeVialVisualRuntime g_held_visual;
void InitHeldVisualDll() {
    if (!VR::Interactions::DllRequested() || g_held_visual_module)
        return;
    // Pose updates are serialized by the body's guard; site A is its startup alternative.
    g_held_visual_module = new VR::Interactions::Module();
    g_held_visual_module->EnableFaultBroadcast();
    g_held_visual.Bind(*g_held_visual_module, g_held_visual_fault);
    if (!g_held_visual_module->LoadBesideExecutable()) {
        g_held_visual_fault = true;
        g_held_visual_module->ReportFault();
        LOG_ERROR(Core, "VR held visual: interaction DLL unavailable: {}",
                  g_held_visual_module->Diagnostic());
    }
}

// Module-relative addresses in the 1.09 eboot (patch file address - 0x400000).
// SfxSlotRequest, `void(SfxCtl* ctl, s32 slot, s32 ffx, const s32* dummy, u8 repeat)`: TAE event
// 96's call. With slot >= 0 it looks for the node with that slot ([ctl+8] list, next at +0x10,
// [n+8] == slot, (u16 [n+0xC] & 0x7FFF) == 0x4002) and renews its request (ffx +0x24, dummy
// +0x2C, repeat bit); without one it allocates a node (constructor 0x18AE690) and pushes it.
// The prefix holds the whole lookup (unique in the eboot).
constexpr u64 SfxSlotRequestRva = 0x1896DB0;
constexpr std::array<u8, 106> SfxSlotRequestSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50, 0x41, 0x89,
    0xF5, 0x49, 0x89, 0xFC, 0x45, 0x85, 0xED, 0x78, 0x43, 0x49, 0x8B, 0x5C, 0x24, 0x08, 0xEB, 0x04,
    0x48, 0x8B, 0x5B, 0x10, 0x48, 0x85, 0xDB, 0x74, 0x33, 0x44, 0x39, 0x6B, 0x08, 0x75, 0xF1, 0x0F,
    0xB7, 0x43, 0x0C, 0x89, 0xC6, 0x81, 0xE6, 0xFF, 0x7F, 0x00, 0x00, 0x66, 0x81, 0xFE, 0x02, 0x40,
    0x75, 0xDE, 0x48, 0x85, 0xDB, 0x74, 0x15, 0x48, 0x89, 0x4D, 0xD0, 0x45, 0x89, 0xC7, 0x41, 0x89,
    0xD6, 0x0D, 0x00, 0x80, 0x00, 0x00, 0x66, 0x89, 0x43, 0x0C, 0xEB, 0x5C, 0x48, 0x89, 0x4D, 0xD0,
    0x45, 0x89, 0xC7, 0x41, 0x89, 0xD6, 0x48, 0x8D, 0x05, 0x03,
};
// TAE event 96's call (0x1A21887): `add rdi, 0x2A0` (the character's effect controller), the dummy
// on the stack, rcx = &dummy, r8d = 0 (no repeat), `call 0x1896DB0`: the target and the argument
// order.
constexpr u64 E96CallRva = 0x1A21887;
constexpr std::array<u8, 28> E96CallSig = {
    0x48, 0x81, 0xC7, 0xA0, 0x02, 0x00, 0x00, 0x89, 0x85, 0x68, 0xFE, 0xFF, 0xFF, 0x48,
    0x8D, 0x8D, 0x68, 0xFE, 0xFF, 0xFF, 0x45, 0x31, 0xC0, 0xE8, 0x0D, 0x55, 0xE7, 0xFF,
};
// ChrIns constructor (0x18B9F05): `lea rax, [rip+...]` = the effect controller's vtable
// (0x532EF90), stored at chr+0x2A0. Checked on the live character before every call.
constexpr u64 CtlCtorRva = 0x18B9F05;
constexpr std::array<u8, 14> CtlCtorSig = {
    0x48, 0x8D, 0x05, 0x84, 0x50, 0xA7, 0x03, 0x49, 0x89, 0x85, 0xA0, 0x02, 0x00, 0x00,
};
constexpr u64 SfxCtlVtableRva = 0x532EF90;
// The node constructor (0x18AE69D): slot 0xBAD, flags 2, the node vtable 0x532F660.
constexpr u64 NodeCtorRva = 0x18AE69D;
constexpr std::array<u8, 41> NodeCtorSig = {
    0x41, 0xC7, 0x46, 0x08, 0xAD, 0x0B, 0x00, 0x00, 0x66, 0x41, 0xC7, 0x46, 0x0C, 0x02,
    0x00, 0x49, 0xC7, 0x46, 0x18, 0x00, 0x00, 0x00, 0x00, 0x49, 0xC7, 0x46, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x48, 0x8D, 0x05, 0x9D, 0x0F, 0xA8, 0x03, 0x49, 0x89, 0x06,
};
constexpr u64 NodeVtableRva = 0x532F660;
// The node update (node vt+0x20, from PlayerIns vt+0xF8): a request (+0x24) persists only with
// the repeat (0x10) or ending (0x40) bit (+0x30); so a slot effect without repeat lives as long as
// it is asked for every update. Its spawned effect is at +0x40.
constexpr u64 NodeUpdateRva = 0x18AEBA0;
constexpr std::array<u8, 80> NodeUpdateSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC,
    0xA8, 0x00, 0x00, 0x00, 0x49, 0x89, 0xF0, 0x49, 0x89, 0xFE, 0x4C, 0x8B, 0x2D, 0x1F, 0x50, 0xB3,
    0x03, 0x49, 0x8B, 0x45, 0x00, 0x48, 0x89, 0x45, 0xD0, 0x4D, 0x8B, 0x08, 0x4D, 0x8B, 0x60, 0x18,
    0x41, 0x8A, 0x4E, 0x30, 0xF6, 0xC1, 0x50, 0x74, 0x17, 0x41, 0x83, 0x7E, 0x24, 0x00, 0x79, 0x10,
    0x41, 0x8B, 0x46, 0x20, 0x41, 0x89, 0x46, 0x24, 0x41, 0x8B, 0x46, 0x28, 0x41, 0x89, 0x46, 0x2C,
};
// ... and its ending check (0x18AF22A): an ending node is freed once its effect's state (+0xC) is
// 3 (finished).
constexpr u64 NodeEndingRva = 0x18AF22A;
constexpr std::array<u8, 38> NodeEndingSig = {
    0xF6, 0xC1, 0x40, 0x74, 0x21, 0x49, 0x8B, 0x56, 0x40, 0x48, 0x85, 0xD2, 0x74,
    0x0C, 0x31, 0xC0, 0x83, 0x7A, 0x0C, 0x03, 0x0F, 0x85, 0x80, 0x02, 0x00, 0x00,
    0x41, 0x80, 0x66, 0x0D, 0xBF, 0x31, 0xC0, 0xE9, 0x74, 0x02, 0x00, 0x00,
};
// ChrAsm update (0x1C9D053): after the weapon slots read their form override bytes (asm+0x268:
// left parts 0-3, right parts 0-3; -1 destroys the part), all eight are reset to 0xFE.
constexpr u64 AsmResetRva = 0x1C9D053;
constexpr std::array<u8, 17> AsmResetSig = {
    0x48, 0xB8, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE,
    0xFE, 0x49, 0x89, 0x86, 0x68, 0x02, 0x00, 0x00,
};
// TAE event 322's store (0x1A22172): asm = [chr+0x4D0], asm[0x268 + b0 * 4 + b1] = b2 (vanilla
// clip 100141: 00 00 FF, the left weapon's part 0 hidden).
constexpr u64 E322StoreRva = 0x1A22172;
constexpr std::array<u8, 52> E322StoreSig = {
    0x49, 0x8B, 0x86, 0xD0, 0x04, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0x47,
    0x24, 0x00, 0x00, 0x48, 0x8B, 0x4B, 0x08, 0x48, 0x85, 0xC9, 0x0F, 0x84, 0x3A,
    0x24, 0x00, 0x00, 0x0F, 0xB6, 0x11, 0x0F, 0xB6, 0x71, 0x01, 0x8A, 0x49, 0x02,
    0x8D, 0x14, 0x96, 0x48, 0x63, 0xD2, 0x88, 0x8C, 0x10, 0x68, 0x02, 0x00, 0x00,
};

// ChrIns::GetDmyPolyMtx, `s32 (ChrIns*, Mtx34* out, const s32* id, u32 max)`: world 3x4 rows of
// the dummy polys with that id (their columns are the dummy's axes). The DIBR depth proxy only.
constexpr u64 DmyPolyRva = 0x18C3BF0;
constexpr std::array<u8, 32> DmyPolySig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC,
    0x28, 0x41, 0x89, 0xCF, 0x48, 0x89, 0xD3, 0x48, 0x89, 0x75, 0xB0, 0x4C, 0x8B, 0x35, 0xCE, 0xFF,
};
constexpr u64 WorldChrManRva = 0x553E878;
constexpr u32 OffsetMainPlayer = 0x60;

constexpr u32 OffsetSfxCtl = 0x2A0;
constexpr u32 OffsetCtlHead = 0x08;
constexpr u32 OffsetNodeSlot = 0x08;
constexpr u32 OffsetNodeFlags = 0x0C;
constexpr u32 OffsetNodeNext = 0x10;
constexpr u32 OffsetNodeCur = 0x20;  // the effect id spawned (-1 none)
constexpr u32 OffsetNodeReq = 0x24;  // the effect id asked for this update (-1 none)
constexpr u32 OffsetNodeBits = 0x30; // 0x20 spawned, 0x40 ending, 0x10 repeat
constexpr u32 OffsetNodeSfx = 0x40;
constexpr u16 NodeSlotFlags = 0x4002;
constexpr u8 NodeSpawned = 0x20;
constexpr u32 OffsetSfxState = 0x0C; // 3 = finished
constexpr u32 OffsetSfxStop = 0x10;  // bit 0: stop requested
constexpr s32 SfxFinished = 3;
constexpr u32 OffsetChrAsm = 0x4D0;
constexpr u32 OffsetAsmOverride = 0x268;
constexpr u32 OffsetAsmParts = 0x28; // model slot objects, 8 bytes each
constexpr s32 LeftPartSlot = 7;      // model slots 7-10: the left weapon's parts 0-3
constexpr u32 OffsetChrModules = 0x3B0;
constexpr u32 OffsetDataModule = 0x20;
constexpr u32 OffsetHp = 0xF8;
constexpr u32 OffsetItemUseState = 0x490; // PlayerIns: a quick item is being used (0 = idle)

constexpr s32 VialFfx = 173;     // the Blood Bullet clip's vial (model s10002, infinite)
constexpr s32 VialModel = 10002; // FFX 173's model: our drained vial draws it (vial_inject_drain)
constexpr s64 OursFreshNs = 150 * 1'000'000; // the renderer trusts an "ours" this recent
constexpr s32 VialDummy = 3;     // L_Hand, the palm
constexpr u32 MaxHops = 256;     // effect list walk
constexpr u32 PendingLimit = 10; // updates without a live effect: this arm falls back
constexpr u32 ArmFailLimit = 3;  // arms in a row that fell back: the session does
constexpr s64 Ms = 1'000'000;
// The vial's glass in dummy 3's frame as a capsule for the DIBR depth proxy, measured (mock, AER
// stereo triangulation of the rendered vial): about 4 cm across along +Y, from y -0.04 (the
// bottom) to +0.095 (the cap), centred at z +0.02 .. +0.04. The capsule adds about 6 mm all
// round for the animation's slack (a vial pixel outside it tears away at the background's depth).
constexpr std::array<float, 3> ProxyA = {0.0f, -0.028f, 0.030f};
constexpr std::array<float, 3> ProxyB = {0.0f, 0.072f, 0.030f};
constexpr float ProxyRadius = 0.026f;
constexpr u32 GunCheckUpdates = 3; // VR_HELD_LOG: the left part is logged this long after a hide

using DmyPolyFn = s32(PS4_SYSV_ABI*)(void* chr, void* out, const s32* ids, u32 max);
using SfxSlotRequestFn = void(PS4_SYSV_ABI*)(void* ctl, s32 slot, s32 ffx, const s32* dummy,
                                             u8 repeat);

u8* g_base = nullptr;
bool g_ready = false; // installed: the sites matched and vial_in_hand=game
SfxSlotRequestFn g_sfx_slot = nullptr;
DmyPolyFn g_dmy_poly = nullptr; // the depth proxy (nullptr: GetDmyPolyMtx did not match)
// The character whose vial the render-camera hook places the depth proxy for (nullptr: none this
// update). Written by the tick (the loop thread, or site A's job thread), read by the hook.
std::atomic<u8*> g_proxy_chr{nullptr};
int g_log = 0;          // VR_HELD_LOG: 1 = a state line every second, 2 = every update
bool g_dry_run = false; // VR_HELD_DRYRUN: everything but the call and the hide byte
std::atomic_bool g_left_hidden{false};
// The grip the hunter's left fingers close into (VR::Body::Grip::Kind, 0 = none): the held item's
// while it is shown (core/vr/quick_items.h HeldLookOf; the vial's own), read by the body's next
// pose copy.
std::atomic<u64> g_grip{0}; // kind and goods published together
// vial_inject_drain: the last update that made the vial in the hand ours (steady ns; 0 = the
// game's or none) and the drain word it saw, read by the render-camera hook of the same frame
// (HeldVialOurs): the renderer's level is the update's, so a drain the presenter ends between the
// update and the render never shows a full vial for a frame.
std::atomic<s64> g_ours_ns{0};
std::atomic<u64> g_ours_word{0};

// One tick at a time: the pose wrapper (loop thread) or site A (the player's update), never both
// (HeldSiteATick only runs without the pose hook).
struct Side {
    const u8* chr{nullptr}; // the character the slot nodes are on
    s32 slot{H::FirstVialSlot};
    bool showing{false};      // asked for last update
    bool shown_logged{false}; // this arm's "shown" line
    u32 pending{0};           // updates of this arm without a live effect
    u32 updates{0};           // of this arm
    u32 arm_fails{0};         // arms that fell back in a row (a shown vial resets it)
    bool arm_failed{false};   // this arm fell back (until the request ends)
    bool failed{false};       // the session fell back
    const char* fail_why{""};
    H::State state{H::State::Off};
    const char* reason{"off"};
    u64 nodes_created{0};
    u64 rotations{0};
    u64 arms{0};
    // VR_HELD_LOG.
    s64 last_log_ns{0};
    s64 tick_ns_max{0};
    u32 consumed{0}; // ticks that found the override bytes reset by the game (0xFE)
    u32 hides{0};
    u32 gun_check{0}; // updates left to log the left part after a hide
    s32 grip_goods{-1};
    u8 grip{0};       // the grip of what this update shows (when it is shown)
    s32 item_logged{-2}; // the quick item of the last "in the hand" line (-1: the open arc)
    // vial_inject_drain: updates of this drain with our vial (0: not ours), and whether the
    // game's effect was still alive at the last one (the hand-over's log).
    u32 ours_updates{0};
    bool ours_game_alive{false};
    bool ours_unready_logged{false};
};
Side g_side{};

// Quick items (Kind::Item): the held effects that never came alive (not resident, or no model):
// our in-frame mesh shows those instead, for the session.
std::array<s32, 16> g_item_ffx_failed{};
u32 g_item_ffx_failed_count = 0;

bool ItemFfxFailed(s32 ffx) {
    for (u32 i = 0; i < std::min<u32>(g_item_ffx_failed_count, 16); ++i) {
        if (g_item_ffx_failed[i] == ffx) {
            return true;
        }
    }
    return false;
}

s64 SteadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

const u8* DataModule(const u8* chr) {
    const u8* const modules = Read<const u8*>(chr + OffsetChrModules);
    if (!Plausible(modules)) {
        return nullptr;
    }
    const u8* const module = Read<const u8*>(modules + OffsetDataModule);
    return Plausible(module) ? module : nullptr;
}

// The node of `slot`, as SfxSlotRequest matches it. False when the list looks broken (an
// implausible pointer, a foreign vtable on our node, or longer than MaxHops).
bool FindNode(const u8* ctl, s32 slot, u8*& node, u32& length) {
    node = nullptr;
    length = 0;
    u8* n = Read<u8*>(ctl + OffsetCtlHead);
    while (n != nullptr) {
        if (length >= MaxHops || !Plausible(n)) {
            return false;
        }
        ++length;
        if (node == nullptr && Read<s32>(n + OffsetNodeSlot) == slot &&
            (Read<u16>(n + OffsetNodeFlags) & 0x7FFF) == NodeSlotFlags) {
            if (Read<u8*>(n) != g_base + NodeVtableRva) {
                return false;
            }
            node = n;
            if (g_log == 0) {
                return true; // the list length is only logged
            }
        }
        n = Read<u8*>(n + OffsetNodeNext);
    }
    return true;
}

u8* NodeSfx(const u8* node) {
    u8* const sfx = Read<u8*>(node + OffsetNodeSfx);
    return Plausible(sfx) ? sfx : nullptr;
}

// The node's effect is alive: spawned, not finished, no stop requested.
bool Alive(const u8* node) {
    const u8* const sfx = NodeSfx(node);
    return sfx != nullptr && Read<s32>(sfx + OffsetSfxState) != SfxFinished &&
           (Read<u8>(sfx + OffsetSfxStop) & 1) == 0;
}

// The game stopped the effect (a stop-all) but kept the node: asking again would never respawn
// it.
bool Stuck(const u8* node) {
    return (Read<u8>(node + OffsetNodeBits) & NodeSpawned) != 0 && !Alive(node);
}

void Publish(const Side& s, bool left_hidden, s64 now) {
    VR::HeldStatus status{};
    status.steady_ns = now;
    status.state = static_cast<u8>(s.state);
    status.left_hidden = left_hidden;
    status.slot = s.slot;
    status.reason = s.reason;
    VR::PublishHeldStatus(status);
    g_left_hidden.store(left_hidden, std::memory_order_relaxed);
    const u64 grip = s.state == H::State::Shown && !g_dry_run
                         ? (static_cast<u64>(static_cast<u32>(s.grip_goods)) << 8) | s.grip
                         : 0;
    g_grip.store(grip, std::memory_order_relaxed);
}

std::string OverrideText(const u8* chr) {
    const u8* const chr_asm = Read<const u8*>(chr + OffsetChrAsm);
    if (!Plausible(chr_asm)) {
        return "asm ?";
    }
    return fmt::format("asm {} left part0 {}", Hook::HexBytes(chr_asm + OffsetAsmOverride, 8),
                       fmt::ptr(Read<const u8*>(chr_asm + OffsetAsmParts + 8 * LeftPartSlot)));
}

// Ends this arm on the overlay vial; the third such arm ends the session's game vial.
void FailArm(Side& s, const char* why) {
    s.arm_failed = true;
    s.state = H::State::Failed;
    s.reason = why;
    s.showing = false;
    ++s.arm_fails;
    if (s.arm_fails >= ArmFailLimit) {
        s.failed = true;
        s.fail_why = why;
        LOG_WARNING(Core,
                    "VR held: FAILED ({}, {} arms in a row) -> the overlay vial for this session",
                    why, s.arm_fails);
    } else {
        LOG_WARNING(Core, "VR held: FAILED ({}) -> the overlay vial until the vial is put away",
                    why);
    }
}

// vial_inject_drain: the vial in the hand is not ours from this update on (put away, suppressed,
// the game's own again, or ours not ready): the renderer stops drawing it with this very update.
void EndOurs(Side& s, const char* why) {
    g_ours_ns.store(0, std::memory_order_release);
    if (s.ours_updates != 0) {
        LOG_INFO(Core, "VR held: drain: our vial gone after {} updates ({})", s.ours_updates, why);
        s.ours_updates = 0;
        s.ours_game_alive = false;
    }
}

void Tick(u8* chr, const HeldGate& gate, const char* site) {
    auto& s = g_side;
    const s64 now = SteadyNs();
    // vial_inject_drain: the drain word before the request. The presenter publishes the request
    // that puts the vial away before it clears the word, so a cleared word read here comes with
    // that request: never a full vial for an update at the end of a drain.
    const u64 drain_word = VR::LatestVialDrain();
    const u64 request_word = VR::LatestHeldRequest();
    const H::Request request = H::UnpackRequest(request_word);
    // A quick item from the arc: the game's held effect of that item (none: only the gun is
    // hidden and the in-frame renderer draws our mesh in the palm).
    const bool item = request.kind == H::Kind::Item;
    s32 item_goods = -1;
    s32 ffx = VialFfx;
    if (item) {
        VR::Quick::LatestHeldItem(item_goods, ffx);
        if (ffx >= 0 && ItemFfxFailed(ffx)) {
            ffx = -1;
        }
    }
    // An item request with no item: the quick-item arc is open (quick_arc_hide_gun_open): the
    // gun is hidden, the fingers stay open (they reach for a card).
    PiNativeVialVisualInput input{};
    input.operation = PI_VV_HELD;
    input.now_ns = now;
    auto& facts = input.held;
    facts.request_word = request_word;
    facts.drain_word = drain_word;
    facts.cinematic = VR::CinematicActive();
    facts.menu = VR::IsGameMenuOpen();
    facts.gate_valid = gate.valid;
    facts.gate_reason = static_cast<uint32_t>(gate.reason);
    facts.owner = static_cast<uint32_t>(gate.owner);
    facts.held = gate.held;
    const u8* const data = DataModule(chr);
    facts.hp = data != nullptr ? Read<s32>(data + OffsetHp) : 1;
    facts.item_use_state = Read<s32>(chr + OffsetItemUseState);
    facts.item_goods = item_goods;
    const auto& settings = VR::GetSettings();
    facts.hide_left = settings.vial_hide_left;
    facts.hide_two_handed = settings.vial_hide_two_handed;
    facts.hide_gun = VR::Quick::GetQuickSettings().hide_gun;
    facts.dry_run = g_dry_run;
    facts.failed = s.failed;
    facts.arm_failed = s.arm_failed;
    facts.owner_changed = chr != s.chr;
    // Coarse read optimization only: eligibility and render hand-over remain DLL-owned.
    if (request.kind == H::Kind::Vial && (request.flags & H::FlagDrain)) {
        facts.model_available = VR::Assets::GetModel(VialModel) != nullptr;
        facts.renderer_available = VR::InFrame::Available();
        facts.inframe_enabled = VR::Quick::GetQuickSettings().inframe;
        facts.carried = CarriedBloodVials();
    }
    const auto reply = g_held_visual.Call(input);
    PiHeldVisibilityOutput decision{};
    decision.reason = PI_HR_FAULT;
    decision.grip_goods = -1;
    if (reply.applied)
        decision = reply.held;
    s.grip_goods = decision.grip_goods;
    s.grip = static_cast<u8>(decision.grip);
    const H::Decision d{decision.requested != 0, decision.show != 0,
                        static_cast<u8>(decision.hide_mask), decision.ours != 0,
                        PhysicalInteractions::HeldVisibility::Reason(decision.reason)};

    if (chr != s.chr) {
        s.chr = chr; // a new character (loading): the old slot nodes went with the old one
        s.slot = H::FirstVialSlot;
        s.showing = false;
    }
    if (!d.requested) {
        s.arm_failed = false; // the next arm tries again
    }
    if (!d.ours || item || g_dry_run || s.failed || s.arm_failed) {
        EndOurs(s, d.show ? "the game's vial" : d.reason);
    }
    g_proxy_chr.store(d.show && !g_dry_run && !s.failed && !s.arm_failed ? chr : nullptr,
                      std::memory_order_relaxed);
    if (s.gun_check != 0 && !d.show && --s.gun_check % GunCheckUpdates == 0) {
        LOG_INFO(Core, "VR held: {} updates after the hide | {}", GunCheckUpdates,
                 OverrideText(chr)); // the left part is back
    }
    if (item) {
        VR::Quick::SetHeldItemNative(false); // set again below once the effect is alive
    }
    if (!item && (s.failed || s.arm_failed)) {
        s.state = H::State::Failed;
        s.reason = s.failed ? s.fail_why : s.reason;
        Publish(s, false, now);
        return;
    }
    if (!d.show) {
        if (s.showing) {
            LOG_INFO(Core, "VR held: hide ({}) after {} updates{}", d.reason, s.updates,
                     g_log > 0 ? " | " + OverrideText(chr) : std::string{});
            s.gun_check = g_log > 0 ? GunCheckUpdates : 0;
        }
        const H::State next = d.requested ? H::State::Suppressed : H::State::Off;
        if (next == H::State::Suppressed && s.state != H::State::Suppressed) {
            LOG_INFO(Core, "VR held: suppressed ({}{}{})", d.reason,
                     gate.valid && gate.held ? ", " : "",
                     gate.valid && gate.held ? VR::Body::AnimClassName(gate.owner) : "");
        }
        s.showing = false;
        s.state = next;
        s.reason = d.reason;
        Publish(s, false, now);
        return;
    }
    if (g_dry_run) {
        s.state = H::State::Failed; // nothing in the hand: the overlay vial shows
        s.reason = "dry run";
    }
    // vial_inject_drain (core/vr/vial_drain.h): the vial is ours while it is armed and through
    // the injection's drain (the in-frame renderer's model s10002 with its level) at the game's
    // point of the palm: the game's effect is not refreshed (an older one ends in this update's
    // effect update), the left weapon stays hidden and the vial grip closed. Only once our model
    // is loaded and the in-frame renderer draws (else the game's vial, full to the end).
    const bool ours = decision.render_ours;
    if (d.ours && !g_dry_run && !ours) {
        EndOurs(s, "not ready");
        if (!s.ours_unready_logged) {
            s.ours_unready_logged = true;
            LOG_INFO(Core, "VR held: drain: our vial is not ready ({}): the game's vial stays full",
                     !facts.model_available ? "model s10002 not loaded"
                                            : "the in-frame renderer is off");
        }
    }
    if (ours) {
        u8* const ctl = chr + OffsetSfxCtl;
        u8* node = nullptr;
        u32 length = 0;
        const bool list_ok = Read<u8*>(ctl) == g_base + SfxCtlVtableRva &&
                             FindNode(ctl, s.slot, node, length);
        const bool game_alive = list_ok && node != nullptr && Alive(node);
        bool hidden = false;
        if (d.hide_left_mask != 0) {
            u8* const chr_asm = Read<u8*>(chr + OffsetChrAsm);
            if (Plausible(chr_asm)) {
                for (u32 i = 0; i < 4; ++i) {
                    if ((d.hide_left_mask & (1u << i)) != 0) {
                        chr_asm[OffsetAsmOverride + i] = 0xFF;
                    }
                }
                hidden = true;
            }
        }
        if (s.ours_updates == 0) {
            LOG_INFO(Core,
                     "VR held: drain: our vial (s{:05}, in-frame) from this update; the game's FFX "
                     "{} (slot {}) is no longer refreshed (alive now: {}){}",
                     VialModel, VialFfx, s.slot, game_alive ? "yes" : "no",
                     hidden ? ", the left weapon stays hidden" : "");
        } else if (s.ours_game_alive && !game_alive) {
            LOG_INFO(Core, "VR held: drain: the game's vial ended after {} update{} of ours",
                     s.ours_updates, s.ours_updates == 1 ? "" : "s");
        }
        s.ours_game_alive = game_alive;
        ++s.ours_updates;
        s.showing = true;
        s.shown_logged = true;
        ++s.updates;
        s.state = H::State::Shown;
        s.reason = "";
        // Armed with no Blood Vials left: an empty vial (a stab is refused, nothing drains).
        const u64 word = decision.look_word;
        if (decision.look_fresh && decision.empty) {
            LOG_INFO(Core, "VR held: drain: no Blood Vials left: the vial in the hand is empty");
        }
        g_ours_word.store(word, std::memory_order_relaxed);
        g_ours_ns.store(now, std::memory_order_release);
        Publish(s, hidden, now);
        return;
    }
    if (item && ffx < 0) {
        // No held effect for this item: hide the gun only (our mesh is drawn in the palm).
        bool hidden = false;
        if (d.hide_left_mask != 0 && !g_dry_run) {
            u8* const chr_asm = Read<u8*>(chr + OffsetChrAsm);
            if (Plausible(chr_asm)) {
                for (u32 i = 0; i < 4; ++i) {
                    if ((d.hide_left_mask & (1u << i)) != 0) {
                        chr_asm[OffsetAsmOverride + i] = 0xFF;
                    }
                }
                hidden = true;
            }
        }
        if (!s.showing || item_goods != s.item_logged) {
            if (!s.showing) {
                s.updates = 0;
            }
            s.showing = true;
            s.item_logged = item_goods;
            if (item_goods < 0) {
                LOG_INFO(Core, "VR held: the quick-item arc is open{}",
                         hidden ? ": the gun hidden" : "");
            } else {
                LOG_INFO(Core, "VR held: quick item {} in the hand (no held effect: our mesh){}",
                         item_goods, hidden ? ", the gun hidden" : "");
            }
        }
        ++s.updates;
        s.state = H::State::Shown;
        s.reason = "";
        g_proxy_chr.store(nullptr, std::memory_order_relaxed);
        Publish(s, hidden, now);
        return;
    }

    u8* const ctl = chr + OffsetSfxCtl;
    if (Read<u8*>(ctl) != g_base + SfxCtlVtableRva) {
        FailArm(s, "the effect controller's vtable is not the ChrIns one");
        Publish(s, false, now);
        return;
    }
    u8* node = nullptr;
    u32 length = 0;
    if (!FindNode(ctl, s.slot, node, length)) {
        FailArm(s, "the effect list looks broken");
        Publish(s, false, now);
        return;
    }
    if (!s.showing) {
        ++s.arms;
        s.showing = true;
        s.shown_logged = false;
        s.pending = 0;
        s.updates = 0;
        LOG_INFO(Core, "VR held: show slot {} (FFX {} on dummy {}, {}{}){}", s.slot, ffx,
                 VialDummy, site, item ? fmt::format(", quick item {}", item_goods) : "",
                 g_dry_run ? " DRY RUN" : "");
    }
    ++s.updates;
    if (node != nullptr && Stuck(node)) {
        const s32 next = H::NextVialSlot(s.slot);
        LOG_INFO(Core,
                 "VR held: the game stopped the effect of slot {} (node {} cur {} sfx {}) -> "
                 "slot {}",
                 s.slot, fmt::ptr(node), Read<s32>(node + OffsetNodeCur), fmt::ptr(NodeSfx(node)),
                 next);
        s.slot = next;
        ++s.rotations;
        // The stuck node is no longer asked for: it ends (slot -1) and is freed. An ended node
        // never matches, so the new slot normally has none and the call below creates it.
        if (!FindNode(ctl, s.slot, node, length)) {
            FailArm(s, "the effect list looks broken");
            Publish(s, false, now);
            return;
        }
    }
    const bool alive = node != nullptr && Alive(node);
    if (!g_dry_run) {
        const s32 dummy = VialDummy;
        // Repeat 0, deliberately: if we stop calling (disarm, crash, disable) the effect ends.
        g_sfx_slot(ctl, s.slot, ffx, &dummy, 0);
        if (node == nullptr) {
            ++s.nodes_created;
        }
    }
    bool hidden = false;
    if (d.hide_left_mask != 0 && !g_dry_run) {
        u8* const chr_asm = Read<u8*>(chr + OffsetChrAsm);
        if (Plausible(chr_asm)) {
            u8* const bytes = chr_asm + OffsetAsmOverride;
            s.consumed += Read<u8>(bytes) == 0xFE ? 1 : 0;
            for (u32 i = 0; i < 4; ++i) {
                if ((d.hide_left_mask & (1u << i)) != 0) {
                    bytes[i] = 0xFF; // as TAE event 322 (00 00 FF): the part is destroyed
                }
            }
            hidden = true;
            ++s.hides;
        }
    }
    if (!g_dry_run) {
        if (item) {
            VR::Quick::SetHeldItemNative(alive);
        }
        if (alive) {
            s.state = H::State::Shown;
            s.reason = "";
            s.pending = 0;
            s.arm_fails = item ? s.arm_fails : 0; // ArmFailLimit counts the vial's failed arms
            if (!s.shown_logged) {
                s.shown_logged = true;
                LOG_INFO(Core, "VR held: shown (node {} sfx {}) after {} updates{}", fmt::ptr(node),
                         fmt::ptr(NodeSfx(node)), s.updates,
                         g_log > 0 ? " | " + OverrideText(chr) : std::string{});
            }
        } else {
            s.state = H::State::Pending;
            s.reason = "starting";
            if (++s.pending > PendingLimit) {
                if (item) {
                    // This item's effect never came alive: our mesh from now on (session).
                    g_item_ffx_failed[g_item_ffx_failed_count++ % g_item_ffx_failed.size()] = ffx;
                    LOG_WARNING(Core,
                                "VR held: quick item {}: FFX {} not alive after {} updates -> "
                                "our in-frame mesh for it",
                                item_goods, ffx, PendingLimit);
                    s.pending = 0;
                } else {
                    FailArm(s, "no live effect after 10 updates");
                }
            }
        }
    }
    Publish(s, hidden, now);

    const s64 work = SteadyNs() - now;
    s.tick_ns_max = std::max(s.tick_ns_max, work);
    if (g_log >= 2 || (g_log == 1 && now - s.last_log_ns >= 1000 * Ms)) {
        s.last_log_ns = now;
        LOG_INFO(
            Core,
            "VR held: state {} slot {} node {} cur/req {}/{} bits {:#x} sfx {} state {} | "
            "list {} nodes created {} rotations {} arms {} | {} | consumed {}/{} | tick max "
            "{:.1f} us | gate {} {} held {} | hp {} item use {}",
            H::StateName(s.state), s.slot, fmt::ptr(node),
            node != nullptr ? Read<s32>(node + OffsetNodeCur) : -2,
            node != nullptr ? Read<s32>(node + OffsetNodeReq) : -2,
            node != nullptr ? Read<u8>(node + OffsetNodeBits) : 0,
            fmt::ptr(node != nullptr ? NodeSfx(node) : nullptr),
            node != nullptr && NodeSfx(node) != nullptr ? Read<s32>(NodeSfx(node) + OffsetSfxState)
                                                        : -1,
            length, s.nodes_created, s.rotations, s.arms, OverrideText(chr), s.consumed, s.hides,
            static_cast<double>(s.tick_ns_max) * 1e-3, VR::Body::GateReasonName(gate.reason),
            VR::Body::AnimClassName(gate.owner), gate.held ? 1 : 0, facts.hp, facts.item_use_state);
        s.tick_ns_max = 0;
    }
}

// quick_music_box_crank (core/vr/music_box.h): while the Tiny Music Box is in the left hand to be
// cranked, the right weapon is put away: its four part bytes (asm+0x26C, TAE event 322's b0 = 1)
// get 0xFF every update, before the weapon update reads them (the ChrAsm update resets them to
// 0xFE after); stopping the writes brings it back at the next weapon update. Independent of the
// vial (vial_in_hand): only the ChrAsm reset and the event 322 store must match.
u8* g_hide_module = nullptr; // the eboot when the reset and the store matched
bool g_right_hidden = false;
u64 g_right_hides = 0;

std::atomic_bool g_lever_left_hidden{false};
void LeverHideTick(u8* chr) {
    VR::Lever::HandPresentation held{};
    const bool active = g_hide_module && chr &&
        VR::Lever::Channel().PresentedHand(held) && VR::Lever::Fresh(held.ns, SteadyNs()) &&
        held.hand >= 0 && held.hand <= 1;
    bool hidden = false;
    if (active) {
        auto* chr_asm = Read<u8*>(chr + OffsetChrAsm);
        if (Plausible(chr_asm)) {
            auto* parts = chr_asm + OffsetAsmOverride + held.hand * 4;
            for (int i = 0; i < 4; ++i)
                parts[i] = 0xFF;
            hidden = true;
        }
    }
    g_lever_left_hidden.store(hidden && held.hand == 0, std::memory_order_relaxed);
}

void MusicBoxHideTick(u8* chr) {
    if (g_hide_module == nullptr || chr == nullptr) {
        return;
    }
    if (!VR::MusicBox::HideReady().load(std::memory_order_relaxed)) {
        VR::MusicBox::HideReady().store(true, std::memory_order_relaxed);
    }
    VR::MusicBox::Scene mb{};
    const s64 now = SteadyNs();
    const bool hide = VR::MusicBox::LatestScene(mb) && VR::MusicBox::ActiveAt(mb, now);
    if (hide) {
        u8* const chr_asm = Read<u8*>(chr + OffsetChrAsm);
        if (Plausible(chr_asm)) {
            u8* const bytes = chr_asm + OffsetAsmOverride + 4; // right parts 0-3
            for (u32 i = 0; i < 4; ++i) {
                bytes[i] = 0xFF;
            }
            ++g_right_hides;
        }
    }
    if (hide != g_right_hidden) {
        g_right_hidden = hide;
        LOG_INFO(Core, "VR held: MUSIC BOX {} ({} updates hidden)",
                 hide ? "in the left hand: the right weapon put away (its parts 0-3 hidden)"
                      : "gone: the right weapon back in the hand",
                 g_right_hides);
        if (!hide) {
            g_right_hides = 0;
        }
    }
}

// ---- The torch held up (vr.ini torch_lift, core/vr/torch_lift.h) --------------------------------
//
// While the Torch or the Hunter's Torch is in the left hand (and not hidden by a held item), every
// update asks for one or two more copies of its own flame on its flame dummy in private slots, by
// how high you hold it (the vial's call: a slot effect lives while it is asked for every update,
// so lowering it ends them within an update). Its own effects are separate from the vial's: they
// install on the effect sites alone, whatever vial_in_hand says.

constexpr s32 FirstTorchSlot = 30020; // private slots (the vial's: 30000 ..)
constexpr s32 LastTorchSlot = 30029;
constexpr u32 OffsetNodeCurDummy = 0x28; // the spawned effect's dummy (the request's at +0x2C)
constexpr u32 OffsetNodeReqDummy = 0x2C;
constexpr u32 OffsetChrEquip = 0x4B8; // the hands' weapons (bloodborne_body.cpp ReadWeaponId)
constexpr u32 OffsetEquipLeftSlot = 0x0C;
constexpr u32 OffsetEquipIds = 0x64;
constexpr s32 LeftFistDummy = 340;              // L_Weapon's attach (the torch's hand)
constexpr s64 TorchScanNs = 1000 * Ms;          // the flame looked for again this often
constexpr u32 TorchParamDumpBytes = 0x1C0;      // its EquipParamWeapon row, logged once

u8* g_torch_base = nullptr;
SfxSlotRequestFn g_torch_sfx = nullptr;
DmyPolyFn g_torch_dmy = nullptr;

struct TorchState {
    s32 weapon{-2};
    bool torch{false};
    int copies{0};
    s32 ffx{0};
    s32 dummy{0};
    const char* source{"none"};
    s64 scan_ns{0};
    bool explained{false}; // the "nothing to add" line with its discovery logs
    s32 far_dummy{0};      // the torch's dummy farthest from the fist (the flame, presumably)
    // The dummy probe (20000 .. 20299), a few ids per update (no one-frame stall), then its log.
    s32 probe_next{-1}; // -1: not started; 20300: done
    float probe_far{-1.0f};
    std::string probe_list{};
    std::string nodes_text{};
};
TorchState g_torch{};

s32 LeftWeaponId(const u8* chr) {
    const u8* const equip = Read<const u8*>(chr + OffsetChrEquip);
    if (!Plausible(equip)) {
        return -1;
    }
    const s32 slot = Read<s32>(equip + OffsetEquipLeftSlot) * 2;
    return slot >= 0 && slot <= 4 ? Read<s32>(equip + OffsetEquipIds + 4 * slot) : -1;
}

// The character's slot effects (its effect controller's nodes), at most out.size().
u32 ReadSlotNodes(const u8* ctl, std::array<VR::Torch::Node, 32>& out) {
    u32 count = 0;
    u8* node = Read<u8*>(ctl + OffsetCtlHead);
    for (u32 hops = 0; node != nullptr && hops < MaxHops && Plausible(node); ++hops) {
        if ((Read<u16>(node + OffsetNodeFlags) & 0x7FFF) == NodeSlotFlags && count < out.size()) {
            const s32 cur = Read<s32>(node + OffsetNodeCur);
            out[count++] = {Read<s32>(node + OffsetNodeSlot), Read<s32>(node + OffsetNodeReq), cur,
                            Read<s32>(node + (cur >= 0 ? OffsetNodeCurDummy : OffsetNodeReqDummy))};
        }
        node = Read<u8*>(node + OffsetNodeNext);
    }
    return count;
}

// The dummy's world position (GetDmyPolyMtx: the first poly with that id).
bool TorchDummyAt(u8* chr, s32 id, std::array<float, 3>& at) {
    alignas(16) std::array<float, 8 * 12> m{};
    if (g_torch_dmy == nullptr || g_torch_dmy(chr, m.data(), &id, 1) < 1) {
        return false;
    }
    at = {m[3], m[7], m[11]};
    return std::isfinite(at[0]) && std::isfinite(at[1]) && std::isfinite(at[2]);
}

constexpr s32 TorchProbeFirst = 20000;
constexpr s32 TorchProbeEnd = 20300;
constexpr s32 TorchProbePerUpdate = 32;

// Starts the discovery (once per torch): the slot effects now, the dummies over the next updates.
void ExplainTorch(TorchState& t, const std::array<VR::Torch::Node, 32>& nodes, u32 count) {
    t.nodes_text.clear();
    for (u32 i = 0; i < count; ++i) {
        t.nodes_text += fmt::format(" [slot {} req {} cur {} dmy {}]", nodes[i].slot,
                                    nodes[i].req, nodes[i].cur, nodes[i].dummy);
    }
    t.probe_next = TorchProbeFirst;
    t.probe_far = -1.0f;
    t.probe_list.clear();
}

// One update's share of the dummy probe: its dummies (20000 + its part's ids) and how far each is
// from the fist, remembering the farthest (its head, where the flame burns). At the end, the log
// with its EquipParamWeapon row.
void ProbeTorch(u8* chr, TorchState& t) {
    if (t.probe_next < TorchProbeFirst || t.probe_next >= TorchProbeEnd) {
        return;
    }
    std::array<float, 3> fist{};
    const bool have_fist = TorchDummyAt(chr, LeftFistDummy, fist);
    const s32 end = std::min(t.probe_next + TorchProbePerUpdate, TorchProbeEnd);
    for (s32 id = t.probe_next; id < end; ++id) {
        std::array<float, 3> at{};
        if (!TorchDummyAt(chr, id, at)) {
            continue;
        }
        const float d = have_fist ? std::hypot(at[0] - fist[0], at[1] - fist[1], at[2] - fist[2])
                                  : -1.0f;
        t.probe_list += fmt::format(" {}({:.0f} cm)", id, d * 100.0f);
        if (d > t.probe_far) {
            t.probe_far = d;
            t.far_dummy = id;
        }
    }
    t.probe_next = end;
    if (t.probe_next < TorchProbeEnd) {
        return;
    }
    LOG_INFO(Core,
             "VR torch: its slot effects:{}; its dummies:{} (the farthest from the fist: {}){}",
             t.nodes_text.empty() ? " none" : t.nodes_text,
             t.probe_list.empty() ? " none" : t.probe_list, t.far_dummy,
             t.ffx > 0 ? "" : "; its flame is not among the slot effects: set torch_lift_ffx (and "
                              "torch_lift_dummy) from these and the param row below");
    if (const u8* row = GunWeaponParamRow(t.weapon); row != nullptr) {
        std::string words;
        for (u32 o = 0; o < TorchParamDumpBytes; o += 4) {
            words += fmt::format("{}{:03X}:{}", o % 32 == 0 ? "\n  " : " ", o, Read<s32>(row + o));
        }
        LOG_INFO(Core, "VR torch: EquipParamWeapon {} (s32 words, hex offset:value):{}", t.weapon,
                 words);
    }
}

void TorchTick(u8* chr) {
    const auto& ts = VR::Torch::GetTorchSettings();
    if (g_torch_sfx == nullptr || chr == nullptr || !ts.enabled) {
        return;
    }
    auto& t = g_torch;
    const s32 weapon = LeftWeaponId(chr);
    if (weapon != t.weapon) {
        t = {};
        t.weapon = weapon;
        PiNativeVialVisualInput owner{};
        owner.operation = PI_VV_TORCH_OWNER;
        owner.torch.weapon_id = weapon;
        const auto selected = g_held_visual.Call(owner);
        t.torch = selected.applied && selected.torch.torch != 0;
        if (t.torch) {
            LOG_INFO(Core, "VR torch: {} in the left hand: hold it up for more light", weapon);
        }
    }
    if (!t.torch) {
        return;
    }
    u8* const ctl = chr + OffsetSfxCtl;
    if (Read<u8*>(ctl) != g_torch_base + SfxCtlVtableRva) {
        return;
    }
    const s64 now = SteadyNs();
    // The flame: vr.ini's, else the torch's own among the slot effects (looked for every second
    // while unknown).
    if (ts.ffx > 0) {
        t.ffx = ts.ffx;
        t.dummy = ts.dummy > 0 ? ts.dummy : t.far_dummy;
        t.source = "vr.ini";
    } else if (t.ffx == 0 && now - t.scan_ns >= TorchScanNs) {
        t.scan_ns = now;
        std::array<VR::Torch::Node, 32> nodes{};
        const u32 count = ReadSlotNodes(ctl, nodes);
        if (VR::Torch::PickFlame(nodes.data(), count, FirstTorchSlot, LastTorchSlot, t.ffx,
                                 t.dummy)) {
            t.source = "its own";
            LOG_INFO(Core, "VR torch: its flame is effect {} on dummy {}", t.ffx, t.dummy);
        } else if (!t.explained) {
            t.explained = true;
            ExplainTorch(t, nodes, count);
        }
    }
    if (t.dummy == 0 && t.ffx > 0 && !t.explained) {
        // vr.ini's flame without its dummy: the farthest from the fist, once the probe has run.
        t.explained = true;
        std::array<VR::Torch::Node, 32> nodes{};
        ExplainTorch(t, nodes, ReadSlotNodes(ctl, nodes));
    }
    ProbeTorch(chr, t);
    if (t.dummy == 0 && t.ffx > 0 && t.far_dummy > 0) {
        t.dummy = t.far_dummy;
    }
    VR::Body::HandsSample hands{};
    PiNativeVialVisualInput lift{};
    lift.operation = PI_VV_TORCH;
    lift.now_ns = now;
    auto& facts = lift.torch;
    facts.enabled = ts.enabled;
    facts.hidden = HeldLeftHidden();
    facts.have_hands = VR::Body::LatestHands(hands);
    facts.hands_ns = hands.steady_ns;
    facts.head_valid = hands.head_valid;
    facts.position_valid = hands.hand[0].position_valid;
    facts.hand[0] = hands.hand[0].p.x;
    facts.hand[1] = hands.hand[0].p.y;
    facts.hand[2] = hands.hand[0].p.z;
    facts.head_y = hands.head_p.y;
    facts.raised_m = ts.raised_m;
    facts.high_m = ts.high_m;
    facts.hysteresis_m = ts.hysteresis_m;
    facts.max_copies = ts.copies;
    facts.ffx = t.ffx;
    facts.dummy = t.dummy;
    const auto lifted = g_held_visual.Call(lift);
    const PiTorchOutput decision = lifted.applied ? lifted.torch : PiTorchOutput{};
    const int level = static_cast<int>(decision.level), copies = static_cast<int>(decision.copies);
    if (copies != t.copies) {
        LOG_INFO(Core, "VR torch: {} ({} more flame{}, effect {} on dummy {}, {})",
                 level == 2 ? "held high" : level == 1 ? "raised" : "lowered", copies,
                 copies == 1 ? "" : "s", t.ffx, t.dummy, t.source);
        t.copies = copies;
    }
    for (int i = 0; i < copies; ++i) {
        const s32 dummy = t.dummy;
        g_torch_sfx(ctl, FirstTorchSlot + i, t.ffx, &dummy, 0); // repeat 0: ends when not asked
    }
}

// The effect sites alone (the vial's checks without its settings and hide sites).
void InstallTorch(u8* module, u64 size) {
    if (!VR::Torch::GetTorchSettings().enabled) {
        LOG_INFO(Core, "VR torch: torch_lift=0 (the game's torch)");
        return;
    }
    if (size < SfxCtlVtableRva + sizeof(u64)) {
        return;
    }
    const bool ok =
        std::memcmp(module + SfxSlotRequestRva, SfxSlotRequestSig.data(),
                    SfxSlotRequestSig.size()) == 0 &&
        std::memcmp(module + CtlCtorRva, CtlCtorSig.data(), CtlCtorSig.size()) == 0 &&
        std::memcmp(module + NodeCtorRva, NodeCtorSig.data(), NodeCtorSig.size()) == 0 &&
        std::memcmp(module + NodeUpdateRva, NodeUpdateSig.data(), NodeUpdateSig.size()) == 0;
    if (!ok) {
        LOG_WARNING(Core, "VR torch: the effect sites do not match; torch_lift off");
        return;
    }
    InitHeldVisualDll();
    g_torch_base = module;
    g_torch_sfx = reinterpret_cast<SfxSlotRequestFn>(module + SfxSlotRequestRva);
    if (std::memcmp(module + DmyPolyRva, DmyPolySig.data(), DmyPolySig.size()) == 0) {
        g_torch_dmy = reinterpret_cast<DmyPolyFn>(module + DmyPolyRva);
    }
    const auto& ts = VR::Torch::GetTorchSettings();
    LOG_INFO(Core,
             "VR torch: ready (raised from {:.0f} cm, high from {:.0f} cm against the eyes, up to "
             "{} more flame{}; the flame: {})",
             ts.raised_m * 100.0f, ts.high_m * 100.0f, ts.copies, ts.copies == 1 ? "" : "s",
             ts.ffx > 0 ? fmt::format("effect {} (vr.ini)", ts.ffx)
                        : std::string{"the torch's own, found in the game"});
}

} // Anonymous namespace

bool HideRightWeaponPart(u8* chr, u32 part) {
    if (g_hide_module == nullptr || chr == nullptr || part > 3) {
        return false;
    }
    u8* const chr_asm = Read<u8*>(chr + OffsetChrAsm);
    if (!Plausible(chr_asm)) {
        return false;
    }
    chr_asm[OffsetAsmOverride + 4 + part] = 0xFF; // (the music box's byte, one part)
    return true;
}

void InstallHeld(VAddr base, u64 size) {
    if (g_ready || g_base != nullptr) {
        return;
    }
    if (g_torch_sfx == nullptr) {
        InstallTorch(reinterpret_cast<u8*>(base), size); // the torch held up (torch_lift)
    }
    if (g_hide_module == nullptr) {
        // quick_music_box_crank: the right weapon's hide needs only these two. Without it the
        // box is used the game's way (core/vr/music_box.h HideReady).
        auto* const eboot = reinterpret_cast<u8*>(base);
        const bool fits = size >= std::max(AsmResetRva + AsmResetSig.size(),
                                           E322StoreRva + E322StoreSig.size());
        const bool reset_ok =
            fits && std::memcmp(eboot + AsmResetRva, AsmResetSig.data(), AsmResetSig.size()) == 0;
        const bool store_ok =
            fits &&
            std::memcmp(eboot + E322StoreRva, E322StoreSig.data(), E322StoreSig.size()) == 0;
        g_hide_module = reset_ok && store_ok ? eboot : nullptr;
        LOG_INFO(Core, "VR held: the right weapon's hide (music box crank) {}",
                 g_hide_module != nullptr ? "ready (ChrAsm reset, event 322 store ok)"
                 : !fits ? "UNAVAILABLE (eboot too small): the music box is used the game's way"
                         : "UNAVAILABLE (signature mismatch): the music box is used the game's way");
    }
    const auto& settings = VR::GetSettings();
    VR::HeldStatus status{};
    status.state = static_cast<u8>(H::State::Unavailable);
    if (!settings.inject || !settings.vial_visual || !settings.vial_in_hand_game) {
        status.reason = "off";
        VR::PublishHeldStatus(status);
        LOG_INFO(Core, "VR held: the game's vial is off (inject={} vial_visual={} vial_in_hand={})",
                 settings.inject ? 1 : 0, settings.vial_visual ? 1 : 0,
                 settings.vial_in_hand_game ? "game" : "overlay");
        return;
    }
    auto* const module = reinterpret_cast<u8*>(base);
    if (size < SfxCtlVtableRva + sizeof(u64)) {
        status.reason = "eboot";
        VR::PublishHeldStatus(status);
        LOG_INFO(Core, "VR held: eboot too small ({:#x}) for Bloodborne 1.09; the overlay vial",
                 size);
        return;
    }
    struct Site {
        const char* name;
        u64 rva;
        const u8* sig;
        size_t length;
    };
    const std::array<Site, 8> sites = {{
        {"SfxSlotRequest", SfxSlotRequestRva, SfxSlotRequestSig.data(), SfxSlotRequestSig.size()},
        {"event 96 call", E96CallRva, E96CallSig.data(), E96CallSig.size()},
        {"effect controller vtable", CtlCtorRva, CtlCtorSig.data(), CtlCtorSig.size()},
        {"node constructor", NodeCtorRva, NodeCtorSig.data(), NodeCtorSig.size()},
        {"node update", NodeUpdateRva, NodeUpdateSig.data(), NodeUpdateSig.size()},
        {"node ending", NodeEndingRva, NodeEndingSig.data(), NodeEndingSig.size()},
        {"ChrAsm reset", AsmResetRva, AsmResetSig.data(), AsmResetSig.size()},
        {"event 322 store", E322StoreRva, E322StoreSig.data(), E322StoreSig.size()},
    }};
    bool ok = true;
    for (const Site& site : sites) {
        if (std::memcmp(module + site.rva, site.sig, site.length) != 0) {
            ok = false;
            LOG_WARNING(Core, "VR held: {} signature MISMATCH at {:#x} (live bytes {})", site.name,
                        site.rva, Hook::HexBytes(module + site.rva, site.length));
        }
    }
    if (!ok) {
        status.reason = "signature";
        VR::PublishHeldStatus(status);
        LOG_WARNING(Core, "VR held: not installed; the armed vial is the overlay vial");
        return;
    }
    g_base = module;
    g_sfx_slot = reinterpret_cast<SfxSlotRequestFn>(module + SfxSlotRequestRva);
    if (std::memcmp(module + DmyPolyRva, DmyPolySig.data(), DmyPolySig.size()) == 0) {
        g_dmy_poly = reinterpret_cast<DmyPolyFn>(module + DmyPolyRva);
    } else {
        LOG_WARNING(Core,
                    "VR held: GetDmyPolyMtx signature MISMATCH (live bytes {}); no DIBR depth "
                    "proxy (the vial may look as far away as what is behind it)",
                    Hook::HexBytes(module + DmyPolyRva, DmyPolySig.size()));
    }
    if (const char* value = std::getenv("VR_HELD_LOG"); value != nullptr) {
        g_log = std::max(std::atoi(value), 1);
    }
    g_dry_run = std::getenv("VR_HELD_DRYRUN") != nullptr;
    InitHeldVisualDll();
    g_ready = true;
    LOG_INFO(
        Core,
        "VR held: game vial ready (SfxSlotRequest, event 96 call, controller and node vtables, "
        "node update, ChrAsm reset, event 322 store ok; FFX {} on dummy {}, slots {}-{}, "
        "vial_hide_left={} vial_hide_two_handed={} DIBR depth proxy {}){}{}",
        VialFfx, VialDummy, H::FirstVialSlot, H::FirstVialSlot + H::VialSlotCount - 1,
        settings.vial_hide_left, settings.vial_hide_two_handed ? 1 : 0,
        g_dmy_poly == nullptr      ? "unavailable"
        : settings.vial_dibr_proxy ? "on"
                                   : "off (vial_dibr_proxy=0)",
        g_log > 0 ? fmt::format(" log={}", g_log) : std::string{}, g_dry_run ? " DRY RUN" : "");
}

void HeldPoseTick(u8* chr, const HeldGate& gate) {
    MusicBoxHideTick(chr);
    if (g_ready && chr != nullptr) {
        Tick(chr, gate, "pose copy");
    }
    LeverHideTick(chr);
    TorchTick(chr); // after the vial's tick: its hide of the left weapon is known
}

void HeldSiteATick(u8* player) {
    if (g_ready && player != nullptr && !PoseHookInstalled()) {
        Tick(player, HeldGate{}, "player update");
    }
    if (player != nullptr && !PoseHookInstalled()) {
        TorchTick(player);
    }
}

void HeldRenderTick(u64 record_id, const float* rows, const float* position) {
    u8* const chr = g_proxy_chr.load(std::memory_order_relaxed);
    if (chr == nullptr || g_dmy_poly == nullptr || rows == nullptr || position == nullptr ||
        !VR::GetSettings().vial_dibr_proxy) {
        return;
    }
    u8* const world = Read<u8*>(g_base + WorldChrManRva);
    if (!Plausible(world) || Read<u8*>(world + OffsetMainPlayer) != chr) {
        return;
    }
    alignas(16) std::array<float, 8 * 12> m{};
    const s32 id = VialDummy;
    if (g_dmy_poly(chr, m.data(), &id, 1) < 1) {
        return;
    }
    // Dummy frame -> world (the columns are its axes) -> the render camera's view (its rows:
    // right, up, forward; position p).
    const auto view = [&](const std::array<float, 3>& l) {
        VR::Vec3 v{};
        float d[3];
        for (int i = 0; i < 3; ++i) {
            d[i] = m[4 * i] * l[0] + m[4 * i + 1] * l[1] + m[4 * i + 2] * l[2] + m[4 * i + 3] -
                   position[i];
        }
        v.x = rows[0] * d[0] + rows[1] * d[1] + rows[2] * d[2];
        v.y = rows[4] * d[0] + rows[5] * d[1] + rows[6] * d[2];
        v.z = rows[8] * d[0] + rows[9] * d[1] + rows[10] * d[2];
        return v;
    };
    // VR_HELD_PROXY=ax,ay,az,bx,by,bz,r (dummy frame, metres): another capsule, for tests.
    static const std::array<float, 7> capsule = [] {
        std::array<float, 7> c{ProxyA[0], ProxyA[1], ProxyA[2],  ProxyB[0],
                               ProxyB[1], ProxyB[2], ProxyRadius};
        if (const char* value = std::getenv("VR_HELD_PROXY"); value != nullptr) {
            std::array<float, 7> v{};
            if (std::sscanf(value, "%f,%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
                            &v[6]) == 7) {
                c = v;
                LOG_INFO(Core, "VR held: VR_HELD_PROXY capsule {}", value);
            }
        }
        return c;
    }();
    VR::Vec3 a = view({capsule[0], capsule[1], capsule[2]});
    VR::Vec3 b = view({capsule[3], capsule[4], capsule[5]});
    // inframe_hand_sync: the vial in his hand (ours, vial_inject_drain, a hand item) is drawn
    // carried with your play space from the update his pose was placed for; its depth goes there
    // with it (the same move), not one update of your walk or strafe behind it.
    if (VR::InFrame::HandCarry carry{}; VR::InFrame::GetHandCarry(record_id, carry)) {
        const auto pa = carry.rv.Point({a.x, a.y, a.z});
        const auto pb = carry.rv.Point({b.x, b.y, b.z});
        const float shift = std::hypot(0.5f * (pa[0] + pb[0] - a.x - b.x),
                                       0.5f * (pa[1] + pb[1] - a.y - b.y),
                                       0.5f * (pa[2] + pb[2] - a.z - b.z)) /
                            std::max(carry.to.scale, 1e-3f);
        a = {pa[0], pa[1], pa[2]};
        b = {pb[0], pb[1], pb[2]};
        static u64 n = 0;
        static double sum = 0.0;
        static float most = 0.0f;
        ++n;
        sum += shift;
        most = std::max(most, shift);
        if (n == 1 || n % (g_log > 0 ? 60u : 1800u) == 0) { // (VR_HELD_LOG: every 60)
            LOG_INFO(Core,
                     "VR held: depth proxy carried with the hand (inframe_hand_sync): {} updates, "
                     "{:.1f} cm avg, {:.1f} cm max (where it was before: behind the drawn vial)",
                     n, 100.0 * sum / static_cast<double>(n), 100.0f * most);
        }
    }
    for (const float v : {a.x, a.y, a.z, b.x, b.y, b.z}) {
        if (!std::isfinite(v) || std::abs(v) > 100.0f) {
            return; // not a hand at arm's length
        }
    }
    VR::SetRecordHeldProxy(record_id, a, b, capsule[6]);
    if (g_log > 0) {
        static s64 last = 0;
        if (const s64 now = SteadyNs(); now - last >= 1000 * Ms) {
            last = now;
            const VR::Vec3 o = view({0.0f, 0.0f, 0.0f});
            const VR::Vec3 x = view({0.1f, 0.0f, 0.0f});
            const VR::Vec3 y = view({0.0f, 0.1f, 0.0f});
            const VR::Vec3 z = view({0.0f, 0.0f, 0.1f});
            LOG_INFO(
                Core,
                "VR held: depth proxy record {} view a ({:.3f},{:.3f},{:.3f}) b "
                "({:.3f},{:.3f},{:.3f}) | dummy {} world ({:.3f},{:.3f},{:.3f}) view "
                "({:.3f},{:.3f},{:.3f}) axes x ({:.2f},{:.2f},{:.2f}) y ({:.2f},{:.2f},{:.2f}) "
                "z ({:.2f},{:.2f},{:.2f})",
                record_id, a.x, a.y, a.z, b.x, b.y, b.z, VialDummy, m[3], m[7], m[11], o.x, o.y,
                o.z, (x.x - o.x) * 10.0f, (x.y - o.y) * 10.0f, (x.z - o.z) * 10.0f,
                (y.x - o.x) * 10.0f, (y.y - o.y) * 10.0f, (y.z - o.z) * 10.0f, (z.x - o.x) * 10.0f,
                (z.y - o.y) * 10.0f, (z.z - o.z) * 10.0f);
        }
    }
}

bool HeldLeftHidden() {
    return g_left_hidden.load(std::memory_order_relaxed) ||
           g_lever_left_hidden.load(std::memory_order_relaxed);
}

bool HeldVialOurs(s64 now_ns, u64& drain_word) {
    const s64 ns = g_ours_ns.load(std::memory_order_acquire);
    drain_word = g_ours_word.load(std::memory_order_relaxed);
    return ns != 0 && now_ns - ns < OursFreshNs;
}

HeldGripState HeldGrip() {
    const u64 word = g_grip.load(std::memory_order_relaxed);
    return {static_cast<u8>(word), word ? static_cast<s32>(word >> 8) : -1};
}
u8 HeldGripKind() {
    return HeldGrip().kind;
}

} // namespace VR::Bloodborne
