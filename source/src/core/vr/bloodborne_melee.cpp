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
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "common/thread.h"
#include "core/vr/bloodborne_gun.h"
#include "core/vr/bloodborne_held.h"
#include "core/vr/bow.h"
#include "core/vr/bow_pose.h"
#include "core/vr/bow_channel.h"
#include "core/vr/bloodborne_iframes.h"
#include "core/vr/bloodborne_layout.h"
#include "core/vr/bloodborne_melee.h"
#include "core/vr/bloodborne_motion.h"
#include "core/vr/bloodborne_transform.h"
#include "core/vr/bloodborne_tuning.h"
#include "core/vr/bloodborne_whip.h"
#include "core/vr/whip_spin.h"
#include "core/vr/whip_damage.h"
#include "core/vr/whip_moves.h"
#include "core/vr/whip_settings.h"
#include "core/vr/body_channel.h"
#include "core/vr/combat_channel.h"
#include "core/vr/feature_cost.h"
#include "core/vr/hook_util.h"
#include "core/vr/bloodborne_visceral_stage.h"
#include "core/vr/melee_settings.h"
#include "core/vr/left_attack.h"
#include "core/vr/melee_window.h"
#include "core/vr/plunge.h"
#include "core/vr/solid_weapon.h"
#include "core/vr/transform_settings.h"
#include "core/vr/trick_weapons.h"
#include "core/vr/two_hand_grip.h"
#include "core/vr/stake_driver.h"
#include "core/vr/visceral_channel.h"
#include "core/vr/visceral_thrust.h"
#include "core/vr/vr_session.h"

namespace VR::Bloodborne {

namespace {

namespace B = VR::Body;
namespace L = VR::Bloodborne::Layout;
namespace M = VR::Melee;
using Hook::Plausible;
using Hook::Read;

// Module-relative addresses in the 1.09 eboot (patch file address - 0x400000). Every prologue is
// unique in .text (offline analysis).

// BehaviorModule::InvokeAttack, `bool(BehaviorModule* = [[chr+0x3B0]+0x98], s32 bid, s32
// attackType, u32 index <= 7, s32 flags)`: TAE event 1's call. A new bid in a slot ends the old
// attack and creates the new one (CreateAttack: the BehaviorParam stamina taken once); the same
// bid only marks the slot refreshed. vt+0xE8 (UpdateAttackSlots, later in the same frame) ends
// every slot not refreshed. The game's TAE uses slots 0-2; 6 and 7 are ours.
constexpr u64 InvokeAttackRva = 0x1A293B0;
constexpr std::array<u8, 28> InvokeAttackSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
    0x81, 0xEC, 0x28, 0x01, 0x00, 0x00, 0x41, 0x89, 0xCE, 0x41, 0x89, 0xD7, 0x89, 0xF3,
};
constexpr u32 OffsetAttackModule = 0x98; // module bag -> the BehaviorModule InvokeAttack takes
constexpr u32 OffsetAttackSlots = 0x20;  // {u32 hit handle, u8 refreshed} x 8
constexpr u32 FirstSlot = 6;
// PlayerIns vt+0x1D0 GetBehaviorId, `s32(chr, judge, hand, 0, 0)` = 1e8 + variation * 1000 +
// judge (called through the live vtable only when the slot holds it).
constexpr u64 GetBehaviorIdRva = 0x18F9070;
constexpr std::array<u8, 28> GetBehaviorIdSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
    0x83, 0xEC, 0x78, 0x44, 0x89, 0xC3, 0x41, 0x89, 0xCE, 0x41, 0x89, 0xD5, 0x41, 0x89,
};
constexpr u32 SlotGetBehaviorId = 0x1D0;
constexpr s32 HandRight = -1;
constexpr s32 HandLeft = -2;
// PlayerIns vt+0x380, the attack's hand: -2 (the left weapon) for weapon style 2, or while the
// HKS input bits ([[chr+0x58]+0x38]+0x5C) hold the L2 request (0x40) without R1 (0x80) and the
// attack type [chr+0x1C4] is -1 (idle). Its bytes prove those offsets; a window is refused then.
constexpr u64 HandCodeRva = 0x18FB100;
constexpr std::array<u8, 32> HandCodeSig = {
    0x48, 0x8B, 0x87, 0xB8, 0x04, 0x00, 0x00, 0x8B, 0x50, 0x08, 0x83, 0xFA, 0x02, 0x0F, 0x95, 0xC0,
    0x0F, 0xB6, 0xC0, 0x83, 0xC8, 0xFE, 0x8B, 0xB7, 0xC4, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x4F, 0x58,
};
constexpr u32 OffsetEquip = 0x4B8; // -> +0x08 the weapon style (1 = form A one-handed, 2 = the
                                   // left weapon two-handed, 3 / 5 = form B)
constexpr u32 OffsetEquipStyle = 0x08;
constexpr u32 OffsetInputBits = 0x5C;   // HKS action state (ChrCtrl +0x38): u16
constexpr u32 OffsetAttackType = 0x1C4; // PlayerIns: s32, -1 idle (4, and 11 in style 1: left)
// ChrThrowModule::TryThrowAll, `s32(ChrThrowModule* = [[chr+0x3B0]+0x88], s32 padType, s32
// throwTypeId)` -> the ThrowParam id started or -1: the R1 input's visceral / riposte / backstab
// check (pad type 3), from PlayerIns::Update like site A.
constexpr u64 TryThrowAllRva = 0x1A60620;
constexpr std::array<u8, 24> TryThrowAllSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
    0x53, 0x48, 0x83, 0xEC, 0x68, 0x89, 0x55, 0x84, 0x89, 0x75, 0x80, 0x48,
};
constexpr s32 PadTypeR1 = 3;
constexpr u32 OffsetThrowAttackType = 0x64; // HKS action state: the throw-attack type (logs)
// The game's attack notices to its AIs (melee_telegraph; core/vr/melee_window.h TelegraphTracker).
// PlayerIns vt+0x440, `void(PlayerIns*, s32 attack type)`: what HKS act 6 (attack type) calls when
// an attack state starts (the type [chr+0x1C4] leaves -1; every right-hand attack sets 0; 4, the
// left hand's, and > 10 notify nothing): AI message 0 {type 0, mode 1, the hunter's handle} to
// every AI manager (0x12E5320). An AI whose target is the hunter arms its FindAttack timer (its
// observer's +0xB0 = 1, +0xB4 = 0.2-0.7 s at random) and raises INTERUPT_FindAttack when it runs
// out (0x12A9040); its battle script decides (FindAttack_Step / _Guard / _Act: the clinic's scourge
// beast steps away 15% of the time within 3 m). Its bytes prove the type gate and the message.
constexpr u64 NotifyAttackRva = 0x18F8EB0;
constexpr std::array<u8, 101> NotifyAttackSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83,
    0xEC, 0x28, 0x4C, 0x8B, 0x35, 0x18, 0xAD, 0xAE, 0x03, 0x49, 0x8B, 0x06, 0x48, 0x89, 0x45,
    0xD0, 0x83, 0xFE, 0x0A, 0x0F, 0x87, 0xA4, 0x00, 0x00, 0x00, 0x83, 0xFE, 0x04, 0x0F, 0x84,
    0x9B, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x87, 0xB0, 0x03, 0x00, 0x00, 0x48, 0x8B, 0x40, 0x40,
    0x48, 0x8B, 0x40, 0x08, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0x83, 0x00, 0x00, 0x00, 0xC7, 0x45,
    0xB8, 0x00, 0x00, 0x00, 0x00, 0xC7, 0x45, 0xBC, 0x01, 0x00, 0x00, 0x00, 0xC7, 0x45, 0xC4,
    0xFF, 0xFF, 0xFF, 0xFF, 0xC7, 0x45, 0xC8, 0x14, 0x00, 0x00, 0x00,
};
constexpr u32 SlotNotifyAttack = 0x440;
constexpr s32 AttackTypeRight = 0;
// ChrIns vt+0x448, `void(ChrIns*)`: AI message 7 (INTERUPT_ParryTiming), what the per-update flag
// copy 0x1514DA0 sends while the HKS action state's +0x40 bit 2 is set: TAE ChrActionFlag 63, the
// first frame (0.000-0.033 s) of every attack clip. Parrying enemies time their parry by it.
constexpr u64 ParryTimingRva = 0x18C6260;
constexpr std::array<u8, 83> ParryTimingSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
    0x83, 0xEC, 0x28, 0x4C, 0x8B, 0x35, 0x68, 0xD9, 0xB1, 0x03, 0x49, 0x8B, 0x06, 0x48,
    0x89, 0x45, 0xD0, 0x48, 0x8B, 0x87, 0xB0, 0x03, 0x00, 0x00, 0x48, 0x8B, 0x40, 0x40,
    0x48, 0x8B, 0x40, 0x08, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0x85, 0x00, 0x00, 0x00, 0x48,
    0xB9, 0x07, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x48, 0x89, 0x4D, 0xB8, 0xC7,
    0x45, 0xC4, 0xFF, 0xFF, 0xFF, 0xFF, 0xC7, 0x45, 0xC8, 0x14, 0x00, 0x00, 0x00,
};
constexpr u32 SlotParryTiming = 0x448;
constexpr s64 TelegraphMaxAgeNs = 200'000'000; // an older telegraph: the update stalled, skip it
// VR_MELEE_AI_LOG (tests): the AIs' side of the notices. [base+0x553B0A0]: the AI managers (s32
// count +0x20, array +0x28, 0xD8 bytes each), each with its AI entries [+0x98, +0xA0) (the
// dispatch 0x12E5320); entry +0x8B0 -> [.] the AI, whose +0x560 is its observer (+0xB0 the
// FindAttack timer, +0xB4 its threshold: the notice's handler 0x1222FDD writes them, the
// interrupt 0x12A9040 reads them) and whose [+0] -> vt+0x30 is its ChrIns (the handler's own
// "not myself" check).
constexpr u64 AiManagersRva = 0x553B0A0;
constexpr u64 AiDispatchRva = 0x12E5320;
constexpr std::array<u8, 83> AiDispatchSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
    0x83, 0xEC, 0x18, 0x49, 0x89, 0xF6, 0x4C, 0x89, 0x75, 0xC8, 0x48, 0x8B, 0x9F, 0x98,
    0x00, 0x00, 0x00, 0x4C, 0x8B, 0xAF, 0xA0, 0x00, 0x00, 0x00, 0x41, 0xB7, 0x01, 0x4C,
    0x39, 0xEB, 0x0F, 0x84, 0x0B, 0x01, 0x00, 0x00, 0x41, 0xB7, 0x01, 0x66, 0x66, 0x2E,
    0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x03, 0x48, 0x85, 0xC0,
    0x0F, 0x84, 0xE4, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0xA0, 0xB0, 0x08, 0x00, 0x00,
};
constexpr u64 AiNoticeRva = 0x1222FDD;
constexpr std::array<u8, 73> AiNoticeSig = {
    0x49, 0x8B, 0x06, 0x4C, 0x8B, 0xB0, 0x60, 0x05, 0x00, 0x00, 0x8B, 0x5B, 0x08, 0x4C, 0x89,
    0xF7, 0xE8, 0xAE, 0x9F, 0x08, 0x00, 0x41, 0xB7, 0x01, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0xB0,
    0xFE, 0xFF, 0xFF, 0x83, 0xFB, 0xFF, 0x0F, 0x84, 0xA7, 0xFE, 0xFF, 0xFF, 0x48, 0x8B, 0x08,
    0x48, 0x89, 0xC7, 0xFF, 0x91, 0xA8, 0x00, 0x00, 0x00, 0x39, 0xD8, 0x0F, 0x85, 0x93, 0xFE,
    0xFF, 0xFF, 0x41, 0xC7, 0x86, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F,
};
constexpr u64 AiFindAttackRva = 0x12A9040;
constexpr std::array<u8, 63> AiFindAttackSig = {
    0x55, 0x48, 0x89, 0xE5, 0x53, 0x50, 0xC5, 0xFA, 0x10, 0x87, 0xB0, 0x00, 0x00, 0x00, 0xC5, 0xFA,
    0x10, 0x0D, 0x9A, 0xC7, 0x67, 0x03, 0xC5, 0xF2, 0x5C, 0x8F, 0xB4, 0x00, 0x00, 0x00, 0x31, 0xC0,
    0xC5, 0xF8, 0x2E, 0xC8, 0x76, 0x19, 0xC5, 0xF0, 0x57, 0xC9, 0xC5, 0xF8, 0x2E, 0xC1, 0x76, 0x0F,
    0xC7, 0x87, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xB8, 0x02, 0x00, 0x00, 0x00,
};
constexpr u32 OffsetAiManagerCount = 0x20;
constexpr u32 OffsetAiManagerArray = 0x28;
constexpr u32 AiManagerSize = 0xD8;
constexpr u32 OffsetAiListBegin = 0x98;
constexpr u32 OffsetAiListEnd = 0xA0;
constexpr u32 OffsetAiEntryThink = 0x8B0;
constexpr u32 OffsetAiObserver = 0x560;
constexpr u32 OffsetAiInterrupts = 0x68;
constexpr u32 OffsetObsTimer = 0xB0;
constexpr u32 OffsetObsThreshold = 0xB4;
constexpr u32 SlotAiChr = 0x30;
// ChrIns and EnemyIns vtables (their vt+0x440 / +0x448 slots at 0x538E710 / 0x538F000).
constexpr u64 ChrInsVtableRva = 0x538E2D0;
constexpr u64 EnemyInsVtableRva = 0x538EBC0;
constexpr u32 OffsetCtrlWorld = 0x90; // ChrCtrl: the world matrix, three float4 rows {R, t}
// PlaySound, `void(u64* handle_out, const float pos[3], s32 type, s32 id)` (TAE event 128's
// call), and that event's sound type: 1, or 0xA when its table byte +3 is set.
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
// TAE event 118 SpawnFFX_Blade's call, `void(SfxCtl* = chr+0x2A0, s32 slot, s32 ffx, const s32*
// base dummy, const s32* tip dummy)`: a blade trail between two weapon dummies, alive while it is
// asked for every update (a node per slot, like SfxSlotRequest). A private slot.
constexpr u64 BladeFfxRva = 0x1898E60;
constexpr std::array<u8, 32> BladeFfxSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50, 0x4C, 0x89,
    0x45, 0xD0, 0x49, 0x89, 0xCF, 0x41, 0x89, 0xD4, 0x41, 0x89, 0xF5, 0x49, 0x89, 0xFE, 0x45, 0x85,
};
constexpr s32 TrailSlot = 30200; // the vial uses 30000-30007, the gun's flash 30100-30103
constexpr s32 TrailFfx = 250;    // the weapons' common trail (Saw Cleaver, a109, a121, ...)
// ChrIns constructor (0x18B9F05): the effect controller's vtable stored at chr+0x2A0.
constexpr u64 CtlCtorRva = 0x18B9F05;
constexpr std::array<u8, 14> CtlCtorSig = {
    0x48, 0x8D, 0x05, 0x84, 0x50, 0xA7, 0x03, 0x49, 0x89, 0x85, 0xA0, 0x02, 0x00, 0x00,
};
constexpr u64 SfxCtlVtableRva = 0x532EF90;
constexpr u32 OffsetSfxCtl = 0x2A0;
// TAE event 225 SetSPRegenRatePercent (0x1A22200): `[[[chr+0x58]+0x38]+0x34] = percent`; reset
// to 100 every update, so a pause is a 0 written every update.
constexpr u64 RegenStoreRva = 0x1A22200;
constexpr std::array<u8, 15> RegenStoreSig = {
    0x48, 0x8B, 0x40, 0x58, 0x48, 0x8B, 0x40, 0x38, 0x41, 0x8A, 0x0C, 0x24, 0x88, 0x48, 0x34,
};
constexpr u32 OffsetRegenPercent = 0x34;
// ChrIns::GetDmyPolyMtx, `s32(ChrIns*, Mtx34* out, const s32* id, u32 max)`.
constexpr u64 DmyPolyRva = 0x18C3BF0;
constexpr std::array<u8, 32> DmyPolySig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC,
    0x28, 0x41, 0x89, 0xCF, 0x48, 0x89, 0xD3, 0x48, 0x89, 0x75, 0xB0, 0x4C, 0x8B, 0x35, 0xCE, 0xFF,
};
constexpr s32 RightWeaponDummy = 300; // R_Weapon's origin (the grip)
constexpr s32 LeftWeaponDummy = 340;  // L_Weapon's origin (the left hand's grip)
constexpr s32 LeftPartDummy = 20100;  // the left weapon's dummy 100 (the gun's back end)
constexpr float GeometryFarM = 3.0f;  // a weapon part not shown answers farther from the grip
// The player BehaviorModule's vt+0xB0 (slot 0x5397CC0), `bool(module, MapObj*, res, u8 soft)`:
// every hit of the hunter's attacks on the map (0x18B8730); it asks vt+0x80 (0x1A32910) for the
// wall bounce (hitType 2 rows). The whole function (37 bytes, position independent) is replayed
// after our call; the 16 patched bytes hold no branch target (its jumps land at +0x11 and +0x23).
// (An identical copy at 0x1A332E0 belongs to another class and is not touched.)
constexpr u64 MapHitRva = 0x1A332B0;
constexpr std::array<u8, 37> MapHitSig = {
    0x55, 0x48, 0x89, 0xE5, 0x83, 0xBA, 0x10, 0x02, 0x00, 0x00, 0x1D, 0x75, 0x04,
    0x31, 0xC0, 0xEB, 0x12, 0x4C, 0x8B, 0x07, 0x0F, 0xB6, 0xC1, 0x48, 0x89, 0xD6,
    0x89, 0xC2, 0x41, 0xFF, 0x90, 0x80, 0x00, 0x00, 0x00, 0x5D, 0xC3,
};
constexpr size_t MapHitPatchLen = 16;
// The attack-hit manager [0x553D740]: entries of 0x300 bytes at [mgr+0x10] + (handle >> 16) *
// 0x300 for handles up to 0x7FFFFF ([entry] == handle), heap entries linked from [mgr] through
// +0x2F0 above (index 0x80). Entry: +0x8C the bid (InvokeAttack's own refresh check), +0xA0 the
// capsule radius, +0x210 its end A now, +0x220 / +0x230 the previous positions.
constexpr u64 HitManagerRva = 0x553D740;
constexpr u32 OffsetPool = 0x10;
constexpr u32 EntrySize = 0x300;
constexpr u32 OffsetEntryBid = 0x8C;
constexpr u32 OffsetEntryRadius = 0xA0;
constexpr u32 OffsetEntryA = 0x210;
constexpr u32 OffsetEntryPrev = 0x230;
constexpr u32 OffsetHeapNext = 0x2F0;
constexpr u64 StakeEntryCopyRva = 0x185D988;
constexpr std::array<u8, 17> StakeEntryCopySig = {
    0x48, 0x8D, 0x7B, 0x54, 0xBA, 0x71, 0x01, 0x00, 0x00,
    0x4C, 0x89, 0xEE, 0xE8, 0xDF, 0x0B, 0x36, 0x01};
constexpr u64 StakeDummyStoreRva = 0x1F1439D;
constexpr std::array<u8, 19> StakeDummyStoreSig = {
    0x44, 0x89, 0x87, 0x34, 0x01, 0x00, 0x00, 0x44, 0x89, 0x8F,
    0x38, 0x01, 0x00, 0x00, 0xC5, 0xFA, 0x11, 0x47, 0x4C};
bool g_stake_geometry = false;
// ChrIns::IsMainPlayer: its lea proves the WorldChrMan global.
constexpr u64 IsMainPlayerRva = 0x18C2D00;
constexpr std::array<u8, 24> IsMainPlayerSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xFB, 0x4C, 0x8D,
    0x35, 0x67, 0xBB, 0xC7, 0x03, 0x49, 0x8B, 0x06, 0x48, 0x85, 0xC0, 0x75,
};
constexpr u64 WorldChrManRva = 0x553E878;
constexpr u32 OffsetMainPlayer = 0x60;

// ChrData (module bag +0x20): HP, stamina and its max.
constexpr u32 OffsetHp = 0xF8;
constexpr u32 OffsetStamina = 0x134;
constexpr u32 OffsetStaminaMax = 0x138;
// InvokeAttack's stamina cost (0x1A297CB, after the hit entry is made): the row's cost (r12d)
// comes off ChrData +0x134 ([[[module+8]+0x3B0]+0x20], clamped to -50 .. max) unless that ChrData's
// +0x200 bit 0x80 is set (the game's per-character no-stamina-consumption flag: the six stamina
// subtractions of the eboot all test it, then the debug flag 0x553E88A). Its bytes prove the read
// and the flag; melee_stamina_pct=0 holds the bit for our call (core/vr/melee_window.h
// SwingStamina; 1 .. 99 let the cost come off and give all but that share back: PartialSwing).
constexpr u64 InvokeStaminaRva = 0x1A297CB;
constexpr std::array<u8, 37> InvokeStaminaSig = {
    0x49, 0x8B, 0x45, 0x08, 0x48, 0x8B, 0x80, 0xB0, 0x03, 0x00, 0x00, 0x4C, 0x8B,
    0x78, 0x20, 0x41, 0x8B, 0x9F, 0x34, 0x01, 0x00, 0x00, 0x45, 0x85, 0xE4, 0x7E,
    0x44, 0x41, 0xF6, 0x87, 0x00, 0x02, 0x00, 0x00, 0x80, 0x75, 0x5C,
};
constexpr u32 OffsetDataFlags = 0x200;
// The damage result of a hit (the final-damage detour's rcx; also the map hit's).
constexpr u32 ResBid = 0x38;
constexpr u32 ResAtkParam = 0x3C;
constexpr u32 ResHand = 0x44;
constexpr u32 ResBullet = 0x14C;
constexpr u32 ResHandle = 0x1F0;
constexpr u32 ResPosition = 0x180;

constexpr s64 Ms = 1'000'000;
constexpr s64 WindowMaxAgeNs = 250 * Ms; // an older publication: the presenter stopped
constexpr s64 StaleCloseNs = 150 * Ms;   // a window closed longer ago is not opened any more
constexpr s64 HitGraceNs = 120 * Ms;     // a hit this long after the end still counts (DamageStep)
// dodge_attack (fair mode): a swing begun longer than this before a dodge is its follow-through,
// which the dodge cancels as the game's own dodge does (its i-frames stay); a younger one is a
// swing into the dodge (it goes on and ends them).
constexpr s64 DodgeCarryGraceNs = 150 * Ms;
constexpr s64 StatusLogNs = 10000 * Ms;
constexpr float RightArmMin = 0.9f;
// The hit dummy belongs to the weapon in hand: this near R_Weapon (a part not shown answers more
// than 3 m away). 2 m: the transformed Bloodletter's head is 1.05-1.5 m out of the hand, its hit
// dummy past 1.5 m, so every form-B swing was refused (the 2026-10-13 run).
constexpr float DummyNearM = 2.0f;
// ... during a flick's transform clip, which may throw the new part out past its rest (the Hunter
// Axe's head reaches 1.69 m from the grip as the haft extends, then settles at 1.30 m); a part not
// shown answers more than 3 m away.
constexpr float FlickNearM = 2.5f;
// ... on a physical whip's chain (core/vr/bloodborne_whip.h), which pays out to 6 m and more.
constexpr float WhipNearM = 9.0f;
// A whip part is in the hand when its handle dummy (12120, B_Master: it never stretches) is.
constexpr s32 WhipHandleDummy = 12120;

using InvokeAttackFn = u8(PS4_SYSV_ABI*)(void* module, s32 bid, s32 type, u32 index, s32 flags);
using GetBehaviorIdFn = s32(PS4_SYSV_ABI*)(void* chr, s32 judge, s32 hand, s32 a, s32 b);
using TryThrowAllFn = s32(PS4_SYSV_ABI*)(void* throws, s32 pad_type, s32 throw_type);
using PlaySoundFn = void(PS4_SYSV_ABI*)(u64* handle, const float* pos, s32 type, s32 id);
using BladeFfxFn = void(PS4_SYSV_ABI*)(void* ctl, s32 slot, s32 ffx, const s32* base,
                                       const s32* tip);
using DmyPolyFn = s32(PS4_SYSV_ABI*)(void* chr, void* out, const s32* ids, u32 max);
using NotifyAttackFn = void(PS4_SYSV_ABI*)(void* chr, s32 attack_type);
using ParryTimingFn = void(PS4_SYSV_ABI*)(void* chr);
using AiChrFn = u8*(PS4_SYSV_ABI*)(void* accessor);

// ---- Install state ------------------------------------------------------------------------------

u8* g_base = nullptr;
bool g_installed = false; // the attack path (physical melee can run)
bool g_physical = false;  // ... and melee=physical
bool g_visceral = false;
bool g_sound = false;
bool g_trail = false;
bool g_regen = false;
bool g_free_flag = false; // InvokeAttack's no-stamina-consumption flag matched (..._pct=0)
bool g_map_hook = false;
InvokeAttackFn g_invoke = nullptr;
using HandCodeFn = s32(PS4_SYSV_ABI*)(u8* chr);
HandCodeFn g_hand_original = nullptr;
bool g_left_hand_hook = false;
std::unique_ptr<Xbyak::CodeGenerator> g_hand_code;

s32 PS4_SYSV_ABI PhysicalAttackHand(u8* chr) {
    return M::LeftAttack::Applies(chr) ? HandLeft : g_hand_original(chr);
}

// InvokeAttack -> 0x1576090 asks PlayerIns::GetAttackHand independently of the
// behavior row. Native L2 normally selects -2 through the animation/input state.
// A physical swing has neither. Override only our scoped call on this thread;
// shooting, right/dual attacks, other actors and other threads execute the original.
bool InstallLeftAttackHand(u8* module) {
    constexpr size_t Replay = 16; // whole instructions, no relative operands
    g_hand_code = std::make_unique<Xbyak::CodeGenerator>(64);
    auto& c = *g_hand_code;
    auto* original = const_cast<u8*>(c.getCurr());
    for (size_t i = 0; i < Replay; ++i) c.db(HandCodeSig[i]);
    c.jmp(Xbyak::util::ptr[Xbyak::util::rip]);
    c.dq(reinterpret_cast<u64>(module + HandCodeRva + Replay));
    c.ready();
    if (!Hook::MakeWritable(module + HandCodeRva, Replay)) return false;
    g_hand_original = reinterpret_cast<HandCodeFn>(original);
    Hook::WriteJump(module + HandCodeRva, Replay, reinterpret_cast<void*>(&PhysicalAttackHand));
    return true;
}
TryThrowAllFn g_try_throw = nullptr;
PlaySoundFn g_play_sound = nullptr;
BladeFfxFn g_blade_ffx = nullptr;
DmyPolyFn g_dmy_poly = nullptr;
const u8* g_sound_table = nullptr;
std::unique_ptr<Xbyak::CodeGenerator> g_code;
// VR_MELEE_LOG: 1 = + the capsule's first frames of each window, 2 = + every frame of it.
int g_log = 0;
bool g_dry_run = false;   // VR_MELEE_DRYRUN: everything decided and logged, no attack (the
                          // telegraph's AI notices are still sent: they are no attack)
bool g_telegraph = false; // the AI notices matched (melee_telegraph can send them)
bool g_ai_log = false;    // VR_MELEE_AI_LOG: the AIs' side (tests; also with melee=vanilla)
bool g_ai_god = false;    // VR_MELEE_AI_GOD (tests, with the AI log): AIs near the hunter keep HP

s64 NowNs() {
    return B::SteadyNowNs();
}

// melee_plunge's settings as the air tracker's config (core/vr/plunge.h).
VR::Melee::Plunge::Config PlungeConfig() {
    const auto& ms = VR::Melee::GetMeleeSettings();
    VR::Melee::Plunge::Config c{};
    c.enabled = ms.plunge;
    c.min_drop_m = ms.plunge_min_drop_m;
    c.damage = ms.plunge_damage;
    c.max_hold_ns = static_cast<s64>(ms.plunge_hold_s * 1e9f);
    return c;
}
// The motion signals older than this (a menu, a load: no main-player update) are no air.
constexpr s64 AirMaxAgeNs = 250 * Ms;

