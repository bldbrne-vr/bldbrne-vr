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
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "common/thread.h"
#include "core/vr/bloodborne_gun.h"
#include "core/vr/bloodborne_held.h"
#include "core/vr/bloodborne_iframes.h"
#include "core/vr/bloodborne_quickitems.h"
#include "core/vr/bloodborne_tuning.h"
#include "core/vr/body_channel.h"
#include "core/vr/body_settings.h"
#include "core/vr/bow.h"
#include "core/vr/bow_channel.h"
#include "core/vr/combat_channel.h"
#include "core/vr/gun_aim.h"
#include "core/vr/gun_settings.h"
#include "core/vr/hook_util.h"
#include "core/vr/melee_settings.h"
#include "core/vr/melee_window.h"
#include "core/vr/shield_guard.h"
#include "core/vr/trick_weapons.h"
#include "core/vr/vr_session.h"

namespace VR::Bloodborne {

namespace {

namespace B = VR::Body;
namespace G = VR::Gun;
using Hook::Plausible;
using Hook::Read;

// Module-relative addresses in the 1.09 eboot (patch file address - 0x400000). Every signature is
// unique in .text and identical in the original and the -mods eboot (combat/gunfire/uniq.py).

// The emitter builder 0x19F2440, right after its `call 0x18C7800` (the emitter matrix into
// [rbp-0x90], a 3x4 row-major [R | t]; rbx = the launcher, r15 = the owner). The 21 patched bytes
// (two row copies and a load) are replayed after our call, so they copy the rewritten rows; no
// branch lands inside them (combat/site_check.py). The window starts 25 bytes before the site.
constexpr u64 EmitSigRva = 0x19F2621;
constexpr u64 EmitSiteRva = 0x19F263A;
constexpr u64 EmitResumeRva = 0x19F264F;
constexpr size_t EmitLead = 25;
constexpr size_t EmitLen = 21;
constexpr std::array<u8, 54> EmitSig = {
    0x0F, 0xB6, 0x8B, 0xD0, 0x02, 0x00, 0x00,       // movzx ecx, byte [rbx+0x2D0]
    0x48, 0x8D, 0xBD, 0x70, 0xFF, 0xFF, 0xFF,       // lea rdi, [rbp-0x90]
    0x4C, 0x89, 0xFE,                               // mov rsi, r15
    0x44, 0x89, 0xE2,                               // mov edx, r12d
    0xE8, 0xC6, 0x51, 0xED, 0xFF,                   // call 0x18C7800
    0xC5, 0xF8, 0x28, 0x85, 0x70, 0xFF, 0xFF, 0xFF, // vmovaps xmm0, [rbp-0x90]  <- site
    0xC5, 0xF8, 0x29, 0x85, 0x30, 0xFF, 0xFF, 0xFF, // vmovaps [rbp-0xD0], xmm0
    0xC5, 0xF8, 0x28, 0x45, 0x80,                   // vmovaps xmm0, [rbp-0x80]
    0xC5, 0xF8, 0x29, 0x85, 0x40, 0xFF, 0xFF, 0xFF, // vmovaps [rbp-0xC0], xmm0  <- resume
};
constexpr s32 EmitOutSlot = -0x90;      // the matrix
constexpr s32 ShotIndexFromOut = -0x60; // f32 [rbp-0xF0]: the shot's index (spread fans)

// InvokeGun, `bool(chr, bid, dummy, dir != 0, hand_a, hand_b, first, not_sustained)`: TAE event
// 318's call (BehaviorParam -> 0x18B61C0 -> 0x18B6680 -> 0x18B7250 -> CreateBullet).
constexpr u64 InvokeGunRva = 0x18C7530;
constexpr std::array<u8, 44> InvokeGunSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81,
    0xEC, 0xC8, 0x00, 0x00, 0x00, 0x44, 0x89, 0x8D, 0x1C, 0xFF, 0xFF, 0xFF, 0x44, 0x89, 0x85,
    0x20, 0xFF, 0xFF, 0xFF, 0x41, 0x89, 0xCC, 0x41, 0x89, 0xD5, 0x89, 0xF3, 0x49, 0x89,
};
// Event 318's call block (0x1A265F7): rdi = chr, esi = bid, edx = dummy, ecx = dir, r8d = r9d =
// hand, [rsp] = first, [rsp+8] = not sustained, call 0x18C7530: the argument order.
constexpr u64 Call318Rva = 0x1A265F7;
constexpr std::array<u8, 66> Call318Sig = {
    0x80, 0xBD, 0x40, 0xFE, 0xFF, 0xFF, 0x00, 0x0F, 0x95, 0xC0, 0x41, 0x80, 0x7E, 0x0D,
    0x00, 0x0F, 0x94, 0xC1, 0x49, 0x8B, 0x7F, 0x10, 0x41, 0x8B, 0x16, 0x0F, 0xB6, 0xC9,
    0x89, 0x4C, 0x24, 0x08, 0x0F, 0xB6, 0x8D, 0x60, 0xFE, 0xFF, 0xFF, 0x89, 0x0C, 0x24,
    0x0F, 0xB6, 0xC8, 0x8B, 0xB5, 0x50, 0xFE, 0xFF, 0xFF, 0x44, 0x8B, 0x85, 0x4C, 0xFE,
    0xFF, 0xFF, 0x45, 0x89, 0xC1, 0xE8, 0xF7, 0x0E, 0xEA, 0xFF,
};
// ConsumeBullets, `bool(PlayerIns*, s32 n)`: Blood Bullets (901) first, then Quicksilver (900);
// short: nothing taken, 0.
constexpr u64 ConsumeBulletsRva = 0x18F9F00;
constexpr std::array<u8, 40> ConsumeBulletsSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
    0x83, 0xEC, 0x18, 0x41, 0x89, 0xF5, 0x48, 0x89, 0xFB, 0x4C, 0x8B, 0x25, 0xC2, 0x9C,
    0xAE, 0x03, 0x49, 0x8B, 0x04, 0x24, 0x48, 0x89, 0x45, 0xD0, 0x48, 0x8B,
};
// CanFireGun, `bool(PlayerIns*, s32 right)` (HKS env 0x56): bullets >= the hand's
// bulletConsumeNum.
constexpr u64 CanFireGunRva = 0x18FA100;
constexpr std::array<u8, 40> CanFireGunSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
    0x83, 0xEC, 0x48, 0x48, 0x89, 0xFB, 0x4C, 0x8B, 0x3D, 0xC5, 0x9A, 0xAE, 0x03, 0x49,
    0x8B, 0x07, 0x48, 0x89, 0x45, 0xD0, 0x48, 0x8B, 0x83, 0xB8, 0x04, 0x00,
};
// PlaySound, `void(u64* handle_out, const float pos[3], s32 type, s32 id)` (TAE event 128's call;
// the handle is never released there either).
constexpr u64 PlaySoundRva = 0x13E0290;
constexpr std::array<u8, 24> PlaySoundSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x81, 0xEC,
    0x28, 0x01, 0x00, 0x00, 0x49, 0x89, 0xF6, 0x48, 0x89, 0xFB, 0x4C, 0x8B,
};
// Event 128's sound type: `lea rcx, [table]; cmp byte [rcx+3], 0; mov ecx, 0xA; jnz; mov ecx, 1`.
constexpr u64 SoundTypeRva = 0x1A2641B;
constexpr std::array<u8, 23> SoundTypeSig = {
    0x48, 0x8D, 0x0D, 0x26, 0xE5, 0xD0, 0x02, 0x80, 0x79, 0x03, 0x00, 0xB9,
    0x0A, 0x00, 0x00, 0x00, 0x75, 0x05, 0xB9, 0x01, 0x00, 0x00, 0x00,
};
// PlayerIns vt+0x1D0 GetBehaviorId, `s32(chr, judge, hand, 0, 0)` = 1e8 + variation * 1000 +
// judge, and vt+0x388 GetWeaponId, `s32(chr, hand)`: each called through the live vtable only
// when its slot holds this function.
constexpr u64 GetBehaviorIdRva = 0x18F9070;
constexpr std::array<u8, 28> GetBehaviorIdSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
    0x83, 0xEC, 0x78, 0x44, 0x89, 0xC3, 0x41, 0x89, 0xCE, 0x41, 0x89, 0xD5, 0x41, 0x89,
};
constexpr u32 SlotGetBehaviorId = 0x1D0;
constexpr u64 GetWeaponIdRva = 0x18FB090;
constexpr std::array<u8, 15> GetWeaponIdSig = {
    0x48, 0x8B, 0x87, 0xB8, 0x04, 0x00, 0x00, 0x8D, 0x4E, 0x06, 0x83, 0xF9, 0x05, 0x77, 0x3E,
};
constexpr u32 SlotGetWeaponId = 0x388;
// BehaviorParamRow, `void(Out{s32 -1; s32 isPc; u8* row}*, s32 bid, s32 isPc)`.
constexpr u64 BehaviorRowRva = 0x1F133C0;
constexpr std::array<u8, 32> BehaviorRowSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x89, 0xD3, 0x41, 0x89, 0xF6,
    0x49, 0x89, 0xFF, 0x31, 0xC9, 0x41, 0x89, 0x5F, 0x04, 0x45, 0x85, 0xF6, 0x0F, 0x88, 0x08, 0x01,
};
constexpr u32 RowRefType = 0x09;  // u8: 1 = a bullet
constexpr u32 RowRefId = 0x0C;    // s32: the BulletParam id
constexpr u32 RowStamina = 0x14;  // s32: the stamina it costs (the layout's next field after the
                                  // effect variation; the bow's first shot logs it)
constexpr u32 RowCategory = 0x1C; // u8: 2 = a gun
// WeaponParamRow, `void(Out*, s32 weapon)` (bloodborne_givekit.cpp): bulletConsumeNum at +0x13A.
constexpr u64 WeaponRowRva = 0x1F29AA0;
constexpr std::array<u8, 32> WeaponRowSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50, 0x41, 0x89,
    0xF4, 0x49, 0x89, 0xFE, 0x45, 0x31, 0xC9, 0x41, 0xBF, 0xFF, 0xFF, 0xFF, 0xFF, 0x41, 0xBA, 0xFF,
};
constexpr u32 WeaponBulletCost = 0x13A; // s8
// SfxSlotRequest (bloodborne_held.cpp has the full story): `void(SfxCtl*, s32 slot, s32 ffx,
// const s32* dummy, u8 repeat)`, the call TAE event 96 makes; a slot effect lives while it is
// asked for every update.
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
// ChrIns constructor (0x18B9F05): the effect controller's vtable stored at chr+0x2A0.
constexpr u64 CtlCtorRva = 0x18B9F05;
constexpr std::array<u8, 14> CtlCtorSig = {
    0x48, 0x8D, 0x05, 0x84, 0x50, 0xA7, 0x03, 0x49, 0x89, 0x85, 0xA0, 0x02, 0x00, 0x00,
};
constexpr u64 SfxCtlVtableRva = 0x532EF90;
// TAE event 225 SetSPRegenRatePercent (0x1A22200): `[[[chr+0x58]+0x38]+0x34] = percent`; the
// player's update resets it to 100 every frame, so a pause is a 0 written every update.
constexpr u64 RegenStoreRva = 0x1A22200;
constexpr std::array<u8, 15> RegenStoreSig = {
    0x48, 0x8B, 0x40, 0x58, 0x48, 0x8B, 0x40, 0x38, 0x41, 0x8A, 0x0C, 0x24, 0x88, 0x48, 0x34,
};
// ChrIns::GetDmyPolyMtx, `s32(ChrIns*, Mtx34* out, const s32* id, u32 max)`.
constexpr u64 DmyPolyRva = 0x18C3BF0;
constexpr std::array<u8, 32> DmyPolySig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC,
    0x28, 0x41, 0x89, 0xCF, 0x48, 0x89, 0xD3, 0x48, 0x89, 0x75, 0xB0, 0x4C, 0x8B, 0x35, 0xCE, 0xFF,
};
// FindItemIndex(inventory, const u32* item): the inventory index or -1 (the logs' bullet counts).
constexpr u64 FindItemRva = 0x14D9F60;
constexpr std::array<u8, 32> FindItemSig = {
    0x8B, 0x06, 0x83, 0xF8, 0xFF, 0x0F, 0x84, 0xA6, 0x00, 0x00, 0x00, 0x89, 0xC1, 0x48, 0x69, 0xC9,
    0x15, 0x3F, 0xEF, 0x03, 0x48, 0xC1, 0xE9, 0x20, 0x89, 0xC2, 0x29, 0xCA, 0xD1, 0xEA, 0x01, 0xCA,
};
// ChrIns::IsMainPlayer: its lea proves the WorldChrMan global.
constexpr u64 IsMainPlayerRva = 0x18C2D00;
constexpr std::array<u8, 24> IsMainPlayerSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xFB, 0x4C, 0x8D,
    0x35, 0x67, 0xBB, 0xC7, 0x03, 0x49, 0x8B, 0x06, 0x48, 0x85, 0xC0, 0x75,
};
constexpr u64 WorldChrManRva = 0x553E878;

// ChrIns / PlayerIns.
constexpr u32 OffsetMainPlayer = 0x60; // WorldChrMan -> the main PlayerIns
constexpr u32 OffsetHandle = 0x08;
constexpr u32 OffsetChrCtrl = 0x58;
constexpr u32 OffsetCtrlWorld = 0x90; // W: three float4 rows {R row i, t_i}
constexpr u32 OffsetCtrlRegen = 0x38; // ChrCtrl -> +0x34 the stamina regen percent (u8)
constexpr u32 OffsetRegenPercent = 0x34;
constexpr u32 OffsetArmStyle = 0xF0; // [[+0xF0]] & ~1 == 2: two-handed (the gun holstered)
constexpr u32 OffsetLockChosen = 0xF8;
constexpr u32 OffsetLockPoint = 0x100;
constexpr u32 OffsetSpEffects = 0x1C8;
constexpr u32 OffsetSfxCtl = 0x2A0;
constexpr u32 OffsetChrModules = 0x3B0; // +0x20 = ChrDataModule
constexpr u32 OffsetDataModule = 0x20;
constexpr u32 OffsetHp = 0xF8;
constexpr u32 OffsetStamina = 0x134;
constexpr u32 OffsetPlayerGameData = 0x3C0;
constexpr u32 OffsetInventory = 0x328; // PlayerGameData: the inventory FindItemIndex takes
// Inventory: 16-byte entries {handle (0 = gone), item, qty, sort}; key items first.
constexpr u32 OffsetInvCapacity = 0x10;
constexpr u32 OffsetInvKeyCap = 0x24;
constexpr u32 OffsetInvNormalList = 0x48;
constexpr u32 OffsetInvKeyList = 0x58;
constexpr u32 OffsetInvHighest = 0x88;
// SpEffect list nodes (as bloodborne_quickitems.cpp walks them).
constexpr u32 OffsetEffectHead = 0x08;
constexpr u32 OffsetEffectFlags = 0x1C;
constexpr u32 OffsetEffectId = 0x40;
constexpr u32 OffsetEffectNext = 0x58;
constexpr u32 EffectSkipMask = 0x800C0003;
constexpr s32 BoneMarrowAsh = 2230; // the SpEffect the Ash puts on (its bullets take judge 810)
// The launcher (0x19F2440's rbx): BehaviorParam id, bullet id, owner handle, category.
constexpr u32 OffsetLauncherBid = 0x1B0;
constexpr u32 OffsetLauncherBullet = 0x1B4;
constexpr u32 OffsetLauncherOwner = 0x1BC;
constexpr u32 OffsetLauncherCategory = 0x1E4; // s32: 2 = a gun's BehaviorParam category
constexpr s32 CategoryGun = 2;
// A launched projectile (the launch tail's P): position, velocity, bullet id, owner handle.
constexpr u32 OffsetPPos = 0x10;
constexpr u32 OffsetPVel = 0x40;
constexpr u32 OffsetPBullet = 0x2F4;
constexpr u32 OffsetPOwner = 0x2FC;

