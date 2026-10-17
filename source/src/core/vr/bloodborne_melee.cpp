// SPDX-License-Identifier: GPL-2.0-or-later

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
#include "core/vr/bow.h"
#include "core/vr/bow_channel.h"
#include "core/vr/bow_pose.h"
#include "core/vr/bow_string_cleanup.h"
#include "core/vr/combat_channel.h"
#include "core/vr/feature_cost.h"
#include "core/vr/hook_util.h"
#include "core/vr/interaction_bow_string.h"
#include "core/vr/interaction_native_attack.h"
#include "core/vr/interaction_native_geometry.h"
#include "core/vr/interaction_native_melee.h"
#include "core/vr/interaction_native_saw.h"
#include "core/vr/interaction_native_skeleton.h"
#include "core/vr/melee_settings.h"
#include "core/vr/melee_window.h"
#include "core/vr/plunge.h"
#include "core/vr/solid_weapon.h"
#include "core/vr/stake_driver.h"
#include "core/vr/transform_settings.h"
#include "core/vr/trick_weapons.h"
#include "core/vr/two_hand_grip.h"
#include "core/vr/visceral_channel.h"
#include "core/vr/visceral_thrust.h"
#include "core/vr/vr_session.h"
#include "core/vr/whip_damage.h"
#include "core/vr/whip_spin.h"