std::string V3(B::Vec3 v) {
    return fmt::format("({:.3f},{:.3f},{:.3f})", v.x, v.y, v.z);
}

u8* MainPlayer() {
    if (g_base == nullptr) {
        return nullptr;
    }
    u8* const world = Read<u8*>(g_base + WorldChrManRva);
    u8* const chr = Plausible(world) ? Read<u8*>(world + OffsetMainPlayer) : nullptr;
    return Plausible(chr) ? chr : nullptr;
}

u8* Module(const u8* chr, u32 offset) {
    u8* const modules = Read<u8*>(chr + L::OffsetChrModules);
    u8* const module = Plausible(modules) ? Read<u8*>(modules + offset) : nullptr;
    return Plausible(module) ? module : nullptr;
}

u8* HksState(const u8* chr) {
    u8* const ctrl = Read<u8*>(chr + L::OffsetChrCtrl);
    u8* const state = Plausible(ctrl) ? Read<u8*>(ctrl + L::OffsetActionState) : nullptr;
    return Plausible(state) ? state : nullptr;
}

bool DummyAt(u8* chr, s32 id, B::Vec3& out) {
    if (g_dmy_poly == nullptr || id < 0) {
        return false;
    }
    alignas(16) std::array<float, 8 * 12> m{};
    if (g_dmy_poly(chr, m.data(), &id, 1) < 1) {
        return false;
    }
    out = {m[3], m[7], m[11]};
    return B::IsFinite(out);
}

// The dummy's matrix (rows of [R | t]: its axes are R's columns).
bool DummyMtx(u8* chr, s32 id, std::array<float, 12>& out) {
    if (g_dmy_poly == nullptr || id < 0) {
        return false;
    }
    alignas(16) std::array<float, 8 * 12> m{};
    if (g_dmy_poly(chr, m.data(), &id, 1) < 1) {
        return false;
    }
    std::copy_n(m.begin(), 12, out.begin());
    return B::IsFinite(B::Vec3{m[3], m[7], m[11]});
}

// `p` (world) in the frame of the dummy matrix `g`, in cm.
B::Vec3 InFrameCm(const std::array<float, 12>& g, B::Vec3 p) {
    const float dx = p.x - g[3];
    const float dy = p.y - g[7];
    const float dz = p.z - g[11];
    return {(g[0] * dx + g[4] * dy + g[8] * dz) * 100.0f,
            (g[1] * dx + g[5] * dy + g[9] * dz) * 100.0f,
            (g[2] * dx + g[6] * dy + g[10] * dz) * 100.0f};
}

s32 WeaponStyle(const u8* chr) {
    u8* const equip = Read<u8*>(chr + OffsetEquip);
    return Plausible(equip, 4) ? Read<s32>(equip + OffsetEquipStyle) : -1;
}

s32 BehaviorId(u8* chr, s32 judge, s32 hand = HandRight) {
    u8* const vtable = Read<u8*>(chr);
    if (!Plausible(vtable)) {
        return -1;
    }
    u8* const fn = Read<u8*>(vtable + SlotGetBehaviorId);
    if (fn != g_base + GetBehaviorIdRva) {
        return -1;
    }
    return reinterpret_cast<GetBehaviorIdFn>(fn)(chr, judge, hand, 0, 0);
}

// The hit entry of a handle (nullptr when it is gone or is not the attack of `bid`).
u8* HitEntry(u32 handle, s32 bid) {
    u8* const mgr = Read<u8*>(g_base + HitManagerRva);
    if (!Plausible(mgr) || handle == 0xFFFFFFFFu) {
        return nullptr;
    }
    u8* entry = nullptr;
    if (handle <= 0x7FFFFF) {
        u8* const pool = Read<u8*>(mgr + OffsetPool);
        if (!Plausible(pool)) {
            return nullptr;
        }
        entry = pool + static_cast<size_t>(handle >> 16) * EntrySize;
    } else if ((handle >> 16) == 0x80) {
        u8* e = Read<u8*>(mgr);
        for (int i = 0; i < 256 && Plausible(e); ++i) {
            if (Read<u32>(e) == handle) {
                entry = e;
                break;
            }
            e = Read<u8*>(e + OffsetHeapNext);
        }
    }
    if (entry == nullptr || Read<u32>(entry) != handle ||
        Read<s32>(entry + OffsetEntryBid) != bid) {
        return nullptr;
    }
    return entry;
}

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

// ---- The attacks of our windows (the pose tick writes, the damage and map hooks read) ----------

struct Attack {
    s32 weapon_variation{-1};
    bool form_b{false};
    u32 serial{0};
    s32 bid{-1};
    u32 handle{0xFFFFFFFFu};
    M::MoveKind kind{M::MoveKind::Light};
    const M::MoveRow* row{nullptr};
    const M::MoveRow* light{nullptr}; // the form's light row (a heavy window judged light)
    bool verdict_light{false};        // a heavy window the classifier judged light
    s32 cost{0};                      // the stamina it took
    M::SwingStamina stamina_rule{};    // frozen for this attack, including refreshes
    int light_stamina_pct{50};
    float since_counted_s{10.0f};
    s64 created_ns{0};
    s64 ended_ns{0}; // 0 while alive
    u32 hits{0};
    u32 walls{0};
    s64 last_wall_ns{0};
    float damage{0.0f}; // dealt so far
    float tip{0.0f};    // the blade tip's speed the pose tick last saw (m/s)
    bool transform{false}; // the game's transform attack of a flick (core/vr/melee_moves.h)
    bool saw{false};       // native grinding attack: no swing-speed or cadence scaling
    bool powered{false};   // released Stake Driver charge: no swing-speed scaling
    bool flick{false};     // opened by a flick (FlickOpen): outside the combo budget
    float two_hand{1.0f};  // a two-handed swing's bonus (MeleeWindow::two_hand, the largest seen)
    // Opened while the hunter fell (melee_plunge, core/vr/plunge.h): held to the landing; its
    // fall's depth so far (the pose tick raises it) sets the plunge's damage.
    bool plunge{false};
    float drop_m{0.0f};
    // A whip form's own light / heavy / charged attack (not a transform attack on the chain): the
    // whip (core/vr/whip_moves.h WhipRecover, the game's pace of its moves; -1: not one), paced
    // from its lash, `created_ns` (its attack waits for the chain's tip to come up to speed, so a
    // stroke, a spin's revolution and a crack count alike; not the window's opening, which comes
    // 50 .. 250 ms before a stroke's lash); `pace` its share of a full hit, set by its first hit
    // that dealt damage (< 0: none yet; vr.ini whip_pace, core/vr/whip_damage.h).
    s32 whip_variation{-1};
    float pace{-1.0f};
};

std::mutex g_attack_mutex;
std::array<Attack, 4> g_attacks{}; // the newest four (a ring)
u32 g_attack_next = 0;
// The whip forms' damage pace (vr.ini whip_pace): one meter, every whip window that hurt.
VR::Whip::PaceMeter g_whip_pace; // (g_attack_mutex)

Attack* FindAttack(s32 bid, u32 handle, s64 now) {
    // handle 0 (a whip's capsules 1-3, which come with other handles): the newest such attack (two
    // quick lashes share the row: the live one, not the one in its grace after the end).
    Attack* best = nullptr;
    for (Attack& a : g_attacks) {
        if (a.serial != 0 && a.bid == bid && (handle == 0 || a.handle == handle) &&
            (a.ended_ns == 0 || now - a.ended_ns <= HitGraceNs)) {
            if (handle != 0) {
                return &a;
            }
            if (best == nullptr || a.created_ns > best->created_ns) {
                best = &a;
            }
        }
    }
    return best;
}

// The blade's bite (the damage / map hooks write, SolveBody reads).
std::atomic<s64> g_bite_ns{0};
std::atomic<float> g_bite_hold{0.0f};
std::atomic<bool> g_bite_wall{false};

// The visceral check of a new window (site A writes, the pose tick reads).
std::atomic<u32> g_visceral_checked{0};
// Physical viscerals (visceral_thrust=1, core/vr/visceral_thrust.h): while a visceral is open and
// a swing cannot start it, a new window waits up to this many game frames for the presenter's
// thrust verdict before its attack is created (site A writes, the pose tick reads).
std::atomic<bool> g_thrust_wait{false};
constexpr u32 ThrustWaitFrames = 8;

// Statistics (the 10 s line).
struct Stats {
    std::atomic<u64> windows{0};
    std::atomic<u64> created{0};
    std::atomic<u64> failed{0};
    std::atomic<u64> denied{0};
    std::atomic<u64> aborted{0};
    std::atomic<u64> refunds{0};
    std::atomic<u64> hits{0};
    std::atomic<u64> walls{0};
    std::atomic<u64> viscerals{0};
    std::atomic<u64> telegraphs{0};
    std::atomic<u64> unpaid{0}; // windows the combo budget could not pay (no attack)
    std::atomic<u64> buffered{0}; // swings held for the game's attack window (swing_buffer_ms)
    std::atomic<u64> buffer_opened{0}; // ... whose attack then opened
};
Stats g_stats{};

// ---- The pose tick ------------------------------------------------------------------------------

// A flick mid-swing (core/vr/melee_window.h FlickJoins): the window whose stroke a transform
// joined, and where its hand-off from the old form's attack to the transform attack stands.
enum class FlickPhase : u8 {
    Old,  // the old form's attack (if any) serves the window until the switch is handed over
    Gap,  // that attack ended (the old part goes): waiting for the new form in the hand
    Live, // the transform attack (or the new form's own row) serves the rest of the window
    Done, // nothing more for this window (it ended, or the transform stopped before its switch)
};

struct FlickStroke {
    u32 window{0};
    u32 run{0};    // the transform run
    u32 serial{0}; // its squeeze
    bool to_b{false};
    s32 style{-1}; // the form it switches to
    FlickPhase phase{FlickPhase::Done};
    u32 waited{0}; // updates the new form's hit dummy was not in the hand yet
    u32 old_hits{0};
    s64 old_created_ns{0};
    s64 gap_ns{0};
};

struct TickState {
    const u8* chr{nullptr};
    s64 last_ns{0};
    u32 updates{0};
    // The weapon (every few updates).
    s32 variation{-1};
    const M::WeaponMoves* moves{nullptr};
    s32 style{-1};
    s32 style_prev{-1};
    u32 style_stable{0}; // updates the style has kept its value
    bool form_b{false};
    const char* weapon_why{"not read yet"};
    // The window being served.
    M::LatchState latch{};
    Attack cur{};         // its attack (a copy of the ring's)
    bool cur_live{false}; // created and not finalized
    // The combo budget could not pay the window (Combat::MeleeWindow::unpaid): no attack, its
    // whoosh and trail only (cur holds its row), until it ends, the presenter pays at its commit
    // or a flick's transform attack takes the stroke over.
    bool unpaid_live{false};
    u32 cur_frames{0};
    bool whoosh_done{false};
    s32 whoosh{-1}; // the weapon's own swing sound and blade trail (core/vr/melee_moves.h)
    s32 trail_ffx{TrailFfx};
    s32 trail_base{-1};
    s32 trail_tip{-1};
    bool trail_checked{false}; // the trail's dummies were found (or replaced by the defaults)
    B::Vec3 last_a{};
    float regen_left{0.0f};
    FlickStroke flick{};
    // Logs.
    s32 logged_variation{-2};
    s32 geo_variation{-2}; // the blade geometry logged for (VR_MELEE_LOG)
    s32 geo_style{-2};
    bool logged_form_b{false};
    bool logged_ok{false};
    bool first_logged{false};
    s64 last_stats_ns{0};
    const char* last_deny{nullptr};
    // Attacks through dodges (dodge_attack): this update's kept dodge takes swings (MeleeBody::
    // dodge_attack) and the last window whose attack ended a dodge's i-frames.
    bool dodge_now{false};
    s32 dodge_clip{-1};
    u32 cut_serial{0};
    u32 dodge_cancel{0}; // the swing in its follow-through as this dodge began: the dodge ends it
    // The swing buffer (swing_buffer_ms): the window held for the game's attack window.
    M::SwingBuffer buffer{};
    u32 buffer_logged{0};
    // The two-handed form's blade in R_Weapon's frame (MeleeStatus::blade_tip), measured once
    // per settled form.
    struct TwoHandGeometry {
        s32 variation{-1};
        s32 style{-1};
        bool valid{false};
        B::Vec3 tip{};
        float strike_near{0.0f};
    } two_hand_geo{};
    // The blade's ends at the last pose tick (TrackBlade), and the weapon they were read for.
    s64 blade_ns{0};
    B::Vec3 blade_base{}, blade_tip{};
    float blade_yaw{0.0f}; // the play space's heading then
    s32 blade_variation{-1};
    s32 blade_style{-1};
    // The plunge (melee_plunge): the hunter in the air, from the motion signals.
    VR::Melee::Plunge::Tracker air{};
    // Simon's Bowblade's bow form (vr.ini bow): the hand its bow hangs on (-1: not the bow).
    s8 bow_hand{-1};
    s8 bow_part{-1};
    s8 bow_arrow_part{-1}; // its part on the string hand (the game's arrow: hidden, vr.ini bow)
    bool bow_arrow_logged{false};
    std::string bow_parts{"?"}; // (logs)
};
TickState g_tick{};
struct BowStringRig {
    const u8* node{};
    const u8* skeleton{};
    s32 count{};
    s32 bone{-1};
};
BowStringRig g_bow_string_rig{}; // body/pose thread, resolved by MeleeBowGeometry
struct BowStringRequest {
    BowStringRig rig{};
    B::Vec3 target{};
    s64 ns{};
};
SeqLock<BowStringRequest> g_bow_string_request;

// Two pose ticks further apart than this give no blade velocity ...
constexpr s64 BladeTickMaxNs = 100 * Ms;
// ... nor two with the play space turned more than this between them (a snap turn; a smooth turn
// at the default 120 degrees a second is 2 to 4 degrees an update).
constexpr float BladeTurnRad = 8.0f * 3.14159265f / 180.0f;

// The right weapon's blade every pose tick (melee_kill_ragdoll: the kill ragdoll's push, read by
// the damage step through MeleeHitInfoOf): its two ends and their velocities from this tick and the
// one before. Under g_attack_mutex.
struct BladeTrack {
    bool valid{false};
    B::Vec3 base{}, tip{};
    B::Vec3 base_vel{}, tip_vel{};
    s64 ns{0};
};
BladeTrack g_blade{};
// The right weapon's blade from its grip, at the last pose tick (MeleeBladeReach).
struct BladeReach {
    bool valid{false};
    float base{0.0f};
    float tip{0.0f};
};
BladeReach g_blade_reach{};
struct PartBlade {
    s32 key{-1};
    bool seen{false};
    s32 logged{-1};
    B::Vec3 base{};
    B::Vec3 tip{};
};
std::array<PartBlade, 2> g_part_blade{};

// A live attack during a kept dodge (dodge_attack): the dodge's invulnerability ends with it
// (dodge_attack_iframes=0; core/vr/bloodborne_iframes.h), once per window.
void DodgeAttackCut(TickState& t, u32 serial, const char* how) {
    if (!t.dodge_now || t.cut_serial == serial) {
        return;
    }
    t.cut_serial = serial;
    LOG_INFO(Core, "VR melee: #{} swing {} during the dodge (clip {}): it goes on, the attack lands",
             serial, how, t.dodge_clip);
    IframesAttackCut("swing", serial);
}

float StepSeconds(TickState& t, s64 now) {
    const float dt =
        t.last_ns != 0 ? std::clamp(static_cast<float>(now - t.last_ns) * 1e-9f, 0.0f, 0.1f) : 0.0f;
    t.last_ns = now;
    return dt;
}

// The right weapon's parts on the ChrAsm (model slots 11-14, WeaponPart vtable 0x539D100; a part
// attached to a body dummy has +0x138 = 2 and the dummy at +0x13C): the hand the Bowblade's bow
// hangs on in its bow form, 0 left (dummies 340-359) or 1 right (300-339). Neither found: the left
// (the game's bow form holds the bow in the left hand), logged.
constexpr s32 BowbladeVariation = 3200;
constexpr s32 StakeDriverVariation = 1100;
constexpr s32 BeastCutterVariation = 2400;
constexpr u32 OffsetChrAsm = 0x4D0;
constexpr u32 OffsetAsmParts = 0x28;
constexpr s32 RightPartSlot = 11;
constexpr u64 WeaponPartVtableRva = 0x539D100;
constexpr u32 OffsetPartMode = 0x138;
constexpr u32 OffsetPartDummy = 0x13C;

s8 BowHandOf(u8* chr, TickState& t) {
    u8* const parts = Read<u8*>(chr + OffsetChrAsm);
    s32 left = -1;
    s32 right = -1;
    s8 left_part = -1;
    s8 right_part = -1;
    std::string seen;
    if (Plausible(parts)) {
        for (s32 i = 0; i < 4; ++i) {
            u8* const part = Read<u8*>(parts + OffsetAsmParts + 8 * (RightPartSlot + i));
            if (!Plausible(part) || Read<u8*>(part) != g_base + WeaponPartVtableRva ||
                Read<s32>(part + OffsetPartMode) != 2) {
                continue;
            }
            const s32 dummy = Read<s32>(part + OffsetPartDummy);
            seen += fmt::format(" {}:dmy{}", i, dummy);
            if (dummy >= 340 && dummy <= 359) {
                left = dummy;
                left_part = static_cast<s8>(i);
            } else if (dummy >= 300 && dummy <= 339) {
                right = dummy;
                right_part = static_cast<s8>(i);
            }
        }
    }
    const s8 hand = left >= 0 ? 0 : right >= 0 ? 1 : 0;
    // The other hand's part: the game's own arrow (the string hand's).
    t.bow_part = hand == 0 ? left_part : right_part;
    const s8 arrow_part = hand == 0 ? right_part : left_part;
    if (arrow_part >= 0) {
        t.bow_arrow_part = arrow_part;
    }
    if (hand != t.bow_hand || seen != t.bow_parts) {
        t.bow_parts = seen;
        LOG_INFO(Core, "VR bow: the Bowblade's bow form: the bow hangs on the {} hand ({}), the {} "
                       "hand draws (parts{}){}",
                 hand == 0 ? "left" : "right",
                 left >= 0    ? fmt::format("dummy {}", left)
                 : right >= 0 ? fmt::format("dummy {}", right)
                              : std::string{"no hand dummy found: assumed"},
                 hand == 0 ? "right" : "left", seen.empty() ? " none" : seen,
                 t.bow_arrow_part >= 0
                     ? fmt::format(", the game's arrow on it: part {}", t.bow_arrow_part)
                     : std::string{", no part on it"});
    }
    return hand;
}

// stake_blast (core/vr/trick_weapons.h): the Stake Driver's transformed charged R2, its blast,
// is left out of the generated table. When the game's row for it is an attack (refType 0), the
// charged swing opens it: the heavy row's capsule with that judge. A bullet row is fired by the
// gun module instead (BladeShotTick). Looked up once; the log says which.
const M::WeaponMoves* StakeMovesFor(s32 variation, const M::WeaponMoves* moves) {
    namespace T = VR::Tricks;
    static M::WeaponMoves stake{};
    static s32 judge = 0; // 0 not looked up yet, -1 none
    if (variation != StakeDriverVariation || moves == nullptr || moves->rows[5].Valid() ||
        !moves->rows[4].Valid() || !T::GetTrickSettings().stake_blast || !g_stake_geometry) {
        return moves;
    }
    if (judge == 0) {
        judge = -1;
        std::string kinds;
        for (const s32 j : T::ChargedJudgesB) {
            const s32 kind = GunBehaviorRefType(100000000 + variation * 1000 + j);
            kinds += fmt::format(" {}: {}", j, kind < 0 ? "no row" : kind == 0 ? "an attack" : "a bullet");
            if (kind == 0 && judge < 0) {
                judge = j;
            }
        }
        if (judge > 0) {
            stake = *moves;
            stake.rows[5] = moves->rows[4];
            stake.rows[5].judge = static_cast<s16>(judge);
            stake.rows[5].charge = true;
            if (judge == 930) {
                // Retail native blast metadata; its body-relative sphere is relocated per
                // attack instance, never by changing the shared parameter table.
                stake.rows[5].stamina = 150;
                stake.rows[5].mv = 355;
                stake.rows[5].radius = 1.0f;
                stake.rows[5].dmy1 = 11206;
                stake.rows[5].dmy2 = -1;
            }
        }
        LOG_INFO(Core, "VR melee: the Stake Driver's transformed charged R2 rows:{} -> {}", kinds,
                 judge > 0 ? fmt::format("the charged swing opens judge {}", judge)
                           : std::string{"no attack row (a bullet is the gun module's)"});
    }
    return judge > 0 ? &stake : moves;
}

// The bare right fist (2026-10-16, the tester: a swing with the empty hand played the game's punch
// animation; it should hit as the weapons do, with no animation). Its rows are left out of the
// generated table (gen_judges.py keeps only parts a right-hand weapon dummy holds; the fist's hit
// is the hand's own dummy 10100), so a bare hand had no physical moves and its swing pressed R1.
// The game's own light / heavy / charged rows of the fist (judges 0 / 150 / 170, BehaviorParam_PC
// and AtkParam_Pc of 1.09; game-derived like core/vr/melee_moves.h, for the same runtime
// extraction before the source release), used only while the game has them as attacks.
const M::WeaponMoves* FistMovesFor(s32 variation, const M::WeaponMoves* moves) {
    namespace T = VR::Tricks;
    static constexpr M::WeaponMoves fists{
        T::FistVariation,
        "Bare fist",
        {{{0, 15, 100, 10100, -1, 0.400f, 0.060f, false, -1, -1, -1, -1, 0.0f, 0.0f},
          {150, 30, 125, 10100, -1, 0.400f, 0.090f, false, -1, -1, -1, -1, 0.0f, 0.0f},
          {170, 45, 145, 10100, -1, 0.400f, 0.120f, true, -1, -1, -1, -1, 0.0f, 0.0f},
          {},
          {},
          {}}}};
    static s32 checked = 0; // 0 not looked up yet, 1 the game has them, -1 not
    static u32 retry = 0;
    if (moves != nullptr || variation != T::FistVariation) {
        return moves;
    }
    // Until the game answers for all three (its param table not up yet reads "no row": asked
    // again every 60 updates, the swing the game's R1 meanwhile).
    if (checked == 0 && retry++ % 60 == 0) {
        std::string kinds;
        bool all = true;
        bool known = true;
        for (std::size_t i = 0; i < 3; ++i) {
            const s32 judge = fists.rows[i].judge;
            const s32 kind = GunBehaviorRefType(100000000 + variation * 1000 + judge);
            kinds += fmt::format(" {}: {}", judge,
                                 kind < 0 ? "no row" : kind == 0 ? "an attack" : "not an attack");
            all = all && kind == 0;
            known = known && kind >= 0;
        }
        if (!known) {
            return moves;
        }
        checked = all ? 1 : -1;
        LOG_INFO(Core, "VR melee: the bare right fist's rows:{} -> {}", kinds,
                 all ? "a swing punches (physical, no animation)"
                     : "left to the game's R1 (a row is not an attack)");
    }
    return checked > 0 ? &fists : moves;
}

void ReadWeapon(u8* chr, TickState& t) {
    const s32 bid0 = BehaviorId(chr, 0);
    const s32 variation_before = t.variation;
    t.variation =
        bid0 >= 100000000 && bid0 < 200000000 && bid0 % 1000 == 0 ? (bid0 - 100000000) / 1000 : -1;
    t.moves = t.variation >= 0 ? M::FindMoves(t.variation) : nullptr;
    // whip=physical: a whip form's rows on its simulated chain (core/vr/bloodborne_whip.h).
    t.moves = WhipMovesFor(t.variation, t.moves);
    t.moves = StakeMovesFor(t.variation, t.moves);
    t.moves = FistMovesFor(t.variation, t.moves);
    t.style = WeaponStyle(chr);
    if (t.style == t.style_prev && t.variation == variation_before) {
        ++t.style_stable;
    } else {
        t.style_stable = 0;
        t.style_prev = t.style;
    }
    t.form_b = VR::Tricks::PhysicalMeleeFormB(t.variation, t.style);
    // Simon's Bowblade's bow form (vr.ini bow, core/vr/bow.h): the hand its bow hangs on.
    t.bow_hand = t.variation == BowbladeVariation && t.form_b ? BowHandOf(chr, t) : -1;
    if (t.bow_hand < 0) {
        t.bow_arrow_part = -1;
        t.bow_arrow_logged = false;
    }
    if (t.variation < 0) {
        t.weapon_why = "no right weapon row";
    } else if (t.moves == nullptr) {
        t.weapon_why = "no physical moves for this weapon";
    } else if (t.style == 2) {
        t.weapon_why = "the left weapon held two-handed";
    } else if (t.style < 0 || t.style > 5) {
        t.weapon_why = "unknown weapon style";
    } else if (M::ChooseMove(*t.moves, t.form_b, M::MoveKind::Light).row == nullptr) {
        t.weapon_why = t.form_b ? "no physical moves in form B" : "no physical moves in form A";
    } else {
        t.weapon_why = nullptr;
    }
}

// The live dmy1 -> dmy2 distance of a row (0: a sphere row; -1: a dummy not found).
float SpanNow(u8* chr, const M::MoveRow& r) {
    if (r.dmy2 < 0) {
        return 0.0f;
    }
    B::Vec3 a{};
    B::Vec3 b{};
    return DummyAt(chr, r.dmy1, a) && DummyAt(chr, r.dmy2, b) ? B::Length(a - b) : -1.0f;
}

// The capsule radius a physical swing gives the row (core/vr/melee_window.h SwingRadius:
// melee_radius_scale, the floor, the blade fit while the part has the model's shape).
float RowRadius(u8* chr, const M::MoveRow& r, bool* fitted = nullptr) {
    const auto& ms = VR::Melee::GetMeleeSettings();
    M::RadiusConfig c{};
    c.scale = ms.radius_scale;
    c.fit = ms.radius_fit;
    c.margin = ms.radius_margin_cm * 0.01f;
    const float span_now = SpanNow(chr, r);
    const float out = M::SwingRadius(r.radius, r.blade, r.span, span_now, c);
    if (fitted != nullptr) {
        M::RadiusConfig nofit = c;
        nofit.fit = false;
        *fitted = out < M::SwingRadius(r.radius, r.blade, r.span, span_now, nofit) - 1e-4f;
    }
    return out;
}