constexpr u32 GoodsCategory = 0x40000000u;
constexpr s32 QuicksilverBullets = 900;
constexpr s32 BloodBullets = 901;
constexpr s32 HandLeft = -2;
constexpr s32 HandRight = -1;
constexpr s32 JudgeShot = 800;
constexpr s32 JudgeAsh = 810;
constexpr s32 PalmDummy = 3;          // L_Hand: the bullet's start (TAE 318 dummy)
constexpr s32 PartLineDummy = 20100;  // the left part's dummies 100 (behind) .. 215 (the muzzle)
constexpr s32 FirstFlashSlot = 30100; // private effect slots (the vial uses 30000-30007)
constexpr s32 FlashSlots = 4;
constexpr s32 FirstBladeFlashSlot = 30110; // the gun-blades' flash: 30110-30113
constexpr float DryCooldownS = 0.3f;
// The palm (the game's start) sits 7-8 cm below the pistol's bore; a larger move means the muzzle
// dummy is not where this hand's gun would have it.
constexpr float MaxOriginShiftM = 0.12f;

constexpr s64 Ms = 1'000'000;
constexpr s64 DirectMaxAgeNs = 150 * Ms; // a direct shot's launch must come this soon
// The game's own (vanilla-timed) shot takes its pull's grip until then: its draw (0.3-0.5 s) after
// the press, or after a dodge or a hit the game first finishes (gun_busy=vanilla).
constexpr s64 PullPoseMaxAgeNs = 1500 * Ms;
constexpr s64 HandsMaxAgeNs = 100 * Ms;
constexpr s64 AnchorMaxAgeNs = 200 * Ms;
constexpr s64 StatusMaxAgeNs = 250 * Ms;
constexpr s64 KickNs = 600 * Ms;
constexpr s64 StalePullNs = 500 * Ms; // a pull the pose tick first sees this late is ignored
constexpr s64 StatsNs = 10000 * Ms;

using InvokeGunFn = u8(PS4_SYSV_ABI*)(void* chr, s32 bid, s32 dummy, u32 dir, s32 hand_a,
                                      s32 hand_b, u32 first, u32 not_sustained);
using ConsumeBulletsFn = u8(PS4_SYSV_ABI*)(void* chr, s32 n);
using CanFireGunFn = u8(PS4_SYSV_ABI*)(void* chr, s32 right);
using PlaySoundFn = void(PS4_SYSV_ABI*)(u64* handle, const float* pos, s32 type, s32 id);
using GetBehaviorIdFn = s32(PS4_SYSV_ABI*)(void* chr, s32 judge, s32 hand, s32 a, s32 b);
using GetWeaponIdFn = s32(PS4_SYSV_ABI*)(void* chr, s32 hand);
struct RowOut {
    s32 id{-1};
    s32 is_pc{1};
    u8* row{nullptr};
};
using BehaviorRowFn = void(PS4_SYSV_ABI*)(RowOut* out, s32 bid, s32 is_pc);
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
using SfxSlotRequestFn = void(PS4_SYSV_ABI*)(void* ctl, s32 slot, s32 ffx, const s32* dummy,
                                             u8 repeat);
using DmyPolyFn = s32(PS4_SYSV_ABI*)(void* chr, void* out, const s32* ids, u32 max);
using FindItemFn = s32(PS4_SYSV_ABI*)(u8* inventory, const u32* item);

// ---- Install state (set before anything can run) ------------------------------------------------

u8* g_base = nullptr;
bool g_aim = false;  // the emitter detour is in
bool g_fire = false; // the direct-fire functions matched
bool g_regen = false;
bool g_sound = false;
bool g_flash = false;
std::unique_ptr<Xbyak::CodeGenerator> g_code;
InvokeGunFn g_invoke_gun = nullptr;
ConsumeBulletsFn g_consume = nullptr;
CanFireGunFn g_can_fire = nullptr;
PlaySoundFn g_play_sound = nullptr;
BehaviorRowFn g_behavior_row = nullptr;
WeaponRowFn g_weapon_row = nullptr;
SfxSlotRequestFn g_sfx_slot = nullptr;
DmyPolyFn g_dmy_poly = nullptr;
FindItemFn g_find_item = nullptr;
const u8* g_sound_table = nullptr; // event 128's sound type byte table
// VR_GUN_LOG: 1 = every shot's aim and launch lines, 2 = + every child bullet, 3 = + the kick on
// every update
int g_log = 0;
// VR_GUN_DRYRUN=1: everything computed and logged, nothing written or fired; =aim: the shots fire
// but keep the game's own direction (the aim control).
bool g_dry_run = false;
bool g_dry_aim = false;

using Clock = std::chrono::steady_clock;

s64 NowNs() {
    return B::SteadyNowNs();
}

std::string V3(B::Vec3 v) {
    return fmt::format("({:.3f},{:.3f},{:.3f})", v.x, v.y, v.z);
}

// Yaw / pitch in degrees, the game's yaw convention (atan2(x, z)).
std::string YawPitch(B::Vec3 d) {
    return fmt::format("({:.1f},{:.1f})", G::YawOf(d) / B::Deg, G::PitchOf(d) / B::Deg);
}

u8* MainPlayer() {
    if (g_base == nullptr) {
        return nullptr;
    }
    u8* const world = Read<u8*>(g_base + WorldChrManRva);
    u8* const chr = Plausible(world) ? Read<u8*>(world + OffsetMainPlayer) : nullptr;
    return Plausible(chr) ? chr : nullptr;
}

u8* DataModule(const u8* chr) {
    u8* const modules = Read<u8*>(chr + OffsetChrModules);
    u8* const data = Plausible(modules) ? Read<u8*>(modules + OffsetDataModule) : nullptr;
    return Plausible(data) ? data : nullptr;
}

B::Vec3 Read3(const u8* p) {
    return {Read<float>(p), Read<float>(p + 4), Read<float>(p + 8)};
}

// A dummy poly's world position and its matrix (3x4 rows), or false when the game has none.
bool DummyAt(u8* chr, s32 id, B::Vec3& out, std::array<float, 12>* rows = nullptr) {
    if (g_dmy_poly == nullptr) {
        return false;
    }
    alignas(16) std::array<float, 8 * 12> m{};
    if (g_dmy_poly(chr, m.data(), &id, 1) < 1) {
        return false;
    }
    out = {m[3], m[7], m[11]};
    if (rows != nullptr) {
        std::copy_n(m.begin(), 12, rows->begin());
    }
    return B::IsFinite(out);
}

// The hunter's forward (W's -column 2): the look-at's up for a ray straight up or down.
B::Vec3 ChrForward(const u8* chr) {
    u8* const ctrl = Read<u8*>(chr + OffsetChrCtrl);
    if (!Plausible(ctrl)) {
        return {0.0f, 0.0f, 1.0f};
    }
    const u8* w = ctrl + OffsetCtrlWorld;
    return B::Normalize(
        B::Vec3{-Read<float>(w + 0x08), -Read<float>(w + 0x18), -Read<float>(w + 0x28)},
        {0.0f, 0.0f, 1.0f});
}

bool HasEffect(const u8* chr, s32 id) {
    u8* const list = Read<u8*>(chr + OffsetSpEffects);
    if (!Plausible(list)) {
        return false;
    }
    u8* node = Read<u8*>(list + OffsetEffectHead);
    for (u32 i = 0; i < 512 && Plausible(node); ++i) {
        if ((Read<u32>(node + OffsetEffectFlags) & EffectSkipMask) == 0 &&
            Read<s32>(node + OffsetEffectId) == id) {
            return true;
        }
        node = Read<u8*>(node + OffsetEffectNext);
    }
    return false;
}

// How many of a goods the hunter carries (logs), -1 when unreadable.
s32 GoodsCount(const u8* chr, s32 goods) {
    if (g_find_item == nullptr) {
        return -1;
    }
    u8* const pgd = Read<u8*>(chr + OffsetPlayerGameData);
    if (!Plausible(pgd)) {
        return -1;
    }
    u8* const inv = pgd + OffsetInventory;
    const u32 item = GoodsCategory | static_cast<u32>(goods);
    const s32 index = g_find_item(inv, &item);
    if (index < 0 || Read<u32>(inv + OffsetInvCapacity) == 0 ||
        static_cast<u32>(index) > Read<u32>(inv + OffsetInvHighest)) {
        return 0;
    }
    const u32 key_cap = Read<u32>(inv + OffsetInvKeyCap);
    const u32 i = static_cast<u32>(index);
    u8* const list = Read<u8*>(inv + (i < key_cap ? OffsetInvKeyList : OffsetInvNormalList));
    if (!Plausible(list)) {
        return -1;
    }
    const u8* e = list + static_cast<size_t>(i < key_cap ? i : i - key_cap) * 16;
    return Read<u32>(e) != 0 && Read<u32>(e + 4) == item ? Read<s32>(e + 8) : 0;
}

bool TwoHanded(const u8* chr) {
    u8* const style = Read<u8*>(chr + OffsetArmStyle);
    return Plausible(style, 4) && (Read<u32>(style) & ~1u) == 2;
}

// The live PlayerIns vtable slot, when it holds the function we checked.
template <typename Fn>
Fn VtableFn(const u8* chr, u32 slot, u64 rva) {
    u8* const vtable = Read<u8*>(chr);
    if (!Plausible(vtable)) {
        return nullptr;
    }
    u8* const fn = Read<u8*>(vtable + slot);
    return fn == g_base + rva ? reinterpret_cast<Fn>(fn) : nullptr;
}

s32 BehaviorId(u8* chr, s32 judge) {
    const auto fn = VtableFn<GetBehaviorIdFn>(chr, SlotGetBehaviorId, GetBehaviorIdRva);
    return fn != nullptr ? fn(chr, judge, HandLeft, 0, 0) : -1;
}

// A hand's weapon's bullets per shot (EquipParamWeapon.bulletConsumeNum), or `fallback`.
s32 BulletCost(u8* chr, s32 fallback, s32 hand = HandLeft) {
    const auto weapon_id = VtableFn<GetWeaponIdFn>(chr, SlotGetWeaponId, GetWeaponIdRva);
    if (weapon_id == nullptr || g_weapon_row == nullptr) {
        return fallback;
    }
    const s32 id = weapon_id(chr, hand);
    if (id < 0) {
        return fallback;
    }
    WeaponRowOut out{};
    g_weapon_row(&out, id);
    if (!Plausible(out.row, 2)) {
        return fallback;
    }
    const s32 cost = Read<s8>(out.row + WeaponBulletCost);
    return cost > 0 && cost <= 20 ? cost : fallback;
}

// ---- The barrel ray (shared by the emitter hook and the pose tick) ------------------------------

// The grip-frame barrel of `attach` through the body's left grip (the global trims; the left hand
// has no per-weapon grip).
B::Vec3 BarrelInGripNow(s32 attach) {
    const B::GripTrim trim = B::DefaultWeaponGrip();
    return G::BarrelInGrip(attach, VR::GetSettings().blade_pitch_deg, trim.yaw_deg, trim.pitch_deg,
                           trim.roll_deg);
}

// What the shot's ray is made of: the pull's grip (direct, or the newest pull for the game's own
// shot) or the newest hand sample.
struct RaySource {
    bool ok{false};
    const char* src{""}; // "pull", "now"
    const char* why{""}; // the vanilla reason when !ok
    B::Quat q{};
    s64 pull_ns{0};
    u32 serial{0};
    s64 hands_age_ns{-1};
};

// ---- The direct shot of this frame, for the emitter hook (both on the game's main thread) ------

struct DirectShot {
    u32 serial{0};
    s32 bid{-1};
    s64 ns{0};      // the pose tick's call
    s64 pull_ns{0}; // the trigger edge
    B::Quat grip_q{};
    bool grip_valid{false};
    u32 space_serial{0};
};

std::mutex g_shot_mutex;
DirectShot g_direct{};
u32 g_game_shot_serial = 0; // the last pull the game's own shot took its grip from
Combat::GunPull g_handed{}; // a direct pull the pose tick handed to the game's L2 (its grip)

// The emitter's last ray, for the launch tail's readback (VR_GUN_LOG).
struct LastAim {
    u32 n{0};
    s32 bullet{-1};
    u32 owner{0};
    B::Vec3 ray{};      // the shot's centre ray (after the assist)
    B::Vec3 start{};    // where the bullet starts
    s64 ns{0};          // the emitter call
    s64 pull_ns{0};     // the trigger edge (0: none)
    u32 launches{0};    // matched so far (spread guns: one per pellet)
    s64 chain_until{0}; // child bullets are followed until then
    u32 children{0};
    bool direct{false};
};
std::atomic_bool g_aim_pending{false};
LastAim g_last{}; // under g_shot_mutex

// The recoil kick (written at the bang, read by the next updates' IK; one thread).
std::atomic<s64> g_kick_ns{0};
std::atomic<u8> g_kick_family{0};
std::atomic<s32> g_kick_attach{-1};

void StoreKick(G::Family family, s32 attach, s64 ns) {
    g_kick_family.store(static_cast<u8>(family), std::memory_order_relaxed);
    g_kick_attach.store(attach, std::memory_order_relaxed);
    g_kick_ns.store(ns, std::memory_order_release);
}

// The sword hand's kick at a blade's shot (BladeKick), as the gun's: written at the shot.
std::atomic<s64> g_blade_kick_ns{0};
std::atomic<s32> g_blade_kick_variation{-1};

// Statistics (the 10 s line).
struct Stats {
    std::atomic<u64> aimed{0};
    std::atomic<u64> assisted{0};
    std::atomic<u64> origin_moved{0};
    std::atomic<u64> vanilla_hands{0};
    std::atomic<u64> vanilla_view{0};
    std::atomic<u64> vanilla_arm{0};
    std::atomic<u64> vanilla_barrel{0};
    std::atomic<u64> fired{0};
    std::atomic<u64> fired_failed{0};
    std::atomic<u64> dry{0};
    std::atomic<u64> denied{0};
    std::atomic<u64> handed{0};
    std::atomic<u64> lines{0};
};
Stats g_stats{};

// Per-shot lines: at most 10 per second.
bool LineAllowed() {
    static std::atomic<s64> window{0};
    static std::atomic<u32> count{0};
    const s64 now = NowNs();
    if (now - window.load(std::memory_order_relaxed) >= 1000 * Ms) {
        window.store(now, std::memory_order_relaxed);
        count.store(0, std::memory_order_relaxed);
    }
    return count.fetch_add(1, std::memory_order_relaxed) < 10;
}

// ---- The emitter hook (0x19F263A, the game's main thread, inside the bullet launch) -------------

