// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "common/thread.h"
#include "core/vr/bloodborne_assets.h"
#include "core/vr/bloodborne_gun.h"
#include "core/vr/bloodborne_heal.h"
#include "core/vr/bloodborne_held.h"
#include "core/vr/bloodborne_melee.h"
#include "core/vr/bloodborne_quickitems.h"
#include "core/vr/bloodborne_whip.h"
#include "core/vr/body_channel.h"
#include "core/vr/body_frame.h"
#include "core/vr/grip_lab_pose.h"
#include "core/vr/grip_preview.h"
#include "core/vr/hook_util.h"
#include "core/vr/hook_wrap.h"
#include "core/vr/inframe.h"
#include "core/vr/interaction_native_quick.h"
#include "core/vr/interaction_native_vial_visual.h"
#include "core/vr/item_attachment.h"
#include "core/vr/live_tune.h"
#include "core/vr/music_box.h"
#include "core/vr/player_body.h"
#include "core/vr/quick_channel.h"
#include "core/vr/quick_items.h"
#include "core/vr/quick_settings.h"
#include "core/vr/seqlock.h"
#include "core/vr/vial_drain.h"
#include "core/vr/vial_liquid.h"
#include "core/vr/vial_needle.h"
#include "core/vr/vr_session.h"

namespace VR::Bloodborne {

namespace {

namespace Q = VR::Quick;
namespace F = VR::InFrame;
using Hook::Plausible;
using Hook::Read;

// Owned by the existing in-frame renderer caller. Retain the module while guest
// hooks are installed; fill history survives controls resets as the old static did.
VR::Interactions::Module* g_vial_visual_module{};
bool g_vial_visual_fault{};
VR::Interactions::NativeVialVisualRuntime g_vial_visual;
void InitVialVisualDll() {
    static const bool initialized = [] {
        if (!VR::Interactions::DllRequested())
            return true;
        g_vial_visual_module = new VR::Interactions::Module();
        g_vial_visual_module->EnableFaultBroadcast();
        // Bind before load: a missing/incompatible DLL must never run legacy policy.
        g_vial_visual.Bind(*g_vial_visual_module, g_vial_visual_fault);
        if (!g_vial_visual_module->LoadBesideExecutable()) {
            g_vial_visual_fault = true;
            g_vial_visual_module->ReportFault();
            LOG_ERROR(Core, "VR vial visual: interaction DLL unavailable: {}",
                      g_vial_visual_module->Diagnostic());
            return false;
        }
        return true;
    }();
    (void)initialized;
}

// quick_arc_pick=sector, except in left_scheme=2: there the trigger off the cards is the gun, and
// the sector (anything 7-30 cm from the arc's centre, +-120 degrees) took most gun pulls as a card.
bool PickSector(const Q::QuickSettings& qs) {
    const auto& vs = VR::GetSettings();
    return qs.pick_sector && (vs.controls_classic || vs.left_scheme != 2);
}

// Module-relative addresses in the 1.09 eboot (patch file address - 0x400000); every range is
// byte-identical in the original and the -mods eboot (quickitems_mech.md).
// PlayerIns::UseGoods (vt+0x408): the item animations' event 65 and the vial's event call it.
constexpr u64 UseGoodsRva = 0x18F9720;
constexpr std::array<u8, 24> UseGoodsSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
    0x53, 0x48, 0x83, 0xEC, 0x18, 0x41, 0x89, 0xD5, 0x41, 0x89, 0xF7, 0x48,
};
// UseGoods' `mov rax, [rip+...]` at 0x18F9851 proves the MenuMan global (0x5562878; +0x1C0 > 0:
// an inventory multi-use is pending).
constexpr u64 MenuManLeaRva = 0x18F9851;
constexpr std::array<u8, 7> MenuManLeaSig = {0x48, 0x8B, 0x05, 0x20, 0x90, 0xC6, 0x03};
constexpr u64 MenuManRva = 0x5562878;
// GoodsParamRow, `void(Out{s32 id; u32 pad; u8* row}*, s32 goods)`: the EquipParamGoods row.
constexpr u64 GoodsRowRva = 0x1F1E3D0;
constexpr std::array<u8, 32> GoodsRowSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x41, 0x89, 0xF4, 0x49, 0x89,
    0xFE, 0x31, 0xC9, 0x45, 0x85, 0xE4, 0x0F, 0x88, 0xF2, 0x00, 0x00, 0x00, 0x4C, 0x8D, 0x3D, 0x4D,
};
// CanUseGoods, `u8(s32 goods, chr, [chr+0x1C8], [chr+0x78], right weapon, left weapon, menu
// multi-use)` (the 7th on the stack), called exactly as IsItemUsable 0x18C3840 does.
constexpr u64 CanUseRva = 0x157F200;
constexpr std::array<u8, 32> CanUseSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC,
    0xE8, 0x01, 0x00, 0x00, 0x45, 0x89, 0xCC, 0x44, 0x89, 0x85, 0x4C, 0xFE, 0xFF, 0xFF, 0x89, 0x8D,
};
// PlayerIns vt+0x388 GetWeaponId(chr, slot): -1 = the right hand's, -2 = the left hand's.
constexpr u64 WeaponIdRva = 0x18FB090;
constexpr std::array<u8, 24> WeaponIdSig = {
    0x48, 0x8B, 0x87, 0xB8, 0x04, 0x00, 0x00, 0x8D, 0x4E, 0x06, 0x83, 0xF9,
    0x05, 0x77, 0x3E, 0x48, 0x8D, 0x15, 0x42, 0x00, 0x00, 0x00, 0x48, 0x63,
};
// The HUD's D-pad "next item" path (0x165BB12): SelectNext, then `byte [hud+0x174] = 1` and the
// three global bytes 0x5595468 (C6 05 rel32), 0x553EA18 and 0x553EA1B (lea + mov byte [rax]).
// These exact bytes prove the three addresses.
constexpr u64 DpadRva = 0x165BB12;
constexpr std::array<u8, 59> DpadSig = {
    0x48, 0x8D, 0x05, 0x17, 0xF6, 0xED, 0x03, 0x48, 0x8B, 0x00, 0xBF, 0xD0, 0x03, 0x00, 0x00,
    0x48, 0x03, 0x78, 0x08, 0xE8, 0x46, 0x36, 0xE8, 0xFF, 0x41, 0xC6, 0x85, 0x74, 0x01, 0x00,
    0x00, 0x01, 0xC6, 0x05, 0x2F, 0x99, 0xF3, 0x03, 0x01, 0x48, 0x8D, 0x05, 0xD8, 0x2E, 0xEE,
    0x03, 0xC6, 0x00, 0x01, 0x48, 0x8D, 0x05, 0xD1, 0x2E, 0xEE, 0x03, 0xC6, 0x00, 0x01,
};
constexpr std::array<u64, 3> HudByteRvas = {0x5595468, 0x553EA18, 0x553EA1B};
// The inventory quantity store (the "Infinite Items" cheat patches it).
constexpr u64 QuantityStoreRva = 0x14D9556;
constexpr std::array<u8, 7> QuantityStoreSig = {0x41, 0x89, 0xC4, 0x44, 0x89, 0x63, 0x08};
// ChrIns::GetDmyPolyMtx (the palm for the arc and our held mesh).
constexpr u64 DmyPolyRva = 0x18C3BF0;
constexpr std::array<u8, 32> DmyPolySig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC,
    0x28, 0x41, 0x89, 0xCF, 0x48, 0x89, 0xD3, 0x48, 0x89, 0x75, 0xB0, 0x4C, 0x8B, 0x35, 0xCE, 0xFF,
};
// H2: the tail of the projectile launch init 0x19F6CA0 (r14 = the projectile P; its velocity
// P+0x40 / +0x50 and orientation P+0x30 are set). The 16 patched bytes are three whole
// position-independent instructions (`vmovss [r14+0x734], xmm0`, `mov rax, [r15]`, `cmp rax,
// [rbp-0x30]`) replayed after our call; the return lands on the jnz that reads the replayed
// flags. No branch lands inside them (plan_throwables.md 3.2). Context: 81 bytes from 0x19F77F4.
constexpr u64 LaunchRva = 0x19F7835;
constexpr u64 LaunchCtxRva = 0x19F77F4;
constexpr size_t LaunchLen = 16;
constexpr std::array<u8, 81> LaunchCtxSig = {
    0x41, 0x8B, 0x86, 0xE0, 0x02, 0x00, 0x00, 0x83, 0xF8, 0x03, 0x74, 0x0E, 0x83, 0xF8, 0x02,
    0x75, 0x12, 0x49, 0x8D, 0x9E, 0x20, 0x08, 0x00, 0x00, 0xEB, 0x15, 0x49, 0x8D, 0x9E, 0x48,
    0x08, 0x00, 0x00, 0xEB, 0x0C, 0x83, 0xF8, 0x01, 0x75, 0x07, 0x49, 0x8D, 0x9E, 0xF8, 0x07,
    0x00, 0x00, 0x48, 0x8B, 0x43, 0x10, 0xC5, 0xF8, 0x57, 0xC0, 0x48, 0x85, 0xC0, 0x74, 0x05,
    0xC5, 0xFA, 0x10, 0x40, 0x10, 0xC4, 0xC1, 0x7A, 0x11, 0x86, 0x34, 0x07, 0x00, 0x00, 0x49,
    0x8B, 0x07, 0x48, 0x3B, 0x45, 0xD0,
};
constexpr size_t LaunchSiteOffset = LaunchRva - LaunchCtxRva; // the patched bytes in the context
constexpr u64 Mode2VtableRva = 0x5397050; // the projectile's mode-2 (ballistic) object
// EquipGameData::AddItem(egd, -, const u32* handle, s32 count, 0, 1, 0): what picking an item up
// calls (0x17D91FF); new goods also go into the first empty quick slot. Tests only
// (VR_QUICK_TEST_GIVE, on the isolated save copy).
constexpr u64 AddItemRva = 0x14CDA70;
constexpr std::array<u8, 32> AddItemSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC,
    0xB8, 0x00, 0x00, 0x00, 0x44, 0x89, 0x8D, 0x20, 0xFF, 0xFF, 0xFF, 0x89, 0x8D, 0x24, 0xFF, 0xFF,
};
constexpr u64 WorldChrManRva = 0x553E878;
constexpr u32 OffsetMainPlayer = 0x60;

// PlayerIns.
constexpr u32 OffsetHandle = 0x08;
constexpr u32 OffsetType = 0x78;
constexpr u32 OffsetArmStyle = 0xF0;    // [[+0xF0]] & ~1 == 2: two-handed (no left weapon)
constexpr u32 OffsetEmitter = 0x180;    // the chr's own bullet emitter (-1 = free)
constexpr u32 OffsetSpEffects = 0x1C8;
constexpr u32 OffsetChrModules = 0x3B0;
constexpr u32 OffsetPlayerGameData = 0x3C0;
constexpr u32 OffsetItemUseState = 0x490; // the menu-item flow (UseGoods resets it)
constexpr u32 OffsetHp = 0xF8;            // ChrDataModule ([[+0x3B0]+0x20])
constexpr u32 OffsetMaxHp = 0xFC;         // ... (bloodborne_heal.cpp reads both the same way)
constexpr u32 OffsetDebugFlags = 0x200;
constexpr u16 FlagNoGoodsConsume = 0x200;
// PlayerGameData: EquipGameData at +0x1D0, its quick-item ring at +0x3D0 (= egd + 0x200).
constexpr u32 OffsetEgd = 0x1D0;
constexpr u32 OffsetChestArmor = 0xD4 + 4 * 9;
constexpr u32 OffsetQuickRing = 0x3D0;
constexpr u32 OffsetRingEgd = 0x10;      // ring -> egd (sanity)
constexpr u32 OffsetRingSelected = 0x48; // ring: selected inventory index (-1 none)
constexpr u32 OffsetSlotIndex = 0x21C;   // egd + 0x21C + 8k: slot k's inventory index
constexpr u32 OffsetOverride = 0x288;    // egd: an item picked in the inventory menu (>= 0)
// EquipGameData's inventory (= PlayerGameData+0x328 = egd + 0x158).
constexpr u32 OffsetInvActive = 0x168;
constexpr u32 OffsetInvNormalCap = 0x16C;
constexpr u32 OffsetInvKeyCap = 0x17C;
constexpr u32 OffsetInvNormalList = 0x1A0;
constexpr u32 OffsetInvKeyList = 0x1B0;
constexpr u32 OffsetInvHighest = 0x1E0;
// SpEffect list nodes.
constexpr u32 OffsetEffectHead = 0x08;
constexpr u32 OffsetNodeFlags = 0x1C;
constexpr u32 OffsetNodeId = 0x40;
constexpr u32 OffsetNodeNext = 0x58;
constexpr u32 NodeSkipMask = 0x800C0003;
constexpr s32 ItemAnimEffect = 207; // every item clip adds it (TAE) until after its use
// EquipParamGoods row.
constexpr u32 RowRefId = 0x00; // refId_default: the SpEffect a buff item puts on (Fire Paper 2200)
constexpr u32 RowVariation = 0x04;
constexpr u32 RowIcon = 0x2C;
constexpr u32 RowUseAnim = 0x3E;
constexpr u32 RowConsumeByte = 0x44; // bit 7: isConsume
// Projectile P.
constexpr u32 OffsetPQuat = 0x30;
constexpr u32 OffsetPVel = 0x40;
constexpr u32 OffsetPPrevVel = 0x50;
constexpr u32 OffsetPMode = 0x2E0;
constexpr u32 OffsetPBullet = 0x2F4;
constexpr u32 OffsetPOwner = 0x2FC;
constexpr u32 OffsetPPos = 0x10;
constexpr u32 OffsetPMode2 = 0x820;
constexpr u32 OffsetPHandle = 0x08;
constexpr u32 OffsetPFlags = 0x7F1; // bit 0: alive
constexpr u32 OffsetModeId = 0x08;
constexpr u32 OffsetModeRow = 0x10;
// BulletParam row (0xA4 bytes).
constexpr size_t BulletRowSize = 0xA4;
constexpr u32 RowLife = 0x10;
constexpr u32 RowDist = 0x14;
constexpr u32 RowGravityIn = 0x1C;
constexpr u32 RowGravityOut = 0x20;
constexpr u32 RowInitSpeed = 0x28;
constexpr u32 RowAccelIn = 0x2C;
constexpr u32 RowAccelOut = 0x30;
constexpr u32 RowMaxSpeed = 0x34;
constexpr u32 RowMinSpeed = 0x38;
constexpr u32 RowAccelTime = 0x3C;
constexpr u32 RowHitBullet = 0x68;  // the impact's child bullet (-1 none)
constexpr u32 RowShootPitch = 0x90; // s8 shootAngleXZ, degrees up
// The lock-on: LockTgtMan (0x188E900) sets chr+0xF8 = 1 and the lock point chr+0x100 (float4,
// world) for the target it chose; 0x18C7800 aims the game's own bullets at it.
constexpr u32 OffsetLockChosen = 0xF8;
constexpr u32 OffsetLockPoint = 0x100;
// The character's frame W: [[chr+0x58]+0x90], three float4 rows {R row i | t_i}.
constexpr u32 OffsetChrCtrl = 0x58;
constexpr u32 OffsetChrFrame = 0x90;
// The game's throw of an item, for the soft cap's range: from this high above the ground (m).
constexpr float ThrowHeightM = 1.5f;
// The throw's whoosh (throw_sound): the game's PlaySound, `void(u64* handle_out, const float
// pos[3], s32 type, s32 id)` (TAE event 128's call; the same site the gun's bang uses, see
// core/vr/bloodborne_gun.cpp), and event 128's sound type for SoundType 1 (`lea rcx, [table];
// cmp byte [rcx+3], 0; mov ecx, 0xA; jnz; mov ecx, 1`). The vanilla throw clips (a00.tae 100030-
// 100033) play SoundType 1 id 751 as the arm comes through, 130-170 ms before the release.
constexpr u64 PlaySoundRva = 0x13E0290;
constexpr std::array<u8, 24> PlaySoundSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x81, 0xEC,
    0x28, 0x01, 0x00, 0x00, 0x49, 0x89, 0xF6, 0x48, 0x89, 0xFB, 0x4C, 0x8B,
};
constexpr u64 SoundTypeRva = 0x1A2641B;
constexpr std::array<u8, 23> SoundTypeSig = {
    0x48, 0x8D, 0x0D, 0x26, 0xE5, 0xD0, 0x02, 0x80, 0x79, 0x03, 0x00, 0xB9,
    0x0A, 0x00, 0x00, 0x00, 0x75, 0x05, 0xB9, 0x01, 0x00, 0x00, 0x00,
};
constexpr s32 ThrowWhooshSound = 751;
// molotov_burst: a projectile's end of flight, `void(P, s32 hit, const float4* at)` (aligned).
// The mode-2 update 0x19F9E50 calls it with hit -1 and P+0x10 when the life P+0x734 runs out;
// it spawns the row's HitBulletID (cached at P+0x850) through CreateBullet 0x1A03E50 at `at`,
// unless P+0x7F1 bit 7 (a child already spawned; it sets it) or the row's byte +0x99 rules the
// hit out (0xFF: not on hit -1; 0xFE: only on -1).
constexpr u64 EndFlightRva = 0x19FBEB0;
constexpr std::array<u8, 32> EndFlightSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC,
    0xF8, 0x06, 0x00, 0x00, 0x48, 0x89, 0x95, 0xE8, 0xF8, 0xFF, 0xFF, 0x89, 0xF3, 0x49, 0x89, 0xFD,
};
constexpr u8 FlagChildSpawned = 0x80; // P+0x7F1
constexpr u32 OffsetPLife = 0x734;    // P: the mode's life left (s)
constexpr u32 OffsetPChild = 0x850;   // P: the HitBulletID its end of flight spawns
// P's per-mode objects (modes 1 / 2 / 3; each: +0x08 the bullet id, +0x10 its BulletParam row,
// read every update: the hit radius 0x19F8CD3).
constexpr std::array<u32, 3> OffsetPModes = {0x7F8, 0x820, 0x848};
constexpr u32 RowHitRadius = 0x44;    // m (the radius at launch)
constexpr u32 RowHitRadiusMax = 0x48; // m, < 0: none (the radius grows to it over spreadTime)
constexpr u32 RowHitFilter = 0x99;
constexpr float BurstDropMps = 30.0f; // molotov_burst_ground: the bottle's fall to the ground
// quick_use_sounds / quick_mouth_sound: an item used at once plays its use clip's own sound (a00
// TAE, SoundType 1: the drinking clip 100050's c100050 at 0.83 s, the bottle at the lips, 0.23 s
// before its ConsumeCurrentGoods; the table is core/vr/quick_items.h UseSoundsOf).

constexpr s64 Ms = 1'000'000;
constexpr s64 RequestTtlMs = 400;   // a request older than this is answered Expired
constexpr s64 ArmedTtlNs = 500 * Ms; // a throw's launch must come this soon
constexpr u32 UsableEvery = 6;       // CanUseGoods every this many updates
constexpr s32 PalmDummy = 3;         // L_Hand, the palm (FFX 173's dummy)
// L_Weapon's dummy: the centre of the hunter's fist, which the left arm's IK puts on your
// controller's grip (the arc's hover point; the palm is 3.1 cm toward the wrist and the palm).
constexpr s32 GripDummy = 340;

struct GoodsRowOut {
    s32 id{-1};
    u32 pad{0};
    u8* row{nullptr};
};
using UseGoodsFn = u8(PS4_SYSV_ABI*)(void* player, s32 goods, s32 dmy);
using GoodsRowFn = void(PS4_SYSV_ABI*)(GoodsRowOut* out, s32 goods);
using CanUseFn = u8(PS4_SYSV_ABI*)(s32 goods, void* chr, void* effects, s32 type, s32 right,
                                   s32 left, s32 multi);
using WeaponIdFn = s32(PS4_SYSV_ABI*)(void* chr, s32 slot);
using DmyPolyFn = s32(PS4_SYSV_ABI*)(void* chr, void* out, const s32* ids, u32 max);
using AddItemFn = s32(PS4_SYSV_ABI*)(void* egd, void* unused, const u32* handle, s32 count, s32 a5,
                                     s32 a6, s32 no_equip);
using PlaySoundFn = void(PS4_SYSV_ABI*)(u64* handle, const float* pos, s32 type, s32 id);
using EndFlightFn = void(PS4_SYSV_ABI*)(u8* p, s32 hit, const float* at);

PlaySoundFn g_play_sound = nullptr; // throw_sound (null: the signatures did not match)
EndFlightFn g_end_flight = nullptr; // molotov_burst (null: the signature did not match)
const u8* g_sound_table = nullptr;
u8* g_base = nullptr;
bool g_ready = false;     // the slots are read, select and use work
bool g_hud_bytes = false; // the D-pad window matched: the HUD bytes are known
bool g_can_use_ok = false;
bool g_launch_hook = false; // H2 installed
UseGoodsFn g_use_goods = nullptr;
// VR_QUICK_USEGOODS_TRACE (tests): the game's own UseGoods calls through the PlayerIns vtables
// (0x538F310 / 0x538FA80, slot +0x408): the item clips' TAE event 65 handler (0x1A25A66: `r13 =
// [vt+0x408]`, goods = [chr+0x1C0], the dummy = the event's first field, `call r13` 0x1A25B0C)
// and the vial's (0x1A25706). Our own uses call 0x18F9720 directly and are not seen here.
constexpr std::array<u64, 2> UseGoodsSlotRvas = {0x538F718, 0x538FE88};
u8 PS4_SYSV_ABI TraceUseGoods(void* player, s32 goods, s32 dmy) {
    const auto* const from = static_cast<const u8*>(__builtin_return_address(0));
    LOG_INFO(Core, "VR quick: GAME UseGoods({}, dmy {}) through vt+0x408 from +{:#x} on '{}'",
             goods, dmy, static_cast<u64>(from - g_base), Common::GetCurrentThreadName());
    return g_use_goods(player, goods, dmy);
}
// Installed at the first update (the slots are relocated by then, core/vr/hook_wrap.h).
void UseGoodsTraceOnce() {
    static bool done = false;
    if (done || g_base == nullptr || g_use_goods == nullptr) {
        return;
    }
    done = true;
    if (std::getenv("VR_QUICK_USEGOODS_TRACE") == nullptr) {
        return;
    }
    const u64 original = reinterpret_cast<u64>(g_use_goods);
    const u64 replacement = reinterpret_cast<u64>(&TraceUseGoods);
    for (const u64 rva : UseGoodsSlotRvas) {
        u64 found = 0;
        const bool ok = Hook::SwapVtableSlot(g_base + rva, original, replacement, found);
        LOG_INFO(Core, "VR quick: USEGOODS TRACE: vtable slot {:#x} {} (it held +{:#x})", rva,
                 ok ? "-> the trace" : "NOT swapped", found - reinterpret_cast<u64>(g_base));
    }
}
GoodsRowFn g_goods_row = nullptr;
CanUseFn g_can_use = nullptr;
DmyPolyFn g_dmy_poly = nullptr;
bool g_grip_missing_logged = false; // no grip dummy on the hunter: the palm hovers
std::unique_ptr<Xbyak::CodeGenerator> g_code;
int g_log = 0;          // VR_QUICK_LOG: 1 = STATE every 10 s, 2 = every 2 s and more
bool g_dry_run = false; // VR_QUICK_DRYRUN: every gate, no store and no UseGoods
bool g_markers = false; // VR_QUICK_MARKERS: axes at the palm dummy and the arc's pivot (in-frame)
std::string g_autotest; // VR_QUICK_AUTOTEST=select:<k>|use:<k>|throw:<k>[,...]
std::string g_test_give; // VR_QUICK_TEST_GIVE=<goods>:<count>[,...] (isolated test saves only)
AddItemFn g_add_item = nullptr;

// The held items' look (the render tick).
bool g_models_requested = false; // their game models were asked for
s32 g_held_logged = -1;          // VR_QUICK_LOG: the held item whose size was logged

s64 NowNs() {
    return Q::NowNs();
}

// ---- Goods rows (site A only) -------------------------------------------------------------------

struct GoodsInfo {
    s32 goods{-1};
    s32 icon{-1};
    s32 ffx{-1};
    s32 ref{-1}; // refId_default (a paper's buff SpEffect)
    s32 use_anim{-1}; // goodsUseAnim (quick_use_sounds: Q::UseSoundsOf)
    bool consume{true};
};
std::array<GoodsInfo, 32> g_goods_cache{};
u32 g_goods_next = 0;

// quick_hold_use: how every goods a quick slot can use is used, from the goods params (their
// goodsUseAnim, the consume flag) against the tables (Q::HoldOf, Q::MouthOf, Q::Classify), logged
// once when the params are first read: "hold" (a ring fills while the button is held: consume /
// rite), "mouth", "throw", "vial", "pull" (used at the pull, as before); a param that disagrees
// with the hold table is named.
void LogUseTable() {
    const auto& qs = Q::GetQuickSettings();
    std::string hold, rite, mouth, wipe, stab, thrown, pull, odd;
    for (const s32 goods : Q::QuickUsableGoods) {
        GoodsRowOut out{};
        g_goods_row(&out, goods);
        if (!Plausible(out.row, 1)) {
            odd += fmt::format(" {}(no row)", goods);
            continue;
        }
        const s32 anim = Read<u8>(out.row + RowUseAnim);
        const bool consume = (Read<u8>(out.row + RowConsumeByte) & 0x80) != 0;
        const std::string tag = fmt::format(" {}(a{}{})", goods, anim, consume ? "" : ",kept");
        const Q::HoldKind hk = Q::HoldOf(goods);
        if (hk != Q::HoldKind::None && !Q::HoldAnimAgrees(hk, anim)) {
            odd += tag;
        }
        // quick_use_crush / quick_use_shake / quick_use_stab against the params too.
        const Q::GestureKind gk = Q::GestureOf(goods);
        if ((gk != Q::GestureKind::None && !Q::GestureAnimAgrees(gk, anim, goods)) ||
            (Q::StabOf(goods) && !Q::StabAnimAgrees(anim))) {
            odd += tag + "(gesture)";
        }
        // cinematic_item: a canned use (its animation shown flat) against its clip too.
        if (!Q::CannedAnimAgrees(goods, anim)) {
            odd += tag + "(canned)";
        }
        const std::string gesture = gk == Q::GestureKind::Crush && qs.use_crush   ? "+crush"
                                    : gk == Q::GestureKind::Shake && qs.use_shake ? "+shake"
                                                                                  : "";
        if (goods == 1000) {
            continue; // the Blood Vial: the belt / X, never the ring
        }
        if (Q::MouthOf(goods) != Q::MouthKind::None) {
            mouth += tag;
        } else if (Q::WipeOf(goods) && qs.use_wipe) {
            wipe += tag;
        } else if (Q::Classify(goods).use == Q::UseKind::Throw) {
            thrown += tag;
        } else if (Q::StabOf(goods) && qs.use_stab && qs.instant) {
            stab += tag;
        } else if (hk == Q::HoldKind::Consume) {
            hold += tag + gesture;
        } else if (hk == Q::HoldKind::Rite) {
            rite += tag + gesture + (Q::CannedOf(goods) ? "+flat" : "");
        } else if (goods == Q::ChoirBell && qs.hold_use && qs.use_shake && qs.shake_choir_bell) {
            rite += tag + gesture;
        } else {
            pull += tag;
        }
    }
    LOG_INFO(Core,
             "VR quick: use table (quick_hold_use={}, {:.2f} s; quick_use_mouth={}; "
             "quick_use_wipe={}; quick_use_crush={} shake={} (choir bell {}) stab={}): hold "
             "(consume, the game's use at once):{} | hold (rite, the game's own act):{} | mouth:{} "
             "| wipe:{} | stab:{} | throw:{} | vial: 1000 | at the pull:{} | params disagreeing:{}",
             qs.hold_use ? 1 : 0, qs.hold_use_s, qs.use_mouth ? 1 : 0, qs.use_wipe ? 1 : 0,
             qs.use_crush ? 1 : 0, qs.use_shake ? 1 : 0, qs.shake_choir_bell ? 1 : 0,
             qs.use_stab ? 1 : 0, hold, rite, mouth, wipe.empty() ? " none" : wipe,
             stab.empty() ? " none" : stab, thrown, pull, odd.empty() ? " none" : odd);
}

GoodsInfo LookupGoods(s32 goods) {
    static bool table_logged = false;
    if (!table_logged) {
        table_logged = true;
        LogUseTable();
    }
    for (const GoodsInfo& g : g_goods_cache) {
        if (g.goods == goods) {
            return g;
        }
    }
    GoodsInfo info{};
    info.goods = goods;
    GoodsRowOut out{};
    g_goods_row(&out, goods);
    if (Plausible(out.row, 1)) {
        info.icon = Read<u16>(out.row + RowIcon);
        const s32 use_anim = Read<u8>(out.row + RowUseAnim);
        info.ffx = Q::UseHeldEffect(goods, use_anim)
                       ? Q::HeldFfx(use_anim, Read<s32>(out.row + RowVariation))
                       : -1;
        info.consume = (Read<u8>(out.row + RowConsumeByte) & 0x80) != 0;
        info.ref = Read<s32>(out.row + RowRefId);
        info.use_anim = use_anim;
        // quick_use_wipe's table (Q::WipeOf, by goods id) against the goods' own use clip (anim
        // 1: the blade wipe), logged when either says so.
        if (Q::WipeOf(goods) || Q::WipeOfAnim(use_anim)) {
            LOG_INFO(Core, "VR quick: goods {} uses clip {} ({}): wiped along the blade {} (its "
                     "buff SpEffect {})",
                     goods, use_anim, Q::WipeOfAnim(use_anim) ? "the blade wipe" : "not the wipe",
                     Q::WipeOf(goods) ? "yes" : "no", info.ref);
        }
        // quick_use_mouth's table (Q::MouthOf, by goods id) against the goods' own use clip
        // (anim 0: eaten, 10: drunk), for the items used without the animation (logged when
        // either says eat or drink: a mismatch shows as "clip 0 (eat): to the mouth none").
        const Q::UseKind use = Q::Classify(goods).use;
        if ((use == Q::UseKind::Instant || use == Q::UseKind::Bullet) &&
            (Q::MouthOf(goods) != Q::MouthKind::None ||
             Q::MouthKindOfAnim(use_anim) != Q::MouthKind::None)) {
            LOG_INFO(Core, "VR quick: goods {} uses clip {} ({}): to the mouth {}", goods, use_anim,
                     Q::MouthName(Q::MouthKindOfAnim(use_anim)), Q::MouthName(Q::MouthOf(goods)));
        }
    }
    g_goods_cache[g_goods_next++ % g_goods_cache.size()] = info;
    return info;
}