// VR_MELEE_LOG: where the hit capsules sit on the weapon in hand, once per weapon and form (when
// the form has settled): every dummy of the right weapon's parts (groups 10-13) in the grip's
// frame (cm, R_Weapon 300's axes) and its distance from the grip, the hand each part is in (its
// nearest dummy to the right grip 300 or the left 340), the left weapon's distance from the left
// hand (the gun in the hand or holstered), and for each move of this form (and the flick's
// transform attack into it) the capsule its row asks for: dmy1 -> dmy2 in the grip's frame, the
// radius as scaled, how far from the grip it reaches against the part's farthest dummy.
void LogBladeGeometry(u8* chr, const TickState& t) {
    std::array<float, 12> g{};
    if (t.moves == nullptr || !DummyMtx(chr, RightWeaponDummy, g)) {
        return;
    }
    const B::Vec3 grip{g[3], g[7], g[11]};
    B::Vec3 left{};
    const bool have_left = DummyAt(chr, LeftWeaponDummy, left);
    static constexpr std::array<s32, 22> Suffixes = {100, 101, 102, 103, 104, 105, 106, 110,
                                                     111, 112, 113, 120, 121, 122, 123, 130,
                                                     131, 132, 133, 140, 141, 142};
    std::array<float, 4> tip_cm{-1.0f, -1.0f, -1.0f, -1.0f};
    std::array<bool, 4> part_left{};
    std::string parts;
    for (s32 group = 10; group <= 13; ++group) {
        std::string list;
        float near_r = 1e9f;
        float near_l = 1e9f;
        for (const s32 s : Suffixes) {
            const s32 id = group * 1000 + s;
            B::Vec3 p{};
            if (!DummyAt(chr, id, p) || B::Length(p - grip) > GeometryFarM) {
                continue;
            }
            const B::Vec3 l = InFrameCm(g, p);
            const float d = B::Length(p - grip) * 100.0f;
            tip_cm[group - 10] = std::max(tip_cm[group - 10], d);
            near_r = std::min(near_r, d);
            if (have_left) {
                near_l = std::min(near_l, B::Length(p - left) * 100.0f);
            }
            list += fmt::format(" {}:({:.0f},{:.0f},{:.0f}){:.0f}", id, l.x, l.y, l.z, d);
        }
        part_left[group - 10] = have_left && near_l < near_r;
        if (!list.empty()) {
            parts += fmt::format(" | part {} in the {} hand ({:.0f} cm from the right grip, {:.0f} "
                                 "from the left):{}",
                                 group, have_left && near_l < near_r ? "LEFT" : "right", near_r,
                                 have_left ? near_l : -1.0f, list);
        }
    }
    const auto capsule = [&](const char* what, const M::MoveRow& r) {
        B::Vec3 a{};
        B::Vec3 b{};
        const bool ha = DummyAt(chr, r.dmy1, a) && B::Length(a - grip) <= GeometryFarM;
        const bool hb = r.dmy2 >= 0 && DummyAt(chr, r.dmy2, b) && B::Length(b - grip) <= GeometryFarM;
        if (!ha) {
            return fmt::format(" | {} judge {}: dmy {} NOT on a part shown", what, r.judge, r.dmy1);
        }
        bool fitted = false;
        const float rad = RowRadius(chr, r, &fitted);
        const float span_now = SpanNow(chr, r);
        const float end_cm =
            std::max(B::Length(a - grip), hb ? B::Length(b - grip) : 0.0f) * 100.0f;
        const s32 group = r.Group();
        const float tip = group >= 10 && group <= 13 ? tip_cm[group - 10] : -1.0f;
        const bool on_left = group >= 10 && group <= 13 && part_left[group - 10];
        const B::Vec3 la = InFrameCm(g, a);
        const B::Vec3 lb = hb ? InFrameCm(g, b) : la;
        return fmt::format(" | {} judge {}: {}{} {} ({:.0f},{:.0f},{:.0f}) -> ({:.0f},{:.0f},{:.0f}) "
                           "radius {:.0f} cm (row {:.0f}, the blade {:.0f} cm off its axis, span "
                           "{:.0f} cm, the model's {:.0f}{}): reaches {:.0f} cm from the grip, the "
                           "part's farthest dummy {:.0f} cm{}",
                           what, r.judge, r.dmy1, hb ? fmt::format("-{}", r.dmy2) : std::string{},
                           hb ? "capsule" : "sphere", la.x, la.y, la.z, lb.x, lb.y, lb.z,
                           rad * 100.0f, r.radius * 100.0f, r.blade * 100.0f, span_now * 100.0f,
                           r.span * 100.0f, fitted ? ": fitted" : "", end_cm + rad * 100.0f, tip,
                           on_left ? " ON THE LEFT HAND'S PART" : "");
    };
    std::string rows;
    const std::size_t base = t.form_b ? 3 : 0;
    static constexpr std::array<const char*, 3> Kinds = {"light", "heavy", "charged"};
    for (std::size_t k = 0; k < 3; ++k) {
        const M::MoveRow& r = t.moves->rows[base + k];
        rows += r.Valid() ? capsule(Kinds[k], r) : fmt::format(" | {} -", Kinds[k]);
    }
    if (const M::MoveRow* tr = M::FindTransformMove(t.variation, t.form_b); tr != nullptr) {
        rows += capsule(t.form_b ? "transform attack A->B" : "transform attack B->A", *tr);
    }
    std::string gun = " | no left weapon dummy";
    if (B::Vec3 p{}; have_left && DummyAt(chr, LeftPartDummy, p)) {
        gun = fmt::format(" | the left weapon ({}) {:.0f} cm from the left hand", LeftPartDummy,
                          B::Length(p - left) * 100.0f);
    }
    LOG_INFO(Core, "VR melee: blade {} form {} (style {}){}{}{}", t.moves->name,
             t.form_b ? "B" : "A", t.style, rows, gun, parts);
}

// The one refusal a new swing waits out (swing_buffer_ms, M::SwingBuffer): compared by pointer.
constexpr const char* AttackWaitWhy = "the game takes no attack yet";

// Why an attack cannot start (or go on) now; nullptr: it can. `flick_stroke`: the window is the
// stroke a transform flick joined (its form change is ours: the transform attack serves it);
// `known_style`: the weapon's form just changed through our own transform (a known-good switch).
const char* BlockReason(u8* chr, const MeleeBody& body, const TickState& t, bool flick_stroke,
                        bool known_style) {
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
    if (!body.driven) {
        return "not first person";
    }
    if (VR::IsGameMenuOpen()) {
        return "menu";
    }
    if (VR::CinematicActive()) {
        return "cinematic";
    }
    if (body.attack_wait && !body.dodge_attack) {
        // A kept hit or dodge (your arm, the game's clip): no swing before its cancel window
        // (a dodge with dodge_attack takes it at once). A new swing waits swing_buffer_ms for it.
        return AttackWaitWhy;
    }
    if (body.dodge_attack && t.dodge_cancel != 0 && t.latch.active &&
        t.latch.serial == t.dodge_cancel) {
        return "the dodge cancels the swing in progress"; // its follow-through (MeleePoseTick)
    }
    if ((!body.right_ik && !body.dodge_attack) || body.right_weight < RightArmMin) {
        return "the game has the right arm";
    }
    if (body.held && body.owner_id >= 0) {
        const s32 base = body.owner_id % 1'000'000;
        if (base >= 19000 && base <= 19999) {
            return "transforming";
        }
    }
    // The weapon-only transform before its form switch has settled (no player clip to see;
    // transform_flick=0 only: with the flick a swing joins the transform).
    if (const char* transform = TransformSwingBlock(); transform != nullptr) {
        return transform;
    }
    // A flick's stroke goes on into a form without moves of its own (a whip with whip=canned):
    // the transform attack serves the rest of it.
    if (t.weapon_why != nullptr && !(flick_stroke && t.moves != nullptr)) {
        return t.weapon_why;
    }
    if (t.style_stable < 2 && !flick_stroke && !known_style) {
        return "the weapon form is changing";
    }
    // The attack's hand as the game computes it (vt+0x380): -2 would make it the left weapon's.
    const s32 attack_type = Read<s32>(chr + OffsetAttackType);
    const u8* hks = HksState(chr);
    const u16 bits = hks != nullptr ? Read<u16>(hks + OffsetInputBits) : u16{0};
    const bool l2 = (bits & 0x80) == 0 && (bits & 0x40) != 0;
    if (attack_type == 4 || (attack_type == 11 && t.style == 1) || (attack_type == -1 && l2)) {
        return "the attack would be the left hand's (L2 held)";
    }
    if (const u8* data = Module(chr, L::OffsetDataModule);
        data != nullptr && Read<s32>(data + OffsetHp) <= 0) {
        return "dead";
    }
    return nullptr;
}

void PauseRegen(u8* chr) {
    u8* const hks = HksState(chr);
    if (hks != nullptr && !g_dry_run) {
        hks[OffsetRegenPercent] = 0; // as TAE event 225 (the player update resets it)
    }
}

void Refund(u8* chr, s32 amount, u32 serial, const char* why) {
    u8* const data = Module(chr, L::OffsetDataModule);
    if (data == nullptr || amount <= 0 || g_dry_run) {
        return;
    }
    const s32 now = Read<s32>(data + OffsetStamina);
    const s32 max = Read<s32>(data + OffsetStaminaMax);
    const s32 after = std::min(now + amount, max);
    std::memcpy(data + OffsetStamina, &after, sizeof(after));
    g_stats.refunds.fetch_add(1, std::memory_order_relaxed);
    Combat::PushMeleeEvent(
        {Combat::MeleeEventKind::Refunded, serial, static_cast<float>(amount), 0.0f, NowNs(), why});
    LOG_INFO(Core, "VR melee: #{} stamina {} -> {} given back ({})", serial, now, after, why);
}

// What a physical swing does with the game's stamina (melee_stamina_pct, melee_regen_pause).
M::SwingStamina SwingRule(M::MoveKind kind = M::MoveKind::Light) {
    const auto& ms = VR::Melee::GetMeleeSettings();
    return M::AttackStaminaOf(kind, ms.stamina_pct, ms.regen_pause);
}

// " (row 30, 10% of 30)" / " (row 30)" / " (row 30, free: melee_stamina_pct=0)" for the open line.
std::string StaminaText(const M::SwingStamina& rule, s32 row_cost, s32 full) {
    if (!rule.charge) {
        return fmt::format(" (row {}, free: melee_stamina_pct=0)", row_cost);
    }
    if (rule.pct >= 100) {
        return fmt::format(" (row {})", row_cost);
    }
    return fmt::format(" (row {}, {}% of {})", row_cost, rule.pct, full);
}

// InvokeAttack on our slot, paying melee_stamina_pct of the game's cost. A partial swing (1 .. 99)
// lets CreateAttack take the game's full cost and gives all but that share back in the same call
// (nothing reads the stamina in between: the gauge only shows the share's dip); `row_cost` is the
// full cost when the game's -50 floor clipped its drop. A free swing (0) holds the game's
// no-stamina-consumption flag for the call, so CreateAttack takes nothing; whatever it still took
// goes back at once (the gauge never dips). `full`: the game's full cost of this call (0 when the
// attack existed already, or free).
u8 Invoke(u8* chr, u8* module, s32 bid, u32 slot, s32 row_cost, const M::SwingStamina& rule,
          s32* full = nullptr) {
    u8* const data = Module(chr, L::OffsetDataModule);
    if (data == nullptr) {
        return g_invoke(module, bid, 0, slot, 1);
    }
    if (rule.charge) {
        const s32 before = Read<s32>(data + OffsetStamina);
        const u8 ok = g_invoke(module, bid, 0, slot, 1);
        const s32 after = Read<s32>(data + OffsetStamina);
        const M::SwingPaid paid = M::PartialSwing(before, after, rule.pct, row_cost);
        if (paid.stamina != after) {
            std::memcpy(data + OffsetStamina, &paid.stamina, sizeof(paid.stamina));
        }
        if (full != nullptr) {
            *full = paid.full;
        }
        return ok;
    }
    const s32 before = Read<s32>(data + OffsetStamina);
    const u8 flags = Read<u8>(data + OffsetDataFlags);
    if (g_free_flag) {
        data[OffsetDataFlags] = M::FreeSwingFlags(flags);
    }
    const u8 ok = g_invoke(module, bid, 0, slot, 1);
    if (g_free_flag) {
        data[OffsetDataFlags] = M::RestoredFlags(Read<u8>(data + OffsetDataFlags), flags);
    }
    const s32 after = Read<s32>(data + OffsetStamina);
    if (const s32 keep = M::FreeSwingStamina(before, after); keep != after) {
        std::memcpy(data + OffsetStamina, &keep, sizeof(keep));
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            LOG_WARNING(Core,
                        "VR melee: a free swing still took stamina {} -> {}: put back at once{}",
                        before, after, g_free_flag ? "" : " (the flag's bytes did not match)");
        }
    }
    return ok;
}

// Whirligig grinding: the native 931/932 rows (a148_52910), without L2, HKS input,
// or a player animation. Alternate the reserved right-hand slots at the original hit cadence;
// the engine retires an attack as soon as we stop refreshing it, just as for physical swings.
constexpr M::MoveRow SawRows[] = {
    {931, 10, 60, 12100, -1, 0.5f, 0.09f, false, -1, -1, -1, -1, 0.6f, 0.0f},
    {932, 10, 60, 12100, -1, 0.5f, 0.09f, false, -1, -1, -1, -1, 0.6f, 0.0f},
};
struct SawAttackState {
    u8* chr{nullptr};
    VR::Tricks::SawAttackClock clock{};
    Attack attack{};
    bool live{false};
    bool exhausted{false};
    u32 pulse{0};
};
SawAttackState g_saw{};

void EndSawAttack(s64 now) {
    if (!g_saw.live) {
        return;
    }
    std::scoped_lock lock{g_attack_mutex};
    for (Attack& a : g_attacks) {
        if (a.saw && a.serial == g_saw.attack.serial && a.created_ns == g_saw.attack.created_ns) {
            a.ended_ns = now;
        }
    }
    g_saw.live = false;
}

bool SawAttackTick(u8* chr, const MeleeBody& body, const TickState& t,
                   const char* gate, float dt, s64 now) {
    namespace Tr = VR::Tricks;
    if (g_saw.chr != chr) {
        EndSawAttack(now);
        g_saw = {};
        g_saw.chr = chr;
    }
    Combat::SawRequest request{};
    const bool fresh = Combat::LatestSawRequest(request) && Tr::SawFresh(request.ns, now);
    const bool owns = fresh && Tr::GetTrickSettings().saw_spin && g_physical &&
                      t.variation == 3100 && t.form_b && request.rev > 0.0f;
    const bool allowed = owns && gate == nullptr && body.body && !TransformRun().active;
    if (!fresh || !request.held) {
        g_saw.exhausted = false; // empty stamina requires releasing the trigger before restarting
    }
    if (!allowed) {
        EndSawAttack(now);
        g_saw.clock = {};
        SetSawWeaponPose(chr, 0.0f, false);
        return owns;
    }
    const bool pulse = g_saw.clock.Step(request.rev, request.held && !g_saw.exhausted, dt);
    if (!SetSawWeaponPose(chr, g_saw.clock.Time(), !g_saw.exhausted)) {
        EndSawAttack(now);
        return owns;
    }
    if (!g_saw.clock.Active()) {
        EndSawAttack(now);
        return owns;
    }
    u8* const module = Module(chr, OffsetAttackModule);
    u8* const data = Module(chr, L::OffsetDataModule);
    B::Vec3 blade{}, grip{};
    if (module == nullptr || data == nullptr || !DummyAt(chr, 12100, blade) ||
        !DummyAt(chr, RightWeaponDummy, grip) || B::Length(blade - grip) > DummyNearM) {
        EndSawAttack(now);
        SetSawWeaponPose(chr, 0.0f, false);
        return owns;
    }
    if (pulse) {
        EndSawAttack(now);
        if (Read<s32>(data + OffsetStamina) <= 0) {
            g_saw.exhausted = true;
            SetSawWeaponPose(chr, 0.0f, false);
            LOG_INFO(Core, "VR saw: stopped (no stamina; release the trigger to restart)");
            return owns;
        }
        const M::MoveRow& row = SawRows[g_saw.pulse++ & 1];
        const s32 bid = BehaviorId(chr, row.judge);
        if (bid < 0 || GunBehaviorRefType(bid) != 0) {
            g_saw.exhausted = true;
            SetSawWeaponPose(chr, 0.0f, false);
            LOG_WARNING(Core, "VR saw: native grinding row {} unavailable", row.judge);
            return owns;
        }
        const u32 serial = 0x80000000u | g_saw.pulse;
        const u32 slot = FirstSlot + (serial & 1);
        const u8 ok = g_dry_run ? 0 : Invoke(chr, module, bid, slot, row.stamina, M::SwingStamina{});
        if (ok == 0) {
            g_saw.exhausted = true;
            return owns;
        }
        Attack a{};
        a.serial = serial;
        a.bid = bid;
        a.handle = Read<u32>(module + OffsetAttackSlots + slot * 8);
        a.row = &row;
        a.saw = true;
        a.created_ns = now;
        a.since_counted_s = Tr::SawHitS;
        g_saw.attack = a;
        g_saw.live = true;
        {
            std::scoped_lock lock{g_attack_mutex};
            g_attacks[g_attack_next++ % g_attacks.size()] = a;
        }
        LOG_DEBUG(Core, "VR saw: grinding judge {} bid {} handle {:#x}", row.judge, bid, a.handle);
    } else if (g_saw.live && !g_dry_run) {
        const Attack& a = g_saw.attack;
        const u32 slot = FirstSlot + (a.serial & 1);
        // Never let an unexpectedly retired hitbox recreate itself and charge stamina each frame.
        if (Read<u32>(module + OffsetAttackSlots + slot * 8) != a.handle ||
            HitEntry(a.handle, a.bid) == nullptr) {
            EndSawAttack(now);
        } else {
            Invoke(chr, module, a.bid, slot, a.row->stamina, M::SwingStamina{});
        }
    }
    if (g_saw.live && g_regen) {
        PauseRegen(chr);
    }
    return owns;
}

// The window's attack is over (ended, replaced, or the window gone): stamina for an onset window
// that never committed and hit nothing, or the difference of a heavy window judged light that hit
// nothing, goes back; `replaced` (a flick: the stroke goes on as the transform attack, which takes
// its own stamina): all of it when it hit nothing. Returns the finalized attack.
Attack Finalize(u8* chr, TickState& t, const Combat::MeleeWindow* w, const char* why, s64 now,
                bool replaced = false) {
    if (!t.cur_live) {
        return {};
    }
    t.cur_live = false;
    Attack a{};
    {
        std::scoped_lock lock{g_attack_mutex};
        for (Attack& r : g_attacks) {
            if (r.serial == t.cur.serial && r.bid == t.cur.bid && r.created_ns == t.cur.created_ns) {
                r.ended_ns = now;
                a = r;
            }
        }
    }
    const u8* data = Module(chr, L::OffsetDataModule);
    const bool aborted = w != nullptr && ((w->serial == a.serial && w->aborted) ||
                                          (w->prev_serial == a.serial && w->prev_aborted));
    if (aborted) {
        g_stats.aborted.fetch_add(1, std::memory_order_relaxed);
    }
    if (a.hits == 0 && replaced) {
        Refund(chr, a.cost, a.serial, "the stroke became the transform attack, nothing hit yet");
    } else if (a.hits == 0 && aborted) {
        Refund(chr, a.cost, a.serial, "the swing never committed and hit nothing");
    } else if (a.hits == 0 && a.verdict_light && a.light != nullptr && a.row != nullptr &&
               a.row->stamina > 0) {
        // The heavy row's cost minus the light row's (the cost the game took, scaled).
        Refund(chr, a.cost - M::PartialCost(a.cost * a.light->stamina / a.row->stamina,
                                         a.light_stamina_pct), a.serial,
               "a wound-up swing judged light, nothing hit");
    }
    LOG_INFO(Core,
             "VR melee: #{} {}{} ended after {:.0f} ms, {} frames ({}): {} hits {:.1f} damage, {} "
             "wall{}, stamina now {}",
             a.serial, a.transform ? "TRANSFORM ATTACK " : "", M::MoveKindName(a.kind),
             static_cast<double>(now - a.created_ns) * 1e-6, t.cur_frames, why, a.hits, a.damage,
             a.walls, aborted ? ", aborted (no commit)" : "",
             data != nullptr ? Read<s32>(data + OffsetStamina) : -1);
    return a;
}

// The weapon's own swing sound and trail of `row` (its clip's), else the Saw Cleaver's.
void SetSwingFx(TickState& t, const M::MoveRow& r) {
    t.whoosh = r.whoosh > 0 ? r.whoosh : t.form_b ? 611 : 610;
    if (r.trail_ffx > 0 && r.trail_base > 0 && r.trail_tip > 0) {
        t.trail_ffx = r.trail_ffx;
        t.trail_base = r.trail_base;
        t.trail_tip = r.trail_tip;
    } else {
        const s32 group = r.Group();
        t.trail_ffx = TrailFfx;
        t.trail_base = group * 1000 + 100;
        t.trail_tip = r.dmy2 >= 0 ? r.dmy2 : group * 1000 + 110;
    }
    t.trail_checked = false;
}

// " budget 1/2" for the open line (the presenter's combo budget, as published with the window).
std::string BudgetText(const Combat::MeleeWindow& w, bool flick) {
    if (w.budget_cap == 0) {
        return {};
    }
    if (flick) {
        return fmt::format(" budget {}/{} (a flick: free)", w.budget_left, w.budget_cap);
    }
    return fmt::format(" budget {}/{} (used {}{}, back in {:.2f} s{})", w.budget_left,
                       w.budget_cap, w.budget_used,
                       w.budget_downgraded ? " as a light, wound up with one left" : "",
                       w.budget_refill_s, w.budget_late ? ", paid at the commit" : "");
}

// Opens the window's attack on `row` (core/vr/melee_moves.h): InvokeAttack on our slot, the
// game's own hit list and capsule, melee_stamina_pct of its stamina (50% by default: Invoke);
// then the swing sound, trail and (a swing that cost the game's full stamina) the regen pause.
// `transform`: the game's transform attack of a flick (FlickOpen), `since_s` its cadence, `near_m`
// how near the grip the row's hit dummy must be (the part in the hand). `soft`: a refusal is only
// logged and the window stays (a flick's stroke, whose transform attack still comes). True when
// the attack was made.
bool OpenAttack(u8* chr, TickState& t, const Combat::MeleeWindow& w, float arm, s64 now,
                const M::MoveRow* row, M::MoveKind kind, M::MoveKind want, bool lighter,
                const M::MoveRow* light, bool transform, float since_s,
                float near_m = DummyNearM, bool soft = false) {
    u8* const module = Module(chr, OffsetAttackModule);
    u8* const data = Module(chr, L::OffsetDataModule);
    const auto affordable = M::AffordableMove({row, kind, lighter}, light ? light :
        (t.moves ? M::ChooseMove(*t.moves, t.form_b, M::MoveKind::Light).row : nullptr),
        data ? Read<s32>(data + OffsetStamina) : 0);
    if (affordable.kind != kind) {
        LOG_INFO(Core, "VR melee: #{} {} -> light (stamina {} < strong cost {})",
                 w.serial, M::MoveKindName(kind), data ? Read<s32>(data + OffsetStamina) : 0,
                 row ? row->stamina : 0);
        row = affordable.row;
        kind = affordable.kind;
        lighter = true;
        transform = false;
    }
    const s32 judge = row != nullptr ? row->judge : -1;
    const s32 bid = judge >= 0 ? BehaviorId(chr, judge) : -1;
    const u32 slot = FirstSlot + (w.serial & 1);
    B::Vec3 dmy{}, grip{};
    const bool have_dmy = row != nullptr && DummyAt(chr, row->dmy1, dmy);
    const bool have_grip = DummyAt(chr, RightWeaponDummy, grip);
    const M::SwingStamina stamina_rule = SwingRule(kind);
    const char* refuse = nullptr;
    if (module == nullptr || data == nullptr) {
        refuse = "no behaviour module";
    } else if (row == nullptr || bid < 0) {
        refuse = "no row";
    } else if (!have_dmy) {
        refuse = "the hit dummy is not on the weapon in hand";
    } else if (have_grip && B::Length(dmy - grip) > near_m) {
        refuse = "the hit dummy is not near the hand";
    } else if (M::StaminaRefuses(stamina_rule, Read<s32>(data + OffsetStamina))) {
        refuse = "no stamina";
    }
    if (refuse != nullptr && soft) {
        LOG_INFO(Core,
                 "VR melee: #{} {} not opened in the old form: {} (variation {} style {} judge {}); "
                 "the transform attack serves the stroke",
                 w.serial, M::MoveKindName(want), refuse, t.variation, t.style, judge);
        return false;
    }
    if (refuse != nullptr) {
        g_stats.denied.fetch_add(1, std::memory_order_relaxed);
        t.latch.active = false;
        t.latch.done = true;
        Combat::PushMeleeEvent({Combat::MeleeEventKind::Denied, w.serial, 0.0f, 0.0f, now, refuse});
        LOG_INFO(Core, "VR melee: #{} {}{} refused: {} (variation {} style {} judge {} bid {})",
                 w.serial, transform ? "TRANSFORM ATTACK " : "", M::MoveKindName(want), refuse,
                 t.variation, t.style, judge, bid);
        return false;
    }
    const s32 stamina_before = Read<s32>(data + OffsetStamina);
    u8 ok = 0;
    s32 full_cost = 0;
    if (!g_dry_run) {
        ok = Invoke(chr, module, bid, slot, row->stamina, stamina_rule, &full_cost);
    }
    const s32 stamina_after = Read<s32>(data + OffsetStamina);
    const u32 handle = Read<u32>(module + OffsetAttackSlots + slot * 8);
    u8* const entry = ok != 0 ? HitEntry(handle, bid) : nullptr;
    const bool stake_native = t.variation == StakeDriverVariation && t.form_b &&
                              kind == M::MoveKind::Charged && judge == 930;
    if (stake_native && entry != nullptr && g_stake_geometry) {
        if (!VR::Tricks::RelocateStakeBlast({entry, EntrySize})) {
            LOG_WARNING(Core, "VR stake: charged attack geometry differs from retail 1.09");
        }
    }
    float radius_before = -1.0f;
    float radius_after = -1.0f;
    if (entry != nullptr) {
        radius_before = Read<float>(entry + OffsetEntryRadius);
        // Only the radius the row asked for (a sanity check of the offset), never twice.
        if (!stake_native && std::abs(radius_before - row->radius) < 0.02f) {
            float want = RowRadius(chr, *row);
            // A whip form's capsules on the chain: wider (x1.6, at least 25 cm) so a lash that
            // lands near an enemy, or slaps the ground in front of one, catches it.
            if (IsWhipRow(row)) {
                want = std::max(want * 1.6f, 0.25f);
            }
            // The Beast Cutter, both forms, wider (2026-10-16, the tester: its swings sometimes
            // missed): form A 21 -> 22 cm; the whip's light (its 35 cm row) 39 -> 45, heavy,
            // charged and the transform attack 25 -> 31.
            if (t.variation == BeastCutterVariation) {
                want = !IsWhipRow(row) ? 0.22f : row->radius >= 0.3f ? 0.45f : 0.31f;
            }
            if (std::abs(want - radius_before) > 0.002f) {
                radius_after = want;
                std::memcpy(entry + OffsetEntryRadius, &radius_after, sizeof(radius_after));
            }
        }
    }
    (ok != 0 ? g_stats.created : g_stats.failed).fetch_add(1, std::memory_order_relaxed);
    if (ok != 0) {
        DodgeAttackCut(t, w.serial, "opened");
    }
    Attack a{};
    a.weapon_variation = t.variation;
    a.form_b = t.form_b;
    a.serial = w.serial;
    a.bid = bid;
    a.handle = handle;
    a.kind = kind;
    a.row = row;
    a.light = light;
    a.transform = transform;
    a.powered = w.powered && !transform && t.variation == StakeDriverVariation &&
                t.form_b && kind == M::MoveKind::Charged;
    a.cost = std::max(stamina_before - stamina_after, 0);
    a.stamina_rule = stamina_rule;
    a.light_stamina_pct = VR::Melee::GetMeleeSettings().stamina_pct;
    a.since_counted_s = since_s;
    a.created_ns = now;
    // A whip form's own move (whip=physical: its rows ride the chain): paced by the game's
    // recovery of that move (MeleeDamage, whip_pace).
    a.whip_variation = !transform && t.form_b && IsWhipRow(row) &&
                               VR::Whip::WhipRecover(t.variation, static_cast<std::size_t>(kind)) > 0.0f
                           ? t.variation
                           : -1;
    a.two_hand = std::clamp(w.two_hand, 1.0f, 2.0f);
    a.verdict_light =
        !transform && w.verdict_known && w.verdict == 0 && kind != M::MoveKind::Light;
    // Opened while the hunter falls (or just landed from a deep fall): a plunge, held to the
    // landing (MeleePoseTick).
    const auto plunge_cfg = PlungeConfig();
    a.plunge = !transform && !a.powered && ok != 0 && t.air.Opens(now, plunge_cfg);
    a.drop_m = a.plunge ? t.air.Drop(now, plunge_cfg) : 0.0f;
    if (a.plunge) {
        LOG_INFO(Core,
                 "VR melee: #{} PLUNGE: opened in the air ({:.2f} m fallen, {:.1f} m/s down): "
                 "its attack holds until the landing",
                 w.serial, a.drop_m, -t.air.Current().vy);
    }
    {
        std::scoped_lock lock{g_attack_mutex};
        g_attacks[g_attack_next++ % g_attacks.size()] = a;
    }
    t.cur = a;
    t.cur_live = ok != 0 || g_dry_run;
    t.unpaid_live = false;
    t.cur_frames = 1;
    t.whoosh_done = false;
    t.last_a = {};
    SetSwingFx(t, *row);
    if (stamina_rule.regen_pause && g_regen) {
        t.regen_left = M::RecoverSeconds(kind); // full-cost light or strong attack
    }
    if (ok == 0 && !g_dry_run) {
        t.latch.active = false;
        t.latch.done = true;
    }
    Combat::PushMeleeEvent(
        {ok != 0 || g_dry_run ? Combat::MeleeEventKind::Opened : Combat::MeleeEventKind::Denied,
         w.serial, static_cast<float>(a.cost), static_cast<float>(static_cast<u8>(kind)), now,
         ok != 0 || g_dry_run ? "" : "the game made no attack"});
    if (a.powered && ok != 0) {
        StakeDischarge(chr, now);
        Combat::PushMeleeEvent({Combat::MeleeEventKind::Shot, w.serial,
                               static_cast<float>(VR::Tricks::Shot::StakeBlast), 1.0f,
                               now, "charged melee discharge"});
    }
    LOG_INFO(Core,
             "VR melee: #{} {} {}{}{} open ({}, window {:.0f} ms old) bid={} judge={} slot {} "
             "ok={} handle={:#x} stamina {}->{}{} mv {} radius {:.2f}->{:.2f} variation "
             "{} {} form {} style {} armR={:.2f} tip {:.1f} m/s since {:.2f} s dmy {} {:.0f} mm "
             "from the grip{}{}",
             w.serial, t.moves != nullptr ? t.moves->name : "?",
             transform ? "TRANSFORM ATTACK " : "", M::MoveKindName(kind),
             lighter ? fmt::format(" (asked {})", M::MoveKindName(want)) : std::string{},
             w.committed ? "committed" : "onset", static_cast<double>(now - w.open_ns) * 1e-6, bid,
             judge, slot, ok, handle, stamina_before, stamina_after,
             StaminaText(stamina_rule, row->stamina, full_cost), row->mv,
             radius_before, radius_after >= 0.0f ? radius_after : radius_before, t.variation,
             w.committed ? "" : "(onset)", t.form_b ? "B" : "A", t.style, arm, w.tip_speed,
             since_s, row->dmy1, have_grip ? B::Length(dmy - grip) * 1000.0f : -1.0f,
             BudgetText(w, transform || t.flick.window == w.serial),
             g_dry_run ? " DRY RUN (no attack)" : "");
    return ok != 0 || g_dry_run;
}