// ---- The bow's release sound (bow_direct, core/vr/bow.h) ------------------------------------------
// A direct arrow (BladeShotTick, no shot clip) plays the bow's own release sound: learned from the
// shots the game's own clip fires (the clip's sound at its arrow's launch, near the bow hand; the
// same one at two arrows, so a footstep or a blow near the hand at one is never taken), saved in
// vr.ini (bow_twang). A PlaySound detour (its 16-byte prologue, replayed) hears
// the game's sounds while the bow form is up and the sound is not known yet; until then the bow's
// shots are the game's own (the presenter: Combat::MeleeStatus::bow_direct).
constexpr s32 ThrowWhoosh = 751;          // the throws' release (a direct arrow before the twang)
constexpr s64 TwangBeforeNs = 100 * Ms;   // the release sound this long before the launch ...
constexpr s64 TwangAfterNs = 60 * Ms;     // ... or this long after it
constexpr float TwangNearM = 1.0f;        // ... this close to the bow hand
struct HeardSound {
    s32 type{0};
    s32 id{-1};
    B::Vec3 pos{};
    s64 ns{0};
};
std::mutex g_heard_mutex;
std::array<HeardSound, 8> g_heard{};
u32 g_heard_next{0};
s64 g_twang_launch_ns{0}; // a game-fired arrow still waiting for its sound (under g_heard_mutex)
B::Vec3 g_twang_hand{};
std::atomic<bool> g_twang_listen{false};
std::atomic<s32> g_twang{-1};
std::atomic<s32> g_twang_candidate{-1}; // heard at one arrow: learned when the next one plays it
std::atomic<bool> g_bow_no_rows{false}; // the bow has no arrow rows for a direct shot
std::unique_ptr<Xbyak::CodeGenerator> g_listen_code;

// The event-128 sound type the game's TAE sounds come with (1, or 0xA with the table's flag).
s32 TaeSoundType() {
    return g_sound_table != nullptr && Read<u8>(g_sound_table + 3) != 0 ? 0xA : 1;
}

void LearnTwang(s32 id, const char* how) {
    if (g_twang_candidate.exchange(id) != id) {
        LOG_INFO(Core, "VR bow: release sound {}? ({}): the next game-fired arrow confirms it", id,
                 how);
        return;
    }
    s32 none = -1;
    if (!g_twang.compare_exchange_strong(none, id)) {
        return;
    }
    VR::SaveSetting("bow_twang", std::to_string(id));
    LOG_INFO(Core, "VR bow: its release sound is {} ({}): saved as bow_twang; from now on the "
                   "arrow leaves at the release",
             id, how);
}

// A sound near the bow hand within the window round a game-fired arrow's launch (heard under
// g_heard_mutex).
bool TwangOf(const HeardSound& h, s64 launch_ns, const B::Vec3& hand) {
    return h.id >= 0 && h.type == TaeSoundType() && h.ns >= launch_ns - TwangBeforeNs &&
           h.ns <= launch_ns + TwangAfterNs && B::Length(h.pos - hand) <= TwangNearM;
}

void PS4_SYSV_ABI OnGameSound(s32 type, s32 id, const float* pos) {
    if (!g_twang_listen.load(std::memory_order_relaxed)) {
        return;
    }
    const bool p = Plausible(reinterpret_cast<const u8*>(pos), 12);
    const HeardSound h{type, id, p ? B::Vec3{pos[0], pos[1], pos[2]} : B::Vec3{}, NowNs()};
    s32 learned = -1;
    {
        std::scoped_lock lock{g_heard_mutex};
        g_heard[g_heard_next++ % g_heard.size()] = h;
        if (g_twang_launch_ns != 0 && TwangOf(h, g_twang_launch_ns, g_twang_hand)) {
            learned = id;
            g_twang_launch_ns = 0;
        }
    }
    if (learned >= 0) {
        LearnTwang(learned, "just after the game's arrow left");
    }
}

// The PlaySound detour (bow=1, bow_direct=1, its release sound not known yet). VR_SOUND_TRACE's
// own detour on the same prologue (tests) wins: then the bow's shots stay the game's own.
void InstallTwangListener(u8* module) {
    namespace W = VR::Bow;
    const auto& bs = W::GetBowSettings();
    g_twang.store(bs.twang);
    if (!g_sound || !bs.enabled || !bs.direct || bs.twang >= 0) {
        return;
    }
    constexpr size_t Prologue = 16;
    u8* const site = module + PlaySoundRva;
    if (std::getenv("VR_SOUND_TRACE") != nullptr ||
        std::memcmp(site, PlaySoundSig.data(), Prologue) != 0 ||
        !Hook::MakeWritable(site, Prologue)) {
        LOG_INFO(Core, "VR bow: its release sound cannot be heard (VR_SOUND_TRACE, or PlaySound "
                       "differs): its shots stay the game's own");
        return;
    }
    try {
        g_listen_code = std::make_unique<Xbyak::CodeGenerator>(1024);
        void* const stub = Hook::EmitHostCallStubWith(
            *g_listen_code, reinterpret_cast<void*>(&OnGameSound), PlaySoundSig.data(), Prologue,
            reinterpret_cast<u64>(site + Prologue), [](Xbyak::CodeGenerator& c) {
                using namespace Xbyak::util;
                c.mov(rdi, rdx); // type
                c.mov(rax, rsi);
                c.mov(rsi, rcx); // id
                c.mov(rdx, rax); // the position
            });
        g_listen_code->ready();
        Hook::WriteJump(site, Prologue, stub);
    } catch (const Xbyak::Error& error) {
        g_listen_code.reset();
        LOG_ERROR(Core, "VR bow: the sound listener's code failed ({})", error.what());
        return;
    }
    LOG_INFO(Core, "VR bow: listening for its release sound (the shots are the game's own until "
                   "two of them play the same one)");
}

// A game-fired arrow just left (BowArrow): the sound the clip played with it, now or just after.
void HearTwang(const B::Vec3& hand, s64 now) {
    if (g_twang.load(std::memory_order_relaxed) >= 0 || !g_twang_listen.load()) {
        return;
    }
    s32 learned = -1;
    {
        std::scoped_lock lock{g_heard_mutex};
        s64 best_ns = 0;
        for (const HeardSound& h : g_heard) {
            if (TwangOf(h, now, hand) && h.ns >= best_ns) {
                best_ns = h.ns;
                learned = h.id;
            }
        }
        if (learned < 0) {
            g_twang_launch_ns = now; // the sound may come after the launch in this update
            g_twang_hand = hand;
        }
    }
    if (learned >= 0) {
        LearnTwang(learned, "just before the game's arrow left");
    }
}

// Simon's Bowblade's arrows (vr.ini bow, core/vr/bow.h): a launch of its behaviour rows (variation
// 3200; the game's own shot, R1 / R2 pressed at the presenter's release) within BowShotMaxAgeNs
// of that release flies along the drawn line (string hand -> bow hand, just before the release)
// from the bow hand. True when the launch was the Bowblade's (aimed or not).
constexpr s32 BowbladeVariation = 3200;
constexpr s64 BowShotMaxAgeNs = 1500 * Ms;
constexpr float BowOriginAheadM = 0.10f; // the arrow starts this far ahead of the bow hand
u32 g_bow_aimed_serial = 0; // the release whose first arrow was aimed (the game's thread)

bool BowArrow(u8* out, u8* launcher, u8* player) {
    const s32 bid = Read<s32>(launcher + OffsetLauncherBid);
    if (bid < 100000000 || bid >= 200000000 || (bid - 100000000) / 1000 != BowbladeVariation) {
        return false;
    }
    const s64 now = NowNs();
    const float index_f = Read<float>(out + ShotIndexFromOut);
    const s32 idx = std::isfinite(index_f) ? static_cast<s32>(index_f) : 0;
    auto* const rows = reinterpret_cast<float*>(out);
    const B::Vec3 vanilla = G::FlightOfRows(rows);
    const s32 bullet = Read<s32>(launcher + OffsetLauncherBullet);
    Combat::BowShot shot{};
    const char* why = nullptr;
    if (!VR::Bow::GetBowSettings().enabled) {
        why = "bow=0";
    } else if (!Combat::BowShotChannel().Load(shot) || shot.serial == 0 ||
               now - shot.release_ns > BowShotMaxAgeNs) {
        why = "no release of yours";
    } else if (idx == 0 && shot.serial == g_bow_aimed_serial) {
        why = "its release already shot";
    }
    B::Anchor anchor{};
    if (why == nullptr &&
        (!B::LatestAnchor(anchor) || !anchor.valid || now - anchor.steady_ns > AnchorMaxAgeNs)) {
        why = "view";
    }
    B::HandsSample hands{};
    if (why == nullptr && B::LatestHands(hands) && hands.space_serial != shot.space_serial) {
        why = "recentered since";
    }
    if (why != nullptr) {
        if (idx == 0 && LineAllowed()) {
            LOG_INFO(Core, "VR bow: ARROW bid={} bullet={} idx={} along the game's aim {} ({})", bid,
                     bullet, idx, YawPitch(vanilla), why);
        }
        return true;
    }
    if (idx == 0) {
        g_bow_aimed_serial = shot.serial;
    }
    const B::Vec3 dir = B::Normalize(
        B::TrackingToWorld({shot.dir[0], shot.dir[1], shot.dir[2]}, anchor.base_yaw, 1.0f),
        vanilla);
    const B::Vec3 game_start{rows[3], rows[7], rows[11]};
    B::Vec3 start = game_start;
    B::Vec3 hand{};
    const s32 hand_dummy = shot.bow_hand == 0 ? 340 : 300; // the fist the bow is in
    const bool have_hand = DummyAt(player, hand_dummy, hand);
    const bool moved = !g_dry_aim && have_hand;
    if (idx == 0 && have_hand) {
        HearTwang(hand, now); // bow_direct: its release sound, once
    }
    if (moved) {
        start = hand + dir * BowOriginAheadM;
        rows[3] = start.x;
        rows[7] = start.y;
        rows[11] = start.z;
    }
    const bool written = !g_dry_aim && G::WriteLookRows(rows, dir, ChrForward(player));
    if (idx == 0 && LineAllowed()) {
        LOG_INFO(Core,
                 "VR bow: ARROW #{} {} bid={} bullet={} draw {:.2f} ray{} (the game's {}, {:.1f} "
                 "deg off) from {} ({:.0f} cm from the game's start) {} ms after the release{}",
                 shot.serial, shot.charged ? "charged" : "quick", bid, bullet, shot.draw,
                 YawPitch(dir), YawPitch(vanilla), B::Angle(vanilla, dir) / B::Deg,
                 moved ? fmt::format("the bow hand (dummy {})", hand_dummy)
                       : std::string{"the game's start"},
                 B::Length(start - game_start) * 100.0f, (now - shot.release_ns) / Ms,
                 written ? "" : " (NOT WRITTEN: dry run)");
    }
    return true;
}

// The right hand's direct shots (BladeShotTick: a gun-blade's trigger, the Moonlight Sword's
// wave, the Stake Driver's blast): the launch of the shot's row within DirectMaxAgeNs starts where
// the tick put it and flies along its line; every bullet of it moves by the first one's shift (a
// wave's segments, a fan keep their shape).
struct BladeAimShot {
    u32 serial{0};
    s32 bid{-1};
    s64 ns{0};
    B::Vec3 origin{};
    B::Vec3 dir{};
    B::Vec3 shift{}; // the first bullet's move (the others take it too)
};
BladeAimShot g_blade_aim{};

bool BladeAim(u8* out, u8* launcher, u8* player) {
    BladeAimShot& a = g_blade_aim;
    if (a.bid < 0 || Read<s32>(launcher + OffsetLauncherBid) != a.bid ||
        NowNs() - a.ns > DirectMaxAgeNs) {
        return false;
    }
    auto* const rows = reinterpret_cast<float*>(out);
    const float index_f = Read<float>(out + ShotIndexFromOut);
    const s32 idx = std::isfinite(index_f) ? static_cast<s32>(index_f) : 0;
    const B::Vec3 vanilla = G::FlightOfRows(rows);
    const B::Vec3 start{rows[3], rows[7], rows[11]};
    if (idx == 0) {
        a.shift = a.origin - start;
    }
    bool written = false;
    if (!g_dry_aim) {
        rows[3] += a.shift.x;
        rows[7] += a.shift.y;
        rows[11] += a.shift.z;
        written = G::WriteLookRows(rows, a.dir, ChrForward(player));
    }
    if (idx == 0 && LineAllowed()) {
        LOG_INFO(Core,
                 "VR blade shot: #{} launched bid={} bullet={} along {} (the game's {}, {:.1f} deg "
                 "off), moved {:.0f} cm{}",
                 a.serial, a.bid, Read<s32>(launcher + OffsetLauncherBullet), YawPitch(a.dir),
                 YawPitch(vanilla), B::Angle(vanilla, a.dir) / B::Deg, B::Length(a.shift) * 100.0f,
                 written ? "" : " (NOT WRITTEN: dry run)");
    }
    return true;
}

// tool_aim (core/vr/trick_weapons.h): the Augur of Ebrietas' burst flies where you look when your
// left hand pointed there within 0.8 s before it leaves (ToolGazeTick), else where the hand points
// (the grip's forward, through the fist) as it leaves; not the hunter's facing. Its launch:
// not a gun row, during an item animation (SpEffect 207) of goods 2000 (the goods the animation's
// TAE event 65 uses, [player+0x1C0]). Thrown items never carry 207 (the quick items throw them
// directly) and keep their own flight.
constexpr s32 AugurGoods = 2000;
constexpr s32 ItemAnimEffect = 207;
constexpr u32 OffsetAnimGoods = 0x1C0;

// When the left hand last pointed where you look (the head's forward, within Tricks::GazeDeg):
// ToolGazeTick, every update; 0: not yet.
std::atomic<s64> g_tool_gaze_ns{0};

// The left hand's pointing (tracked, fresh), and the head's (its forward), in the tracking space.
bool LeftPoint(const B::HandsSample& hands, s64 now, B::Vec3& point) {
    const B::TrackedHand& hand = hands.hand[B::C0000::Left];
    if (now - hands.steady_ns > HandsMaxAgeNs || !B::IsTracked(hand)) {
        return false;
    }
    point = B::Rotate(B::Normalize(hand.q), B::Vec3{0.0f, 0.0f, -1.0f});
    return true;
}
B::Vec3 GazeOf(const B::HandsSample& hands) {
    return B::Rotate(B::Normalize(hands.head_q), B::Vec3{0.0f, 0.0f, -1.0f});
}

void ToolGazeTick(s64 now) {
    B::HandsSample hands{};
    B::Vec3 point{};
    if (B::LatestHands(hands) && hands.head_valid && LeftPoint(hands, now, point) &&
        B::Angle(point, GazeOf(hands)) <= VR::Tricks::GazeDeg * B::Deg) {
        g_tool_gaze_ns.store(now, std::memory_order_relaxed);
    }
}