// VR_QUICK_PGD_DIFF=1 (tests): the player's game data (its first 0x600 bytes) as a use goes out,
// compared 0.6 s later: every 32-bit field it changed is logged (the coldbloods' blood echoes,
// Madman's Knowledge's insight), so a mock run shows what the use did to the hunter.
struct PgdDiff {
    bool on{false};
    bool read{false};
    s64 ns{0};
    s32 goods{-1};
    std::array<u8, 0x600> before{};
};
PgdDiff g_pgd{};

void PgdDiffArm(const u8* pgd, s32 goods) {
    if (!g_pgd.read) {
        g_pgd.read = true;
        g_pgd.on = std::getenv("VR_QUICK_PGD_DIFF") != nullptr;
    }
    if (!g_pgd.on || pgd == nullptr) {
        return;
    }
    std::memcpy(g_pgd.before.data(), pgd, g_pgd.before.size());
    g_pgd.ns = NowNs();
    g_pgd.goods = goods;
}

void PgdDiffTick(const u8* pgd) {
    if (g_pgd.ns == 0 || pgd == nullptr || NowNs() - g_pgd.ns < 600'000'000) {
        return;
    }
    g_pgd.ns = 0;
    std::string changed;
    int n = 0;
    for (u32 off = 0; off + 4 <= g_pgd.before.size(); off += 4) {
        s32 a = 0;
        std::memcpy(&a, g_pgd.before.data() + off, 4);
        const s32 b = Read<s32>(pgd + off);
        if (a != b && n++ < 16) {
            changed += fmt::format(" +0x{:X}: {} -> {} ({:+})", off, a, b,
                                   static_cast<s64>(b) - static_cast<s64>(a));
        }
    }
    LOG_INFO(Core, "VR quick: PGD after goods {} (0.6 s): {} field(s) changed:{}", g_pgd.goods, n,
             changed.empty() ? std::string{" none"} : changed);
}

// ---- The player's data ------------------------------------------------------------------------