// A window the combo budget could not pay: no attack (no stamina, no hit list), its whoosh at the
// commit and its trail while it is open (Refresh), so the swing still looks and sounds like one.
void UnpaidOpen(TickState& t, const Combat::MeleeWindow& w, const M::MoveRow* row,
                M::MoveKind kind) {
    g_stats.unpaid.fetch_add(1, std::memory_order_relaxed);
    t.cur = {};
    t.cur.serial = w.serial;
    t.cur.row = row;
    t.cur.kind = kind;
    t.cur_frames = 0;
    t.unpaid_live = row != nullptr;
    t.whoosh_done = false;
    t.last_a = {};
    if (row != nullptr) {
        SetSwingFx(t, *row);
    }
    LOG_INFO(Core,
             "VR melee: #{} {} {} NOT REGISTERED ({}, window {:.0f} ms old): combo budget {}/{}: "
             "no attack, its whoosh and trail only",
             w.serial, t.moves != nullptr ? t.moves->name : "?", M::MoveKindName(kind),
             w.committed ? "committed" : "onset",
             static_cast<double>(NowNs() - w.open_ns) * 1e-6, w.budget_left, w.budget_cap);
}

// The window's own attack (its form's light / heavy / charged row). `flick`: the stroke a transform
// flick joined, before its switch: the old form's attack while the clip may already move the part
// (the Hunter Axe's head 1.69 m out), and no refusal (the transform attack serves the stroke).
void Create(u8* chr, TickState& t, const Combat::MeleeWindow& w, float arm, s64 now,
            bool flick = false) {
    if (t.moves == nullptr || (flick && t.weapon_why != nullptr)) {
        if (flick) {
            t.flick.phase = FlickPhase::Gap; // no old-form swing: straight to the transform attack
            return;
        }
        t.latch.active = false; // (the gates refuse a weapon without moves first)
        t.latch.done = true;
        return;
    }
    const auto want = static_cast<M::MoveKind>(std::min<u8>(w.kind, 2));
    const M::MoveChoice choice = M::ChooseMove(*t.moves, t.form_b, want);
    const M::MoveRow* light = M::ChooseMove(*t.moves, t.form_b, M::MoveKind::Light).row;
    if (w.unpaid) {
        // The combo budget had no charge for this swing: it does not register. A flick's stroke
        // goes straight to its transform attack (transforms never need charges).
        if (flick) {
            t.flick.phase = FlickPhase::Gap;
            LOG_INFO(Core,
                     "VR melee: #{} {} not registered (combo budget {}/{}): the flick's "
                     "transform attack serves the stroke",
                     w.serial, M::MoveKindName(want), w.budget_left, w.budget_cap);
            return;
        }
        UnpaidOpen(t, w, choice.row, choice.kind);
        return;
    }
    const bool made =
        OpenAttack(chr, t, w, arm, now, choice.row, choice.kind, want, choice.lighter, light, false,
                   w.since_counted_s,
                   IsWhipRow(choice.row) ? WhipNearM
                   : flick               ? FlickNearM
                                         : DummyNearM,
                   flick);
    if (flick && !made && t.latch.active) {
        t.flick.phase = FlickPhase::Gap; // the transform attack serves the whole stroke
    }
}

// ---- A flick mid-swing (core/vr/melee_window.h FlickJoins) --------------------------------------

constexpr u32 FlickPartWaitUpdates = 8; // the new form's hit dummy must reach the hand by then

const char* FlickPhaseName(FlickPhase p) {
    switch (p) {
    case FlickPhase::Old:
        return "old";
    case FlickPhase::Gap:
        return "gap";
    case FlickPhase::Live:
        return "live";
    case FlickPhase::Done:
        return "done";
    }
    return "?";
}

void FlickDone(TickState& t, const char* why) {
    FlickStroke& f = t.flick;
    if (f.phase == FlickPhase::Done) {
        return;
    }
    LOG_INFO(Core, "VR melee: #{} flick of transform #{}: no transform attack ({}; was {})",
             f.window, f.serial, why, FlickPhaseName(f.phase));
    f.phase = FlickPhase::Done;
    if (t.latch.serial == f.window && t.latch.active && !t.cur_live) {
        t.latch.active = false; // nothing alive for this window any more
        t.latch.done = true;
    }
}

// Every update, before the latch: a window joining the newest transform run, and the old form's
// attack ended as the switch is handed to the game (before the old part's dummies leave the blade:
// a capsule following a hidden part would sweep across the map).
void FlickTick(u8* chr, TickState& t, const Combat::MeleeWindow& w, bool have,
               const TransformRunInfo& run, s64 now) {
    FlickStroke& f = t.flick;
    const auto& ts = VR::Transform::GetTransformSettings();
    if (have && !w.powered && w.serial != 0 && w.serial != f.window && run.id != 0 && run.id != f.run &&
        !run.no_join && !(t.latch.serial == w.serial && t.latch.done)) {
        M::FlickJoinInput in{};
        in.enabled = g_physical && ts.flick;
        in.window = w.serial;
        in.open = w.open;
        in.open_ns = w.open_ns;
        in.close_ns = w.close_ns;
        in.run = run.id;
        in.run_active = run.active;
        in.switched = run.switched;
        in.press_ns = run.start_ns;
        in.now = now;
        if (M::FlickJoins(in)) {
            f = {};
            f.window = w.serial;
            f.run = run.id;
            f.serial = run.serial;
            f.to_b = run.to_b;
            f.style = run.style;
            f.phase = FlickPhase::Old;
            if (!run.switched && !run.flick) {
                TransformMakeFlick(run.id, w.serial); // the rest of the clip at the flick's pace
            }
            const M::MoveRow* row =
                ts.attack && t.variation >= 0 ? M::FindTransformMove(t.variation, run.to_b) : nullptr;
            LOG_INFO(Core,
                     "VR melee: #{} the stroke joins transform #{} ({}, {}; window {} {:.0f} ms "
                     "{} the squeeze): {}",
                     w.serial, run.serial, run.to_b ? "A->B" : "B->A",
                     run.switched ? "switched" : run.switch_asked ? "switching" : "before its switch",
                     w.open ? "open" : "closed",
                     std::abs(static_cast<double>(w.open_ns - run.start_ns)) * 1e-6,
                     w.open_ns <= run.start_ns ? "before" : "after",
                     row != nullptr ? fmt::format("the transform attack (judge {}, mv {}) after the "
                                                  "switch",
                                                  row->judge, row->mv)
                                    : std::string{ts.attack ? "no transform attack for this weapon "
                                                              "and direction: the new form's swing"
                                                            : "transform_attack=0: the new form's "
                                                              "swing"});
        }
    }
    if (f.phase == FlickPhase::Done || f.window == 0) {
        return;
    }
    if (!have || w.serial != f.window) {
        f.phase = FlickPhase::Done; // a newer window: this stroke is over
        return;
    }
    if (t.latch.serial == f.window && t.latch.done && !t.cur_live) {
        if (f.phase != FlickPhase::Live) {
            LOG_INFO(Core,
                     "VR melee: #{} flick of transform #{}: no transform attack (the stroke ended "
                     "{}; was {})",
                     f.window, f.serial,
                     run.switched ? "before the new form was in the hand" : "before the switch",
                     FlickPhaseName(f.phase));
        }
        f.phase = FlickPhase::Done; // the window is over (ended, refused)
        return;
    }
    if (run.id != f.run) {
        FlickDone(t, "a newer transform");
        return;
    }
    if (f.phase == FlickPhase::Old) {
        if (run.aborted) {
            f.phase = FlickPhase::Done; // stopped before its switch: the old form goes on
            LOG_INFO(Core, "VR melee: #{} transform #{} stopped before its switch: the stroke "
                           "goes on in the old form",
                     f.window, f.serial);
            return;
        }
        if (run.switch_asked || run.switched) {
            if (t.cur_live && t.cur.serial == f.window) {
                const Attack old = Finalize(chr, t, &w, "the form switches: the transform attack "
                                                        "takes over",
                                            now, true);
                f.old_hits = old.hits;
                f.old_created_ns = old.created_ns;
            }
            f.phase = FlickPhase::Gap;
            f.gap_ns = now;
        }
    }
    if (f.phase == FlickPhase::Gap && run.aborted) {
        FlickDone(t, "the transform stopped before its switch");
    }
}

// The stroke's attack after the switch (the latch's create or refresh while the hand-off is under
// way): the transform attack, or the new form's own row, once the new form's part is in the hand.
void FlickOpen(u8* chr, TickState& t, const Combat::MeleeWindow& w, float arm, s64 now) {
    FlickStroke& f = t.flick;
    const TransformRunInfo& run = TransformRun();
    if (run.id != f.run || run.aborted) {
        FlickDone(t, "the transform stopped before its switch");
        return;
    }
    if (!run.switched || t.style != f.style) {
        return; // the switch is not done yet
    }
    if (!w.open) {
        FlickDone(t, "the window closed before the new form was in the hand");
        return;
    }
    const auto want = static_cast<M::MoveKind>(std::min<u8>(w.kind, 2));
    const bool attack = VR::Transform::GetTransformSettings().attack;
    const M::MoveRow* row =
        attack && t.variation >= 0 ? M::FindTransformMove(t.variation, f.to_b) : nullptr;
    const bool transform = row != nullptr;
    M::MoveKind kind = want;
    bool lighter = false;
    const M::MoveRow* light = nullptr;
    if (row == nullptr && t.moves != nullptr) {
        const M::MoveChoice choice = M::ChooseMove(*t.moves, t.form_b, want);
        row = choice.row;
        kind = choice.kind;
        lighter = choice.lighter;
        light = M::ChooseMove(*t.moves, t.form_b, M::MoveKind::Light).row;
    }
    if (row == nullptr) {
        FlickDone(t, "the new form has no physical move");
        return;
    }
    // The new form's part must be in the hand (the game shows it the update after the switch). A
    // whip's: its handle (the transform attack's tip dummy rides the chain, paying out with the
    // flick: core/vr/bloodborne_whip.h).
    const bool whip = IsWhipRow(row) && row->Group() == WhipHandleDummy / 1000;
    B::Vec3 dmy{}, grip{};
    if (!DummyAt(chr, whip ? WhipHandleDummy : row->dmy1, dmy) ||
        !DummyAt(chr, RightWeaponDummy, grip) || B::Length(dmy - grip) > FlickNearM) {
        if (++f.waited > FlickPartWaitUpdates) {
            FlickDone(t, "the new form's hit dummy never came to the hand");
        }
        return;
    }
    // Its cadence: the stroke's own, but a second hit of the same stroke (the old form's attack
    // already hit) counts from that attack (the game's own R1 -> trick attack takes 0.57 s).
    const float since =
        f.old_hits > 0 ? static_cast<float>(now - f.old_created_ns) * 1e-9f : w.since_counted_s;
    OpenAttack(chr, t, w, arm, now, row, kind, want, lighter, light, transform, since,
               whip ? WhipNearM : FlickNearM);
    if (t.cur_live) {
        f.phase = FlickPhase::Live;
        t.cur.flick = true;
        {
            std::scoped_lock lock{g_attack_mutex};
            for (Attack& r : g_attacks) {
                if (r.serial == t.cur.serial && r.created_ns == t.cur.created_ns) {
                    r.flick = true;
                }
            }
        }
        if (w.budget_used > 0 && f.old_hits == 0) {
            // The combo budget: the stroke is the transform's now (charges back).
            Combat::PushMeleeEvent({Combat::MeleeEventKind::BudgetRefund, w.serial,
                                    static_cast<float>(w.budget_used), 0.0f, now,
                                    "a flick before its own attack hit anything"});
        }
        LOG_INFO(Core,
                 "VR melee: #{} flick of transform #{}: {} {:.0f} ms after the squeeze, {:.0f} ms "
                 "after the switch ({} updates waited for the new part)",
                 f.window, f.serial,
                 transform ? fmt::format("the TRANSFORM ATTACK (judge {})", row->judge)
                           : fmt::format("the new form's {} row (judge {})",
                                         M::MoveKindName(kind), row->judge),
                 static_cast<double>(now - run.start_ns) * 1e-6,
                 static_cast<double>(now - run.switched_ns) * 1e-6, f.waited);
    } else {
        f.phase = FlickPhase::Done;
    }
}

void Refresh(u8* chr, TickState& t, const Combat::MeleeWindow& w, s64 now) {
    u8* const module = Module(chr, OffsetAttackModule);
    if (module == nullptr || (!t.cur_live && !t.unpaid_live) || t.cur.row == nullptr) {
        return;
    }
    const bool unpaid = !t.cur_live; // the combo budget's: no attack to keep alive
    const u32 slot = FirstSlot + (t.cur.serial & 1);
    u8 ok = 1;
    if (!g_dry_run && !unpaid) {
        // (A new attack here would cost the swing's share again: it must not happen, logged.)
        ok = Invoke(chr, module, t.cur.bid, slot, t.cur.row->stamina, t.cur.stamina_rule);
        const u32 handle = Read<u32>(module + OffsetAttackSlots + slot * 8);
        if (handle != t.cur.handle) {
            // The game ended it and made a new one (it must not: a second hit list, stamina).
            LOG_WARNING(Core, "VR melee: #{} the attack's handle changed {:#x} -> {:#x}",
                        t.cur.serial, t.cur.handle, handle);
            std::scoped_lock lock{g_attack_mutex};
            for (Attack& r : g_attacks) {
                if (r.serial == t.cur.serial && r.created_ns == t.cur.created_ns) {
                    r.handle = handle;
                }
            }
            t.cur.handle = handle;
        }
    }
    ++t.cur_frames;
    (void)ok;
    // The classifier's verdict on a heavy window (a wound-up swing judged light), and the blade's
    // speed for the damage hook.
    const bool judged_light = w.serial == t.cur.serial && w.verdict_known && w.verdict == 0 &&
                              t.cur.kind != M::MoveKind::Light && !t.cur.transform;
    t.cur.verdict_light = t.cur.verdict_light || judged_light;
    if (!unpaid) {
        std::scoped_lock lock{g_attack_mutex};
        for (Attack& r : g_attacks) {
            if (r.serial == t.cur.serial && r.created_ns == t.cur.created_ns) {
                r.verdict_light = r.verdict_light || judged_light;
                if (w.serial == t.cur.serial) {
                    r.tip = w.tip_speed;
                    // Two-handed (both hands moved): known once the left hand has moved.
                    r.two_hand = std::max(r.two_hand, std::clamp(w.two_hand, 1.0f, 2.0f));
                }
                if (r.plunge) {
                    r.drop_m = std::max(r.drop_m, t.air.Drop(now, PlungeConfig()));
                    t.cur.drop_m = r.drop_m;
                }
            }
        }
    }
    const auto& ms = VR::Melee::GetMeleeSettings();
    B::Vec3 base{}, tip{};
    bool have_base = DummyAt(chr, t.trail_base, base);
    bool have_tip = DummyAt(chr, t.trail_tip, tip);
    if (!t.trail_checked) {
        t.trail_checked = true;
        B::Vec3 grip{};
        const bool have_grip = DummyAt(chr, RightWeaponDummy, grip);
        const float near_m = IsWhipRow(t.cur.row) ? WhipNearM : DummyNearM;
        const auto on_part = [&](bool have, B::Vec3 p) {
            return have && (!have_grip || B::Length(p - grip) <= near_m);
        };
        const bool base_ok = on_part(have_base, base);
        const bool tip_ok = on_part(have_tip, tip);
        if (!base_ok || !tip_ok) {
            // A dummy of the clip's trail that is not on the part in hand (the Saw Cleaver's form
            // B names 101, its part has 100) is replaced by the default end, 100 at the tip or
            // the row's second dummy; the other one is kept (its form-B trail 12100 -> 12110,
            // tip to mid-blade, as the game's clip), the clip's FFX only while one of its own
            // dummies is.
            const s32 group = t.cur.row->Group();
            const s32 def_base = group * 1000 + 100;
            const s32 def_tip = t.cur.row->dmy2 >= 0 ? t.cur.row->dmy2 : group * 1000 + 110;
            const s32 old_base = t.trail_base;
            const s32 old_tip = t.trail_tip;
            if (!base_ok) {
                t.trail_base = def_base;
            }
            if (!tip_ok) {
                t.trail_tip = def_tip;
            }
            if (t.trail_base == t.trail_tip || (!base_ok && !tip_ok)) {
                t.trail_base = def_base;
                t.trail_tip = def_tip;
                t.trail_ffx = TrailFfx;
            }
            if (g_log >= 1) {
                LOG_INFO(Core, "VR melee: #{} trail dummies {} / {} not on the part; {} / {} (FFX {})",
                         t.cur.serial, old_base, old_tip, t.trail_base, t.trail_tip, t.trail_ffx);
            }
            have_base = DummyAt(chr, t.trail_base, base);
            have_tip = DummyAt(chr, t.trail_tip, tip);
        }
    }
    if (w.committed && !t.whoosh_done && ms.whoosh) {
        t.whoosh_done = true;
        const B::Vec3 at = have_base && have_tip ? (base + tip) * 0.5f : base;
        const u64 h = Sound(t.whoosh, at);
        if (g_log >= 1) {
            LOG_INFO(Core, "VR melee: #{} whoosh {} at {} (h={:#x}), trail FFX {} {} -> {}{}",
                     t.cur.serial, t.whoosh, V3(at), h, t.trail_ffx, t.trail_base, t.trail_tip,
                     have_base && have_tip ? "" : " (no dummies: no trail)");
        }
    }
    if (ms.trail && g_trail && have_base && have_tip && !g_dry_run) {
        u8* const ctl = chr + OffsetSfxCtl;
        if (Read<u8*>(ctl) == g_base + SfxCtlVtableRva) {
            g_blade_ffx(ctl, TrailSlot, t.trail_ffx, &t.trail_base, &t.trail_tip);
        }
    }
    // The capsule against the blade (VR_MELEE_LOG): the hit entry's end A this frame, where it was
    // before, and the dummy it hangs from.
    if (!unpaid && (g_log >= 2 || (g_log >= 1 && t.cur_frames <= 4))) {
        u8* const entry = HitEntry(t.cur.handle, t.cur.bid);
        B::Vec3 dmy{};
        const bool have_dmy = DummyAt(chr, t.cur.row->dmy1, dmy);
        if (entry != nullptr) {
            const B::Vec3 a{Read<float>(entry + OffsetEntryA),
                            Read<float>(entry + OffsetEntryA + 4),
                            Read<float>(entry + OffsetEntryA + 8)};
            const B::Vec3 prev{Read<float>(entry + OffsetEntryPrev),
                               Read<float>(entry + OffsetEntryPrev + 4),
                               Read<float>(entry + OffsetEntryPrev + 8)};
            LOG_INFO(Core,
                     "VR melee: #{} f{} capsule A {} (prev {}, moved {:.1f} cm) radius {:.2f} | "
                     "dmy {} {} -> A {:.1f} mm | last frame's dmy -> A {:.1f} mm",
                     t.cur.serial, t.cur_frames, V3(a), V3(prev), B::Length(a - prev) * 100.0f,
                     Read<float>(entry + OffsetEntryRadius), t.cur.row->dmy1, V3(dmy),
                     have_dmy ? B::Length(a - dmy) * 1000.0f : -1.0f,
                     B::LengthSq(t.last_a) > 0.0f ? B::Length(a - t.last_a) * 1000.0f : -1.0f);
        } else if (g_log >= 2) {
            LOG_INFO(Core, "VR melee: #{} f{} no hit entry for handle {:#x} | dmy {} {}",
                     t.cur.serial, t.cur_frames, t.cur.handle, t.cur.row->dmy1, V3(dmy));
        }
        if (have_dmy) {
            t.last_a = dmy;
        }
    }
    (void)now;
}

void LogStats(TickState& t, s64 now) {
    if (now - t.last_stats_ns < StatusLogNs || g_stats.windows.load() == 0) {
        return;
    }
    t.last_stats_ns = now;
    LOG_INFO(Core,
             "VR melee: STATS windows={} attacks={} failed={} denied={} aborted={} refunds={} "
             "hits={} walls={} viscerals={} telegraphs={} unpaid={} buffered={} (opened {})",
             g_stats.windows.load(), g_stats.created.load(), g_stats.failed.load(),
             g_stats.denied.load(), g_stats.aborted.load(), g_stats.refunds.load(),
             g_stats.hits.load(), g_stats.walls.load(), g_stats.viscerals.load(),
             g_stats.telegraphs.load(), g_stats.unpaid.load(), g_stats.buffered.load(),
             g_stats.buffer_opened.load());
}

// ---- The map-hit detour (DamageStep) ------------------------------------------------------------

u32 PS4_SYSV_ABI MeleeMapHit(u8* module, u8* /*map*/, u8* res, u32 /*soft*/) {
    if (!g_physical || res == nullptr || !Plausible(module)) {
        return 0;
    }
    u8* const player = MainPlayer();
    if (player == nullptr || Read<u8*>(module + L::OffsetOwner) != player) {
        return 0;
    }
    const s64 now = NowNs();
    const s32 bid = Read<s32>(res + ResBid);
    const u32 handle = Read<u32>(res + ResHandle);
    u32 serial = 0;
    bool report = false;
    bool whip = false;
    {
        std::scoped_lock lock{g_attack_mutex};
        Attack* a = FindAttack(bid, handle, now);
        if (a == nullptr) {
            // A whip's capsules 1-3 down the chain come with other handles (as in MeleeDamage).
            a = FindAttack(bid, 0, now);
            if (a != nullptr && !IsWhipRow(a->row)) {
                a = nullptr;
            }
        }
        if (a == nullptr) {
            return 0;
        }
        serial = a->serial;
        whip = IsWhipRow(a->row);
        // The first contact only: a blade dragged along a wall touches it every frame.
        report = ++a->walls == 1;
        a->last_wall_ns = now;
    }
    if (report) {
        g_stats.walls.fetch_add(1, std::memory_order_relaxed);
        if (!whip) {
            // (A whip's chain metres out on the floor does not catch the hand: the clank only.)
            g_bite_wall.store(true, std::memory_order_relaxed);
            g_bite_hold.store(0.06f, std::memory_order_relaxed);
            g_bite_ns.store(now, std::memory_order_release);
        }
        Combat::PushMeleeEvent({Combat::MeleeEventKind::Wall, serial, 1.0f, 0.0f, now, ""});
        const B::Vec3 at{Read<float>(res + ResPosition), Read<float>(res + ResPosition + 4),
                         Read<float>(res + ResPosition + 8)};
        LOG_INFO(Core,
                 "VR melee: #{} WALL at {} (bid {} atk {}): the game's bounce suppressed, t={:.3f}",
                 serial, V3(at), bid, Read<s32>(res + ResAtkParam),
                 static_cast<double>(now) * 1e-9);
    }
    return 1; // never the canned wall bounce for a physical swing (sparks and sound stay)
}

bool InstallMapHit(u8* module) {
    if (std::memcmp(module + MapHitRva, MapHitSig.data(), MapHitSig.size()) != 0) {
        LOG_WARNING(Core, "VR melee: map-hit detour NOT installed: {:#x} differs (live {})",
                    MapHitRva, Hook::HexBytes(module + MapHitRva, MapHitSig.size()));
        return false;
    }
    using namespace Xbyak::util;
    auto& c = *g_code;
    c.align(16);
    auto* const entry = const_cast<u8*>(c.getCurr());
    Xbyak::Label suppress;
    // Entry rsp = 8 (mod 16); five pushes: 0 (mod 16) at the call.
    c.push(rbp);
    c.mov(rbp, rsp);
    c.push(rbx);
    c.push(r12);
    c.push(r13);
    c.push(r14);
    c.mov(rbx, rdi);
    c.mov(r12, rsi);
    c.mov(r13, rdx);
    c.mov(r14, rcx);
    c.mov(rax, reinterpret_cast<u64>(&MeleeMapHit));
    c.call(rax);
    c.test(eax, eax);
    c.jnz(suppress);
    c.mov(rdi, rbx);
    c.mov(rsi, r12);
    c.mov(rdx, r13);
    c.mov(rcx, r14);
    c.pop(r14);
    c.pop(r13);
    c.pop(r12);
    c.pop(rbx);
    c.pop(rbp);
    for (const u8 b : MapHitSig) { // the original function, verbatim
        c.db(b);
    }
    c.L(suppress);
    c.xor_(eax, eax);
    c.pop(r14);
    c.pop(r13);
    c.pop(r12);
    c.pop(rbx);
    c.pop(rbp);
    c.ret();
    c.ready();
    u8* const at = module + MapHitRva;
    if (!Hook::MakeWritable(at, MapHitPatchLen)) {
        LOG_ERROR(Core, "VR melee: could not make {:#x} writable; map-hit detour not installed",
                  MapHitRva);
        return false;
    }
    Hook::WriteJump(at, MapHitPatchLen, entry);
    return true;
}