// The burst flies where you look when your hand pointed there within GazeS (thrust at what you
// look at, it need not still point there as the clip lets it go), else where the hand points;
// the game's own aim without either. The left hand feels it leave (GunEventKind::ToolBurst).
bool ToolAim(u8* out, u8* launcher, u8* player) {
    if (!VR::Tricks::GetTrickSettings().tool_aim ||
        Read<s32>(launcher + OffsetLauncherCategory) == CategoryGun ||
        Read<s32>(player + OffsetAnimGoods) != AugurGoods || !HasEffect(player, ItemAnimEffect)) {
        return false;
    }
    const s64 now = NowNs();
    auto* const rows = reinterpret_cast<float*>(out);
    const B::Vec3 vanilla = G::FlightOfRows(rows);
    const float index_f = Read<float>(out + ShotIndexFromOut);
    const s32 idx = std::isfinite(index_f) ? static_cast<s32>(index_f) : 0;
    B::HandsSample hands{};
    B::Anchor anchor{};
    B::Vec3 point{};
    const bool have = B::LatestHands(hands);
    const bool tracked = have && LeftPoint(hands, now, point);
    const s64 seen = g_tool_gaze_ns.load(std::memory_order_relaxed);
    const bool look = have && hands.head_valid && now - hands.steady_ns <= HandsMaxAgeNs &&
                      seen != 0 && now - seen <= static_cast<s64>(VR::Tricks::GazeS * 1e9f);
    const char* why = nullptr;
    if (!look && !tracked) {
        why = "no left hand";
    } else if (!B::LatestAnchor(anchor) || !anchor.valid ||
               now - anchor.steady_ns > AnchorMaxAgeNs) {
        why = "view";
    }
    B::Vec3 dir = vanilla;
    if (why == nullptr) {
        const B::Vec3 along = look ? GazeOf(hands) : point;
        dir = B::Normalize(B::TrackingToWorld(along, anchor.base_yaw, 1.0f), vanilla);
        if (!g_dry_aim) {
            G::WriteLookRows(rows, dir, ChrForward(player));
        }
    }
    if (idx == 0) {
        Combat::PushGunEvent({Combat::GunEventKind::ToolBurst, 0, 0, 0.0f, now, ""});
    }
    if (idx == 0 && LineAllowed()) {
        LOG_INFO(Core, "VR tool aim: the Augur's burst bid={} bullet={} along {} (the game's {}, "
                       "{:.1f} deg off){}",
                 Read<s32>(launcher + OffsetLauncherBid), Read<s32>(launcher + OffsetLauncherBullet),
                 YawPitch(dir), YawPitch(vanilla), B::Angle(vanilla, dir) / B::Deg,
                 why != nullptr ? fmt::format(" (the game's aim: {})", why)
                 : look         ? fmt::format(" (where you look: the hand pointed there {:.0f} "
                                              "ms before)",
                                              static_cast<double>(now - seen) * 1e-6)
                                : std::string{" (where the hand points)"});
    }
    return true;
}

// molotov_burst (core/vr/bloodborne_quickitems.h): the first bullet of a shot, its start and ray.
void NoteShot(const B::Vec3& start, const B::Vec3& ray) {
    const std::array<float, 3> from{start.x, start.y, start.z};
    const std::array<float, 3> along{ray.x, ray.y, ray.z};
    QuickItemsGunShot(from.data(), along.data());
}

void PS4_SYSV_ABI GunEmitterHook(u8* out, u8* /*saved_ymm*/, u8* launcher) {
    u8* const player = MainPlayer();
    const bool own = player != nullptr && out != nullptr && Plausible(launcher) &&
                     Read<u32>(launcher + OffsetLauncherOwner) == Read<u32>(player + OffsetHandle);
    if (own && g_log >= 1 && Read<s32>(launcher + OffsetLauncherCategory) != CategoryGun &&
        LineAllowed()) {
        // VR_GUN_LOG: every other launch of the hunter's (the right hand's shots, the tools).
        LOG_INFO(Core,
                 "VR gun: launch bid={} bullet={} category={} anim goods={} item anim={} idx={:.0f}",
                 Read<s32>(launcher + OffsetLauncherBid), Read<s32>(launcher + OffsetLauncherBullet),
                 Read<s32>(launcher + OffsetLauncherCategory), Read<s32>(player + OffsetAnimGoods),
                 HasEffect(player, ItemAnimEffect) ? 1 : 0, Read<float>(out + ShotIndexFromOut));
    }
    if (own && BladeAim(out, launcher, player)) {
        return; // a right-hand shot of ours: from where the tick put it, along its line
    }
    if (own && BowArrow(out, launcher, player)) {
        return; // Simon's Bowblade's arrow (vr.ini bow): along the drawn line
    }
    if (own && ToolAim(out, launcher, player)) {
        return; // the Augur (tool_aim): where the left hand points
    }
    if (player == nullptr || out == nullptr || !Plausible(launcher) ||
        Read<u32>(launcher + OffsetLauncherOwner) != Read<u32>(player + OffsetHandle) ||
        Read<s32>(launcher + OffsetLauncherCategory) != CategoryGun) {
        return; // not the hunter's gun: items (5), right-hand trick guns (1), every NPC
    }
    const s32 bid = Read<s32>(launcher + OffsetLauncherBid);
    const G::GunInfo info = G::GunOfBehavior(bid);
    if (info.family == G::Family::None) {
        return;
    }
    const auto& gs = G::GetGunSettings();
    auto* const rows = reinterpret_cast<float*>(out);
    const s64 now = NowNs();
    const float index_f = Read<float>(out + ShotIndexFromOut);
    const s32 idx = std::isfinite(index_f) ? static_cast<s32>(index_f) : 0;
    const s32 bullet = Read<s32>(launcher + OffsetLauncherBullet);
    B::Vec3 start{rows[3], rows[7], rows[11]};
    const B::Vec3 vanilla = G::FlightOfRows(rows);

    // The grip: the direct pull of this frame, else the newest pull (the game's own shot, aimed
    // when the trigger broke), else the newest hand sample.
    RaySource src{};
    bool direct = false;
    {
        std::scoped_lock lock{g_shot_mutex};
        if (g_direct.bid == bid && g_direct.grip_valid && now - g_direct.ns <= DirectMaxAgeNs) {
            src = {true, "pull", "", g_direct.grip_q, g_direct.pull_ns, g_direct.serial, -1};
            direct = true;
        }
    }
    B::HandsSample hands{};
    const bool have_hands = B::LatestHands(hands);
    if (!src.ok) {
        // The newest of: a pull the presenter sent to L2, a direct pull handed to L2.
        Combat::GunPull pull{};
        const bool sent = Combat::LatestGunPull(pull) && !pull.direct;
        Combat::GunPull handed{};
        {
            std::scoped_lock lock{g_shot_mutex};
            handed = g_handed;
        }
        if (!sent || (handed.serial != 0 && handed.press_ns > pull.press_ns)) {
            pull = handed;
        }
        if (pull.serial != 0 && pull.grip_valid && now - pull.press_ns <= PullPoseMaxAgeNs &&
            (pull.serial != g_game_shot_serial || idx > 0) &&
            (!have_hands || hands.space_serial == pull.space_serial)) {
            src = {true, "pull", "", pull.grip_q, pull.press_ns, pull.serial, -1};
            if (idx == 0) {
                g_game_shot_serial = pull.serial;
            }
        }
    }
    if (!src.ok && have_hands && B::IsTracked(hands.hand[B::C0000::Left]) &&
        now - hands.steady_ns <= HandsMaxAgeNs) {
        src = {true, "now", "", hands.hand[B::C0000::Left].q, 0, 0, now - hands.steady_ns};
    }
    const char* why = gs.aim ? nullptr : "aim disabled";
    if (!src.ok) {
        why = "hands";
    }
    B::Anchor anchor{};
    if (why == nullptr &&
        (!B::LatestAnchor(anchor) || !anchor.valid || now - anchor.steady_ns > AnchorMaxAgeNs)) {
        why = "view";
    }
    B::BodyStatus body{};
    // The arm as drawn (a kept dodge's shot, dodge_attack, leaves along the barrel in your hand).
    if (why == nullptr && (!B::LatestBodyStatus(body) || now - body.steady_ns > StatusMaxAgeNs ||
                           !body.driven || body.drawn_weight[B::C0000::Left] < 0.5f)) {
        why = "arm";
    }
    const B::Vec3 barrel = BarrelInGripNow(info.attach);
    if (why == nullptr && B::LengthSq(barrel) == 0.0f) {
        why = "nobarrel";
    }
    if (why != nullptr) {
        std::atomic<u64>& counter = why[0] == 'h'   ? g_stats.vanilla_hands
                                    : why[0] == 'v' ? g_stats.vanilla_view
                                    : why[0] == 'a' ? g_stats.vanilla_arm
                                                    : g_stats.vanilla_barrel;
        counter.fetch_add(1, std::memory_order_relaxed);
        if (idx == 0) {
            NoteShot(start, vanilla);
        }
        if (idx == 0 && LineAllowed()) {
            LOG_INFO(Core,
                     "VR gun: SHOT {} bid={} bullet={} idx={} along the game's aim {} "
                     "(vanilla:{}){}",
                     G::FamilyName(info.family), bid, bullet, idx, YawPitch(vanilla), why,
                     direct ? " DIRECT" : "");
        }
        if (idx == 0 && !direct) {
            Combat::PushGunEvent({Combat::GunEventKind::GameShot, src.serial,
                                  static_cast<u8>(info.family), 0.0f, now, why});
            StoreKick(info.family, info.attach, now);
        }
        return;
    }
    const B::Vec3 ray = G::BarrelRayWorld(src.q, barrel, anchor.base_yaw);
    const bool locked = VR::IsLockedOn();
    const bool have_target = Read<u8>(player + OffsetLockChosen) != 0;
    const B::Vec3 target = Read3(player + OffsetLockPoint);
    G::AssistCfg cfg{};
    cfg.strength = gs.assist;
    cfg.cone_deg = gs.assist_deg;
    cfg.unlocked = gs.assist_unlocked;
    const G::AssistOut a =
        G::GentleAssist(ray, start, target, have_target, locked, info.gravity, cfg);
    // The start onto the barrel's line, unless the game pulled it back from a wall (then it is no
    // longer the palm) or the muzzle is not where a gun in this hand would have it.
    B::Vec3 shift{};
    bool moved = false;
    B::Vec3 palm{};
    const s32 muzzle_id = info.attach == 348 || info.family == G::Family::Sprayer ? 20206 : 20215;
    B::Vec3 muzzle{};
    std::array<float, 12> muzzle_rows{};
    const bool have_muzzle = DummyAt(player, muzzle_id, muzzle, &muzzle_rows);
    if (gs.origin_barrel && have_muzzle && DummyAt(player, PalmDummy, palm) &&
        B::Length(start - palm) <= 0.01f) {
        B::Vec3 p = start;
        moved = G::OriginOnBarrel(p, a.dir, muzzle, MaxOriginShiftM, &shift);
        if (moved && !g_dry_aim) {
            rows[3] = p.x;
            rows[7] = p.y;
            rows[11] = p.z;
            start = p;
        }
    }
    const bool written = !g_dry_aim && G::WriteLookRows(rows, a.dir, ChrForward(player));
    g_stats.aimed.fetch_add(1, std::memory_order_relaxed);
    if (a.weight > 0.0f) {
        g_stats.assisted.fetch_add(1, std::memory_order_relaxed);
    }
    if (moved) {
        g_stats.origin_moved.fetch_add(1, std::memory_order_relaxed);
    }
    if (idx == 0 && !direct) {
        Combat::PushGunEvent({Combat::GunEventKind::GameShot, src.serial,
                              static_cast<u8>(info.family), 1.0f, now, ""});
        StoreKick(info.family, info.attach, now);
    }
    u32 n = 0;
    static s64 last_shot_ns = 0; // the emitter's own (game thread)
    const s64 since_ns = idx == 0 && last_shot_ns != 0 ? now - last_shot_ns : -1;
    if (idx == 0) {
        last_shot_ns = now;
    }
    {
        std::scoped_lock lock{g_shot_mutex};
        if (idx == 0) {
            NoteShot(start, g_dry_aim ? vanilla : a.dir);
            g_last = {};
            g_last.n = ++g_stats.lines;
            g_last.bullet = bullet;
            g_last.owner = Read<u32>(player + OffsetHandle);
            g_last.ray = g_dry_aim ? vanilla : a.dir;
            g_last.start = start;
            g_last.ns = now;
            g_last.pull_ns = src.pull_ns;
            g_last.direct = direct;
            g_aim_pending.store(true, std::memory_order_release);
        }
        n = g_last.n;
    }
    if (idx != 0 && g_log < 1) {
        return;
    }
    if (!LineAllowed()) {
        return;
    }
    // The rendered gun against the ray: the muzzle dummy's axes (its forward is the barrel) and
    // the line from the part's dummy 100 to the muzzle (about 4 degrees off the barrel on the
    // pistol, by the part's geometry).
    std::string barrel_text;
    if (have_muzzle && g_log >= 1) {
        const B::Vec3 col{muzzle_rows[2], muzzle_rows[6], muzzle_rows[10]};
        const B::Vec3 row{muzzle_rows[8], muzzle_rows[9], muzzle_rows[10]};
        B::Vec3 back{};
        const bool line = DummyAt(player, PartLineDummy, back);
        barrel_text = fmt::format(
            " | muzzle dmy {} axis z vs ray: col {:.1f} row {:.1f} deg{}", muzzle_id,
            B::Angle(col, a.dir) / B::Deg, B::Angle(row, a.dir) / B::Deg,
            line ? fmt::format(", line {}->{} vs ray {:.1f} deg, muzzle {:.1f} cm off the ray",
                               PartLineDummy, muzzle_id, B::Angle(muzzle - back, a.dir) / B::Deg,
                               B::Length(B::Perp(muzzle - start, a.dir)) * 100.0f)
                 : std::string{});
    }
    LOG_INFO(
        Core,
        "VR gun: SHOT #{} {} bid={} bullet={} idx={} {}{} P={} vanilla{} ray{} out{} "
        "d_vanilla={:.1f} src={}{} locked={} lock_pt={} assist w={:.2f} err={:.1f} "
        "moved={:.2f} deg origin {} armL={:.2f} hands={} ms view={} ms{}{}",
        n, G::FamilyName(info.family), bid, bullet, idx, direct ? "DIRECT" : "game's own",
        since_ns >= 0
            ? fmt::format(" ({:.0f} ms after the last shot)", static_cast<double>(since_ns) * 1e-6)
            : std::string{},
        V3(start), YawPitch(vanilla), YawPitch(ray), YawPitch(a.dir),
        B::Angle(vanilla, a.dir) / B::Deg, src.src,
        src.pull_ns != 0 ? fmt::format(" (pull {} ms ago)", (now - src.pull_ns) / Ms)
                         : std::string{},
        locked ? 1 : 0, have_target ? 1 : 0, a.weight, a.angle_deg, a.moved_deg,
        moved ? fmt::format("moved {:.1f} cm", B::Length(shift) * 100.0f) : std::string{"kept"},
        body.arm_weight[B::C0000::Left], src.hands_age_ns >= 0 ? src.hands_age_ns / Ms : -1,
        (now - anchor.steady_ns) / Ms, written ? "" : " (NOT WRITTEN: dry run, the game's aim)",
        barrel_text);
}

// ---- The pose tick (the body's pose-copy wrapper, the game's main thread) ----------------------

struct TickState {
    const u8* chr{nullptr};
    s64 last_ns{0};
    u32 handled{0}; // the newest pull serial looked at
    bool pending{false};
    Combat::GunPull pull{};
    bool first_look{true};
    float cooldown_left{0.0f};
    float flash_left{0.0f};
    s32 flash_slot{FirstFlashSlot};
    s32 flash_ffx{-1};
    s32 flash_dummy{20215};
    float regen_left{0.0f};
    u32 shots{0};
    s64 last_shot_ns{0};
    u32 updates{0};
    // Cached each few updates.
    s32 bid{-1};
    G::GunInfo info{};
    const G::DirectGun* gun{nullptr};
    bool can_fire{false};
    bool two_handed{false};
    // Logs.
    bool status_logged{false};
    G::Family logged_family{G::Family::None};
    bool logged_direct{false};
    s64 last_stats_ns{0};
};
TickState g_tick{};

float StepSeconds(TickState& t, s64 now) {
    const float dt =
        t.last_ns != 0 ? std::clamp(static_cast<float>(now - t.last_ns) * 1e-9f, 0.0f, 0.1f) : 0.0f;
    t.last_ns = now;
    return dt;
}