namespace VR::Bloodborne {

namespace {

namespace B = VR::Body;
namespace L = VR::Bloodborne::Layout;
namespace M = VR::Melee;
using Hook::Plausible;
using Hook::Read;

// Module-relative addresses in the 1.09 eboot (patch file address - 0x400000). Every prologue is
// unique in .text (combat/gunfire/uniq.py, scratchpad/melee/sigs.py).

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
TryThrowAllFn g_try_throw = nullptr;
PlaySoundFn g_play_sound = nullptr;
BladeFfxFn g_blade_ffx = nullptr;
DmyPolyFn g_dmy_poly = nullptr;
const u8* g_sound_table = nullptr;
std::unique_ptr<Xbyak::CodeGenerator> g_code;
// VR_MELEE_LOG: 1 = + the capsule's first frames of each window, 2 = + every frame of it.
int g_log = 0;
// Native saw uses its context only on the pose caller. NativeMelee and NativeGeometry are
// stateless: callers read the immutable table without entering that context or sharing state.
VR::Interactions::Module* g_native_saw_module{};
VR::Interactions::NativeAttackRuntime g_native_attack_runtime{};
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
PiWhipPaceState g_whip_pace{}; // (g_attack_mutex)

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
namespace FlickPhase {
constexpr u32 Old = PI_AF_OLD, Gap = PI_AF_GAP, Live = PI_AF_LIVE, Done = PI_AF_DONE;
}

struct TickState {
    const u8* chr{nullptr};
    s64 last_ns{0};
    u32 updates{0};
    // The weapon (every few updates).
    s32 variation{-1};
    const M::WeaponMoves* moves{nullptr};
    s32 style{-1};
    u32 style_stable{0}; // updates the style has kept its value
    bool form_b{false};
    const char* weapon_why{"not read yet"};
    // The window being served.
    VR::Interactions::NativeLatch latch{g_native_attack_runtime, 1};
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
    VR::Interactions::NativeFlick flick{g_native_attack_runtime};
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
    VR::Interactions::NativeSwingBuffer buffer{g_native_attack_runtime};
    u32 buffer_logged{0};
    // The plunge (melee_plunge): the hunter in the air, from the motion signals.
    VR::Interactions::NativeAir air{g_native_attack_runtime};
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
// One Game:Main native part-pose owner, separate from the body and physical-chain contexts.
VR::Interactions::Module* g_bow_string_module{};
VR::Interactions::BowString g_bow_string_policy{};
VR::Bow::StringCleanup g_bow_string_cleanup{};
bool g_bow_string_fault{}, g_bow_string_dll{};
const u8* g_bow_string_last_node{};
const u8* g_bow_string_last_skeleton{};
u64 g_bow_string_identity{};
bool InitBowStringDll() {
    if (!VR::Interactions::DllRequested())
        return true;
    g_bow_string_dll = true;
    if (g_bow_string_module)
        return !g_bow_string_fault;
    g_bow_string_module = new VR::Interactions::Module();
    g_bow_string_module->EnableFaultBroadcast();
    if (!g_bow_string_module->LoadBesideExecutable()) {
        g_bow_string_fault = true;
        LOG_ERROR(Core, "VR bow: string DLL unavailable: {}", g_bow_string_module->Diagnostic());
        return false;
    }
    g_bow_string_policy.Bind(*g_bow_string_module, g_bow_string_fault);
    return true;
}

// Two pose ticks further apart than this give no blade velocity ...
constexpr s64 BladeTickMaxNs = 100 * Ms;
// ... nor two with the play space turned more than this between them (a snap turn; a smooth turn
// at the default 120 degrees a second is 2 to 4 degrees an update).

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
constexpr s32 StakeDriverVariation = 1100;
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
    static M::WeaponMoves stake{}; // native-side approved row storage, not a judge cache
    PiNativeAttackInput input{};
    input.operation = PI_ATTACK_STAKE_ROWS;
    auto& facts = input.decision.stake;
    facts.command = PI_SR_BEGIN;
    facts.variation = variation;
    facts.have_moves = moves != nullptr;
    facts.charged_valid = moves && moves->rows[5].Valid();
    facts.heavy_valid = moves && moves->rows[4].Valid();
    facts.enabled = VR::Tricks::GetTrickSettings().stake_blast;
    facts.geometry = g_stake_geometry;
    std::string kinds;
    for (int stage = 0; stage < 4; ++stage) {
        const auto packet = g_native_attack_runtime.Call(input);
        if (!packet.valid)
            return nullptr;
        const auto& result = packet.stake;
        if (result.query) {
            const s32 judge = result.query_judge;
            const s32 kind = GunBehaviorRefType(100000000 + variation * 1000 + judge);
            kinds += fmt::format(" {}: {}", judge,
                                 kind < 0    ? "no row"
                                 : kind == 0 ? "an attack"
                                             : "a bullet");
            facts.command = PI_SR_REPLY;
            facts.token = result.token;
            facts.reply_judge = judge;
            facts.kind = kind;
            continue;
        }
        if (result.build && moves) {
            stake = *moves;
            auto& row = stake.rows[result.target_row];
            row = moves->rows[result.source_row];
            row.judge = static_cast<s16>(result.judge);
            row.charge = result.charge;
            if (result.patch_stats) {
                row.stamina = static_cast<s16>(result.stamina);
                row.mv = static_cast<s16>(result.mv);
                row.radius = result.radius;
                row.dmy1 = result.dmy1;
                row.dmy2 = result.dmy2;
            }
        }
        if (result.log)
            LOG_INFO(Core, "VR melee: the Stake Driver's transformed charged R2 rows:{} -> {}",
                     kinds,
                     result.use ? fmt::format("the charged swing opens judge {}", result.judge)
                                : std::string{"no attack row (a bullet is the gun module's)"});
        return result.use ? &stake : moves;
    }
    if (g_native_saw_module)
        g_native_saw_module->ReportFault();
    return nullptr;
}

void ReadWeapon(u8* chr, TickState& t) {
    PiNativeAttackInput input{};
    input.operation = PI_ATTACK_WEAPON;
    auto& facts = input.decision.weapon;
    facts.command = PI_NW_ID;
    facts.bid = BehaviorId(chr, 0);
    const auto fail = [&] {
        t.variation = t.style = -1;
        t.style_stable = 0;
        t.form_b = false;
        t.moves = nullptr;
        t.bow_hand = -1;
        t.bow_arrow_part = -1;
        t.bow_arrow_logged = false;
        t.weapon_why = PhysicalInteractions::NativeWeaponReasons[PI_NWR_MODULE];
    };
    auto result = g_native_attack_runtime.Call(input);
    if (!result.valid) {
        fail();
        return;
    }
    t.variation = result.weapon.variation;
    facts.request_id = result.weapon.request_id;
    t.moves = t.variation >= 0 ? M::FindMoves(t.variation) : nullptr;
    // whip=physical: a whip form's rows on its simulated chain (core/vr/bloodborne_whip.h).
    t.moves = WhipMovesFor(t.variation, t.moves);
    t.moves = StakeMovesFor(t.variation, t.moves);
    facts.command = PI_NW_STYLE;
    facts.style = WeaponStyle(chr);
    result = g_native_attack_runtime.Call(input);
    if (!result.valid) {
        fail();
        return;
    }
    t.style = result.weapon.style;
    t.style_stable = result.weapon.stable_frames;
    t.form_b = result.weapon.form_b;
    // The DLL approves the bow form before the host resolves its native hand/part dummies.
    t.bow_hand = result.weapon.bow_eligible ? BowHandOf(chr, t) : -1;
    if (t.bow_hand < 0) {
        t.bow_arrow_part = -1;
        t.bow_arrow_logged = false;
    }
    facts.command = PI_NW_ELIGIBILITY;
    facts.have_moves = t.moves != nullptr;
    if (t.moves)
        for (int i = 0; i < 6; ++i)
            if (t.moves->rows[i].Valid())
                facts.valid_rows |= 1u << i;
    result = g_native_attack_runtime.Call(input);
    if (!result.valid) {
        fail();
        return;
    }
    t.weapon_why = PhysicalInteractions::NativeWeaponReasons[result.weapon.reason];
}

// The live dmy1 -> dmy2 distance of a row (0: a sphere row; -1: a dummy not found).
float SpanNow(u8* chr, const M::MoveRow& r) {
    PiNativeGeometryInput input{};
    input.operation = PI_NG_SPAN;
    input.dmy2 = r.dmy2;
    B::Vec3 a{}, b{};
    if (r.dmy2 >= 0 && DummyAt(chr, r.dmy1, a) && DummyAt(chr, r.dmy2, b)) {
        input.flags = PI_NGF_HAVE;
        PhysicalInteractions::AttackPackets::Pack(input.state.a, a);
        PhysicalInteractions::AttackPackets::Pack(input.point, b);
    }
    PiNativeGeometryOutput output{};
    return VR::Interactions::NativeGeometry(g_native_saw_module, input, output) && output.valid
               ? output.measured_span
               : -1.f;
}

// The capsule radius a physical swing gives the row (core/vr/melee_window.h SwingRadius:
// melee_radius_scale, the floor, the blade fit while the part has the model's shape).
float RowRadius(u8* chr, const M::MoveRow& r, bool* fitted = nullptr) {
    const auto& ms = VR::Melee::GetMeleeSettings();
    PiNativeGeometryInput input{};
    input.operation = PI_NG_RADIUS;
    input.scale = ms.radius_scale;
    input.fit = ms.radius_fit;
    input.margin = ms.radius_margin_cm * .01f;
    input.row_radius = r.radius;
    input.blade = r.blade;
    input.span = r.span;
    input.span_now = SpanNow(chr, r);
    PiNativeGeometryOutput result{};
    if (!VR::Interactions::NativeGeometry(g_native_saw_module, input, result) || !result.valid) {
        if (fitted)
            *fitted = false;
        return r.radius;
    }
    if (fitted)
        *fitted = result.fitted;
    const float out = result.radius;
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
    PiNativeMeleeInput p{};
    auto& g = p.gate;
    g.body = body.body; g.reason = static_cast<u32>(body.reason); g.driven = body.driven;
    g.menu = VR::IsGameMenuOpen(); g.cinematic = VR::CinematicActive();
    g.attack_wait = body.attack_wait; g.dodge_attack = body.dodge_attack;
    g.dodge_cancel = t.dodge_cancel; g.latch_active = t.latch.State().active; g.latch_serial = t.latch.State().serial;
    g.right_ik = body.right_ik; g.right_weight = body.right_weight; g.held = body.held; g.owner_id = body.owner_id;
    PiNativeMeleeOutput result{};
    const auto call = [&](u32 operation) {
        p.operation = operation;
        return VR::Interactions::NativeMelee(g_native_saw_module, p, result) && result.valid;
    };
    const auto text = [&]() { return result.reason == 9 ? AttackWaitWhy : PhysicalInteractions::NativeMeleeReasons[result.reason]; };
    if (!call(PI_NATIVE_MELEE_GATE_BODY)) return PhysicalInteractions::NativeMeleeReasons[17];
    if (result.reason) return text();
    const char* transform = TransformSwingBlock();
    g.transform_block = transform != nullptr; g.weapon_block = t.weapon_why != nullptr;
    g.flick_stroke = flick_stroke; g.moves = t.moves != nullptr; g.style_stable = t.style_stable; g.known_style = known_style;
    if (!call(PI_NATIVE_MELEE_GATE_WEAPON)) return PhysicalInteractions::NativeMeleeReasons[17];
    if (result.reason == 13) return transform;
    if (result.reason == 14) return t.weapon_why;
    if (result.reason) return text();
    g.attack_type = Read<s32>(chr + OffsetAttackType); g.style = t.style;
    const u8* hks = HksState(chr);
    g.input_bits = hks != nullptr ? Read<u16>(hks + OffsetInputBits) : u16{0};
    if (!call(PI_NATIVE_MELEE_GATE_HAND)) return PhysicalInteractions::NativeMeleeReasons[17];
    if (result.reason) return text();
    const u8* data = Module(chr, L::OffsetDataModule);
    g.have_data = data != nullptr; g.hp = data ? Read<s32>(data + OffsetHp) : 1;
    if (!call(PI_NATIVE_MELEE_GATE_HP)) return PhysicalInteractions::NativeMeleeReasons[17];
    return text();
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
    Attack attack{};
    bool live{false};
};
SawAttackState g_saw{};

// Dedicated serial native pose owner; no calls from XR, damage hooks or part-pose contexts.

VR::Interactions::NativeSaw g_native_saw_policy{};
bool g_native_saw_fault{}, g_native_saw_dll{};
u64 g_native_saw_owner{};
bool InitNativeSawDll() {
    if (!VR::Interactions::DllRequested()) return true;
    g_native_saw_dll = true;
    if (g_native_saw_module) return !g_native_saw_fault;
    g_native_saw_module = new VR::Interactions::Module();
    g_native_saw_module->EnableFaultBroadcast();
    if (!g_native_saw_module->LoadBesideExecutable()) {
        g_native_saw_fault = true;
        LOG_ERROR(Core, "VR saw: native policy DLL unavailable: {}", g_native_saw_module->Diagnostic());
        return false;
    }
    g_native_saw_policy.Bind(*g_native_saw_module, g_native_saw_fault);
    g_native_attack_runtime.Bind(*g_native_saw_module);
    return true;
}

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
        ++g_native_saw_owner; // opaque native owner generation, never an address in the DLL
    }
    Combat::SawRequest request{};
    PiNativeSawInput input{};
    input.operation = PI_NATIVE_SAW_BEGIN;
    input.owner = g_native_saw_owner;
    input.now_ns = now;
    input.have_request = Combat::LatestSawRequest(request);
    input.request_ns = request.ns;
    input.rev = request.rev;
    input.held = request.held;
    input.enabled = Tr::GetTrickSettings().saw_spin;
    input.physical = g_physical;
    input.variation = t.variation;
    input.form_b = t.form_b;
    input.gate_clear = gate == nullptr;
    input.body = body.body;
    input.transforming = TransformRun().active;
    input.dt = dt;
    input.dry_run = g_dry_run;
    input.regen = g_regen;
    u8* module = nullptr;
    u8* data = nullptr;
    s32 bid = -1;
    const M::MoveRow* row = nullptr;
    for (int step = 0; step < 8; ++step) {
        PiNativeSawOutput output{};
        if (!g_native_saw_policy.Step(input, output)) break;
        if (output.flags & PI_NATIVE_SAW_END) EndSawAttack(now);
        bool pose_ok = true;
        if (output.flags & PI_NATIVE_SAW_WRITE_POSE) {
            pose_ok = SetSawWeaponPose(chr, output.pose_time,
                                      (output.flags & PI_NATIVE_SAW_POSE_ACTIVE) != 0);
        }
        if (output.reason == 1) {
            LOG_INFO(Core, "VR saw: stopped (no stamina; release the trigger to restart)");
        } else if (output.reason == 2) {
            LOG_WARNING(Core, "VR saw: native grinding row {} unavailable", row->judge);
        }
        if (output.flags & PI_NATIVE_SAW_REGISTER) {
            Attack a{};
            a.serial = output.serial;
            a.bid = bid;
            const u32 slot = FirstSlot + (a.serial & 1);
            a.handle = Read<u32>(module + OffsetAttackSlots + slot * 8);
            a.row = row;
            a.saw = true;
            a.created_ns = now;
            a.since_counted_s = Tr::SawHitS;
            g_saw.attack = a;
            g_saw.live = true;
            {
                std::scoped_lock lock{g_attack_mutex};
                g_attacks[g_attack_next++ % g_attacks.size()] = a;
            }
            LOG_DEBUG(Core, "VR saw: grinding judge {} bid {} handle {:#x}", row->judge, bid, a.handle);
        }
        if (output.flags & PI_NATIVE_SAW_REFRESH_ATTACK) {
            const Attack& a = g_saw.attack;
            Invoke(chr, module, a.bid, FirstSlot + (a.serial & 1), a.row->stamina, M::SwingStamina{});
        }
        if (output.flags & PI_NATIVE_SAW_PAUSE_REGEN) PauseRegen(chr);
        if (!output.next) return output.owns != 0;
        input = {};
        input.operation = output.next;
        switch (output.next) {
        case PI_NATIVE_SAW_POSE:
            input.success = pose_ok;
            break;
        case PI_NATIVE_SAW_GEOMETRY: {
            module = Module(chr, OffsetAttackModule);
            data = Module(chr, L::OffsetDataModule);
            B::Vec3 blade{}, grip{};
            input.success = module != nullptr && data != nullptr && DummyAt(chr, 12100, blade) &&
                            DummyAt(chr, RightWeaponDummy, grip);
            PhysicalInteractions::AttackPackets::Pack(input.blade, blade);
            PhysicalInteractions::AttackPackets::Pack(input.grip, grip);
            break;
        }
        case PI_NATIVE_SAW_STAMINA:
            input.stamina = Read<s32>(data + OffsetStamina);
            break;
        case PI_NATIVE_SAW_BEHAVIOR:
            row = &SawRows[output.row];
            bid = BehaviorId(chr, row->judge);
            input.success = bid >= 0 && GunBehaviorRefType(bid) == 0;
            break;
        case PI_NATIVE_SAW_INVOKE:
            input.success = !g_dry_run && Invoke(chr, module, bid,
                FirstSlot + (output.serial & 1), row->stamina, M::SwingStamina{}) != 0;
            break;
        case PI_NATIVE_SAW_REFRESH: {
            const Attack& a = g_saw.attack;
            const u32 slot = FirstSlot + (a.serial & 1);
            input.success = Read<u32>(module + OffsetAttackSlots + slot * 8) == a.handle &&
                            HitEntry(a.handle, a.bid) != nullptr;
            break;
        }
        default: break;
        }
    }
    // Fatal transport/peer failure: stop this host-owned attack; never retry legacy policy.
    g_native_saw_fault = true;
    if (g_native_saw_module) g_native_saw_module->ReportFault();
    EndSawAttack(now);
    SetSawWeaponPose(chr, 0.0f, false);
    return true;
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
    PiNativeMeleeInput policy{};
    policy.operation = PI_NATIVE_MELEE_FINALIZE;
    auto& f = policy.finish;
    f.hits = a.hits;
    f.replaced = replaced;
    f.have_window = w != nullptr;
    f.serial = a.serial;
    if (w) {
        f.window_serial = w->serial;
        f.window_aborted = w->aborted;
        f.prev_serial = w->prev_serial;
        f.prev_aborted = w->prev_aborted;
    }
    f.verdict_light = a.verdict_light;
    f.have_light = a.light != nullptr;
    f.have_row = a.row != nullptr;
    f.cost = a.cost;
    f.light_stamina = a.light ? a.light->stamina : 0;
    f.row_stamina = a.row ? a.row->stamina : 0;
    f.light_stamina_pct = a.light_stamina_pct;
    PiNativeMeleeOutput decision{};
    const bool approved =
        VR::Interactions::NativeMelee(g_native_saw_module, policy, decision) &&
        decision.valid;
    const bool aborted = approved && decision.aborted;
    if (aborted)
        g_stats.aborted.fetch_add(1, std::memory_order_relaxed);
    if (approved && decision.refund_reason) {
        constexpr const char* reasons[] = {
            nullptr, "the stroke became the transform attack, nothing hit yet",
            "the swing never committed and hit nothing",
            "a wound-up swing judged light, nothing hit"};
        if (decision.refund_reason <= 3)
            Refund(chr, decision.refund, a.serial, reasons[decision.refund_reason]);
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
    PiNativeMeleeInput p{};
    p.operation = PI_NATIVE_MELEE_FX_SELECT;
    p.fx.form_b = t.form_b;
    p.fx.whoosh = r.whoosh;
    p.fx.ffx = r.trail_ffx;
    p.fx.base_id = r.trail_base;
    p.fx.tip_id = r.trail_tip;
    p.fx.group = r.Group();
    p.fx.dmy2 = r.dmy2;
    PiNativeMeleeOutput o{};
    if (VR::Interactions::NativeMelee(g_native_saw_module, p, o) && o.valid) {
        t.whoosh = o.whoosh;
        t.trail_ffx = o.ffx;
        t.trail_base = o.base_id;
        t.trail_tip = o.tip_id;
    } else {
        t.whoosh = t.trail_ffx = t.trail_base = t.trail_tip = -1;
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

// Row pointers never cross the DLL boundary: the policy selects an index in copied
// validity facts, which the pose owner resolves against its immutable native row table.
M::MoveChoice ChooseAttackMove(const M::WeaponMoves& moves, bool form_b, M::MoveKind want) {
    PiNativeAttackInput p{};
    p.operation = PI_ATTACK_CHOOSE;
    p.decision.move.form_b = form_b;
    p.decision.move.want = static_cast<u32>(want);
    for (size_t i = 0; i < moves.rows.size(); ++i)
        p.decision.move.valid_rows[i] = moves.rows[i].Valid();
    const auto o = g_native_attack_runtime.Call(p);
    if (!o.valid || o.decision.row_index < 0)
        return {};
    return {&moves.rows[o.decision.row_index], static_cast<M::MoveKind>(o.decision.kind),
            o.decision.lighter != 0};
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
    const auto* fallback_light =
        light ? light
              : (t.moves ? ChooseAttackMove(*t.moves, t.form_b, M::MoveKind::Light).row : nullptr);
    PiNativeAttackInput selection{};
    selection.operation = PI_ATTACK_AFFORDABLE;
    selection.decision.admit.have_row = row != nullptr;
    selection.decision.admit.have_light = fallback_light != nullptr;
    selection.decision.admit.kind = static_cast<u32>(kind);
    selection.decision.admit.lighter = lighter;
    selection.decision.admit.row_stamina = row ? row->stamina : 0;
    selection.decision.admit.stamina = data ? Read<s32>(data + OffsetStamina) : 0;
    const auto selected = g_native_attack_runtime.Call(selection);
    if (!selected.valid)
        return false;
    const M::MoveChoice affordable{selected.decision.use_light ? fallback_light : row,
                                   static_cast<M::MoveKind>(selected.decision.kind),
                                   selected.decision.lighter != 0};
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
    PiNativeAttackInput admission{};
    admission.operation = PI_ATTACK_ADMIT;
    auto& facts = admission.decision.admit;
    facts.have_row = row != nullptr;
    facts.kind = static_cast<u32>(kind);
    facts.light_pct = VR::Melee::GetMeleeSettings().stamina_pct;
    facts.regen_pause = VR::Melee::GetMeleeSettings().regen_pause;
    facts.have_module = module != nullptr;
    facts.have_data = data != nullptr;
    facts.bid = bid;
    facts.have_dummy = have_dmy;
    facts.have_grip = have_grip;
    facts.dummy[0] = dmy.x;
    facts.dummy[1] = dmy.y;
    facts.dummy[2] = dmy.z;
    facts.grip[0] = grip.x;
    facts.grip[1] = grip.y;
    facts.grip[2] = grip.z;
    facts.near_m = near_m;
    // Query native stamina only after the earlier module/row/geometry gates pass.
    facts.stamina = INT32_MAX;
    auto admitted = g_native_attack_runtime.Call(admission);
    if (!admitted.valid)
        return false;
    if (admitted.decision.proceed) {
        facts.stamina = Read<s32>(data + OffsetStamina);
        admitted = g_native_attack_runtime.Call(admission);
        if (!admitted.valid)
            return false;
    }
    const auto& rule = admitted.decision;
    const M::SwingStamina stamina_rule{rule.pct, rule.charge != 0, rule.refuse_empty != 0,
                                       rule.regen_pause != 0};
    constexpr const char* refusal_text[] = {nullptr,
                                            "no behaviour module",
                                            "no row",
                                            "the hit dummy is not on the weapon in hand",
                                            "the hit dummy is not near the hand",
                                            "no stamina"};
    const char* refuse = refusal_text[admitted.decision.reason];
    if (refuse != nullptr && soft) {
        LOG_INFO(Core,
                 "VR melee: #{} {} not opened in the old form: {} (variation {} style {} judge {}); "
                 "the transform attack serves the stroke",
                 w.serial, M::MoveKindName(want), refuse, t.variation, t.style, judge);
        return false;
    }
    if (refuse != nullptr) {
        g_stats.denied.fetch_add(1, std::memory_order_relaxed);
        t.latch.End();
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
    const auto plunge_cfg = PlungeConfig();
    PiNativeAttackInput outcome{};
    outcome.operation = PI_ATTACK_OPEN_RESULT;
    outcome.now_ns = now;
    outcome.config = VR::Interactions::AP::Pack(plunge_cfg);
    auto& opened = outcome.decision.opened;
    opened.variation = t.variation;
    opened.judge = judge;
    opened.form_b = t.form_b;
    opened.kind = static_cast<u32>(kind);
    opened.transform = transform;
    opened.powered = w.powered;
    opened.ok = ok != 0;
    opened.dry_run = g_dry_run;
    opened.verdict_known = w.verdict_known;
    opened.verdict = w.verdict;
    opened.stamina_before = stamina_before;
    opened.stamina_after = stamina_after;
    opened.two_hand = w.two_hand;
    opened.regen_available = g_regen;
    opened.regen_pause = stamina_rule.regen_pause;
    const auto result = g_native_attack_runtime.Call(outcome);
    if (!result.valid) {
        t.latch.End();
        return false;
    }
    const auto& approved = result.decision;
    const bool stake_native = approved.stake;
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
        PiNativeGeometryInput guard{};
        guard.operation = PI_NG_RADIUS_GUARD;
        guard.flags = stake_native ? PI_NGF_STAKE : 0;
        guard.current_radius = radius_before;
        guard.row_radius = row->radius;
        PiNativeGeometryOutput allowed{};
        if (VR::Interactions::NativeGeometry(g_native_saw_module, guard, allowed) &&
            allowed.valid && allowed.success) {
            PiNativeGeometryInput patch{};
            patch.operation = PI_NG_RADIUS_PATCH;
            patch.flags = IsWhipRow(row) ? PI_NGF_WHIP : 0;
            patch.current_radius = radius_before;
            patch.desired_radius = RowRadius(chr, *row);
            PiNativeGeometryOutput result{};
            if (VR::Interactions::NativeGeometry(g_native_saw_module, patch, result) &&
                result.valid && (result.write_flags & PI_NGW_RADIUS)) {
                radius_after = result.radius;
                std::memcpy(entry + OffsetEntryRadius, &radius_after, sizeof(radius_after));
            }
        }
    }
    (ok != 0 ? g_stats.created : g_stats.failed).fetch_add(1, std::memory_order_relaxed);
    if (ok != 0) {
        DodgeAttackCut(t, w.serial, "opened");
    }
    Attack a{};
    a.serial = w.serial;
    a.bid = bid;
    a.handle = handle;
    a.kind = kind;
    a.row = row;
    a.light = light;
    a.transform = transform;
    a.powered = approved.powered;
    a.cost = approved.cost;
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
    a.two_hand = approved.two_hand;
    a.verdict_light = approved.verdict_light;
    a.plunge = approved.plunge;
    a.drop_m = approved.drop_m;
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
    t.cur_live = approved.live;
    t.unpaid_live = false;
    t.cur_frames = 1;
    t.whoosh_done = false;
    t.last_a = {};
    SetSwingFx(t, *row);
    if (approved.regen_pause) {
        t.regen_left = approved.regen_seconds;
    }
    if (approved.end_latch) {
        t.latch.End();
    }
    Combat::PushMeleeEvent(
        {approved.live ? Combat::MeleeEventKind::Opened : Combat::MeleeEventKind::Denied, w.serial,
         static_cast<float>(a.cost), static_cast<float>(static_cast<u8>(kind)), now,
         approved.live ? "" : "the game made no attack"});
    if (approved.discharge) {
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
             StaminaText(stamina_rule, row->stamina, full_cost), row->mv, radius_before,
             radius_after >= 0.0f ? radius_after : radius_before, t.variation,
             w.committed ? "" : "(onset)", t.form_b ? "B" : "A", t.style, arm, w.tip_speed, since_s,
             row->dmy1, have_grip ? B::Length(dmy - grip) * 1000.0f : -1.0f,
             BudgetText(w, transform || t.flick.State().window == w.serial),
             g_dry_run ? " DRY RUN (no attack)" : "");
    return approved.live;
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
            t.flick.Gap(); // no old-form swing: straight to the transform attack
            return;
        }
        t.latch.End();
        return;
    }
    const auto want = static_cast<M::MoveKind>(std::min<u8>(w.kind, 2));
    const M::MoveChoice choice = ChooseAttackMove(*t.moves, t.form_b, want);
    const M::MoveRow* light = ChooseAttackMove(*t.moves, t.form_b, M::MoveKind::Light).row;
    if (w.unpaid) {
        // The combo budget had no charge for this swing: it does not register. A flick's stroke
        // goes straight to its transform attack (transforms never need charges).
        if (flick) {
            t.flick.Gap();
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
    if (flick && !made && t.latch.State().active) {
        t.flick.Gap(); // the transform attack serves the whole stroke
    }
}

// ---- A flick mid-swing (core/vr/melee_window.h FlickJoins) --------------------------------------

const char* FlickPhaseName(u32 p) {
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

void FlickNotice(TickState& t, const PiNativeAttackOutput& o, bool switched = false) {
    if (!o.valid)
        return;
    const auto& d = o.flick;
    const auto& f = t.flick.State();
    constexpr const char* reasons[] = {"",
                                       "a newer transform",
                                       "the transform stopped before its switch",
                                       "the window closed before the new form was in the hand",
                                       "the new form has no physical move",
                                       "the new form's hit dummy never came to the hand"};
    if (d.flags & PI_AFO_LOG_DONE) {
        LOG_INFO(Core, "VR melee: #{} flick of transform #{}: no transform attack ({}; was {})",
                 f.window, f.serial, reasons[d.reason <= PI_AFR_NO_PART ? d.reason : 0],
                 FlickPhaseName(d.prior_phase));
    } else if (d.reason == PI_AFR_ENDED) {
        LOG_INFO(Core,
                 "VR melee: #{} flick of transform #{}: no transform attack (the stroke ended {}; "
                 "was {})",
                 f.window, f.serial,
                 switched ? "before the new form was in the hand" : "before the switch",
                 FlickPhaseName(d.prior_phase));
    } else if (d.reason == PI_AFR_OLD_ABORTED) {
        LOG_INFO(Core,
                 "VR melee: #{} transform #{} stopped before its switch: the stroke goes on in the "
                 "old form",
                 f.window, f.serial);
    }
    if (d.flags & PI_AFO_END_LATCH)
        t.latch.End();
}

void FlickTick(u8* chr, TickState& t, const Combat::MeleeWindow& w, bool have,
               const TransformRunInfo& run, s64 now) {
    const auto& ts = VR::Transform::GetTransformSettings();
    PiAttackFlickFacts facts{};
    facts.command = PI_AF_TICK;
    facts.window = w.serial;
    facts.run = run.id;
    facts.serial = run.serial;
    facts.style = run.style;
    facts.cur_serial = t.cur.serial;
    facts.open_ns = w.open_ns;
    facts.close_ns = w.close_ns;
    facts.press_ns = run.start_ns;
    facts.flags = (have ? PI_AF_HAVE : 0) | (w.powered ? PI_AF_POWERED : 0) |
                  (w.open ? PI_AF_OPEN : 0) | (run.active ? PI_AF_ACTIVE : 0) |
                  (run.switched ? PI_AF_SWITCHED : 0) | (run.no_join ? PI_AF_NO_JOIN : 0) |
                  (run.flick ? PI_AF_RUN_FLICK : 0) | (run.aborted ? PI_AF_ABORTED : 0) |
                  (run.switch_asked ? PI_AF_SWITCH_ASKED : 0) |
                  (g_physical && ts.flick ? PI_AF_ENABLED : 0) | (t.cur_live ? PI_AF_CUR_LIVE : 0) |
                  (run.to_b ? PI_AF_TO_B : 0);
    const auto decision = t.flick.Call(facts, now);
    if (!decision.valid)
        return;
    const auto& f = t.flick.State();
    if (decision.flick.flags & PI_AFO_MAKE_FLICK)
        TransformMakeFlick(run.id, w.serial);
    if (decision.flick.flags & PI_AFO_JOINED) {
        const M::MoveRow* row =
            ts.attack && t.variation >= 0 ? M::FindTransformMove(t.variation, run.to_b) : nullptr;
        LOG_INFO(Core,
                 "VR melee: #{} the stroke joins transform #{} ({}, {}; window {} {:.0f} ms "
                 "{} the squeeze): {}",
                 w.serial, run.serial, run.to_b ? "A->B" : "B->A",
                 run.switched       ? "switched"
                 : run.switch_asked ? "switching"
                                    : "before its switch",
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
    if (decision.flick.flags & PI_AFO_FINALIZE) {
        const Attack old =
            Finalize(chr, t, &w, "the form switches: the transform attack takes over", now, true);
        PiAttackFlickFacts reply{};
        reply.command = PI_AF_OLD_RESULT;
        reply.window = f.window;
        reply.run = f.run;
        reply.old_hits = old.hits;
        reply.old_created_ns = old.created_ns;
        t.flick.Call(reply, now);
    }
    FlickNotice(t, decision, run.switched);
}

// The stroke's attack after the switch (the latch's create or refresh while the hand-off is under
// way): the transform attack, or the new form's own row, once the new form's part is in the hand.
void FlickOpen(u8* chr, TickState& t, const Combat::MeleeWindow& w, float arm, s64 now) {
    const auto& f = t.flick.State();
    const TransformRunInfo& run = TransformRun();
    PiAttackFlickFacts gate{};
    gate.command = PI_AF_OPEN_GATE;
    gate.run = run.id;
    gate.style = t.style;
    gate.flags = (run.aborted ? PI_AF_ABORTED : 0) | (run.switched ? PI_AF_SWITCHED : 0) |
                 (w.open ? PI_AF_OPEN : 0) | (t.cur_live ? PI_AF_CUR_LIVE : 0);
    const auto opened = t.flick.Call(gate, now);
    FlickNotice(t, opened);
    if (!opened.valid || !(opened.flick.flags & PI_AFO_PROCEED))
        return;
    const auto want = static_cast<M::MoveKind>(std::min<u8>(w.kind, 2));
    const bool attack = VR::Transform::GetTransformSettings().attack;
    const M::MoveRow* row =
        attack && t.variation >= 0 ? M::FindTransformMove(t.variation, f.to_b) : nullptr;
    const bool transform = row != nullptr;
    M::MoveKind kind = want;
    bool lighter = false;
    const M::MoveRow* light = nullptr;
    if (row == nullptr && t.moves != nullptr) {
        const M::MoveChoice choice = ChooseAttackMove(*t.moves, t.form_b, want);
        row = choice.row;
        kind = choice.kind;
        lighter = choice.lighter;
        light = ChooseAttackMove(*t.moves, t.form_b, M::MoveKind::Light).row;
    }
    PiAttackFlickFacts part{};
    part.command = PI_AF_PART;
    part.flags = (row ? PI_AF_HAVE_ROW : 0) | (t.cur_live ? PI_AF_CUR_LIVE : 0);
    part.since_s = w.since_counted_s;
    part.near_m = FlickNearM;
    const bool whip = row && IsWhipRow(row) && row->Group() == WhipHandleDummy / 1000;
    B::Vec3 dmy{}, grip{};
    if (row) {
        const bool have_dummy = DummyAt(chr, whip ? WhipHandleDummy : row->dmy1, dmy);
        const bool have_grip = have_dummy && DummyAt(chr, RightWeaponDummy, grip);
        part.flags |= (have_dummy ? PI_AF_HAVE_DUMMY : 0) | (have_grip ? PI_AF_HAVE_GRIP : 0);
        part.dummy[0] = dmy.x;
        part.dummy[1] = dmy.y;
        part.dummy[2] = dmy.z;
        part.grip[0] = grip.x;
        part.grip[1] = grip.y;
        part.grip[2] = grip.z;
    }
    const auto ready = t.flick.Call(part, now);
    FlickNotice(t, ready);
    if (!ready.valid || !(ready.flick.flags & PI_AFO_PROCEED))
        return;
    const float since = ready.flick.since_s;
    OpenAttack(chr, t, w, arm, now, row, kind, want, lighter, light, transform, since,
               whip ? WhipNearM : FlickNearM);
    PiAttackFlickFacts result{};
    result.command = PI_AF_OPEN_RESULT;
    result.window = f.window;
    result.run = f.run;
    result.budget_used = w.budget_used;
    result.flags = t.cur_live ? PI_AF_CUR_LIVE : 0;
    const auto accepted = t.flick.Call(result, now);
    if (accepted.valid && (accepted.flick.flags & PI_AFO_MARK_FLICK)) {
        t.cur.flick = true;
        {
            std::scoped_lock lock{g_attack_mutex};
            for (Attack& r : g_attacks) {
                if (r.serial == t.cur.serial && r.created_ns == t.cur.created_ns) {
                    r.flick = true;
                }
            }
        }
        if (accepted.flick.flags & PI_AFO_REFUND_BUDGET) {
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
    }
}

void Refresh(u8* chr, TickState& t, const Combat::MeleeWindow& w, s64 now) {
    u8* const module = Module(chr, OffsetAttackModule);
    PiNativeAttackInput begin{};
    begin.operation = PI_ATTACK_REFRESH_BEGIN;
    begin.decision.refresh.have_module = module != nullptr;
    begin.decision.refresh.live = t.cur_live;
    begin.decision.refresh.unpaid_live = t.unpaid_live;
    begin.decision.refresh.have_row = t.cur.row != nullptr;
    begin.decision.refresh.dry_run = g_dry_run;
    const auto refresh = g_native_attack_runtime.Call(begin);
    if (!refresh.valid || !refresh.decision.proceed) {
        return;
    }
    const bool unpaid = !t.cur_live; // the combo budget's: no attack to keep alive
    const u32 slot = FirstSlot + (t.cur.serial & 1);
    u8 ok = 1;
    if (refresh.decision.invoke) {
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
    const auto metadata = [&](const Attack& attack) {
        PiNativeAttackInput p{};
        p.operation = PI_ATTACK_REFRESH_ROW;
        p.now_ns = now;
        p.config = VR::Interactions::AP::Pack(PlungeConfig());
        auto& f = p.decision.refresh;
        f.serial = attack.serial;
        f.window_serial = w.serial;
        f.verdict_known = w.verdict_known;
        f.verdict = w.verdict;
        f.kind = static_cast<u32>(attack.kind);
        f.transform = attack.transform;
        f.verdict_light = attack.verdict_light;
        f.plunge = attack.plunge;
        f.tip = attack.tip;
        f.two_hand = attack.two_hand;
        f.window_tip = w.tip_speed;
        f.window_two_hand = w.two_hand;
        f.drop_m = attack.drop_m;
        return g_native_attack_runtime.Call(p);
    };
    const auto current = metadata(t.cur);
    if (!current.valid)
        return;
    t.cur.verdict_light = current.decision.verdict_light;
    if (!unpaid) {
        std::scoped_lock lock{g_attack_mutex};
        for (Attack& r : g_attacks) {
            if (r.serial == t.cur.serial && r.created_ns == t.cur.created_ns) {
                const auto updated = metadata(r);
                if (!updated.valid)
                    return;
                r.verdict_light = updated.decision.verdict_light;
                if (updated.decision.update_window) {
                    r.tip = updated.decision.tip;
                    r.two_hand = updated.decision.two_hand;
                }
                if (r.plunge) {
                    r.drop_m = updated.decision.drop_m;
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
        PiNativeMeleeInput p{};
        p.operation = PI_NATIVE_MELEE_FX_RESOLVE;
        auto& x = p.fx;
        x.have_base = have_base;
        x.have_tip = have_tip;
        x.have_grip = have_grip;
        x.near_m = near_m;
        x.base[0] = base.x;
        x.base[1] = base.y;
        x.base[2] = base.z;
        x.tip[0] = tip.x;
        x.tip[1] = tip.y;
        x.tip[2] = tip.z;
        x.grip[0] = grip.x;
        x.grip[1] = grip.y;
        x.grip[2] = grip.z;
        x.ffx = t.trail_ffx;
        x.base_id = t.trail_base;
        x.tip_id = t.trail_tip;
        x.group = t.cur.row->Group();
        x.dmy2 = t.cur.row->dmy2;
        PiNativeMeleeOutput o{};
        if (!VR::Interactions::NativeMelee(g_native_saw_module, p, o) || !o.valid)
            return;
        if (o.requery) {
            const s32 old_base = t.trail_base;
            const s32 old_tip = t.trail_tip;
            t.trail_base = o.base_id;
            t.trail_tip = o.tip_id;
            t.trail_ffx = o.ffx;
            if (g_log >= 1) {
                LOG_INFO(Core, "VR melee: #{} trail dummies {} / {} not on the part; {} / {} (FFX {})",
                         t.cur.serial, old_base, old_tip, t.trail_base, t.trail_tip, t.trail_ffx);
            }
            have_base = DummyAt(chr, t.trail_base, base);
            have_tip = DummyAt(chr, t.trail_tip, tip);
        }
    }
    PiNativeMeleeInput fx{};
    fx.operation = PI_NATIVE_MELEE_FX_EMIT;
    fx.fx.committed = w.committed;
    fx.fx.whoosh_done = t.whoosh_done;
    fx.fx.whoosh_enabled = ms.whoosh;
    fx.fx.trail_enabled = ms.trail;
    fx.fx.trail_available = g_trail;
    fx.fx.dry_run = g_dry_run;
    fx.fx.have_base = have_base;
    fx.fx.have_tip = have_tip;
    fx.fx.base[0] = base.x;
    fx.fx.base[1] = base.y;
    fx.fx.base[2] = base.z;
    fx.fx.tip[0] = tip.x;
    fx.fx.tip[1] = tip.y;
    fx.fx.tip[2] = tip.z;
    PiNativeMeleeOutput effects{};
    if (!VR::Interactions::NativeMelee(g_native_saw_module, fx, effects) || !effects.valid)
        return;
    t.whoosh_done = effects.whoosh_done;
    if (effects.emit_whoosh) {
        const B::Vec3 at{effects.sound_at[0], effects.sound_at[1], effects.sound_at[2]};
        const u64 h = Sound(t.whoosh, at);
        if (g_log >= 1) {
            LOG_INFO(Core, "VR melee: #{} whoosh {} at {} (h={:#x}), trail FFX {} {} -> {}{}",
                     t.cur.serial, t.whoosh, V3(at), h, t.trail_ffx, t.trail_base, t.trail_tip,
                     have_base && have_tip ? "" : " (no dummies: no trail)");
        }
    }
    if (effects.emit_trail) {
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
    PiNativeMeleeOutput wall{};
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
        PiNativeMeleeInput input{}; input.operation = PI_NATIVE_MELEE_WALL;
        input.damage.whip = IsWhipRow(a->row); input.walls = a->walls;
        if (!VR::Interactions::NativeMelee(g_native_saw_module,input,wall) || !wall.valid) return 0;
        report = wall.report; a->walls = wall.walls;
        a->last_wall_ns = now;
    }
    if (report) {
        g_stats.walls.fetch_add(1, std::memory_order_relaxed);
        if (wall.bite_wall) {
            // (A whip's chain metres out on the floor does not catch the hand: the clank only.)
            g_bite_wall.store(true, std::memory_order_relaxed);
            g_bite_hold.store(wall.bite_hold, std::memory_order_relaxed);
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
    return wall.suppress_bounce; // the game retains sparks and sound
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
// or plunge of its own. A handgun in the left hand (Tricks::PunchGun) punches with the bare fist's
// learned row, the gun in the hand: the first such attack must show its hitbox at the left fist
// (CheckGunPunch), else gun punches stay off. A window that closed without its swing (no commit,
// or the presenter dropped it as the off hand's counter-motion) and hit nothing gives its stamina
// back; one closed so before the game ever saw it open makes no attack. A refusal is sent back
// (LeftDenied: the deny tick, the charge back).
constexpr u32 LeftFirstSlot = 4;
// All recognition decisions run on the existing pose-owned native attack context.
PiNativeAttackOutput LeftPolicy(const PiAttackLeftFacts& facts) {
    PiNativeAttackInput p{};
    p.operation = PI_ATTACK_LEFT;
    p.decision.left = facts;
    return g_native_attack_runtime.Call(p);
}

struct LeftTick {
    VR::Interactions::NativeLeftRows rows{g_native_attack_runtime};
    // Published under the native attack mutex for the concurrent damage hook.
    bool gun{false};
    VR::Interactions::NativeLatch latch{g_native_attack_runtime, 0};
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
    PiAttackLeftRowsFacts f{};
    f.command = PI_ALR_RESTORE;
    f.learned = VR::Tricks::LeftRowOf(LearnedLeftRows(), l.rows.State().key);
    const auto o = l.rows.Call(f);
    if (o.valid && (o.rows.flags & PI_ALRO_RESTORED)) {
        const auto& row = l.rows.State();
        LOG_INFO(Core, "VR melee left: judge {} {}", row.judges[row.next],
                 o.rows.flags & PI_ALRO_KNOWN ? "known: the game's own left-hand attack"
                                              : "remembered (melee_left_rows)");
    }
}

// The weapon's attack rows (refType 0) its left hand may use: the right weapon's transformed
// form's (judges 400 .. 999) that are not the right hand's own (the table's) nor its B->A trick
// attack (790) or a shot (800 ..); a left weapon's L2 (800 ..) first, then its others (0 .. 399).
void ScanLeftRows(LeftTick& l, const M::WeaponMoves* moves) {
    PiAttackLeftRowsFacts f{};
    f.command = PI_ALR_SCAN_BEGIN;
    if (moves) {
        f.flags = PI_ALR_MOVES;
        for (size_t i = 0; i < moves->rows.size(); ++i)
            f.right_judges[i] = moves->rows[i].judge;
    }
    auto o = l.rows.Call(f);
    for (u32 queries = 0; o.valid && (o.rows.flags & PI_ALRO_QUERY); ++queries) {
        if (queries >= 600) {
            if (g_native_saw_module)
                g_native_saw_module->ReportFault();
            return;
        }
        PiAttackLeftRowsFacts reply{};
        reply.command = PI_ALR_SCAN_REPLY;
        reply.judge = o.rows.query_judge;
        reply.ref_type =
            GunBehaviorRefType(100000000 + l.rows.State().variation * 1000 + reply.judge);
        o = l.rows.Call(reply);
    }
    if (!o.valid)
        return;
    const auto& rows = l.rows.State();
    std::string list;
    for (u32 i = 0; i < rows.count; ++i)
        list += fmt::format(" {}", rows.judges[i]);
    LOG_INFO(Core, "VR melee left: variation {} ({}): native attack rows to observe:{}",
             rows.variation,
             l.gun                   ? "a handgun's punch: the fist's, learned with the hand empty"
             : rows.hand == HandLeft ? "the left weapon"
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
    PiNativeMeleeInput p{};
    p.operation = PI_NATIVE_MELEE_FINALIZE;
    p.finish.left = 1;
    p.finish.refund = refund;
    p.finish.hits = hits;
    p.finish.cost = l.paid;
    PiNativeMeleeOutput o{};
    if (VR::Interactions::NativeMelee(g_native_saw_module, p, o) && o.valid &&
        o.refund_reason == 4)
        Refund(chr, o.refund, l.serial, "a left swing that never came and hit nothing");
    l.paid = 0;
}

void LeftOpen(u8* chr, LeftTick& l, const Combat::MeleeWindow& w, s64 now) {
    u8* const module = Module(chr, OffsetAttackModule);
    u8* const data = Module(chr, L::OffsetDataModule);
    const s32 judge = l.rows.State().judges[l.rows.State().next];
    // A gun's punch: the fist's row itself (the game's id for the left hand is the gun's).
    const s32 bid = l.gun ? 100000000 + VR::Tricks::FistVariation * 1000 + judge
                          : BehaviorId(chr, judge, l.rows.State().hand);
    PiAttackLeftFacts admission{};
    admission.command = PI_AL_ADMIT;
    admission.hand = l.rows.State().hand;
    admission.bid = bid;
    admission.light_pct = VR::Melee::GetMeleeSettings().stamina_pct;
    admission.stamina = INT32_MAX;
    admission.flags = (l.rows.State().verified ? PI_AL_VERIFIED : 0) | (l.gun ? PI_AL_GUN : 0) |
                      (w.unpaid ? PI_AL_UNPAID : 0) | (module ? PI_AL_MODULE : 0) |
                      (data ? PI_AL_DATA : 0) |
                      (VR::Melee::GetMeleeSettings().regen_pause ? PI_AL_REGEN : 0);
    auto approved = LeftPolicy(admission);
    if (!approved.valid)
        return;
    if (approved.left.flags & PI_ALO_PROCEED) {
        admission.stamina = Read<s32>(data + OffsetStamina);
        approved = LeftPolicy(admission);
        if (!approved.valid)
            return;
    }
    const auto& decision = approved.left;
    const M::SwingStamina rule{decision.pct, decision.charge != 0, decision.refuse_empty != 0,
                               decision.regen_pause != 0};
    const bool probe = (decision.flags & PI_ALO_PROBE) != 0;
    constexpr const char* reasons[] = {nullptr,
                                       "no observed left-hand row",
                                       "combo budget unpaid",
                                       "no behaviour module",
                                       "no row",
                                       "no stamina"};
    const char* refuse = reasons[decision.reason];
    if (refuse != nullptr) {
        l.latch.End();
        Combat::PushMeleeEvent({Combat::MeleeEventKind::LeftDenied, w.serial, 0.0f, 0.0f, now,
                                refuse});
        LOG_INFO(Core, "VR melee left: #{} refused: {} (judge {})", w.serial, refuse, judge);
        return;
    }
    const u32 slot = LeftFirstSlot + (w.serial & 1);
    PiAttackLeftRowsFacts begin{};
    begin.command = PI_ALR_BEGIN_OPEN;
    if (!l.rows.Call(begin).valid)
        return;
    const s32 before = Read<s32>(data + OffsetStamina);
    const u8 ok = g_dry_run ? 0 : Invoke(chr, module, bid, slot, l.rows.State().cost, rule);
    const u32 handle = Read<u32>(module + OffsetAttackSlots + slot * 8);
    const s32 after = Read<s32>(data + OffsetStamina);
    PiAttackLeftFacts result{};
    result.command = PI_AL_OPEN_RESULT;
    result.stamina = before;
    result.after = after;
    result.whoosh = l.rows.State().whoosh;
    result.flags = (ok ? PI_AL_OK : 0) | (g_dry_run ? PI_AL_DRY_RUN : 0) |
                   (VR::Melee::GetMeleeSettings().whoosh ? PI_AL_WHOOSH : 0);
    const auto outcome = LeftPolicy(result);
    if (!outcome.valid)
        return;
    {
        std::scoped_lock lock{g_attack_mutex};
        l.live = outcome.left.live;
        l.serial = w.serial;
        l.bid = bid;
        l.slot = slot;
        l.handle = handle;
        l.frames = 0;
        l.paid = outcome.left.paid;
        l.hits = 0;
        l.created_ns = now;
        l.ended_ns = 0;
    }
    LOG_INFO(Core, "VR melee left: #{} open judge {} bid {} slot {} ok={} stamina {} -> {}{}",
             w.serial, judge, bid, slot, ok, before, after,
             l.gun   ? " (a punch with the gun in the hand)"
             : probe ? fmt::format(" (trying it: candidate {} of {})", l.rows.State().next + 1,
                                   l.rows.State().count)
                     : std::string{});
    if (outcome.left.flags & PI_ALO_DENY) {
        // As the right hand's Denied: the deny tick, the charge back.
        Combat::PushMeleeEvent({Combat::MeleeEventKind::LeftDenied, w.serial, 0.0f, 0.0f, now,
                                "the game made no attack"});
    }
    // The dual blades' left blade whooshes as the right one does (the user, 2026-10-14): the
    // weapon's own light swing sound, once as its attack opens, at the left hand.
    B::Vec3 at{};
    if ((outcome.left.flags & PI_ALO_SOUND) && DummyAt(chr, LeftWeaponDummy, at)) {
        const u64 h = Sound(l.rows.State().whoosh, at);
        LOG_INFO(Core, "VR melee left: #{} whoosh {} at {} (h={:#x})", w.serial,
                 l.rows.State().whoosh, V3(at), h);
    }
}

// The first punch with a gun in the hand: the fist's row with another weapon in that hand is
// untested, so its hitbox must show up at the left fist. Else it ends now (its stamina back unless
// it hit) and gun punches stay off (logged); with the fists together, the hitbox between them or
// not built yet, ask again next update.
// The DLL first approves dummy queries from the copied native hit entry. Preserve
// short-circuit left/right reads; probing never invokes a candidate to discover its side.
PiAttackLeftFacts LeftHitFacts(u8* chr, const LeftTick& l) {
    PiAttackLeftFacts facts{};
    facts.command = PI_AL_ENTRY;
    facts.frames = l.frames;
    facts.next = l.rows.State().next;
    facts.count = l.rows.State().count;
    facts.unseen = l.rows.State().unseen;
    facts.check = l.rows.State().check;
    if (l.gun)
        facts.flags |= PI_AL_GUN;
    if (const u8* entry = HitEntry(l.handle, l.bid)) {
        facts.flags |= PI_AL_ENTRY_PRESENT;
        facts.point[0] = Read<float>(entry + OffsetEntryA);
        facts.point[1] = Read<float>(entry + OffsetEntryA + 4);
        facts.point[2] = Read<float>(entry + OffsetEntryA + 8);
    }
    const auto query = LeftPolicy(facts);
    if (query.valid && (query.left.flags & PI_ALO_QUERY_DUMMIES)) {
        B::Vec3 left{}, right{};
        if (DummyAt(chr, LeftWeaponDummy, left)) {
            facts.flags |= PI_AL_HAVE_LEFT;
            facts.left[0] = left.x;
            facts.left[1] = left.y;
            facts.left[2] = left.z;
            if (DummyAt(chr, RightWeaponDummy, right)) {
                facts.flags |= PI_AL_HAVE_RIGHT;
                facts.right[0] = right.x;
                facts.right[1] = right.y;
                facts.right[2] = right.z;
            }
        }
    }
    return facts;
}
void CheckGunPunch(u8* chr, LeftTick& l, s64 now) {
    auto facts = LeftHitFacts(chr, l);
    facts.command = PI_AL_PUNCH;
    const auto o = LeftPolicy(facts);
    if (!o.valid)
        return;
    if (o.left.reason == 1) {
        LOG_INFO(Core,
                 "VR melee left: #{} a punch with the gun in the hand: no hitbox seen in {} "
                 "updates: gun punches off for this session",
                 l.serial, l.frames);
    } else if (o.left.reason == 2) {
        LOG_INFO(Core,
                 "VR melee left: #{} the first hit {} (check {}): its hitbox {:.2f} m from the "
                 "left fist, {:.2f} m from the right: {}",
                 l.serial, l.gun ? "of a punch with a gun or shield in the hand" : "of a known row",
                 l.rows.State().check, o.left.to_left, o.left.to_right,
                 o.left.flags & PI_ALO_GOOD ? "at the left hand, on"
                                            : "NOT at the left hand, off for this session");
    }
    if (o.left.flags & PI_ALO_FINALIZE)
        LeftFinalize(chr, l, true,
                     o.left.reason == 1 ? "the gun punch showed no hitbox"
                                        : "the gun punch's hitbox is not at the hand",
                     now);
}

// A left part's row being tried (VR::Tricks::ProbeStep): its hitbox at the left fist, learned and
// kept in vr.ini (melee_left_rows); at the right one, ended now (its stamina back unless it hit)
// and the next candidate opens within the same swing; its side unseen, ended, tried again next
// swing, passed over after two such swings.
void CheckLeftProbe(u8* chr, LeftTick& l, s64 now) {
    namespace Tr = VR::Tricks;
    auto facts = LeftHitFacts(chr, l);
    facts.command = PI_AL_PROBE;
    const s32 judge = l.rows.State().judges[l.rows.State().next];
    const u32 old_next = l.rows.State().next;
    facts.flags |= PI_AL_MODEL;
    const auto o = LeftPolicy(facts);
    if (!o.valid)
        return;
    const float to_left = o.left.to_left, to_right = o.left.to_right;
    switch (static_cast<Tr::Probe>(o.left.probe)) {
    case Tr::Probe::Wait:
        return;
    case Tr::Probe::Learned:
        LearnedLeftRows() = Tr::WithLeftRow(LearnedLeftRows(), l.rows.State().key, judge);
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
        return;
    case Tr::Probe::Unseen:
        LeftFinalize(chr, l, true, "its hitbox's side unseen", now);
        if (o.left.next != old_next) {
            LOG_INFO(Core, "VR melee left: judge {}: its side never showed: the next one", judge);
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
    PiAttackLeftFacts facts{};
    facts.command = PI_AL_REFRESH;
    facts.check = l.rows.State().check;
    const auto refresh = [&](bool after_probe) {
        facts.flags = (l.live ? PI_AL_LIVE : 0) | (module ? PI_AL_MODULE : 0) |
                      (l.rows.State().verified ? PI_AL_VERIFIED : 0) |
                      (l.rows.State().probing ? PI_AL_PROBING : 0) |
                      (g_dry_run ? PI_AL_DRY_RUN : 0) | (after_probe ? PI_AL_GAME_FACTS : 0);
        return LeftPolicy(facts);
    };
    auto decision = refresh(false);
    if (!decision.valid)
        return;
    if (decision.left.flags & PI_ALO_END) {
        LeftEnd(l, "no verified attack", now);
        return;
    }
    if (decision.left.flags & PI_ALO_PROBE) {
        CheckLeftProbe(chr, l, now);
        if (!l.live) {
            return; // not refreshed: the game ends it this update
        }
    }
    decision = refresh(true);
    if (!decision.valid)
        return;
    if (decision.left.flags & PI_ALO_CHECK_PUNCH) {
        CheckGunPunch(chr, l, now);
        if (!l.live) {
            return; // not refreshed: the game ends it this update
        }
    }
    if (decision.left.flags & PI_ALO_INVOKE) {
        g_invoke(module, l.bid, 0, l.slot, 1);
    }
    ++l.frames;
}

// Read only: slots 0..2 belong to the game's animations: a left weapon's row (a torch's, the
// fists') shows in the game's own L2 attack. (The right weapon's left part learns from your first
// swings instead, CheckLeftProbe: physical swings never let the game show one; one shown anyway
// is learned here too.)
void ObserveLeftRow(u8* chr, LeftTick& l) {
    PiAttackLeftFacts admission{};
    admission.command = PI_AL_OBSERVE_GATE;
    auto intent = LeftPolicy(admission);
    if (!intent.valid || !(intent.left.flags & PI_ALO_QUERY_DUMMIES))
        return;
    u8* const module = Module(chr, OffsetAttackModule);
    B::Vec3 left{};
    B::Vec3 right{};
    const bool have_left = module != nullptr && DummyAt(chr, LeftWeaponDummy, left);
    const bool have_right = have_left && DummyAt(chr, RightWeaponDummy, right);
    admission.flags = PI_AL_GAME_FACTS | (module ? PI_AL_MODULE : 0) |
                      (have_left ? PI_AL_HAVE_LEFT : 0) | (have_right ? PI_AL_HAVE_RIGHT : 0);
    PhysicalInteractions::AttackPackets::Pack(admission.left, left);
    PhysicalInteractions::AttackPackets::Pack(admission.right, right);
    intent = LeftPolicy(admission);
    if (!intent.valid || !(intent.left.flags & PI_ALO_QUERY_GAME))
        return;
    for (u32 slot = 0; slot < 3; ++slot) {
        const u8* const native = module + OffsetAttackSlots + slot * 8;
        if (Read<u8>(native + 4) == 0) {
            continue; // not refreshed by a native attack this update
        }
        for (u32 i = 0; i < l.rows.State().count; ++i) {
            const s32 bid = BehaviorId(chr, l.rows.State().judges[i], l.rows.State().hand);
            u8* const entry = bid >= 0 ? HitEntry(Read<u32>(native), bid) : nullptr;
            if (entry == nullptr) {
                continue;
            }
            const B::Vec3 a{Read<float>(entry + OffsetEntryA),
                            Read<float>(entry + OffsetEntryA + 4),
                            Read<float>(entry + OffsetEntryA + 8)};
            PiAttackLeftFacts facts{};
            facts.command = PI_AL_OBSERVE;
            facts.flags = PI_AL_ENTRY_PRESENT | PI_AL_HAVE_LEFT | PI_AL_HAVE_RIGHT;
            facts.point[0] = a.x;
            facts.point[1] = a.y;
            facts.point[2] = a.z;
            facts.left[0] = left.x;
            facts.left[1] = left.y;
            facts.left[2] = left.z;
            facts.right[0] = right.x;
            facts.right[1] = right.y;
            facts.right[2] = right.z;
            const auto observed = LeftPolicy(facts);
            if (observed.valid && (observed.left.flags & PI_ALO_LEARN)) {
                PiAttackLeftRowsFacts observed_row{};
                observed_row.command = PI_ALR_OBSERVE;
                observed_row.index = i;
                if (!l.rows.Call(observed_row).valid)
                    return;
                LearnedLeftRows() = VR::Tricks::WithLeftRow(LearnedLeftRows(), l.rows.State().key,
                                                            l.rows.State().judges[i]);
                VR::SaveSetting("melee_left_rows", LearnedLeftRows());
                LOG_INFO(Core,
                         "VR melee left: learned judge {} from native slot {} (kept in vr.ini: "
                         "melee_left_rows)",
                         l.rows.State().judges[i], slot);
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
    PiAttackLeftFacts selection{};
    selection.command = PI_AL_SELECT;
    selection.variation = t.variation;
    selection.style = t.style;
    selection.flags = t.form_b ? PI_AL_FORM_B : 0;
    auto selected = LeftPolicy(selection);
    if (!selected.valid)
        return;
    if (selected.left.flags & PI_ALO_QUERY_LEFT) {
        selection.bid = BehaviorId(chr, 0, HandLeft);
        VR::Body::BodyStatus body_status{};
        selection.flags |= PI_AL_LEFT_FACTS;
        if (VR::Body::LatestBodyStatus(body_status))
            selection.flags |= PI_AL_BODY_STATUS;
        selection.family = body_status.left_weapon_family;
        selected = LeftPolicy(selection);
        if (!selected.valid)
            return;
    }
    const s32 variation = selected.left.variation, hand = selected.left.hand,
              punch = selected.left.punch;
    const bool gun = selected.left.gun;
    PiAttackLeftRowsFacts config{};
    config.command = PI_ALR_CONFIG;
    config.variation = variation;
    config.hand = hand;
    config.punch = punch;
    config.flags = gun ? PI_ALR_GUN : 0;
    const auto configured = l.rows.Call(config);
    if (!configured.valid)
        return;
    if (configured.rows.flags & PI_ALRO_CHANGED) {
        LeftEnd(l, "another weapon", now);
        l.latch.End();
        {
            std::scoped_lock lock{g_attack_mutex};
            l.gun = gun;
        }
        if (variation >= 0) {
            ScanLeftRows(l, hand == HandRight ? t.moves : nullptr);
            RestoreLeftRow(l);
            PiAttackLeftRowsFacts cost{};
            cost.command = PI_ALR_COST;
            if (hand == HandRight && t.moves) {
                if (const M::MoveRow* light =
                        ChooseAttackMove(*t.moves, true, M::MoveKind::Light).row;
                    light) {
                    cost.flags = PI_ALR_LIGHT;
                    cost.cost = light->stamina;
                    cost.whoosh = light->whoosh;
                }
            }
            if (!l.rows.Call(cost).valid)
                return;
        }
    }
    if (g_physical && Tr::GetTrickSettings().melee_left && !l.gun) {
        ObserveLeftRow(chr, l); // (a gun's punch is never shown: its trigger shoots)
    }
    const TransformRunInfo& run = TransformRun();
    PiAttackLeftFacts gate{};
    gate.command = PI_AL_GATE;
    gate.variation = variation;
    gate.next = l.rows.State().next;
    gate.count = l.rows.State().count;
    gate.hand = l.rows.State().hand;
    gate.check = l.rows.State().check;
    gate.weight = body.left_weight;
    gate.flags = (Tr::GetTrickSettings().melee_left ? PI_AL_ENABLED : 0) |
                 (g_physical ? PI_AL_PHYSICAL : 0) | (l.gun ? PI_AL_GUN : 0) |
                 (l.rows.State().verified ? PI_AL_VERIFIED : 0) | (body.body ? PI_AL_BODY : 0) |
                 (body.driven ? PI_AL_DRIVEN : 0) | (run.active ? PI_AL_RUN : 0) |
                 (body.left_ik ? PI_AL_LEFT_IK : 0);
    auto allowed = LeftPolicy(gate);
    if (!allowed.valid)
        return;
    if (allowed.left.flags & PI_ALO_QUERY_GAME) {
        gate.flags |= PI_AL_GAME_FACTS;
        if (VR::IsGameMenuOpen())
            gate.flags |= PI_AL_MENU;
        else if (VR::CinematicActive())
            gate.flags |= PI_AL_CINEMATIC;
        allowed = LeftPolicy(gate);
        if (!allowed.valid)
            return;
    }
    constexpr const char* reasons[] = {
        nullptr,
        "melee_left=0",
        "melee=canned",
        "no left-hand blade or weapon",
        "no left-hand row found",
        "a punch with the gun: the fist's punch is not learned yet (punch once with the hand "
        "empty)",
        "waiting to observe a native left attack",
        "its first hit was not at the left hand (off for this session)",
        "menu",
        "cinematic",
        "body off",
        "a transform is running",
        "the game has the left arm"};
    const char* why = reasons[allowed.left.reason];
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
    if (have && w.serial != l.latch.State().serial && l.live) {
        // A newer window replaced the live one before an update saw it close (the latch just stops
        // refreshing it): finished here, its stamina back when it was no swing.
        LeftFinalize(chr, l, w.prev_serial == l.serial && w.prev_aborted, "a new swing", now);
    }
    switch (l.latch.Step(li)) {
    case M::LatchStep::Create:
        LeftOpen(chr, l, w, now);
        break;
    case M::LatchStep::Refresh:
        LeftRefresh(chr, l, now);
        if (l.rows.State().reprobe && !l.live && have && w.open && w.serial == l.serial &&
            why == nullptr) {
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
        if (!l.latch.State().active) {
            LeftEnd(l, "over", now);
        }
        break;
    }
    status.left_ok = why == nullptr;
    status.left_why = why != nullptr ? why : "";
    status.left_learning = variation >= 0 && g_physical && Tr::GetTrickSettings().melee_left &&
                           !l.rows.State().verified && !l.gun;
    status.left_handled = l.latch.State().serial;
    status.left_dual = variation >= 0 && hand == HandRight;
    status.left_gun = l.gun;
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
    if (!InitBowStringDll() || !InitNativeSawDll()) {
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
    if (body.hands_reused) return;
    B::Vec3 base{}, tip{}, grip{};
    const char* how = nullptr;
    PiNativeAttackInput input{};
    input.operation = PI_ATTACK_BLADE;
    input.valid = MeleeBladeSegment(chr, base, tip, how, &grip);
    input.now_ns = now; input.yaw = body.base_yaw;
    input.variation = t.variation; input.style = t.style;
    PhysicalInteractions::AttackPackets::Pack(input.base, base);
    PhysicalInteractions::AttackPackets::Pack(input.tip, tip);
    PhysicalInteractions::AttackPackets::Pack(input.grip, grip);
    const auto result = g_native_attack_runtime.Call(input);
    if (!result.valid || result.clear_reach) {
        g_blade_reach.valid = false;
        return;
    }
    if (result.reach_valid) g_blade_reach = {true, result.reach_base, result.reach_tip};
    if (result.publish) {
        namespace P = PhysicalInteractions::AttackPackets;
        std::scoped_lock lock{g_attack_mutex};
        g_blade = {true, P::V(result.base), P::V(result.tip), P::V(result.base_velocity), P::V(result.tip_velocity), now};
    }
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
            PiNativeGeometryInput input{};
            input.operation = PI_NG_PART;
            input.hand = hand;
            // Four attachments, each with a two-point shortcut and 22-point fallback.
            // The DLL requests only the reads reached by the original short-circuit flow.
            for (int stage = 0; stage < 104; ++stage) {
                PiNativeGeometryOutput result{};
                if (!VR::Interactions::NativeGeometry(g_native_saw_module, input, result) ||
                    !result.valid)
                    break;
                if (result.done) {
                    if (result.success) {
                        found = 10 + result.state.part_index;
                        blade.seen = true;
                        blade.base = {result.state.base[0], result.state.base[1],
                                      result.state.base[2]};
                        blade.tip = {result.state.tip[0], result.state.tip[1], result.state.tip[2]};
                    }
                    break;
                }
                input.operation = PI_NG_PART_REPLY;
                input.state = result.state;
                input.reply_id = result.state.query;
                input.flags = 0;
                if (result.state.phase == 10) {
                    input.attachment = attachment(result.state.query);
                } else {
                    B::Vec3 world{};
                    if (DummyAt(chr, result.state.query, world)) {
                        const auto point = InFrameCm(grip, world) * 0.01f;
                        PhysicalInteractions::AttackPackets::Pack(input.point, point);
                        input.flags = PI_NGF_HAVE;
                    }
                }
                if (stage == 103 && g_native_saw_module)
                    g_native_saw_module->ReportFault();
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
    if (g_native_saw_dll && (g_native_saw_fault ||
                            VR::Interactions::ProcessFaultLatch().Tripped())) {
        EndSawAttack(NowNs());
        SetSawWeaponPose(chr, 0.0f, false);
        return;
    }
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
        const auto latch = t.latch;
        g_native_attack_runtime.OwnerReset();
        t = {};
        t.chr = chr;
        t.last_ns = now;
        t.latch = latch;
        t.latch.End();
        std::scoped_lock lock{g_attack_mutex};
        const auto left_latch = g_left.latch;
        g_left = {};
        g_left.latch = left_latch;
        g_left.latch.Cover();
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
    const bool new_window = have && w.serial != t.latch.State().serial;
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
        have && t.flick.State().window == w.serial && t.flick.State().phase != FlickPhase::Done;
    const bool known_style = run.switched && run.style == t.style && run.switched_ns != 0 &&
                             now - run.switched_ns < 1'000'000'000;
    const char* gate = BlockReason(chr, body, t, flick_stroke, known_style);
    if (SawAttackTick(chr, body, t, gate, dt, now)) {
        gate = "the saw's grinding attack"; // never create an ordinary swing alongside the spin
    }
    if (g_native_saw_fault) {
        g_gate_block.store("physical interaction DLL failure", std::memory_order_relaxed);
        return;
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
    li.open = (w.open && !(whip_slow && t.latch.State().active)) || (have && WhipHoldsWindow(w, now)) ||
              plunge_hold;
    li.stale = !w.open && w.close_ns != 0 && now - w.close_ns > StaleCloseNs;
    li.gates = gate == nullptr;
    li.ready = !g_visceral || !VR::Melee::GetMeleeSettings().visceral ||
               g_visceral_checked.load(std::memory_order_acquire) == w.serial;
    // The swing buffer: a new swing refused only because the game takes no attack yet (a kept
    // stagger's attack window about to open) waits for it, its window still open, at most
    // swing_buffer_ms; then (or at any other refusal) the latch refuses it as before.
    const bool buffer_pending =
        have && (w.serial != t.latch.State().serial || (!t.latch.State().active && !t.latch.State().done));
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
    if (whip_slow && !t.latch.State().active) {
        li.ready = false;
        li.max_wait = 1u << 30; // until the chain is fast, or the next window
    }
    if (new_window) {
        g_stats.windows.fetch_add(1, std::memory_order_relaxed);
    }
    const M::LatchStep step = t.latch.Step(li);
    const s64 held_ns = t.buffer_logged == w.serial ? t.buffer.HeldNs(w.serial, now) : 0;
    if (step == M::LatchStep::Create && held_ns > 0) {
        g_stats.buffer_opened.fetch_add(1, std::memory_order_relaxed);
        LOG_INFO(Core, "VR melee: #{} held {:.0f} ms: the game takes attacks again, its attack opens",
                 w.serial, held_ns / 1e6);
    }
    switch (step) {
    case M::LatchStep::Create:
        if (flick_stroke && t.flick.State().phase != FlickPhase::Old) {
            FlickOpen(chr, t, w, body.right_weight, now); // the switch is under way or done
        } else {
            Create(chr, t, w, body.right_weight, now, flick_stroke);
        }
        break;
    case M::LatchStep::Refresh:
        if (flick_stroke && t.flick.State().phase == FlickPhase::Gap) {
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
    if (step == M::LatchStep::Idle && t.cur_live && !t.latch.State().active) {
        Finalize(chr, t, have ? &w : nullptr, "over", now);
    }
    if (!t.latch.State().active) {
        t.unpaid_live = false;
    }
    Combat::MeleeStatus status{};
    status.ns = now;
    status.installed = g_installed;
    status.physical = g_physical;
    // A flick's stroke keeps the swing route while it lasts, also into a form without moves of
    // its own (the presenter would close the window and press R1 / R2 for the rest of it).
    const bool flick_live = flick_stroke && w.open && t.flick.State().phase != FlickPhase::Done;
    status.ok = g_physical && body.body && (t.weapon_why == nullptr || flick_live);
    status.variation = t.variation;
    status.form_b = t.form_b;
    status.charged_melee = t.moves != nullptr && t.moves->rows[t.form_b ? 5 : 2].Valid();
    // Publish geometry, not world velocity: XR differentiates this tip on its final steered
    // pose in tracking space, so locomotion/snap turns cannot manufacture an attack. Measured
    // once per settled form (the dummies of a spinning part move; the dmy lookups are not free).
    PiNativeAttackInput geometry_input{};
    geometry_input.operation = PI_ATTACK_TWO_HAND_GEOMETRY;
    auto& geometry_facts = geometry_input.decision.geometry;
    geometry_facts.command = PI_ATG_CHECK;
    geometry_facts.variation = t.variation;
    geometry_facts.style = t.style;
    geometry_facts.stable_frames = t.style_stable;
    auto geometry_result = g_native_attack_runtime.Call(geometry_input);
    if (geometry_result.valid && geometry_result.geometry.measure) {
        B::Vec3 base{}, tip{};
        const char* how = nullptr;
        float strike_near = 0.f;
        std::array<float, 12> grip{};
        geometry_facts.command = PI_ATG_REPLY;
        geometry_facts.request_id = geometry_result.geometry.request_id;
        geometry_facts.have = MeleeBladeSegment(chr, base, tip, how, nullptr, &strike_near) &&
                              DummyMtx(chr, RightWeaponDummy, grip);
        if (geometry_facts.have) {
            PhysicalInteractions::AttackPackets::Pack(geometry_facts.tip,
                                                      InFrameCm(grip, tip) * .01f);
            geometry_facts.strike_near = strike_near;
        }
        geometry_result = g_native_attack_runtime.Call(geometry_input);
    }
    if (geometry_result.valid && geometry_result.geometry.eligible) {
        const auto& geometry = geometry_result.geometry;
        status.blade_tip_valid = geometry.valid;
        status.blade_tip = PhysicalInteractions::AttackPackets::V(geometry.tip);
        status.blade_strike_near = geometry.strike_near;
    }
    status.bow_hand = t.bow_hand;
    status.bow_direct = t.bow_hand >= 0 && VR::Bow::GetBowSettings().direct && GunBowDirectOk();
    status.handled = t.latch.State().serial;
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
                result = g_try_throw(throws, PadTypeR1, 0);
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
            PiNativeMeleeInput pace{}; pace.operation = PI_NATIVE_MELEE_PACE;
            pace.pace = g_whip_pace; pace.pace_at_ns = found->created_ns;
            pace.pace_recover_s = VR::Whip::WhipRecover(found->whip_variation, static_cast<std::size_t>(kind));
            PiNativeMeleeOutput approved{};
            if (!VR::Interactions::NativeMelee(g_native_saw_module, pace, approved) || !approved.valid) return false;
            g_whip_pace = approved.pace; found->pace = approved.pace_share;
        }
        a = *found;
    }
    const auto& ms = VR::Melee::GetMeleeSettings();
    Combat::MeleeWindow w{};
    const bool have_w = Combat::LatestMeleeWindow(w) && w.serial == a.serial;
    // (A whip's chain still lashes after its window closed: its hits take the speed of the chain's
    // own tip, mapped onto a blade's scale: core/vr/whip_spin.h BladeEquivalentTip.)
    const WhipChainState chain = IsWhipRow(a.row) ? WhipChainNow(now) : WhipChainState{};
    PiNativeMeleeInput input{};
    input.operation = PI_NATIVE_MELEE_DAMAGE;
    auto& d = input.damage;
    d.kind = static_cast<u32>(a.kind); d.saw = a.saw; d.powered = a.powered;
    d.speed_damage = ms.speed_damage; d.budget = ms.budget; d.flick = a.flick;
    d.have_window = have_w; d.whip = IsWhipRow(a.row); d.chain_applies = chain.applies;
    d.transform = a.transform; d.verdict_light = a.verdict_light; d.have_light = a.light != nullptr; d.have_row = a.row != nullptr;
    d.plunge = a.plunge;
    const auto plunge = PlungeConfig();
    d.plunge_enabled = plunge.enabled; d.plunge_min_m = plunge.min_drop_m; d.plunge_damage = plunge.damage;
    d.chain_variation = chain.variation; d.bid = a.bid; d.row_mv = a.row ? a.row->mv : 0; d.light_mv = a.light ? a.light->mv : 0;
    d.tip = a.tip; d.window_tip = w.tip_speed; d.window_peak = w.tip_peak; d.chain_tip = chain.tip;
    d.since_s = a.since_counted_s; d.two_hand = a.two_hand; d.window_two_hand = w.two_hand; d.drop_m = a.drop_m;
    input.pace_share = a.pace; input.pace.have = a.pace >= 0;
    d.before = *damage; d.hitstop = a.row ? a.row->hitstop : .09f;
    d.chain_grip[0] = chain.grip.x; d.chain_grip[1] = chain.grip.y; d.chain_grip[2] = chain.grip.z;
    if (chain.applies && chain.variation == 2400 && a.bid >= 100000000 && (a.bid-100000000)/1000 == 2400) {
        for (int i=0;i<3;++i) d.contact[i] = Read<float>(res + ResPosition + i*4);
    }
    PiNativeMeleeOutput hit{};
    if (!VR::Interactions::NativeMelee(g_native_saw_module,input,hit) || !hit.valid) return false;
    const float tip = hit.tip, factor = hit.factor, two_hand = hit.two_hand;
    const float row_factor = hit.row_factor, plunge_factor = hit.plunge_factor;
    const float contact_distance = hit.contact_distance, range_factor = hit.range_factor;
    const float before = d.before, after = hit.after, hitstop = hit.hitstop;
    const float felt = hit.felt;
    const bool whip_paced = a.pace >= 0;
    const float whip_pace = whip_paced ? a.pace : 1.f;
    if (hit.write_damage) *damage = after;
    if (two_hand > 1.0f) {
        LOG_INFO(Core, "VR melee: #{} two-handed swing: x{:.2f} (in the HIT line's factor)", a.serial,two_hand);
    }
    if (plunge_factor > 1.0f) {
        LOG_INFO(Core, "VR melee: #{} PLUNGE hit after a {:.2f} m fall: x{:.2f}",a.serial,a.drop_m,plunge_factor);
    }
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
    g_bite_hold.store(hit.bite_hold, std::memory_order_relaxed);
    g_bite_ns.store(now, std::memory_order_release);
    Combat::PushMeleeEvent({Combat::MeleeEventKind::Hit, a.serial, felt, hitstop, now, ""});
    const u8* data = Plausible(target) ? Module(target, L::OffsetDataModule) : nullptr;
    LOG_INFO(Core,
             "VR melee: #{} HIT {}{} bid={} atk={} tgt={} tgtHp={} dmg {:.2f}->{:.2f} x{:.3f} (tip "
             "{:.1f} m/s -> speed x{:.2f}, cadence {:.2f} s -> x{:.2f}{}) {:.0f} ms after the "
             "open, t={:.3f}",
             a.serial, a.transform ? "TRANSFORM ATTACK " : "", M::MoveKindName(a.kind), bid,
             Read<s32>(res + ResAtkParam), fmt::ptr(target),
             data != nullptr ? Read<s32>(data + OffsetHp) : -1, before, after, factor * row_factor * range_factor * whip_pace,
             tip, hit.speed_factor, a.since_counted_s, hit.cadence_factor,
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
    out.swing = true;
    out.serial = a.serial;
    out.kind = M::MoveKindName(a.kind);
    out.transform = a.transform;
    PiNativeMeleeInput input{}; input.operation = PI_NATIVE_MELEE_INFO;
    input.damage.have_window = have_w; input.damage.window_tip = w.tip_speed; input.damage.tip = a.tip;
    PiNativeMeleeOutput info{};
    if (!VR::Interactions::NativeMelee(g_native_saw_module,input,info) || !info.valid) { out={}; return false; }
    out.tip_speed = info.tip;
    return true;
}

VR::Body::BiteFacts MeleeTargetFacts() {
    VR::Body::BiteFacts facts{};
    facts.ns = g_bite_ns.load(std::memory_order_acquire);
    facts.hold = g_bite_hold.load(std::memory_order_relaxed);
    facts.wall = g_bite_wall.load(std::memory_order_relaxed);
    facts.enabled = VR::Melee::GetMeleeSettings().bite;
    return facts;
}

// quick_use_wipe (core/vr/bloodborne_quickitems.cpp): the right weapon's blade as its own hit
// capsule has it, for the paper wiped along it: the first move of the form in the hand (the light
// one; dmy1 -> dmy2, its end nearer the grip the base; a sphere row: from its blade trail's other
// end, else from the grip, to dmy1; a whip's row on the chain: the grip to the chain's handle end),
// both ends on the weapon in the hand (within BladeNearM of its grip). World; false (`how` says
// why) when the weapon or its dummies are not known. Game:Main, as the pose tick that reads it.
const char* GeometryReason(uint32_t reason) {
    constexpr const char* reasons[] = {nullptr,
                                       "the right weapon not read",
                                       "no hit capsule in form A",
                                       "no hit capsule in form B",
                                       "no right grip dummy",
                                       "the whip: the grip to the chain's handle end",
                                       "the hit capsule's dummy not on the weapon shown",
                                       "the hit capsule",
                                       "the haft (the blade trail) to the hit capsule",
                                       "the hit sphere and its trail",
                                       "the grip to the hit sphere",
                                       "a module fault"};
    return reasons[reason <= PI_NGR_MODULE ? reason : PI_NGR_MODULE];
}
const M::MoveRow* FirstGeometryRow(u8* chr, const TickState& t, const char*& how) {
    PiNativeGeometryInput p{};
    p.operation = PI_NG_CHOOSE;
    p.form_b = t.form_b;
    if (g_installed && chr && chr == t.chr && t.moves) {
        p.flags = PI_NGF_AVAILABLE;
        for (size_t i = 0; i < t.moves->rows.size(); ++i)
            if (t.moves->rows[i].Valid())
                p.valid_rows |= 1u << i;
    }
    PiNativeGeometryOutput o{};
    if (!VR::Interactions::NativeGeometry(g_native_saw_module, p, o) || !o.valid) {
        how = GeometryReason(PI_NGR_MODULE);
        return nullptr;
    }
    if (!o.success) {
        how = GeometryReason(o.state.reason);
        return nullptr;
    }
    return &t.moves->rows[o.row_index];
}
PiNativeGeometryInput GeometryRow(const M::MoveRow& row) {
    PiNativeGeometryInput p{};
    p.dmy1 = row.dmy1;
    p.dmy2 = row.dmy2;
    p.trail_base = row.trail_base;
    p.trail_tip = row.trail_tip;
    p.blade = row.blade;
    if (IsWhipRow(&row))
        p.flags = PI_NGF_WHIP;
    return p;
}
bool MeleeBladeSegment(u8* chr, B::Vec3& base, B::Vec3& tip, const char*& how, B::Vec3* grip_out,
                       float* strike_near) {
    const auto* row = FirstGeometryRow(chr, g_tick, how);
    if (!row)
        return false;
    auto p = GeometryRow(*row);
    p.operation = PI_NG_SEGMENT;
    for (int stage = 0; stage < 8; ++stage) {
        PiNativeGeometryOutput o{};
        if (!VR::Interactions::NativeGeometry(g_native_saw_module, p, o) || !o.valid) {
            how = GeometryReason(PI_NGR_MODULE);
            return false;
        }
        const auto& s = o.state;
        if (s.reason)
            how = GeometryReason(s.reason);
        if ((o.write_flags & PI_NGW_GRIP) && grip_out)
            *grip_out = {s.grip[0], s.grip[1], s.grip[2]};
        if (o.write_flags & PI_NGW_BASE)
            base = {s.base[0], s.base[1], s.base[2]};
        if (o.write_flags & PI_NGW_TIP)
            tip = {s.tip[0], s.tip[1], s.tip[2]};
        if ((o.write_flags & PI_NGW_STRIKE) && strike_near)
            *strike_near = s.strike_near;
        if (o.done)
            return o.success;
        B::Vec3 point{};
        const bool have = DummyAt(chr, s.query, point);
        p.operation = PI_NG_REPLY;
        p.state = s;
        p.reply_id = s.query;
        p.flags = (p.flags & ~PI_NGF_HAVE) | (have ? PI_NGF_HAVE : 0);
        p.point[0] = point.x;
        p.point[1] = point.y;
        p.point[2] = point.z;
    }
    if (g_native_saw_module)
        g_native_saw_module->ReportFault();
    how = GeometryReason(PI_NGR_MODULE);
    return false;
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
        if (!Plausible(part) || Read<const u8*>(part) != g_base + WeaponPartVtableRva)
            return false;
        const auto* anim = Read<const u8*>(part + 0x158);
        const auto* state = Plausible(anim) ? Read<const u8*>(anim) : nullptr;
        const auto* player = Plausible(state) ? Read<const u8*>(state) : nullptr;
        const auto* node = Plausible(player) ? Read<const u8*>(player + 0x20) : nullptr;
        if (!Plausible(node))
            return false;
        const auto* pose = node + 0x38;
        const auto* skel = Read<const u8*>(pose);
        if (!Plausible(skel))
            return false;
        const int count = Read<s32>(skel + 0x30);
        const auto* names = Read<const u8*>(skel + 0x28);
        const auto* parents = Read<const s16*>(skel + 0x18);
        const auto* locals = Read<const B::HkQsTransform*>(pose + 0x08);
        if (count < 1 || count > 64 || Read<s32>(pose + 0x10) != count ||
            Read<s32>(skel + 0x20) != count || !Plausible(names) ||
            !Plausible(reinterpret_cast<const u8*>(parents), 2) ||
            !Plausible(reinterpret_cast<const u8*>(locals), 16))
            return false;
        PiNativeSkeletonInput skeleton{};
        skeleton.size = sizeof(skeleton);
        for (int i = 0; i < count; ++i) {
            const int parent = parents[i];
            if (parent < -1 || parent >= i)
                return false;
            const auto local = B::FromHavok(locals[i]);
            if (!B::IsFinite(local) || B::Dot(local.q, local.q) < 0.5f)
                return false;
            // Validate each native prefix entry before following its name pointer, preserving
            // the original early-read gates. Model composition belongs to the DLL.
            skeleton.parent[i] = parent;
            skeleton.local[i] = PhysicalInteractions::NativeSkeletonPackets::Pack(local);
            const auto raw = Read<uintptr_t>(names + 16 * i) & ~uintptr_t{1};
            const auto* name = reinterpret_cast<const char*>(raw);
            if (!Plausible(reinterpret_cast<const u8*>(name), 1))
                continue;
            std::size_t len = 0;
            while (len < 48 && name[len] != 0)
                ++len;
            if (std::string_view{name, len} == wanted) {
                skeleton.count = i + 1;
                skeleton.target = i;
                PiNativeSkeletonOutput solved{};
                if (!VR::Interactions::NativeSkeleton(g_native_saw_module, skeleton, solved) ||
                    !solved.valid)
                    return false;
                if (wanted == "B_String")
                    g_bow_string_rig = {node, skel, count, i};
                result = PhysicalInteractions::NativeSkeletonPackets::Unpack(solved.model);
                return true;
            }
        }
        return false;
    };
    B::QsTransform grip{}, string{};
    if (!bone(g_tick.bow_part, "B_Grip", out.bow_root) || !bone(g_tick.bow_part, "B_Bowgrip", grip))
        return false;
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
    if (!g_bow_string_request.Load(req) || req.rig.node != node)
        return;
    const auto& r = req.rig;
    auto* const pose = node + 0x38;
    if (Read<const u8*>(pose) != r.skeleton || r.count < 1 || r.count > 64 || r.bone < 0 ||
        r.bone >= r.count || Read<s32>(pose + 0x10) != r.count)
        return;
    auto* const locals = Read<B::HkQsTransform*>(pose + 0x08);
    const auto* parents = Read<const s16*>(r.skeleton + 0x18);
    auto* const flags = Read<u32*>(pose + 0x28);
    if (!Plausible(reinterpret_cast<const u8*>(locals), 16) ||
        !Plausible(reinterpret_cast<const u8*>(parents), 2) ||
        !Plausible(reinterpret_cast<const u8*>(flags), 4) || Read<s32>(pose + 0x30) != r.count)
        return;
    const s64 now = NowNs();
    auto current = B::FromHavok(locals[r.bone]);
    if (g_bow_string_last_node != node || g_bow_string_last_skeleton != r.skeleton) {
        g_bow_string_last_node = node;
        g_bow_string_last_skeleton = r.skeleton;
        ++g_bow_string_identity;
    }
    const auto dirty = [&] {
        for (int i = 0; i < r.count; ++i)
            flags[i] = 2;
        const u16 value = 0x0100;
        std::memcpy(pose + 0x38, &value, sizeof(value));
    };
    const auto cleanup = [&] {
        if (g_bow_string_cleanup.Restore(g_bow_string_identity, r.count, r.bone, current.t)) {
            B::ToHavok(current, locals[r.bone]);
            dirty();
        }
        Combat::BowPartNode().store(0, std::memory_order_release);
    };
    if (g_bow_string_dll && (!g_bow_string_module || g_bow_string_fault ||
                             VR::Interactions::ProcessFaultLatch().Tripped())) {
        cleanup();
        return;
    }
    VR::Bow::StringFrame f{};
    f.rig = g_bow_string_identity;
    f.now_ns = now;
    f.request_ns = req.ns;
    f.count = r.count;
    f.bone = r.bone;
    f.enabled = VR::Bow::GetBowSettings().enabled;
    f.target = req.target;
    for (int i = 0; i < r.count; ++i) {
        f.parents[i] = parents[i];
        f.pose[i] = B::FromHavok(locals[i]);
    }
    if (!g_bow_string_policy.Step(f)) {
        cleanup();
        return;
    }
    if (g_bow_string_dll && VR::Interactions::ProcessFaultLatch().Tripped()) {
        cleanup();
        return;
    }
    const auto& result = g_bow_string_policy.Output();
    if (!result.valid)
        return;
    if (result.write != VR::Bow::StringWrite::None) {
        current.t = result.translation;
        B::ToHavok(current, locals[r.bone]);
    }
    if (result.clear_node)
        Combat::BowPartNode().store(0, std::memory_order_release);
    g_bow_string_cleanup.Accept(f.rig, f.count, f.bone, result);
    if (result.write != VR::Bow::StringWrite::None)
        dirty();
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
    const char* how = nullptr;
    const auto* row = FirstGeometryRow(chr, g_tick, how);
    if (!row)
        return false;
    auto p = GeometryRow(*row);
    p.operation = PI_NG_TRAIL;
    for (int stage = 0; stage < 8; ++stage) {
        PiNativeGeometryOutput o{};
        if (!VR::Interactions::NativeGeometry(g_native_saw_module, p, o) || !o.valid)
            return false;
        const auto& s = o.state;
        if (o.write_flags & PI_NGW_IDS) {
            id_a = p.trail_base;
            id_b = p.trail_tip;
        }
        if (o.write_flags & PI_NGW_BASE)
            a = {s.base[0], s.base[1], s.base[2]};
        if (o.write_flags & PI_NGW_TIP)
            b = {s.tip[0], s.tip[1], s.tip[2]};
        if (o.done)
            return o.success;
        B::Vec3 point{};
        const bool have = DummyAt(chr, s.query, point);
        p.operation = PI_NG_REPLY;
        p.state = s;
        p.reply_id = s.query;
        p.flags = (p.flags & ~PI_NGF_HAVE) | (have ? PI_NGF_HAVE : 0);
        p.point[0] = point.x;
        p.point[1] = point.y;
        p.point[2] = point.z;
    }
    if (g_native_saw_module)
        g_native_saw_module->ReportFault();
    return false;
}

} // namespace VR::Bloodborne