// ---- The telegraph (main-player update) ---------------------------------------------------------

u32 g_telegraph_handled = 0;
std::atomic<s64> g_telegraph_sent_ns{0}; // the last notice we sent (the AI log tells it apart)
std::atomic<u32> g_telegraph_sent{0};
// Why a swing cannot attack now (the pose tick's last gate; nullptr: it can).
std::atomic<const char*> g_gate_block{"not read yet"};

// A new telegraph from the presenter: the game's own attack notices, as a vanilla attack's start
// sends them (HKS act 6 -> vt+0x440; the clip's first frame -> message 7), in the same update
// stage (PlayerIns::Update: vanilla's R1 input check calls vt+0x440 here too). Nothing of the
// hunter's own state changes: no attack type, no animation, no flag.
void TelegraphTick(u8* player, s64 now) {
    Combat::MeleeTelegraph tg{};
    if (!g_telegraph || !Combat::LatestMeleeTelegraph(tg) || tg.serial == g_telegraph_handled) {
        return;
    }
    g_telegraph_handled = tg.serial;
    const char* skipped = nullptr;
    u8* const vtable = Read<u8*>(player);
    const u8* data = Module(player, L::OffsetDataModule);
    if (VR::Melee::GetMeleeSettings().telegraph == 0) {
        skipped = "melee_telegraph=off";
    } else if (now - tg.ns > TelegraphMaxAgeNs) {
        skipped = "stale";
    } else if (VR::IsGameMenuOpen() || VR::CinematicActive()) {
        skipped = "menu or cinematic";
    } else if (data == nullptr || Read<s32>(data + OffsetHp) <= 0) {
        skipped = "dead";
    } else if (const char* block = g_gate_block.load(std::memory_order_relaxed); block != nullptr) {
        skipped = block; // no swing can follow (a dodge, a hit taken, a transform, ...)
    } else if (!Plausible(vtable) ||
               Read<u8*>(vtable + SlotNotifyAttack) != g_base + NotifyAttackRva ||
               Read<u8*>(vtable + SlotParryTiming) != g_base + ParryTimingRva) {
        skipped = "not the player's notices";
    }
    if (skipped == nullptr) { // (also in a dry run: a notice is no attack; the AI tests use it)
        g_telegraph_sent_ns.store(now, std::memory_order_relaxed);
        g_telegraph_sent.store(tg.serial, std::memory_order_relaxed);
        reinterpret_cast<NotifyAttackFn>(g_base + NotifyAttackRva)(player, AttackTypeRight);
        reinterpret_cast<ParryTimingFn>(g_base + ParryTimingRva)(player);
        g_stats.telegraphs.fetch_add(1, std::memory_order_relaxed);
    }
    LOG_INFO(Core, "VR melee: telegraph #{} ({}{}): {} {:.0f} ms after the presenter{}", tg.serial,
             tg.why != nullptr ? tg.why : "",
             tg.window != 0 ? fmt::format(", window #{}", tg.window) : std::string{},
             skipped != nullptr ? fmt::format("skipped ({})", skipped)
                                : std::string{"the AIs got the attack notice (FindAttack) and "
                                              "the parry timing"},
             static_cast<double>(now - tg.ns) * 1e-6, "");
}

// VR_MELEE_AI_LOG (tests): every AI's FindAttack timer, read every main-player update: a notice
// (the timer jumps to 1: ours, or the game's own for a vanilla attack), the interrupt (back to 0),
// and the animations of the AIs' characters within AiWatchM of the hunter (new ids only), so a
// step, guard or counter can be matched with the notice before it.
struct AiWatch {
    u8* ai{nullptr};
    u8* chr{nullptr};
    float timer{0.0f};
    s64 notice_ns{0}; // the newest notice (0: none pending)
    u32 notice_serial{0};
    const u8* ring{nullptr};
    s32 ring_write{-1};
    s32 last_id{-1};
    bool seen{false};
};
std::array<AiWatch, 48> g_ai{};
s64 g_ai_list_ns = 0;
constexpr float AiWatchM = 8.0f;

B::Vec3 ChrPos(const u8* chr) {
    u8* const ctrl = Read<u8*>(chr + L::OffsetChrCtrl);
    if (!Plausible(ctrl)) {
        return {1e9f, 1e9f, 1e9f};
    }
    const u8* w = ctrl + OffsetCtrlWorld;
    return {Read<float>(w + 0x0C), Read<float>(w + 0x1C), Read<float>(w + 0x2C)};
}

u8* AiChr(u8* ai) {
    u8* const accessor = Read<u8*>(ai);
    u8* const vtable = Plausible(accessor) ? Read<u8*>(accessor) : nullptr;
    u8* const fn = Plausible(vtable) ? Read<u8*>(vtable + SlotAiChr) : nullptr;
    if (!Plausible(fn) || fn < g_base || fn >= g_base + ChrInsVtableRva) { // (code is below)
        return nullptr;
    }
    u8* const chr = reinterpret_cast<AiChrFn>(fn)(accessor);
    if (!Plausible(chr)) {
        return nullptr;
    }
    u8* const cv = Read<u8*>(chr);
    return cv == g_base + ChrInsVtableRva || cv == g_base + EnemyInsVtableRva ? chr : nullptr;
}

void AiWatchTick(u8* player, s64 now) {
    u8* const mgrs = Read<u8*>(g_base + AiManagersRva);
    if (!Plausible(mgrs)) {
        return;
    }
    const s32 count = std::min(Read<s32>(mgrs + OffsetAiManagerCount), 8);
    u8* const array = Read<u8*>(mgrs + OffsetAiManagerArray);
    if (count <= 0 || !Plausible(array)) {
        return;
    }
    for (AiWatch& a : g_ai) {
        a.seen = false;
    }
    const B::Vec3 me = ChrPos(player);
    u32 total = 0;
    for (s32 m = 0; m < count; ++m) {
        u8* const mgr = array + static_cast<size_t>(m) * AiManagerSize;
        u8* p = Read<u8*>(mgr + OffsetAiListBegin);
        u8* const end = Read<u8*>(mgr + OffsetAiListEnd);
        for (int k = 0; k < 256 && Plausible(p) && p < end; ++k, p += 8) {
            u8* const entry = Read<u8*>(p);
            u8* const think = Plausible(entry) ? Read<u8*>(entry + OffsetAiEntryThink) : nullptr;
            u8* const ai = Plausible(think) ? Read<u8*>(think) : nullptr;
            u8* const obs = Plausible(ai) ? Read<u8*>(ai + OffsetAiObserver) : nullptr;
            if (!Plausible(obs)) {
                continue;
            }
            ++total;
            AiWatch* slot = nullptr;
            for (AiWatch& a : g_ai) {
                if (a.ai == ai) {
                    slot = &a;
                    break;
                }
            }
            if (slot == nullptr) {
                for (AiWatch& a : g_ai) {
                    if (a.ai == nullptr) {
                        slot = &a;
                        *slot = {};
                        slot->ai = ai;
                        slot->chr = AiChr(ai);
                        slot->timer = Read<float>(obs + OffsetObsTimer);
                        break;
                    }
                }
            }
            if (slot == nullptr) {
                continue;
            }
            slot->seen = true;
            const float timer = Read<float>(obs + OffsetObsTimer);
            const float threshold = Read<float>(obs + OffsetObsThreshold);
            const float dist = slot->chr != nullptr ? B::Length(ChrPos(slot->chr) - me) : -1.0f;
            if (timer > slot->timer + 0.5f) {
                // A notice: ours if we sent one this update or the last few.
                const bool ours =
                    now - g_telegraph_sent_ns.load(std::memory_order_relaxed) < 100'000'000;
                slot->notice_ns = now;
                slot->notice_serial = ours ? g_telegraph_sent.load(std::memory_order_relaxed) : 0;
                LOG_INFO(Core,
                         "VR melee: AI {} (chr {}, {:.1f} m) NOTICE from {}: FindAttack timer "
                         "{:.2f} -> {:.2f}, fires after {:.2f} of it (entry {} think==entry {})",
                         fmt::ptr(ai), fmt::ptr(slot->chr), dist,
                         ours ? fmt::format("our telegraph #{}", slot->notice_serial)
                              : std::string{"the game (a vanilla attack)"},
                         slot->timer, timer, threshold, fmt::ptr(entry), ai == entry ? 1 : 0);
            } else if (slot->notice_ns != 0 && timer <= 0.0f && slot->timer > 0.0f) {
                LOG_INFO(Core,
                         "VR melee: AI {} (chr {}, {:.1f} m) FINDATTACK interrupt {:.0f} ms after "
                         "the notice{} (interrupt bits {:#x})",
                         fmt::ptr(ai), fmt::ptr(slot->chr), dist,
                         static_cast<double>(now - slot->notice_ns) * 1e-6,
                         slot->notice_serial != 0
                             ? fmt::format(" (telegraph #{})", slot->notice_serial)
                             : std::string{},
                         Read<u64>(ai + OffsetAiInterrupts));
                slot->notice_ns = 0;
            }
            slot->timer = timer;
            // The character's new animations while it is near.
            if (slot->chr == nullptr || dist < 0.0f || dist > AiWatchM) {
                slot->ring = nullptr;
                continue;
            }
            if (u8* const cd = g_ai_god ? Module(slot->chr, L::OffsetDataModule) : nullptr;
                cd != nullptr) {
                const s32 hp = Read<s32>(cd + OffsetHp);
                const s32 max = Read<s32>(cd + OffsetHp + 4);
                if (hp > 0 && hp < max && max < 100000) {
                    std::memcpy(cd + OffsetHp, &max, sizeof(max)); // test only: never dies
                }
            }
            u8* const ta = Module(slot->chr, L::OffsetTimeActModule);
            if (ta == nullptr || Read<u8*>(ta) != g_base + L::TimeActVtableRva) {
                continue;
            }
            const s32 write = Read<s32>(ta + L::OffsetTimeActWrite);
            if (write < 0 || write >= L::TimeActRingSize) {
                continue;
            }
            if (ta != slot->ring) {
                slot->ring = ta;
                slot->ring_write = write;
                continue;
            }
            for (s32 i = 0;
                 i < (write - slot->ring_write + L::TimeActRingSize) % L::TimeActRingSize; ++i) {
                const s32 index = (slot->ring_write + i) % L::TimeActRingSize;
                const s32 id = Read<s32>(ta + L::OffsetTimeActRing + index * L::TimeActEntrySize);
                if (id == slot->last_id) {
                    continue;
                }
                slot->last_id = id;
                const s64 since = g_telegraph_sent_ns.load(std::memory_order_relaxed);
                LOG_INFO(Core, "VR melee: AI chr {} anim {} ({:.1f} m){}", fmt::ptr(slot->chr), id,
                         dist,
                         since != 0 && now - since < 3'000'000'000
                             ? fmt::format(", {:.0f} ms after telegraph #{}",
                                           static_cast<double>(now - since) * 1e-6,
                                           g_telegraph_sent.load(std::memory_order_relaxed))
                             : std::string{});
            }
            slot->ring_write = write;
        }
    }
    for (AiWatch& a : g_ai) {
        if (a.ai != nullptr && !a.seen) {
            a = {}; // gone (a load, a death)
        }
    }
    if (now - g_ai_list_ns > 10'000'000'000) {
        g_ai_list_ns = now;
        std::string nearby;
        for (const AiWatch& a : g_ai) {
            if (a.chr != nullptr) {
                const float d = B::Length(ChrPos(a.chr) - me);
                if (d < 30.0f) {
                    nearby += fmt::format(" [{} {:.1f} m]", fmt::ptr(a.chr), d);
                }
            }
        }
        LOG_INFO(Core, "VR melee: AI watch: {} AIs in {} managers; within 30 m:{}", total, count,
                 nearby.empty() ? std::string{" none"} : nearby);
    }
}

// The hunter landed (core/vr/plunge.h): a plunge's attack still live gets its landing (haptics,
// the log); it holds land_grace more, then ends (MeleePoseTick's latch).
void PlungeLanded(TickState& t, const VR::Melee::Plunge::State& s, s64 now) {
    if (!t.cur_live || !t.cur.plunge) {
        if (g_log >= 1 && s.landed_drop_m >= 0.5f) {
            LOG_INFO(Core, "VR melee: landed from {:.2f} m (no plunge in progress)",
                     s.landed_drop_m);
        }
        return;
    }
    u32 hits = 0;
    {
        std::scoped_lock lock{g_attack_mutex};
        for (const Attack& r : g_attacks) {
            if (r.serial == t.cur.serial && r.created_ns == t.cur.created_ns) {
                hits = r.hits;
            }
        }
    }
    Combat::PushMeleeEvent({Combat::MeleeEventKind::Plunge, t.cur.serial, s.landed_drop_m,
                            hits > 0 ? 1.0f : 0.0f, now, ""});
    LOG_INFO(Core, "VR melee: #{} PLUNGE landed from {:.2f} m, {} hit{} so far, {:.0f} ms after "
                   "it opened",
             t.cur.serial, s.landed_drop_m, hits, hits == 1 ? "" : "s",
             static_cast<double>(now - t.cur.created_ns) * 1e-6);
}

// ---- The left hand's swings (melee_left, core/vr/trick_weapons.h) -------------------------------
//
// A left swing (the presenter's second swing tracker: Combat::LatestMeleeWindowLeft) opens an
// attack of the left hand's own: the second blade of a transformed Blade of Mercy or Rakuyo, the
// Beast Claw's left claw, the Kos Parasite's parasite (the right weapon's rows), or a left-hand
// melee weapon's (the Torch, the Fist of Gratia, the fist: its own rows, hand -2). No table knows
// which rows those are (the generated one keeps the right hand's only), so observe the game's
// native attack slots until a candidate's capsule is seen at the left fist. Never invoke an
// unknown row to discover its side: it could hit with the right weapon or body before we check.
// Once learned, the game's own attack, as the right hand's: its stamina rule, slots 4 / 5 (the
// right hand's are 6 / 7, the game's own 0 .. 2), kept alive every update, ended by not refreshing.
// Light swings only (one row), paid from the right hand's combo budget (the presenter); no flick
// or plunge of its own. An eligible gun in the left hand (Tricks::PunchGun) punches with the
// bare fist's learned row, the gun in the hand: its first hitbox must show at the left fist
// (CheckGunPunch), else gun punches stay off. A window that closed without its swing (no commit,
// or the presenter dropped it as the off hand's counter-motion) and hit nothing gives its stamina
// back; one closed so before the game ever saw it open makes no attack. A refusal is sent back
// (LeftDenied: the deny tick, the charge back).
constexpr u32 LeftFirstSlot = 4;
constexpr s32 LeftDefaultCost = 20;
constexpr float LeftApartM = 0.30f;    // do not learn a side with the fists together
constexpr float GunPunchReachM = 0.5f; // a gun punch's hitbox this close to the left fist ...
constexpr float KnownRowReachM = 1.0f; // ... a known row's (the Torch's burning head) this close
constexpr u32 GunPunchFrames = 6;      // ... seen within this many updates of its attack

// A left attack not seen in the game before (the fist's punch with a gun or a shield in the hand,
// a known row: Tricks::KnownLeftRow): unknown until its first attack shows where it hits. Each
// left weapon's own (`what`: the gun's variation, minus the shield's family, the row's key), so a
// shield's failing never turns a gun's punches off.
enum class GunPunch : u8 { Unknown, Good, Bad };
struct HitCheck {
    s32 what{0};
    GunPunch state{GunPunch::Unknown};
};
// Separate checks for four gun variations, two shields, and three native left-melee rows.
std::array<HitCheck, 10> g_hit_checks{};
GunPunch& HitCheckOf(s32 what) {
    for (HitCheck& c : g_hit_checks) {
        if (c.what == what) {
            return c.state;
        }
    }
    for (HitCheck& c : g_hit_checks) {
        if (c.what == 0) {
            c.what = what;
            return c.state;
        }
    }
    return g_hit_checks.back().state;
}

struct LeftTick {
    s32 key{-1};         // what the rows were found for (variation * 10 + form B)
    s32 hand{HandRight}; // the rows' hand: -1 the right weapon's part, -2 the left weapon
    s32 variation{-1};
    bool gun{false}; // a handgun's punch: the fist's rows (variation 100), the gun in the hand
    s32 check{0};    // its first hit checked at the left fist for this (HitCheckOf; 0: none)
    s32 punch{0};    // the gun's variation, minus the shield's family (0: no punch)
    std::array<s32, 24> judges{};
    u32 count{0};
    u32 next{0}; // the learned row (or the one being tried)
    bool verified{false};
    // The right weapon's left part learns from your first swings (CheckLeftProbe): the live attack
    // tries judges[next]; a wrong one ended opens the next within the same swing (reprobe); a row
    // whose side two swings never showed is passed over (unseen).
    bool probing{false};
    bool reprobe{false};
    u8 unseen{0};
    s32 cost{LeftDefaultCost};
    s32 whoosh{-1}; // the dual blades' left swing sound (the right weapon's light row's; -1: none)
    M::LatchState latch{};
    // The live attack (its identity also under g_attack_mutex: the damage step reads it).
    bool live{false};
    u32 serial{0};
    s32 bid{-1};
    u32 slot{0};
    u32 handle{0};
    u32 frames{0};
    s32 paid{0}; // the stamina it took
    u32 hits{0}; // (under g_attack_mutex: the damage step counts them)
    s64 created_ns{0};
    s64 ended_ns{0};
    const char* logged_why{nullptr};
#ifndef BBVR_NO_LOGS
    s64 diagnostic_ns{0}; // weapon lookup snapshots, at most once every two seconds
#endif
};
LeftTick g_left{};

// The rows learned (vr.ini melee_left_rows, as read at start; then this update's own copy).
std::string& LearnedLeftRows() {
    static std::string rows = VR::Tricks::GetTrickSettings().left_rows;
    return rows;
}

// A row learned before, in this session or an earlier one: the weapon keeps it through its
// transforms (a split, a join) and from session to session.
void RestoreLeftRow(LeftTick& l) {
    const s32 learned = VR::Tricks::LeftRowOf(LearnedLeftRows(), l.key);
    const s32 judge = learned >= 0 ? learned : VR::Tricks::KnownLeftRow(l.key);
    for (u32 i = 0; judge >= 0 && i < l.count; ++i) {
        if (l.judges[i] == judge) {
            l.next = i;
            l.verified = true;
            if (learned < 0 && l.check == 0) {
                l.check = l.key; // known, never seen: its first hit is checked
            }
            LOG_INFO(Core, "VR melee left: judge {} {}", judge,
                     learned >= 0 ? "remembered (melee_left_rows)"
                                  : "known: the game's own left-hand attack");
            return;
        }
    }
}

// The weapon's attack rows (refType 0) its left hand may use: the right weapon's transformed
// form's (judges 400 .. 999) that are not the right hand's own (the table's) nor its B->A trick
// attack (790) or a shot (800 ..); a left weapon's L2 (800 ..) first, then its others (0 .. 399).
void ScanLeftRows(LeftTick& l, const M::WeaponMoves* moves) {
    l.count = 0;
    l.next = 0;
    l.verified = false;
    std::string list;
    const auto add = [&](s32 judge) {
        if (l.count >= l.judges.size()) {
            return;
        }
        if (moves != nullptr && l.hand == HandRight) {
            for (const M::MoveRow& r : moves->rows) {
                if (r.judge == judge) {
                    return; // the right hand's own
                }
            }
        }
        if (GunBehaviorRefType(100000000 + l.variation * 1000 + judge) == 0) {
            l.judges[l.count++] = judge;
            list += fmt::format(" {}", judge);
        }
    };
    if (l.hand == HandLeft) {
        for (s32 j = 800; j <= 899; ++j) {
            add(j);
        }
        for (s32 j = 0; j <= 399; ++j) {
            add(j);
        }
    } else {
        for (s32 j = 400; j <= 999; ++j) {
            if (j != 790 && (j < 800 || j > 899)) {
                add(j);
            }
        }
    }
    LOG_INFO(Core, "VR melee left: variation {} ({}): native attack rows to observe:{}", l.variation,
             l.gun                ? "a handgun's punch: the fist's, learned with the hand empty"
             : l.hand == HandLeft ? "the left weapon"
                                  : "the right weapon's left part",
             list.empty() ? std::string{" none"} : list);
}

void LeftEnd(LeftTick& l, const char* why, s64 now) {
    if (!l.live) {
        return;
    }
    {
        std::scoped_lock lock{g_attack_mutex};
        l.live = false;
        l.ended_ns = now;
    }
    LOG_INFO(Core, "VR melee left: #{} ended after {} frames ({})", l.serial, l.frames, why);
}

// The attack is over: ended, its stamina back when it was no swing (`refund`: its window never
// committed, or was dropped as the off hand's counter-motion) and it hit nothing, as the right
// hand's (Finalize).
void LeftFinalize(u8* chr, LeftTick& l, bool refund, const char* why, s64 now) {
    LeftEnd(l, why, now);
    u32 hits = 0;
    {
        std::scoped_lock lock{g_attack_mutex};
        hits = l.hits;
    }
    if (refund && hits == 0 && l.paid > 0) {
        Refund(chr, l.paid, l.serial, "a left swing that never came and hit nothing");
    }
    l.paid = 0;
}

void LeftOpen(u8* chr, LeftTick& l, const Combat::MeleeWindow& w, s64 now) {
    u8* const module = Module(chr, OffsetAttackModule);
    u8* const data = Module(chr, L::OffsetDataModule);
    const s32 judge = l.judges[l.next];
    // A gun's punch: the fist's row itself (the game's id for the left hand is the gun's).
    const s32 bid = l.gun ? 100000000 + VR::Tricks::FistVariation * 1000 + judge
                          : BehaviorId(chr, judge, l.hand);
    M::SwingStamina rule = SwingRule(M::MoveKind::Light);
    // Gun punches only; preserve exhaustion/recovery flags. Dual-wield parts have punch=0,
    // shields have negative family ids. Right-hand attacks and shooting use other paths.
    rule.pct = VR::Tricks::GunPunchStaminaPct(l.punch, rule.pct);
    const bool probe = !l.verified && l.hand == HandRight && !l.gun; // a left part's: tried
    const char* refuse = !l.verified && !probe                ? "no observed left-hand row"
                         : w.unpaid                           ? "combo budget unpaid"
                         : module == nullptr || data == nullptr ? "no behaviour module"
                         : bid < 0                            ? "no row"
                         : M::StaminaRefuses(rule, Read<s32>(data + OffsetStamina))
                             ? "no stamina"
                             : nullptr;
    if (refuse != nullptr) {
        l.latch.active = false;
        l.latch.done = true;
        Combat::PushMeleeEvent({Combat::MeleeEventKind::LeftDenied, w.serial, 0.0f, 0.0f, now,
                                refuse});
        LOG_INFO(Core, "VR melee left: #{} refused: {} (judge {})", w.serial, refuse, judge);
        return;
    }
    const u32 slot = LeftFirstSlot + (w.serial & 1);
    l.probing = probe;
    l.reprobe = false;
    const s32 before = Read<s32>(data + OffsetStamina);
    const u8 ok = [&] {
        if (g_dry_run) return u8{0};
        M::LeftAttack::Scope scope(M::LeftAttack::NativeWeapon(l.variation, l.gun) ? chr : nullptr);
        return Invoke(chr, module, bid, slot, l.cost, rule);
    }();
    const u32 handle = Read<u32>(module + OffsetAttackSlots + slot * 8);
    const s32 after = Read<s32>(data + OffsetStamina);
    {
        std::scoped_lock lock{g_attack_mutex};
        l.live = ok != 0;
        l.serial = w.serial;
        l.bid = bid;
        l.slot = slot;
        l.handle = handle;
        l.frames = 0;
        l.paid = ok != 0 ? std::max(before - after, 0) : 0;
        l.hits = 0;
        l.created_ns = now;
        l.ended_ns = 0;
    }
    LOG_INFO(Core, "VR melee left: #{} open judge {} bid {} slot {} ok={} stamina {} -> {}{}",
             w.serial, judge, bid, slot, ok, before, after,
             l.gun   ? " (a punch with the gun in the hand)"
             : probe ? fmt::format(" (trying it: candidate {} of {})", l.next + 1, l.count)
                     : std::string{});
    if (ok == 0 && !g_dry_run) {
        // As the right hand's Denied: the deny tick, the charge back.
        Combat::PushMeleeEvent({Combat::MeleeEventKind::LeftDenied, w.serial, 0.0f, 0.0f, now,
                                "the game made no attack"});
    }
    // The dual blades' left blade whooshes as the right one does (2026-10-14): the
    // weapon's own light swing sound, once as its attack opens, at the left hand.
    B::Vec3 at{};
    if (ok != 0 && l.whoosh > 0 && VR::Melee::GetMeleeSettings().whoosh &&
        DummyAt(chr, LeftWeaponDummy, at)) {
        const u64 h = Sound(l.whoosh, at);
        LOG_INFO(Core, "VR melee left: #{} whoosh {} at {} (h={:#x})", w.serial, l.whoosh, V3(at),
                 h);
    }
}

// The first punch with a gun in the hand: the fist's row with another weapon in that hand is
// untested, so its hitbox must show up at the left fist. Else it ends now (its stamina back unless
// it hit) and gun punches stay off (logged); with the fists together, the hitbox between them or
// not built yet, ask again next update.
void CheckGunPunch(u8* chr, LeftTick& l, s64 now) {
    GunPunch& state = HitCheckOf(l.check);
    const float reach = l.gun ? GunPunchReachM : KnownRowReachM;
    const auto off = [&](const char* why) {
        state = GunPunch::Bad;
        LeftFinalize(chr, l, true, why, now);
    };
    u8* const entry = HitEntry(l.handle, l.bid);
    B::Vec3 left{};
    B::Vec3 right{};
    B::Vec3 a{};
    if (entry != nullptr) {
        a = {Read<float>(entry + OffsetEntryA), Read<float>(entry + OffsetEntryA + 4),
             Read<float>(entry + OffsetEntryA + 8)};
    }
    if (entry == nullptr || !B::IsFinite(a) || B::LengthSq(a) == 0.0f ||
        !DummyAt(chr, LeftWeaponDummy, left) || !DummyAt(chr, RightWeaponDummy, right)) {
        if (l.frames >= GunPunchFrames) {
            LOG_INFO(Core, "VR melee left: #{} a punch with the gun in the hand: no hitbox seen in "
                           "{} updates: gun punches off for this session", l.serial, l.frames);
            off("the gun punch showed no hitbox");
        }
        return;
    }
    if (M::LeftAttack::NativeWeapon(l.variation, l.gun) && l.judges[l.next] == 800) {
        // Retail 1.09 torch/Gratia L2 hit0 uses local dummy 100. GetAttackHand=-2
        // maps it to the left weapon (20100), not the body or right weapon.
        B::Vec3 weapon{};
        if (!DummyAt(chr, 20100, weapon)) {
            if (l.frames >= GunPunchFrames) off("left weapon dummy unavailable");
            return;
        }
        const float error = B::Length(a - weapon);
        const float reach = B::Length(weapon - left);
        const bool ok = M::LeftAttack::AtWeapon(error, reach);
        LOG_INFO(Core, "VR melee left: #{} native weapon check {}: hit to dummy 20100 {:.3f} m, "
                       "dummy to grip {:.3f} m: {}", l.serial, l.check, error, reach,
                 ok ? "on the left weapon" : "wrong placement; off for this session");
        if (ok) state = GunPunch::Good;
        else off("native left attack is not on its weapon dummy");
        return;
    }
    if (B::Length(left - right) < LeftApartM) {
        return; // the fists together: no side to tell
    }
    const float to_left = B::Length(a - left);
    const float to_right = B::Length(a - right);
    const VR::Tricks::CapsuleSide side = VR::Tricks::SideOf(to_left, to_right);
    if (side == VR::Tricks::CapsuleSide::Unsure && to_left <= reach) {
        return; // between the fists: no side to tell yet
    }
    const bool ok = to_left <= reach && side == VR::Tricks::CapsuleSide::Left;
    LOG_INFO(Core, "VR melee left: #{} the first hit {} (check {}): its hitbox {:.2f} m from the left "
                   "fist, {:.2f} m from the right: {}",
             l.serial, l.gun ? "of a punch with a gun or shield in the hand" : "of a known row",
             l.check, to_left, to_right,
             ok ? "at the left hand, on" : "NOT at the left hand, off for this session");
    if (ok) {
        state = GunPunch::Good;
    } else {
        off("the gun punch's hitbox is not at the hand");
    }
}