// The game's sound for a gun event, at `pos` (event 128's type, SoundType 1).
u64 Sound(s32 id, B::Vec3 pos) {
    if (!g_sound || id < 0 || g_dry_run) {
        return 0;
    }
    alignas(16) std::array<float, 4> p{pos.x, pos.y, pos.z, 1.0f};
    u64 handle = 0;
    const s32 type = g_sound_table != nullptr && Read<u8>(g_sound_table + 3) != 0 ? 0xA : 1;
    g_play_sound(&handle, p.data(), type, id);
    return handle;
}

void RequestFlash(u8* chr, const TickState& t) {
    if (!g_flash || g_dry_run || t.flash_ffx < 0) {
        return;
    }
    u8* const ctl = chr + OffsetSfxCtl;
    if (Read<u8*>(ctl) != g_base + SfxCtlVtableRva) {
        return;
    }
    const s32 dummy = t.flash_dummy;
    g_sfx_slot(ctl, t.flash_slot, t.flash_ffx, &dummy, 0);
}

void Fire(u8* chr, TickState& t, s64 now) {
    const auto& gs = G::GetGunSettings();
    const G::DirectGun& gun = *t.gun;
    const u32 serial = t.pull.serial;
    const float pull_ms = static_cast<float>(now - t.pull.press_ns) * 1e-6f;
    B::Vec3 muzzle{};
    if (!DummyAt(chr, gun.flash_dummy, muzzle) && !DummyAt(chr, PalmDummy, muzzle)) {
        muzzle = {};
    }
    const s32 q_before = GoodsCount(chr, QuicksilverBullets);
    const s32 b_before = GoodsCount(chr, BloodBullets);
    if (g_can_fire(chr, 0) == 0) {
        const u64 h = Sound(gun.dry, muzzle);
        t.cooldown_left = DryCooldownS;
        g_stats.dry.fetch_add(1, std::memory_order_relaxed);
        Combat::PushGunEvent({Combat::GunEventKind::Dry, serial, static_cast<u8>(gun.family), 0.0f,
                              now, "no bullets"});
        LOG_INFO(Core,
                 "VR gun: DRY #{} serial={} pull->e0={:.1f} ms bullets 900:{} 901:{} -> click {} "
                 "(h={:#x}), no bullet{}",
                 t.shots, serial, pull_ms, q_before, b_before, gun.dry, h,
                 g_dry_run ? " DRY RUN" : "");
        return;
    }
    const bool ash = HasEffect(chr, BoneMarrowAsh);
    const s32 judge = ash ? JudgeAsh : JudgeShot;
    const s32 bid = BehaviorId(chr, judge);
    RowOut row{};
    if (bid > 0) {
        g_behavior_row(&row, bid, 1);
    }
    const bool row_ok = Plausible(row.row, 4) && Read<u8>(row.row + RowRefType) == 1 &&
                        Read<u8>(row.row + RowCategory) == CategoryGun &&
                        G::GunOfBehavior(bid).family == gun.family;
    if (!row_ok) {
        g_stats.handed.fetch_add(1, std::memory_order_relaxed);
        {
            std::scoped_lock lock{g_shot_mutex};
            g_handed = t.pull;
        }
        Combat::PushGunEvent(
            {Combat::GunEventKind::Vanilla, serial, static_cast<u8>(gun.family), 0.0f, now, "bid"});
        LOG_WARNING(Core,
                    "VR gun: pull {} -> the game's L2: behaviour {} (judge {}) is not a gun row "
                    "(row {} refType {} category {})",
                    serial, bid, judge, fmt::ptr(row.row),
                    Plausible(row.row, 4) ? Read<u8>(row.row + RowRefType) : -1,
                    Plausible(row.row, 4) ? Read<u8>(row.row + RowCategory) : -1);
        return;
    }
    const s32 bullet = Read<s32>(row.row + RowRefId);
    {
        std::scoped_lock lock{g_shot_mutex};
        g_direct = {serial,
                    bid,
                    now,
                    t.pull.press_ns,
                    t.pull.grip_q,
                    t.pull.grip_valid,
                    t.pull.space_serial};
    }
    const s32 cost = BulletCost(chr, G::GunOfBehavior(bid).cost);
    u8 ok = 0;
    u8 consumed = 0;
    if (!g_dry_run) {
        ok = g_invoke_gun(chr, bid, gun.shot_dummy, 0, HandLeft, HandLeft, 1, 1);
        // As the event does: after the shot, whatever it returned.
        consumed = g_consume(chr, cost);
    }
    ++t.shots;
    // The flash: the next private slot (overlapping flashes each keep their own node), renewed
    // every update for the clip's flash time.
    t.flash_slot = FirstFlashSlot + static_cast<s32>(t.shots % FlashSlots);
    t.flash_ffx = ash ? gun.flash_ffx_ash : gun.flash_ffx;
    t.flash_dummy = gun.flash_dummy;
    t.flash_left = gun.flash_s;
    RequestFlash(chr, t);
    const u64 h = Sound(gun.bang, muzzle);
    if (gs.regen_pause && g_regen) {
        t.regen_left = gun.regen_pause_s;
    }
    t.cooldown_left = gun.cadence_s * gs.rate_scale;
    StoreKick(gun.family, t.info.attach, now);
    const s64 since_ns = t.last_shot_ns != 0 ? now - t.last_shot_ns : -1;
    t.last_shot_ns = now;
    (ok != 0 ? g_stats.fired : g_stats.fired_failed).fetch_add(1, std::memory_order_relaxed);
    Combat::PushGunEvent({Combat::GunEventKind::Fired, serial, static_cast<u8>(gun.family),
                          ok != 0 ? 1.0f : 0.0f, now, ""});
    LOG_INFO(Core,
             "VR gun: FIRE #{} serial={} {} pull->e0={:.1f} ms{} bid={} judge={} bullet={} dmy={} "
             "ok={} cost={} consumed={} bullets 900:{}->{} 901:{}->{} flash ffx {}@{} slot {} "
             "bang {} (h={:#x}) cool {:.3f} s regen pause {:.3f} s thread '{}'{}",
             t.shots, serial, G::FamilyName(gun.family), pull_ms,
             since_ns >= 0 ? fmt::format(" ({:.0f} ms after the last shot)",
                                         static_cast<double>(since_ns) * 1e-6)
                           : std::string{},
             bid, judge, bullet, gun.shot_dummy, ok, cost, consumed, q_before,
             GoodsCount(chr, QuicksilverBullets), b_before, GoodsCount(chr, BloodBullets),
             t.flash_ffx, t.flash_dummy, t.flash_slot, gun.bang, h, t.cooldown_left, t.regen_left,
             Common::GetCurrentThreadName(), g_dry_run ? " DRY RUN (nothing fired)" : "");
}

const char* HardReason(const u8* chr, const GunBody& body) {
    if (!body.body) {
        return "body off";
    }
    switch (body.reason) {
    case B::GateReason::Dead:
        return "dead";
    case B::GateReason::Throw:
        return "visceral or grab";
    case B::GateReason::Ladder:
        return "ladder";
    case B::GateReason::Event:
        return "event";
    default:
        break;
    }
    if (const u8* data = DataModule(chr); data != nullptr && Read<s32>(data + OffsetHp) <= 0) {
        return "dead";
    }
    if (VR::IsGameMenuOpen()) {
        return "menu";
    }
    if (VR::CinematicActive()) {
        return "cinematic";
    }
    if (!body.driven) {
        return "not first person";
    }
    return nullptr;
}

// VR_GUN_SCAN=<ms> (tests): every <ms> where the hunter is and faces, the camera's view and the
// game's lock-on candidates (LockTgtMan [0x553E860]: a list from +0x08 linked through +0x80, the
// world lock point at +0x00, flags at +0x90; the quick items' SCAN line, faster), and for 4 s after
// each hit the hunter dealt, the target's new animations and HP (a gun parry's stagger).
// VR_GUN_PARRY_PROBE=<ms>[,<ms>...] (tests; with VR_GUN_SCAN): the last target is followed for
// good, and each attack it starts (its animations 3000-3999) gets a direct shot the next delay
// later (cycling through the list), aimed at its chest through the same barrel-ray path (the pull's
// grip is turned so the barrel points there). VR_GUN_GOD=1 (tests): the hunter's HP is kept full.
struct ScanState {
    s64 interval_ns{-1};
    s64 last_ns{0};
    u8* target{nullptr};
    s64 target_hit_ns{0};
    const u8* ring{nullptr};
    s32 ring_write{-1};
    s32 last_id{-1};
    // The parry probe.
    std::vector<s32> probe_ms;
    bool probe_read{false};
    u32 probe_count{0};
    s64 probe_at{0}; // the shot is due then (0: none)
    s32 probe_anim{-1};
    s64 probe_anim_ns{0};
    bool god{false};
    s32 god_hp{-1};
    bool god_grab{false}; // VR_GUN_GOD=grab
    bool god_low{false};  // this flat period's HP drop is done
};
constexpr s32 GodGrabHp = 60;
ScanState g_scan{};

void ScanTick(u8* chr, s64 now) {
    auto& sc = g_scan;
    if (sc.interval_ns < 0) {
        const char* value = std::getenv("VR_GUN_SCAN");
        sc.interval_ns = value != nullptr ? std::max(std::atoi(value), 50) * Ms : 0;
    }
    if (sc.interval_ns == 0) {
        return;
    }
    if (!sc.probe_read) {
        sc.probe_read = true;
        if (const char* value = std::getenv("VR_GUN_PARRY_PROBE"); value != nullptr) {
            for (const char* p = value; *p != 0;) {
                char* end = nullptr;
                const long ms = std::strtol(p, &end, 10);
                if (end == p) {
                    break;
                }
                sc.probe_ms.push_back(static_cast<s32>(std::clamp(ms, 0L, 3000L)));
                p = *end == ',' ? end + 1 : end;
            }
        }
        const char* god = std::getenv("VR_GUN_GOD");
        sc.god = god != nullptr;
        sc.god_grab = god != nullptr && std::strcmp(god, "grab") == 0;
    }
    if (sc.god) {
        if (u8* const data = DataModule(chr); data != nullptr) {
            const s32 hp = Read<s32>(data + OffsetHp);
            const s32 max = Read<s32>(data + OffsetHp + 4);
            // VR_GUN_GOD=grab (tests: a grab death): kept full outside a flat period; one that
            // starts (an enemy grab) drops HP to GodGrabHp once, so the grab kills.
            const auto phase = sc.god_grab ? CinematicPhase() : Cinematic::Phase::Off;
            const bool flat = phase == Cinematic::Phase::FadeOut || phase == Cinematic::Phase::Flat;
            if (flat) {
                if (!sc.god_low && hp > GodGrabHp) {
                    const s32 low = GodGrabHp;
                    std::memcpy(data + OffsetHp, &low, sizeof(low)); // test only
                }
                sc.god_low = true;
            } else if (hp > 0) {
                sc.god_low = false;
                if (hp < max) {
                    std::memcpy(data + OffsetHp, &max, sizeof(max)); // test only: never dies
                }
            }
        }
    }
    if (now - sc.last_ns >= sc.interval_ns) {
        sc.last_ns = now;
        u8* const ctrl = Read<u8*>(chr + OffsetChrCtrl);
        if (Plausible(ctrl)) {
            const u8* w = ctrl + OffsetCtrlWorld;
            const B::Vec3 me{Read<float>(w + 0x0C), Read<float>(w + 0x1C), Read<float>(w + 0x2C)};
            std::string list;
            u8* const mgr = Read<u8*>(g_base + 0x553E860);
            u8* e = Plausible(mgr) ? Read<u8*>(mgr + 0x08) : nullptr;
            for (int i = 0; i < 12 && Plausible(e, 16); ++i) {
                const B::Vec3 at = Read3(e);
                list += fmt::format(" [({:.2f},{:.2f},{:.2f}) {:.1f} m f={:#x}]", at.x, at.y, at.z,
                                    std::hypot(at.x - me.x, at.z - me.z), Read<u32>(e + 0x90));
                e = Read<u8*>(e + 0x80);
            }
            B::Anchor anchor{};
            const float view = B::LatestAnchor(anchor) ? anchor.base_yaw : 0.0f;
            LOG_INFO(Core,
                     "VR gun: SCAN hunter ({:.2f},{:.2f},{:.2f}) facing ({:.2f},{:.2f}) view "
                     "({:.2f},{:.2f}) | lock candidates:{}",
                     me.x, me.y, me.z, -Read<float>(w + 0x08), -Read<float>(w + 0x28),
                     std::sin(view), std::cos(view), list.empty() ? std::string{" none"} : list);
        }
    }
    s64 hit_ns = 0;
    u8* const target = LastPlayerHitTarget(hit_ns);
    const bool follow = !sc.probe_ms.empty(); // the probe follows its target for good
    if (target != nullptr && hit_ns != sc.target_hit_ns && now - hit_ns < 4000 * Ms) {
        if (target != sc.target) {
            sc.ring = nullptr;
            sc.last_id = -1;
        }
        sc.target = target;
        sc.target_hit_ns = hit_ns;
    }
    if (sc.target == nullptr || (!follow && now - sc.target_hit_ns > 4000 * Ms) ||
        !Plausible(sc.target)) {
        return;
    }
    u8* const mods = Read<u8*>(sc.target + OffsetChrModules);
    u8* const ta = Plausible(mods) ? Read<u8*>(mods + 0x18) : nullptr; // TimeAct module
    if (!Plausible(ta) || Read<u8*>(ta) != g_base + 0x5335190) {
        return;
    }
    const s32 write = Read<s32>(ta + 0xC0);
    if (write < 0 || write >= 10) {
        return;
    }
    if (ta != sc.ring) {
        sc.ring = ta;
        sc.ring_write = (write + 9) % 10; // the newest entry counts as new
    }
    // An enemy's ring takes an entry every update (the clip playing): only a new id is logged.
    const u8* data = DataModule(sc.target);
    for (s32 k = 0; k < (write - sc.ring_write + 10) % 10; ++k) {
        const s32 index = (sc.ring_write + k) % 10;
        const s32 id = Read<s32>(ta + 0x20 + index * 16);
        if (id == sc.last_id) {
            continue;
        }
        sc.last_id = id;
        if (follow && id >= 3000 && id <= 3999) {
            sc.probe_anim = id;
            sc.probe_anim_ns = now;
            sc.probe_at =
                now + static_cast<s64>(sc.probe_ms[sc.probe_count % sc.probe_ms.size()]) * Ms;
        }
        LOG_INFO(Core, "VR gun: TARGET {} anim {} (a{:03}_{:06}) {:.0f} ms after the hit, hp {}",
                 fmt::ptr(sc.target), id, id / 1'000'000, id % 1'000'000,
                 static_cast<float>(now - sc.target_hit_ns) * 1e-6f,
                 data != nullptr ? Read<s32>(data + OffsetHp) : -1);
    }
    sc.ring_write = write;
}