u8* DataModule(const u8* player) {
    u8* const modules = Read<u8*>(player + OffsetChrModules);
    if (!Plausible(modules)) {
        return nullptr;
    }
    u8* const module = Read<u8*>(modules + 0x20);
    return Plausible(module) ? module : nullptr;
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

// The inventory entry of index `idx` (as 0x14D1EF0 / 0x14DEFF0 compute it), nullptr if out of
// range: {u32 handle (0 = gone), u32 item id (0x4xxxxxxx = goods), u32 quantity, u32 sort}.
u8* EntryOf(u8* egd, s32 idx) {
    if (idx < 0 || Read<u32>(egd + OffsetInvActive) == 0 ||
        static_cast<u32>(idx) > std::min(Read<u32>(egd + OffsetInvHighest), 4096u)) {
        return nullptr;
    }
    const u32 key_cap = Read<u32>(egd + OffsetInvKeyCap);
    if (static_cast<u32>(idx) < key_cap) {
        u8* const list = Read<u8*>(egd + OffsetInvKeyList);
        return Plausible(list) ? list + 16 * static_cast<size_t>(idx) : nullptr;
    }
    const u32 normal = static_cast<u32>(idx) - key_cap;
    u8* const list = Read<u8*>(egd + OffsetInvNormalList);
    if (!Plausible(list) || normal >= Read<u32>(egd + OffsetInvNormalCap)) {
        return nullptr;
    }
    return list + 16 * static_cast<size_t>(normal);
}

struct Pointers {
    u8* pgd{nullptr};
    u8* egd{nullptr};
    u8* ring{nullptr};
};

bool PointersOf(const u8* player, Pointers& p) {
    p.pgd = Read<u8*>(player + OffsetPlayerGameData);
    if (!Plausible(p.pgd)) {
        return false;
    }
    p.egd = p.pgd + OffsetEgd;
    p.ring = p.pgd + OffsetQuickRing;
    return Read<u8*>(p.ring + OffsetRingEgd) == p.egd && Read<u32>(p.egd + OffsetInvActive) != 0;
}

s32 SlotIndex(const Pointers& p, int k) {
    return Read<s32>(p.egd + OffsetSlotIndex + 8 * k);
}

// Goods id and quantity of slot k (goods -1 when empty or used up).
void ReadSlot(const Pointers& p, int k, s32& idx, s32& goods, s32& qty) {
    idx = SlotIndex(p, k);
    goods = -1;
    qty = 0;
    u8* const entry = EntryOf(p.egd, idx);
    if (entry == nullptr || Read<u32>(entry) == 0) {
        return;
    }
    const u32 id = Read<u32>(entry + 4);
    if ((id & 0xF0000000u) != 0x40000000u) {
        return; // not goods
    }
    goods = static_cast<s32>(id & 0x0FFFFFFFu);
    qty = static_cast<s32>(std::min(Read<u32>(entry + 8), 9999u));
}

bool InfiniteItems() {
    return std::memcmp(g_base + QuantityStoreRva, QuantityStoreSig.data(),
                       QuantityStoreSig.size()) != 0;
}

bool MenuMulti() {
    u8* const man = Read<u8*>(g_base + MenuManRva);
    return Plausible(man) && Read<s32>(man + 0x1C0) > 0;
}

// CanUseGoods exactly as IsItemUsable (0x18C3840) asks it, for an explicit goods id.
bool CanUse(u8* player, s32 goods) {
    if (!g_can_use_ok) {
        return true;
    }
    u8* const vtable = Read<u8*>(player);
    if (!Plausible(vtable)) {
        return false;
    }
    const auto weapon_id = reinterpret_cast<WeaponIdFn>(Read<u8*>(vtable + 0x388));
    if (reinterpret_cast<u8*>(weapon_id) != g_base + WeaponIdRva) {
        return true; // an unexpected vtable: no opinion (the use itself is still gated)
    }
    const s32 right = weapon_id(player, -1);
    u8* const style = Read<u8*>(player + OffsetArmStyle);
    const s32 left =
        Plausible(style, 4) && (Read<u32>(style) & ~1u) == 2 ? -1 : weapon_id(player, -2);
    return g_can_use(goods & 0x0FFFFFFF, player, Read<u8*>(player + OffsetSpEffects),
                     Read<s32>(player + OffsetType), right, left, MenuMulti() ? 1 : 0) != 0;
}

// ---- The launch override (H2) -------------------------------------------------------------------

struct Armed {
    u32 serial{0};
    s32 bullet{-1};
    u32 owner{0};
    s32 goods{-1};
    u8 throw_class{0};
    s64 issue_ns{0};   // UseGoods called (site A)
    s64 release_ns{0}; // the grip let go (the presenter's request)
    float scale{1.0f}; // world units per metre
    std::array<float, 3> v{};     // the throw (world units / s), before the soft cap
    std::array<float, 3> carry{}; // the hunter's own horizontal velocity, added after the cap
    bool target{false};           // locked on (or VR_THROW_FAKE_TARGET): the lock point ...
    bool fake_target{false};
    std::array<float, 3> target_p{}; // ... (world)
    // knife_assist (knives only): the game's lock-on candidates' lock points (world), locked on
    // or not.
    u8 candidates{0};
    std::array<std::array<float, 3>, 8> candidate_p{};
};
SeqLock<Armed> g_armed;
// LockTgtMan ([base+0x553E860]): its lock-on candidates, a list from +0x08 linked through +0x80,
// each with its world lock point at +0x00 (float4) and flags at +0x90 (plan_gun-aim.md 2.4); kept
// while unlocked too (the enemies in lock-on range; empty once they are dead).
constexpr u64 LockTgtManRva = 0x553E860;
constexpr u32 OffsetLockList = 0x08;
constexpr u32 OffsetLockNext = 0x80;
// Where the held item's model is (world), for the throw: published by the render tick, read by the
// launch hook (the game spawns the projectile at the palm dummy; it is moved to the model's origin,
// throw_from_hand).
struct HeldPlace {
    s64 ns{0};
    s32 goods{-1};
    std::array<float, 3> origin{}; // the model's origin (world): where the thrown one starts
    std::array<float, 3> centre{}; // the model's bounds' centre (world)
    float carried_m{0.0f}; // inframe_hand_sync: how far the hand sync carried it (metres)
};
SeqLock<HeldPlace> g_held_place;
std::atomic<u32> g_armed_serial{0};
std::atomic<u32> g_consumed_serial{0};
std::atomic<u32> g_launches{0};
std::atomic<bool> g_launch_thread_logged{false};
// Private BulletParam rows for thrown items (a projectile lives at most 4 s; 32 throws ago).
alignas(16) std::array<std::array<u8, 0xB0>, 32> g_rows{};
std::atomic<u32> g_row_next{0};
// The soft cap per bullet (ThrowCapOf of its own row), worked out at its first throw (the launch
// hook's thread only).
struct CapEntry {
    s32 bullet{-1};
    float cap{0.0f};   // m/s: the best arc's (a lob's), or the fixed cap of a dropped item
    float range{0.0f}; // the game's own throw (m)
    float reach{0.0f}; // m: a throw lands at most this far (ThrowCapAt); 0 = `cap` at every pitch
};
std::array<CapEntry, 16> g_caps{};
u32 g_caps_next = 0;
// throw_sound: a launch's whoosh, queued by the launch hook and played by the next render tick
// (Game:Main, the thread the launches and the gun's bang run on), outside the projectile's set-up.
struct Whoosh {
    u32 serial{0};
    s64 ns{0};
    std::array<float, 3> position{}; // where the thrown item started (world)
};
SeqLock<Whoosh> g_whoosh;
std::atomic<u32> g_whoosh_serial{0};
u32 g_whoosh_played = 0; // the render tick's
// quick_use_sounds (and quick_mouth_sound: a drink at the mouth): an item used at once (site A,
// or the Blood Vial's injection: QueueQuickUseSound), its use clip's sounds (Q::UseSoundsOf)
// played by the render ticks that follow (Game:Main, as the whoosh) where the held item was last
// drawn, else at the game camera (first person: the hunter's head).
struct UseCue {
    u32 serial{0};
    s64 ns{0};
    s32 goods{-1};
    s32 use_anim{-1};
};
SeqLock<UseCue> g_use_cue;
std::atomic<u32> g_use_cue_serial{0};
// The render tick's: the cue being played (its sounds go out at their offsets).
struct UsePlaying {
    u32 taken{0};
    s64 ns{0};
    s64 use_ns{0};
    s32 goods{-1};
    s32 use_anim{-1};
    Q::UseSounds sounds{};
    u8 next{0};
    bool at_hand{false};
    std::array<float, 3> position{};
};
UsePlaying g_use_playing{};

void QueueUseCue(s32 goods, s32 use_anim) {
    // (quick_music_box_crank: the music box's tune is its own cue, core/vr/music_box.h TuneCue.)
    const s32 count = use_anim == VR::MusicBox::TuneCue ? VR::MusicBox::Tune().count
                                                        : Q::UseSoundsForGoods(goods, use_anim).count;
    if (count == 0) {
        return;
    }
    g_use_cue.Store({g_use_cue_serial.load(std::memory_order_relaxed) + 1, Q::NowNs(), goods,
                     use_anim});
    g_use_cue_serial.fetch_add(1, std::memory_order_release);
}

// VR_THROW_TRACE=1 (tests): the flight of every throw of ours and of the game's own (the hunter's
// flying bullets), sampled by site A every update (`TRAJ`) until the projectile is gone, and the
// launch of its impact child (`IMPACT`: where it landed).
bool g_trace_on = false;
std::atomic<u32> g_player_handle{0};
struct Trace {
    u32 id{0};
    u8* p{nullptr};
    u32 handle{0};
    s32 bullet{-1};
    s32 goods{-1};
    s32 impact{-1}; // the row's HitBulletID
    s64 t0{0};
    bool vanilla{false};
    std::array<float, 3> p0{};
    std::array<float, 3> v0{};
};
SeqLock<Trace> g_trace;
std::atomic<u32> g_trace_ids{0};
std::atomic<s32> g_trace_impact{-1};
std::atomic<s64> g_trace_t0{0};
std::atomic<u32> g_trace_id{0};
// VR_THROW_FAKE_TARGET=<right>,<up>,<forward> (m from the hunter's feet in his facing; tests
// without an enemy): a lock point for the assist when the game has none.
bool g_fake_target = false;
std::array<float, 3> g_fake_target_m{};

// The hunter's flying throwable bullets (plan_throwables.md 3.2): the game's own throws to trace.
constexpr bool IsFlyingThrowable(s32 bullet) {
    switch (bullet) {
    case 100:
    case 105:
    case 111:
    case 115:
    case 120:
    case 121:
    case 150:
    case 160:
    case 170:
    case 180:
    case 300:
        return true;
    default:
        return false;
    }
}

Q::BulletFlight FlightOf(const u8* row) {
    Q::BulletFlight b{};
    b.life = Read<float>(row + RowLife);
    b.dist = Read<float>(row + RowDist);
    b.gravity_in = Read<float>(row + RowGravityIn);
    b.gravity_out = Read<float>(row + RowGravityOut);
    b.init_speed = Read<float>(row + RowInitSpeed);
    b.accel_in = Read<float>(row + RowAccelIn);
    b.accel_out = Read<float>(row + RowAccelOut);
    b.max_speed = Read<float>(row + RowMaxSpeed);
    b.min_speed = Read<float>(row + RowMinSpeed);
    b.accel_time = Read<float>(row + RowAccelTime);
    b.pitch_deg = static_cast<float>(Read<s8>(row + RowShootPitch));
    return b;
}

// The soft cap of a throw of `bullet` (m/s): from its own row, once (logged), or the class's.
CapEntry CapOf(s32 bullet, const u8* row, Q::ThrowClass cls) {
    for (const CapEntry& e : g_caps) {
        if (e.bullet == bullet) {
            return e;
        }
    }
    const auto& s = Q::GetQuickSettings();
    const Q::ThrowLimits limits = Q::LimitsOf(cls);
    CapEntry e{bullet, limits.max_speed, 0.0f, limits.range * s.throw_range_factor};
    if (row != nullptr) {
        const Q::BulletFlight b = FlightOf(row);
        const bool sane = std::isfinite(b.init_speed) && b.init_speed > 0.0f &&
                          b.init_speed < 200.0f && std::isfinite(b.gravity_in) &&
                          std::isfinite(b.gravity_out) && b.life > 0.0f && b.life < 60.0f;
        const float g = std::max(s.throw_gravity, 1.0f);
        if (sane) {
            e.cap = Q::ThrowCapOf(b, cls, s.throw_range_factor, ThrowHeightM, g, &e.range);
            e.reach = Q::ThrowReachOf(b, s.throw_range_factor, ThrowHeightM);
        }
        const float flat = Q::ThrowCapAt(e.reach, ThrowHeightM, g, 0.0f, s.throw_cap_flat_deg,
                                         e.cap);
        LOG_INFO(Core,
                 "VR quick: bullet {}: the game throws it {:.1f} m (from {:.1f} m: {:.1f} m/s, "
                 "{:+.0f} deg, gravity {:.2f} / {:.2f} after {:.2f} m, accel {:.2f} / {:.2f} "
                 "after {:.2f} s, speed {:.1f}..{:.1f}, life {:.2f} s) -> soft cap {:.1f} m/s "
                 "(its best arc: {:.1f} m; bending from {:.1f} m/s){}{}",
                 bullet, e.range, ThrowHeightM, b.init_speed, b.pitch_deg, b.gravity_in,
                 b.gravity_out, b.dist, b.accel_in, b.accel_out, b.accel_time, b.min_speed,
                 b.max_speed, b.life, e.cap, e.range * s.throw_range_factor,
                 e.cap * std::clamp(s.throw_cap_knee, 0.0f, 0.95f),
                 e.reach > 0.0f
                     ? fmt::format("; a flatter throw up to {:.1f} m/s (at +{:.0f} deg and "
                                   "below), every arc landing within {:.1f} m",
                                   flat, s.throw_cap_flat_deg, e.reach)
                     : std::string{"; dropped by the game: that cap at every pitch"},
                 sane ? "" : " (implausible row: the class's cap)");
        g_caps[g_caps_next++ % g_caps.size()] = e; // a missing row is tried again next time
    }
    return e;
}

// Hamilton (x, y, z, w) helpers for the projectile's orientation (forward = local +Z).
struct Qf {
    float x, y, z, w;
};
Qf QMul(const Qf& a, const Qf& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Qf ShortestArc(Q::V3 from, Q::V3 to) {
    const float d = Q::Dot(from, to);
    if (d < -0.9999f) {
        const Q::V3 axis =
            Q::Normalize(std::abs(from.x) < 0.9f ? Q::Cross(from, {1, 0, 0})
                                                  : Q::Cross(from, {0, 1, 0}));
        return {axis.x, axis.y, axis.z, 0.0f};
    }
    const Q::V3 c = Q::Cross(from, to);
    Qf q{c.x, c.y, c.z, 1.0f + d};
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}

std::array<float, 3> Read3(const u8* p) {
    return {Read<float>(p), Read<float>(p + 4), Read<float>(p + 8)};
}

// molotov_burst: the hunter's Molotovs (the plain, rope and delayed ones' flying bullets).
constexpr bool IsMolotov(s32 bullet) {
    return bullet == 100 || bullet == 105 || bullet == 111 || bullet == 115;
}

// molotov_burst: his Molotovs in flight (registered by the launch hook, any throw of his, ours or
// the game's own) and the gun's newest shot (the emitter); the render tick looks once per shot.
struct Flying {
    u8* p{nullptr};
    u32 handle{0};
    s32 bullet{-1};
    s64 ns{0};
    std::array<float, 3> from{}; // where it was launched
    bool tested{false};          // VR_BURST_TEST's shot taken
};
std::mutex g_flying_mutex;
std::array<Flying, 8> g_flying{}; // under g_flying_mutex
u32 g_flying_next = 0;
struct GunShot {
    u32 serial{0};
    s64 ns{0};
    std::array<float, 3> start{};
    std::array<float, 3> ray{};
};
SeqLock<GunShot> g_gun_shot;
std::atomic<u32> g_gun_shot_serial{0};
u32 g_gun_shot_taken = 0; // the render tick's
// The air burst being spawned (the render tick, around the end-of-flight call; the launch hook
// runs inside it, on the same thread): its bullet id, -1 none; and its radius as widened.
std::atomic<s32> g_burst_spawning{-1};
float g_burst_radius_from = -1.0f;
float g_burst_radius_to = -1.0f;
// VR_BURST_TEST=<ms> (tests, no gun needed): a shot from its launch point straight at each
// Molotov this long after its launch.
s64 g_burst_test_ms = 0;

void RegisterMolotov(u8* p, s32 bullet, s64 now) {
    std::scoped_lock lock{g_flying_mutex};
    g_flying[g_flying_next++ % g_flying.size()] = {p, Read<u32>(p + OffsetPHandle), bullet, now,
                                                   Read3(p + OffsetPPos)};
}

// The gun's shot for the next render tick (QuickItemsGunShot, VR_BURST_TEST).
void QuickItemsGunShotAt(const std::array<float, 3>& start, const std::array<float, 3>& ray) {
    if (g_end_flight == nullptr || !Q::GetQuickSettings().molotov_burst) {
        return;
    }
    g_gun_shot.Store({g_gun_shot_serial.load(std::memory_order_relaxed) + 1, NowNs(), start, ray});
    g_gun_shot_serial.fetch_add(1, std::memory_order_release);
}

// The render tick (Game:Main, where the projectiles update): a new shot bursts every Molotov in
// flight whose centre is within molotov_burst_deg of its ray (1-60 m ahead). The game's own end
// of flight spawns molotov_burst_bullet (the Cannon's explosion) right where it is; with
// molotov_burst_ground the bottle then falls straight down and its own landing spawns its own
// child (the fire) on the ground (bit 7 cleared), else its life is ended (bit 7 stays).
void MolotovBurstTick(s64 now) {
    if (g_burst_test_ms > 0) {
        std::array<float, 3> from{};
        std::array<float, 3> ray{};
        bool fake = false;
        {
            std::scoped_lock lock{g_flying_mutex};
            for (Flying& f : g_flying) {
                if (f.p == nullptr || f.tested || now - f.ns < g_burst_test_ms * Ms ||
                    !Plausible(f.p) || Read<u32>(f.p + OffsetPHandle) != f.handle) {
                    continue;
                }
                f.tested = true;
                const auto at = Read3(f.p + OffsetPPos);
                const Q::V3 d = Q::Normalize(Q::V3{at[0] - f.from[0], at[1] - f.from[1],
                                                   at[2] - f.from[2]},
                                             {0.0f, 0.0f, 1.0f});
                from = f.from;
                ray = {d.x, d.y, d.z};
                fake = true;
                LOG_INFO(Core, "VR quick: BURST TEST: a shot at bullet {} {} ms after its launch",
                         f.bullet, (now - f.ns) / Ms);
                break;
            }
        }
        if (fake) {
            QuickItemsGunShotAt(from, ray);
        }
    }
    if (g_gun_shot_serial.load(std::memory_order_acquire) == g_gun_shot_taken) {
        return;
    }
    GunShot shot{};
    if (!g_gun_shot.Load(shot) || shot.serial == g_gun_shot_taken) {
        return;
    }
    g_gun_shot_taken = shot.serial;
    const auto& s = Q::GetQuickSettings();
    if (!s.molotov_burst || g_end_flight == nullptr || now - shot.ns > 250 * Ms) {
        return;
    }
    const float cos_max = std::cos(s.molotov_burst_deg * 0.0174532925f);
    const Q::V3 start{shot.start[0], shot.start[1], shot.start[2]};
    const Q::V3 ray{shot.ray[0], shot.ray[1], shot.ray[2]};
    std::scoped_lock lock{g_flying_mutex};
    for (Flying& f : g_flying) {
        if (f.p == nullptr) {
            continue;
        }
        u8* const p = f.p;
        const bool alive = now - f.ns < 10'000 * Ms && Plausible(p) &&
                           Read<u32>(p + OffsetPHandle) == f.handle &&
                           (Read<u8>(p + OffsetPFlags) & 1) != 0 &&
                           (Read<u8>(p + OffsetPFlags) & FlagChildSpawned) == 0 &&
                           Read<s32>(p + OffsetPBullet) == f.bullet &&
                           Read<s32>(p + OffsetPMode) == 2;
        if (!alive) {
            f = {};
            continue;
        }
        const auto at = Read3(p + OffsetPPos);
        const Q::V3 to = Q::V3{at[0], at[1], at[2]} - start;
        const float dist = Q::Length(to);
        if (dist < 1.0f || dist > 60.0f) {
            continue;
        }
        const float cos_off = Q::Dot(to, ray) / dist;
        if (cos_off < cos_max) {
            continue;
        }
        // The row's hit filter: 0xFF never spawns the child on hit -1 (the life running out),
        // any other hit kind does.
        const u8* const row = Read<u8*>(p + OffsetPMode2 + OffsetModeRow);
        const u8 filter = Plausible(row, 4) ? Read<u8>(row + RowHitFilter) : 0;
        const s32 hit = filter == 0xFF ? 0 : -1;
        // molotov_burst_bullet: the air burst spawns that bullet (the Cannon's explosion) in place
        // of the Molotov's own child, which its landing then spawns below.
        const s32 child = Read<s32>(p + OffsetPChild);
        const s32 burst = s.molotov_burst_bullet > 0 ? s.molotov_burst_bullet : child;
        std::memcpy(p + OffsetPChild, &burst, sizeof(burst));
        alignas(16) std::array<float, 4> where{at[0], at[1], at[2], 1.0f};
        g_burst_radius_from = g_burst_radius_to = -1.0f;
        g_burst_spawning.store(burst, std::memory_order_relaxed);
        g_end_flight(p, hit, where.data());
        g_burst_spawning.store(-1, std::memory_order_relaxed);
        std::memcpy(p + OffsetPChild, &child, sizeof(child));
        const bool spawned = (Read<u8>(p + OffsetPFlags) & FlagChildSpawned) != 0;
        const bool drop = spawned && s.molotov_burst_ground;
        if (drop) {
            const u8 flags = Read<u8>(p + OffsetPFlags) & static_cast<u8>(~FlagChildSpawned);
            std::memcpy(p + OffsetPFlags, &flags, sizeof(flags));
            const std::array<float, 4> down{0.0f, -BurstDropMps, 0.0f, 0.0f};
            std::memcpy(p + OffsetPVel, down.data(), sizeof(down));
            std::memcpy(p + OffsetPPrevVel, down.data(), sizeof(down));
        } else {
            const float zero = 0.0f;
            std::memcpy(p + OffsetPLife, &zero, sizeof(zero));
        }
        LOG_INFO(Core,
                 "VR quick: BURST bullet {} at ({:.2f},{:.2f},{:.2f}), {:.1f} m out, {:.1f} deg off "
                 "the shot ({:.0f} ms after it; hit filter {:#x} -> {}): bullet {} (its own child {}) {}{}{} "
                 "[{}]",
                 f.bullet, at[0], at[1], at[2], dist,
                 std::acos(std::clamp(cos_off, -1.0f, 1.0f)) * 57.2957795f,
                 static_cast<float>(now - shot.ns) * 1e-6f, filter, hit, burst, child,
                 spawned ? "spawned in the air" : "NOT spawned",
                 g_burst_radius_to >= 0.0f
                     ? fmt::format(" (radius {:.2f} -> {:.2f} m)", g_burst_radius_from,
                                   g_burst_radius_to)
                     : std::string{},
                 drop ? ", the bottle drops to light the ground" : ", its flight ended",
                 Common::GetCurrentThreadName());
        f = {};
    }
}

// molotov_burst_radius_m (the launch hook, inside the air burst's spawn): the burst's own mode
// object gets a private copy of its BulletParam row with that hit radius (a growing one's maximum
// in proportion).
void WidenBurst(u8* p, s32 bullet) {
    const float radius_m = Q::GetQuickSettings().molotov_burst_radius_m;
    const s32 mode = Read<s32>(p + OffsetPMode);
    if (radius_m <= 0.0f || mode < 1 || mode > 3) {
        return;
    }
    u8* const object = p + OffsetPModes[static_cast<size_t>(mode - 1)];
    u8* const row = Read<u8*>(object + OffsetModeRow);
    if (!Plausible(row, 4) || Read<s32>(object + OffsetModeId) != bullet ||
        !(Read<float>(row + RowHitRadius) > 0.0f)) {
        return;
    }
    const float scale = radius_m / Read<float>(row + RowHitRadius);
    u8* const copy = g_rows[g_row_next.fetch_add(1) % g_rows.size()].data();
    std::memcpy(copy, row, BulletRowSize);
    for (const u32 offset : {RowHitRadius, RowHitRadiusMax}) {
        const float radius = Read<float>(row + offset);
        if (radius > 0.0f) {
            const float wider = radius * scale;
            std::memcpy(copy + offset, &wider, sizeof(wider));
        }
    }
    std::memcpy(object + OffsetModeRow, &copy, sizeof(copy));
    g_burst_radius_from = Read<float>(row + RowHitRadius);
    g_burst_radius_to = Read<float>(copy + RowHitRadius);
}

// VR_THROW_TRACE: follow projectile `p` (its impact child from `row`).
void StartTrace(u8* p, const u8* row, s32 bullet, s32 goods, bool vanilla, s64 now) {
    Trace t{};
    t.id = g_trace_ids.fetch_add(1) + 1;
    t.p = p;
    t.handle = Read<u32>(p + OffsetPHandle);
    t.bullet = bullet;
    t.goods = goods;
    t.impact = row != nullptr ? Read<s32>(row + RowHitBullet) : -1;
    t.t0 = now;
    t.vanilla = vanilla;
    t.p0 = Read3(p + OffsetPPos);
    t.v0 = Read3(p + OffsetPVel);
    g_trace_impact.store(t.impact, std::memory_order_relaxed);
    g_trace_t0.store(now, std::memory_order_relaxed);
    g_trace_id.store(t.id, std::memory_order_relaxed);
    g_trace.Store(t);
    LOG_INFO(Core,
             "VR quick: TRAJ #{} start {} bullet {} at ({:.3f},{:.3f},{:.3f}) v ({:.3f},{:.3f},"
             "{:.3f}) {:.2f} m/s, impact child {}",
             t.id, vanilla ? "GAME'S OWN THROW" : "throw", bullet, t.p0[0], t.p0[1], t.p0[2],
             t.v0[0], t.v0[1], t.v0[2], std::hypot(t.v0[0], t.v0[1], t.v0[2]), t.impact);
}

void PS4_SYSV_ABI LaunchHook(u8* p, u8* /*saved_ymm*/) {
    GunLaunchTail(p); // VR_GUN_LOG: the gun shot's readback (core/vr/bloodborne_gun.h)
    if (g_end_flight != nullptr && p != nullptr && IsMolotov(Read<s32>(p + OffsetPBullet)) &&
        Read<s32>(p + OffsetPMode) == 2 &&
        Read<u32>(p + OffsetPOwner) == g_player_handle.load(std::memory_order_relaxed)) {
        RegisterMolotov(p, Read<s32>(p + OffsetPBullet), NowNs()); // molotov_burst
    }
    if (const s32 burst = g_burst_spawning.load(std::memory_order_relaxed);
        burst >= 0 && p != nullptr && Read<s32>(p + OffsetPBullet) == burst) {
        WidenBurst(p, burst); // molotov_burst_radius_m
    }
    const u32 armed_serial = g_armed_serial.load(std::memory_order_acquire);
    const bool ours_pending = armed_serial != g_consumed_serial.load(std::memory_order_relaxed);
    if ((!ours_pending && !g_trace_on) || p == nullptr) {
        return; // the usual case: nothing of ours in flight
    }
    const s32 bullet = Read<s32>(p + OffsetPBullet);
    const u32 owner = Read<u32>(p + OffsetPOwner);
    Armed a{};
    const bool match = ours_pending && g_armed.Load(a) && a.serial == armed_serial &&
                       bullet == a.bullet && owner == a.owner;
    if (!match) {
        if (g_trace_on && owner != 0 && owner == g_player_handle.load(std::memory_order_relaxed)) {
            const s64 now = NowNs();
            if (bullet == g_trace_impact.load(std::memory_order_relaxed) && bullet >= 0) {
                const auto at = Read3(p + OffsetPPos);
                LOG_INFO(Core,
                         "VR quick: IMPACT #{} bullet {} at ({:.3f},{:.3f},{:.3f}) {:.3f} s after "
                         "the launch",
                         g_trace_id.load(std::memory_order_relaxed), bullet, at[0], at[1], at[2],
                         static_cast<float>(now - g_trace_t0.load(std::memory_order_relaxed)) *
                             1e-9f);
                g_trace_impact.store(-1, std::memory_order_relaxed);
            } else if (IsFlyingThrowable(bullet) && Read<s32>(p + OffsetPMode) == 2) {
                u8* const row = Read<u8*>(p + OffsetPMode2 + OffsetModeRow);
                StartTrace(p, Plausible(row, 4) ? row : nullptr, bullet, -1, true, now);
            }
        }
        return;
    }
    const s64 now = NowNs();
    u32 expected = g_consumed_serial.load(std::memory_order_relaxed);
    if (now - a.issue_ns > ArmedTtlNs || expected == armed_serial ||
        !g_consumed_serial.compare_exchange_strong(expected, armed_serial)) {
        return;
    }
    if (!g_launch_thread_logged.exchange(true)) {
        LOG_INFO(Core, "VR quick: projectile launches run on thread '{}'",
                 Common::GetCurrentThreadName());
    }
    const auto& s = Q::GetQuickSettings();
    if (Read<s32>(p + OffsetPMode) != 2 || Read<u8*>(p + OffsetPMode2) != g_base + Mode2VtableRva) {
        LOG_WARNING(Core, "VR quick: launch of bullet {} skipped (mode {} / mode object {}): the "
                          "game's own direction",
                    a.bullet, Read<s32>(p + OffsetPMode), fmt::ptr(Read<u8*>(p + OffsetPMode2)));
        return;
    }
    const Q::V3 v0{Read<float>(p + OffsetPVel), Read<float>(p + OffsetPVel + 4),
                   Read<float>(p + OffsetPVel + 8)};
    const auto cls = static_cast<Q::ThrowClass>(a.throw_class);
    u8* const mode = p + OffsetPMode2;
    u8* const row = Read<u8*>(mode + OffsetModeRow);
    const bool row_ok = Plausible(row, 4) && Read<s32>(mode + OffsetModeId) == a.bullet;
    // The throw, bent softly toward the speed whose arc at its pitch lands at the item's reach
    // (throw_range_factor x its own range, from its BulletParam row), then the hunter's own
    // motion on top.
    const CapEntry cap = CapOf(a.bullet, row_ok ? row : nullptr, cls);
    const Q::V3 thrown{a.v[0], a.v[1], a.v[2]};
    const float cap_mps = Q::ThrowCapAt(cap.reach, ThrowHeightM, std::max(s.throw_gravity, 1.0f),
                                        Q::PitchOf(thrown), s.throw_cap_flat_deg, cap.cap);
    const Q::V3 capped = Q::SoftCap(thrown, cap_mps * a.scale, s.throw_cap_knee);
    Q::V3 v = capped + Q::V3{a.carry[0], a.carry[1], a.carry[2]};
    bool copied = false;
    if (row_ok) {
        if (s.throw_physics) {
            u8* const copy = g_rows[g_row_next.fetch_add(1) % g_rows.size()].data();
            std::memcpy(copy, row, BulletRowSize);
            const auto put = [copy](u32 offset, float value) {
                std::memcpy(copy + offset, &value, sizeof(value));
            };
            put(RowGravityIn, s.throw_gravity * a.scale);
            put(RowGravityOut, s.throw_gravity * a.scale);
            put(RowAccelIn, 0.0f);
            put(RowAccelOut, 0.0f);
            put(RowAccelTime, 0.0f);
            put(RowMinSpeed, 0.3f);
            put(RowMaxSpeed, std::max(Read<float>(row + RowMaxSpeed), 60.0f));
            u8* const copy_ptr = copy;
            std::memcpy(mode + OffsetModeRow, &copy_ptr, sizeof(copy_ptr));
            copied = true;
        } else {
            // The game's own clamps would apply from the next frame anyway.
            const float lo = Read<float>(row + RowMinSpeed);
            const float hi = Read<float>(row + RowMaxSpeed);
            const float speed = Q::Length(v);
            if (speed > 1e-3f && hi >= lo && lo >= 0.0f) {
                v = v * (std::clamp(speed, lo, hi) / speed);
            }
        }
    }
    // The thrown item starts where the held one was: the game spawns it at the palm dummy, the hand
    // showed the game's model around it (a Molotov by its neck, the bottle below the fist); the
    // projectile goes to that model's origin (at most 25 cm away, else it stays at the palm). Only
    // from a spawn still at the palm: the game pulls the spawn back to a wall it finds between the
    // hunter's body and his palm, and then the held model may be in or beyond that wall (the
    // projectile stays where the game put it, as the gun's start does).
    std::array<float, 3> position = Read3(p + OffsetPPos);
    HeldPlace held{};
    if (g_held_place.Load(held) && held.goods == a.goods && now - held.ns < 150 * Ms) {
        const float moved = std::hypot(position[0] - held.origin[0],
                                       position[1] - held.origin[1], position[2] - held.origin[2]);
        const float before = std::hypot(position[0] - held.centre[0],
                                        position[1] - held.centre[1], position[2] - held.centre[2]);
        float palm_off = -1.0f;
        u8* const world = Read<u8*>(g_base + WorldChrManRva);
        u8* const chr = Plausible(world) ? Read<u8*>(world + OffsetMainPlayer) : nullptr;
        if (g_dmy_poly != nullptr && Plausible(chr)) {
            alignas(16) std::array<float, 8 * 12> m{};
            const s32 id = PalmDummy;
            if (g_dmy_poly(chr, m.data(), &id, 1) >= 1) {
                palm_off = std::hypot(position[0] - m[3], position[1] - m[7], position[2] - m[11]);
            }
        }
        const bool at_palm = palm_off >= 0.0f && palm_off <= 0.015f * a.scale;
        const bool move = s.throw_from_hand && moved < 0.25f * a.scale && at_palm;
        if (move) {
            std::memcpy(p + OffsetPPos, held.origin.data(), sizeof(held.origin));
            position = held.origin;
        }
        LOG_INFO(Core,
                 "VR quick: LAUNCH from the held model: the palm spawn was {:.1f} cm from its "
                 "centre, {:.1f} cm from its origin (as drawn: the hand sync carried it {:.1f} "
                 "cm){}",
                 100.0f * before, 100.0f * moved, 100.0f * held.carried_m,
                 move               ? " -> moved to its origin"
                 : palm_off < 0.0f  ? " (kept: no palm dummy)"
                 : !at_palm         ? fmt::format(" (kept: the game pulled the spawn {:.1f} cm back "
                                                  "from the palm, a wall)",
                                                  100.0f * palm_off)
                                    : std::string{" (kept at the palm)"});
    }
    // Locked on: the very gentle yaw assist (direction only, never range), from where it starts;
    // not locked on, the same toward the candidate nearest the throw's heading
    // (throw_assist_unlocked; not knives). Knives (knife_assist): the strong one, yaw and drop,
    // toward the lock-on target or the candidate nearest the throw's line, locked on or not.
    float turn_deg = 0.0f;
    float off_deg = 0.0f;
    std::string knife_text; // the knife's or the unlocked assist's log text
    if (cls == Q::ThrowClass::Knife && s.knife_assist > 0.0f) {
        std::array<Q::V3, 9> points{};
        int count = 0;
        int lock = -1;
        if (a.target) {
            lock = count;
            points[static_cast<size_t>(count++)] = {a.target_p[0], a.target_p[1], a.target_p[2]};
        }
        for (u32 i = 0; i < a.candidates && i < a.candidate_p.size(); ++i) {
            const auto& c = a.candidate_p[i];
            points[static_cast<size_t>(count++)] = {c[0], c[1], c[2]};
        }
        // The flight's gravity (the launch's units): ours with throw_physics, else the row's.
        const float g = copied ? s.throw_gravity * a.scale
                        : row_ok ? Read<float>(row + RowGravityIn)
                                 : s.throw_gravity * a.scale;
        const Q::V3 from{position[0], position[1], position[2]};
        const Q::V3 before = v;
        Q::KnifeOut k{};
        v = Q::KnifeAssist(v, from, points.data(), count, lock, g, s.knife_assist,
                           s.knife_assist_deg, 40.0f * a.scale, k);
        knife_text =
            k.target < 0
                ? fmt::format("knife assist: no target within {:.0f} deg ({} candidate(s){})",
                              s.knife_assist_deg * (4.0f / 3.0f), a.candidates,
                              a.target ? ", locked on" : "")
                : fmt::format("knife assist: {} target {:.1f} m away, {:.1f} deg off the arc onto "
                              "it -> {:.1f} deg (weight {:.2f}, turned {:.1f} deg{}; {} "
                              "candidate(s))",
                              k.target == lock ? "lock-on" : "nearest", k.distance / a.scale,
                              k.off_deg, k.after_deg, k.weight,
                              std::acos(std::clamp(Q::Dot(Q::Normalize(before), Q::Normalize(v)),
                                                   -1.0f, 1.0f)) *
                                  57.2957795f,
                              k.reachable ? "" : ", out of reach: its heading only", a.candidates);
    } else if (a.target) {
        const float cone =
            cls == Q::ThrowClass::Knife ? s.throw_assist_knife_deg : s.throw_assist_deg;
        const Q::V3 to{a.target_p[0] - position[0], a.target_p[1] - position[1],
                       a.target_p[2] - position[2]};
        v = Q::YawAssist(v, to, s.throw_assist, cone, turn_deg, off_deg);
    } else if (cls != Q::ThrowClass::Knife && s.throw_assist_unlocked > 0.0f) {
        // Not locked on (every throw but the knives): the same yaw-only turn toward the lock-on
        // candidate whose heading is nearest the throw's (throw_assist_unlocked).
        std::array<Q::V3, 8> points{};
        int count = 0;
        for (u32 i = 0; i < a.candidates && i < a.candidate_p.size(); ++i) {
            const auto& c = a.candidate_p[i];
            points[static_cast<size_t>(count++)] = {c[0], c[1], c[2]};
        }
        const Q::V3 from{position[0], position[1], position[2]};
        float yaw_off = 0.0f;
        const int pick = Q::PickYawTarget(v, from, points.data(), count, s.throw_assist_deg,
                                          40.0f * a.scale, yaw_off);
        if (pick >= 0) {
            const Q::V3 to = points[static_cast<size_t>(pick)] - from;
            v = Q::YawAssist(v, to, s.throw_assist_unlocked, s.throw_assist_deg, turn_deg, off_deg);
        }
        knife_text =
            pick < 0 ? fmt::format("no lock-on; unlocked assist: no candidate within {:.0f} deg "
                                   "({} candidate(s))",
                                   s.throw_assist_deg, a.candidates)
                     : fmt::format("no lock-on; unlocked assist: nearest candidate {:.1f} m away, "
                                   "{:.1f} deg off -> turned {:+.2f} deg ({:.2f}; {} "
                                   "candidate(s))",
                                   Q::Length(points[static_cast<size_t>(pick)] - from) / a.scale,
                                   off_deg, turn_deg, s.throw_assist_unlocked, a.candidates);
    }
    const float game_speed = Q::Length(v0);
    const Q::V3 d0 = Q::Normalize(v0, {0.0f, 0.0f, 1.0f});
    const Q::V3 d1 = Q::Normalize(v, d0);
    const Qf q0{Read<float>(p + OffsetPQuat), Read<float>(p + OffsetPQuat + 4),
                Read<float>(p + OffsetPQuat + 8), Read<float>(p + OffsetPQuat + 12)};
    const Qf q1 = QMul(ShortestArc(d0, d1), q0);
    const std::array<float, 4> vel{v.x, v.y, v.z, 0.0f};
    std::memcpy(p + OffsetPVel, vel.data(), sizeof(vel));
    std::memcpy(p + OffsetPPrevVel, vel.data(), sizeof(vel));
    std::memcpy(p + OffsetPQuat, &q1, sizeof(q1));
    Q::Launch launch{};
    launch.count = g_launches.fetch_add(1) + 1;
    launch.steady_ns = now;
    launch.bullet = a.bullet;
    launch.goods = a.goods;
    launch.position = position;
    launch.velocity = {v.x, v.y, v.z};
    launch.game_speed = game_speed;
    launch.issue_to_launch_ms = static_cast<float>(now - a.issue_ns) * 1e-6f;
    Q::PublishLaunch(launch);
    if (s.throw_sound && g_play_sound != nullptr && cls != Q::ThrowClass::Coin &&
        cls != Q::ThrowClass::None) {
        g_whoosh.Store({g_whoosh_serial.fetch_add(1) + 1, now, position});
    }
    const float horizontal = std::hypot(v.x, v.z);
    const float carried = std::hypot(a.carry[0], a.carry[2]);
    LOG_INFO(Core,
             "VR quick: LAUNCH bullet {} (goods {}) at ({:.3f},{:.3f},{:.3f}): game {:.1f} m/s -> "
             "({:.3f},{:.3f},{:.3f}) {:.2f} m/s, pitch {:.1f} deg, {} | throw {:.2f} m/s, soft cap "
             "{:.1f} m/s (the game throws it {:.1f} m){} | carry {:.2f} m/s | {} | release -> "
             "launch {:.0f} ms ({:.0f} ms after the use)",
             a.bullet, a.goods, position[0], position[1], position[2], game_speed, v.x, v.y, v.z,
             Q::Length(v), std::atan2(v.y, horizontal) * 57.2957795f,
             copied ? fmt::format("real gravity {:.2f}", s.throw_gravity)
                    : std::string{"the game's flight"},
             Q::Length(thrown) / a.scale, cap_mps, cap.range,
             Q::Length(capped) + 1e-3f < Q::Length(thrown)
                 ? fmt::format(" -> {:.2f}", Q::Length(capped) / a.scale)
                 : std::string{},
             carried / a.scale,
             !knife_text.empty() ? fmt::format("{}{}", a.fake_target ? "FAKE lock-on, " : "",
                                               knife_text)
             : !a.target         ? std::string{"no lock-on"}
                         : fmt::format("{}lock-on: {:.1f} deg off -> turned {:+.2f} deg",
                                       a.fake_target ? "FAKE " : "", off_deg, turn_deg),
             a.release_ns > 0 ? static_cast<float>(now - a.release_ns) * 1e-6f : -1.0f,
             launch.issue_to_launch_ms);
    if (g_trace_on) {
        StartTrace(p, row_ok ? row : nullptr, a.bullet, a.goods, false, now);
    }
}

// throw_sound (the render tick, Game:Main): the game's own throw whoosh (SoundType 1, c751: what
// its throw clips play as the arm comes through) where the item the launch hook just threw
// started; a queued one older than 150 ms is dropped (a stall: it would trail the throw).
void PlayWhoosh(s64 now) {
    if (g_whoosh_serial.load(std::memory_order_acquire) == g_whoosh_played) {
        return;
    }
    Whoosh w{};
    if (!g_whoosh.Load(w) || w.serial == g_whoosh_played) {
        return;
    }
    g_whoosh_played = w.serial;
    if (g_play_sound == nullptr || now - w.ns > 150 * Ms) {
        return;
    }
    alignas(16) std::array<float, 4> pos{w.position[0], w.position[1], w.position[2], 1.0f};
    u64 handle = 0;
    const s32 type = g_sound_table != nullptr && Read<u8>(g_sound_table + 3) != 0 ? 0xA : 1;
    g_play_sound(&handle, pos.data(), type, ThrowWhooshSound);
    static u32 logged = 0;
    if (logged < 3 || g_log > 1) {
        ++logged;
        LOG_INFO(Core,
                 "VR quick: throw whoosh {} (type {}) at ({:.3f},{:.3f},{:.3f}) {:.0f} ms after "
                 "the launch, h={:#x}",
                 ThrowWhooshSound, type, pos[0], pos[1], pos[2],
                 static_cast<float>(now - w.ns) * 1e-6f, handle);
    }
}

// quick_use_sounds (the render tick, Game:Main): the queued use's sounds (SoundType 1), each at
// its offset after the first, where the held item was drawn in the last 300 ms (its model's
// centre) or else at the game camera `cam`; a cue queued more than 300 ms ago is dropped (a
// stall: it would trail the use).
void PlayUseSounds(s64 now, const float* cam) {
    auto& p = g_use_playing;
    if (g_use_cue_serial.load(std::memory_order_acquire) != p.taken) {
        UseCue c{};
        if (g_use_cue.Load(c) && c.serial != p.taken) {
            p.taken = c.serial;
            if (now - c.ns <= 300 * Ms) {
                p.ns = now;
                p.use_ns = c.ns;
                p.goods = c.goods;
                p.use_anim = c.use_anim;
                // (quick_music_box_crank: the music box's tune, core/vr/music_box.h TuneCue.)
                p.sounds = c.use_anim == VR::MusicBox::TuneCue ? VR::MusicBox::Tune()
                                                               : Q::UseSoundsForGoods(c.goods, c.use_anim);
                p.next = 0;
                HeldPlace held{};
                p.at_hand = g_held_place.Load(held) && held.goods == c.goods &&
                            now - held.ns < 300 * Ms;
                p.position = p.at_hand ? held.centre
                                       : std::array<float, 3>{cam[0], cam[1], cam[2]};
            } else {
                p.next = p.sounds.count; // nothing left to play
            }
        }
    }
    while (p.next < p.sounds.count &&
           now - p.ns >= static_cast<s64>(p.sounds.sound[p.next].after_ms) * Ms) {
        const Q::UseSound sound = p.sounds.sound[p.next++];
        if (g_play_sound == nullptr) {
            continue;
        }
        alignas(16) std::array<float, 4> pos{p.position[0], p.position[1], p.position[2], 1.0f};
        u64 handle = 0;
        const s32 type = g_sound_table != nullptr && Read<u8>(g_sound_table + 3) != 0 ? 0xA : 1;
        g_play_sound(&handle, pos.data(), type, sound.id);
        LOG_INFO(Core,
                 "VR quick: use sound c{} (type {}) for goods {} (anim {}, clip {}, {}) at the {} "
                 "({:.3f},{:.3f},{:.3f}) {:.0f} ms after the use, h={:#x}",
                 sound.id, type, p.goods, p.use_anim, p.sounds.clip,
                 p.sounds.own ? "its own" : "the closest", p.at_hand ? "hand" : "camera", pos[0],
                 pos[1], pos[2], static_cast<float>(now - p.use_ns) * 1e-6f, handle);
    }
}

// Site A: VR_THROW_TRACE's samples of the projectile being followed, every update until it is
// gone (its handle changed or it is no longer alive), at most 6 s.
struct TraceSampler {
    u32 id{0};
    Trace t{};
    u32 samples{0};
    bool done{true};
    std::array<float, 3> last{};
    float last_t{0.0f};
};
TraceSampler g_sampler{};

void TraceTick(u8* player) {
    if (!g_trace_on) {
        return;
    }
    // Every second: where the hunter is and the lock-on candidates (LockTgtMan [0x553E860]: a list
    // from +0x08 linked through +0x80, each with its world lock point at +0x00 and flags at
    // +0x90; plan_gun-aim.md 2.4) - test navigation toward an enemy.
    static s64 last_scan = 0;
    if (NowNs() - last_scan >= 1000 * Ms) {
        last_scan = NowNs();
        u8* const ctrl = Read<u8*>(player + OffsetChrCtrl);
        std::array<float, 3> me{};
        if (Plausible(ctrl)) {
            const u8* w = ctrl + OffsetChrFrame;
            me = {Read<float>(w + 0x0C), Read<float>(w + 0x1C), Read<float>(w + 0x2C)};
            std::string list;
            u8* const mgr = Read<u8*>(g_base + 0x553E860);
            u8* e = Plausible(mgr) ? Read<u8*>(mgr + 0x08) : nullptr;
            for (int i = 0; i < 12 && Plausible(e, 16); ++i) {
                const auto at = Read3(e);
                list += fmt::format(" [({:.1f},{:.1f},{:.1f}) {:.1f} m f={:#x}]", at[0], at[1],
                                    at[2], std::hypot(at[0] - me[0], at[2] - me[2]),
                                    Read<u32>(e + 0x90));
                e = Read<u8*>(e + 0x80);
            }
            VR::Body::Anchor anchor{};
            const float view = VR::Body::LatestAnchor(anchor) ? anchor.base_yaw : 0.0f;
            LOG_INFO(Core, "VR quick: SCAN hunter ({:.2f},{:.2f},{:.2f}) facing ({:.2f},{:.2f}) "
                           "view ({:.2f},{:.2f}) | lock candidates:{}",
                     me[0], me[1], me[2], -Read<float>(w + 0x08), -Read<float>(w + 0x28),
                     std::sin(view), std::cos(view), list.empty() ? std::string{" none"} : list);
        }
    }
    // The lock-on (the camera's flag and the game's chosen lock point), on every change.
    static bool was_locked = false;
    const bool locked = VR::IsLockedOn() && Read<u8>(player + OffsetLockChosen) != 0;
    if (locked != was_locked) {
        was_locked = locked;
        const auto at = Read3(player + OffsetLockPoint);
        LOG_INFO(Core, "VR quick: LOCK {} ({:.3f},{:.3f},{:.3f})", locked ? "on" : "off", at[0],
                 at[1], at[2]);
    }
    auto& sm = g_sampler;
    const u32 id = g_trace_id.load(std::memory_order_relaxed);
    if (id != sm.id) {
        Trace t{};
        if (!g_trace.Load(t) || t.id != id) {
            return;
        }
        sm = {};
        sm.id = id;
        sm.t = t;
        sm.done = false;
        // Where the hunter stands (W's translation: his feet) and faces.
        u8* const ctrl = Read<u8*>(player + OffsetChrCtrl);
        if (Plausible(ctrl)) {
            const u8* w = ctrl + OffsetChrFrame;
            VR::Body::Anchor anchor{};
            const float view = VR::Body::LatestAnchor(anchor) ? anchor.base_yaw : 0.0f;
            LOG_INFO(Core,
                     "VR quick: TRAJ #{} hunter at ({:.3f},{:.3f},{:.3f}) facing ({:.3f},{:.3f}) "
                     "view ({:.3f},{:.3f})",
                     id, Read<float>(w + 0x0C), Read<float>(w + 0x1C), Read<float>(w + 0x2C),
                     -Read<float>(w + 0x08), -Read<float>(w + 0x28), std::sin(view),
                     std::cos(view));
        }
    }
    if (sm.done) {
        return;
    }
    const s64 now = NowNs();
    const float t = static_cast<float>(now - sm.t.t0) * 1e-9f;
    u8* const p = sm.t.p;
    const bool alive = Plausible(p) && Read<u32>(p + OffsetPHandle) == sm.t.handle &&
                       (Read<u8>(p + OffsetPFlags) & 1) != 0 && t < 6.0f;
    if (!alive) {
        sm.done = true;
        LOG_INFO(Core, "VR quick: TRAJ #{} end after {} samples: last ({:.3f},{:.3f},{:.3f}) at "
                       "{:.3f} s",
                 sm.id, sm.samples, sm.last[0], sm.last[1], sm.last[2], sm.last_t);
        return;
    }
    const auto at = Read3(p + OffsetPPos);
    const auto v = Read3(p + OffsetPVel);
    if (sm.samples > 0 && at == sm.last) {
        return; // not moved since the last sample (no projectile update in between)
    }
    ++sm.samples;
    sm.last = at;
    sm.last_t = t;
    LOG_INFO(Core, "VR quick: TRAJ #{} t={:.4f} p=({:.4f},{:.4f},{:.4f}) v=({:.3f},{:.3f},{:.3f})",
             sm.id, t, at[0], at[1], at[2], v[0], v[1], v[2]);
}

// ---- Site A ------------------------------------------------------------------------------------

struct SiteState {
    u64 updates{0};
    u32 taken_serial{0};
    u32 usable_tick{0};
    std::array<s32, Q::SlotCount> usable_goods{-1, -1, -1, -1, -1, -1};
    std::array<bool, Q::SlotCount> usable{};
    Q::Snapshot last{};
    bool first_valid_logged{false};
    s64 first_valid_ns{0};
    s64 last_state_log_ns{0};
    // Autotest.
    std::size_t autotest_step{0};
    s64 autotest_next_ns{0};
    u32 autotest_serial{0x80000000u};
    // Counters (VR_QUICK_LOG).
    u32 selects{0};
    u32 uses{0};
    u32 throws{0};
    u32 denies{0};
    // quick_use_wipe: a paper applied by a wipe; its buff (the goods' refId SpEffect) is looked
    // for on the hunter until it shows (the log) or 1.5 s pass.
    s32 wipe_ref{-1};
    s32 wipe_goods{-1};
    s64 wipe_ns{0};
};
SiteState g_site{};

// The player-update hook's busy guard owns this context. Renderer visuals use their own module.
VR::Interactions::Module* g_native_quick_module{};
bool g_native_quick_fault{};
VR::Interactions::NativeQuickRuntime g_native_quick;
void InitNativeQuickDll() {
    if (!VR::Interactions::DllRequested() || g_native_quick_module)
        return;
    g_native_quick_module = new VR::Interactions::Module();
    g_native_quick_module->EnableFaultBroadcast();
    g_native_quick.Bind(*g_native_quick_module, g_native_quick_fault);
    if (!g_native_quick_module->LoadBesideExecutable()) {
        g_native_quick_fault = true;
        g_native_quick_module->ReportFault();
        LOG_ERROR(Core, "VR quick native: interaction DLL unavailable: {}",
                  g_native_quick_module->Diagnostic());
    }
}

// Host copies the latest render anchor. Duplicate/freshness/history decisions are DLL-owned.
void SampleMotion() {
    VR::Body::Anchor anchor{};
    PiNativeQuickInput p{};
    p.operation = PI_NQ_SAMPLE;
    p.now_ns = NowNs();
    p.anchor_available = VR::Body::LatestAnchor(anchor);
    p.anchor_valid = anchor.valid;
    p.record = anchor.record;
    p.sample_ns = anchor.steady_ns;
    p.position[0] = anchor.chr_t.x;
    p.position[1] = anchor.chr_t.y;
    p.position[2] = anchor.chr_t.z;
    g_native_quick.Call(p);
}

std::string MotionWhy(const PiNativeQuickOutput& o, float world_scale) {
    switch (o.reason) {
    case PI_NQ_TICKS:
        return fmt::format("{} render ticks seen", o.ticks);
    case PI_NQ_STALE:
        return fmt::format("the last render tick {} ms old", o.age_ns / Ms);
    case PI_NQ_SPAN:
        return "no render tick 30 .. 300 ms before the last";
    default:
        return fmt::format("{:.2f} m/s over {} ms", o.speed / world_scale, o.interval_ns / Ms);
    }
}

const char* UiGate() {
    if (VR::IsGameMenuOpen()) {
        return "a game menu is open";
    }
    if (VR::CinematicActive()) {
        return "a cinematic";
    }
    return nullptr;
}

void Answer(const Q::Request& r, Q::Outcome outcome, const char* reason, s32 before = 0,
            s32 after = 0) {
    Q::Result result{};
    result.serial = r.serial;
    result.outcome = outcome;
    result.reason = reason;
    result.goods = r.goods;
    result.qty_before = before;
    result.qty_after = after;
    if (r.serial < 0x80000000u) {
        Q::PublishResult(result);
    }
    if (outcome == Q::Outcome::Denied || outcome == Q::Outcome::Expired) {
        ++g_site.denies;
    }
    if (outcome != Q::Outcome::Selected || g_log > 0) {
        LOG_INFO(Core, "VR quick: request {} {} slot {} goods {} -> {}{}{}{}", r.serial,
                 r.action == Q::Action::Use     ? "use"
                 : r.action == Q::Action::Throw ? "throw"
                                                : "select",
                 r.slot, r.goods, Q::OutcomeName(outcome), reason[0] != 0 ? " (" : "", reason,
                 reason[0] != 0 ? ")" : "");
    }
}

// The one aligned store SelectNext makes, plus the D-pad path's HUD bytes. False: denied (why).
const char* Select(const Pointers& p, int k, bool& changed) {
    changed = false;
    if (k < 0 || k >= Q::SlotCount) {
        return "no such slot";
    }
    if (Read<s32>(p.egd + OffsetOverride) >= 0) {
        return "an inventory item is pending";
    }
    s32 idx{}, goods{}, qty{};
    ReadSlot(p, k, idx, goods, qty);
    if (goods < 0 || qty <= 0) {
        return "the slot is empty";
    }
    auto* const selected = reinterpret_cast<std::atomic<s32>*>(p.ring + OffsetRingSelected);
    if (reinterpret_cast<uintptr_t>(selected) % 4 != 0) {
        return "misaligned selection";
    }
    if (selected->load(std::memory_order_relaxed) == idx) {
        return nullptr;
    }
    if (g_dry_run) {
        return "dry run";
    }
    selected->store(idx, std::memory_order_relaxed);
    if (g_hud_bytes) {
        for (const u64 rva : HudByteRvas) {
            *(g_base + rva) = 1;
        }
    }
    changed = true;
    ++g_site.selects;
    return nullptr;
}

s32 QtyOf(const Pointers& p, s32 goods, s32 slot_hint) {
    for (int k = 0; k < Q::SlotCount; ++k) {
        const int slot = k == 0 && slot_hint >= 0 ? slot_hint : (k == slot_hint ? 0 : k);
        s32 idx{}, g{}, qty{};
        ReadSlot(p, slot, idx, g, qty);
        if (g == goods) {
            return qty;
        }
    }
    return 0;
}

// Use or throw `r.goods` now (the gates of quickitems_design.md 5.1).
void Act(u8* player, const Pointers& p, const Q::Request& r) {
    const Q::GoodsClass cls = Q::Classify(r.goods);
    const bool throwing = r.action == Q::Action::Throw;
    if (const char* ui = UiGate(); ui != nullptr) {
        return Answer(r, Q::Outcome::Denied, ui);
    }
    const u8* const module = DataModule(player);
    if (module == nullptr || Read<s32>(module + OffsetHp) <= 0) {
        return Answer(r, Q::Outcome::Denied, "dead");
    }
    if (Read<s32>(player + OffsetItemUseState) != 0) {
        return Answer(r, Q::Outcome::Denied, "a menu item is being used");
    }
    if (HasEffect(player, ItemAnimEffect)) {
        return Answer(r, Q::Outcome::Denied, "an item animation is playing");
    }
    if (MenuMulti()) {
        return Answer(r, Q::Outcome::Denied, "an inventory multi-use is pending");
    }
    if (Read<s32>(p.egd + OffsetOverride) >= 0) {
        return Answer(r, Q::Outcome::Denied, "an inventory item is pending");
    }
    // quick_music_box_crank: the Tiny Music Box played by its crank (core/vr/music_box.h) is
    // used through UseGoods like an instant item (its clip, a00 100170, is skipped: no animation).
    const bool crank = r.crank != 0 && !throwing && r.goods == VR::MusicBox::Goods;
    if (!crank && (throwing ? cls.use != Q::UseKind::Throw
                            : (cls.use != Q::UseKind::Instant && cls.use != Q::UseKind::Bullet))) {
        return Answer(r, Q::Outcome::Denied, "not that kind of item");
    }
    const s32 before = QtyOf(p, r.goods, r.slot);
    if (before <= 0) {
        return Answer(r, Q::Outcome::Denied, "none left");
    }
    if (!CanUse(player, r.goods)) {
        return Answer(r, Q::Outcome::Denied, "the game says no (CanUseGoods)", before, before);
    }
    if (!throwing && (r.stab != 0 || Q::BloodDrinkOf(r.goods) || r.goods == Q::QueenlyBlood)) {
        // Preserve the blood items' existing full-HP protection when drunk or squeezed.
        // inject_full_hp=1 remains the explicit override, as for the regular Blood Vial.
        const s32 hp = Read<s32>(module + OffsetHp);
        const s32 max_hp = Read<s32>(module + OffsetMaxHp);
        if (max_hp > 0 && hp >= max_hp && !VR::GetSettings().inject_full_hp) {
            LOG_INFO(Core, "VR quick: blood goods {} refused at full HP ({}/{})", r.goods, hp,
                     max_hp);
            return Answer(r, Q::Outcome::Denied, "full HP", before, before);
        }
    }
    if (throwing && Read<s32>(player + OffsetEmitter) != -1) {
        return Answer(r, Q::Outcome::Denied, "the emitter is busy", before, before);
    }
    if (g_dry_run) {
        return Answer(r, Q::Outcome::Denied, "dry run", before, before);
    }
    const GoodsInfo info = LookupGoods(r.goods);
    if (!throwing) {
        PgdDiffArm(p.pgd, r.goods); // VR_QUICK_PGD_DIFF (tests)
    }
    u32 armed = 0;
    if (throwing && g_launch_hook && cls.bullet >= 0) {
        // Armed before the call: the launch may come during it.
        VR::Body::Anchor anchor{};
        const bool have_anchor = VR::Body::LatestAnchor(anchor);
        const float world_scale = have_anchor ? anchor.world_scale : 1.0f;
        PiNativeQuickInput prepare{};
        prepare.operation = PI_NQ_THROW;
        prepare.now_ns = NowNs();
        prepare.anchor_available = have_anchor;
        prepare.world_scale = anchor.world_scale;
        prepare.base_yaw = anchor.base_yaw;
        prepare.inherit_motion = Q::GetQuickSettings().throw_inherit_motion;
        std::copy_n(r.v_world.data(), 3, prepare.local_velocity);
        const auto prepared = g_native_quick.Call(prepare);
        if (!prepared.applied)
            return Answer(r, Q::Outcome::Denied, "interaction DLL fault", before, before);
        const VR::Body::Vec3 world{prepared.world[0], prepared.world[1], prepared.world[2]};
        const VR::Body::Vec3 carry{prepared.carry[0], prepared.carry[1], prepared.carry[2]};
        const std::string why = MotionWhy(prepared, world_scale);
        if (std::hypot(carry.x, carry.z) > 0.2f * world_scale || g_log > 0) {
            LOG_INFO(Core, "VR quick: throw carries the hunter's motion ({:.2f}, {:.2f}) m/s ({})",
                     carry.x / world_scale, carry.z / world_scale, why);
        }
        Armed a{};
        a.serial = armed = g_armed_serial.load(std::memory_order_relaxed) + 1;
        a.bullet = cls.bullet;
        a.owner = Read<u32>(player + OffsetHandle);
        a.goods = r.goods;
        a.throw_class = static_cast<u8>(cls.throw_class);
        a.issue_ns = NowNs();
        a.release_ns = r.steady_ms * Ms;
        a.scale = world_scale;
        a.v = {world.x, world.y, world.z};
        a.carry = {carry.x, 0.0f, carry.z};
        // Locked on: the lock point the game aims its own bullets at (the very gentle assist).
        if (VR::IsLockedOn() && Read<u8>(player + OffsetLockChosen) != 0) {
            a.target = true;
            a.target_p = Read3(player + OffsetLockPoint);
        } else if (g_fake_target) {
            u8* const ctrl = Read<u8*>(player + OffsetChrCtrl);
            if (Plausible(ctrl)) {
                // W * model: the hunter faces model -Z, his right is model -X.
                const u8* w = ctrl + OffsetChrFrame;
                const std::array<float, 3> m{-g_fake_target_m[0], g_fake_target_m[1],
                                             -g_fake_target_m[2]};
                for (int i = 0; i < 3; ++i) {
                    const u8* row = w + 16 * i;
                    a.target_p[static_cast<size_t>(i)] =
                        Read<float>(row) * m[0] + Read<float>(row + 4) * m[1] +
                        Read<float>(row + 8) * m[2] + Read<float>(row + 12);
                }
                a.target = a.fake_target = true;
            }
        }
        // knife_assist: the enemies the game would let him lock on to, locked on or not; the
        // other throws' unlocked assist (throw_assist_unlocked): the same, not locked on.
        const bool knife = cls.throw_class == Q::ThrowClass::Knife;
        if (knife ? Q::GetQuickSettings().knife_assist > 0.0f
                  : !a.target && Q::GetQuickSettings().throw_assist_unlocked > 0.0f) {
            u8* const mgr = Read<u8*>(g_base + LockTgtManRva);
            u8* e = Plausible(mgr) ? Read<u8*>(mgr + OffsetLockList) : nullptr;
            for (int i = 0; i < 16 && Plausible(e, 16) && a.candidates < a.candidate_p.size();
                 ++i) {
                const auto at = Read3(e);
                if (std::isfinite(at[0]) && std::isfinite(at[1]) && std::isfinite(at[2])) {
                    a.candidate_p[a.candidates++] = at;
                }
                e = Read<u8*>(e + OffsetLockNext);
            }
        }
        g_armed.Store(a);
        g_armed_serial.store(armed, std::memory_order_release);
    }
    static bool thread_logged = false;
    if (!thread_logged) {
        thread_logged = true;
        LOG_INFO(Core, "VR quick: the first use runs on thread '{}'",
                 Common::GetCurrentThreadName());
    }
    // (The music box: its clip's event 65 dummy, 17, where its bullet 400 starts.)
    const s32 dmy = throwing ? PalmDummy
                    : crank  ? (r.dmy >= 0 ? r.dmy : 17)
                    : (cls.use == Q::UseKind::Bullet ? cls.dmy : -1);
    const auto start = std::chrono::steady_clock::now();
    // Items that are never used up (the Infinite Items cheat, the game's no-consume debug flag,
    // a non-consumed goods): the count cannot tell whether the use went out.
    const bool frozen = InfiniteItems() || !info.consume ||
                        (Read<u16>(module + OffsetDebugFlags) & FlagNoGoodsConsume) != 0;
    const bool ran = g_use_goods(player, r.goods, dmy) != 0;
    s32 after = QtyOf(p, r.goods, r.slot);
    s32 used_dmy = dmy;
    const bool launched =
        armed != 0 && g_consumed_serial.load(std::memory_order_acquire) == armed;
    if (throwing && ran && after == before && !frozen && !launched) {
        // No bullet (no dummy found): the back of the left hand, then give up. Never when the
        // count cannot drop (it would throw a second one) or the first one already launched.
        used_dmy = 2;
        g_use_goods(player, r.goods, used_dmy);
        after = QtyOf(p, r.goods, r.slot);
    }
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
    if (!ran) {
        return Answer(r, Q::Outcome::Denied, "the character type forbids items", before, after);
    }
    LOG_INFO(Core, "VR quick: UseGoods({}, dmy {}) count {} -> {} ({} us){}", r.goods, used_dmy,
             before, after, micros, frozen && after == before ? " (not consumed: no-consume)" : "");
    if (crank) {
        // Never consumed: UseGoods ran is the play. Its tune (the clip's c100171, c100170) plays
        // whatever quick_use_sounds says: the tune is what the crank is for.
        if (g_play_sound != nullptr) {
            QueueUseCue(r.goods, VR::MusicBox::TuneCue);
        }
        LOG_INFO(Core, "VR quick: MUSIC BOX played by the crank: UseGoods({}, dmy {}) ran, its "
                       "tune queued (use anim {}){}",
                 r.goods, used_dmy, info.use_anim, g_play_sound == nullptr ? " (NO PlaySound)" : "");
        ++g_site.uses;
        return Answer(r, Q::Outcome::Used, "", before, after);
    }
    if (after == before - 1 || (frozen && after == before)) {
        if (r.goods >= 701 && r.goods <= 706) {
            VR::NoteItemHeal(); // the NPC bloods heal: the heal vignette
        }
        // quick_use_sounds: the use clip's own sound (the clip was skipped); 0: only a drink at
        // the mouth (quick_mouth_sound), as before.
        const auto& qs = Q::GetQuickSettings();
        if (!throwing && g_play_sound != nullptr) {
            if (qs.use_sounds) {
                QueueUseCue(r.goods, info.use_anim);
            } else if (r.mouth == static_cast<u8>(Q::MouthKind::Drink) && qs.mouth_sound) {
                QueueUseCue(r.goods, 10);
            }
        }
        if (r.wipe != 0 && !throwing) {
            // quick_use_wipe: the buff the paper puts on the blade, looked for from now (site A).
            g_site.wipe_ref = info.ref;
            g_site.wipe_goods = r.goods;
            g_site.wipe_ns = NowNs();
        }
        if (throwing) {
            ++g_site.throws;
        } else {
            ++g_site.uses;
        }
        return Answer(r, throwing ? Q::Outcome::Thrown : Q::Outcome::Used, "", before, after);
    }
    if (armed != 0) {
        g_consumed_serial.store(armed, std::memory_order_release); // no launch to wait for
    }
    Answer(r, Q::Outcome::Latched, "nothing was consumed: the game's own use from now on", before,
           after);
}

void Handle(u8* player, const Pointers& p, const Q::Request& r, bool may_use) {
    const s64 age_ms = NowNs() / Ms - r.steady_ms;
    if (age_ms > RequestTtlMs || age_ms < -RequestTtlMs) {
        return Answer(r, Q::Outcome::Expired, "too old");
    }
    if (r.select >= 0) {
        if (const char* ui = UiGate(); ui != nullptr) {
            return Answer(r, Q::Outcome::Denied, ui);
        }
        bool changed = false;
        if (const char* why = Select(p, r.select, changed); why != nullptr) {
            if (r.action == Q::Action::None) {
                return Answer(r, Q::Outcome::Denied, why);
            }
            LOG_INFO(Core, "VR quick: select slot {} skipped ({})", r.select, why);
        }
    }
    if (r.action == Q::Action::None) {
        return Answer(r, Q::Outcome::Selected, "");
    }
    if (!may_use) {
        return Answer(r, Q::Outcome::Denied, "the vial was injected this update");
    }
    Act(player, p, r);
}

void Publish(u8* player, const Pointers* p) {
    if (p != nullptr) {
        PgdDiffTick(p->pgd); // VR_QUICK_PGD_DIFF (tests)
    }
    UseGoodsTraceOnce(); // VR_QUICK_USEGOODS_TRACE (tests)
    Q::Snapshot s{};
    s.seq = ++g_site.updates;
    s.steady_ns = NowNs();
    s.valid = p != nullptr;
    if (p != nullptr) {
        const s32 selected = Read<s32>(p->ring + OffsetRingSelected);
        s.override_idx = Read<s32>(p->egd + OffsetOverride);
        const bool recheck = ++g_site.usable_tick >= UsableEvery;
        if (recheck) {
            g_site.usable_tick = 0;
        }
        for (int k = 0; k < Q::SlotCount; ++k) {
            Q::Slot& slot = s.slot[static_cast<size_t>(k)];
            ReadSlot(*p, k, slot.inv_idx, slot.goods, slot.qty);
            if (slot.goods >= 0) {
                const GoodsInfo info = LookupGoods(slot.goods);
                slot.icon = info.icon;
                slot.ffx = info.ffx;
                if (recheck || g_site.usable_goods[static_cast<size_t>(k)] != slot.goods) {
                    g_site.usable_goods[static_cast<size_t>(k)] = slot.goods;
                    g_site.usable[static_cast<size_t>(k)] = CanUse(player, slot.goods);
                }
                slot.usable = g_site.usable[static_cast<size_t>(k)];
                if (slot.qty > 0 && slot.inv_idx == selected && s.selected < 0) {
                    s.selected = k;
                }
            }
        }
        const u8* const module = DataModule(player);
        s.alive = module == nullptr || Read<s32>(module + OffsetHp) > 0;
        s.busy = Read<s32>(player + OffsetItemUseState) != 0;
    }
    Q::PublishSnapshot(s);
    const s64 now = s.steady_ns;
    if (s.valid && !g_site.first_valid_logged) {
        g_site.first_valid_logged = true;
        g_site.first_valid_ns = now;
        if (!g_test_give.empty() && g_add_item != nullptr) {
            // Tests: the items through the game's own pickup path (it fills empty quick slots).
            size_t at = 0;
            while (at < g_test_give.size()) {
                const size_t end = std::min(g_test_give.find(',', at), g_test_give.size());
                const std::string item = g_test_give.substr(at, end - at);
                at = end + 1;
                const auto colon = item.find(':');
                const s32 goods = std::atoi(item.c_str());
                const s32 count = colon == std::string::npos ? 1 : std::atoi(item.c_str() + colon + 1);
                if (goods <= 0 || count <= 0) {
                    continue;
                }
                alignas(16) std::array<u32, 8> handle{0xB0000000u | static_cast<u32>(goods)};
                s32 index = g_add_item(p->egd, nullptr, handle.data(), count, 0, 1, 0);
                if (index < 0) {
                    // Already at its maximum (a tool such as the Monocular): the one owned.
                    const u32 highest = std::min(Read<u32>(p->egd + OffsetInvHighest), 2048u);
                    for (u32 i = 0; i <= highest && index < 0; ++i) {
                        u8* const e = EntryOf(p->egd, static_cast<s32>(i));
                        if (e != nullptr && Read<u32>(e) != 0 &&
                            Read<u32>(e + 4) == (0x40000000u | static_cast<u32>(goods))) {
                            index = static_cast<s32>(i);
                        }
                    }
                }
                // Into the first empty quick slot, as the Equipment menu would put it (the pickup
                // path leaves the slots alone here).
                int slot = -1;
                for (int k = 0; k < Q::SlotCount && index >= 0; ++k) {
                    const s32 idx = SlotIndex(*p, k);
                    if (idx == index) {
                        slot = k;
                        break;
                    }
                    if (idx == -1 && slot < 0) {
                        slot = k;
                    }
                }
                if (slot >= 0 && SlotIndex(*p, slot) != index) {
                    const u32 h = handle[0];
                    std::memcpy(p->egd + OffsetSlotIndex - 4 + 8 * slot, &h, sizeof(h));
                    std::memcpy(p->egd + OffsetSlotIndex + 8 * slot, &index, sizeof(index));
                    if (Read<s32>(p->ring + OffsetRingSelected) < 0) {
                        std::memcpy(p->ring + OffsetRingSelected, &index, sizeof(index));
                    }
                    if (g_hud_bytes) {
                        for (const u64 rva : HudByteRvas) {
                            *(g_base + rva) = 1;
                        }
                    }
                }
                LOG_INFO(Core, "VR quick: TEST give goods {} x{} -> inventory index {}, quick slot {}",
                         goods, count, index, slot);
            }
        }
        if (g_log >= 3) {
            // VR_QUICK_LOG=3: the goods in the inventory once (tests: what can be equipped).
            std::string goods;
            const u32 highest = std::min(Read<u32>(p->egd + OffsetInvHighest), 2048u);
            for (u32 i = 0; i <= highest; ++i) {
                u8* const e = EntryOf(p->egd, static_cast<s32>(i));
                if (e != nullptr && Read<u32>(e) != 0 &&
                    (Read<u32>(e + 4) & 0xF0000000u) == 0x40000000u) {
                    goods += fmt::format(" {}:{}x{}", i, Read<u32>(e + 4) & 0x0FFFFFFFu,
                                         Read<u32>(e + 8));
                }
            }
            LOG_INFO(Core, "VR quick: inventory goods (index:goods x count):{}", goods);
        }
    }
    const s64 every = g_log >= 2 ? 2000 * Ms : 10000 * Ms;
    const bool changed = [&] {
        for (int k = 0; k < Q::SlotCount; ++k) {
            const auto& a = s.slot[static_cast<size_t>(k)];
            const auto& b = g_site.last.slot[static_cast<size_t>(k)];
            if (a.goods != b.goods || a.qty != b.qty || a.usable != b.usable) {
                return true;
            }
        }
        return s.selected != g_site.last.selected || s.valid != g_site.last.valid;
    }();
    if (changed || (g_log > 0 && now - g_site.last_state_log_ns >= every)) {
        g_site.last_state_log_ns = now;
        std::string slots;
        for (int k = 0; k < Q::SlotCount; ++k) {
            const auto& a = s.slot[static_cast<size_t>(k)];
            slots += fmt::format("{}{}:{}x{}{}{}", k == 0 ? "" : " ", k, a.goods, a.qty,
                                 a.usable ? "" : "(no)", k == s.selected ? "*" : "");
        }
        LOG_INFO(Core,
                 "VR quick: STATE {} [{}] override {} busy {} | selects {} uses {} throws {} "
                 "denies {} launches {}",
                 s.valid ? "ok" : "no data", slots, s.override_idx, s.busy ? 1 : 0,
                 g_site.selects, g_site.uses, g_site.throws, g_site.denies,
                 g_launches.load(std::memory_order_relaxed));
    }
    g_site.last = s;
}

// VR_QUICK_AUTOTEST=select:<k>|use:<k>|throw:<k>[,...]: one step every 3 s from 3 s after the
// first valid snapshot (no controller needed). Throws go forward and up at 7 m/s (LOCAL).
bool AutotestRequest(const Q::Snapshot& s, Q::Request& r) {
    auto& st = g_site;
    if (g_autotest.empty() || !s.valid || st.first_valid_ns == 0) {
        return false;
    }
    const s64 now = NowNs();
    if (st.autotest_next_ns == 0) {
        st.autotest_next_ns = st.first_valid_ns + 3000 * Ms;
    }
    if (now < st.autotest_next_ns) {
        return false;
    }
    size_t begin = 0;
    for (size_t i = 0; i < st.autotest_step; ++i) {
        begin = g_autotest.find(',', begin);
        if (begin == std::string::npos) {
            return false;
        }
        ++begin;
    }
    const std::string step = g_autotest.substr(begin, g_autotest.find(',', begin) - begin);
    ++st.autotest_step;
    st.autotest_next_ns = now + 3000 * Ms;
    const auto colon = step.find(':');
    const int k = colon == std::string::npos ? 0 : std::atoi(step.c_str() + colon + 1);
    const std::string verb = step.substr(0, colon);
    r = {};
    r.serial = ++st.autotest_serial;
    r.steady_ms = now / Ms;
    r.slot = std::clamp(k, 0, Q::SlotCount - 1);
    r.goods = s.slot[static_cast<size_t>(r.slot)].goods;
    if (verb == "select") {
        r.select = static_cast<s8>(r.slot);
    } else if (verb == "use") {
        r.action = Q::Action::Use;
    } else if (verb == "throw") {
        r.action = Q::Action::Throw;
        r.v_world = {0.0f, 3.5f, -6.0f}; // LOCAL: forward (-Z) and up
    } else {
        LOG_WARNING(Core, "VR quick: AUTOTEST unknown step '{}'", step);
        return false;
    }
    LOG_INFO(Core, "VR quick: AUTOTEST {} slot {} (goods {})", verb, r.slot, r.goods);
    return true;
}

// ---- Render tick (loop thread) ------------------------------------------------------------------

struct Mapping {
    u32 space_serial{0};
    VR::Body::Vec3 eye_world{}; // the mono eye E
    VR::Body::Vec3 head{};      // the tracking head the camera was placed for
    float base_yaw{0.0f};
    float scale{1.0f};
    std::array<float, 12> rows{};
    VR::Body::Vec3 position{};

    VR::Body::Vec3 ToWorld(Q::V3 p) const {
        return eye_world + VR::Body::TrackingToWorld({p.x - head.x, p.y - head.y, p.z - head.z},
                                                     base_yaw, scale);
    }
    Q::V3 ToTracking(VR::Body::Vec3 w) const {
        const VR::Body::Vec3 t = VR::Body::WorldToTracking(w - eye_world, base_yaw, scale);
        return {head.x + t.x, head.y + t.y, head.z + t.z};
    }
    Q::V3 WorldToView(VR::Body::Vec3 w) const {
        const VR::Body::Vec3 d = w - position;
        return {rows[0] * d.x + rows[1] * d.y + rows[2] * d.z,
                rows[4] * d.x + rows[5] * d.y + rows[6] * d.z,
                rows[8] * d.x + rows[9] * d.y + rows[10] * d.z};
    }
    Q::V3 DirToView(VR::Body::Vec3 d) const {
        return {rows[0] * d.x + rows[1] * d.y + rows[2] * d.z,
                rows[4] * d.x + rows[5] * d.y + rows[6] * d.z,
                rows[8] * d.x + rows[9] * d.y + rows[10] * d.z};
    }
    // The inverse of WorldToView (the camera's rows are orthonormal: the transpose).
    VR::Body::Vec3 ViewToWorld(Q::V3 v) const {
        return position + VR::Body::Vec3{rows[0] * v.x + rows[4] * v.y + rows[8] * v.z,
                                         rows[1] * v.x + rows[5] * v.y + rows[9] * v.z,
                                         rows[2] * v.x + rows[6] * v.y + rows[10] * v.z};
    }
    // inframe_hand_sync: this update's carry (the render-camera hook's HandSyncBegin), if any.
    bool carry_on{false};
    F::HandCarry carry{};
    // A world point placed from his pose this update, where the hand sync draws what is placed
    // there (the hand items are carried after the producers; this is for what is kept: the hover
    // point, the use ring's place, the throw's start). As is without a carry.
    VR::Body::Vec3 Carried(VR::Body::Vec3 w) const {
        if (!carry_on) {
            return w;
        }
        const auto c = F::CarryPoint(carry.from, carry.to, {w.x, w.y, w.z});
        return {c[0], c[1], c[2]};
    }
};

struct RenderState {
    u32 placed_serial{0};
    u32 waiting_serial{0}; // an open whose placement waits for the arm (logged once)
    s64 arm_since_ns{0};   // the left arm has been yours (weight >= arm_min) since, 0 = not
    Q::Layout layout{};
    std::array<s8, Q::SlotCount> card_slot{}; // layout index -> slot
    s64 placed_ns{0};
    int hover{-1};       // layout index
    s64 carry_log_ns{0}; // VR_QUICK_LOG=2: the hover point's carry, every 250 ms
    s64 hover_ns{0};
    float hover_glow{0.0f};
    std::array<float, Q::SlotCount> lift{}; // eased hover lift per layout index
    s64 last_ns{0};
    bool placed_logged{false};
};
RenderState g_render{};

// An item: `centre` (view), the axes (view, unit), sizes in metres (times the world scale).
F::Item MakeItem(F::MeshId mesh, Q::V3 centre, Q::V3 x, Q::V3 y, Q::V3 z, float sx, float sy,
                 float sz) {
    F::Item item{};
    item.mesh = mesh;
    item.mv = {x.x * sx, y.x * sy, z.x * sz, centre.x, x.y * sx, y.y * sy, z.y * sz, centre.y,
               x.z * sx, y.z * sy, z.z * sz, centre.z};
    return item;
}

constexpr std::array<float, 4> Linear(float r, float g, float b, float a = 1.0f) {
    return {r, g, b, a};
}

std::string CountText(s32 qty) {
    return std::to_string(std::clamp(qty, 0, 999));
}

// The arc's look, after the game's own HUD item boxes and menus: a small rounded square of dark,
// softly translucent slate per item, a hairline frame of worn pewter (the game's selected item:
// warmer, like brass), the game's own icon and count in its font and bone white; the hovered item
// brightens with a soft candle-like glow, as a selection in the game's menus does. It fades and
// eases in and out (no pops), and dims with the scene round it so it never looks pasted on.
struct ArcStyle {
    std::array<float, 4> slate{0.030f, 0.033f, 0.038f, 0.74f};       // linear rgb, opacity
    std::array<float, 4> frame{0.26f, 0.245f, 0.215f, 0.85f};        // worn pewter
    std::array<float, 4> frame_selected{0.50f, 0.38f, 0.19f, 0.95f}; // the game's selection
    std::array<float, 4> frame_deny{0.42f, 0.12f, 0.07f, 0.95f};
    std::array<float, 3> bone{0.80f, 0.765f, 0.69f}; // the game's text white
    std::array<float, 3> ochre{0.70f, 0.48f, 0.22f}; // the last one left
    float icon_scale{0.80f};                         // of the card
    float fade_in_s{0.16f};
    float fade_out_s{0.14f};
    float stagger_s{0.018f};
    float hover_s{0.12f};
    float hover_grow{0.08f};
    float hover_lift_m{0.010f};
    // The hovered card where the hunter's own fist hides it (it reaches into the card): shown
    // through the fingers at this opacity, faded in with the hover (quick_arc_hover_xray).
    float xray_alpha{0.85f};
};

// A copy of `item` drawn only where the scene hides it (F::Item::xray) at `alpha` of its opacity.
void AddXray(F::List& list, const F::Item& item, float alpha) {
    if (alpha <= 0.01f) {
        return;
    }
    F::Item x = item;
    x.xray = true;
    x.color[3] *= alpha;
    x.emissive[3] *= alpha;
    list.Add(x);
}

float EaseOut(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

void BuildArc(F::List& list, const Mapping& map, const Q::Scene& scene, const Q::Snapshot& snap,
              s64 now, float scale_m) {
    auto& rs = g_render;
    const auto& qs = Q::GetQuickSettings();
    ArcStyle st{};
    st.slate[3] = VR::Tune::Get("arc.slate_a", st.slate[3]);
    st.frame[3] = VR::Tune::Get("arc.frame_a", st.frame[3]);
    st.icon_scale = VR::Tune::Get("arc.icon", st.icon_scale);
    const bool open = scene.state == Q::SceneState::Open;
    // An arc open again at its placement (left_scheme=2: an item put back, the next throw) fades
    // in from that open, not from its placement.
    const s64 shown_ns = open ? std::max(rs.placed_ns, scene.state_ns) : rs.placed_ns;
    const float seconds = static_cast<float>(now - shown_ns) * 1e-9f;
    const float since = static_cast<float>(now - scene.state_ns) * 1e-9f;
    const float dt = std::clamp(static_cast<float>(now - rs.last_ns) * 1e-9f, 0.0f, 0.1f);
    rs.last_ns = now;
    const Q::V3 eye_view{0.0f, 0.0f, 0.0f};
    const float card = qs.card_m * (VR::GetSettings().seated ? 0.9f : 1.0f);
    int name_slot = -1;
    for (int i = 0; i < rs.layout.n; ++i) {
        const int k = rs.card_slot[static_cast<size_t>(i)];
        const Q::Slot& slot = snap.slot[static_cast<size_t>(k)];
        // Fade and ease: in over fade_in_s (staggered from the middle out), out over fade_out_s.
        const float centre_rank = std::abs(static_cast<float>(i) - 0.5f * (rs.layout.n - 1));
        const float fade = open ? EaseOut((seconds - st.stagger_s * centre_rank) / st.fade_in_s)
                                : 1.0f - EaseOut(since / st.fade_out_s);
        if (fade <= 0.01f) {
            continue;
        }
        const bool hovered = open && i == rs.hover;
        float& lift = rs.lift[static_cast<size_t>(i)];
        lift += ((hovered ? 1.0f : 0.0f) - lift) * std::min(dt / (0.5f * st.hover_s), 1.0f);
        const float ease = lift * lift * (3.0f - 2.0f * lift);
        const VR::Body::Vec3 world = map.ToWorld(rs.layout.card[static_cast<size_t>(i)]);
        Q::V3 c = map.WorldToView(world);
        const Q::V3 n = Q::Normalize(eye_view - c, {0.0f, 0.0f, -1.0f}); // toward the eye
        c = c + n * (st.hover_lift_m * ease * scale_m);
        Q::V3 up{0.0f, 1.0f, 0.0f};
        up = Q::Normalize(up - n * Q::Dot(up, n), {0.0f, 1.0f, 0.0f});
        const Q::V3 right = Q::Cross(n, up);
        const float grow = (open ? 0.92f + 0.08f * fade : 0.96f + 0.04f * fade);
        const float size = card * grow * (1.0f + st.hover_grow * ease) * scale_m;
        const bool empty = slot.Empty();
        const bool selected = k == snap.selected;
        // A grab (warm) or a deny (muted rust) flashes the card.
        float flash = 0.0f;
        if (scene.pulse_slot == k) {
            const float t = static_cast<float>(now - scene.pulse_ns) * 1e-9f;
            flash = t >= 0.0f && t < 0.3f ? 1.0f - t / 0.3f : 0.0f;
        }
        const bool deny = scene.pulse_deny && flash > 0.0f;
        // The slate with its hairline frame and the hover's candlelight (writes depth: DIBR shows
        // the card at its own depth).
        F::Item plate = MakeItem(F::MeshId::Quad, c, right, up, n, size, size, 1.0f);
        plate.shade = F::Shade::Card;
        plate.color = st.slate;
        plate.color[3] *= fade;
        const auto& frame = deny ? st.frame_deny : selected ? st.frame_selected : st.frame;
        plate.emissive = frame;
        const float bright = 1.0f + 0.5f * ease + 0.6f * (deny ? 0.0f : flash);
        for (int ch = 0; ch < 3; ++ch) {
            plate.emissive[static_cast<size_t>(ch)] *= bright;
        }
        plate.emissive[3] *= fade;
        plate.uv = {std::max(ease, deny ? 0.0f : flash) * fade, 0.0f, 0.0f, 0.0f};
        list.Add(plate);
        // The hovered card through the fist that covers it (plate, icon and count below); every
        // card faintly wherever the scene hides it (a wall or pillar the arc opened into, the
        // hunter's own arm), so no card is ever out of sight (quick_arc_hidden_xray).
        const float xray =
            std::max(qs.hover_xray && open ? st.xray_alpha * ease : 0.0f, qs.hidden_xray);
        AddXray(list, plate, xray);
        // The game's icon on it (dimmed when the game would not allow it now).
        if (!empty && qs.icons && slot.icon >= 0) {
            F::Item icon = MakeItem(F::MeshId::Quad, c + n * (0.004f * scale_m), right, up, n,
                                    size * st.icon_scale, size * st.icon_scale, 1.0f);
            icon.shade = F::Shade::Icon;
            icon.tex = F::Tex::Icon;
            icon.tex_key = static_cast<u32>(slot.icon);
            const float tint = (slot.usable ? 1.0f : 0.42f) * (1.0f + 0.15f * ease);
            icon.color = Linear(tint, tint, tint, fade);
            // 400 of the 416 texels are the icon (BC7 padding).
            icon.uv = {0.0f, 0.0f, 400.0f / 416.0f, 400.0f / 416.0f};
            list.Add(icon);
            AddXray(list, icon, xray);
        }
        // The count, bottom right over the box's corner as on the game's HUD.
        if (!empty && qs.labels) {
            const std::string text = CountText(slot.qty);
            const int label = list.AddLabel(text.c_str(), 48);
            if (label >= 0) {
                const float h = size * 0.34f;
                const Q::V3 at =
                    c + right * (size * 0.30f) - up * (size * 0.34f) + n * (0.006f * scale_m);
                F::Item digits = MakeItem(F::MeshId::Quad, at, right, up, n, h, h, 1.0f);
                digits.shade = F::Shade::Text;
                digits.tex = F::Tex::Label;
                digits.tex_key = static_cast<u32>(label);
                digits.fit_label = static_cast<s8>(label);
                const auto& ink = slot.qty == 1 ? st.ochre : st.bone;
                digits.color = Linear(ink[0], ink[1], ink[2], fade);
                digits.emissive = {0.005f, 0.005f, 0.006f, 0.85f}; // a soft dark halo
                list.Add(digits);
                AddXray(list, digits, xray);
            }
        }
        if (hovered || (name_slot < 0 && open && rs.hover < 0 && selected)) {
            name_slot = k;
        }
    }
    // The hovered (else the game's selected) item's name above the arc, in the game's font with a
    // hairline under it, as on the game's HUD.
    if (name_slot >= 0 && qs.labels && rs.layout.n > 0) {
        const Q::Slot& slot = snap.slot[static_cast<size_t>(name_slot)];
        const std::string name = VR::Assets::GoodsName(slot.goods);
        const int label = name.empty() ? -1 : list.AddLabel(name.c_str(), 48);
        if (label >= 0) {
            const bool hovered_name = rs.hover >= 0;
            const float fade =
                open ? EaseOut(seconds / st.fade_in_s) : 1.0f - EaseOut(since / st.fade_out_s);
            const float h = 0.017f * scale_m; // text height (cap to descender)
            // Above the top card in the arc's (leaning) plane.
            float top = -1e9f;
            for (int i = 0; i < rs.layout.n; ++i) {
                top = std::max(top, Q::Dot(rs.layout.card[static_cast<size_t>(i)] - rs.layout.pivot,
                                           rs.layout.up));
            }
            const Q::V3 above = rs.layout.pivot + rs.layout.up * (top + card * 0.5f + 0.028f);
            const Q::V3 at = map.WorldToView(map.ToWorld(above));
            const Q::V3 name_n = Q::Normalize(eye_view - at, {0.0f, 0.0f, -1.0f});
            Q::V3 name_up{0.0f, 1.0f, 0.0f};
            name_up = Q::Normalize(name_up - name_n * Q::Dot(name_up, name_n), {0.0f, 1.0f, 0.0f});
            const Q::V3 name_right = Q::Cross(name_n, name_up);
            const float a = (hovered_name ? 1.0f : 0.6f) * fade;
            F::Item text = MakeItem(F::MeshId::Quad, at, name_right, name_up, name_n, h, h, 1.0f);
            text.shade = F::Shade::Text;
            text.tex = F::Tex::Label;
            text.tex_key = static_cast<u32>(label);
            text.fit_label = static_cast<s8>(label);
            text.color = Linear(st.bone[0], st.bone[1], st.bone[2], a);
            text.emissive = {0.005f, 0.005f, 0.006f, 0.85f};
            list.Add(text);
            // The hovered item's name through the hand, too.
            const float name_xray = std::max(
                qs.hover_xray && open && hovered_name ? st.xray_alpha : 0.0f, qs.hidden_xray);
            AddXray(list, text, name_xray);
            // The hairline under it (as long as the name: fitted to the label).
            F::Item line = MakeItem(F::MeshId::Quad, at - name_up * (0.62f * h), name_right,
                                    name_up, name_n, h, 0.10f * h, 1.0f);
            line.shade = F::Shade::Unlit;
            line.color =
                Linear(st.frame[0] * 1.3f, st.frame[1] * 1.3f, st.frame[2] * 1.3f, 0.5f * a);
            line.fit_label = static_cast<s8>(label);
            line.fit_pad = 0.2f;
            list.Add(line);
            AddXray(list, line, name_xray);
        }
    }
}

// A held item's look with VR_TUNE_FILE's values over it (held.<goods>.<field>, degrees for the
// turns): placement work in a test run without a rebuild.
Q::HeldLook TunedLook(s32 goods) {
    Q::HeldLook look = Q::HeldLookOf(goods);
    if (look.fit_m > 0) {
        if (const auto model = VR::Assets::GetModel(look.model)) {
            const float longest = std::max({model->max[0] - model->min[0],
                model->max[1] - model->min[1], model->max[2] - model->min[2]});
            look.scale *= look.fit_m / std::max(longest, .001f);
            look.hold_x += .5f * (model->min[0] + model->max[0]);
            look.hold_y += .5f * (model->min[1] + model->max[1]);
            look.hold_z += .5f * (model->min[2] + model->max[2]);
        }
    }
    if (!VR::Tune::Active()) {
        return look;
    }
    const std::string base = fmt::format("held.{}.", goods);
    const auto get = [&base](const char* field, float value) {
        return VR::Tune::Get((base + field).c_str(), value);
    };
    look.scale = get("scale", look.scale);
    look.hold_y = get("hold", look.hold_y);
    look.pitch_deg = get("pitch", look.pitch_deg);
    look.yaw_deg = get("yaw", look.yaw_deg);
    look.roll_deg = get("roll", look.roll_deg);
    look.lift_m = get("lift", look.lift_m);
    look.out_m = get("out", look.out_m);
    look.side_m = get("side", look.side_m);
    look.hold_x = get("pivot_x", look.hold_x);
    look.hold_z = get("pivot_z", look.hold_z);
    return look;
}

// The grab's confirmation on the held item: a soft warm glow, mostly round its silhouette (the
// shader's rim), that rises over 60 ms and fades over 0.45 s.
std::array<float, 4> GrabGlow(const Q::Scene& scene, s64 now) {
    if (scene.pulse_slot < 0 || scene.pulse_slot != scene.held_slot || scene.pulse_deny) {
        return {};
    }
    const float t = static_cast<float>(now - scene.pulse_ns) * 1e-9f;
    if (t < 0.0f || t >= 0.51f) {
        return {};
    }
    const float rise = std::min(t / 0.06f, 1.0f);
    const float fall = 1.0f - std::clamp((t - 0.06f) / 0.45f, 0.0f, 1.0f);
    const float glow = 0.55f * rise * fall * fall;
    return {1.00f * glow, 0.62f * glow, 0.30f * glow, 0.0f};
}

// quick_hold_use's ring round the held item (vr_inframe.frag ShadeHoldRing): a thin band facing
// the eye round the item's centre (`c`, view; `size`: the item's own size, in the units of its
// card), filling clockwise from the top over the hold (Scene::hold_from_ns, hold_s): a dark
// slate track, the fill bone white warming to gold as it nears the end, its leading end a little
// brighter. Once full (Scene::hold_done_ns) its flash: the band whole and bright, a warm glow
// inside it, the ring opening out a little as it fades over HoldFlashS (also after the item has
// left the hand: where it last was, g_ring_place). Drawn over the fist that holds the item
// (depth_pull), still hidden by what really is in front of it.
constexpr float HoldFlashS = 0.38f;
struct RingPlace {
    bool valid{false};
    VR::Body::Vec3 at{}; // world
    float size{0.0f};    // game units
    s64 ns{0};
};
RingPlace g_ring_place{};

// The flash's strength now (1 at the use, 0 when over; < 0: none).
float HoldFlash(const Q::Scene& scene, s64 now) {
    if (scene.hold_done_ns <= 0) {
        return -1.0f;
    }
    const float t = static_cast<float>(now - scene.hold_done_ns) * 1e-9f / HoldFlashS;
    if (t < 0.0f || t >= 1.0f) {
        return -1.0f;
    }
    return (1.0f - t) * (1.0f - t);
}

void AddHoldRing(F::List& list, const Mapping& map, Q::V3 c, float size, const Q::Scene& scene,
                 s64 now, bool filling) {
    float progress = -1.0f;
    if (filling && scene.hold_from_ns > 0 && scene.hold_s > 0.0f) {
        progress = std::clamp(static_cast<float>(now - scene.hold_from_ns) * 1e-9f / scene.hold_s,
                              0.0f, 1.0f);
    }
    const float flash = HoldFlash(scene, now);
    if (progress < 0.0f && flash < 0.0f) {
        return;
    }
    // Where the ring is drawn: round the item in his fist (`filling`: a hand item, carried by the
    // hand sync) or, after the use, the place kept from the last of those.
    g_ring_place = {true, filling ? map.Carried(map.ViewToWorld(c)) : map.ViewToWorld(c), size,
                    now};
    const Q::V3 n = Q::Normalize(Q::V3{0.0f, 0.0f, 0.0f} - c, {0.0f, 0.0f, -1.0f});
    Q::V3 up{0.0f, 1.0f, 0.0f};
    up = Q::Normalize(up - n * Q::Dot(up, n), {0.0f, 1.0f, 0.0f});
    const Q::V3 right = Q::Cross(n, up);
    // The band's radius 0.44 of the quad: its centre line 0.70 of the item's size from its centre
    // (round a card's corners); 0.022 x 2 of the quad thick (about 6 mm round a 9 cm card).
    const float grow = flash >= 0.0f ? 1.0f + 0.25f * (1.0f - std::sqrt(flash)) : 1.0f;
    const float side = size * 0.70f / 0.44f * VR::Tune::Get("hold.ring", 1.0f) * grow;
    F::Item ring = MakeItem(F::MeshId::Quad, c + n * (0.006f * map.scale), right, up, n, side,
                            side, 1.0f);
    ring.shade = F::Shade::HoldRing;
    const float p = std::max(progress, 0.0f);
    const ArcStyle st{};
    ring.color = {st.slate[0], st.slate[1], st.slate[2], 0.62f};     // the track
    const std::array<float, 3> gold{0.86f, 0.60f, 0.26f};           // brass, lit
    const float warm = flash >= 0.0f ? 1.0f : p * p;
    ring.emissive = {st.bone[0] + (gold[0] - st.bone[0]) * warm,
                     st.bone[1] + (gold[1] - st.bone[1]) * warm,
                     st.bone[2] + (gold[2] - st.bone[2]) * warm, 0.95f}; // the fill
    if (flash >= 0.0f) {
        ring.color[3] = 0.62f * flash;
        ring.emissive[3] = 0.95f * std::sqrt(flash);
    }
    ring.uv = {flash >= 0.0f ? 1.0f : p, std::max(flash, 0.0f), 0.44f,
               VR::Tune::Get("hold.ring_w", 0.022f)};
    ring.depth_pull = 0.05f * map.scale;
    list.Add(ring);
}

// The held quick item in the hunter's left palm (dummy 3's frame `m`): the game's own model of it
// (HeldLookOf: life size, lit by the scene round it, with the grab's glow), its icon as a small
// cut-out for the items the game has no model of, or, while the model is not loaded (or the game
// files lack it), our plain mesh.
// The items from `first` on sit on the hunter's hand (F::Item::hand).
void MarkHand(F::List& list, u32 first) {
    for (u32 i = first; i < list.count; ++i) {
        list.items[i].hand = true;
    }
}

// The blade's rubs (quick_use_wipe: Fire Paper, Bolt Paper, the Empty Phantasm Shell): our models
// (vr_goods_meshes.h), each held by its left end (the mesh's origin), the rest reaching right
// toward the blade, its face (+z) to the eye. Their bounds (mesh metres) seat them in a holder and
// are the wipe's rectangle, so the rub is where the item is drawn.
struct RubLook {
    F::MeshId mesh{F::MeshId::Count};
    std::array<float, 3> mid{}, half{};
    // Its origin (the held end) from the card's centre in the card's frame (the eye's right, up,
    // toward the eye; metres at the card's 9 cm), and its roll about the eye's axis (degrees,
    // negative: its far end droops): fitted to the RubPinch grip on a proxy hand, held 2 cm into
    // the palm (the user, 2026-10-12).
    std::array<float, 3> pinch{};
    float roll_deg{0.0f};
};
constexpr RubLook RubLookOf(s32 goods) {
    switch (goods) {
    case 1300:
        return {F::MeshId::RubPaperFire, {.040f, .0095f, -.009f}, {.055f, .029f, .013f},
                {.020f, -.013f, -.0305f}, -15.0f};
    case 1320:
        return {F::MeshId::RubPaperBolt, {.040f, .0095f, -.009f}, {.055f, .029f, .013f},
                {.020f, -.013f, -.0305f}, -15.0f};
    case 1310:
        return {F::MeshId::PhantasmShell, {.027f, -.025f, .0016f}, {.043f, .050f, .016f},
                {.045f, .015f, -.032f}, -25.0f};
    default:
        return {};
    }
}
// A rub's frame on the card: its origin and its own right / up (the card's turned by its roll),
// shared by BuildHeld (what you see) and WipeSample (what rubs). held.rub_* nudge it live.
struct RubFrame {
    Q::V3 origin, right, up;
};
RubFrame RubFrameOf(const RubLook& rub, Q::V3 card, Q::V3 right, Q::V3 up, Q::V3 n, float k) {
    const float roll = (rub.roll_deg + VR::Tune::Get("held.rub_roll", 0.0f)) * 0.0174533f;
    const float x = rub.pinch[0] + VR::Tune::Get("held.rub_x", 0.0f);
    const float y = rub.pinch[1] + VR::Tune::Get("held.rub_y", 0.0f);
    const float z = rub.pinch[2] + VR::Tune::Get("held.rub_z", 0.0f);
    const float c = std::cos(roll), s = std::sin(roll);
    return {card + (right * x + up * y + n * z) * k, right * c + up * s, up * c - right * s};
}
void AddRub(F::List& list, const RubLook& rub, Q::V3 origin, Q::V3 x, Q::V3 y, Q::V3 z,
            float scale, const std::array<float, 4>& glow = {}) {
    F::Item item = MakeItem(rub.mesh, origin, x, y, z, scale, scale, scale);
    item.shade = F::Shade::Goods;
    item.color = {1.0f, 1.0f, 1.0f, 1.0f};
    item.emissive = {glow[0], glow[1], glow[2], 0.0f};
    list.Add(item);
}

// Our own held models (vr_goods_meshes.h) for goods the game shows none of: up to two parts
// placed together (by HeldLookOf), Shade::Goods unless a liquid's. A liquid part is the Blood
// Vial's glass (Shade::Syringe), its chamber (mesh metres) given here.
struct GoodsPart {
    F::MeshId mesh{F::MeshId::Count};
    std::array<float, 3> tint{1.0f, 1.0f, 1.0f};
};
struct GoodsLook {
    std::array<GoodsPart, 2> parts{};
    int count{0};
    int liquid{-1};                          // the part drawn as the vial's glass, -1 none
    float bottom{}, top{}, radius{}, fill{}; // its chamber
    bool potion{false};                      // gold blood and bubbles (vr_inframe.frag)
    float longest{0.10f};                    // metres (the hold ring)
    std::array<float, 3> lo{}, hi{};         // its parts' bounds (mesh metres): the belt's holder
};
GoodsLook GoodsLookOf(s32 goods) {
    using M = F::MeshId;
    // The tubes' chamber is the vial's (vr_syringe_mesh.h); the bottle's its body.
    constexpr float TubeBottom = 0.0352f, TubeTop = 0.1408f, TubeRadius = 0.0146f;
    const auto reused = [](M mesh, std::array<float,3> lo, std::array<float,3> hi,
                           std::array<float,3> tint = {1,1,1}) {
        GoodsLook result{};
        result.parts[0] = {mesh,tint}; result.count = 1;
        result.lo = lo; result.hi = hi;
        result.longest = std::max({hi[0]-lo[0],hi[1]-lo[1],hi[2]-lo[2]});
        return result;
    };
    switch (goods) {
    case 100:
    case 1400: return reused(M::HunterMark,{-.015f,-.030f,-.025f},{.095f,.040f,.012f});
    case 130: return {{{{M::RegisterBook}}}, 1, -1, 0, 0, 0, 0, false, .12f,
                      {-.048f,-.060f,-.015f},{.047f,.060f,.012f}};
    case 131: return {{{{M::LeagueBook}}}, 1, -1, 0, 0, 0, 0, false, .12f,
                      {-.048f,-.060f,-.015f},{.047f,.060f,.012f}};
    case 200: return reused(M::BeckoningBell,{-.024f,-.001f,-.024f},{.029f,.081f,.024f},{.65f,1.45f,1.35f});
    case 2050: return reused(M::ChoirBell,{-.024f,-.001f,-.024f},{.029f,.081f,.024f},{1.8f,1.8f,1.9f});
    case 1110: return reused(M::BeastPellet,{-.015f,-.015f,-.015f},{.015f,.015f,.015f});
    case 1330: return reused(M::AshUrn,{-.022f,-.027f,-.017f},{.022f,.034f,.017f});
    case 2000: return reused(M::PhantasmShell,{-.016f,-.075f,-.015f},{.070f,.025f,.020f},{.75f,.82f,.65f});
    case 2110: return reused(M::Skull,{-.073f,-.064f,-.111f},{.073f,.105f,.111f},{.70f,.70f,.68f});
    case 4320:
    case 4321:
    case 4322:
    case 4323: return reused(M::UmbilicalCord,{-.016f,-.075f,-.015f},{.070f,.025f,.020f});
    case 701: // Blood of Arianna: velvet tied over the tube
        return {{{{M::BloodTube}, {M::BloodClothVelvet}}}, 2, 0, TubeBottom, TubeTop, TubeRadius,
                0.8f, false, 0.19f, {-.029f, .012f, -.027f}, {.028f, .170f, .029f}};
    case 702: // Blood of Adella: linen
        return {{{{M::BloodTube}, {M::BloodClothLinen}}}, 2, 0, TubeBottom, TubeTop, TubeRadius,
                0.8f, false, 0.19f, {-.028f, .012f, -.026f}, {.027f, .170f, .028f}};
    case 706: // Blood of Adeline: a steel pull stopper
        return {{{{M::BloodTubeRing}}}, 1, 0, TubeBottom, TubeTop, TubeRadius, 0.8f, false, 0.19f,
                {-.018f, .012f, -.017f}, {.018f, .203f, .017f}};
    case 703: // Iosefka's Blood Vial: the open bottle and its label
        return {{{{M::IosefkaBottle}, {M::IosefkaLabel}}}, 2, 0, -0.056f, 0.016f, 0.0222f, 0.72f,
                true, 0.13f, {-.026f, -.062f, -.026f}, {.026f, .067f, .026f}};
    case 205: // Small Resonant Bell
        return {{{{M::ResonantBell}}}, 1, -1, 0, 0, 0, 0, false, 0.11f, {-.024f, -.001f, -.024f},
                {.029f, .081f, .024f}};
    case 225: // Sinister Resonant Bell: the same bell, blood-darkened
        return {{{{M::ResonantBell, {1.0f, 0.55f, 0.5f}}}}, 1, -1, 0, 0, 0, 0, false, 0.11f,
                {-.024f, -.001f, -.024f}, {.029f, .081f, .024f}};
    case 1500: // Madman's Knowledge, Great One's Wisdom
    case 1501:
        return {{{{M::Skull}}}, 1, -1, 0, 0, 0, 0, false, 0.22f, {-.073f, -.064f, -.111f},
                {.073f, .105f, .111f}};
    case 1100: // Antidote
        return {{{{M::Antidote}}}, 1, -1, 0, 0, 0, 0, false, 0.08f, {-.040f, -.001f, -.040f},
                {.040f, .014f, .040f}};
    case 120: // Notebook
        return {{{{M::Notebook}}}, 1, -1, 0, 0, 0, 0, false, 0.12f, {-.048f, -.060f, -.015f},
                {.047f, .060f, .012f}};
    default:
        break;
    }
    if (goods == 704 || goods == 705 || (goods >= 1510 && goods <= 1519) || (goods >= 1590 && goods <= 1594)) { // the coldbloods
        return {{{{M::Coldblood}}}, 1, -1, 0, 0, 0, 0, false, 0.06f, {-.031f, -.012f, -.031f},
                {.031f, .030f, .031f}};
    }
    return {};
}
// Use the same carried vessel as the renderer, in tracking metres. Head turns and walking
// through the game do not become fake liquid impulses; recentering changes the space serial.
PiLiquidOutput LiquidSurface(uint32_t slot, s32 goods, const Mapping& map,
                             const std::array<float, 12>& mv, float centre_y, s64 now, float radius,
                             float half_height, float fill, bool goods_response = false) {
    InitVialVisualDll();
    PiNativeVialVisualInput input{};
    input.operation = PI_VV_LIQUID;
    input.now_ns = now;
    auto& pose = input.liquid;
    pose.slot = slot;
    pose.goods = goods;
    pose.space = map.space_serial;
    pose.valid = 1;
    pose.response = goods_response ? PI_LIQUID_GOODS_RESPONSE : PI_LIQUID_DEFAULT_RESPONSE;
    pose.radius = radius;
    pose.half_height = half_height;
    pose.fill = fill;
    const Q::V3 centre{mv[3] + centre_y * mv[1], mv[7] + centre_y * mv[5],
                       mv[11] + centre_y * mv[9]};
    const auto tracking = [&](Q::V3 view) {
        return map.ToTracking(map.Carried(map.ViewToWorld(view)));
    };
    const Q::V3 origin = tracking(centre);
    pose.position[0] = origin.x;
    pose.position[1] = origin.y;
    pose.position[2] = origin.z;
    for (int a = 0; a < 3; ++a) {
        const Q::V3 column{mv[a], mv[4 + a], mv[8 + a]};
        const Q::V3 direction = tracking(centre + column) - origin;
        pose.valid = pose.valid && Q::Length(direction) > 1e-5f;
        const Q::V3 axis = Q::Normalize(direction);
        pose.axes[a][0] = axis.x;
        pose.axes[a][1] = axis.y;
        pose.axes[a][2] = axis.z;
    }
    const auto out = g_vial_visual.Call(input);
    if (!out.applied) {
        PiLiquidOutput quiet{};
        quiet.normal[1] = 1;
        return quiet;
    }
    return out.liquid;
}

// Papers and the Empty Phantasm Shell follow the hand, even away from the eye.
// WipeSample uses this same frame for contact, in tracking rather than view space.
template <size_t N>
Q::ItemFrame HandRubFrame(const std::array<float, N>& m, const Q::HeldLook& look) {
    return Q::ItemFrameOf(m, look, Q::GetQuickSettings().held_scale);
}

void BuildHeld(F::List& list, const Mapping& map, const std::array<float, 12>& m,
               const Q::Scene& scene, const Q::Snapshot& snap, s64 now, float scale_m) {
    const s32 goods = scene.held_goods;
    const Q::HeldLook look = TunedLook(goods);
    if (VR::GripPreview::Enabled() && (look.model <= 0 || VR::Assets::GetModel(look.model))) {
        static s32 logged_goods = -1;
        if (logged_goods != goods) {
            logged_goods = goods;
            LOG_INFO(Core, "VR grip preview: goods={} name={} grip={} model={} scale={} "
                           "hold={} pitch={} yaw={} roll={} lift={} out={} side={}",
                     goods, VR::Assets::GoodsName(goods), look.grip, look.model, look.scale,
                     look.hold_y, look.pitch_deg, look.yaw_deg, look.roll_deg,
                     look.lift_m, look.out_m, look.side_m);
        }
    }
    const float k = look.scale * Q::GetQuickSettings().held_scale;
    // The dummy's axes (world, unit) and the grip's centre in its frame.
    const VR::Body::Vec3 dx = VR::Body::Normalize(VR::Body::Vec3{m[0], m[4], m[8]});
    const VR::Body::Vec3 dy = VR::Body::Normalize(VR::Body::Vec3{m[1], m[5], m[9]});
    const VR::Body::Vec3 dz = VR::Body::Normalize(VR::Body::Vec3{m[2], m[6], m[10]});
    const VR::Body::Vec3 origin{m[3], m[7], m[11]};
    const VR::Body::Vec3 centre_d{VR::Tune::Get("held.cx", Q::GripCentre[0]) + look.side_m,
                                  VR::Tune::Get("held.cy", Q::GripCentre[1]) + look.lift_m,
                                  VR::Tune::Get("held.cz", Q::GripCentre[2]) + look.out_m};
    const auto to_world_dir = [&](VR::Body::Vec3 d) { return dx * d.x + dy * d.y + dz * d.z; };
    // The item's turn in the fist: about its own axis (yaw), then leaning (roll, pitch).
    const float deg = 3.14159265f / 180.0f;
    const VR::Body::Quat turn = VR::Body::AxisAngle({1.0f, 0.0f, 0.0f}, look.pitch_deg * deg) *
                                VR::Body::AxisAngle({0.0f, 0.0f, 1.0f}, look.roll_deg * deg) *
                                VR::Body::AxisAngle({0.0f, 1.0f, 0.0f}, look.yaw_deg * deg);
    const std::array<VR::Body::Vec3, 3> axes{VR::Body::Rotate(turn, {1.0f, 0.0f, 0.0f}),
                                             VR::Body::Rotate(turn, {0.0f, 1.0f, 0.0f}),
                                             VR::Body::Rotate(turn, {0.0f, 0.0f, 1.0f})};
    const std::array<float, 4> glow = GrabGlow(scene, now);
    if (goods == 1300 || goods == 1310 || goods == 1320) {
        if (g_held_logged != goods) {
            g_held_logged = goods;
            LOG_INFO(Core, "VR quick: held {} = hand-attached blade-rub mesh", goods);
        }
        const auto f = HandRubFrame(m, look);
        AddRub(list, RubLookOf(goods), map.WorldToView(f.origin), map.DirToView(f.x),
               map.DirToView(f.y), map.DirToView(f.z), k, glow);
        const auto carried = map.Carried(f.origin);
        g_held_place.Store({now, goods, {carried.x,carried.y,carried.z},
                           {carried.x,carried.y,carried.z}, 0});
        AddHoldRing(list, map, map.WorldToView(f.origin), .06f, scene, now, true);
        return;
    }
    const bool have_model = look.model > 0 && VR::Assets::GetModel(look.model) != nullptr;
    if (const GoodsLook goods_look = GoodsLookOf(goods);
        !have_model && goods_look.count > 0 && VR::Tune::Get("held.goods_models", 1.0f) >= 0.5f) {
        // Our model: placed as a game model is (its point (0, hold_y, 0) at the grip's centre).
        const VR::Body::Vec3 hold_d = VR::Body::Rotate(turn, {look.hold_x * k, look.hold_y * k, look.hold_z * k});
        const VR::Body::Vec3 at = origin + to_world_dir(centre_d - hold_d);
        const Q::V3 c = map.WorldToView(at);
        std::array<Q::V3, 3> cols{};
        for (int a = 0; a < 3; ++a) {
            cols[static_cast<size_t>(a)] =
                map.DirToView(to_world_dir(axes[static_cast<size_t>(a)])) * k;
        }
        F::Item item{};
        item.mv = {cols[0].x, cols[1].x, cols[2].x, c.x,       cols[0].y, cols[1].y,
                   cols[2].y, c.y,       cols[0].z, cols[1].z, cols[2].z, c.z};
        for (int i = 0; i < goods_look.count; ++i) {
            const GoodsPart& part = goods_look.parts[static_cast<size_t>(i)];
            item.mesh = part.mesh;
            item.tex = F::Tex::None;
            if (i == goods_look.liquid) {
                // The Blood Vial's glass and sloshing liquid (Shade::Syringe): world up in the
                // mesh's axes drives the surface; its plane keeps the volume.
                const float half = 0.5f * (goods_look.top - goods_look.bottom);
                const auto surface = LiquidSurface(PI_LIQUID_GOODS, goods, map, item.mv,
                                                   0.5f * (goods_look.bottom + goods_look.top), now,
                                                   goods_look.radius, half, goods_look.fill);
                const auto& normal = surface.normal;
                const float distance = surface.distance;
                item.shade = F::Shade::Syringe;
                item.color = {normal[0], normal[1], normal[2], distance};
                item.uv = {goods_look.bottom, goods_look.top, goods_look.radius, goods_look.fill};
                // A potion's bubbles loop over 4 s (its emissive.z); the grip's mesh y for the
                // hand's light.
                const float phase = goods_look.potion
                                        ? static_cast<float>(now % 4'000'000'000LL) / 4.0e9f
                                        : 0.0f;
                item.emissive = {0.0f, 0.0f, phase, look.hold_y};
            } else {
                item.shade = F::Shade::Goods;
                item.color = {part.tint[0], part.tint[1], part.tint[2], 1.0f};
                item.uv = {goods == Q::QueenlyBlood ? 1.0f : 0.0f, 0.0f, 1.0f, 1.0f};
                item.emissive = {glow[0], glow[1], glow[2], 0.0f};
            }
            list.Add(item);
        }
        // Where it is (the use's sound comes from there), and the hold ring round it.
        const VR::Body::Vec3 at_c = map.Carried(at);
        g_held_place.Store({now, goods, {at_c.x, at_c.y, at_c.z}, {at_c.x, at_c.y, at_c.z},
                            VR::Body::Length(at_c - at) / std::max(map.scale, 1e-3f)});
        AddHoldRing(list, map, c, std::clamp(0.75f * goods_look.longest * k, 0.06f, 0.14f), scene,
                    now, true);
        if (g_log >= 1 && g_held_logged != goods) {
            g_held_logged = goods;
            LOG_INFO(Core, "VR quick: held {} = our model (mesh {}, {} parts, grip {})", goods,
                     static_cast<int>(goods_look.parts[0].mesh), goods_look.count, look.grip);
        }
        return;
    }
    if (have_model) {
        // Model space -> dummy: centre + turn * (k * (p - hold)); -> world -> the camera's view.
        const VR::Body::Vec3 hold_d = VR::Body::Rotate(turn, {look.hold_x * k, look.hold_y * k, look.hold_z * k});
        const VR::Body::Vec3 at = origin + to_world_dir(centre_d - hold_d);
        F::Item item{};
        item.mesh = F::MeshId::Model;
        item.tex = F::Tex::Model;
        item.tex_key = static_cast<u32>(look.model);
        item.shade = F::Shade::Model;
        const Q::V3 c = map.WorldToView(at);
        std::array<Q::V3, 3> cols{};
        for (int a = 0; a < 3; ++a) {
            cols[static_cast<size_t>(a)] =
                map.DirToView(to_world_dir(axes[static_cast<size_t>(a)])) * k;
        }
        item.mv = {cols[0].x, cols[1].x, cols[2].x, c.x,       cols[0].y, cols[1].y,
                   cols[2].y, c.y,       cols[0].z, cols[1].z, cols[2].z, c.z};
        item.color = {1.0f, 1.0f, 1.0f, 1.0f};
        item.emissive = glow;
        item.emissive[3] = look.hold_y; // the shader lights it like the hand there
        // Cosmetic bottle replacement; original bounds, grip transform and throw origin stay intact.
        if (look.model == 10010 && VR::Tune::Get("held.molotov_look", 1.0f) >= 0.5f) {
            item.shade = F::Shade::Molotov;
            item.mesh = F::MeshId::VictorianMolotov;
            item.tex = F::Tex::None;
        }
        // Native bottle silhouettes and approved attachments are unchanged. Only exposed
        // glass panels use the vial liquid; the shader keeps labels, caps and cloth opaque.
        const int liquid_profile = goods == 1120 && look.model == 10017 ? 1 :
                                   goods == 2030 && look.model == 10023 ? 2 :
                                   goods == 1260 && look.model == 10015 ? 3 : 0;
        if (liquid_profile && VR::Tune::Get("held.native_liquid", 1.0f) >= 0.5f) {
            // Inscribed chambers in the original meshes, before the approved item scale.
            const std::array<float, 4> chamber = liquid_profile == 1 ?
                std::array<float, 4>{-0.112f, 0.046f, 0.047f, 0.68f} : liquid_profile == 2 ?
                std::array<float, 4>{-0.049f, 0.016f, 0.033f, 0.48f} :
                std::array<float, 4>{-0.100f, 0.014f, 0.032f, 0.76f};
            const auto surface = LiquidSurface(PI_LIQUID_GOODS, goods, map, item.mv,
                                               0.5f * (chamber[0] + chamber[1]), now, chamber[2],
                                               0.5f * (chamber[1] - chamber[0]), chamber[3], true);
            const auto& normal = surface.normal;
            const float distance = surface.distance;
            item.shade = F::Shade::NativeLiquid;
            item.color = {normal[0], normal[1], normal[2], distance};
            item.uv = chamber;
            item.emissive = {0.0f, 0.0f, static_cast<float>(liquid_profile), look.hold_y};
        }
        list.Add(item);
        if (const auto model = VR::Assets::GetModel(look.model)) {
            const VR::Body::Vec3 mid{0.5f * (model->min[0] + model->max[0]),
                                     0.5f * (model->min[1] + model->max[1]),
                                     0.5f * (model->min[2] + model->max[2])};
            const VR::Body::Vec3 mid_d = centre_d + VR::Body::Rotate(turn, mid * k) - hold_d;
            const VR::Body::Vec3 w = origin + to_world_dir(mid_d);
            // (Where it is drawn: carried by the hand sync, the throw starts there.)
            const VR::Body::Vec3 at_c = map.Carried(at);
            const VR::Body::Vec3 w_c = map.Carried(w);
            g_held_place.Store({now, goods, {at_c.x, at_c.y, at_c.z}, {w_c.x, w_c.y, w_c.z},
                                VR::Body::Length(at_c - at) / std::max(map.scale, 1e-3f)});
            // quick_hold_use: the ring round the model (its longest side, within 6 .. 14 cm).
            const float longest =
                k * std::max({model->max[0] - model->min[0], model->max[1] - model->min[1],
                              model->max[2] - model->min[2]});
            AddHoldRing(list, map, map.WorldToView(w), std::clamp(0.75f * longest, 0.06f, 0.14f),
                        scene, now, true);
            if (g_log >= 1 && g_held_logged != goods) {
                // How big it looks: its longest side at its distance from the eye.
                g_held_logged = goods;
                const Q::V3 v = map.WorldToView(w);
                const float distance = Q::Length(v) / std::max(scale_m, 1e-3f);
                const float side =
                    k * std::max({model->max[0] - model->min[0], model->max[1] - model->min[1],
                                  model->max[2] - model->min[2]});
                LOG_INFO(Core,
                         "VR quick: held {} = model s{:05} ({} grip), {:.3f} m long, {:.2f} m "
                         "from the eye: {:.1f} deg",
                         goods, look.model, look.grip, side, distance,
                         2.0f * std::atan(0.5f * side / std::max(distance, 0.05f)) * 57.29578f);
            }
        }
        return;
    }
    const Q::GoodsClass cls = Q::Classify(goods);
    s32 icon = -1;
    if (VR::GripPreview::Enabled()) {
        icon = LookupGoods(goods).icon;
    }
    if (scene.held_slot >= 0 && scene.held_slot < Q::SlotCount) {
        icon = snap.slot[static_cast<size_t>(scene.held_slot)].icon;
    }
    const RubLook rub = RubLookOf(goods);
    if (rub.mesh != F::MeshId::Count ||
        (look.model < 0 && icon >= 0 && Q::GetQuickSettings().icons)) {
        // The item's card (its icon on the arc's slate), pinched between the thumb and the
        // fingers, out of the top of the fist and turned to the eye.
        const float size = 0.09f * k;
        const float lift = VR::Tune::Get("held.card_lift", 0.70f) * size;
        const VR::Body::Vec3 at_world =
            origin + to_world_dir(centre_d + VR::Body::Vec3{0.0f, lift, 0.0f});
        Q::V3 c = map.WorldToView(at_world);
        const Q::V3 n = Q::Normalize(Q::V3{0.0f, 0.0f, 0.0f} - c, {0.0f, 0.0f, -1.0f});
        // In front of the knuckles (the fist would hide it), as if pinched by the thumb.
        c = c + n * (VR::Tune::Get("held.card_front", 0.045f) * scale_m);
        Q::V3 up = Q::Normalize(map.DirToView(dy), {0.0f, 1.0f, 0.0f});
        up = Q::Normalize(up - n * Q::Dot(up, n), {0.0f, 1.0f, 0.0f});
        const Q::V3 right = Q::Cross(n, up);
        if (rub.mesh != F::MeshId::Count) {
            // A blade rub: pinched by its left end, the rest reaching right (WipeSample measures
            // the same rectangle).
            const float k_rub = size / 0.09f;
            const RubFrame f = RubFrameOf(rub, c, right, up, n, k_rub);
            AddRub(list, rub, f.origin, f.right, f.up, n, k_rub, glow);
            const Q::V3 mid =
                f.origin + (f.right * rub.mid[0] + f.up * rub.mid[1] + n * rub.mid[2]) * k_rub;
            AddHoldRing(list, map, mid, std::max(rub.half[0], rub.half[1]) * k_rub * 1.2f, scene, now,
                        true);
            return;
        }
        const ArcStyle st{};
        F::Item plate = MakeItem(F::MeshId::Quad, c, right, up, n, size, size, 1.0f);
        plate.shade = F::Shade::Card;
        plate.color = st.slate;
        plate.color[3] = 0.85f;
        plate.emissive = st.frame;
        plate.uv = {std::min(glow[0] * 1.5f, 1.0f), 0.0f, 0.0f, 0.0f}; // the grab's candlelight
        list.Add(plate);
        F::Item card = MakeItem(F::MeshId::Quad, c + n * (0.003f * scale_m), right, up, n,
                                size * st.icon_scale, size * st.icon_scale, 1.0f);
        card.shade = F::Shade::Model;
        card.tex = F::Tex::Icon;
        card.tex_key = static_cast<u32>(icon);
        card.uv = {0.0f, 0.0f, 400.0f / 416.0f, 400.0f / 416.0f};
        card.color = {1.0f, 1.0f, 1.0f, 1.0f};
        card.emissive = glow;
        card.emissive[3] = -0.55f / st.icon_scale; // the card's bottom edge, in the fist
        list.Add(card);
        // quick_hold_use: the ring round the card (in the card's own units).
        AddHoldRing(list, map, c, size, scene, now, true);
        if (g_log >= 1 && g_held_logged != goods) {
            g_held_logged = goods;
            const float distance = Q::Length(c) / std::max(scale_m, 1e-3f);
            LOG_INFO(Core,
                     "VR quick: held {} = its card (icon {}), {:.3f} m wide, {:.2f} m from the "
                     "eye: {:.1f} deg",
                     goods, icon, size, distance,
                     2.0f * std::atan(0.5f * size / std::max(distance, 0.05f)) * 57.29578f);
        }
        return;
    }
    // Our plain mesh (the model is still loading, or missing), in the game's palette.
    const float size = scale_m * k;
    const VR::Body::Vec3 at = origin + to_world_dir(centre_d);
    const Q::V3 c = map.WorldToView(at);
    const Q::V3 x = Q::Normalize(map.DirToView(dx));
    const Q::V3 y = Q::Normalize(map.DirToView(dy));
    const Q::V3 z = Q::Normalize(map.DirToView(dz));
    const auto mesh =
        static_cast<F::MeshId>(static_cast<u16>(F::MeshId::Bottle) + static_cast<u16>(cls.mesh) -
                               static_cast<u16>(Q::MeshKind::Bottle));
    F::Item item = MakeItem(cls.mesh == Q::MeshKind::Token ? F::MeshId::Token : mesh, c, x, y, z,
                            size, size, size);
    item.shade = F::Shade::Lit;
    item.color = Linear(0.16f, 0.13f, 0.10f); // dark umber glass and clay
    item.emissive = glow;
    list.Add(item);
    AddHoldRing(list, map, c, 0.08f * size, scene, now, true); // quick_hold_use
}

// vial_inject_drain (core/vr/vial_drain.h): the armed vial and the injection's drain. While the
// update says so (HeldVialOurs) the vial in the hunter's hand is ours: the game's own model s10002
// where FFX 173 puts it on the palm dummy (its origin at the fist's channel, GripCentre, its axes
// the dummy's, life size: its silhouette within a pixel or two of the game's in the eye dumps),
// shaded as the held items are (vr_inframe.frag ShadeVialDrain) with its blood level from the
// drain word that update saw (0: full). VR_TUNE_FILE keys drain.* move, turn, size and
// colour it in a test run (drain.fill freezes the level).
constexpr s32 DrainModel = 10002;
// The blood's column (fractions of the model's height from its bottom): the glass between the
// needle's end and the cap.
constexpr float DrainColumnLo = 0.20f;
constexpr float DrainColumnHi = 0.80f;
// The blood (linear): a dark venous red, never bright; it takes the hand's light like the glass.
constexpr std::array<float, 3> DrainBlood{0.05f, 0.004f, 0.005f};
constexpr float DrainBloodOpacity = 0.88f;
constexpr float DrainFilm = 0.30f; // the red film's strength on the drained glass
// Depth-tested a millimetre nearer: drawn over the game's vial if it lingers a frame at the
// hand-over, still under the fingers round it.
constexpr float DrainPull = 0.001f;
struct DrainLog {
    u64 word{0};
    int stage{0}; // 1 first frame, 2 half, 3 empty
};
DrainLog g_drain_log{};

// The vial model's rows (view <- its mesh, metres) on the palm dummy `m` (its world rows), where
// FFX 173 puts the game's own vial; `c` (out): its origin in view.
// `look`: drawn (true) or for the injection's contact (false). The contact keeps the tuned
// place; the drawn vial sits 4 mm further up the fist's channel and 5 mm off the palm (the user,
// 2026-10-12: the relaxed fingers rest on its glass, more of its blood shows), its needle with it.
std::array<float, 12> VialRows(const Mapping& map, const std::array<float, 12>& m, Q::V3& c,
                               bool look = true) {
    const float k = VR::Tune::Get("drain.scale", 1.0f);
    const VR::Body::Vec3 dx = VR::Body::Normalize(VR::Body::Vec3{m[0], m[4], m[8]});
    const VR::Body::Vec3 dy = VR::Body::Normalize(VR::Body::Vec3{m[1], m[5], m[9]});
    const VR::Body::Vec3 dz = VR::Body::Normalize(VR::Body::Vec3{m[2], m[6], m[10]});
    const VR::Body::Vec3 origin{m[3], m[7], m[11]};
    const auto to_world_dir = [&](VR::Body::Vec3 d) { return dx * d.x + dy * d.y + dz * d.z; };
    const VR::Body::Vec3 centre_d{
        VR::Tune::Get("drain.cx", Q::GripCentre[0]),
        VR::Tune::Get("drain.cy", Q::GripCentre[1] + 0.012f) +
            (look ? VR::Tune::Get("drain.look_up", 0.016f) : 0.0f),
        VR::Tune::Get("drain.cz", Q::GripCentre[2]) +
            (look ? VR::Tune::Get("drain.look_out", 0.005f) : 0.0f)};
    const float deg = 3.14159265f / 180.0f;
    const VR::Body::Quat turn =
        VR::Body::AxisAngle({1.0f, 0.0f, 0.0f}, VR::Tune::Get("drain.rx", 0.0f) * deg) *
        VR::Body::AxisAngle({0.0f, 0.0f, 1.0f}, VR::Tune::Get("drain.rz", 0.0f) * deg) *
        VR::Body::AxisAngle({0.0f, 1.0f, 0.0f}, VR::Tune::Get("drain.ry", 0.0f) * deg);
    const VR::Body::Vec3 at = origin + to_world_dir(centre_d);
    c = map.WorldToView(at);
    std::array<Q::V3, 3> cols{};
    for (int a = 0; a < 3; ++a) {
        VR::Body::Vec3 axis{};
        (a == 0 ? axis.x : a == 1 ? axis.y : axis.z) = 1.0f;
        cols[static_cast<size_t>(a)] =
            map.DirToView(to_world_dir(VR::Body::Rotate(turn, axis))) * k;
    }
    return {cols[0].x, cols[1].x, cols[2].x, c.x,       cols[0].y, cols[1].y,
            cols[2].y, c.y,       cols[0].z, cols[1].z, cols[2].z, c.z};
}

// vial_needle (core/vr/vial_needle.h): the needle and its hub on the vial whose rows are `vial`
// (its model `model`), in the same list as the vial, so they are carried with it
// (inframe_hand_sync) and drawn with the same pose, depth and disparity. The stabbing end and
// the axis come from the user's own model (NeedleRootOf, once per model). VR_TUNE_FILE keys
// (tests): needle.on (0 hides it), needle.len / needle.dia / needle.hub_len / needle.hub_dia /
// needle.inset (mm), needle.end (-1 the bottom, +1 the top), needle.r/g/b and needle.spec /
// needle.sharp / needle.px / needle.tint (the shaft), hub.r/g/b and hub.spec / hub.sharp /
// hub.tint (the hub).
struct NeedleCache {
    const VR::Assets::Model* model{nullptr};
    float end{0.0f};
    VR::Held::NeedleRoot root{};
    bool logged{false};
};
NeedleCache g_needle{};

// The needle's steel and the hub's (linear), their highlights (strength, sharpness / 100). The
// steel is a pale, neutral grey: a polished needle is lighter than the dark glass and cap it
// comes out of, so it reads against the clinic's dark floors as well as against his sleeve. (The
// mock, the needle at arm's length over the clinic's floor: 2.6 x the contrast of the first dark
// steel 0.085, its line's brightness even along it, CV 0.11 against 0.29.)
constexpr std::array<float, 3> NeedleSteel{0.27f, 0.27f, 0.28f};
// How much of the colour of the light round the hand (HeldLight's tint) the metal takes; its
// brightness it always takes. That light is mostly his skin, his sleeve and the vial's blood:
// with all of it (and the cooler steel 0.26 / 0.27 / 0.30 to offset it) the needle read pink in
// the close-ups (R / G 1.13 in the eye image beside his warm white shirt's 1.04; 0.3: 1.04).
constexpr float SteelTint = 0.3f;
constexpr float NeedleSpec = 0.55f;
constexpr float NeedleSharp = 0.40f;
constexpr std::array<float, 3> HubMetal{0.070f, 0.062f, 0.052f};
constexpr float HubSpec = 0.35f;
constexpr float HubSharp = 0.25f;

std::array<float, 3> AddVialNeedle(F::List& list, const std::array<float, 12>& vial,
                                   const VR::Assets::Model& model, s64 now, bool draw) {
    namespace H = VR::Held;
    H::NeedleLook look{};
    // Needle length is cosmetic; contact is at the end of the hub.
    if (VR::Tune::Get("drain.look", 1.0f) >= 0.5f) {
        look.length_m = H::AntiqueNeedleLength;
    }
    look.end = VR::Tune::Get("needle.end", -1.0f) < 0.0f ? -1.0f : 1.0f;
    look.length_m = VR::Tune::Get("needle.len", look.length_m * 1000.0f) * 0.001f;
    look.diameter_m = VR::Tune::Get("needle.dia", look.diameter_m * 1000.0f) * 0.001f;
    look.hub_length_m = VR::Tune::Get("needle.hub_len", look.hub_length_m * 1000.0f) * 0.001f;
    look.hub_diameter_m = VR::Tune::Get("needle.hub_dia", look.hub_diameter_m * 1000.0f) * 0.001f;
    look.inset_m = VR::Tune::Get("needle.inset", look.inset_m * 1000.0f) * 0.001f;
    if (g_needle.model != &model || g_needle.end != look.end) {
        g_needle.model = &model;
        g_needle.end = look.end;
        g_needle.root = H::NeedleRootOf(model.vertices.empty() ? nullptr
                                                               : model.vertices[0].position,
                                        model.vertices.size(),
                                        sizeof(VR::Assets::ModelVertex) / sizeof(float),
                                        {model.min[0], model.min[1], model.min[2]},
                                        {model.max[0], model.max[1], model.max[2]}, look.end);
        g_needle.logged = false;
    }
    const H::NeedleRoot& root = g_needle.root;
    if (!root.valid) {
        return H::RowsPoint(vial, {root.x, model.min[1], root.z});
    }
    const auto part_item = [&](H::NeedlePart part) {
        const bool hub = part == H::NeedlePart::Hub;
        F::Item item{};
        item.mesh = hub ? F::MeshId::NeedleHub : F::MeshId::Needle;
        item.shade = F::Shade::Steel;
        item.mv = H::NeedlePartRows(vial, root, look, part);
        const auto fist = H::VialPointInPart(vial, item.mv, {0.0f, 0.0f, 0.0f});
        // (The keys spelled out: no string is built per frame.)
        const auto& base = hub ? HubMetal : NeedleSteel;
        item.color = {VR::Tune::Get(hub ? "hub.r" : "needle.r", base[0]),
                      VR::Tune::Get(hub ? "hub.g" : "needle.g", base[1]),
                      VR::Tune::Get(hub ? "hub.b" : "needle.b", base[2]), 1.0f};
        item.emissive = {fist[0], fist[1], fist[2],
                         VR::Tune::Get(hub ? "hub.spec" : "needle.spec",
                                       hub ? HubSpec : NeedleSpec)};
        // uv.x > 0: the needle is drawn as an anti-aliased wire (vr_inframe.vert); uv.z: how
        // much of the colour of the hand's light it takes.
        item.uv = {hub ? 0.0f : VR::Tune::Get("needle.px", H::NeedleMinHalfPx), 0.0f,
                   VR::Tune::Get(hub ? "hub.tint" : "needle.tint", SteelTint),
                   VR::Tune::Get(hub ? "hub.sharp" : "needle.sharp",
                                 hub ? HubSharp : NeedleSharp)};
        item.depth_pull = VR::Tune::Get("drain.pull", DrainPull);
        return item;
    };
    const F::Item hub = part_item(H::NeedlePart::Hub);
    const F::Item needle = part_item(H::NeedlePart::Needle);
    if (draw) { list.Add(hub); list.Add(needle); }
    if (draw && !g_needle.logged) {
        g_needle.logged = true;
        const auto tip = H::NeedlePoint(needle.mv);
        LOG_INFO(Core,
                 "VR held: needle on the vial (s{:05}: {} end at y {:.4f}, its face {:.1f} mm "
                 "across, {:.1f} / {:.1f} / {:.1f} mm across 2 / 5 / 10 mm in, the axis at x "
                 "{:.4f} z {:.4f}, {:.1f} cm long): a {:.1f} mm needle {:.1f} cm past a {:.1f} x "
                 "{:.1f} mm hub, its root {:.1f} mm in (flush); its point at view "
                 "({:.3f},{:.3f},{:.3f}) | t {:.3f}",
                 model.id, look.end < 0.0f ? "bottom" : "top", root.y, 2000.0f * root.face_r,
                 2000.0f * root.profile[3], 2000.0f * root.profile[9],
                 2000.0f * root.profile[19], root.x, root.z, 100.0f * root.length,
                 1000.0f * look.diameter_m, 100.0f * look.length_m,
                 1000.0f * look.hub_diameter_m, 1000.0f * look.hub_length_m,
                 1000.0f * (root.InsetFor(0.5f * look.hub_diameter_m) + look.inset_m), tip[0],
                 tip[1], tip[2], static_cast<double>(now) * 1e-9);
    }
    // The contact sits 0.5 cm back from the hub's far end, towards the vial (tune
    // vial.contact_back_mm): at the hub's end it felt a little too close to the needle.
    PiNativeVialVisualInput contact{};
    contact.operation = PI_VV_CONTACT;
    contact.back_m = VR::Tune::Get("vial.contact_back_mm", 5.0f) * 0.001f;
    contact.hub_length_m = look.hub_length_m;
    std::copy(hub.mv.begin(), hub.mv.end(), contact.rows);
    const auto out = g_vial_visual.Call(contact);
    return {out.contact[0], out.contact[1], out.contact[2]};
}

// Whether the needle goes on our vial this camera update (core/vr/vial_needle.h NeedleVisible).
bool NeedleOn(bool ours, bool model, bool menu) {
    PiNativeVialVisualInput in{};
    in.operation = PI_VV_NEEDLE;
    in.setting = VR::GetSettings().vial_needle;
    in.tuned_off = VR::Tune::Get("needle.on", 1.0f) < 0.5f;
    in.ours = ours;
    in.model = model;
    in.menu = menu;
    return g_vial_visual.Call(in).needle_visible != 0;
}

void BuildVialDrain(F::List& list, const Mapping& map, const std::array<float, 12>& m, s64 now,
                    u64 word) {
    const auto model = VR::Assets::GetModel(DrainModel);
    if (!model) {
        return;
    }
    InitVialVisualDll();
    PiNativeVialVisualInput visual{};
    visual.operation = PI_VV_DRAIN;
    visual.now_ns = now;
    visual.word = word;
    visual.variation_lo = VR::Tune::Get("vial.fill_var_lo", -10.0f) * 0.01f;
    visual.variation_hi = VR::Tune::Get("vial.fill_var_hi", 5.0f) * 0.01f;
    visual.override_fill = VR::Tune::Get("drain.fill", -1.0f);
    const auto level = g_vial_visual.Call(visual);
    if (g_vial_visual.Faulted())
        return;
    const float fill = level.fill, shown = level.shown;
    Q::V3 c{};
    F::Item item{};
    item.mesh = F::MeshId::Model;
    item.tex = F::Tex::Model;
    item.tex_key = static_cast<u32>(DrainModel);
    item.shade = F::Shade::VialDrain;
    item.mv = VialRows(map, m, c);
    const float y0 = model->min[1];
    const float y1 = model->max[1];
    VR::Body::BodyStatus body{};
    const bool have_body = VR::Body::LatestBodyStatus(body) && body.driven;
    const float lo = y0 + VR::Tune::Get("drain.lo", DrainColumnLo) * (y1 - y0);
    const float hi = y0 + VR::Tune::Get("drain.hi", DrainColumnHi) * (y1 - y0);
    item.uv = {lo + std::min(shown, 1.0f) * (hi - lo), lo, hi, std::min(shown, 1.0f)};
    item.color = {VR::Tune::Get("drain.br", DrainBlood[0]), VR::Tune::Get("drain.bg", DrainBlood[1]),
                  VR::Tune::Get("drain.bb", DrainBlood[2]),
                  VR::Tune::Get("drain.ba", DrainBloodOpacity)};
    // The axis (mesh x, z) for the film's streaks, the film's strength, the grip's mesh y (the
    // hand's light is read there).
    item.emissive = {0.5f * (model->min[0] + model->max[0]), 0.5f * (model->min[2] + model->max[2]),
                     VR::Tune::Get("drain.film", DrainFilm), 0.0f};
    const auto needle_rows = item.mv;
    // Cosmetic antique syringe look. drain.look=0 restores the original mesh and shader for comparison.
    // Its bounds, attachment, needle, fill word and injection timing are retained.
    if (VR::Tune::Get("drain.look", 1.0f) >= 0.5f) {
        constexpr float bottom = 0.0352f, top = 0.1408f, radius = 0.0146f;
        const float fit = (y1 - y0) / 0.176f;
        const float cx = item.emissive[0], cz = item.emissive[1];
        for (int r = 0; r < 3; ++r) {
            item.mv[4 * r + 3] +=
                item.mv[4 * r] * cx + item.mv[4 * r + 1] * y0 + item.mv[4 * r + 2] * cz;
            for (int a = 0; a < 3; ++a) {
                item.mv[4 * r + a] *= fit;
            }
        }
        // A small air pocket makes motion visible, even with a full dose.
        const auto surface = LiquidSurface(PI_LIQUID_VIAL, -1, map, item.mv, 0.5f * (bottom + top),
                                           now, radius, (top - bottom) * 0.5f, shown * 0.87f);
        const auto& normal = surface.normal;
        const float distance = surface.distance;
        item.mesh = F::MeshId::Syringe;
        item.tex = F::Tex::None;
        item.shade = F::Shade::Syringe;
        item.color = {normal[0], normal[1], normal[2], distance};
        item.uv = {bottom, top, radius, shown};
        item.emissive = {0.0f, 0.0f, item.emissive[2], -y0 / fit};
    }
    item.depth_pull = VR::Tune::Get("drain.pull", DrainPull);
    list.Add(item);
    // The little funnel's far end is the physical syringe tip. Compute it even
    // when the needle is hidden; neither its length nor drain.look affects contact. Drawn on the
    // vial as you see it; the contact from the vial's tuned place (VialRows' `look`).
    AddVialNeedle(list, needle_rows, *model, now, NeedleOn(true,true,false));
    Q::V3 contact_c{};
    const auto tip_view =
        AddVialNeedle(list, VialRows(map, m, contact_c, false), *model, now, false);
    // A rejected contact packet cannot publish a synthetic tip at the origin.
    if (g_vial_visual.Faulted())
        return;
    VR::VialContact::Sample contact{};
    if (have_body && body.injection_body.grip_valid) {
        // Keep the attachment in the controller's local frame. The XR thread
        // applies its current pose each frame instead of reusing a game-rate tip.
        const auto point = map.ToTracking(map.Carried(map.ViewToWorld(
            {tip_view[0], tip_view[1], tip_view[2]})));
        contact.valid = true;
        contact.ns = now;
        contact.space = map.space_serial;
        contact.tip = {point.x, point.y, point.z};
        contact.body = body.injection_body;
        contact.grip_local = true;
        contact.grip_offset = VR::Body::Rotate(VR::Body::Conjugate(
            VR::Body::Normalize(contact.body.grip_q)), contact.tip-contact.body.grip_p);
    }
    if (contact.valid && contact.body.valid) Q::PublishVialContact(contact);
    static const bool contact_audit = std::getenv("VR_INJECT_AUDIT") != nullptr;
    static s64 last_contact_audit = 0;
    if (contact_audit && contact.valid && now-last_contact_audit > 200'000'000) {
        last_contact_audit = now;
        const auto wrist = (contact.body.p[8]+contact.body.p[9])*.5f;
        const auto forearm = (contact.body.p[7]+contact.body.p[8])*.5f;
        const auto chest = (contact.body.p[2]+contact.body.p[3])*.5f;
        LOG_INFO(Core, "VR syringe hub audit: tip ({:.4f},{:.4f},{:.4f}) wrist ({:.4f},{:.4f},{:.4f}) "
                      "forearm ({:.4f},{:.4f},{:.4f}) chest ({:.4f},{:.4f},{:.4f})",
                 contact.tip.x,contact.tip.y,contact.tip.z,wrist.x,wrist.y,wrist.z,
                 forearm.x,forearm.y,forearm.z,chest.x,chest.y,chest.z);
    }
    // One line as a drain starts, at half and when empty (the timing against the THUMP's).
    if (word != g_drain_log.word) {
        g_drain_log = {word, 0};
    }
    // (Not for an empty vial armed with none left: the game's update logs that.)
    const int stage = word == 0 || word == VR::Held::EmptyDrainWord ? 0
                      : fill <= 0.0f                                ? 3
                      : fill <= 0.5f                                ? 2
                                                                    : 1;
    if (stage > g_drain_log.stage) {
        g_drain_log.stage = stage;
        const VR::Held::DrainWord d = VR::Held::UnpackDrain(word);
        LOG_INFO(Core,
                 "VR held: drain: our vial {} (fill {:.2f}, {} ms after the start of a {} ms "
                 "drain) at view ({:.3f},{:.3f},{:.3f}) | t {:.3f}",
                 stage == 1 ? "drawn" : stage == 2 ? "half empty" : "empty", fill,
                 word != 0 ? now / 1'000'000 - d.start_ms : -1, d.drain_ms, c.x, c.y, c.z,
                 static_cast<double>(now) * 1e-9);
    }
}

// VR_SOUND_TRACE[=<n>] (tests): the first n (200) PlaySound calls (the game's TAE sounds, ours,
// the melee's and the gun's) logged with their type, id and position, so a mock run shows what a
// use sounds like. Installed by the first render tick (Game:Main, where the TAE sounds play),
// after every module checked the function's signature at load: a detour over its 16-byte
// prologue (push rbp .. sub rsp, 0x128), replayed by the stub.
std::unique_ptr<Xbyak::CodeGenerator> g_trace_code;
std::atomic<int> g_trace_left{0};

void PS4_SYSV_ABI OnPlaySound(s32 type, s32 id, const float* pos) {
    if (g_trace_left.fetch_sub(1, std::memory_order_relaxed) <= 0) {
        return;
    }
    const bool p = Plausible(reinterpret_cast<const u8*>(pos), 4);
    LOG_INFO(Core, "VR sound: PlaySound type {} id {} at ({:.2f},{:.2f},{:.2f}) on '{}'", type, id,
             p ? pos[0] : 0.0f, p ? pos[1] : 0.0f, p ? pos[2] : 0.0f,
             Common::GetCurrentThreadName());
}

void InstallSoundTrace() {
    static bool tried = false;
    if (tried) {
        return;
    }
    tried = true;
    const char* const env = std::getenv("VR_SOUND_TRACE");
    if (env == nullptr || g_play_sound == nullptr) {
        return;
    }
    constexpr size_t Prologue = 16;
    u8* const site = g_base + PlaySoundRva;
    if (std::memcmp(site, PlaySoundSig.data(), Prologue) != 0 ||
        !Hook::MakeWritable(site, Prologue)) {
        LOG_WARNING(Core, "VR sound: trace not installed (the prologue differs)");
        return;
    }
    const int n = std::atoi(env);
    g_trace_left.store(n > 0 ? n : 200, std::memory_order_relaxed);
    g_trace_code = std::make_unique<Xbyak::CodeGenerator>(1024);
    void* const stub = Hook::EmitHostCallStubWith(
        *g_trace_code, reinterpret_cast<void*>(&OnPlaySound), PlaySoundSig.data(), Prologue,
        reinterpret_cast<u64>(site + Prologue), [](Xbyak::CodeGenerator& c) {
            using namespace Xbyak::util;
            c.mov(rdi, rdx); // type
            c.mov(rax, rsi);
            c.mov(rsi, rcx); // id
            c.mov(rdx, rax); // the position
        });
    g_trace_code->ready();
    Hook::WriteJump(site, Prologue, stub);
    LOG_INFO(Core, "VR sound: PlaySound trace installed at {:#x} ({} calls)", PlaySoundRva,
             g_trace_left.load(std::memory_order_relaxed));
}


// quick_music_box_crank (core/vr/music_box.h): the Tiny Music Box in the left hand, from the
// presenter's scene (LOCAL: the left controller's grip, where the left arm's IK puts the hunter's
// fist): the game's own model of it (s11012, FFX 20180's: the use clip's event 96), life size, or
// a small wooden box where it is not loaded; and our brass crank on its side toward the right hand
// at the angle the presenter turned it to (the hand on it, or where it was left), its knob
// brighter while the right hand holds it and the box glowing warm for a moment when it plays.
constexpr s32 MusicBoxModel = 11012;
s32 g_music_box_logged = 0; // the model's bounds (once)

void BuildMusicBox(F::List& list, const Mapping& map, const VR::MusicBox::Scene& mb, s64 now,
                   float scale_m, const std::array<float, 12>& palm) {
    namespace MB = VR::MusicBox;
    const auto look = TunedLook(MB::Goods);
    const float k_model = look.scale * Q::GetQuickSettings().held_scale;
    const auto attachment = Q::ItemFrameOf(palm, look, Q::GetQuickSettings().held_scale);
    const auto model = VR::Assets::GetModel(MusicBoxModel);
    const VR::Body::Vec3 mid = model
        ? VR::Body::Vec3{.5f*(model->min[0]+model->max[0]),
                        .5f*(model->min[1]+model->max[1]),
                        .5f*(model->min[2]+model->max[2])}
        : VR::Body::Vec3{.0175745f, .01475f, .033567f};
    const auto centre = attachment.origin +
        (attachment.x*mid.x + attachment.y*mid.y + attachment.z*mid.z)*k_model;
    const auto track_origin = map.ToTracking(attachment.origin);
    const float metres = k_model / std::max(scale_m, 1e-3f);
    MB::Geometry g{};
    g.width_m *= metres; g.height_m *= metres; g.depth_m *= metres;
    g.hub_out_m *= metres; g.axle_in_m *= metres; g.arm_m *= metres; g.knob_out_m *= metres;
    const auto f = MB::FrameForModel(track_origin,
        map.ToTracking(attachment.origin+attachment.x)-track_origin,
        map.ToTracking(attachment.origin+attachment.y)-track_origin,
        map.ToTracking(attachment.origin+attachment.z)-track_origin,
        metres, {mid.x, mid.y, mid.z});
    // Drawing and interaction follow the same carried hunter attachment.
    const auto carried_origin = map.ToTracking(map.Carried(attachment.origin));
    const auto contact = MB::FrameForModel(carried_origin,
        map.ToTracking(map.Carried(attachment.origin+attachment.x))-carried_origin,
        map.ToTracking(map.Carried(attachment.origin+attachment.y))-carried_origin,
        map.ToTracking(map.Carried(attachment.origin+attachment.z))-carried_origin,
        metres, {mid.x, mid.y, mid.z});
    MB::AttachmentChannel().Store({now, contact, g});
    const auto view = [&](Q::V3 p) { return map.WorldToView(map.ToWorld(p)); };
    const auto dir = [&](Q::V3 d) {
        const VR::Body::Vec3 w = VR::Body::TrackingToWorld({d.x, d.y, d.z}, map.base_yaw, 1.0f);
        return Q::Normalize(map.DirToView(w));
    };
    const Q::V3 ax = dir(f.axis); // native +Y, out toward the wearer's right
    const float since_play =
        mb.play_ns != 0 ? static_cast<float>(now - mb.play_ns) * 1e-9f : 99.0f;
    const float glow = since_play < 1.2f ? 1.0f - since_play / 1.2f : 0.0f;
    const std::array<float, 4> warm{0.55f * glow, 0.36f * glow, 0.12f * glow, 0.0f};
    // The native box and crank share the approved palm attachment.
    if (model != nullptr) {
        F::Item item = MakeItem(F::MeshId::Model, map.WorldToView(attachment.origin),
                                map.DirToView(attachment.x), map.DirToView(attachment.y),
                                map.DirToView(attachment.z), k_model, k_model, k_model);
        item.tex = F::Tex::Model;
        item.tex_key = static_cast<u32>(MusicBoxModel);
        item.shade = F::Shade::Model;
        item.color = {1.0f, 1.0f, 1.0f, 1.0f};
        item.emissive = warm;
        item.emissive[3] = 0.0f;
        list.Add(item);
        if (g_music_box_logged != 1) {
            g_music_box_logged = 1;
            LOG_INFO(Core,
                     "VR quick: MUSIC BOX model s{:05} drawn, bounds ({:.3f},{:.3f},{:.3f}) .. "
                     "({:.3f},{:.3f},{:.3f}) m (our box {:.3f} x {:.3f} x {:.3f})",
                     MusicBoxModel, model->min[0], model->min[1], model->min[2], model->max[0],
                     model->max[1], model->max[2], g.width_m, g.height_m, g.depth_m);
        }
    } else {
        // Native mesh loading fallback (Plaque is a unit cube).
        F::Item box = MakeItem(F::MeshId::Plaque, view(f.box), map.DirToView(attachment.x), map.DirToView(attachment.y),
                               map.DirToView(attachment.z), g.width_m * scale_m,
                               g.height_m * scale_m, g.depth_m * scale_m);
        box.shade = F::Shade::Lit;
        box.color = Linear(0.17f, 0.085f, 0.035f);
        box.emissive = warm;
        list.Add(box);
        // Its lid, a shade lighter.
        F::Item lid = MakeItem(F::MeshId::Plaque, map.WorldToView(centre + attachment.y * (.5f*g.height_m*scale_m)),
                               map.DirToView(attachment.x), map.DirToView(attachment.z),
                               map.DirToView(attachment.y), g.width_m * 1.04f * scale_m, g.depth_m * 1.04f * scale_m,
                               0.004f * scale_m);
        lid.shade = F::Shade::Lit;
        lid.color = Linear(0.24f, 0.13f, 0.06f);
        lid.emissive = warm;
        list.Add(lid);
        if (g_music_box_logged != 2) {
            g_music_box_logged = 2;
            LOG_INFO(Core, "VR quick: MUSIC BOX drawn as our box (model s{:05} not loaded)",
                     MusicBoxModel);
        }
    }
    // Where the box is: its tune plays there (PlayUseSounds, the held item's place).
    {
        const VR::Body::Vec3 w = map.Carried(map.ToWorld(f.box));
        g_held_place.Store({now, MB::Goods, {w.x, w.y, w.z}, {w.x, w.y, w.z}});
    }
    // Plaque is a unit cube (RoundedBox half extents .5), so every scale is a
    // physical dimension. The former thin-plaque x10 depth compensation enlarged
    // the crank tenfold after the mesh became a cube.
    // The crank: the hub on the box's side, the arm to the knob, the knob's handle along the axis.
    const float crank_scale = k_model;
    const std::array<float, 4> brass = Linear(0.42f, 0.30f, 0.10f);
    const Q::V3 radial_l = f.u * std::cos(mb.angle) + f.v * std::sin(mb.angle);
    const Q::V3 radial = dir(radial_l);
    const Q::V3 tangent = Q::Cross(ax, radial);
    F::Item hub = MakeItem(F::MeshId::Plaque, view(f.hub - f.axis*(.5f*g.axle_in_m)),
                           radial, tangent, ax, 0.008f * crank_scale, 0.008f * crank_scale,
                           g.axle_in_m * scale_m + 0.008f * crank_scale);
    hub.shade = F::Shade::Lit;
    hub.color = brass;
    list.Add(hub);
    F::Item arm = MakeItem(F::MeshId::Plaque, view(f.hub + radial_l * (0.5f * g.arm_m)), radial,
                           tangent, ax, g.arm_m * scale_m + 0.008f * crank_scale, 0.007f * crank_scale,
                           0.004f * crank_scale);
    arm.shade = F::Shade::Lit;
    arm.color = brass;
    list.Add(arm);
    F::Item knob = MakeItem(F::MeshId::Plaque, view(MB::KnobAt(g, f, mb.angle)), ax, radial,
                            tangent, g.knob_out_m * scale_m, 0.011f * crank_scale,
                            0.011f * crank_scale);
    knob.shade = F::Shade::Lit;
    knob.color = mb.grabbed ? Linear(0.26f, 0.145f, 0.06f) : Linear(0.19f, 0.10f, 0.04f);
    knob.emissive = mb.grabbed ? std::array<float, 4>{0.06f, 0.04f, 0.015f, 0.0f}
                               : std::array<float, 4>{};
    list.Add(knob);
}

} // Anonymous namespace

void QueueQuickUseSound(s32 goods, s32 use_anim) {
    if (g_play_sound != nullptr && Q::GetQuickSettings().use_sounds) {
        QueueUseCue(goods, use_anim);
    }
}

void QuickItemsGunShot(const float* start, const float* ray) {
    QuickItemsGunShotAt({start[0], start[1], start[2]}, {ray[0], ray[1], ray[2]});
}

void InstallQuickItems(VAddr base, u64 size) {
    if (g_ready || g_base != nullptr) {
        return;
    }
    const auto& qs = Q::GetQuickSettings();
    if (!qs.quick_items) {
        LOG_INFO(Core, "VR quick: quick_items=0, not installed (Y is Square)");
        return;
    }
    auto* const module = reinterpret_cast<u8*>(base);
    if (size < MenuManRva + 8) {
        LOG_INFO(Core, "VR quick: eboot too small ({:#x}) for Bloodborne 1.09; not installed",
                 size);
        return;
    }
    const auto matches = [module](u64 rva, const auto& sig) {
        return std::memcmp(module + rva, sig.data(), sig.size()) == 0;
    };
    const auto verdict = [](bool ok) { return ok ? "ok" : "MISMATCH"; };
    const bool use_ok = matches(UseGoodsRva, UseGoodsSig);
    const bool menu_ok = matches(MenuManLeaRva, MenuManLeaSig);
    const bool row_ok = matches(GoodsRowRva, GoodsRowSig);
    const bool can_ok = matches(CanUseRva, CanUseSig) && matches(WeaponIdRva, WeaponIdSig);
    const bool dpad_ok = matches(DpadRva, DpadSig);
    const bool dmy_ok = matches(DmyPolyRva, DmyPolySig);
    const bool qty_ok = matches(QuantityStoreRva, QuantityStoreSig);
    const bool launch_ok = matches(LaunchCtxRva, LaunchCtxSig);
    const auto dump = [module](const char* name, u64 rva, size_t length) {
        LOG_WARNING(Core, "VR quick: {} live bytes at {:#x}: {}", name, rva,
                    Hook::HexBytes(module + rva, length));
    };
    if (!use_ok) {
        dump("UseGoods", UseGoodsRva, UseGoodsSig.size());
    }
    if (!row_ok) {
        dump("GoodsParamRow", GoodsRowRva, GoodsRowSig.size());
    }
    if (!dpad_ok) {
        dump("D-pad path", DpadRva, DpadSig.size());
    }
    if (!launch_ok) {
        dump("launch", LaunchCtxRva, LaunchCtxSig.size());
    }
    g_ready = use_ok && menu_ok && row_ok;
    if (!g_ready) {
        LOG_WARNING(Core,
                    "VR quick: usegoods={} menuman={} goodsrow={}: NOT installed (Y is Square)",
                    verdict(use_ok), verdict(menu_ok), verdict(row_ok));
        return;
    }
    InitNativeQuickDll();
    g_base = module;
    g_use_goods = reinterpret_cast<UseGoodsFn>(module + UseGoodsRva);
    g_goods_row = reinterpret_cast<GoodsRowFn>(module + GoodsRowRva);
    g_can_use_ok = can_ok;
    g_can_use = reinterpret_cast<CanUseFn>(module + CanUseRva);
    g_hud_bytes = dpad_ok;
    g_dmy_poly = dmy_ok ? reinterpret_cast<DmyPolyFn>(module + DmyPolyRva) : nullptr;
    // The throw's whoosh (throw_sound): the game's PlaySound and event 128's sound type table.
    const bool sound_ok = matches(PlaySoundRva, PlaySoundSig) && matches(SoundTypeRva, SoundTypeSig);
    if (sound_ok) {
        g_play_sound = reinterpret_cast<PlaySoundFn>(module + PlaySoundRva);
        s32 disp{};
        std::memcpy(&disp, module + SoundTypeRva + 3, sizeof(disp));
        g_sound_table = module + SoundTypeRva + 7 + disp;
    } else {
        LOG_WARNING(Core, "VR quick: PlaySound / sound type signature MISMATCH: throws are silent");
    }
    if (matches(EndFlightRva, EndFlightSig)) {
        g_end_flight = reinterpret_cast<EndFlightFn>(module + EndFlightRva);
    } else {
        dump("end of flight", EndFlightRva, EndFlightSig.size());
        LOG_WARNING(Core, "VR quick: end-of-flight signature MISMATCH: no Molotov bursts");
    }
    if (const char* value = std::getenv("VR_QUICK_LOG"); value != nullptr) {
        g_log = std::max(std::atoi(value), 1);
    }
    g_dry_run = std::getenv("VR_QUICK_DRYRUN") != nullptr;
    g_trace_on = std::getenv("VR_THROW_TRACE") != nullptr;
    if (const char* value = std::getenv("VR_THROW_FAKE_TARGET"); value != nullptr) {
        auto& m = g_fake_target_m;
        g_fake_target = std::sscanf(value, "%f,%f,%f", &m[0], &m[1], &m[2]) == 3;
    }
    g_markers = std::getenv("VR_QUICK_MARKERS") != nullptr;
    if (const char* value = std::getenv("VR_BURST_TEST"); value != nullptr) {
        g_burst_test_ms = std::max(std::atoi(value), 1);
    }
    if (const char* value = std::getenv("VR_QUICK_AUTOTEST"); value != nullptr) {
        g_autotest = value;
    }
    if (const char* value = std::getenv("VR_QUICK_TEST_GIVE"); value != nullptr) {
        if (matches(AddItemRva, AddItemSig)) {
            g_test_give = value;
            g_add_item = reinterpret_cast<AddItemFn>(module + AddItemRva);
        } else {
            LOG_WARNING(Core, "VR quick: VR_QUICK_TEST_GIVE: AddItem signature MISMATCH");
        }
    }
    // H2: only for physical throws.
    if (qs.throws && launch_ok && std::getenv("VR_THROW_H2_OFF") == nullptr) {
        u8* const site = module + LaunchRva;
        if (!Hook::MakeWritable(site, LaunchLen)) {
            LOG_ERROR(Core, "VR quick: could not make the launch site writable");
        } else {
            try {
                g_code = std::make_unique<Xbyak::CodeGenerator>(1024);
                void* const stub = Hook::EmitHostCallStub(
                    *g_code, reinterpret_cast<void*>(&LaunchHook),
                    LaunchCtxSig.data() + LaunchSiteOffset, LaunchLen,
                    reinterpret_cast<u64>(site + LaunchLen), Xbyak::util::r14);
                Hook::WriteJump(site, LaunchLen, stub);
                g_launch_hook = true;
            } catch (const Xbyak::Error& e) {
                LOG_ERROR(Core, "VR quick: launch hook not generated ({})", e.what());
            }
        }
    }
    LOG_INFO(Core,
             "VR quick: installed: siteA ok usegoods ok goodsrow ok canuse {} hudbytes {} "
             "dmypoly {} quantity store {} launch {} throw sound {}{}{}{}",
             can_ok ? "ok" : "skip", dpad_ok ? "ok" : "skip", dmy_ok ? "ok" : "MISMATCH (no arc)",
             qty_ok ? "original" : "patched (Infinite Items?)",
             g_launch_hook ? "hooked (physical throws)"
             : !qs.throws  ? "off (quick_throw=0: the game's own throw)"
                           : "MISMATCH (throws fly the game's way)",
             g_play_sound == nullptr ? "MISMATCH" : qs.throw_sound ? "ok" : "off",
             g_log > 0 ? fmt::format(" log={}", g_log) : std::string{},
             std::string{g_dry_run ? " DRY RUN" : ""} + (g_trace_on ? " TRACE" : "") +
                 (g_fake_target ? fmt::format(" FAKE TARGET ({}, {}, {})", g_fake_target_m[0],
                                              g_fake_target_m[1], g_fake_target_m[2])
                                : std::string{}),
             g_autotest.empty() ? std::string{} : " autotest " + g_autotest);
}

bool QuickItemsReady() {
    return g_ready;
}

// quick_use_wipe: the applied paper's buff SpEffect on the hunter (the log: the game's own buff
// took, its effect on the blade with it), once, or that it never showed within 1.5 s.
void WipeBuffTick(u8* player) {
    if (g_site.wipe_goods < 0) {
        return;
    }
    const s64 ms = (NowNs() - g_site.wipe_ns) / Ms;
    const bool on = g_site.wipe_ref > 0 && HasEffect(player, g_site.wipe_ref);
    if (on || ms >= 1500) {
        LOG_INFO(Core, "VR quick: WIPE buff of goods {}: SpEffect {} {} on the hunter{}",
                 g_site.wipe_goods, g_site.wipe_ref, on ? "is" : "NOT",
                 on ? fmt::format(" {} ms after the use", ms) : std::string{" within 1.5 s"});
        g_site.wipe_goods = -1;
    }
}

void QuickItemsSiteA(u8* player, bool may_use) {
    if (!g_ready || player == nullptr) {
        return;
    }
    Pointers p{};
    const bool valid = PointersOf(player, p);
    SampleMotion();
    g_player_handle.store(Read<u32>(player + OffsetHandle), std::memory_order_relaxed);
    TraceTick(player);
    WipeBuffTick(player);
    Publish(player, valid ? &p : nullptr);
    if (!valid) {
        return;
    }
    Q::Request r{};
    if (Q::LatestRequest(r) && r.serial != g_site.taken_serial && r.serial != 0) {
        g_site.taken_serial = r.serial;
        Handle(player, p, r, may_use);
        if (r.action != Q::Action::None || r.select >= 0) {
            Publish(player, &p); // the new count and selection at once
        }
        return;
    }
    if (AutotestRequest(g_site.last, r)) {
        Handle(player, p, r, may_use);
        Publish(player, &p);
    }
}

// quick_use_wipe: one sample of the paper against the blade (QuickItemsRenderTick, every rendered
// frame while a blade-wipe item is in the hunter's hand): the paper where the card sits in his
// fist (GripCentre in the palm dummy's frame) and his right weapon's blade (its hit capsule,
// MeleeBladeSegment), both from this frame's pose, in LOCAL metres, for the presenter's stroke
// detector (Q::WipeDetector). The blade's place is logged once per item taken (where to wipe;
// headset tuning) and with VR_QUICK_LOG=2 every half second.
struct WipeLog {
    s32 goods{-1};
    s64 next_ns{0};
    // The sample's own cost (the blade's dummies, the paper, the publish), since the last line.
    s64 cost_sum_ns{0};
    s64 cost_max_ns{0};
    u32 cost_n{0};
};
WipeLog g_wipe_log{};
u32 g_wipe_serial = 0;

// quick_use_wipe's measurement, once per take (the user's 2026-10-08 "easier to find the spot"):
// where the game's own blade trail (the light move's SpawnFFX_Blade dummies, drawn along the
// blade you see) lies against the segment the paper is measured on, LOCAL cm.
void LogWipeTrail(u8* chr, const Mapping& map, Q::V3 base, Q::V3 tip) {
    VR::Body::Vec3 a_w{};
    VR::Body::Vec3 b_w{};
    s32 id_a = -1;
    s32 id_b = -1;
    if (!MeleeBladeTrail(chr, a_w, b_w, id_a, id_b)) {
        LOG_INFO(Core, "VR quick: wipe: the blade trail: none known for this form");
        return;
    }
    const auto at = [&](Q::V3 p, float& along, float& off) {
        const Q::WipeContact w = Q::WipeMeasure(p, base, tip);
        along = w.along;
        off = w.dist;
    };
    float a_along = 0.0f, a_off = 0.0f, b_along = 0.0f, b_off = 0.0f;
    const Q::V3 a = map.ToTracking(a_w);
    const Q::V3 b = map.ToTracking(b_w);
    at(a, a_along, a_off);
    at(b, b_along, b_off);
    // The trail line's direction against the segment's.
    const float cosang = Q::Dot(Q::Normalize(b - a), Q::Normalize(tip - base));
    LOG_INFO(Core,
             "VR quick: wipe: the blade trail {} -> {}: {:.1f} cm along / {:.1f} cm off the "
             "segment, {:.1f} cm along / {:.1f} cm off; {:.0f} cm long, {:.0f} deg to it",
             id_a, id_b, 100.0f * a_along, 100.0f * a_off, 100.0f * b_along, 100.0f * b_off,
             100.0f * Q::Length(b - a),
             std::acos(std::clamp(std::abs(cosang), 0.0f, 1.0f)) * 57.2958f);
}

void WipeSample(u8* chr, const std::array<float, 8 * 12>& palm, const Mapping& map, s32 goods,
                const VR::Body::BodyStatus& status, bool status_fresh, s64 now) {
    const s64 cost_from = NowNs();
    const auto& c = Q::GripCentre;
    const VR::Body::Vec3 paper_w{palm[0] * c[0] + palm[1] * c[1] + palm[2] * c[2] + palm[3],
                                 palm[4] * c[0] + palm[5] * c[1] + palm[6] * c[2] + palm[7],
                                 palm[8] * c[0] + palm[9] * c[1] + palm[10] * c[2] + palm[11]};
    VR::Body::Vec3 base_w{};
    VR::Body::Vec3 tip_w{};
    const char* how = "";
    const bool blade = MeleeBladeSegment(chr, base_w, tip_w, how);
    const Q::V3 paper = map.ToTracking(paper_w);
    Q::V3 base = map.ToTracking(base_w);
    Q::V3 tip = map.ToTracking(tip_w);
    // The rub (quick_wipe_rub): the paper as you see it, its card as BuildHeld draws it (the
    // item's icon pinched between the thumb and the fingers: held.card_lift of its size out of the
    // fist's channel, held.card_front toward the eye, facing the eye, its up the channel's), a
    // little inside its 9 cm slate.
    const Q::HeldLook look = TunedLook(goods);
    const float size = 0.09f * look.scale * Q::GetQuickSettings().held_scale;
    const VR::Body::Vec3 dy = VR::Body::Normalize(VR::Body::Vec3{palm[1], palm[5], palm[9]});
    const float lift = VR::Tune::Get("held.card_lift", 0.70f) * size;
    const VR::Body::Vec3 card_w = paper_w + dy * lift;
    Q::V3 card = map.ToTracking(card_w);
    const Q::V3 eye{map.head.x, map.head.y, map.head.z};
    const Q::V3 n = Q::Normalize(eye - card, {0.0f, 0.0f, 1.0f});
    card = card + n * VR::Tune::Get("held.card_front", 0.045f);
    Q::V3 up = map.ToTracking(card_w + dy) - map.ToTracking(card_w);
    up = Q::Normalize(up - n * Q::Dot(up, n), {0.0f, 1.0f, 0.0f});
    // The eye's right in tracking space (right-handed, -z forward): up x n. (BuildHeld's
    // Cross(n, up) is the same direction in its view space, z forward.)
    const Q::V3 right = Q::Cross(up, n);
    const float inset = VR::Tune::Get("wipe.card_inset", 0.9f);
    float half_w = 0.5f * size * inset, half_h = half_w;
    Q::V3 rect_right = right, rect_up = up;
    if (const RubLook rub = RubLookOf(goods); rub.mesh != F::MeshId::Count) {
        // Our model's own rectangle, where BuildHeld draws it (pinched by its left end): its
        // offsets and size are game units there, tracking metres here (/ the world's scale).
        const float k_rub = size / 0.09f / std::max(map.scale, 1e-3f);
        const RubFrame f = RubFrameOf(rub, card, right, up, n, k_rub);
        card = f.origin + (f.right * rub.mid[0] + f.up * rub.mid[1] + n * rub.mid[2]) * k_rub;
        rect_right = f.right;
        rect_up = f.up;
        // A little wiggle room round its outline (the user, 2026-10-12): easy to apply.
        const float margin = VR::Tune::Get("wipe.rub_margin", 0.015f);
        half_w = rub.half[0] * k_rub + margin;
        half_h = rub.half[1] * k_rub + margin;
    }
    if (goods == 1300 || goods == 1310 || goods == 1320) {
        const auto f = HandRubFrame(palm, look);
        const auto rub = RubLookOf(goods);
        const float k = look.scale * Q::GetQuickSettings().held_scale / std::max(map.scale, 1e-3f);
        const Q::V3 origin = map.ToTracking(map.Carried(f.origin));
        rect_right = Q::Normalize(map.ToTracking(map.Carried(f.origin + f.x)) - origin);
        rect_up = Q::Normalize(map.ToTracking(map.Carried(f.origin + f.y)) - origin);
        const Q::V3 normal = Q::Normalize(map.ToTracking(map.Carried(f.origin + f.z)) - origin);
        card = origin + (rect_right * rub.mid[0] + rect_up * rub.mid[1] + normal * rub.mid[2]) * k;
        const float margin = VR::Tune::Get("wipe.rub_margin", .015f);
        half_w = rub.half[0] * k + margin;
        half_h = rub.half[1] * k + margin;
    }
    Q::WipeContact w = blade ? Q::WipeMeasureCard(paper, card, rect_right, rect_up, half_w, half_h,
                                                  base, tip)
                             : Q::WipeContact{};
    std::array<VR::Body::Vec3, Q::WipeContact::MaxPathPoints> chain_world{};
    int chain_count = 0;
    if (WhipWipePoints(chr, chain_world, chain_count)) {
        std::array<Q::V3, Q::WipeContact::MaxPathPoints> chain{};
        for (int i = 0; i < chain_count; ++i) chain[i] = map.ToTracking(chain_world[i]);
        w = Q::WipeMeasurePath(paper, card, rect_right, rect_up, half_w, half_h,
                              chain.data(), chain_count);
        how = "the posed whip blade chain (handle excluded)";
        if (w.valid) {
            // Diagnostics report the path's endpoints, not the old handle-only segment.
            base = chain[0];
            tip = chain[chain_count - 1];
        }
    }
    w.serial = ++g_wipe_serial;
    w.steady_ns = now;
    w.goods = goods;
    w.arms = status_fresh && status.arm_weight[0] >= 0.9f && status.arm_weight[1] >= 0.9f;
    Q::PublishWipe(w);
    const s64 cost = NowNs() - cost_from;
    g_wipe_log.cost_sum_ns += cost;
    g_wipe_log.cost_max_ns = std::max(g_wipe_log.cost_max_ns, cost);
    ++g_wipe_log.cost_n;
    if (g_wipe_log.goods == goods && (g_log < 2 || now < g_wipe_log.next_ns)) {
        return;
    }
    const bool first = g_wipe_log.goods != goods;
    g_wipe_log.goods = goods;
    g_wipe_log.next_ns = now + 500 * Ms;
    if (g_log >= 2 && g_wipe_log.cost_n > 0) {
        LOG_INFO(Core, "VR quick: wipe: the sample's cost {:.1f} us mean, {:.1f} us max over {}",
                 static_cast<double>(g_wipe_log.cost_sum_ns) * 1e-3 / g_wipe_log.cost_n,
                 static_cast<double>(g_wipe_log.cost_max_ns) * 1e-3, g_wipe_log.cost_n);
    }
    g_wipe_log.cost_sum_ns = g_wipe_log.cost_max_ns = 0;
    g_wipe_log.cost_n = 0;
    if (!w.valid) {
        LOG_INFO(Core, "VR quick: wipe: no blade for goods {} ({}): it cannot be wiped on | t {:.3f}",
                 goods, how, static_cast<double>(now) * 1e-9);
        return;
    }
    LOG_INFO(Core,
             "VR quick: wipe: the blade ({}) base ({:.3f},{:.3f},{:.3f}) tip ({:.3f},{:.3f},{:.3f}) "
             "LOCAL, {:.0f} cm; the paper ({:.3f},{:.3f},{:.3f}) {:.1f} cm from it, {:.0f} cm "
             "along; its card ({:.3f},{:.3f},{:.3f}, {:.1f} cm) {:.1f} cm from the axis, {:.0f} "
             "cm along; the eye ({:.3f},{:.3f},{:.3f}); arms {:.2f} / {:.2f} | t {:.3f}",
             how, base.x, base.y, base.z, tip.x, tip.y, tip.z, w.length * 100.0f, paper.x, paper.y,
             paper.z, w.dist * 100.0f, w.along * 100.0f, card.x, card.y, card.z,
             200.0f * half_w, w.gap * 100.0f, w.card_along * 100.0f, eye.x, eye.y, eye.z,
             status.arm_weight[0], status.arm_weight[1], static_cast<double>(now) * 1e-9);
    if (first && w.path_count == 0) {
        LogWipeTrail(chr, map, base, tip);
    }
}

// Body attachments use the bones AND character transform from the same pose update,
// viewed through this render's camera. They never receive the late hand carry.
VR::Holsters::Targets BuildHolsters(F::List& list, const Mapping& map, const Q::Scene& scene,
                                    const Q::Snapshot& snap, const VR::BodyMounts::Pose& pose,
                                    s64 now, s32 chest_armor) {
    const auto& qs = Q::GetQuickSettings();
    VR::Holsters::Targets targets{};
    targets.ns = now;
    targets.space = map.space_serial;
    auto& h = targets.layout;
    h.valid = true;
    h.assignment = VR::Holsters::InventoryAssignment;
    if (!VR::GetSettings().inject)
        for (int& assignment : h.assignment)
            if (assignment == 1)
                assignment = 0;
    h.hover = scene.holsters.assignment == h.assignment ? scene.holsters.hover : -1;
    const auto dir = [&](VR::Body::Vec3 p) {
        return map.DirToView(p);
    };
    const auto target_at = [&](int slot, Q::V3 view) {
        const auto tracking = map.ToTracking(map.ViewToWorld(view));
        h.centre[slot] = {tracking.x, tracking.y, tracking.z};
    };
    const float size = VR::PlayerBody::BodyScaleFor(VR::HeightBodyOffsetM()) * map.scale;
    for (int i = 0; i < 3; ++i) {
        if (h.assignment[i] <= 0)
            continue;
        auto mount = VR::BodyMounts::WorldSurface(pose, i, qs.holster_height_cm * .01f * map.scale,
                                                  qs.holster_width_cm * .01f * map.scale,
                                                  qs.holster_forward_cm * .01f * map.scale);
        if (h.assignment[i] == 1)
            mount = VR::BodyMounts::FitVialPouch(mount, chest_armor, size);
        const auto surface = map.WorldToView(mount.origin);
        const Q::V3 x = dir(mount.right), y = dir(mount.up), z = dir(mount.back);
        const float k = i == 2 ? .72f : 1.0f;
        const auto seat = [&](float back_extent) {
            const auto p = VR::BodyMounts::Seat({surface.x, surface.y, surface.z}, {z.x, z.y, z.z},
                                                back_extent, .002f * map.scale);
            return Q::V3{p.x, p.y, p.z};
        };
        // Back wall and belt fittings rest against the body instead of centering in it.
        const auto c = seat(.064f * size * k);
        if (h.assignment[i] >= 2 && h.assignment[i] < Q::SlotCount + 2) {
            const int slot_index = h.assignment[i] - 2;
            const auto& slot = snap.slot[static_cast<size_t>(slot_index)];
            const bool fresh =
                snap.valid && now >= snap.steady_ns && now - snap.steady_ns < 300 * Ms;
            const auto look = TunedLook(slot.goods);
            const auto model = look.model > 0 ? VR::Assets::GetModel(look.model) : nullptr;
            const RubLook rub = RubLookOf(slot.goods);
            const bool paper = rub.mesh != F::MeshId::Count;
            // Our model of it, as the hand shows it (GoodsLookOf).
            const GoodsLook goods_look = GoodsLookOf(slot.goods);
            const bool ours = !model && goods_look.count > 0 &&
                              VR::Tune::Get("held.goods_models", 1.0f) >= 0.5f;
            const bool molotov =
                look.model == 10010 && VR::Tune::Get("held.molotov_look", 1.0f) >= .5f;
            const auto kind = Q::Classify(slot.goods).mesh;
            const bool strapped = look.model == 10010 || kind == Q::MeshKind::Knife;
            // Bare strapped items sit diagonally; their pickup centre remains the mount centre.
            const Q::V3 sx = strapped ? x * .9396926f + y * .3420201f : x;
            const Q::V3 sy = strapped ? y * .9396926f - x * .3420201f : y;
            const float model_scale =
                .90f * size * look.scale * Q::GetQuickSettings().held_scale * (paper ? .9f : 1.0f);
            Q::V3 lo{-.035f, -.05f, -.012f}, hi{.035f, .05f, .012f};
            if (paper) {
                lo = {rub.mid[0] - rub.half[0], rub.mid[1] - rub.half[1], rub.mid[2] - rub.half[2]};
                hi = {rub.mid[0] + rub.half[0], rub.mid[1] + rub.half[1], rub.mid[2] + rub.half[2]};
            } else if (ours) {
                lo = {goods_look.lo[0], goods_look.lo[1], goods_look.lo[2]};
                hi = {goods_look.hi[0], goods_look.hi[1], goods_look.hi[2]};
            } else if (molotov) {
                lo = {-.053f, -.096f, -.053f};
                hi = {.053f, .20f, .053f};
            } else if (model) {
                lo = {model->min[0], model->min[1], model->min[2]};
                hi = {model->max[0], model->max[1], model->max[2]};
            }
            const Q::V3 mid = (lo + hi) * .5f;
            const Q::V3 dimensions = (hi - lo) * model_scale;
            // Fit the actual back face; a flat knife does not float like a round bottle.
            const Q::V3 centre = seat(dimensions.z * .5f + (strapped ? 0.f : .009f * size));
            target_at(i, centre);
            const float holder_h = std::clamp(dimensions.y * .4f, .035f * size, .12f * size);
            const Q::V3 holder_c = centre - y * (dimensions.y * .5f - holder_h * .5f);
            auto holder = MakeItem(F::MeshId::QuickHolster, holder_c, x, y, z,
                                   (dimensions.x + .014f * size) * 10, holder_h * 10,
                                   (dimensions.z + .018f * size) * 10);
            // Fallback primitives can have unrelated dimensions: keep them uncullable until
            // the actual model arrives. Known meshes share one generous sphere with the holder.
            const float radius = model || paper || ours || molotov || slot.Empty()
                                     ? Q::Length(dimensions) * .5f + .06f * size
                                     : 0.0f;
            const std::array<float, 4> bounds{centre.x, centre.y, centre.z, radius};
            holder.cull_sphere = bounds;
            holder.shade = F::Shade::Apothecary;
            if (h.hover == i)
                holder.emissive = {.12f, .065f, .02f, 0};
            if (!strapped)
                list.Add(holder);
            if (!fresh || !Q::ShowStoredItem(scene, slot, slot_index))
                continue;
            if (paper) {
                const auto first = list.count;
                // Its face out of the holder (-z); its bounds' centre on the seat.
                const Q::V3 front = z * -1.0f;
                AddRub(list, rub,
                       centre - (x * mid.x + y * mid.y + front * mid.z) * model_scale, x, y, front,
                       model_scale);
                for (auto n = first; n < list.count; ++n)
                    list.items[n].cull_sphere = bounds;
            } else if (ours) {
                const Q::V3 origin = centre - (x * mid.x + y * mid.y + z * mid.z) * model_scale;
                for (int p = 0; p < goods_look.count; ++p) {
                    const GoodsPart& part = goods_look.parts[static_cast<size_t>(p)];
                    auto stored = MakeItem(part.mesh, origin, x, y, z, model_scale, model_scale,
                                           model_scale);
                    if (p == goods_look.liquid) {
                        // The vial's glass, its blood level and still on the belt.
                        const float half = .5f * (goods_look.top - goods_look.bottom);
                        stored.shade = F::Shade::Syringe;
                        stored.color = {0, 1, 0,
                                        VR::Held::Liquid::Plane({0, 1, 0}, goods_look.radius, half,
                                                                goods_look.fill)};
                        stored.uv = {goods_look.bottom, goods_look.top, goods_look.radius,
                                     goods_look.fill};
                        stored.emissive = {0, 0, 0, 0};
                    } else {
                        stored.shade = F::Shade::Goods;
                        stored.uv[0] = slot.goods == Q::QueenlyBlood ? 1.0f : 0.0f;
                        stored.color = {part.tint[0], part.tint[1], part.tint[2], 1};
                    }
                    stored.cull_sphere = bounds;
                    list.Add(stored);
                }
            } else {
                const Q::V3 origin = centre - (sx * mid.x + sy * mid.y + z * mid.z) * model_scale;
                const auto fallback = static_cast<F::MeshId>(static_cast<u16>(F::MeshId::Bottle) +
                                                             static_cast<u16>(kind) -
                                                             static_cast<u16>(Q::MeshKind::Bottle));
                auto stored = MakeItem(model                        ? F::MeshId::Model
                                       : kind == Q::MeshKind::Token ? F::MeshId::Token
                                                                    : fallback,
                                       origin, sx, sy, z, model_scale, model_scale, model_scale);
                stored.shade = F::Shade::Model;
                if (model) {
                    stored.tex = F::Tex::Model;
                    stored.tex_key = static_cast<u32>(look.model);
                    stored.emissive[3] = mid.y;
                } else
                    stored.color = {.12f, .08f, .045f, 1};
                if (molotov) {
                    stored.mesh = F::MeshId::VictorianMolotov;
                    stored.shade = F::Shade::Molotov;
                    stored.tex = F::Tex::None;
                    stored.color = {1, 1, 1, 1};
                }
                stored.cull_sphere = bounds;
                list.Add(stored);
            }
            continue;
        }
        target_at(i, c);
        // Slim only the displayed case and contents along the outward axis. Anchor
        // the back at the belt; keep the original pickup centre and reach unchanged.
        constexpr float pouch_depth = .85f;
        const auto visual_c = c + z * (.064f * (1.0f - pouch_depth) * size * k);
        auto item = MakeItem(F::MeshId::ApothecaryCase, visual_c, x, y, z,
                             size * k, size * k, size * k * pouch_depth);
        // Includes the case, flap and all four stored syringes, at every body scale.
        const std::array<float, 4> bounds{c.x, c.y, c.z, .18f * size * k};
        item.cull_sphere = bounds;
        item.shade = F::Shade::Apothecary;
        if (h.hover == i)
            item.emissive = {.12f, .065f, .02f, 0};
        list.Add(item);
        if (h.assignment[i] == 1) {
            u64 held_word = 0;
            const bool unspent = HeldVialOurs(now, held_word) && held_word == 0;
            const int count = std::clamp(CarriedBloodVials() - (unspent ? 1 : 0), 0, 4);
            for (int vial = 0; vial < count; ++vial) {
                const Q::V3 origin = visual_c +
                                     x * ((static_cast<float>(vial % 2) - .5f) * .044f * size * k) +
                                     z * ((static_cast<float>(vial / 2) - .5f) * .046f * size * k * pouch_depth) -
                                     y * (.105f * size * k);
                auto stored = MakeItem(F::MeshId::StoredSyringe, origin, x, y, z, size * k,
                                       size * k, size * k * pouch_depth);
                stored.cull_sphere = bounds;
                stored.shade = F::Shade::Syringe;
                stored.color = {0, 1, 0, .047f}; // upright, full blood chamber
                stored.uv = {.0352f, .1408f, .0146f, 1.0f};
                stored.emissive = {0, 0, .30f, .10f};
                list.Add(stored);
            }
        }
    }
    return targets;
}

// Developer-only numerical capture of the actual game pose and submitted item matrices.
// Request after the grip has settled: lab.capture=N in VR_TUNE_FILE, output VR_GRIP_LAB_DUMP.
void CaptureGripLab(const F::List& list, const Mapping& map,
                    const std::array<float, 12>& palm, u32 first, s32 goods) {
    static const char* path = std::getenv("VR_GRIP_LAB_DUMP");
    if (!path || !*path || !VR::GripPreview::Enabled()) return;
    static int captured = 0;
    const int request = static_cast<int>(VR::Tune::Get("lab.capture", 0.0f));
    if (request <= 0 || request == captured || first >= list.count) return;
    GripLabPose pose{};
    if (!ReadGripLabPose(pose)) return;
    std::FILE* out = std::fopen(path, "w");
    if (!out) return;
    const auto array = [out](const auto& values) {
        std::fprintf(out, "[");
        bool comma = false;
        for (float x : values) { std::fprintf(out, "%s%.9g", comma ? "," : "", double(x)); comma = true; }
        std::fprintf(out, "]");
    };
    std::fprintf(out, "{\"version\":1,\"stage\":\"game render preparation before shared hand carry\","
                      "\"request\":%d,\"goods\":%d,\"record\":%llu,\"world\":", request, goods,
                 static_cast<unsigned long long>(list.record));
    array(pose.world); std::fprintf(out, ",\"palm\":"); array(palm);
    std::fprintf(out, ",\"view_rows\":"); array(map.rows);
    std::fprintf(out, ",\"camera_position\":[%.9g,%.9g,%.9g],\"local\":[",
                 double(map.position.x), double(map.position.y), double(map.position.z));
    for (size_t i=0;i<pose.local.size();++i) { if(i)std::fprintf(out,","); array(pose.local[i]); }
    std::fprintf(out, "],\"model\":[");
    for (size_t i=0;i<pose.model.size();++i) { if(i)std::fprintf(out,","); array(pose.model[i]); }
    std::fprintf(out, "],\"items\":[");
    for (u32 i=first;i<list.count;++i) {
        if(i>first)std::fprintf(out,",");
        std::fprintf(out,"{\"mesh\":%u,\"model\":%u,\"mv\":",
                     unsigned(list.items[i].mesh),list.items[i].tex_key);
        array(list.items[i].mv); std::fprintf(out,"}");
    }
    std::fprintf(out,"]}\n"); std::fclose(out); captured=request;
    LOG_INFO(Core,"VR grip lab: captured request {} goods {} record {}", request, goods, list.record);
}

void QuickItemsRenderTick(u64 record_id, const float* rows, const float* position) {
    // Publish exactly once, including every early exit; never leave invisible grab targets.
    struct MountPublication {
        VR::Holsters::Targets targets{};
        ~MountPublication() { Q::PublishHolsterTargets(targets); }
    } mounts;
    if (!g_ready || g_dmy_poly == nullptr || rows == nullptr || position == nullptr) {
        return;
    }
    Q::Scene scene{};
    const bool have_scene = Q::LatestScene(scene);
    const s64 now = NowNs();
    InstallSoundTrace(); // VR_SOUND_TRACE (tests)
    PlayWhoosh(now);
    PlayUseSounds(now, position);
    MolotovBurstTick(now);
    auto& rs = g_render;
    VR::CameraRecord rec{};
    if (!VR::GetRecord(record_id, rec) || !rec.driven || rec.menu) return;
    const bool holsters = have_scene && scene.holsters_enabled && now >= scene.holsters_ns &&
                          now - scene.holsters_ns < 250 * Ms &&
                          Q::GetQuickSettings().holster_mode != 0;
    const bool folding = have_scene && scene.state == Q::SceneState::Closed &&
                         rs.placed_serial == scene.open_serial && scene.open_serial != 0 &&
                         now - scene.state_ns < 120 * Ms;
    const bool open = have_scene && scene.state == Q::SceneState::Open;
    const bool held_mesh =
        have_scene && scene.state == Q::SceneState::Holding && scene.held_goods >= 0;
    // vial_inject_drain: this frame's update made the vial in the hand ours (at its level).
    u64 drain_word = 0;
    const bool drain = HeldVialOurs(now, drain_word);
    if (!g_models_requested) {
        // The held items' game models load in the background from the first tick on (one read
        // of the sfx archive), long before the first grab.
        g_models_requested = true;
        (void)VR::Assets::GetModel(Q::HeldLookOf(1200).model);
    }
    if (!held_mesh) {
        g_held_logged = -1;
        g_wipe_log.goods = -1; // the next wipe item taken logs its blade again
    }
    // quick_hold_use: the ring's flash once the item has left the hand (used: where it last was).
    const bool flashing = have_scene && !held_mesh && g_ring_place.valid &&
                          now - g_ring_place.ns < 1000 * Ms && HoldFlash(scene, now) >= 0.0f;
    if (!open && !folding && !held_mesh && !flashing && !g_markers && !drain && !holsters) {
        return;
    }
    u8* const world = Read<u8*>(g_base + WorldChrManRva);
    u8* const chr = Plausible(world) ? Read<u8*>(world + OffsetMainPlayer) : nullptr;
    if (!Plausible(chr)) {
        return;
    }
    alignas(16) std::array<float, 8 * 12> m{};
    const s32 id = PalmDummy;
    if (g_dmy_poly(chr, m.data(), &id, 1) < 1) {
        return;
    }
    // The hover point: the fist's centre (else the palm, logged once).
    alignas(16) std::array<float, 8 * 12> g{};
    const s32 grip_id = GripDummy;
    const bool grip_found = g_dmy_poly(chr, g.data(), &grip_id, 1) >= 1;
    if (!grip_found && !g_grip_missing_logged) {
        g_grip_missing_logged = true;
        LOG_WARNING(Core, "VR quick: no grip dummy {} on the hunter: the arc hovers from his palm",
                    GripDummy);
    }
    Mapping map{};
    map.space_serial = rec.space_serial;
    map.position = {position[0], position[1], position[2]};
    map.eye_world = map.position - VR::Body::Vec3{rec.eye_offset.x, rec.eye_offset.y,
                                                  rec.eye_offset.z};
    map.head = {rec.head_position.x, rec.head_position.y, rec.head_position.z};
    map.base_yaw = rec.base_yaw;
    map.scale = rec.world_scale > 0.0f ? rec.world_scale : 1.0f;
    std::memcpy(map.rows.data(), rows, sizeof(float) * 12);
    map.carry_on = F::GetHandCarry(record_id, map.carry);
    const VR::Body::Vec3 palm_world{m[3], m[7], m[11]};
    // The fist where the frame shows it: inframe_hand_sync carries it as it does the hand items,
    // so the hover point is in this update's play space with the cards (not one update of your
    // walk or strafe behind the fist).
    const Q::V3 palm = map.ToTracking(map.Carried(palm_world));
    const VR::Body::Vec3 grip_world{g[3], g[7], g[11]};
    const Q::V3 hand = grip_found ? map.ToTracking(map.Carried(grip_world)) : palm;
    const Q::V3 eye{map.head.x, map.head.y, map.head.z};
    const auto& qs = Q::GetQuickSettings();

    Q::Snapshot snap{};
    Q::LatestSnapshot(snap);
    // The hunter's left arm is yours (it follows the controller), not the game's (a roll, a hit,
    // a knockdown: his animated hand may be on the floor or behind him).
    VR::Body::BodyStatus status{};
    const bool status_fresh = VR::Body::LatestBodyStatus(status) &&
                              VR::Body::SteadyNowNs() - status.steady_ns < 250 * Ms;
    const bool arm = status_fresh && status.arm_weight[0] >= Q::Config{}.arm_min;
    // quick_use_wipe: a blade-wipe item in his hand: the paper against his blade (WipeSample).
    if (held_mesh && Q::WipeOf(scene.held_goods) && qs.use_wipe) {
        WipeSample(chr, m, map, scene.held_goods, status, status_fresh, now);
    }
    // The arc is placed once the arm is all yours again (it blends back over 0.3 s: at 0.9 the
    // fist can still be a tenth of the way to the animation's hand, 10 cm after a roll), or 0.4 s
    // after it was mostly yours.
    if (!arm) {
        rs.arm_since_ns = 0;
    } else if (rs.arm_since_ns == 0) {
        rs.arm_since_ns = now;
    }
    const bool arm_settled =
        arm && (status.arm_weight[0] >= 0.98f || now - rs.arm_since_ns >= 400 * Ms);
    if (open && rs.placed_serial != scene.open_serial && !arm_settled &&
        rs.waiting_serial != scene.open_serial) {
        rs.waiting_serial = scene.open_serial;
        LOG_INFO(Core,
                 "VR quick: arc {} waits for the arm (the game has it: weight {:.2f}); placed "
                 "around your hand once it is back | t {:.3f}",
                 scene.open_serial, status.arm_weight[0], static_cast<double>(now) * 1e-9);
    }
    if (open && rs.placed_serial != scene.open_serial && arm_settled) {
        // Placed once per open, around where the hunter's fist is now (once the arm follows your
        // controller again: never around his animated hand); it then stays put in your play space
        // (and moves with you when you walk or turn).
        rs.placed_serial = scene.open_serial;
        rs.placed_ns = now;
        rs.hover = -1;
        rs.lift.fill(0.0f);
        int n = 0;
        for (int k = 0; k < Q::SlotCount; ++k) {
            if ((scene.cards & (1u << k)) != 0) {
                rs.card_slot[static_cast<size_t>(n++)] = static_cast<s8>(k);
            }
        }
        Q::LayoutConfig config{};
        const bool seated = VR::GetSettings().seated;
        config.radius_m = qs.radius_m * (seated ? 0.875f : 1.0f);
        config.card_m = qs.card_m;
        config.span_deg = qs.span_deg;
        rs.layout = Q::PlaceArc(n, hand, eye, config);
        LOG_INFO(Core,
                 "VR quick: arc {} placed: {} cards around the {} ({:.3f},{:.3f},{:.3f}) LOCAL, "
                 "eye ({:.3f},{:.3f},{:.3f}), pivot ({:.3f},{:.3f},{:.3f}), palm "
                 "({:.3f},{:.3f},{:.3f}), radius {:.3f} span {:.0f} deg, {}, arm {:.2f}{} | t "
                 "{:.3f}",
                 scene.open_serial, n, grip_found ? "grip" : "palm", hand.x, hand.y, hand.z,
                 eye.x, eye.y, eye.z, rs.layout.pivot.x, rs.layout.pivot.y, rs.layout.pivot.z,
                 palm.x, palm.y, palm.z, config.radius_m, config.span_deg,
                 PickSector(qs) ? "sector" : "hover", status.arm_weight[0],
                 rs.waiting_serial == scene.open_serial
                     ? fmt::format(" after waiting {} ms",
                                   (now - scene.state_ns) / Ms)
                     : std::string{},
                 static_cast<double>(now) * 1e-9);
        if (g_log >= 2) {
            for (int i = 0; i < n; ++i) {
                const Q::V3 c = rs.layout.card[static_cast<size_t>(i)];
                LOG_INFO(Core, "VR quick: card {} (slot {}) at ({:.3f},{:.3f},{:.3f}) LOCAL", i,
                         rs.card_slot[static_cast<size_t>(i)], c.x, c.y, c.z);
            }
        }
    }
    if (open && rs.placed_serial == scene.open_serial) {
        // Only the player's own hand hovers (the arm follows the controller): not the game's
        // animation sweeping it through the cards (a dodge, a hit), nor the hand as the arc
        // unfolds around it (80 ms).
        int hover = -1;
        if (arm && now - rs.placed_ns >= 80 * Ms) {
            hover = PickSector(qs) ? Q::PickSector(rs.layout, hand, rs.hover)
                                   : Q::PickHover(rs.layout, hand, eye, rs.hover, qs.hover_scale);
        }
        if (hover != rs.hover && g_log >= 1) {
            // The card entered (or left): how far the hand is from it.
            const int k = hover >= 0 ? hover : rs.hover;
            const Q::V3 c = rs.layout.card[static_cast<size_t>(k)];
            LOG_INFO(Core,
                     "VR quick: hover {} -> {} ({} ({:.3f},{:.3f},{:.3f}) {:.1f} cm from card {}, "
                     "metric {:.2f}; palm ({:.3f},{:.3f},{:.3f})) | t {:.3f}",
                     rs.hover < 0 ? -1 : rs.card_slot[static_cast<size_t>(rs.hover)],
                     hover < 0 ? -1 : rs.card_slot[static_cast<size_t>(hover)],
                     grip_found ? "grip" : "palm", hand.x, hand.y, hand.z,
                     100.0f * Q::Length(hand - c), rs.card_slot[static_cast<size_t>(k)],
                     Q::HoverMetric(hand, c, Q::Normalize(eye - c), qs.hover_scale), palm.x,
                     palm.y, palm.z, static_cast<double>(now) * 1e-9);
        }
        rs.hover = hover;
        if (g_log >= 2 && map.carry_on && now >= rs.carry_log_ns) {
            // inframe_hand_sync: how far the hover point moved to the fist as drawn (LOCAL).
            rs.carry_log_ns = now + 250 * Ms;
            const Q::V3 raw = map.ToTracking(grip_found ? grip_world : palm_world);
            LOG_INFO(Core,
                     "VR quick: hover point ({:.3f},{:.3f},{:.3f}) LOCAL, carried {:.1f} cm from "
                     "the pose's ({:.3f},{:.3f},{:.3f}) | t {:.3f}",
                     hand.x, hand.y, hand.z, 100.0f * Q::Length(hand - raw), raw.x, raw.y, raw.z,
                     static_cast<double>(now) * 1e-9);
        }
        Q::Hover h{};
        h.open_serial = scene.open_serial;
        h.steady_ns = now;
        h.placed = true;
        h.placed_ns = rs.placed_ns;
        h.slot = static_cast<s8>(hover < 0 ? -1 : rs.card_slot[static_cast<size_t>(hover)]);
        h.hand = {hand.x, hand.y, hand.z};
        h.centre = {rs.layout.pivot.x, rs.layout.pivot.y, rs.layout.pivot.z};
        h.eye = {eye.x, eye.y, eye.z};
        h.scale = qs.hover_scale;
        h.card_count = static_cast<u8>(rs.layout.n);
        h.card_m = qs.card_m * (VR::GetSettings().seated ? 0.9f : 1.0f) * 1.25f; // hover-grown
        for (int i = 0; i < rs.layout.n; ++i) {
            const Q::V3 c = rs.layout.card[static_cast<size_t>(i)];
            h.cards[static_cast<size_t>(i)] = {c.x, c.y, c.z};
            h.card_slot[static_cast<size_t>(i)] = rs.card_slot[static_cast<size_t>(i)];
        }
        Q::PublishHover(h);
    }
    if (!qs.inframe) {
        return;
    }
    F::List list{};
    list.record = record_id;
    list.steady_ns = now;
    list.exposure = qs.inframe_exposure;
    if (holsters && status_fresh && status.driven && status.mounts.valid &&
        status.mounts.character.valid && status.mounts.space == rec.space_serial) {
        // Keep the root transform and mount bones from one pose update. ChrCtrl may
        // already have advanced while the rendered body/dummies still use that pose.
        const auto* pgd = Read<u8*>(chr + OffsetPlayerGameData);
        const s32 chest_armor = Plausible(pgd)
                                    ? Read<s32>(pgd + OffsetEgd + OffsetChestArmor)
                                    : -1;
        mounts.targets = BuildHolsters(list, map, scene, snap, status.mounts, now, chest_armor);
    }
    if ((open || folding) && rs.placed_serial == scene.open_serial) {
        BuildArc(list, map, scene, snap, now, map.scale);
    }
    VR::MusicBox::Scene music_box{};
    if (held_mesh && scene.held_goods == VR::MusicBox::Goods &&
        VR::MusicBox::LatestScene(music_box) && VR::MusicBox::ActiveAt(music_box, now)) {
        // quick_music_box_crank: the box and its crank (the presenter's LOCAL pose).
        const u32 first = list.count;
        std::array<float, 12> palm_m{};
        std::memcpy(palm_m.data(), m.data(), sizeof(palm_m));
        BuildMusicBox(list, map, music_box, now, map.scale, palm_m);
        CaptureGripLab(list, map, palm_m, first, scene.held_goods);
        MarkHand(list, first);
    } else if (held_mesh) {
        std::array<float, 12> palm_m{};
        std::memcpy(palm_m.data(), m.data(), sizeof(palm_m));
        const u32 first = list.count;
        BuildHeld(list, map, palm_m, scene, snap, now, map.scale);
        CaptureGripLab(list, map, palm_m, first, scene.held_goods);
        MarkHand(list, first); // (in his fist: inframe_hand_sync carries it, F::Carry)
    } else if (flashing) {
        AddHoldRing(list, map, map.WorldToView(g_ring_place.at), g_ring_place.size, scene, now,
                    false);
    }
    if (drain) {
        std::array<float, 12> palm_m{};
        std::memcpy(palm_m.data(), m.data(), sizeof(palm_m));
        const u32 first = list.count;
        BuildVialDrain(list, map, palm_m, now, drain_word);
        CaptureGripLab(list, map, palm_m, first, 1000);
        MarkHand(list, first);
    }
    if (g_markers) {
        // 4 cm axes at the palm dummy (X red, Y green, Z blue), a 1 cm dot at the arc's pivot.
        const Q::V3 o = map.WorldToView(palm_world);
        const std::array<VR::Body::Vec3, 3> axes{VR::Body::Vec3{m[0], m[4], m[8]},
                                                 VR::Body::Vec3{m[1], m[5], m[9]},
                                                 VR::Body::Vec3{m[2], m[6], m[10]}};
        for (int a = 0; a < 3; ++a) {
            const Q::V3 y = Q::Normalize(map.DirToView(axes[static_cast<size_t>(a)]));
            const Q::V3 x = Q::Normalize(Q::Cross(y, std::abs(y.z) < 0.9f ? Q::V3{0, 0, 1}
                                                                          : Q::V3{1, 0, 0}));
            const Q::V3 z = Q::Cross(x, y);
            F::Item axis = MakeItem(F::MeshId::Axis, o, x, y, z, 0.04f * map.scale,
                                    0.04f * map.scale, 0.04f * map.scale);
            axis.shade = F::Shade::Unlit;
            axis.color = {a == 0 ? 1.0f : 0.0f, a == 1 ? 1.0f : 0.0f, a == 2 ? 1.0f : 0.0f, 1.0f};
            list.Add(axis);
        }
        if (rs.placed_serial == scene.open_serial && scene.open_serial != 0) {
            const Q::V3 c = map.WorldToView(map.ToWorld(rs.layout.pivot));
            F::Item dot = MakeItem(F::MeshId::Pellet, c, {1, 0, 0}, {0, 1, 0}, {0, 0, 1},
                                   0.8f * map.scale, 0.8f * map.scale, 0.8f * map.scale);
            dot.shade = F::Shade::Unlit;
            dot.color = {1.0f, 0.0f, 1.0f, 1.0f};
            list.Add(dot);
        }
    }
    if (list.count > 0) {
        F::Submit(list);
    }
}

} // namespace VR::Bloodborne