// A left part's row being tried (VR::Tricks::ProbeStep): its hitbox at the left fist, learned and
// kept in vr.ini (melee_left_rows); at the right one, ended now (its stamina back unless it hit)
// and the next candidate opens within the same swing; its side unseen, ended, tried again next
// swing, passed over after two such swings.
void CheckLeftProbe(u8* chr, LeftTick& l, s64 now) {
    namespace Tr = VR::Tricks;
    u8* const entry = HitEntry(l.handle, l.bid);
    B::Vec3 left{};
    B::Vec3 right{};
    B::Vec3 a{};
    if (entry != nullptr) {
        a = {Read<float>(entry + OffsetEntryA), Read<float>(entry + OffsetEntryA + 4),
             Read<float>(entry + OffsetEntryA + 8)};
    }
    const bool seen = entry != nullptr && B::IsFinite(a) && B::LengthSq(a) > 0.0f &&
                      DummyAt(chr, LeftWeaponDummy, left) &&
                      DummyAt(chr, RightWeaponDummy, right) &&
                      B::Length(left - right) >= LeftApartM;
    const float to_left = seen ? B::Length(a - left) : 0.0f;
    const float to_right = seen ? B::Length(a - right) : 0.0f;
    const s32 judge = l.judges[l.next];
    switch (Tr::ProbeStep(seen, to_left, to_right, l.frames)) {
    case Tr::Probe::Wait:
        return;
    case Tr::Probe::Learned:
        l.probing = false;
        l.verified = true;
        l.unseen = 0;
        LearnedLeftRows() = Tr::WithLeftRow(LearnedLeftRows(), l.key, judge);
        VR::SaveSetting("melee_left_rows", LearnedLeftRows());
        LOG_INFO(Core,
                 "VR melee left: #{} learned judge {} from your swing: its hitbox {:.2f} m from "
                 "the left fist, {:.2f} m from the right (kept in vr.ini: melee_left_rows)",
                 l.serial, judge, to_left, to_right);
        return;
    case Tr::Probe::Wrong:
        LOG_INFO(Core,
                 "VR melee left: #{} judge {} is the right hand's ({:.2f} m from the right fist): "
                 "the next one",
                 l.serial, judge, to_right);
        LeftFinalize(chr, l, true, "a right-hand row", now);
        l.unseen = 0;
        ++l.next;
        l.reprobe = l.next < l.count;
        return;
    case Tr::Probe::Unseen:
        LeftFinalize(chr, l, true, "its hitbox's side unseen", now);
        if (++l.unseen >= 2) {
            LOG_INFO(Core, "VR melee left: judge {}: its side never showed: the next one", judge);
            l.unseen = 0;
            ++l.next;
        }
        return;
    }
}

// A learned row, or a left part's row being tried, is refreshed.
void LeftRefresh(u8* chr, LeftTick& l, s64 now) {
    if (!l.live) {
        return;
    }
    u8* const module = Module(chr, OffsetAttackModule);
    if (module == nullptr || (!l.verified && !l.probing)) {
        LeftEnd(l, "no verified attack", now);
        return;
    }
    if (l.probing) {
        CheckLeftProbe(chr, l, now);
        if (!l.live) {
            return; // not refreshed: the game ends it this update
        }
    }
    if (l.check != 0 && HitCheckOf(l.check) == GunPunch::Unknown) {
        CheckGunPunch(chr, l, now);
        if (!l.live) {
            return; // not refreshed: the game ends it this update
        }
    }
    if (!g_dry_run) {
        M::LeftAttack::Scope scope(M::LeftAttack::NativeWeapon(l.variation, l.gun) ? chr : nullptr);
        g_invoke(module, l.bid, 0, l.slot, 1);
    }
    ++l.frames;
}

// Read only: slots 0..2 belong to the game's animations: a left weapon's row (a torch's, the
// fists') shows in the game's own L2 attack. (The right weapon's left part learns from your first
// swings instead, CheckLeftProbe: physical swings never let the game show one; one shown anyway
// is learned here too.)
void ObserveLeftRow(u8* chr, LeftTick& l) {
    if (l.verified || l.count == 0) {
        return;
    }
    u8* const module = Module(chr, OffsetAttackModule);
    B::Vec3 left{};
    B::Vec3 right{};
    if (module == nullptr || !DummyAt(chr, LeftWeaponDummy, left) ||
        !DummyAt(chr, RightWeaponDummy, right) || B::Length(left - right) < LeftApartM) {
        return;
    }
    for (u32 slot = 0; slot < 3; ++slot) {
        const u8* const native = module + OffsetAttackSlots + slot * 8;
        if (Read<u8>(native + 4) == 0) {
            continue; // not refreshed by a native attack this update
        }
        for (u32 i = 0; i < l.count; ++i) {
            const s32 bid = BehaviorId(chr, l.judges[i], l.hand);
            u8* const entry = bid >= 0 ? HitEntry(Read<u32>(native), bid) : nullptr;
            if (entry == nullptr) {
                continue;
            }
            const B::Vec3 a{Read<float>(entry + OffsetEntryA),
                            Read<float>(entry + OffsetEntryA + 4),
                            Read<float>(entry + OffsetEntryA + 8)};
            if (B::IsFinite(a) && B::LengthSq(a) > 0.0f &&
                VR::Tricks::SideOf(B::Length(a - left), B::Length(a - right)) ==
                    VR::Tricks::CapsuleSide::Left) {
                l.next = i;
                l.verified = true;
                LearnedLeftRows() = VR::Tricks::WithLeftRow(LearnedLeftRows(), l.key, l.judges[i]);
                VR::SaveSetting("melee_left_rows", LearnedLeftRows());
                LOG_INFO(Core,
                         "VR melee left: learned judge {} from native slot {} (kept in vr.ini: "
                         "melee_left_rows)",
                         l.judges[i], slot);
                return;
            }
        }
    }
}

// Every update after the right hand's (MeleePoseTick): whose left hand, its rows, its window.
void LeftMeleeTick(u8* chr, const TickState& t, const MeleeBody& body, s64 now,
                   Combat::MeleeStatus& status) {
    namespace Tr = VR::Tricks;
    auto& l = g_left;
    s32 variation = -1;
    s32 hand = HandRight;
    bool gun = false;
    s32 punch = 0; // the gun's variation, minus the shield's family (its first hit's check)
    if (t.variation >= 0 && t.form_b && Tr::LeftPartWeapon(t.variation)) {
        variation = t.variation;
    } else if (t.style >= 0 && t.style != 2 && t.style != 3) { // the left weapon in its hand
        // Judge 0 is a generic action for guns, not their equipped weapon variation.
        // Read the same weapon-specific L2 row that the shooting path uses.
        const s32 v = Tr::LeftWeaponVariation(BehaviorId(chr, Tr::LeftWeaponJudge, HandLeft));
        VR::Body::BodyStatus body_status{};
        const bool shield = VR::Body::LatestBodyStatus(body_status) &&
                            Tr::IsShieldFamily(body_status.left_weapon_family);
        if (Tr::LeftMeleeWeapon(v) || Tr::PunchGun(v) || shield) {
            gun = Tr::PunchGun(v) || shield; // a shield bashes with the fist's punch, as a gun
            variation = gun ? Tr::FistVariation : v; // a gun's punch: the fist's rows (and key)
            hand = HandLeft;
            punch = shield ? -body_status.left_weapon_family : gun ? v : 0;
        }
    }
    const s32 key = variation >= 0 ? variation * 10 + (hand == HandRight ? 1 : 0) : -1;
    if (key != l.key || gun != l.gun || punch != l.punch) {
        LeftEnd(l, "another weapon", now);
        l.latch.active = false;
        l.latch.done = true;
        l.key = key;
        l.hand = hand;
        l.variation = variation;
        l.punch = punch;
        l.check = punch; // (a known row's, RestoreLeftRow)
        {
            std::scoped_lock lock{g_attack_mutex};
            l.gun = gun; // (the damage step reads it)
        }
        l.cost = LeftDefaultCost;
        l.count = 0;
        l.probing = false;
        l.reprobe = false;
        l.unseen = 0;
        if (variation >= 0) {
            ScanLeftRows(l, hand == HandRight ? t.moves : nullptr);
            RestoreLeftRow(l);
            l.whoosh = hand == HandRight ? 611 : -1; // (the form-B default, as the right's)
            if (hand == HandRight && t.moves != nullptr) {
                if (const M::MoveRow* light = M::ChooseMove(*t.moves, true, M::MoveKind::Light).row;
                    light != nullptr) {
                    l.cost = light->stamina; // the right hand's light: the same weapon's pace
                    if (light->whoosh > 0) {
                        l.whoosh = light->whoosh; // ... and its swing sound
                    }
                }
            }
        }
    }
    if (g_physical && Tr::GetTrickSettings().melee_left && !l.gun) {
        ObserveLeftRow(chr, l); // (a gun's punch is never shown: its trigger shoots)
    }
    const TransformRunInfo& run = TransformRun();
    const char* why = !Tr::GetTrickSettings().melee_left ? "melee_left=0"
                      : !g_physical                      ? "melee=canned"
                      : variation < 0                    ? "no left-hand blade or weapon"
                      : M::LeftAttack::NativeWeapon(variation, gun) && !g_left_hand_hook
                          ? "native left attack hand hook unavailable"
                      : l.next >= l.count                ? "no left-hand row found"
                      : !l.verified && l.gun ? "a punch with the gun: the fist's punch is not "
                                               "learned yet (punch once with the hand empty)"
                      : !l.verified && l.hand == HandLeft
                          ? "waiting to observe a native left attack"
                      : l.check != 0 && HitCheckOf(l.check) == GunPunch::Bad
                          ? "its first hit was not at the left hand (off for this session)"
                      : VR::IsGameMenuOpen()              ? "menu"
                      : VR::CinematicActive()             ? "cinematic"
                      : !body.body || !body.driven       ? "body off"
                      : run.active                       ? "a transform is running"
                      : !body.left_ik || body.left_weight < 0.5f ? "the game has the left arm"
                                                                 : nullptr;
#ifndef BBVR_NO_LOGS
    // Read-only comparison with the gun's judge-800 lookup. Do not use these values to
    // select an attack: this diagnoses weapons rejected before a left swing can open.
    if (l.diagnostic_ns == 0 || now - l.diagnostic_ns >= 2'000'000'000) {
        l.diagnostic_ns = now;
        const s32 lookup0 = BehaviorId(chr, 0, HandLeft);
        const s32 lookup800 = BehaviorId(chr, 800, HandLeft);
        const auto decoded = [](s32 bid) {
            return bid >= 100000000 && bid < 200000000 ? (bid - 100000000) / 1000 : -1;
        };
        VR::Body::BodyStatus bs{};
        Combat::GunStatus gs{};
        const bool have_body = VR::Body::LatestBodyStatus(bs);
        const bool have_gun = Combat::LatestGunStatus(gs);
        LOG_INFO(Core,
                 "VR melee left DIAG: style={} right_variation={} form_b={} "
                 "judge0={} variation0={} supported0={} judge800={} variation800={} "
                 "supported800={} body_status={} body_age_ms={} left_family={} left_slot={} "
                 "gun_status={} gun_age_ms={} gun_family={} gun_in_hand={} gun_direct={} "
                 "selected={} hand={} punch={} rows={} next={} verified={} check={} "
                 "physical={} enabled={} driven={} left_ik={} left_weight={:.2f} gate={}",
                 t.style, t.variation, t.form_b, lookup0, decoded(lookup0),
                 Tr::LeftMeleeWeapon(decoded(lookup0)) || Tr::PunchGun(decoded(lookup0)),
                 lookup800, decoded(lookup800),
                 Tr::LeftMeleeWeapon(decoded(lookup800)) || Tr::PunchGun(decoded(lookup800)),
                 have_body, have_body ? (now - bs.steady_ns) / 1'000'000 : -1,
                 bs.left_weapon_family, bs.left_weapon_slot, have_gun,
                 have_gun ? (now - gs.ns) / 1'000'000 : -1, static_cast<unsigned>(gs.family),
                 gs.in_hand, gs.direct_ok, variation, hand, punch, l.count, l.next, l.verified,
                 l.check, g_physical, Tr::GetTrickSettings().melee_left, body.driven,
                 body.left_ik, body.left_weight, why != nullptr ? why : "ready");
    }
#endif
    if (why != l.logged_why) {
        l.logged_why = why;
        LOG_INFO(Core, "VR melee left: {}", why == nullptr ? "left swings attack" : why);
    }
    Combat::MeleeWindow w{};
    const bool have = Combat::LatestMeleeWindowLeft(w) && now - w.ns <= WindowMaxAgeNs;
    M::LatchInput li{};
    li.have = have;
    li.serial = w.serial;
    li.open = have && w.open;
    li.paid = !w.unpaid;
    // (A window that closed without its swing before an update saw it open makes no attack.)
    li.stale = have && !w.open &&
               (w.aborted || (w.close_ns != 0 && now - w.close_ns > StaleCloseNs));
    li.gates = why == nullptr;
    if (have && w.serial != l.latch.serial && l.live) {
        // A newer window replaced the live one before an update saw it close (the latch just stops
        // refreshing it): finished here, its stamina back when it was no swing.
        LeftFinalize(chr, l, w.prev_serial == l.serial && w.prev_aborted, "a new swing", now);
    }
    switch (M::StepLatch(l.latch, li)) {
    case M::LatchStep::Create:
        LeftOpen(chr, l, w, now);
        break;
    case M::LatchStep::Refresh:
        LeftRefresh(chr, l, now);
        if (l.reprobe && !l.live && have && w.open && w.serial == l.serial && why == nullptr) {
            LeftOpen(chr, l, w, now); // a left part's next candidate, in the same swing
        }
        break;
    case M::LatchStep::End: {
        // Its swing never came (no commit, or the off hand's counter-motion): the stamina back
        // unless it hit something.
        const bool aborted = have && w.serial == l.serial && w.aborted;
        const char* end = why != nullptr ? why : aborted ? "no swing" : "the window closed";
        LeftFinalize(chr, l, aborted, end, now);
        break;
    }
    case M::LatchStep::Deny:
        Combat::PushMeleeEvent({Combat::MeleeEventKind::LeftDenied, w.serial, 0.0f, 0.0f, now,
                                why != nullptr ? why : "?"});
        LOG_INFO(Core, "VR melee left: #{} refused ({})", w.serial, why != nullptr ? why : "?");
        break;
    case M::LatchStep::Wait:
    case M::LatchStep::Idle:
        if (!l.latch.active) {
            LeftEnd(l, "over", now);
        }
        break;
    }
    status.left_ok = why == nullptr;
    status.left_why = why != nullptr ? why : "";
    status.left_learning = variation >= 0 && g_physical && Tr::GetTrickSettings().melee_left &&
                           !l.verified && !l.gun;
    status.left_handled = l.latch.serial;
    status.left_dual = variation >= 0 && hand == HandRight;
    status.left_gun = l.gun;
    status.left_shield = l.punch < 0;
}

// The damage step: a hit of the left swing's attack (its row and capsule): the left hand's impact.
bool LeftMeleeHit(u8* res, float damage, s64 now) {
    const s32 bid = Read<s32>(res + ResBid);
    const u32 handle = Read<u32>(res + ResHandle);
    u32 serial = 0;
    bool gun = false;
    {
        std::scoped_lock lock{g_attack_mutex};
        LeftTick& l = g_left;
        if (l.bid < 0 || bid != l.bid || handle != l.handle ||
            (!l.live && now - l.ended_ns > HitGraceNs)) {
            return false;
        }
        serial = l.serial;
        gun = l.gun;
        ++l.hits;
    }
    Combat::PushMeleeEvent({Combat::MeleeEventKind::LeftHit, serial, damage, 0.09f, now, ""});
    // A gun's punch: whose attack rating it hits with (the gun's, or the right weapon's?).
    LOG_INFO(Core, "VR melee left: #{} HIT dmg {:.1f}{}", serial, damage,
             gun ? " (a punch with the gun in the hand)" : "");
    return true;
}

} // Anonymous namespace

void InstallMelee(VAddr base, u64 size) {
    if (g_base != nullptr) {
        return;
    }
    auto* const module = reinterpret_cast<u8*>(base);
    const auto& ms = VR::Melee::GetMeleeSettings();
    bool physical = ms.physical;
    if (const char* env = std::getenv("VR_MELEE"); env != nullptr) {
        physical = std::string_view{env} != "vanilla" && std::string_view{env} != "0";
    }
    if (const char* value = std::getenv("VR_MELEE_LOG"); value != nullptr) {
        g_log = std::max(std::atoi(value), 1);
    }
    g_dry_run = std::getenv("VR_MELEE_DRYRUN") != nullptr;
    if (size <= WorldChrManRva + 8) {
        LOG_INFO(Core, "VR melee: eboot too small ({:#x}) for Bloodborne 1.09; melee is vanilla",
                 size);
        return;
    }
    // VR_MELEE_AI_LOG (tests): the AIs' side of the attack notices, also with melee=vanilla (the
    // game's own attacks for comparison).
    if (std::getenv("VR_MELEE_AI_LOG") != nullptr) {
        g_ai_log =
            std::memcmp(module + AiDispatchRva, AiDispatchSig.data(), AiDispatchSig.size()) == 0 &&
            std::memcmp(module + AiNoticeRva, AiNoticeSig.data(), AiNoticeSig.size()) == 0 &&
            std::memcmp(module + AiFindAttackRva, AiFindAttackSig.data(), AiFindAttackSig.size()) ==
                0;
        if (g_ai_log) {
            g_base = module;
            g_ai_god = std::getenv("VR_MELEE_AI_GOD") != nullptr;
        }
        LOG_INFO(Core, "VR melee: AI log {}{}", g_ai_log ? "on" : "NOT on (a signature differs)",
                 g_ai_god ? " (VR_MELEE_AI_GOD: the AIs near the hunter keep their HP)" : "");
    }
    if (!physical) {
        LOG_INFO(Core, "VR melee: melee=vanilla: a swing presses R1 / R2 (the game's attack "
                       "animation)");
        return;
    }
    g_base = module;
    struct Site {
        const char* name;
        u64 rva;
        const u8* sig;
        size_t length;
    };
    const std::array<Site, 7> sites = {{
        {"InvokeAttack", InvokeAttackRva, InvokeAttackSig.data(), InvokeAttackSig.size()},
        {"GetBehaviorId", GetBehaviorIdRva, GetBehaviorIdSig.data(), GetBehaviorIdSig.size()},
        {"hand code", HandCodeRva, HandCodeSig.data(), HandCodeSig.size()},
        {"GetDmyPolyMtx", DmyPolyRva, DmyPolySig.data(), DmyPolySig.size()},
        {"IsMainPlayer", IsMainPlayerRva, IsMainPlayerSig.data(), IsMainPlayerSig.size()},
        {"PlaySound", PlaySoundRva, PlaySoundSig.data(), PlaySoundSig.size()},
        {"event 128 sound type", SoundTypeRva, SoundTypeSig.data(), SoundTypeSig.size()},
    }};
    bool ok = true;
    std::string bad;
    for (const Site& s : sites) {
        if (std::memcmp(module + s.rva, s.sig, s.length) != 0) {
            ok = false;
            bad += fmt::format(" {} at {:#x} (live {})", s.name, s.rva,
                               Hook::HexBytes(module + s.rva, std::min<size_t>(s.length, 24)));
        }
    }
    if (ok) {
        s32 disp{};
        std::memcpy(&disp, module + IsMainPlayerRva + 13, sizeof(disp));
        if (IsMainPlayerRva + 17 + static_cast<s64>(disp) != WorldChrManRva) {
            ok = false;
            bad += " WorldChrMan proof";
        }
    }
    if (!ok) {
        g_base = nullptr;
        LOG_WARNING(Core, "VR melee: physical melee NOT installed:{}; a swing presses R1 / R2",
                    bad);
        return;
    }
    g_stake_geometry =
        std::memcmp(module + StakeEntryCopyRva, StakeEntryCopySig.data(), StakeEntryCopySig.size()) == 0 &&
        std::memcmp(module + StakeDummyStoreRva, StakeDummyStoreSig.data(), StakeDummyStoreSig.size()) == 0;
    g_invoke = reinterpret_cast<InvokeAttackFn>(module + InvokeAttackRva);
    g_dmy_poly = reinterpret_cast<DmyPolyFn>(module + DmyPolyRva);
    g_play_sound = reinterpret_cast<PlaySoundFn>(module + PlaySoundRva);
    {
        s32 disp{};
        std::memcpy(&disp, module + SoundTypeRva + 3, sizeof(disp));
        g_sound_table = module + SoundTypeRva + 7 + disp;
    }
    g_sound = true;
    g_visceral =
        std::memcmp(module + TryThrowAllRva, TryThrowAllSig.data(), TryThrowAllSig.size()) == 0;
    if (g_visceral) {
        g_try_throw = reinterpret_cast<TryThrowAllFn>(module + TryThrowAllRva);
    }
    g_trail = std::memcmp(module + BladeFfxRva, BladeFfxSig.data(), BladeFfxSig.size()) == 0 &&
              std::memcmp(module + CtlCtorRva, CtlCtorSig.data(), CtlCtorSig.size()) == 0;
    if (g_trail) {
        g_blade_ffx = reinterpret_cast<BladeFfxFn>(module + BladeFfxRva);
    }
    g_regen = std::memcmp(module + RegenStoreRva, RegenStoreSig.data(), RegenStoreSig.size()) == 0;
    g_free_flag = std::memcmp(module + InvokeStaminaRva, InvokeStaminaSig.data(),
                              InvokeStaminaSig.size()) == 0;
    g_telegraph =
        std::memcmp(module + NotifyAttackRva, NotifyAttackSig.data(), NotifyAttackSig.size()) ==
            0 &&
        std::memcmp(module + ParryTimingRva, ParryTimingSig.data(), ParryTimingSig.size()) == 0;
    try {
        g_code = std::make_unique<Xbyak::CodeGenerator>(512);
        g_map_hook = InstallMapHit(module);
    } catch (const Xbyak::Error& error) {
        LOG_ERROR(Core, "VR melee: code emission failed ({}); no map-hit detour", error.what());
        g_map_hook = false;
    }
    try {
        g_left_hand_hook = InstallLeftAttackHand(module);
    } catch (const Xbyak::Error& error) {
        LOG_ERROR(Core, "VR melee: left attack hand hook failed ({})", error.what());
    }
    LOG_INFO(Core, "VR melee left: scoped native weapon hand selection {}",
             g_left_hand_hook ? "ready" : "unavailable (torch/Gratia swings disabled)");
    g_installed = true;
    g_physical = true;
    LOG_INFO(Core,
             "VR melee: physical melee installed (InvokeAttack, GetBehaviorId, hand code, "
             "GetDmyPolyMtx, WorldChrMan, PlaySound ok; visceral {}, trail {}, regen pause {}, "
             "free-swing flag {}, map-hit detour {}, AI notices {}): the swing is the attack, {} "
             "weapons with moves, swings {} | {}{}{}",
             g_visceral ? "ok" : "MISMATCH (none)", g_trail ? "ok" : "MISMATCH (none)",
             g_regen ? "ok" : "MISMATCH (none)",
             g_free_flag ? "ok" : "MISMATCH (a free swing's stamina put back after the call)",
             g_map_hook ? "ok" : "MISMATCH (bounces stay)",
             g_telegraph ? "ok" : "MISMATCH (no telegraph)", M::Moves.size(),
             SwingRule().pct >= 100 ? std::string{"cost the game's stamina"}
             : SwingRule().charge   ? fmt::format("cost {}% of the game's stamina", SwingRule().pct)
                                    : std::string{"cost no stamina"},
             VR::Melee::MeleeSettingsText(),
             g_log > 0 ? fmt::format(" log={}", g_log) : std::string{},
             g_dry_run ? " DRY RUN (no attack)" : "");
}

bool MeleePhysicalWanted() {
    return g_installed && g_physical;
}

// The right weapon's blade (MeleeBladeSegment: its hit capsule, base -> tip) this update and its
// velocity since the last one, for the kill ragdoll's push and the weapon against the walls. A
// weapon or form change, a gap or a snap turn starts over; the hands reused (a stereo pair's
// second eye) wait for the next sample, the velocity then over both updates.
void TrackBlade(u8* chr, TickState& t, const MeleeBody& body, s64 now) {
    if (body.hands_reused) {
        return;
    }
    B::Vec3 base{}, tip{}, grip{};
    const char* how = nullptr;
    if (!MeleeBladeSegment(chr, base, tip, how, &grip)) {
        t.blade_ns = 0;
        g_blade_reach.valid = false;
        return;
    }
    // Its reach from the grip (weapon_wall's solid blade, the body's: along R_Weapon -Y).
    g_blade_reach = {true, B::Length(base - grip), B::Length(tip - grip)};
    const float turn = std::remainder(body.base_yaw - t.blade_yaw, 2.0f * 3.14159265f);
    const bool same = t.blade_ns != 0 && now > t.blade_ns && now - t.blade_ns <= BladeTickMaxNs &&
                      t.blade_variation == t.variation && t.blade_style == t.style &&
                      std::abs(turn) <= BladeTurnRad;
    if (same) {
        const float inv_dt = 1e9f / static_cast<float>(now - t.blade_ns);
        const B::Vec3 base_vel = (base - t.blade_base) * inv_dt;
        const B::Vec3 tip_vel = (tip - t.blade_tip) * inv_dt;
        if (B::IsFinite(base_vel) && B::IsFinite(tip_vel)) {
            std::scoped_lock lock{g_attack_mutex};
            g_blade = {true, base, tip, base_vel, tip_vel, now};
        }
    }
    t.blade_ns = now;
    t.blade_base = base;
    t.blade_tip = tip;
    t.blade_yaw = body.base_yaw;
    t.blade_variation = t.variation;
    t.blade_style = t.style;
}

void TrackPartBlades(u8* chr, const TickState& t) {
    for (auto& blade : g_part_blade) {
        blade.key = -1;
        blade.seen = false;
    }
    if (!VR::Tricks::GetTrickSettings().weapon_wall || !t.form_b ||
        !VR::Tricks::LeftPartWeapon(t.variation)) {
        return;
    }
    const s32 key = t.variation * 10 + 1;
    u8* const parts = Read<u8*>(chr + OffsetChrAsm);
    const auto attachment = [&](int i) -> s32 {
        if (!Plausible(parts)) {
            return -1;
        }
        u8* const part = Read<u8*>(parts + OffsetAsmParts + 8 * (RightPartSlot + i));
        if (!Plausible(part) || Read<u8*>(part) != g_base + WeaponPartVtableRva ||
            Read<s32>(part + OffsetPartMode) != 2) {
            return -1;
        }
        return Read<s32>(part + OffsetPartDummy);
    };
    for (int hand = 0; hand < 2; ++hand) {
        PartBlade& blade = g_part_blade[hand];
        blade.key = key;
        std::array<float, 12> grip{};
        s32 found = -1;
        if (DummyMtx(chr, hand == 0 ? LeftWeaponDummy : RightWeaponDummy, grip)) {
            VR::Solid::Shape line{};
            const auto point = [&](int id, B::Vec3& p) {
                B::Vec3 world{};
                if (!DummyAt(chr, id, world)) {
                    return false;
                }
                p = InFrameCm(grip, world) * 0.01f;
                return true;
            };
            found = VR::Solid::AttachedPartLine(hand, attachment, point, line);
            if (found >= 0) {
                blade.seen = true;
                blade.base = line.base;
                blade.tip = line.tip;
            }
        }
        if (blade.logged != key * 2 + (found >= 0 ? 1 : 0)) {
            blade.logged = key * 2 + (found >= 0 ? 1 : 0);
            LOG_INFO(Core, "VR solid: {} part of variation {}: group {}, line {} cm",
                     hand == 0 ? "left" : "right", t.variation, found,
                     blade.seen ? B::Length(blade.tip - blade.base) * 100.0f : 0.0f);
        }
    }
}