// VR_GUN_PARRY_PROBE: the due shot at the followed target's attack, as a direct pull whose grip
// turns the barrel toward the target's chest (its W translation + 1 m) from the palm.
void ProbePull(u8* chr, TickState& t, s64 now) {
    auto& sc = g_scan;
    u8* const ctrl = Plausible(sc.target) ? Read<u8*>(sc.target + OffsetChrCtrl) : nullptr;
    B::Anchor anchor{};
    B::Vec3 palm{};
    if (!Plausible(ctrl) || !B::LatestAnchor(anchor) || !DummyAt(chr, PalmDummy, palm)) {
        return;
    }
    const u8* w = ctrl + OffsetCtrlWorld;
    const B::Vec3 chest{Read<float>(w + 0x0C), Read<float>(w + 0x1C) + 1.0f, Read<float>(w + 0x2C)};
    const B::Vec3 dir = B::Normalize(chest - palm);
    const B::Vec3 barrel = BarrelInGripNow(t.info.attach);
    if (B::LengthSq(dir) == 0.0f || B::LengthSq(barrel) == 0.0f) {
        return;
    }
    const B::Vec3 tracking = B::Normalize(B::WorldToTracking(dir, anchor.base_yaw, 1.0f));
    B::HandsSample hands{};
    B::LatestHands(hands);
    Combat::GunPull pull{};
    pull.serial = 0x80000000u | ++sc.probe_count;
    pull.direct = true;
    pull.press_ns = now;
    pull.grip_valid = true;
    pull.grip_q = B::FromTo(barrel, tracking);
    pull.space_serial = hands.space_serial;
    t.pending = true;
    t.pull = pull;
    t.first_look = true;
    LOG_INFO(Core,
             "VR gun: PROBE shot {} at the target's anim {} +{:.0f} ms (delay {} ms), the target "
             "{:.2f} m away",
             sc.probe_count, sc.probe_anim, static_cast<double>(now - sc.probe_anim_ns) * 1e-6,
             sc.probe_ms[(sc.probe_count - 1) % sc.probe_ms.size()], B::Length(chest - palm));
}

void LogStats(TickState& t, s64 now) {
    if (now - t.last_stats_ns < StatsNs ||
        (g_log == 0 && g_stats.lines.load() == 0 && t.shots == 0)) {
        return;
    }
    t.last_stats_ns = now;
    LOG_INFO(Core,
             "VR gun: STATS aimed={} assisted={} origin_moved={} game's aim: hands={} view={} "
             "arm={} nobarrel={} | direct fired={} failed={} dry={} denied={} to L2={}",
             g_stats.aimed.load(), g_stats.assisted.load(), g_stats.origin_moved.load(),
             g_stats.vanilla_hands.load(), g_stats.vanilla_view.load(), g_stats.vanilla_arm.load(),
             g_stats.vanilla_barrel.load(), g_stats.fired.load(), g_stats.fired_failed.load(),
             g_stats.dry.load(), g_stats.denied.load(), g_stats.handed.load());
}

// ---- Install ------------------------------------------------------------------------------------

template <size_t N>
bool Matches(const u8* module, u64 rva, const std::array<u8, N>& sig) {
    return std::memcmp(module + rva, sig.data(), N) == 0;
}

bool InstallAim(u8* module) {
    // Observe vanilla shots too, so wrist reload can be enabled live with gun_aim=0.
    const char* site = std::getenv("VR_GUN_SITE");
    if (site != nullptr && std::string_view{site} == "off") {
        LOG_INFO(Core, "VR gun: shot observer disabled (VR_GUN_SITE=off)");
        return false;
    }
    if (!Matches(module, EmitSigRva, EmitSig)) {
        LOG_WARNING(Core,
                    "VR gun: aim hook NOT installed: the emitter window at {:#x} differs (live "
                    "bytes {}); shots go the game's way",
                    EmitSigRva, Hook::HexBytes(module + EmitSigRva, EmitSig.size()));
        return false;
    }
    using namespace Xbyak::util;
    auto& c = *g_code;
    void* const host = Hook::EmitHostCallStub(
        c, reinterpret_cast<void*>(&GunEmitterHook), EmitSig.data() + EmitLead, EmitLen,
        reinterpret_cast<u64>(module + EmitResumeRva), rax, &rbx);
    // rax (0x18C7800's return, the matrix) is dead at the site: the entry loads the matrix
    // itself, from the guest frame.
    c.align(16);
    auto* const entry = const_cast<u8*>(c.getCurr());
    c.lea(rax, ptr[rbp + EmitOutSlot]);
    c.jmp(host, Xbyak::CodeGenerator::T_NEAR);
    c.ready();
    u8* const at = module + EmitSiteRva;
    if (!Hook::MakeWritable(at, EmitLen)) {
        LOG_ERROR(Core, "VR gun: could not make {:#x} writable; aim hook not installed",
                  EmitSiteRva);
        return false;
    }
    Hook::WriteJump(at, EmitLen, entry);
    return true;
}

bool InstallFire(u8* module, bool wcm_ok) {
    const auto& gs = G::GetGunSettings();
    if (!gs.fire_direct) {
        LOG_INFO(Core, "VR gun: direct fire off (gun_fire=vanilla): the left trigger is L2");
    }
    struct Site {
        const char* name;
        u64 rva;
        const u8* sig;
        size_t length;
    };
    const std::array<Site, 11> sites = {{
        {"InvokeGun", InvokeGunRva, InvokeGunSig.data(), InvokeGunSig.size()},
        {"event 318 call", Call318Rva, Call318Sig.data(), Call318Sig.size()},
        {"ConsumeBullets", ConsumeBulletsRva, ConsumeBulletsSig.data(), ConsumeBulletsSig.size()},
        {"CanFireGun", CanFireGunRva, CanFireGunSig.data(), CanFireGunSig.size()},
        {"GetBehaviorId", GetBehaviorIdRva, GetBehaviorIdSig.data(), GetBehaviorIdSig.size()},
        {"GetWeaponId", GetWeaponIdRva, GetWeaponIdSig.data(), GetWeaponIdSig.size()},
        {"BehaviorParam row", BehaviorRowRva, BehaviorRowSig.data(), BehaviorRowSig.size()},
        {"WeaponParam row", WeaponRowRva, WeaponRowSig.data(), WeaponRowSig.size()},
        {"GetDmyPolyMtx", DmyPolyRva, DmyPolySig.data(), DmyPolySig.size()},
        {"PlaySound", PlaySoundRva, PlaySoundSig.data(), PlaySoundSig.size()},
        {"event 128 sound type", SoundTypeRva, SoundTypeSig.data(), SoundTypeSig.size()},
    }};
    bool ok = wcm_ok;
    std::string bad;
    for (const Site& s : sites) {
        if (std::memcmp(module + s.rva, s.sig, s.length) != 0) {
            ok = false;
            bad += fmt::format(" {} at {:#x} (live {})", s.name, s.rva,
                               Hook::HexBytes(module + s.rva, std::min<size_t>(s.length, 24)));
        }
    }
    if (!ok) {
        LOG_WARNING(Core,
                    "VR gun: direct fire NOT installed (WorldChrMan {}){}: the left trigger "
                    "is L2",
                    wcm_ok ? "ok" : "MISMATCH", bad);
        return false;
    }
    g_invoke_gun = reinterpret_cast<InvokeGunFn>(module + InvokeGunRva);
    g_consume = reinterpret_cast<ConsumeBulletsFn>(module + ConsumeBulletsRva);
    g_can_fire = reinterpret_cast<CanFireGunFn>(module + CanFireGunRva);
    g_behavior_row = reinterpret_cast<BehaviorRowFn>(module + BehaviorRowRva);
    g_weapon_row = reinterpret_cast<WeaponRowFn>(module + WeaponRowRva);
    g_play_sound = reinterpret_cast<PlaySoundFn>(module + PlaySoundRva);
    {
        s32 disp{};
        std::memcpy(&disp, module + SoundTypeRva + 3, sizeof(disp));
        g_sound_table = module + SoundTypeRva + 7 + disp;
    }
    g_sound = true;
    g_flash = Matches(module, SfxSlotRequestRva, SfxSlotRequestSig) &&
              Matches(module, CtlCtorRva, CtlCtorSig);
    if (g_flash) {
        g_sfx_slot = reinterpret_cast<SfxSlotRequestFn>(module + SfxSlotRequestRva);
    }
    g_regen = Matches(module, RegenStoreRva, RegenStoreSig);
    if (Matches(module, FindItemRva, FindItemSig)) {
        g_find_item = reinterpret_cast<FindItemFn>(module + FindItemRva);
    }
    LOG_INFO(Core,
             "VR gun: direct fire installed (InvokeGun, event 318 call, ConsumeBullets, "
             "CanFireGun, GetBehaviorId, GetWeaponId, rows, GetDmyPolyMtx, PlaySound ok; muzzle "
             "flash {}, regen pause {}, bullet counts {}): the bang at the trigger pull for the "
             "pistols, Blunderbuss and Ludwig's Rifle",
             g_flash ? "ok" : "MISMATCH (none)", g_regen ? "ok" : "MISMATCH (none)",
             g_find_item != nullptr ? "ok" : "unreadable");
    return true;
}

// ---- The right hand's shots (core/vr/trick_weapons.h Shot; BladeShotTick) ------------------------

// The right weapon's bullet rows (refType 1) among its transformed form's judges (400 .. 999),
// scanned once per weapon, and the shot and charged shot picked from them: the gun-blade's
// transformed R2 (550, 910 or 500, else its first bullet row: its bullets are checked and spent);
// the Moonlight Sword's waves (the first after its R2 550 and after its charged R2 570, nothing
// else); the Stake Driver's charged R2 (570 or 930, when a bullet). The log lists them all.
struct BladeRows {
    s32 variation{-1};
    s32 shot{-1};    // judge, -1 none
    s32 charged{-1}; // judge, -1 none
};

BladeRows ScanBladeRows(s32 variation, VR::Tricks::Shot shot) {
    namespace T = VR::Tricks;
    BladeRows r{};
    r.variation = variation;
    std::array<s32, 32> found{};
    u32 n = 0;
    std::string list;
    for (s32 judge = 400; judge <= 999; ++judge) {
        RowOut row{};
        g_behavior_row(&row, 100000000 + variation * 1000 + judge, 1);
        if (!Plausible(row.row, 4) || Read<u8>(row.row + RowRefType) != 1) {
            continue;
        }
        if (n < found.size()) {
            found[n++] = judge;
        }
        list += fmt::format(" {} (bullet {} category {})", judge, Read<s32>(row.row + RowRefId),
                            Read<u8>(row.row + RowCategory));
    }
    const auto first_of = [&](std::span<const s32> judges) {
        for (const s32 j : judges) {
            if (std::find(found.begin(), found.begin() + n, j) != found.begin() + n) {
                return j;
            }
        }
        return -1;
    };
    const auto first_in = [&](s32 lo, s32 hi) {
        for (u32 k = 0; k < n; ++k) {
            if (found[k] >= lo && found[k] <= hi) {
                return found[k];
            }
        }
        return -1;
    };
    switch (shot) {
    case T::Shot::GunBlade:
        r.shot = first_of(T::HeavyJudgesB);
        r.shot = r.shot >= 0 ? r.shot : (n > 0 ? found[0] : -1);
        break;
    case T::Shot::MoonWave:
        // Only the rows right after its R2 and charged R2: any other bullet row of the weapon may
        // be an attack the game charges bullets for (its transformed L2), never fired free here.
        r.shot = first_in(551, 569);
        r.charged = first_in(571, 599);
        r.charged = r.charged >= 0 ? r.charged : r.shot;
        break;
    case T::Shot::StakeBlast:
        // Its own charged rows only: an attack row there is the charged swing's (the melee's).
        r.charged = first_of(T::ChargedJudgesB);
        r.shot = r.charged;
        break;
    case T::Shot::Bow:
        // The quick shot: its R1 (400 .. 449); the charged: its charged R2, else its R2.
        r.shot = first_in(400, 449);
        r.charged = first_of(T::ChargedJudgesB);
        r.charged = r.charged >= 0 ? r.charged : first_of(T::HeavyJudgesB);
        r.charged = r.charged >= 0 ? r.charged : r.shot;
        if (r.shot < 0) {
            r.shot = r.charged;
        }
        break;
    case T::Shot::None:
        break;
    }
    LOG_INFO(Core, "VR blade shot: variation {}'s bullet rows (judges 400-999):{} -> shot {}, "
                   "charged {}",
             variation, list.empty() ? std::string{" none"} : list, r.shot, r.charged);
    return r;
}

struct BladeTick {
    u8* chr{nullptr};
    u32 handled{0};
    BladeRows rows{};
    float cooldown_left{0.0f};
    s64 last_ns{0};
    u32 shots{0};
    // The gun-blade's muzzle flash, renewed every update for its time (as the gun's).
    s32 flash_ffx{-1};
    s32 flash_dummy{-1};
    s32 flash_slot{FirstBladeFlashSlot};
    float flash_left{0.0f};
};
BladeTick g_blade{};

void RequestBladeFlash(u8* chr, const BladeTick& t) {
    if (!g_flash || g_dry_run || t.flash_ffx < 0) {
        return;
    }
    u8* const ctl = chr + OffsetSfxCtl;
    if (Read<u8*>(ctl) == g_base + SfxCtlVtableRva) {
        g_sfx_slot(ctl, t.flash_slot, t.flash_ffx, &t.flash_dummy, 0);
    }
}

} // Anonymous namespace

void InstallGun(VAddr base, u64 size) {
    if (g_base != nullptr) {
        return;
    }
    auto* const module = reinterpret_cast<u8*>(base);
    if (size <= WorldChrManRva + 8) {
        LOG_INFO(Core, "VR gun: eboot too small ({:#x}) for Bloodborne 1.09; the gun is vanilla",
                 size);
        return;
    }
    g_base = module;
    if (const char* value = std::getenv("VR_GUN_LOG"); value != nullptr) {
        g_log = std::max(std::atoi(value), 1);
    }
    if (const char* dry = std::getenv("VR_GUN_DRYRUN"); dry != nullptr) {
        g_dry_aim = true;
        g_dry_run = std::string_view{dry} != "aim";
    }
    bool wcm_ok = Matches(module, IsMainPlayerRva, IsMainPlayerSig);
    if (wcm_ok) {
        s32 disp{};
        std::memcpy(&disp, module + IsMainPlayerRva + 13, sizeof(disp));
        wcm_ok = IsMainPlayerRva + 17 + static_cast<s64>(disp) == WorldChrManRva;
    }
    if (Matches(module, DmyPolyRva, DmyPolySig)) {
        g_dmy_poly = reinterpret_cast<DmyPolyFn>(module + DmyPolyRva);
    }
    try {
        g_code = std::make_unique<Xbyak::CodeGenerator>(1024);
        g_aim = wcm_ok && InstallAim(module);
    } catch (const Xbyak::Error& error) {
        LOG_ERROR(Core, "VR gun: code emission failed ({}); aim hook not installed", error.what());
        g_aim = false;
    }
    if (!wcm_ok) {
        LOG_WARNING(Core,
                    "VR gun: the WorldChrMan proof (IsMainPlayer) differs; the gun is vanilla");
    }
    g_fire = InstallFire(module, wcm_ok);
    InstallTwangListener(module);
    if (g_aim) {
        LOG_INFO(Core,
                 "VR gun: aim hook at {:#x} (emitter window ok, WorldChrMan ok, GetDmyPolyMtx {}): "
                 "shots leave along the visible barrel | {}{}{}",
                 EmitSiteRva, g_dmy_poly != nullptr ? "ok" : "MISMATCH", G::GunSettingsText(),
                 g_log > 0 ? fmt::format(" log={}", g_log) : std::string{},
                 g_dry_run   ? " DRY RUN (nothing written or fired)"
                 : g_dry_aim ? " DRY RUN (aim: the game's own direction)"
                             : "");
    }
}

// weapon_wall (core/vr/solid_weapon.h): the left gun's line along its barrel, from its back
// (dummy 20100) to its muzzle, as far from the left hand's grip (L_Weapon's origin, dummy 340)
// along the barrel; measured once per gun as it is in the hand (GunBarrelReach).
constexpr s32 LeftGripDummy = 340;
struct BarrelReach {
    s32 key{-1}; // family * 1000 + attach
    bool valid{false};
    s32 attach{-1};
    float back{0.0f};
    float front{0.0f};
};
BarrelReach g_barrel{};

void MeasureBarrel(u8* chr, const TickState& t, bool in_hand) {
    if (!in_hand || t.info.family == G::Family::None) {
        g_barrel.valid = false;
        g_barrel.key = -1;
        return;
    }
    const s32 key = static_cast<s32>(t.info.family) * 1000 + t.info.attach;
    if (key == g_barrel.key) {
        return; // measured
    }
    g_barrel.valid = false; // another gun: none until it shows in the hand
    const s32 muzzle_id =
        t.info.attach == 348 || t.info.family == G::Family::Sprayer ? 20206 : 20215;
    B::Vec3 grip{};
    B::Vec3 back{};
    B::Vec3 muzzle{};
    if (!DummyAt(chr, LeftGripDummy, grip) || !DummyAt(chr, PartLineDummy, back) ||
        !DummyAt(chr, muzzle_id, muzzle) || B::Length(muzzle - back) < 0.05f ||
        B::Length(back - grip) > 1.0f) {
        return; // not shown in the hand yet
    }
    const B::Vec3 axis = B::Normalize(muzzle - back);
    g_barrel = {key, true, t.info.attach, B::Dot(back - grip, axis), B::Dot(muzzle - grip, axis)};
    LOG_INFO(Core,
             "VR gun: {}'s barrel for weapon_wall: {:.0f} .. {:.0f} cm along it from the left "
             "grip (attach {}, muzzle dummy {})",
             G::FamilyName(t.info.family), g_barrel.back * 100.0f, g_barrel.front * 100.0f,
             t.info.attach, muzzle_id);
}

// shield_guard (core/vr/shield_guard.h): the two shields' rows raised while it is on, the
// game's values back when it is turned off. Cheap: two row lookups a frame.
void ShieldGuardTick() {
    static std::array<VR::Shield::RowPatch, VR::Shield::Targets.size()> patches{};
    static std::array<bool, VR::Shield::Targets.size()> warned{};
    const bool on = VR::Tricks::GetTrickSettings().shield_guard;
    for (std::size_t i = 0; i < patches.size(); ++i) {
        const VR::Shield::Target& target = VR::Shield::Targets[i];
        u8* const row = const_cast<u8*>(GunWeaponParamRow(target.weapon));
        if (row == nullptr) {
            continue;
        }
        const auto result = patches[i].Step(row, target.at_least, on);
        const VR::Shield::Guard& g = patches[i].Game();
        const VR::Shield::Guard& w = patches[i].Written();
        if (result == VR::Shield::RowPatch::Result::Raised) {
            LOG_INFO(Core,
                     "VR shield: weapon {} guards better: physical {:.0f} -> {:.0f}%, arcane "
                     "{:.0f} -> {:.0f}, fire {:.0f} -> {:.0f}, bolt {:.0f} -> {:.0f}, stability "
                     "{} -> {}",
                     target.weapon, g.phys, w.phys, g.mag, w.mag, g.fire, w.fire, g.thun, w.thun,
                     g.repel, w.repel);
        } else if (result == VR::Shield::RowPatch::Result::Restored) {
            LOG_INFO(Core, "VR shield: weapon {}: the game's own guard values back",
                     target.weapon);
        } else if (result == VR::Shield::RowPatch::Result::Implausible && !warned[i]) {
            warned[i] = true;
            LOG_WARNING(Core, "VR shield: weapon {}'s row differs from 1.09: left as it is",
                        target.weapon);
        }
    }
}

void GunPoseTick(u8* chr, const GunBody& body) {
    if (g_base == nullptr || chr == nullptr) {
        return;
    }
    ShieldGuardTick();
    auto& t = g_tick;
    const auto& gs = G::GetGunSettings();
    const s64 now = NowNs();
    ToolGazeTick(now);
    const float dt = StepSeconds(t, now);
    static u32 player_epoch = 0;
    static s64 player_since_ns = 0;
    if (chr != t.chr) {
        ++player_epoch;
        player_since_ns = now;
        // A new character object (a load): nothing of the old one carries over.
        const u32 handled = t.handled;
        t = {};
        t.chr = chr;
        t.handled = handled;
        t.last_ns = now;
    }
    t.cooldown_left = std::max(t.cooldown_left - dt, 0.0f);
    t.regen_left = std::max(t.regen_left - dt, 0.0f);
    if (t.flash_left > 0.0f) {
        t.flash_left -= dt;
        if (t.flash_left > 0.0f) {
            RequestFlash(chr, t);
        }
    }
    if (t.regen_left > 0.0f && !g_dry_run) {
        u8* const ctrl = Read<u8*>(chr + OffsetChrCtrl);
        u8* const regen = Plausible(ctrl) ? Read<u8*>(ctrl + OffsetCtrlRegen) : nullptr;
        if (Plausible(regen)) {
            regen[OffsetRegenPercent] = 0; // as TAE event 225 (the player update resets it)
        }
    }
    ScanTick(chr, now);
    if (!g_fire) {
        LogStats(t, now);
        return;
    }
    // The left gun (every 4th update; at once for a new pull).
    Combat::GunPull pull{};
    const bool have_pull = Combat::LatestGunPull(pull) && pull.serial != t.handled;
    if (t.updates++ % 4 == 0 || have_pull) {
        t.bid = BehaviorId(chr, JudgeShot);
        t.info = G::GunOfBehavior(t.bid);
        t.gun = G::DirectGunOf(t.info.family);
        t.can_fire = t.info.family != G::Family::None && g_can_fire(chr, 0) != 0;
        t.two_handed = TwoHanded(chr);
    }
    const char* hard = HardReason(chr, body);
    const bool busy = !body.left_ik || body.left_weight < 0.5f;
    const bool hidden = HeldLeftHidden();
    MeasureBarrel(chr, t, !t.two_handed && !hidden && VR::Tricks::GetTrickSettings().weapon_wall);
    const u8* data = DataModule(chr);
    const bool stamina = data == nullptr || Read<s32>(data + OffsetStamina) > 0;
    const bool direct_ok = gs.fire_direct && t.gun != nullptr && !t.two_handed && body.body;
    Combat::GunStatus status{};
    status.ns = now;
    status.family = static_cast<u8>(t.info.family);
    status.attach = t.info.attach;
    status.aim = g_aim;
    status.in_hand = t.info.family != G::Family::None && !t.two_handed && !hidden;
    status.player_epoch = player_epoch;
    status.player_since_ns = player_since_ns;
    status.direct_ok = direct_ok;
    status.can_fire = t.can_fire;
    status.cooldown_left_s = t.cooldown_left;
    status.cadence_s = t.gun != nullptr ? t.gun->cadence_s * gs.rate_scale : 0.0f;
    status.ready = direct_ok && hard == nullptr && !busy && !hidden && stamina &&
                   t.cooldown_left <= 0.0f && t.can_fire && !t.pending;
    if (t.info.family != t.logged_family || direct_ok != t.logged_direct) {
        t.logged_family = t.info.family;
        t.logged_direct = direct_ok;
        LOG_INFO(Core, "VR gun: left weapon {} (bid {} attach {}){}: {}",
                 G::FamilyName(t.info.family), t.bid, t.info.attach,
                 t.two_handed ? " held two-handed" : "",
                 direct_ok          ? "the bang at the pull"
                 : t.gun == nullptr ? "the game's own L2 (not a direct-fire gun)"
                                    : "the game's own L2");
    }

    // VR_GUN_PARRY_PROBE (tests): a due shot at the followed target's attack.
    if (g_scan.probe_at != 0 && now >= g_scan.probe_at && !t.pending && t.gun != nullptr) {
        g_scan.probe_at = 0;
        ProbePull(chr, t, now);
    }
    // A new pull (one from before a load or a long stall is not one: it was never for this
    // moment).
    if (have_pull) {
        t.handled = pull.serial;
        if (pull.direct && now - pull.press_ns > StalePullNs) {
            LOG_INFO(Core, "VR gun: pull {} ignored ({} ms old)", pull.serial,
                     (now - pull.press_ns) / Ms);
        } else if (pull.direct) {
            if (t.pending) {
                g_stats.denied.fetch_add(1, std::memory_order_relaxed);
                Combat::PushGunEvent({Combat::GunEventKind::Denied, t.pull.serial,
                                      static_cast<u8>(t.info.family), 0.0f, now, "superseded"});
                LOG_INFO(Core, "VR gun: pull {} dropped (pull {} came first)", t.pull.serial,
                         pull.serial);
            }
            t.pending = true;
            t.pull = pull;
            t.first_look = true;
        }
    }
    status.handled = t.handled;
    if (t.pending) {
        G::PullDecision d{};
        if (!direct_ok) {
            // The route changed under the pull (another weapon, two hands): the game's own L2.
            d = {G::PullAction::Vanilla, t.gun == nullptr ? "no direct gun" : "two-handed"};
        } else {
            G::PullInputs in{};
            in.age_ms = static_cast<float>(now - t.pull.press_ns) * 1e-6f;
            in.first = t.first_look;
            in.cooldown_left_s = t.cooldown_left;
            in.hard = hard;
            in.busy = busy;
            in.hidden = hidden;
            in.stamina = stamina;
            in.busy_ms = gs.buffer_ms;
            in.busy_mode = gs.busy == G::Busy::Vanilla ? G::BusyMode::Vanilla
                           : gs.busy == G::Busy::Drop  ? G::BusyMode::Drop
                                                       : G::BusyMode::Buffer;
            d = G::DecidePull(in);
            if (d.action == G::PullAction::Wait && t.first_look) {
                LOG_INFO(Core, "VR gun: pull {} waits ({}; cooldown {:.3f} s, arm {} {:.2f})",
                         t.pull.serial, d.why, t.cooldown_left, body.left_ik ? "yours" : "game's",
                         body.left_weight);
            }
        }
        t.first_look = false;
        switch (d.action) {
        case G::PullAction::Wait:
            break;
        case G::PullAction::Fire:
            t.pending = false;
            t.can_fire = g_can_fire(chr, 0) != 0;
            Fire(chr, t, now);
            if (body.dodge_attack && t.can_fire) {
                // Attacks through dodges: the dodge goes on, its i-frames end with the shot
                // (dodge_attack_iframes=0).
                LOG_INFO(Core, "VR gun: pull {} fired during the dodge", t.pull.serial);
                IframesAttackCut("shot", t.pull.serial);
            }
            break;
        case G::PullAction::Deny:
            t.pending = false;
            g_stats.denied.fetch_add(1, std::memory_order_relaxed);
            Combat::PushGunEvent({Combat::GunEventKind::Denied, t.pull.serial,
                                  static_cast<u8>(t.info.family), 0.0f, now, d.why});
            LOG_INFO(Core, "VR gun: pull {} denied after {:.0f} ms ({})", t.pull.serial,
                     static_cast<float>(now - t.pull.press_ns) * 1e-6f, d.why);
            break;
        case G::PullAction::Vanilla:
            t.pending = false;
            g_stats.handed.fetch_add(1, std::memory_order_relaxed);
            {
                std::scoped_lock lock{g_shot_mutex};
                g_handed = t.pull; // the game's own shot still leaves along this pull's barrel
            }
            Combat::PushGunEvent({Combat::GunEventKind::Vanilla, t.pull.serial,
                                  static_cast<u8>(t.info.family), 0.0f, now, d.why});
            LOG_INFO(Core, "VR gun: pull {} -> the game's L2 after {:.0f} ms ({})", t.pull.serial,
                     static_cast<float>(now - t.pull.press_ns) * 1e-6f, d.why);
            break;
        }
    }
    status.cooldown_left_s = t.cooldown_left;
    Combat::PublishGunStatus(status);
    if (!t.status_logged) {
        t.status_logged = true;
        LOG_INFO(Core,
                 "VR gun: first pose tick on thread '{}' (body {}, driven {}, left {} {:.2f})",
                 Common::GetCurrentThreadName(), body.body ? 1 : 0, body.driven ? 1 : 0,
                 body.left_ik ? "yours" : "game's", body.left_weight);
    }
    LogStats(t, now);
}

bool GunBarrelReach(s32& attach, float& back, float& front) {
    if (!g_barrel.valid) {
        return false;
    }
    attach = g_barrel.attach;
    back = g_barrel.back;
    front = g_barrel.front;
    return true;
}

// bow_direct: an arrow costs stamina as a swing does: its behaviour row's cost (the game's shot
// clip would take it; 1 .. 200, else a light swing's 20), the quick shot at melee_stamina_pct, the
// charged one in full (as a heavy swing); none left, no arrow. `row_cost`: what the row said (-1:
// not read).
s32 BowStaminaCost(s32 bid, bool charged, s32& row_cost) {
    row_cost = -1;
    if (g_behavior_row != nullptr && bid > 0) {
        RowOut row{};
        g_behavior_row(&row, bid, 1);
        if (Plausible(row.row, 4)) {
            row_cost = Read<s32>(row.row + RowStamina);
        }
    }
    const s32 base = row_cost >= 1 && row_cost <= 200 ? row_cost : 20;
    const s32 pct = charged ? 100 : std::clamp(VR::Melee::GetMeleeSettings().stamina_pct, 0, 100);
    return pct > 0 ? std::max((base * pct + 50) / 100, 1) : 0; // (at least 1, as a swing's share)
}

// (A charged arrow always costs stamina: melee_stamina_pct=0 frees the quick shot only.)
bool BowOutOfStamina(u8* chr, bool charged) {
    const u8* data = DataModule(chr);
    return (charged || VR::Melee::GetMeleeSettings().stamina_pct > 0) && data != nullptr &&
           Read<s32>(data + OffsetStamina) <= 0;
}