void MeleePoseTick(u8* chr, const MeleeBody& body) {
    // The weapon-only transform shares this tick and its right-arm state (and goes first: its
    // gate holds swings back while the form changes).
    TransformPoseTick(chr, body);
    // ... and the physical whip (whip=physical: the chain of a whip form on the hand).
    WhipPoseTick(chr, body);
    if (!g_installed || chr == nullptr) {
        SetSawWeaponPose(nullptr, 0.0f, false);
        g_part_blade = {};
        return;
    }
    auto& t = g_tick;
    const s64 now = NowNs();
    const float dt = StepSeconds(t, now);
    if (chr != t.chr) {
        // A new character object (a load): nothing of the old one carries over.
        const M::LatchState latch = t.latch;
        t = {};
        t.chr = chr;
        t.last_ns = now;
        t.latch = latch;
        t.latch.active = false;
        t.latch.done = true;
        std::scoped_lock lock{g_attack_mutex};
        const M::LatchState left_latch = g_left.latch;
        g_left = {};
        g_left.latch.serial = left_latch.serial;
        g_left.latch.done = true;
    }
    // The plunge (melee_plunge): the hunter in the air, from his last update's motion signals.
    const VR::Melee::Plunge::Config plunge_cfg = PlungeConfig();
    const AirSample air = LatestAir();
    const auto& air_state = t.air.Step(
        {air.valid && now - air.steady_ns < AirMaxAgeNs, air.grounded, air.y, air.steady_ns},
        plunge_cfg);
    if (air_state.landed) {
        PlungeLanded(t, air_state, now);
    }
    // A dodge begins (dodge_attack, fair mode): a swing begun more than DodgeCarryGraceNs before it
    // is in its follow-through; the dodge cancels it as the game's own dodge would and keeps its
    // i-frames (BlockReason ends it this update). One begun just before is a swing into the dodge.
    if (body.dodge_attack && (!t.dodge_now || body.owner_id != t.dodge_clip)) {
        const bool fair = !VR::GetSettings().dodge_attack_iframes;
        t.dodge_cancel = fair && t.cur_live && now - t.cur.created_ns > DodgeCarryGraceNs
                             ? t.cur.serial
                             : 0;
    } else if (!body.dodge_attack) {
        t.dodge_cancel = 0;
    }
    t.dodge_now = body.dodge_attack;
    t.dodge_clip = body.owner_id;
    if (t.cur_live && t.cur.serial != t.dodge_cancel) {
        DodgeAttackCut(t, t.cur.serial, "still running"); // a swing carried into the dodge
    }
    if (!t.first_logged) {
        t.first_logged = true;
        LOG_INFO(Core,
                 "VR melee: first pose tick on thread '{}' (body {}, driven {}, right {} "
                 "{:.2f})",
                 Common::GetCurrentThreadName(), body.body ? 1 : 0, body.driven ? 1 : 0,
                 body.right_ik ? "yours" : "game's", body.right_weight);
    }
    t.regen_left = std::max(t.regen_left - dt, 0.0f);
    if (t.regen_left > 0.0f) {
        PauseRegen(chr);
    }
    Combat::MeleeWindow w{};
    const bool have = Combat::LatestMeleeWindow(w) && now - w.ns < WindowMaxAgeNs;
    const bool new_window = have && w.serial != t.latch.serial;
    if (t.updates++ % 4 == 0 || new_window) {
        ReadWeapon(chr, t);
    } else if (const s32 style = WeaponStyle(chr); style != t.style) {
        ReadWeapon(chr, t); // a transform: at once
    }
    if (t.variation != t.logged_variation || t.form_b != t.logged_form_b ||
        (t.weapon_why == nullptr) != t.logged_ok) {
        t.logged_variation = t.variation;
        t.logged_form_b = t.form_b;
        t.logged_ok = t.weapon_why == nullptr;
        LOG_INFO(Core, "VR melee: right weapon {} (variation {}) form {} (style {}): {}",
                 t.moves != nullptr ? t.moves->name : "?", t.variation, t.form_b ? "B" : "A",
                 t.style,
                 t.weapon_why == nullptr ? "physical swings"
                                         : fmt::format("R1 / R2 ({})", t.weapon_why));
    }
    // Hide only an empty hand. Keep the remembered slot while hidden: the game's hide
    // override destroys the part, and rediscovering it every tick caused the blinking.
    Combat::BowVisual bow_visual{};
    if (t.bow_hand >= 0 && t.bow_arrow_part >= 0 && VR::Bow::GetBowSettings().enabled &&
        Combat::BowVisualChannel().Load(bow_visual) && bow_visual.ns > 0 &&
        now >= bow_visual.ns && now - bow_visual.ns < 250 * Ms && !bow_visual.arrow) {
        HideRightWeaponPart(chr, static_cast<u32>(t.bow_arrow_part));
    }
    // The blade's motion for the kill ragdoll (melee_kill_ragdoll) and its reach for the solid
    // weapons (weapon_wall, the body's): every update, swing or not.
    if (VR::GetSettings().melee_kill_ragdoll || VR::Tricks::GetTrickSettings().weapon_wall) {
        TrackBlade(chr, t, body, now);
    }
    TrackPartBlades(chr, t);
    // (After a transform: once its clip has settled, about 1.5 s after the switch.)
    if (g_log >= 1 && t.moves != nullptr && t.style_stable >= 12 &&
        (t.variation != t.geo_variation || t.style != t.geo_style)) {
        t.geo_variation = t.variation;
        t.geo_style = t.style;
        LogBladeGeometry(chr, t);
    }
    // A flick mid-swing: the window joins the transform; its old-form attack ends as the switch
    // is handed over, the transform attack opens once the new form is in the hand.
    const TransformRunInfo& run = TransformRun();
    FlickTick(chr, t, w, have, run, now);
    const bool flick_stroke =
        have && t.flick.window == w.serial && t.flick.phase != FlickPhase::Done;
    const bool known_style = run.switched && run.style == t.style && run.switched_ns != 0 &&
                             now - run.switched_ns < 1'000'000'000;
    const char* gate = BlockReason(chr, body, t, flick_stroke, known_style);
    if (SawAttackTick(chr, body, t, gate, dt, now)) {
        gate = "the saw's grinding attack"; // never create an ordinary swing alongside the spin
    }
    g_gate_block.store(gate, std::memory_order_relaxed); // (a telegraph needs a swing possible)
    // A new window replaces the one served: that one is over.
    if (new_window && t.cur_live) {
        Finalize(chr, t, &w, "a new window", now);
    }
    if (new_window) {
        t.unpaid_live = false;
    }
    M::LatchInput li{};
    li.have = have;
    li.serial = w.serial;
    // A physical whip's chain hurts only while it moves fast (core/vr/bloodborne_whip.h): its
    // attack waits for the chain's tip to come up to speed (the window may close first: the lash
    // after it still hits, else none) and ends once the tip has slowed. A flick's transform attack
    // is the transform's.
    const WhipChainState chain = WhipChainNow(now);
    const bool whip_slow = chain.applies && !chain.fast && !flick_stroke;
    // A physical whip still lashing after the swing's window keeps its attack (the chain's lag).
    // A plunge's attack holds until the hunter has landed (core/vr/plunge.h).
    const bool plunge_hold = have && t.cur_live && t.cur.plunge && w.serial == t.cur.serial &&
                             t.air.Holds(now, t.cur.created_ns, plunge_cfg);
    li.open = (w.open && !(whip_slow && t.latch.active)) || (have && WhipHoldsWindow(w, now)) ||
              plunge_hold;
    li.stale = !w.open && w.close_ns != 0 && now - w.close_ns > StaleCloseNs;
    li.gates = gate == nullptr;
    li.ready = !g_visceral || !VR::Melee::GetMeleeSettings().visceral ||
               g_visceral_checked.load(std::memory_order_acquire) == w.serial;
    // The swing buffer: a new swing refused only because the game takes no attack yet (a kept
    // stagger's attack window about to open) waits for it, its window still open, at most
    // swing_buffer_ms; then (or at any other refusal) the latch refuses it as before.
    const bool buffer_pending =
        have && (w.serial != t.latch.serial || (!t.latch.active && !t.latch.done));
    const s64 buffer_ns =
        static_cast<s64>(VR::Melee::GetMeleeSettings().swing_buffer_ms * 1e6f);
    const bool buffering =
        t.buffer.Hold(w.serial, buffer_pending, gate == AttackWaitWhy, w.open, now, buffer_ns);
    if (buffering) {
        li.ready = false;
        li.max_wait = 1u << 30; // until the game's window or the buffer's end
        if (t.buffer_logged != w.serial) {
            t.buffer_logged = w.serial;
            g_stats.buffered.fetch_add(1, std::memory_order_relaxed);
            LOG_INFO(Core,
                     "VR melee: #{} swing held ({}): up to {:.0f} ms for the game's attack window",
                     w.serial, AttackWaitWhy, buffer_ns / 1e6);
        }
    }
    if (g_thrust_wait.load(std::memory_order_relaxed)) {
        li.max_wait = std::max(li.max_wait, ThrustWaitFrames); // the thrust verdict first
    }
    if (whip_slow && !t.latch.active) {
        li.ready = false;
        li.max_wait = 1u << 30; // until the chain is fast, or the next window
    }
    if (new_window) {
        g_stats.windows.fetch_add(1, std::memory_order_relaxed);
    }
    const M::LatchStep step = M::StepLatch(t.latch, li);
    const s64 held_ns = t.buffer_logged == w.serial ? t.buffer.HeldNs(w.serial, now) : 0;
    if (step == M::LatchStep::Create && held_ns > 0) {
        g_stats.buffer_opened.fetch_add(1, std::memory_order_relaxed);
        LOG_INFO(Core, "VR melee: #{} held {:.0f} ms: the game takes attacks again, its attack opens",
                 w.serial, held_ns / 1e6);
    }
    switch (step) {
    case M::LatchStep::Create:
        if (flick_stroke && t.flick.phase != FlickPhase::Old) {
            FlickOpen(chr, t, w, body.right_weight, now); // the switch is under way or done
        } else {
            Create(chr, t, w, body.right_weight, now, flick_stroke);
        }
        break;
    case M::LatchStep::Refresh:
        if (flick_stroke && t.flick.phase == FlickPhase::Gap) {
            FlickOpen(chr, t, w, body.right_weight, now);
        } else if (t.unpaid_live && !t.cur_live && !w.unpaid && w.serial == t.cur.serial) {
            // The presenter paid for it at its commit (a charge came back): its attack now (its
            // whoosh once).
            LOG_INFO(Core, "VR melee: #{} paid at its commit: the attack opens now", w.serial);
            const bool whooshed = t.whoosh_done;
            Create(chr, t, w, body.right_weight, now, flick_stroke);
            t.whoosh_done = t.whoosh_done || (t.cur_live && whooshed);
        } else {
            Refresh(chr, t, w, now);
        }
        break;
    case M::LatchStep::End:
        t.unpaid_live = false;
        Finalize(chr, t, have ? &w : nullptr,
                 !have             ? "no window"
                 : gate != nullptr ? gate
                                   : "the window closed",
                 now);
        break;
    case M::LatchStep::Deny:
        g_stats.denied.fetch_add(1, std::memory_order_relaxed);
        Combat::PushMeleeEvent({Combat::MeleeEventKind::Denied, w.serial, 0.0f, 0.0f, now, gate});
        LOG_INFO(Core, "VR melee: #{} {} refused ({}){}", w.serial,
                 M::MoveKindName(static_cast<M::MoveKind>(std::min<u8>(w.kind, 2))),
                 gate != nullptr ? gate : "?",
                 held_ns > 0 ? fmt::format(" after holding it {:.0f} ms", held_ns / 1e6)
                             : std::string{});
        break;
    case M::LatchStep::Wait:
    case M::LatchStep::Idle:
        break;
    }
    if (step == M::LatchStep::Idle && t.cur_live && !t.latch.active) {
        Finalize(chr, t, have ? &w : nullptr, "over", now);
    }
    if (!t.latch.active) {
        t.unpaid_live = false;
    }
    Combat::MeleeStatus status{};
    status.ns = now;
    status.installed = g_installed;
    status.physical = g_physical;
    // A flick's stroke keeps the swing route while it lasts, also into a form without moves of
    // its own (the presenter would close the window and press R1 / R2 for the rest of it).
    const bool flick_live = flick_stroke && w.open && t.flick.phase != FlickPhase::Done;
    status.ok = g_physical && body.body && (t.weapon_why == nullptr || flick_live);
    status.variation = t.variation;
    status.form_b = t.form_b;
    status.charged_melee = t.moves != nullptr && t.moves->rows[t.form_b ? 5 : 2].Valid();
    // Publish geometry, not world velocity: XR differentiates this tip on its final steered
    // pose in tracking space, so locomotion/snap turns cannot manufacture an attack. Measured
    // once per settled form (the dummies of a spinning part move; the dmy lookups are not free).
    if (t.style == 3 && VR::TwoHand::SharedHandle(t.variation / 10)) {
        auto& g = t.two_hand_geo;
        if (g.variation != t.variation || g.style != t.style) {
            g = {};
            g.variation = t.variation;
            g.style = t.style;
        }
        if (!g.valid && t.style_stable >= 12) {
            B::Vec3 base{}, tip{};
            const char* how = nullptr;
            float strike_near = 0.0f;
            std::array<float, 12> grip{};
            if (MeleeBladeSegment(chr, base, tip, how, nullptr, &strike_near) &&
                DummyMtx(chr, RightWeaponDummy, grip)) {
                g.tip = InFrameCm(grip, tip) * 0.01f;
                g.strike_near = strike_near;
                g.valid = B::IsFinite(g.tip) && B::Length(g.tip) > 0.05f && std::isfinite(strike_near);
            }
        }
        status.blade_tip_valid = g.valid;
        status.blade_tip = g.tip;
        status.blade_strike_near = g.strike_near;
    }
    status.bow_hand = t.bow_hand;
    status.bow_direct = t.bow_hand >= 0 && VR::Bow::GetBowSettings().direct && GunBowDirectOk();
    status.handled = t.latch.serial;
    {
        const VR::Melee::Plunge::State& air = t.air.Current();
        status.falling = plunge_cfg.enabled && air.airborne && air.descending;
        status.fall_m = status.falling ? air.drop_m : 0.0f;
    }
    LeftMeleeTick(chr, t, body, now, status);
    status.why = !body.body                ? "body off"
                 : status.ok               ? ""
                 : t.weapon_why != nullptr ? t.weapon_why
                                           : "";
    Combat::PublishMeleeStatus(status);
    LogStats(t, now);
}

// ---- Physical viscerals (visceral_thrust=1; core/vr/visceral_thrust.h) --------------------------

// A target in its visceral stagger carries these SpEffects (the gun parry's stagger a000_002100
// has both, 3.6 s; the charged swing's back stagger too).
constexpr std::array<s32, 2> VisceralEffects = {52, 53};
constexpr u32 OffsetChrSpEffects = 0x1C8; // ChrIns -> SpEffect list: first node +0x08
constexpr u32 OffsetEffectHead = 0x08;
constexpr u32 OffsetEffectFlags = 0x1C;
constexpr u32 OffsetEffectId = 0x40;
constexpr u32 OffsetEffectNext = 0x58;
constexpr u32 EffectSkipMask = 0x800C0003;
constexpr s64 VisceralTargetNs = 8'000'000'000; // the last target hit is followed this long
constexpr float VisceralOpenM = 6.0f;           // ... while this near
constexpr s64 ThrustMaxAgeNs = 300'000'000;     // an older thrust is not acted on
constexpr s64 ThrustStateMaxAgeNs = 500'000'000; // the presenter's state is current

struct VisceralWatch {
    u8* target{nullptr};
    bool open{false};
    u32 serial{0};
    s32 effect{-1};
    s64 open_ns{0};
    u64 effects_hash{0}; // VR_MELEE_LOG: the target's SpEffects, logged on change
};
VisceralWatch g_vwatch{};
u32 g_thrust_handled = 0;

bool IsChr(const u8* chr) {
    if (!Plausible(chr)) {
        return false;
    }
    const u8* const vt = Read<const u8*>(chr);
    return vt == g_base + ChrInsVtableRva || vt == g_base + EnemyInsVtableRva;
}

// The first of VisceralEffects the character has (-1: none); `hash` / `ids` (logs) the list.
s32 VisceralEffectOf(const u8* chr, u64* hash = nullptr, std::string* ids = nullptr) {
    u8* const list = Read<u8*>(chr + OffsetChrSpEffects);
    if (!Plausible(list)) {
        return -1;
    }
    s32 found = -1;
    u8* node = Read<u8*>(list + OffsetEffectHead);
    for (u32 i = 0; i < 256 && Plausible(node); ++i) {
        if ((Read<u32>(node + OffsetEffectFlags) & EffectSkipMask) == 0) {
            const s32 id = Read<s32>(node + OffsetEffectId);
            if (found < 0 && std::find(VisceralEffects.begin(), VisceralEffects.end(), id) !=
                                 VisceralEffects.end()) {
                found = id;
            }
            if (hash != nullptr) {
                *hash = *hash * 1000003u + static_cast<u32>(id);
            }
            if (ids != nullptr && i < 24) {
                *ids += fmt::format(" {}", id);
            }
        }
        node = Read<u8*>(node + OffsetEffectNext);
    }
    return found;
}

// VR_MELEE_AI_SPFX=<id>,... (tests, with VR_MELEE_AI_LOG): these SpEffects on the hunter and on
// every AI's character, logged as they come and go (4640: what the Tiny Music Box's bullet 400
// puts on every character within its 10 m, the SpEffect m24_01's EMEVD 12414808 / 12414809 wait
// for on Father Gascoigne to make him stagger), and the AI observers' message bits (+0xB8: the AI
// messages 1 / 4 / 5 / 6 set 0x1 / 0x40 / 0x80 / 0x100, the handler 0x1222E70) as they change.
struct SpfxSlot {
    const u8* chr{nullptr};
    u32 mask{0};
    u16 flags{0};
    std::array<s64, 8> since{};
    bool seen{false};
};
std::array<SpfxSlot, 49> g_spfx{};
std::array<s32, 8> g_spfx_ids{};
s32 g_spfx_count = -1; // -1: not read yet

void SpfxWatchTick(u8* player, s64 now) {
    if (g_spfx_count < 0) {
        g_spfx_count = 0;
        if (const char* env = std::getenv("VR_MELEE_AI_SPFX"); env != nullptr) {
            for (const char* p = env; *p != 0 && g_spfx_count < 8;) {
                char* end = nullptr;
                const long id = std::strtol(p, &end, 10);
                if (end == p) {
                    ++p;
                    continue;
                }
                g_spfx_ids[static_cast<size_t>(g_spfx_count++)] = static_cast<s32>(id);
                p = end;
            }
            LOG_INFO(Core, "VR melee: SPFX watch: {} SpEffect ids from VR_MELEE_AI_SPFX ({})",
                     g_spfx_count, env);
        }
    }
    if (g_spfx_count == 0 || !Plausible(player)) {
        return;
    }
    const auto mask_of = [](const u8* chr) {
        u32 mask = 0;
        u8* const list = Read<u8*>(chr + OffsetChrSpEffects);
        if (!Plausible(list)) {
            return mask;
        }
        u8* node = Read<u8*>(list + OffsetEffectHead);
        for (u32 i = 0; i < 256 && Plausible(node); ++i) {
            const s32 id = Read<s32>(node + OffsetEffectId);
            for (s32 k = 0; k < g_spfx_count; ++k) {
                if (g_spfx_ids[static_cast<size_t>(k)] == id) {
                    mask |= 1u << k;
                }
            }
            node = Read<u8*>(node + OffsetEffectNext);
        }
        return mask;
    };
    for (SpfxSlot& s : g_spfx) {
        s.seen = false;
    }
    const B::Vec3 me = ChrPos(player);
    const auto visit = [&](const u8* chr, u8* ai) {
        SpfxSlot* slot = nullptr;
        for (SpfxSlot& s : g_spfx) {
            if (s.chr == chr) {
                slot = &s;
                break;
            }
        }
        if (slot == nullptr) {
            for (SpfxSlot& s : g_spfx) {
                if (s.chr == nullptr) {
                    slot = &s;
                    *slot = {};
                    slot->chr = chr;
                    break;
                }
            }
        }
        if (slot == nullptr) {
            return;
        }
        slot->seen = true;
        const bool hunter = chr == player;
        const float dist = hunter ? 0.0f : B::Length(ChrPos(chr) - me);
        const u32 mask = mask_of(chr);
        for (s32 k = 0; k < g_spfx_count; ++k) {
            const u32 bit = 1u << k;
            if ((mask & bit) == (slot->mask & bit)) {
                continue;
            }
            const s32 id = g_spfx_ids[static_cast<size_t>(k)];
            if ((mask & bit) != 0) {
                slot->since[static_cast<size_t>(k)] = now;
                LOG_INFO(Core, "VR melee: SPFX {} ON {} {} ({:.1f} m)", id,
                         hunter ? "the hunter" : "AI chr", fmt::ptr(chr), dist);
            } else {
                LOG_INFO(Core, "VR melee: SPFX {} OFF {} {} ({:.1f} m) after {:.0f} ms", id,
                         hunter ? "the hunter" : "AI chr", fmt::ptr(chr), dist,
                         static_cast<double>(now - slot->since[static_cast<size_t>(k)]) * 1e-6);
            }
        }
        slot->mask = mask;
        u8* const obs = ai != nullptr ? Read<u8*>(ai + OffsetAiObserver) : nullptr;
        if (Plausible(obs)) {
            const u16 flags = Read<u16>(obs + 0xB8) & 0x01C1;
            if (flags != slot->flags && dist < 30.0f) {
                LOG_INFO(Core, "VR melee: AI chr {} ({:.1f} m) observer message bits {:#x} -> {:#x}",
                         fmt::ptr(chr), dist, slot->flags, flags);
            }
            slot->flags = flags;
        }
    };
    visit(player, nullptr);
    for (const AiWatch& a : g_ai) {
        if (a.ai != nullptr && a.chr != nullptr && a.chr != player) {
            visit(a.chr, a.ai);
        }
    }
    for (SpfxSlot& s : g_spfx) {
        if (s.chr != nullptr && !s.seen) {
            s = {};
        }
    }
}

// Whether a visceral is open: the character the hunter last hit (within VisceralTargetNs; the
// gun's parry shot or a charged swing) is alive within VisceralOpenM and carries a visceral
// SpEffect, the hunter not in a throw himself. Published for the presenter's heartbeat cue.
bool VisceralOpenTick(u8* player, s64 now) {
    VR::FeatureCost::Scope cost{VR::FeatureCost::VisceralOpen};
    auto& v = g_vwatch;
    s64 hit_ns = 0;
    u8* const target = LastPlayerHitTarget(hit_ns);
    // (Never the hunter himself: his own Molotov's blast is a hit of his on him.)
    u8* const followed = target != nullptr && target != player &&
                                 now - hit_ns < VisceralTargetNs && IsChr(target)
                             ? target
                             : nullptr;
    s32 effect = -1;
    float distance = 0.0f;
    s32 hp = -1;
    if (followed != nullptr) {
        u64 hash = 0;
        std::string ids;
        effect = VisceralEffectOf(followed, g_log >= 1 ? &hash : nullptr,
                                  g_log >= 1 ? &ids : nullptr);
        const B::Vec3 a = ChrPos(player);
        const B::Vec3 b = ChrPos(followed);
        distance = std::hypot(b.x - a.x, b.z - a.z);
        u8* const data = Module(followed, L::OffsetDataModule);
        hp = data != nullptr ? Read<s32>(data + L::OffsetHp) : -1;
        if (g_log >= 1 && (hash != v.effects_hash || followed != v.target)) {
            v.effects_hash = hash;
            LOG_INFO(Core, "VR melee: visceral watch: target {} hp {} {:.2f} m, SpEffects:{}",
                     fmt::ptr(followed), hp, distance, ids.empty() ? " none" : ids);
        }
    }
    v.target = followed;
    u8* const throws = Module(player, L::OffsetThrowModule);
    u8* const node = throws != nullptr ? Read<u8*>(throws + L::OffsetThrowNode) : nullptr;
    const bool in_throw = Plausible(node) && Read<u8>(node + L::OffsetThrowState) != 0;
    const bool open = followed != nullptr && effect >= 0 && hp > 0 &&
                      distance <= VisceralOpenM && !in_throw;
    if (open && !v.open) {
        ++v.serial;
        v.open_ns = now;
        v.effect = effect;
        LOG_INFO(Core,
                 "VR melee: a visceral is open: target {} (SpEffect {}) {:.2f} m away, hp {}",
                 fmt::ptr(followed), effect, distance, hp);
    } else if (!open && v.open) {
        LOG_INFO(Core, "VR melee: the visceral closed after {:.2f} s ({})",
                 static_cast<double>(now - v.open_ns) * 1e-9,
                 in_throw          ? "the hunter's throw"
                 : followed == nullptr ? "no target"
                 : hp <= 0         ? "the target died"
                 : effect < 0      ? "the stagger ended"
                                   : "out of reach");
    }
    v.open = open;
    Combat::VisceralOpen pub{};
    pub.serial = v.serial;
    pub.open = open;
    pub.distance = distance;
    pub.target = open ? followed : nullptr;
    pub.ns = now;
    Combat::VisceralOpenChannel().Store(pub);
    return open;
}

// The game's R1 visceral check now (TryThrowAll, pad type R1): the ThrowParam id started or -1
// (`skipped`: why it was not called).
s32 TryVisceralNow(u8* player, bool fresh, const char*& skipped) {
    s32 result = -1;
    if (!VR::Melee::GetMeleeSettings().visceral) {
        skipped = "melee_visceral=0";
    } else if (!fresh) {
        skipped = "stale";
    } else if (VR::IsGameMenuOpen() || VR::CinematicActive()) {
        skipped = "menu or cinematic";
    } else {
        u8* const throws = Module(player, L::OffsetThrowModule);
        if (throws == nullptr || Read<u8*>(throws) != g_base + L::ThrowVtableRva ||
            Read<const u8*>(throws + L::OffsetOwner) != player) {
            skipped = "no throw module";
        } else {
            u8* const node = Read<u8*>(throws + L::OffsetThrowNode);
            if (Plausible(node) && Read<u8>(node + L::OffsetThrowState) != 0) {
                skipped = "already in a throw";
            } else if (!g_dry_run) {
                const u64 stage = BeginVisceralStage(player);
                result = g_try_throw(throws, PadTypeR1, 0);
                ConfirmVisceralStage(stage, result >= 0);
            }
        }
    }
    return result;
}