void BladeShotTick(u8* chr, const BladeBody& body) {
    namespace T = VR::Tricks;
    if (g_base == nullptr || chr == nullptr || !g_fire) {
        return;
    }
    auto& t = g_blade;
    const s64 now = NowNs();
    const float dt =
        t.last_ns != 0 ? std::clamp(static_cast<float>(now - t.last_ns) * 1e-9f, 0.0f, 0.1f) : 0.0f;
    t.last_ns = now;
    if (chr != t.chr) {
        const u32 handled = t.handled;
        t = {};
        t.chr = chr;
        t.handled = handled;
        t.last_ns = now;
    }
    t.cooldown_left = std::max(t.cooldown_left - dt, 0.0f);
    if (t.flash_left > 0.0f) {
        t.flash_left -= dt;
        if (t.flash_left > 0.0f) {
            RequestBladeFlash(chr, t);
        }
    }
    {
        // bow_direct: the game's sounds heard while the bow is up and its release sound unknown.
        const auto& bs = VR::Bow::GetBowSettings();
        Combat::MeleeStatus up{};
        g_twang_listen.store(bs.enabled && bs.direct && g_listen_code != nullptr &&
                                 g_twang.load(std::memory_order_relaxed) < 0 &&
                                 Combat::LatestMeleeStatus(up) && now - up.ns <= StatusMaxAgeNs &&
                                 up.bow_hand >= 0,
                             std::memory_order_relaxed);
    }
    Combat::BladeShot req{};
    if (!Combat::LatestBladeShot(req) || req.serial == t.handled) {
        return;
    }
    t.handled = req.serial;
    const auto shot = static_cast<T::Shot>(req.shot);
    Combat::MeleeStatus ms{};
    const T::ShotWeapon* w = nullptr;
    const char* why = nullptr;
    const GunBody gb{true, body.driven, body.arm_free, 1.0f, body.reason, false};
    if (now - req.press_ns > StalePullNs) {
        why = "too old";
    } else if (!Combat::LatestMeleeStatus(ms) || now - ms.ns > StatusMaxAgeNs) {
        why = "no melee status";
    } else if (w = T::ShotWeaponOf(ms.variation); w == nullptr || w->shot != shot || !ms.form_b ||
                                                    (shot == T::Shot::Bow && ms.bow_hand < 0)) {
        why = "not that weapon in its transformed form";
    } else if (const char* hard = HardReason(chr, gb); hard != nullptr) {
        why = hard;
    } else if (!body.arm_free) {
        why = "the game has the arm";
    } else if (t.cooldown_left > 0.0f) {
        why = "cooldown";
    } else if (shot == T::Shot::Bow && BowOutOfStamina(chr, req.charged)) {
        why = Combat::NoStamina;
    }
    s32 judge = -1;
    if (why == nullptr) {
        if (t.rows.variation != ms.variation) {
            t.rows = ScanBladeRows(ms.variation, shot);
        }
        judge = req.charged && t.rows.charged >= 0 ? t.rows.charged : t.rows.shot;
        if (judge < 0) {
            why = shot == T::Shot::StakeBlast
                      ? "no bullet row (the blast may be an attack: the charged swing's)"
                      : "no bullet row";
            if (shot == T::Shot::Bow) {
                g_bow_no_rows.store(true); // its shots stay the game's own
            }
        }
    }
    // Where it leaves from, which way: the transformed part's line (its own two dummies), else the
    // presenter's (the wave out through the blade, the punch, the drawn arrow) in the world.
    B::Vec3 base{};
    B::Vec3 tip{};
    const bool line = why == nullptr && w->base >= 0 && DummyAt(chr, w->base, base) &&
                      DummyAt(chr, w->tip, tip) && B::Length(tip - base) > 0.05f;
    B::Vec3 origin{};
    B::Vec3 dir{};
    B::Anchor anchor{};
    B::HandsSample hands{};
    const bool mapped = why == nullptr && req.dir_valid && B::LatestAnchor(anchor) &&
                        anchor.valid && now - anchor.steady_ns <= AnchorMaxAgeNs &&
                        B::LatestHands(hands) && hands.space_serial == req.space_serial;
    if (why == nullptr && shot == T::Shot::Bow) {
        // The arrow: from just ahead of the bow hand along the drawn line (as BowArrow's).
        B::Vec3 hand{};
        if (!mapped) {
            why = "no aim (the view, or a recenter since the release)";
        } else if (!DummyAt(chr, ms.bow_hand == 0 ? 340 : 300, hand)) {
            why = "no bow hand";
        } else {
            dir = B::Normalize(B::TrackingToWorld(req.dir, anchor.base_yaw, 1.0f), ChrForward(chr));
            origin = hand + dir * BowOriginAheadM;
        }
    } else if (why == nullptr) {
        const B::Vec3 along = line ? B::Normalize(tip - base) : ChrForward(chr);
        dir = mapped ? B::Normalize(B::TrackingToWorld(req.dir, anchor.base_yaw, 1.0f), along)
                     : along;
        B::Vec3 fist{};
        const bool have_fist = !line && DummyAt(chr, 300, fist); // R_Weapon's grip
        origin = !line ? fist : shot == T::Shot::MoonWave ? (base + tip) * 0.5f : tip;
        if (shot == T::Shot::GunBlade && !line) {
            why = "no barrel line (the weapon's dummies)";
        } else if (!line && !have_fist) {
            why = "no hand";
        }
    }
    if (why != nullptr) {
        Combat::PushMeleeEvent({Combat::MeleeEventKind::Shot, req.serial,
                                static_cast<float>(req.shot), -1.0f, now, why});
        LOG_INFO(Core, "VR blade shot: #{} {} refused ({})", req.serial, static_cast<int>(req.shot),
                 why);
        return;
    }
    // The Stake Driver's blast takes a bullet too (2026-10-12): its weapon's own cost,
    // at least one, from the Quicksilver and Blood Bullets.
    const s32 stake_cost = shot == T::Shot::StakeBlast ? std::max(BulletCost(chr, 1, HandRight), 1)
                                                       : 0;
    // (An unreadable count, -1, never holds a blast back: the consume below decides.)
    const s32 quick = shot == T::Shot::StakeBlast ? GoodsCount(chr, QuicksilverBullets) : 0;
    const s32 blood = shot == T::Shot::StakeBlast ? GoodsCount(chr, BloodBullets) : 0;
    const bool dry = (shot == T::Shot::GunBlade && g_can_fire(chr, 1) == 0) ||
                     (shot == T::Shot::StakeBlast && quick >= 0 && blood >= 0 &&
                      quick + blood < stake_cost);
    if (dry) {
        Sound(w->dry, tip);
        t.cooldown_left = DryCooldownS;
        Combat::PushMeleeEvent({Combat::MeleeEventKind::Shot, req.serial,
                                static_cast<float>(req.shot), 0.0f, now, "no bullets"});
        LOG_INFO(Core, "VR blade shot: #{} DRY (no bullets)", req.serial);
        return;
    }
    if (shot == T::Shot::MoonWave) {
        // A bit of target assist (2026-10-13): within MoonWaveAssistDeg of the target
        // the game picked (locked on or not), the wave turns part of the way to it, as a gun's
        // shot does (GentleAssist); a wave sent away from it flies where it was sent.
        G::AssistCfg cfg{};
        cfg.strength = T::MoonWaveAssist;
        cfg.cone_deg = T::MoonWaveAssistDeg;
        cfg.unlocked = true;
        const bool have_target = Read<u8>(chr + OffsetLockChosen) != 0;
        const G::AssistOut a = G::GentleAssist(dir, origin, Read3(chr + OffsetLockPoint),
                                               have_target, VR::IsLockedOn(), false, cfg);
        if (a.weight > 0.0f) {
            LOG_INFO(Core, "VR blade shot: #{} the wave {:.1f} deg off the target: turned {:.1f} deg "
                           "toward it",
                     req.serial, a.angle_deg, a.moved_deg);
            dir = a.dir;
        }
    }
    const s32 bid = 100000000 + ms.variation * 1000 + judge;
    g_blade_aim = {req.serial, bid, now, origin, dir, {}};
    u8 ok = 0;
    u8 consumed = 0;
    s32 cost = 0;
    // A bow shot's stamina before the game's call: whatever that call takes itself counts toward
    // the arrow's cost (never twice).
    u8* const bow_data = shot == T::Shot::Bow && !g_dry_run ? DataModule(chr) : nullptr;
    const s32 bow_before = bow_data != nullptr ? Read<s32>(bow_data + OffsetStamina) : 0;
    if (!g_dry_run) {
        const s32 start = shot == T::Shot::Bow ? (ms.bow_hand == 0 ? 340 : 300) : w->tip;
        ok = g_invoke_gun(chr, bid, start, 0, HandRight, HandRight, 1, 1);
        if (shot == T::Shot::GunBlade) {
            cost = BulletCost(chr, 1, HandRight);
            consumed = g_consume(chr, cost);
        } else if (shot == T::Shot::StakeBlast && ok != 0) {
            cost = stake_cost;
            consumed = g_consume(chr, cost);
        }
    }
    ++t.shots;
    if (ok != 0 && (shot == T::Shot::GunBlade || shot == T::Shot::StakeBlast)) {
        g_blade_kick_variation.store(ms.variation, std::memory_order_relaxed);
        g_blade_kick_ns.store(now, std::memory_order_release);
    }
    if (shot == T::Shot::GunBlade) {
        t.cooldown_left = w->cadence_s;
        t.flash_slot = FirstBladeFlashSlot + static_cast<s32>(t.shots % FlashSlots);
        t.flash_ffx = w->flash_ffx;
        t.flash_dummy = w->tip;
        t.flash_left = 1.0f;
        RequestBladeFlash(chr, t);
        Sound(w->bang, tip);
    } else if (shot == T::Shot::StakeBlast) {
        t.cooldown_left = w->cadence_s; // the blast's cooldown
    } else if (shot == T::Shot::Bow && ok != 0) {
        const s32 twang = g_twang.load(std::memory_order_relaxed);
        Sound(twang >= 0 ? twang : ThrowWhoosh, origin); // the string's release
        s32 row_cost = -1;
        const s32 stamina = BowStaminaCost(bid, req.charged, row_cost);
        if (bow_data != nullptr) {
            const s32 game_took = bow_before - Read<s32>(bow_data + OffsetStamina);
            const s32 after = std::max(bow_before - stamina, VR::Melee::StaminaFloor);
            std::memcpy(bow_data + OffsetStamina, &after, sizeof(after));
            LOG_INFO(Core,
                     "VR blade shot: #{} the arrow's stamina {} -> {} ({} of the row's {}{}; the "
                     "game's call took {})",
                     req.serial, bow_before, after, stamina, row_cost,
                     row_cost >= 1 && row_cost <= 200 ? "" : ": not read, a light swing's 20",
                     game_took);
        }
    }
    Combat::PushMeleeEvent({Combat::MeleeEventKind::Shot, req.serial, static_cast<float>(req.shot),
                            ok != 0 ? 1.0f : -1.0f, now, ok != 0 ? "" : "the game refused it"});
    LOG_INFO(Core,
             "VR blade shot: #{} FIRE {} variation {} judge {}{} bid={} ok={} cost={} consumed={} "
             "from {} along {}{} {:.0f} ms after the press{}",
             req.serial, static_cast<int>(req.shot), ms.variation, judge,
             req.charged ? " (charged)" : "", bid, ok, cost, consumed, V3(origin), YawPitch(dir),
             req.dir_valid ? " (yours)" : " (the weapon's line)",
             static_cast<float>(now - req.press_ns) * 1e-6f,
             g_dry_run ? " DRY RUN (nothing fired)" : "");
}

bool GunBowDirectOk() {
    return g_fire && !g_bow_no_rows.load();
}

s32 GunBehaviorRefType(s32 bid) {
    if (g_behavior_row == nullptr || bid <= 0) {
        return -1;
    }
    RowOut row{};
    g_behavior_row(&row, bid, 1);
    return Plausible(row.row, 4) ? Read<u8>(row.row + RowRefType) : -1;
}

void GunLaunchTail(u8* p) {
    if (!g_aim_pending.load(std::memory_order_acquire) || p == nullptr) {
        return;
    }
    const s64 now = NowNs();
    const s32 bullet = Read<s32>(p + OffsetPBullet);
    const u32 owner = Read<u32>(p + OffsetPOwner);
    std::scoped_lock lock{g_shot_mutex};
    LastAim& l = g_last;
    if (owner != l.owner) {
        return;
    }
    if (now - l.ns > 100 * Ms && now > l.chain_until) {
        g_aim_pending.store(false, std::memory_order_relaxed);
        return;
    }
    const B::Vec3 v = Read3(p + OffsetPVel);
    const B::Vec3 at = Read3(p + OffsetPPos);
    if (bullet == l.bullet && now - l.ns <= 100 * Ms) {
        ++l.launches;
        l.chain_until = now + 1500 * Ms;
        if (g_log >= 1 && l.launches <= 8) {
            LOG_INFO(
                Core,
                "VR gun: LAUNCH #{}.{} bullet={} at {} v{} {:.2f} m/s: d_ray={:.2f} deg, start "
                "{:.1f} cm from the aimed start{}",
                l.n, l.launches, bullet, V3(at), YawPitch(v), B::Length(v),
                B::Angle(v, l.ray) / B::Deg, B::Length(at - l.start) * 100.0f,
                l.pull_ns != 0 && l.launches == 1
                    ? fmt::format(", trigger -> launch {:.1f} ms",
                                  static_cast<float>(now - l.pull_ns) * 1e-6f)
                    : std::string{});
        }
        return;
    }
    // Child bullets (the pistol's chain 501 .. 504, the Blunderbuss's 601 ..): spawned where the
    // segment before them ended or hit, with its direction.
    if (bullet > l.bullet && bullet <= l.bullet + 5 && now <= l.chain_until && l.children < 12) {
        ++l.children;
        if (g_log >= 2 || (g_log >= 1 && l.children <= 4)) {
            const B::Vec3 rel = at - l.start;
            LOG_INFO(Core,
                     "VR gun: CHILD #{} bullet={} at {} {:.2f} m along the ray, {:.1f} cm off it, "
                     "v{} {:.2f} m/s ({:.2f} deg off the ray) {:.0f} ms after the launch",
                     l.n, bullet, V3(at), B::Dot(rel, l.ray),
                     B::Length(B::Perp(rel, l.ray)) * 100.0f, YawPitch(v), B::Length(v),
                     B::Length(v) > 1e-3f ? B::Angle(v, l.ray) / B::Deg : 0.0f,
                     static_cast<float>(now - l.ns) * 1e-6f);
        }
    }
}

void GunKick(VR::Body::ModelTarget& left_target, s64 now_ns) {
    const s64 at = g_kick_ns.load(std::memory_order_acquire);
    const float visual = G::GetGunSettings().recoil_visual;
    if (at == 0 || visual <= 0.0f || now_ns - at > KickNs || now_ns < at || HeldLeftHidden()) {
        return;
    }
    const auto family = static_cast<G::Family>(g_kick_family.load(std::memory_order_relaxed));
    const s32 attach = g_kick_attach.load(std::memory_order_relaxed);
    const G::Kick kick = G::KickOf(family);
    const float e = G::KickEnvelope(kick, static_cast<float>(now_ns - at) * 1e-6f) * visual;
    left_target = G::ApplyKick(left_target, attach, kick, e);
    if (g_log >= 3) {
        LOG_INFO(Core, "VR gun: kick e={:.2f} pitch={:.1f} deg back={:.1f} cm at +{:.0f} ms", e,
                 e * kick.pitch_deg, e * kick.back_m * 100.0f,
                 static_cast<float>(now_ns - at) * 1e-6f);
    }
}

void StakeDischarge(u8* chr, s64 now_ns) {
    g_blade_kick_variation.store(1100, std::memory_order_relaxed);
    g_blade_kick_ns.store(now_ns, std::memory_order_release);
    // a116_042630 (charged R2), event 96: native discharge FFX 252020 for 0.1 s.
    // Its character-animation attachment is replaced by the tracked weapon's tip.
    if (chr != nullptr) {
        g_blade.flash_slot = FirstBladeFlashSlot + static_cast<s32>(++g_blade.shots % FlashSlots);
        g_blade.flash_ffx = 252020;
        g_blade.flash_dummy = 11206;
        g_blade.flash_left = 0.1f;
        RequestBladeFlash(chr, g_blade);
    }
}

void BladeKick(VR::Body::ModelTarget& right_target, s64 now_ns) {
    const s64 at = g_blade_kick_ns.load(std::memory_order_acquire);
    const float visual = G::GetGunSettings().recoil_visual;
    constexpr s64 BladeKickNs = 1200 * Ms; // (the slowest, the stake's, settled to under 0.1%)
    if (at == 0 || visual <= 0.0f || now_ns - at > BladeKickNs || now_ns < at) {
        return;
    }
    const G::Kick kick = G::BladeKickOf(g_blade_kick_variation.load(std::memory_order_relaxed));
    const float e = G::KickEnvelope(kick, static_cast<float>(now_ns - at) * 1e-6f) * visual;
    right_target = G::ApplyBladeKick(right_target, kick, e);
}

const u8* GunWeaponParamRow(s32 weapon) {
    if (g_weapon_row == nullptr || weapon < 0) {
        return nullptr;
    }
    WeaponRowOut out{};
    g_weapon_row(&out, weapon);
    return Plausible(out.row, 2) ? out.row : nullptr;
}

} // namespace VR::Bloodborne