void ReportVisceral(u8* player, u32 serial, bool thrust, s32 result, const char* skipped,
                    s64 now) {
    if (result >= 0) {
        g_stats.viscerals.fetch_add(1, std::memory_order_relaxed);
        Combat::PushMeleeEvent({Combat::MeleeEventKind::Visceral, serial,
                                static_cast<float>(result), thrust ? 1.0f : 0.0f, now, ""});
        LOG_INFO(Core, "VR melee: #{} VISCERAL{}: the game's throw {} started (thread '{}')",
                 serial, thrust ? " (a thrust)" : "", result, Common::GetCurrentThreadName());
    } else if (g_log >= 1 || thrust) {
        // The HKS's throw-attack type ([[chr+0x58]+0x38]+0x64: 1 in idle, walk, run, dash and
        // guard; the visceral rows want 1) tells a refused check apart from no target.
        const u8* hks = HksState(player);
        LOG_INFO(Core, "VR melee: #{} {}visceral check: {} (throw-attack type {})", serial,
                 thrust ? "thrust: " : "",
                 skipped != nullptr ? skipped : "TryThrowAll -1 (none possible)",
                 hks != nullptr ? static_cast<int>(Read<u8>(hks + OffsetThrowAttackType)) : -1);
    }
}

void MeleeSiteA(u8* player) {
    if (player == nullptr || g_base == nullptr) {
        return;
    }
    const s64 now = NowNs();
    if (g_installed) {
        TelegraphTick(player, now); // a swing the enemies may read: the game's attack notice
    }
    if (g_ai_log) {
        AiWatchTick(player, now);
        SpfxWatchTick(player, now); // VR_MELEE_AI_SPFX (tests)
    }
    if (!g_installed || !g_visceral) {
        g_thrust_wait.store(false, std::memory_order_relaxed);
        return;
    }
    const auto& ms = VR::Melee::GetMeleeSettings();
    // Physical viscerals: the presenter's thrusts (visceral_thrust=1). A swing never starts one
    // then (visceral_swing=0): a new window waits for its verdict while a visceral is open.
    const bool open = ms.visceral_thrust && VisceralOpenTick(player, now);
    Combat::ThrustState th{};
    const bool thrust_mode = ms.visceral_thrust && Combat::ThrustChannel().Load(th) &&
                             th.enabled && now - th.ns < ThrustStateMaxAgeNs;
    const bool swing_only_thrust = thrust_mode && !ms.visceral_swing;
    g_thrust_wait.store(swing_only_thrust && open, std::memory_order_relaxed);
    Combat::MeleeWindow w{};
    const bool have_w = Combat::LatestMeleeWindow(w) && w.serial != 0;
    if (thrust_mode && th.thrust_serial != g_thrust_handled) {
        const bool first = g_thrust_handled == 0 && now - th.thrust_ns >= ThrustMaxAgeNs;
        g_thrust_handled = th.thrust_serial;
        if (!first) {
            const char* skipped = nullptr;
            const s32 result = TryVisceralNow(player, now - th.thrust_ns < ThrustMaxAgeNs, skipped);
            ReportVisceral(player, th.thrust_window, true, result, skipped, now);
            LOG_INFO(Core,
                     "VR melee: thrust {} ({:.2f} m/s, {:.0f} cm, window #{}, a "
                     "visceral {}open): {}",
                     th.thrust_serial, th.peak, th.travel * 100.0f, th.thrust_window,
                     open ? "" : "not ", result >= 0 ? "the visceral" : "none");
            if (have_w && th.thrust_window == w.serial) {
                g_visceral_checked.store(w.serial, std::memory_order_release); // checked now
            }
        }
    }
    if (!have_w || w.serial == g_visceral_checked.load(std::memory_order_relaxed)) {
        return;
    }
    const bool fresh = now - w.ns < WindowMaxAgeNs &&
                       (w.open || (w.close_ns != 0 && now - w.close_ns <= StaleCloseNs));
    if (swing_only_thrust) {
        namespace T = VR::Melee::Thrust;
        const T::Verdict verdict = th.verdict_window == w.serial
                                       ? static_cast<T::Verdict>(th.verdict)
                                       : T::Verdict::Pending;
        if (verdict == T::Verdict::Pending && open && fresh && w.open) {
            return; // the thrust verdict first (the pose tick waits ThrustWaitFrames for it)
        }
        if (verdict != T::Verdict::Thrust) {
            g_visceral_checked.store(w.serial, std::memory_order_release);
            if (open) {
                LOG_INFO(Core,
                         "VR melee: #{} a swing at an open visceral: it hits as a swing "
                         "(visceral_thrust=1: a thrust starts the visceral; {} {})",
                         w.serial, T::VerdictName(verdict),
                         th.verdict_window == w.serial && th.why != nullptr ? th.why : "");
            }
            return;
        }
    }
    const char* skipped = nullptr;
    const s32 result = TryVisceralNow(player, fresh, skipped);
    g_visceral_checked.store(w.serial, std::memory_order_release);
    ReportVisceral(player, w.serial, swing_only_thrust, result, skipped, now);
}

bool MeleeDamage(u8* res, u8* target, float* damage) {
    if (!g_physical || res == nullptr || damage == nullptr) {
        return false;
    }
    if (Read<s32>(res + ResBullet) == -1 && LeftMeleeHit(res, *damage, NowNs())) {
        return true; // a left swing's hit (reported; its damage the game's)
    }
    if (Read<s32>(res + ResHand) != HandRight || Read<s32>(res + ResBullet) != -1) {
        return false;
    }
    const s64 now = NowNs();
    const s32 bid = Read<s32>(res + ResBid);
    const u32 handle = Read<u32>(res + ResHandle);
    Attack a{};
    {
        std::scoped_lock lock{g_attack_mutex};
        Attack* found = FindAttack(bid, handle, now);
        if (found == nullptr) {
            // A whip's row has four capsules down its chain (core/vr/whip_moves.h); a hit by one
            // after the first comes with another handle: the attack is matched by its row.
            found = FindAttack(bid, 0, now);
            if (found != nullptr && !IsWhipRow(found->row)) {
                found = nullptr;
            }
        }
        if (found == nullptr) {
            return false;
        }
        // A whip form's move (whip_pace, core/vr/whip_damage.h PaceMeter): the window's share of a
        // full hit at the game's own pace (less the share of its move's recovery it came early by);
        // set by its first hit that deals damage, the same for every enemy and capsule after it.
        if (found->whip_variation >= 0 && found->pace < 0.0f && VR::Whip::GetWhipSettings().pace &&
            std::isfinite(*damage) && *damage > 0.0f) {
            const M::MoveKind kind = found->verdict_light ? M::MoveKind::Light : found->kind;
            found->pace = g_whip_pace.Pay(
                found->created_ns,
                VR::Whip::WhipPaceRecover(found->whip_variation, static_cast<std::size_t>(kind)));
        }
        a = *found;
    }
    const auto& ms = VR::Melee::GetMeleeSettings();
    Combat::MeleeWindow w{};
    const bool have_w = Combat::LatestMeleeWindow(w) && w.serial == a.serial;
    // (A whip's chain still lashes after its window closed: its hits take the speed of the chain's
    // own tip, mapped onto a blade's scale: core/vr/whip_spin.h BladeEquivalentTip.)
    const WhipChainState chain = IsWhipRow(a.row) ? WhipChainNow(now) : WhipChainState{};
    const float tip =
        chain.applies && chain.tip > 0.0f ? VR::Whip::BladeEquivalentTip(chain.tip)
                      : std::max(have_w ? (IsWhipRow(a.row) ? std::max(w.tip_speed, w.tip_peak)
                                                            : w.tip_speed)
                                        : 0.0f,
                                 a.tip);
    M::DamageConfig cfg{};
    cfg.speed =
        !a.saw && !a.powered && ms.speed_damage &&
        (have_w || a.tip > 0.0f || (chain.applies && chain.tip > 0.0f));
    // The combo budget sets the rate instead; a whip form's own moves: their pace (whip_pace).
    const bool whip_paced = a.pace >= 0.0f;
    const float whip_pace = whip_paced ? a.pace : 1.0f;
    cfg.cadence = !a.saw && !whip_paced && (!ms.budget || a.flick);
    float factor = M::DamageFactor(tip, a.since_counted_s, a.kind, cfg);
    // The two-handed grip's swing (core/vr/two_hand_grip.h): in the HIT line's factor.
    const float two_hand =
        std::max(a.two_hand, have_w ? std::clamp(w.two_hand, 1.0f, 2.0f) : 1.0f);
    if (two_hand > 1.0f) {
        factor *= two_hand;
        LOG_INFO(Core, "VR melee: #{} two-handed swing: x{:.2f} (in the HIT line's factor)",
                 a.serial, two_hand);
    }
    float row_factor = 1.0f;
    if (!a.transform && a.verdict_light && a.light != nullptr && a.row != nullptr &&
        a.row->mv > 0) {
        // A wound-up swing the classifier judged light: the light row's motion value.
        row_factor = static_cast<float>(a.light->mv) / static_cast<float>(a.row->mv);
    }
    // A plunge after a deep enough fall (core/vr/plunge.h): the fall is its speed.
    const float plunge_factor =
        a.plunge ? VR::Melee::Plunge::DamageFactor(a.drop_m, PlungeConfig()) : 1.0f;
    if (plunge_factor > 1.0f) {
        factor = std::max(factor, 1.0f);
        LOG_INFO(Core, "VR melee: #{} PLUNGE hit after a {:.2f} m fall: x{:.2f}", a.serial,
                 a.drop_m, plunge_factor);
    }
    float contact_distance = -1.0f;
    float range_factor = 1.0f;
    // Use each enemy's actual collision point. Two enemies hit by the same sweep
    // can therefore take different damage, even with the tip fully extended.
    // The attack's behavior id also identifies its weapon, including upgrade levels.
    const s32 attack_variation = a.bid >= 100000000 ? (a.bid - 100000000) / 1000 : -1;
    if (chain.applies && chain.variation == 2400 && attack_variation == 2400) {
        const B::Vec3 contact{Read<float>(res + ResPosition), Read<float>(res + ResPosition + 4),
                              Read<float>(res + ResPosition + 8)};
        if (B::IsFinite(contact) && B::IsFinite(chain.grip)) {
            contact_distance = B::Length(contact - chain.grip);
            range_factor = VR::Whip::ContactDamageFactor(attack_variation, true, contact_distance);
        }
    }
    const float before = *damage;
    float after = before;
    // The hit's feel (the presenter's impact: its strength from the damage, a knock under 1): the
    // damage before a whip's pace, so every lash that lands feels as it did before whip_pace (the
    // pace is the game's damage per second, not a weaker blow on the hand).
    float felt = before;
    if (std::isfinite(before) && before > 0.0f) {
        felt = before * factor * row_factor * plunge_factor * range_factor;
        after = felt * whip_pace;
        *damage = after;
    }
    const float hitstop = a.row != nullptr ? a.row->hitstop : 0.09f;
    {
        std::scoped_lock lock{g_attack_mutex};
        for (Attack& r : g_attacks) {
            if (r.serial == a.serial && r.created_ns == a.created_ns) {
                ++r.hits;
                r.damage += after;
            }
        }
    }
    g_stats.hits.fetch_add(1, std::memory_order_relaxed);
    g_bite_wall.store(false, std::memory_order_relaxed);
    g_bite_hold.store(std::clamp(hitstop * 0.8f, 0.05f, 0.1f), std::memory_order_relaxed);
    g_bite_ns.store(now, std::memory_order_release);
    Combat::PushMeleeEvent({Combat::MeleeEventKind::Hit, a.serial, felt, hitstop, now, ""});
    const u8* data = Plausible(target) ? Module(target, L::OffsetDataModule) : nullptr;
    LOG_INFO(Core,
             "VR melee: #{} HIT {}{} bid={} atk={} tgt={} tgtHp={} dmg {:.2f}->{:.2f} x{:.3f} (tip "
             "{:.1f} m/s -> speed x{:.2f}, cadence {:.2f} s -> x{:.2f}{}) {:.0f} ms after the "
             "open, t={:.3f}",
             a.serial, a.transform ? "TRANSFORM ATTACK " : "", M::MoveKindName(a.kind), bid,
             Read<s32>(res + ResAtkParam), fmt::ptr(target),
             data != nullptr ? Read<s32>(data + OffsetHp) : -1, before, after,
             factor * row_factor * range_factor * whip_pace,
             tip, cfg.speed ? M::SpeedFactor(tip, cfg) : 1.0f, a.since_counted_s,
             cfg.speed && cfg.cadence
                 ? M::CadenceFactor(a.since_counted_s, M::RecoverSeconds(a.kind))
                 : 1.0f,
             (row_factor != 1.0f ? fmt::format(", judged light x{:.2f}", row_factor)
                                 : std::string{}) +
                 (chain.applies && chain.tip > 0.0f
                      ? fmt::format(", the whip's tip {:.1f} m/s", chain.tip)
                                : std::string{}) +
                 (contact_distance >= 0.0f
                      ? fmt::format(", contact {:.2f} m -> range x{:.2f}", contact_distance, range_factor)
                      : std::string{}) +
                 (whip_paced ? fmt::format(", whip pace x{:.2f}", whip_pace) : std::string{}),
             static_cast<double>(now - a.created_ns) * 1e-6, static_cast<double>(now) * 1e-9);
    return true;
}

bool MeleeHitInfoOf(u8* res, MeleeHitInfo& out) {
    out = {};
    if (!g_installed || res == nullptr) {
        return false;
    }
    if (Read<s32>(res + ResHand) != HandRight || Read<s32>(res + ResBullet) != -1) {
        return false;
    }
    const s64 now = NowNs();
    {
        std::scoped_lock lock{g_attack_mutex};
        if (g_blade.valid && now - g_blade.ns <= BladeTickMaxNs) {
            out.blade = true;
            out.base = g_blade.base;
            out.tip = g_blade.tip;
            out.base_vel = g_blade.base_vel;
            out.tip_vel = g_blade.tip_vel;
            out.blade_ns = g_blade.ns;
        }
    }
    if (!g_physical) {
        return false;
    }
    const s32 bid = Read<s32>(res + ResBid);
    const u32 handle = Read<u32>(res + ResHandle);
    Attack a{};
    {
        std::scoped_lock lock{g_attack_mutex};
        Attack* found = FindAttack(bid, handle, now);
        if (found == nullptr) {
            found = FindAttack(bid, 0, now); // a whip's capsules down the chain (as MeleeDamage)
            if (found != nullptr && !IsWhipRow(found->row)) {
                found = nullptr;
            }
        }
        if (found == nullptr) {
            return false;
        }
        a = *found;
    }
    Combat::MeleeWindow w{};
    const bool have_w = Combat::LatestMeleeWindow(w) && w.serial == a.serial;
    out.weapon_variation = a.weapon_variation;
    out.form_b = a.form_b;
    out.two_handed = std::max(a.two_hand, have_w ? w.two_hand : 1.0f) > 1.0f;
    out.swing = true;
    out.serial = a.serial;
    out.kind = M::MoveKindName(a.kind);
    out.transform = a.transform;
    out.tip_speed = std::max(have_w ? w.tip_speed : 0.0f, a.tip);
    return true;
}

void MeleeBite(VR::Body::ModelTarget& right_target, s64 now_ns) {
    static s64 anchor_for = 0;
    static B::ModelTarget anchor{};
    const s64 at = g_bite_ns.load(std::memory_order_acquire);
    if (at == 0 || !VR::Melee::GetMeleeSettings().bite || now_ns < at || now_ns - at > 400 * Ms) {
        return;
    }
    if (at != anchor_for) {
        anchor_for = at;
        anchor = right_target; // where the blade was when it bit
    }
    const bool wall = g_bite_wall.load(std::memory_order_relaxed);
    const float since = static_cast<float>(now_ns - at) * 1e-9f;
    const float follow =
        wall ? M::BiteFollow(since, 0.06f, 0.12f, 0.25f)
             : M::BiteFollow(since, g_bite_hold.load(std::memory_order_relaxed), 0.08f, 0.5f);
    if (follow >= 1.0f) {
        return;
    }
    right_target.p = B::Lerp(anchor.p, right_target.p, follow);
    right_target.q = B::Slerp(anchor.q, right_target.q, follow);
}


// quick_use_wipe (core/vr/bloodborne_quickitems.cpp): the right weapon's blade as its own hit
// capsule has it, for the paper wiped along it: the first move of the form in the hand (the light
// one; dmy1 -> dmy2, its end nearer the grip the base; a sphere row: from its blade trail's other
// end, else from the grip, to dmy1; a whip's row on the chain: the grip to the chain's handle end),
// both ends on the weapon in the hand (within BladeNearM of its grip). World; false (`how` says
// why) when the weapon or its dummies are not known. Game:Main, as the pose tick that reads it.
bool MeleeBladeSegment(u8* chr, B::Vec3& base, B::Vec3& tip, const char*& how,
                       B::Vec3* grip_out, float* strike_near) {
    constexpr float BladeNearM = 2.0f;
    const TickState& t = g_tick;
    if (!g_installed || chr == nullptr || chr != t.chr || t.moves == nullptr) {
        how = "the right weapon not read";
        return false;
    }
    const M::MoveRow* row = nullptr;
    const std::size_t first = t.form_b ? 3 : 0;
    for (std::size_t k = first; k < first + 3 && row == nullptr; ++k) {
        if (t.moves->rows[k].Valid()) {
            row = &t.moves->rows[k];
        }
    }
    if (row == nullptr) {
        how = t.form_b ? "no hit capsule in form B" : "no hit capsule in form A";
        return false;
    }
    B::Vec3 grip{};
    if (!DummyAt(chr, RightWeaponDummy, grip)) {
        how = "no right grip dummy";
        return false;
    }
    if (grip_out != nullptr) {
        *grip_out = grip;
    }
    const auto on_weapon = [&](s32 id, B::Vec3& p) {
        return id >= 0 && DummyAt(chr, id, p) && B::Length(p - grip) <= BladeNearM;
    };
    if (IsWhipRow(row)) {
        base = grip;
        how = "the whip: the grip to the chain's handle end";
        if (strike_near != nullptr) {
            *strike_near = 0.0f;
        }
        return on_weapon(WhipHandleDummy, tip);
    }
    B::Vec3 a{};
    if (!on_weapon(row->dmy1, a)) {
        how = "the hit capsule's dummy not on the weapon shown";
        return false;
    }
    B::Vec3 b{};
    if (row->dmy2 >= 0 && on_weapon(row->dmy2, b)) {
        const bool a_first = B::Length(a - grip) <= B::Length(b - grip);
        base = a_first ? a : b;
        tip = a_first ? b : a;
        how = "the hit capsule";
        if (strike_near != nullptr) {
            *strike_near = B::Length(base - grip); // the capsule's own near end, not the haft
        }
        // A capsule out at the end of a long haft (the Hunter Axe's form B: its 26 cm head 1.1 m
        // from the grip, out of the left hand's reach; the 2026-10-08 mock measure): from the
        // grip side of the blade trail the game draws along the weapon (the haft) to the
        // capsule's far end, so the paper can be wiped where the hand reaches.
        constexpr float HaftFarM = 0.6f;
        B::Vec3 t0{};
        B::Vec3 t1{};
        if (B::Length(base - grip) > HaftFarM && row->trail_base >= 0 && row->trail_tip >= 0 &&
            on_weapon(row->trail_base, t0) && on_weapon(row->trail_tip, t1)) {
            const B::Vec3 near_end = B::Length(t0 - grip) <= B::Length(t1 - grip) ? t0 : t1;
            if (B::Length(near_end - grip) < B::Length(base - grip) - 0.2f) {
                base = near_end;
                how = "the haft (the blade trail) to the hit capsule";
            }
        }
        return true;
    }
    // A sphere (an axe's head, the wheel): from the trail's other end, else the grip, to it.
    const s32 other = row->trail_base == row->dmy1 ? row->trail_tip : row->trail_base;
    if (other != row->dmy1 && on_weapon(other, b) && B::Length(b - a) >= 0.1f) {
        base = b;
        how = "the hit sphere and its trail";
    } else {
        base = grip;
        how = "the grip to the hit sphere";
    }
    tip = a;
    if (strike_near != nullptr) {
        // A head round the hit sphere: its model reach off dmy1, else a typical 15 cm head.
        *strike_near = std::max(B::Length(a - grip) - (row->blade > 0.0f ? row->blade : 0.15f),
                                0.0f);
    }
    return true;
}

bool MeleeBladeReach(float& base, float& tip) {
    if (!g_blade_reach.valid) {
        return false;
    }
    base = g_blade_reach.base;
    tip = g_blade_reach.tip;
    return true;
}

bool MeleeBowGeometry(const u8* chr, VR::Bow::NativeGeometry& out, bool arrow_present) {
    if (!g_installed || chr != g_tick.chr || g_tick.bow_hand < 0 ||
        g_tick.bow_part < 0 || (arrow_present && g_tick.bow_arrow_part < 0)) return false;
    const auto* parts = Read<const u8*>(chr + OffsetChrAsm);
    if (!Plausible(parts)) return false;
    // WeaponPart +0x158 -> animation state -> player +0x20 -> pose node;
    // the same checked layout used by the physical whip's native part hook.
    const auto bone = [&](int slot, std::string_view wanted, B::QsTransform& result) {
        const auto* part = Read<const u8*>(parts + OffsetAsmParts + 8 * (RightPartSlot + slot));
        if (!Plausible(part) || Read<const u8*>(part) != g_base + WeaponPartVtableRva) return false;
        const auto* anim = Read<const u8*>(part + 0x158);
        const auto* state = Plausible(anim) ? Read<const u8*>(anim) : nullptr;
        const auto* player = Plausible(state) ? Read<const u8*>(state) : nullptr;
        const auto* node = Plausible(player) ? Read<const u8*>(player + 0x20) : nullptr;
        if (!Plausible(node)) return false;
        const auto* pose = node + 0x38;
        const auto* skel = Read<const u8*>(pose);
        if (!Plausible(skel)) return false;
        const int count = Read<s32>(skel + 0x30);
        const auto* names = Read<const u8*>(skel + 0x28);
        const auto* parents = Read<const s16*>(skel + 0x18);
        const auto* locals = Read<const B::HkQsTransform*>(pose + 0x08);
        if (count < 1 || count > 64 || Read<s32>(pose + 0x10) != count ||
            Read<s32>(skel + 0x20) != count || !Plausible(names) ||
            !Plausible(reinterpret_cast<const u8*>(parents), 2) || !Plausible(reinterpret_cast<const u8*>(locals), 16)) return false;
        std::array<B::QsTransform, 64> model{};
        for (int i = 0; i < count; ++i) {
            const int parent = parents[i];
            if (parent < -1 || parent >= i) return false;
            const auto local = B::FromHavok(locals[i]);
            if (!B::IsFinite(local) || B::Dot(local.q, local.q) < 0.5f) return false;
            model[i] = parent >= 0 ? B::Compose(model[parent], local) : local;
            const auto raw = Read<uintptr_t>(names + 16 * i) & ~uintptr_t{1};
            const auto* name = reinterpret_cast<const char*>(raw);
            if (!Plausible(reinterpret_cast<const u8*>(name), 1)) continue;
            std::size_t len = 0;
            while (len < 48 && name[len] != 0) ++len;
            if (std::string_view{name, len} == wanted) {
                if (wanted == "B_String") g_bow_string_rig = {node, skel, count, i};
                result = model[i];
                return true;
            }
        }
        return false;
    };
    B::QsTransform grip{}, string{};
    if (!bone(g_tick.bow_part, "B_Grip", out.bow_root) ||
        !bone(g_tick.bow_part, "B_Bowgrip", grip)) return false;
    out.bow_grip = grip.t;
    // The empty bow must not depend on the arrow part or an active string rig.
    return !arrow_present || (bone(g_tick.bow_part, "B_String", string) &&
                              bone(g_tick.bow_arrow_part, "B_Body", out.arrow_root));
}

void MeleeBowString(B::Vec3 target, s64 ns) {
    g_bow_string_request.Store({g_bow_string_rig, target, ns});
    if (ns > 0) {
        Combat::BowPartNode().store(reinterpret_cast<uintptr_t>(g_bow_string_rig.node),
                                    std::memory_order_release);
    }
}

void MeleeBowPartPose(u8* node) {
    BowStringRequest req{};
    if (!g_bow_string_request.Load(req) || req.rig.node != node) return;
    const auto& r = req.rig;
    auto* const pose = node + 0x38;
    if (Read<const u8*>(pose) != r.skeleton || r.count < 1 || r.count > 64 ||
        r.bone < 0 || r.bone >= r.count || Read<s32>(pose + 0x10) != r.count) return;
    auto* const locals = Read<B::HkQsTransform*>(pose + 0x08);
    const auto* parents = Read<const s16*>(r.skeleton + 0x18);
    auto* const flags = Read<u32*>(pose + 0x28);
    if (!Plausible(reinterpret_cast<const u8*>(locals), 16) || !Plausible(reinterpret_cast<const u8*>(parents), 2) || !Plausible(reinterpret_cast<const u8*>(flags), 4) ||
        Read<s32>(pose + 0x30) != r.count) return;
    const s64 now = NowNs();
    const bool active = req.ns > 0 && now >= req.ns && now - req.ns < 250 * Ms &&
                        VR::Bow::GetBowSettings().enabled;
    // Restore the original translation if animation throttling reused our last write.
    static const u8* last_node{};
    static const u8* last_skeleton{};
    static B::Vec3 original{}, written{};
    static bool edited{};
    auto current = B::FromHavok(locals[r.bone]);
    if (last_node != node || last_skeleton != r.skeleton) {
        last_node = node;
        last_skeleton = r.skeleton;
        edited = false;
    }
    const bool reused = edited && B::LengthSq(current.t - written) < 1e-12f;
    if (!active) {
        if (reused) { current.t = original; B::ToHavok(current, locals[r.bone]); }
        edited = false;
        Combat::BowPartNode().store(0, std::memory_order_release);
        if (!reused) return;
    } else {
        if (!reused) original = current.t;
        std::array<B::QsTransform, 64> local{};
        for (int i = 0; i < r.count; ++i) local[i] = B::FromHavok(locals[i]);
        if (!VR::Bow::MoveString(local.data(), parents, r.count, r.bone, req.target)) return;
        current.t = local[r.bone].t;
        B::ToHavok(current, locals[r.bone]);
        written = current.t;
        edited = true;
    }
    // Same local-authoritative state as hkaPose::setPoseLocalSpace. The original
    // hook tail calls syncModelSpace immediately after this callback.
    for (int i = 0; i < r.count; ++i) flags[i] = 2;
    const u16 dirty = 0x0100;
    std::memcpy(pose + 0x38, &dirty, sizeof(dirty));
}

s32 MeleePartLine(int hand, B::Vec3& base, B::Vec3& tip, bool& seen) {
    seen = false;
    if (hand < 0 || hand > 1) {
        return -1;
    }
    const PartBlade& blade = g_part_blade[hand];
    seen = blade.seen;
    base = blade.base;
    tip = blade.tip;
    return blade.key;
}

// quick_use_wipe's measurement (the wipe's log at the take): the light move's blade trail (its
// SpawnFFX_Blade dummies: the game draws the swing's trail between them, along the blade you
// see), both on the weapon in the hand. False when the row has none or they are not shown.
bool MeleeBladeTrail(u8* chr, B::Vec3& a, B::Vec3& b, s32& id_a, s32& id_b) {
    constexpr float BladeNearM = 2.0f;
    const TickState& t = g_tick;
    if (!g_installed || chr == nullptr || chr != t.chr || t.moves == nullptr) {
        return false;
    }
    const M::MoveRow* row = nullptr;
    const std::size_t first = t.form_b ? 3 : 0;
    for (std::size_t k = first; k < first + 3 && row == nullptr; ++k) {
        if (t.moves->rows[k].Valid()) {
            row = &t.moves->rows[k];
        }
    }
    B::Vec3 grip{};
    if (row == nullptr || row->trail_base < 0 || row->trail_tip < 0 ||
        !DummyAt(chr, RightWeaponDummy, grip)) {
        return false;
    }
    id_a = row->trail_base;
    id_b = row->trail_tip;
    return DummyAt(chr, id_a, a) && DummyAt(chr, id_b, b) && B::Length(a - grip) <= BladeNearM &&
           B::Length(b - grip) <= BladeNearM;
}

} // namespace VR::Bloodborne
