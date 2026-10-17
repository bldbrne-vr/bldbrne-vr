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
#include "core/vr/bb_weapons.h"
#include "core/vr/bloodborne_heal.h"
#include "core/vr/bloodborne_layout.h"
#include "core/vr/bloodborne_transform.h"
#include "core/vr/body_channel.h"
#include "core/vr/combat_channel.h"
#include "core/vr/hook_util.h"
#include "core/vr/seqlock.h"
#include "core/vr/transform.h"
#include "core/vr/transform_settings.h"
#include "core/vr/trick_weapons.h"
#include "core/vr/vr_session.h"

namespace VR::Bloodborne {

namespace {

namespace B = VR::Body;
namespace L = VR::Bloodborne::Layout;
namespace T = VR::Transform;
using Hook::Plausible;
using Hook::Read;

// Module-relative addresses in the 1.09 eboot (patch file address - 0x400000). Every signature is
// unique in the eboot (scratchpad/transform/sigs.py).

// PlayerIns vt+0x118 (0x18F84F0, Game:Main): after the ChrAsm model update it picks the first
// non-locomotion TimeAct entry (id in r14d, its clip time stored at [rbp-0x58]) and updates every
// weapon part with `0x1CA6E30(part, id, force_default, time, dt)`. The 15 patched bytes choose
// the part loop by the attack type [r15+0x1C4] (`mov ecx, [r15+0x1C4]; cmp ecx, 6; vmovss xmm1,
// [rbp-0x54]`); every branch into the window lands on its first byte (three `jmp 0x18F894D`) and
// rax is dead there. Our stub calls WeaponAnimOverride(r15, rbp, r14d): >= 0 is the clip for the
// parts (its time already written to [rbp-0x58]) and the stub goes to loop 3 (0x18F8A80: parts
// 6-28, none forced to its default; it needs only r14d, [rbp-0x58] and xmm1 = dt); < 0 replays
// the 15 bytes and returns to 0x18F895C (the `ja` on their flags).
constexpr u64 WeaponAnimSiteRva = 0x18F894D;
constexpr size_t WeaponAnimLen = 15;
constexpr std::array<u8, WeaponAnimLen> WeaponAnimSig = {
    0x41, 0x8B, 0x8F, 0xC4, 0x01, 0x00, 0x00, // mov ecx, [r15+0x1C4]
    0x83, 0xF9, 0x06,                         // cmp ecx, 6
    0xC5, 0xFA, 0x10, 0x4D, 0xAC,             // vmovss xmm1, [rbp-0x54]
};
// The bytes before (the entry's time stored at [rbp-0x58]) and after (the loop choice).
constexpr u64 WeaponAnimCtxRva = 0x18F8942;
constexpr std::array<u8, 11> WeaponAnimCtxSig = {
    0xC5, 0xFA, 0x10, 0x44, 0x39, 0x28, // vmovss xmm0, [rcx+rdi+0x28]
    0xC5, 0xFA, 0x11, 0x45, 0xA8,       // vmovss [rbp-0x58], xmm0
};
constexpr u64 WeaponAnimNextRva = 0x18F895C;
constexpr std::array<u8, 14> WeaponAnimNextSig = {
    0x0F, 0x87, 0x95, 0x00, 0x00, 0x00, // ja 0x18F89F7
    0xB8, 0x63, 0x00, 0x00, 0x00,       // mov eax, 0x63
    0x0F, 0xA3, 0xC8,                   // bt eax, ecx
};
constexpr u64 Loop3Rva = 0x18F8A80;
constexpr std::array<u8, 26> Loop3Sig = {
    0x4D, 0x8B, 0xAF, 0xD0, 0x04, 0x00, 0x00, // mov r13, [r15+0x4D0]
    0xBB, 0x06, 0x00, 0x00, 0x00,             // mov ebx, 6
    0x49, 0x8B, 0x7C, 0xDD, 0x00,             // mov rdi, [r13+rbx*8]
    0x48, 0x85, 0xFF, 0x74, 0x14,             // test rdi, rdi; je
    0x31, 0xD2,                               // xor edx, edx (no part forced)
    0x44, 0x89,                               // mov esi, r14d ...
};
constexpr s32 FrameClipTime = -0x58; // f32 [rbp-0x58]
constexpr s32 FrameDt = -0x54;       // f32 [rbp-0x54]

// The TAE event listener (0x18 bytes, vtable 0x5335150, +0x10 = its character) the character's
// constructor stores at chr+0x68 (0x18BB7ED: `lea rdx, [vtable]; mov [rax], rdx; mov [rax+0x10],
// r14; ...; mov [r14+0x68], rax`). Its vt+0x18 (0x1A216B0) handles the TAE events of type 32 (and
// 96, 128, ...): it reads only the event's type [ev+0] and its params pointer [ev+8]. Case 32 (the
// dispatch `cmp eax, 0x20` at 0x1A21B2E) checks IsMainPlayer, writes the style into the equip
// data (+0x08) and the HKS mirror [[chr+0xF0]], calls the ChrCtrl's vt+0x70, drops the other
// hand's buffs for styles 2 / 3, syncs the PlayerGameData (0x1894F60) and, when the style changed,
// the game's own telemetry: what L1's clip does.
constexpr u64 ListenerCtorRva = 0x18BB7ED;
constexpr std::array<u8, 26> ListenerCtorSig = {
    0x48, 0x8D, 0x15, 0x5C, 0x99, 0xA7, 0x03, // lea rdx, [rip+...] (the vtable)
    0x48, 0x89, 0x10,                         // mov [rax], rdx
    0x4C, 0x89, 0x70, 0x10,                   // mov [rax+0x10], r14
    0xEB, 0x06, 0x49, 0x8B, 0x4E, 0x58, 0x31, 0xC0, 0x49, 0x89, 0x46, 0x68, // mov [r14+0x68], rax
};
constexpr u64 ListenerVtableRva = 0x5335150;
constexpr u64 TaeHandlerRva = 0x1A216B0;
// (Its prologue up to `mov r12, [rbx+8]; test r12, r12`: the params pointer, null = no event.)
constexpr std::array<u8, 48> TaeHandlerSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC,
    0x28, 0x02, 0x00, 0x00, 0x48, 0x89, 0xF3, 0x49, 0x89, 0xFE, 0x4C, 0x8B, 0x3D, 0x0F, 0x25, 0x9C,
    0x03, 0x49, 0x8B, 0x07, 0x48, 0x89, 0x45, 0xD0, 0x4C, 0x8B, 0x63, 0x08, 0x4D, 0x85, 0xE4, 0x0F,
};
constexpr u64 Case32HeadRva = 0x1A21B2E;
constexpr std::array<u8, 25> Case32HeadSig = {
    0x83, 0xF8, 0x20, 0x0F, 0x85, 0xE5, 0x0A, 0x00, 0x00, // cmp eax, 0x20; jne
    0x4D, 0x8B, 0x76, 0x10,                               // mov r14, [r14+0x10] (the chr)
    0x49, 0x8B, 0x06, 0x4C, 0x89, 0xF7,                   // IsMainPlayer ...
    0xFF, 0x90, 0xB8, 0x01, 0x00, 0x00,
};
constexpr u64 Case32BodyRva = 0x1A229A7;
constexpr std::array<u8, 56> Case32BodySig = {
    0x48, 0x8B, 0x5B, 0x08, 0x49, 0x8B, 0x06, 0x4C, 0x89, 0xF7, 0xFF, 0x90, 0xE8, 0x02,
    0x00, 0x00, 0x44, 0x8B, 0x60, 0x08, 0x49, 0x8B, 0x06, 0x4C, 0x89, 0xF7, 0xFF, 0x90,
    0xE8, 0x02, 0x00, 0x00, 0x8B, 0x33, 0x89, 0x70, 0x08, 0x49, 0x8B, 0x86, 0xF0, 0x00,
    0x00, 0x00, 0x89, 0x30, 0x49, 0x8B, 0x7E, 0x58, 0x48, 0x8B, 0x07, 0xFF, 0x50, 0x70,
};
constexpr u64 Case32SyncRva = 0x1A22AF0;
constexpr std::array<u8, 30> Case32SyncSig = {
    0x4D, 0x8B, 0xAE, 0xB8, 0x04, 0x00, 0x00, 0x49, 0x8B, 0x06, 0x4C, 0x89, 0xF7, 0xFF, 0x90,
    0xC8, 0x01, 0x00, 0x00, 0x4C, 0x89, 0xEF, 0x48, 0x89, 0xC6, 0xE8, 0x52, 0x24, 0xE7, 0xFF,
};
constexpr u32 OffsetListener = 0x68;
constexpr u32 OffsetListenerOwner = 0x10;
constexpr u32 SlotTaeEvent = 0x18;
constexpr s32 EventSetWeaponStyle = 32;

// PlayerIns vt+0x388 GetWeaponId (read directly, never called; the whole function holds the
// layout): equip data E = [chr+0x4B8], the right hand's active slot [E+0x10], its id at [E + 0x64
// + 4 * (slot * 2 + 1)].
constexpr u64 WeaponIdRva = 0x18FB090;
constexpr std::array<u8, 85> WeaponIdSig = {
    0x48, 0x8B, 0x87, 0xB8, 0x04, 0x00, 0x00, 0x8D, 0x4E, 0x06, 0x83, 0xF9, 0x05, 0x77, 0x3E,
    0x48, 0x8D, 0x15, 0x42, 0x00, 0x00, 0x00, 0x48, 0x63, 0x0C, 0x8A, 0x48, 0x01, 0xD1, 0xFF,
    0xE1, 0x8B, 0x48, 0x14, 0x8D, 0x74, 0x09, 0x04, 0xEB, 0x25, 0x8B, 0x48, 0x18, 0x8D, 0x74,
    0x09, 0x04, 0xEB, 0x1C, 0x8B, 0x48, 0x1C, 0xEB, 0x03, 0x8B, 0x48, 0x20, 0x8D, 0x74, 0x09,
    0x05, 0xEB, 0x0E, 0x8B, 0x70, 0x0C, 0x01, 0xF6, 0xEB, 0x07, 0x8B, 0x48, 0x10, 0x8D, 0x74,
    0x09, 0x01, 0x48, 0x63, 0xCE, 0x8B, 0x44, 0x88, 0x64, 0xC3,
};
constexpr u32 OffsetEquip = 0x4B8;
constexpr u32 OffsetEquipStyle = 0x08;
constexpr u32 OffsetEquipRightSlot = 0x10;
constexpr u32 OffsetEquipIds = 0x64;
constexpr u32 OffsetPlayerGameData = 0x3C0;
constexpr u32 OffsetPgdStyle = 0x248;   // the style the save keeps (copied into E every update)
constexpr u32 OffsetStyleMirror = 0xF0; // -> s32: the style HKS reads (env 46)

// ChrIns::IsMainPlayer: its lea proves the WorldChrMan global.
constexpr u64 IsMainPlayerRva = 0x18C2D00;
constexpr std::array<u8, 24> IsMainPlayerSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xFB, 0x4C, 0x8D,
    0x35, 0x67, 0xBB, 0xC7, 0x03, 0x49, 0x8B, 0x06, 0x48, 0x85, 0xC0, 0x75,
};
constexpr u64 WorldChrManRva = 0x553E878;
constexpr u32 OffsetMainPlayer = 0x60;

// PlaySound, `void(u64* handle_out, const float pos[3], s32 type, s32 id)` (TAE event 128's call),
// and that event's sound type: 1, or 0xA when its table byte +3 is set.
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
// SfxSlotRequest (bloodborne_held.cpp has the full story): `void(SfxCtl*, s32 slot, s32 ffx,
// const s32* dummy, u8 repeat)`, TAE event 96's call: slot -1 = a one-shot effect (event 96 asks
// for it on the event's first frame only), slot >= 0 lives while asked for every update.
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
constexpr u32 OffsetSfxCtl = 0x2A0;
// ChrIns::GetDmyPolyMtx, `s32(ChrIns*, Mtx34* out, const s32* id, u32 max)`.
constexpr u64 DmyPolyRva = 0x18C3BF0;
constexpr std::array<u8, 32> DmyPolySig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC,
    0x28, 0x41, 0x89, 0xCF, 0x48, 0x89, 0xD3, 0x48, 0x89, 0x75, 0xB0, 0x4C, 0x8B, 0x35, 0xCE, 0xFF,
};
constexpr s32 RightWeaponDummy = 300; // R_Weapon's origin (the grip)
// a148_052910's event 96: sustained saw sparks on the transformed blade. Use a private slot
// so refreshing the spin cannot replace the game's transform or other weapon effects.
constexpr s32 SawSpinFfx = 252071;
constexpr s32 SawSpinDummy = 12130;
constexpr s32 SawSpinSlot = 30300;
// The right weapon's dummies the logs trace (part 0 = form A, part 2 = form B; 100-120 along the
// blade on most weapons): the clip's motion relative to the grip.
constexpr std::array<s32, 6> TraceDummies = {10100, 10110, 10120, 12100, 12110, 12120};

using TaeEventFn = void(PS4_SYSV_ABI*)(void* listener, void* event);
using PlaySoundFn = void(PS4_SYSV_ABI*)(u64* handle, const float* pos, s32 type, s32 id);
using SfxSlotRequestFn = void(PS4_SYSV_ABI*)(void* ctl, s32 slot, s32 ffx, const s32* dummy,
                                             u8 repeat);
using DmyPolyFn = s32(PS4_SYSV_ABI*)(void* chr, void* out, const s32* ids, u32 max);

constexpr s64 Ms = 1'000'000;
constexpr float TraceFarM = 3.0f; // a dummy this far from the grip: its part is not shown (logs)
constexpr s64 StatusFreshNs = 250 * Ms; // a melee window older than this is not open any more
constexpr s64 SwitchWaitNs = 500 * Ms;  // site A must answer a switch within this
constexpr s64 EndGraceNs = 300 * Ms;    // the clip's end waits this long for a late switch
constexpr float SwingSettleS = 0.1f;    // swings wait this long after the switch
// A squeeze while the game's own attack has the right arm (a swing on R1 / R2: a form without
// physical moves such as a whip with whip=canned, or melee=vanilla) waits this long for its
// cancel window (the game buffers L1 through its attacks the same way), then transforms.
constexpr s64 GameAttackWaitNs = 1500 * Ms;
constexpr const char* GameAttackBlock = "the game's own attack has the right arm";

// ---- Install state ------------------------------------------------------------------------------

u8* g_base = nullptr;
bool g_installed = false; // the detour and the form switch (a native transform can run)
bool g_hooked = false;    // the detour is written
bool g_native = false;    // ... and transform=native
bool g_vanilla = false;   // transform=vanilla (the status says so instead of "not installed")
bool g_sound = false;
bool g_ffx = false;
TaeEventFn g_tae_event = nullptr;
PlaySoundFn g_play_sound = nullptr;
SfxSlotRequestFn g_sfx_slot = nullptr;
DmyPolyFn g_dmy_poly = nullptr;
const u8* g_sound_table = nullptr;
std::unique_ptr<Xbyak::CodeGenerator> g_code;
// VR_TRANSFORM_LOG: 1 = + the weapon's dummies relative to the grip through each transform, 2 = +
// every change of the id the game hands its weapon parts (also with transform=vanilla).
int g_log = 0;
bool g_dry_run = false; // VR_TRANSFORM_DRYRUN: decided and logged, the game untouched

s64 NowNs() {
    return B::SteadyNowNs();
}

u8* MainPlayer() {
    if (g_base == nullptr) {
        return nullptr;
    }
    u8* const world = Read<u8*>(g_base + WorldChrManRva);
    u8* const chr = Plausible(world) ? Read<u8*>(world + OffsetMainPlayer) : nullptr;
    return Plausible(chr) ? chr : nullptr;
}

u8* Equip(const u8* chr) {
    u8* const equip = Read<u8*>(chr + OffsetEquip);
    return Plausible(equip) ? equip : nullptr;
}

s32 WeaponStyle(const u8* chr) {
    const u8* equip = Equip(chr);
    return equip != nullptr ? Read<s32>(equip + OffsetEquipStyle) : -1;
}

// The right hand's weapon (EquipParamWeapon id) and its slot; -1 unreadable.
s32 RightWeapon(const u8* chr, s32* slot_out = nullptr) {
    const u8* equip = Equip(chr);
    if (equip == nullptr) {
        return -1;
    }
    const s32 slot = Read<s32>(equip + OffsetEquipRightSlot);
    if (slot_out != nullptr) {
        *slot_out = slot;
    }
    if (slot < 0 || slot > 2) {
        return -1;
    }
    return Read<s32>(equip + OffsetEquipIds + 4 * (slot * 2 + 1));
}

s32 PgdStyle(const u8* chr) {
    const u8* pgd = Read<u8*>(chr + OffsetPlayerGameData);
    return Plausible(pgd) ? Read<s32>(pgd + OffsetPgdStyle) : -1;
}

s32 MirrorStyle(const u8* chr) {
    const u8* mirror = Read<u8*>(chr + OffsetStyleMirror);
    return Plausible(mirror, 4) ? Read<s32>(mirror) : -1;
}

s32 Hp(const u8* chr) {
    u8* const modules = Read<u8*>(chr + L::OffsetChrModules);
    u8* const data = Plausible(modules) ? Read<u8*>(modules + L::OffsetDataModule) : nullptr;
    return Plausible(data) ? Read<s32>(data + L::OffsetHp) : -1;
}

// A dummy poly's world matrix (3x4 rows [R | t]).
bool DummyMtx(u8* chr, s32 id, std::array<float, 12>& out) {
    if (g_dmy_poly == nullptr || id < 0) {
        return false;
    }
    alignas(16) std::array<float, 8 * 12> m{};
    if (g_dmy_poly(chr, m.data(), &id, 1) < 1) {
        return false;
    }
    std::copy_n(m.begin(), 12, out.begin());
    return std::isfinite(out[3]) && std::isfinite(out[7]) && std::isfinite(out[11]);
}

std::string FamilyName(s32 family) {
    const auto* f = B::Weapons::FindFamily(family);
    return f != nullptr ? std::string{f->name} : fmt::format("family {}", family);
}

// ---- Shared state -------------------------------------------------------------------------------

// The running transform as the weapon-animation detour needs it (the pose tick writes it at the
// start and the end; the detour reads it every frame on Game:Main).
struct HookRun {
    u8* chr{nullptr};
    std::int64_t base_ns{0}; // the clock's base instant (the start, or a flick re-basing it)
    std::int32_t clip{-1};
    std::int32_t index{-1}; // into T::Clips
    T::Clock clock{};
    std::uint32_t run{0};
    bool active{false};
};
struct SawWeaponPose {
    u8* chr{nullptr};
    s64 ns{0};
    float time{0.0f};
    bool active{false};
};
SeqLock<SawWeaponPose> g_saw_pose;
SeqLock<HookRun> g_hook_run;
std::atomic<bool> g_hook_active{false};
std::atomic<u64> g_hook_frames{0}; // frames the detour handed the clip (logs)

// The form switch, handed from the pose tick to site A and answered back.
struct SwitchRequest {
    u32 run{0};
    u8* chr{nullptr};
    s32 style{-1};  // the style to set
    s32 from{-1};   // the style it must still be
    s32 weapon{-1}; // the weapon it must still be
    s64 due_ns{0};
};
struct SwitchResult {
    u32 run{0};
    bool ok{false};
    bool dry{false};
    s32 e{-1};
    s32 pgd{-1};
    s32 mirror{-1};
    s64 ns{0};
    const char* why{""};
};
std::mutex g_switch_mutex;
SwitchRequest g_switch_req{};
SwitchResult g_switch_res{};
std::atomic<u32> g_switch_due{0};

// Physical melee's gate (the pose tick writes, MeleePoseTick reads on the same thread).
const char* g_swing_block = nullptr;

// ---- The weapon-animation detour (Game:Main) ---------------------------------------------------

// VR_TRANSFORM_LOG=2: what the game hands the main player's weapon parts (the detour writes, the
// pose tick traces the blade while it is a transform clip: vanilla L1 for comparison).
struct HookObserve {
    s32 id{-2};
    s64 last_ns{0};
};
HookObserve g_observe{};
std::atomic<s32> g_seen_id{-1};
std::atomic<float> g_seen_t{0.0f};

s64 PS4_SYSV_ABI WeaponAnimOverride(u8* chr, u8* frame, s32 game_id) {
    if (g_log >= 2 && chr != nullptr && chr == MainPlayer()) {
        // What the game hands the weapon parts (vanilla L1: a000_19xxx with its clip time).
        const s64 now = NowNs();
        g_seen_id.store(game_id, std::memory_order_relaxed);
        g_seen_t.store(Read<float>(frame + FrameClipTime), std::memory_order_relaxed);
        if (game_id != g_observe.id || (game_id >= 0 && now - g_observe.last_ns > 100 * Ms)) {
            g_observe.id = game_id;
            g_observe.last_ns = now;
            u8* const modules = Read<u8*>(chr + L::OffsetChrModules);
            u8* const flags =
                Plausible(modules) ? Read<u8*>(modules + L::OffsetActionFlagModule) : nullptr;
            LOG_INFO(Core,
                     "VR transform: parts get id {} t={:.3f} (attack type {}, flag {}) style {} "
                     "pgd {} mirror {}",
                     game_id, Read<float>(frame + FrameClipTime), Read<s32>(chr + 0x1C4),
                     Plausible(flags) ? Read<s32>(flags + 0x1C) : -99, WeaponStyle(chr),
                     PgdStyle(chr), MirrorStyle(chr));
        }
    }
    if (!g_hook_active.load(std::memory_order_acquire)) {
        SawWeaponPose saw{};
        if (chr == nullptr || chr != MainPlayer() || !g_saw_pose.Load(saw) ||
            !saw.active || saw.chr != chr || !VR::Tricks::SawFresh(saw.ns, NowNs()) ||
            WeaponStyle(chr) != 3 || B::Weapons::FamilyOf(RightWeapon(chr)) != VR::Tricks::SawFamily) {
            return -1;
        }
        std::memcpy(frame + FrameClipTime, &saw.time, sizeof(saw.time));
        // The weapon clip does not run the hunter's TAE, so sustain its native sparks here.
        // One slot follows the blade through spin-up and coast; it expires when this pose stops
        // being accepted (including stale requests, weapon changes and native transforms).
        u8* const ctl = chr + OffsetSfxCtl;
        if (g_ffx && Read<u8*>(ctl) == g_base + SfxCtlVtableRva) {
            g_sfx_slot(ctl, SawSpinSlot, SawSpinFfx, &SawSpinDummy, 0);
        }
        return VR::Tricks::SawLoopClip;
    }
    HookRun r{};
    if (!g_hook_run.Load(r) || !r.active || r.chr != chr || r.index < 0 ||
        r.index >= static_cast<s32>(T::Clips.size())) {
        return -1;
    }
    if (chr != MainPlayer()) {
        return -1;
    }
    const T::Clip& c = T::Clips[static_cast<size_t>(r.index)];
    const float elapsed = static_cast<float>(NowNs() - r.base_ns) * 1e-9f;
    if (elapsed > T::ClockSecondsTo(r.clock, c.length, c) + 1.0f) {
        return -1; // a run the pose tick never ended (a load): the game's own choice
    }
    const float t = T::ClockTime(r.clock, elapsed, c);
    std::memcpy(frame + FrameClipTime, &t, sizeof(t));
    g_hook_frames.fetch_add(1, std::memory_order_relaxed);
    return r.clip;
}

bool InstallHook(u8* module) {
    using namespace Xbyak::util;
    auto& c = *g_code;
    c.align(16);
    auto* const entry = const_cast<u8*>(c.getCurr());
    c.lea(rsp, ptr[rsp - 128]); // keep the guest red zone intact
    c.pushf();
    for (const auto& reg : {rax, rcx, rdx, rbx, rsi, rdi, r8, r9, r10, r11, rbp}) {
        c.push(reg);
    }
    c.mov(rdi, r15); // the PlayerIns
    c.mov(rsi, rbp); // its frame
    c.mov(edx, r14d);
    c.mov(rbp, rsp);
    c.and_(rsp, -32);
    c.sub(rsp, 32 * 16);
    for (int i = 0; i < 16; ++i) {
        c.vmovdqu(ptr[rsp + 32 * i], Xbyak::Ymm(i));
    }
    c.mov(rax, reinterpret_cast<u64>(&WeaponAnimOverride));
    c.call(rax);
    c.mov(ptr[rbp + 10 * 8], rax); // into the saved rax (pushed first): popped with the rest
    for (int i = 0; i < 16; ++i) {
        c.vmovdqu(Xbyak::Ymm(i), ptr[rsp + 32 * i]);
    }
    c.mov(rsp, rbp);
    for (const auto& reg : {rbp, r11, r10, r9, r8, rdi, rsi, rbx, rdx, rcx, rax}) {
        c.pop(reg);
    }
    c.popf();
    c.lea(rsp, ptr[rsp + 128]);
    Xbyak::Label vanilla;
    c.test(rax, rax);
    c.js(vanilla, Xbyak::CodeGenerator::T_NEAR);
    c.mov(r14d, eax); // the transform clip for every part
    c.vmovss(xmm1, ptr[rbp + FrameDt]);
    c.db(0xFF); // jmp qword [rip+0]; dq loop 3
    c.db(0x25);
    c.dd(0);
    c.dq(reinterpret_cast<u64>(module + Loop3Rva));
    c.L(vanilla);
    for (const u8 b : WeaponAnimSig) {
        c.db(b);
    }
    c.db(0xFF); // jmp qword [rip+0]; dq the loop choice's ja
    c.db(0x25);
    c.dd(0);
    c.dq(reinterpret_cast<u64>(module + WeaponAnimNextRva));
    c.ready();
    u8* const site = module + WeaponAnimSiteRva;
    if (!Hook::MakeWritable(site, WeaponAnimLen)) {
        LOG_ERROR(Core, "VR transform: could not make {:#x} writable", WeaponAnimSiteRva);
        return false;
    }
    Hook::WriteJump(site, WeaponAnimLen, entry);
    return true;
}

// ---- The pose tick ------------------------------------------------------------------------------

struct Run {
    bool active{false};
    u32 id{0};     // +1 per run
    u32 serial{0}; // the squeeze
    const T::Clip* clip{nullptr};
    s32 index{-1};
    T::Timing timing{};
    T::Clock clock{}; // the clip time from base_ns (a flick may re-base it)
    s64 base_ns{0};
    bool flick{false};
    bool no_join{false}; // a hand transform's: no swing joins it
    u32 window{0}; // the swing the flick happened in (logs)
    s64 switched_ns{0};
    s64 press_ns{0}; // the squeeze (presenter clock = ours)
    float start_t{0.0f};
    s64 start_ns{0};
    s32 weapon{-1};
    s32 slot{-1};
    s32 from{-1};
    u32 fired{0};
    bool switch_asked{false};
    bool switched{false};
    s64 switch_asked_ns{0};
    float t{0.0f};
    float last_trace_t{-1.0f};
    u32 sounds{0};
    u32 effects{0};
};

struct TickState {
    const u8* chr{nullptr};
    u64 character_generation{};
    T::PullLatch latch{};
    Combat::TransformPull claimed_pull{};
    bool have_claimed_pull{};
    Run run{};
    u32 runs{0};
    s32 weapon{-1};
    s32 slot{-1};
    s32 style{-1};
    T::Choice choice{};
    s32 logged_weapon{-2};
    s32 logged_style{-2};
    const char* logged_why{nullptr};
    bool first_logged{false};
    const char* last_wait{nullptr};
};
TickState g_tick{};

// The newest run for physical melee (TransformRun; the pose tick's thread only).
TransformRunInfo g_info{};

T::Timing CurrentTiming() {
    const auto& s = T::GetTransformSettings();
    return {s.speed, s.skip_s, s.retract_speed};
}

T::FlickTiming CurrentFlick() {
    const auto& s = T::GetTransformSettings();
    return {s.flick_speed, s.flick_lead_ms * 1e-3f};
}

void PublishHook(const Run& r, u8* chr) {
    HookRun h{};
    h.chr = chr;
    h.base_ns = r.base_ns;
    h.clip = r.clip != nullptr ? r.clip->clip : -1;
    h.index = r.index;
    h.clock = r.clock;
    h.run = r.id;
    h.active = r.active && !g_dry_run;
    g_hook_run.Store(h);
    g_hook_active.store(h.active, std::memory_order_release);
}

void PublishInfo(const Run& r) {
    TransformRunInfo& i = g_info;
    i.id = r.id;
    i.serial = r.serial;
    i.active = r.active;
    i.flick = r.flick;
    i.no_join = r.no_join;
    i.to_b = r.clip != nullptr && r.clip->to_b;
    i.from = r.from;
    i.style = r.clip != nullptr ? r.clip->style : -1;
    i.weapon = r.weapon;
    i.switch_asked = r.switch_asked;
    i.switched = r.switched;
    i.aborted = !r.active && !r.switched;
    i.start_ns = r.press_ns != 0 ? std::min(r.press_ns, r.start_ns) : r.start_ns;
    i.switched_ns = r.switched_ns;
}

// The game has the right arm for a dodge (roll, quickstep, backstep), an unverified clip or the
// game's own transform clip (L1, a000_019xxx: a weapon left on L1, e.g. the Boom Hammer's closing):
// the weapon may still transform (its clip is the weapon's own; vanilla only waits because the
// hunter's arms are busy). A weapon switch under way ends a transform anyway (the weapon id
// changes).
bool ArmFreeForWeapon(const MeleeBody& body) {
    if (!body.held || body.owner_id < 0) {
        return false;
    }
    const B::AnimClass c = B::ClassifyAnim(body.owner_id);
    return c == B::AnimClass::Dodge || c == B::AnimClass::Unverified ||
           c == B::AnimClass::Transform;
}

// The game animates the right arm for something else (an attack, a hit, an item, a gesture).
bool GameHasArm(const MeleeBody& body) {
    return !body.right_ik && !ArmFreeForWeapon(body);
}

// ... for its own attack (R1 / R2: no physical moves in this form, or melee=vanilla).
bool GameAttackHasArm(const MeleeBody& body) {
    if (!body.held || body.owner_id < 0) {
        return false;
    }
    const B::AnimClass c = B::ClassifyAnim(body.owner_id);
    return c == B::AnimClass::AttackRight || c == B::AnimClass::AttackBoth;
}

// Why a transform cannot start now (nullptr: it can).
const char* StartBlock(u8* chr, const MeleeBody& body, const TickState& t, s64 now) {
    if (t.choice.clip == nullptr) {
        return t.choice.why != nullptr ? t.choice.why : "no clip";
    }
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
    if (GameHasArm(body)) {
        // (A kept hit: your arm, but the game takes no transform before its cancel window.)
        return GameAttackHasArm(body) ? GameAttackBlock
               : body.attack_wait     ? "the game takes no attack yet"
                                      : "the game has the right arm";
    }
    if (Hp(chr) <= 0) {
        return "dead";
    }
    // A squeeze during a swing is a flick (transform_flick): the weapon changes form within the
    // stroke and physical melee makes it the transform attack. Without the flick it waits.
    Combat::MeleeWindow w{};
    if (!T::GetTransformSettings().flick && Combat::LatestMeleeWindow(w) && w.open &&
        now - w.ns < StatusFreshNs) {
        return "a swing is open";
    }
    if (t.run.active && !T::Interruptible(*t.run.clip, t.run.t, t.run.switched)) {
        return "the transform before is still locking";
    }
    return nullptr;
}

void Event(Combat::TransformEventKind kind, u32 serial, float a, float b, s64 now,
           const char* why = "") {
    Combat::PushTransformEvent({kind, serial, a, b, now, why});
}

// Retire a character or disabled transport without reading a possibly destroyed guest object.
void RetireDetached(TickState& t, s64 now, const char* why) {
    if (t.have_claimed_pull) {
        Event(Combat::TransformEventKind::Denied, t.claimed_pull.serial, 0, 0, now, why);
        t.have_claimed_pull = false;
        t.latch.pending = 0;
    }
    if (t.run.active) {
        t.run.active = false;
        PublishHook(t.run, nullptr);
        PublishInfo(t.run);
        {
            std::scoped_lock lock{g_switch_mutex};
            if (g_switch_req.run == t.run.id)
                g_switch_due.store(0, std::memory_order_release);
        }
        Event(Combat::TransformEventKind::Aborted, t.run.serial, t.run.switched ? 1.f : 0.f, 0, now,
              why);
    }
}

void StartRun(u8* chr, TickState& t, u32 serial, s64 now, bool flick, u32 window,
              s64 press_ns, bool no_join) {
    const T::Clip& c = *t.choice.clip;
    Run& r = t.run;
    if (r.active) {
        LOG_INFO(Core, "VR transform: run {} cut short at t={:.3f} by a new squeeze", r.id, r.t);
    }
    r = {};
    r.active = true;
    r.id = ++t.runs;
    r.serial = serial;
    r.clip = &c;
    r.index = static_cast<s32>(&c - T::Clips.data());
    r.timing = CurrentTiming();
    // A flick (a squeeze in motion) starts at the clip's flick start at the flick's pace: the form
    // switches within the stroke; a plain squeeze from the skipped still lead-in.
    r.flick = flick;
    r.no_join = no_join;
    r.window = window;
    r.clock = flick ? T::FlickClock(c, CurrentFlick()) : T::NormalClock(c, r.timing);
    r.base_ns = now;
    r.start_t = r.clock.base_t;
    r.t = r.start_t;
    r.start_ns = now;
    r.press_ns = press_ns;
    r.weapon = t.weapon;
    r.slot = t.slot;
    r.from = t.style;
    PublishHook(r, chr);
    const float to_switch = T::ClockSecondsTo(r.clock, c.t_style, c);
    Event(Combat::TransformEventKind::Started, serial, static_cast<float>(c.clip), to_switch, now);
    LOG_INFO(Core,
             "VR transform: #{} start {} {} a000_{} (style {} -> {} at t={:.3f}; clip {:.3f} s "
             "from t={:.3f} at x{:.2f}{}: switch in {:.3f} s){}{} | E {} PGD {} mirror {} | thread "
             "'{}'",
             serial, FamilyName(c.family), c.to_b ? "A->B" : "B->A", c.clip, t.style, c.style,
             c.t_style, c.length, r.start_t, r.clock.speed,
             T::ClockLeadSeconds(r.clock) > 0.0f
                 ? fmt::format(" (x{:.2f} to the switch: transform_retract_speed)",
                               r.clock.lead_speed)
                 : std::string{},
             to_switch,
             flick ? fmt::format(" FLICK{} {:.0f} ms after the squeeze",
                                 window != 0 ? fmt::format(" in swing #{}", window) : std::string{},
                                 static_cast<double>(now - press_ns) * 1e-6)
                   : std::string{},
             c.weapon_anim ? "" : " (no weapon clip: the parts change at the switch)",
             WeaponStyle(chr), PgdStyle(chr), MirrorStyle(chr), Common::GetCurrentThreadName());
}

// Ends the run (the detour lets the game's own choice through again).
void EndRun(u8* chr, TickState& t, const char* why, bool done, s64 now) {
    Run& r = t.run;
    if (!r.active) {
        return;
    }
    r.active = false;
    PublishHook(r, chr);
    PublishInfo(r);
    {
        std::scoped_lock lock{g_switch_mutex};
        if (g_switch_req.run == r.id) {
            g_switch_due.store(0, std::memory_order_release); // a switch not done any more
        }
    }
    const float elapsed = static_cast<float>(now - r.start_ns) * 1e-9f;
    if (done) {
        Event(Combat::TransformEventKind::Done, r.serial, 0.0f, 0.0f, now);
    } else {
        Event(Combat::TransformEventKind::Aborted, r.serial, r.switched ? 1.0f : 0.0f, 0.0f, now,
              why);
    }
    LOG_INFO(Core,
             "VR transform: #{} {}{} after {:.3f} s (t={:.3f}{}): style now E {} PGD {} mirror {}; "
             "{} sounds, {} effects, the detour {} frames",
             r.serial, r.flick ? "flick " : "", done ? "done" : fmt::format("ABORTED ({})", why),
             elapsed, r.t, r.switched ? ", switched" : ", no form change", WeaponStyle(chr),
             PgdStyle(chr),
             MirrorStyle(chr), r.sounds, r.effects,
             g_hook_frames.exchange(0, std::memory_order_relaxed));
}

void PlayCues(u8* chr, Run& r, const T::CueStep& step, s64 now) {
    const T::Clip& c = *r.clip;
    const bool cues = T::GetTransformSettings().cues && !g_dry_run;
    std::array<float, 12> grip{};
    const bool have_grip = DummyMtx(chr, RightWeaponDummy, grip);
    u8* const ctl = chr + OffsetSfxCtl;
    const bool ctl_ok = g_ffx && Read<u8*>(ctl) == g_base + SfxCtlVtableRva;
    for (u32 i = 0; i < c.cue_count; ++i) {
        const u32 bit = 1u << i;
        const T::Cue& q = c.cues[i];
        if ((step.fire & bit) != 0) {
            u64 handle = 0;
            if (q.kind == T::CueKind::Sound) {
                ++r.sounds;
                if (cues && g_sound && have_grip) {
                    alignas(16) std::array<float, 4> p{grip[3], grip[7], grip[11], 1.0f};
                    const s32 type =
                        g_sound_table != nullptr && Read<u8>(g_sound_table + 3) != 0 ? 0xA : 1;
                    g_play_sound(&handle, p.data(), type, q.a);
                }
                Event(Combat::TransformEventKind::Detent, r.serial, static_cast<float>(q.a), r.t,
                      now);
            } else {
                ++r.effects;
                if (cues && ctl_ok) {
                    const s32 dummy = q.b;
                    g_sfx_slot(ctl, q.c, q.a, &dummy, 0);
                }
            }
            if (g_log >= 1) {
                LOG_INFO(Core, "VR transform: #{} t={:.3f} {} {} (event at {:.3f})", r.serial, r.t,
                         q.kind == T::CueKind::Sound ? "sound c" : "effect",
                         q.kind == T::CueKind::Sound
                             ? fmt::format("{} handle {:#x}", q.a, handle)
                             : fmt::format("{} on dummy {} slot {}", q.a, q.b, q.c),
                         q.t0);
            }
        }
        if ((step.held & bit) != 0 && cues && ctl_ok) {
            const s32 dummy = q.b;
            g_sfx_slot(ctl, q.c, q.a, &dummy, 0);
            if ((r.fired & bit) == 0) {
                r.fired |= bit; // counted once
                ++r.effects;
            }
        }
    }
}

// VR_TRANSFORM_LOG: the weapon's dummies in the grip's frame (cm: x right, y up, z forward of the
// R_Weapon dummy), every ~0.05 s of clip time: the clip moving the blade in the hand.
void TraceBlade(u8* chr, u32 serial, float t, float& last_t) {
    if (g_log < 1 || (t - last_t < 0.05f && t >= last_t)) {
        return;
    }
    last_t = t;
    std::array<float, 12> g{};
    if (!DummyMtx(chr, RightWeaponDummy, g)) {
        return;
    }
    std::string text;
    for (const s32 id : TraceDummies) {
        std::array<float, 12> m{};
        if (!DummyMtx(chr, id, m)) {
            continue;
        }
        const float dx = m[3] - g[3];
        const float dy = m[7] - g[7];
        const float dz = m[11] - g[11];
        if (dx * dx + dy * dy + dz * dz > TraceFarM * TraceFarM) {
            continue; // the part is not shown (its dummies answer far away)
        }
        // Rows of [R | t]: the grip's axes are R's columns.
        const float lx = g[0] * dx + g[4] * dy + g[8] * dz;
        const float ly = g[1] * dx + g[5] * dy + g[9] * dz;
        const float lz = g[2] * dx + g[6] * dy + g[10] * dz;
        text +=
            fmt::format(" {}:({:.1f},{:.1f},{:.1f})", id, lx * 100.0f, ly * 100.0f, lz * 100.0f);
    }
    LOG_INFO(Core, "VR transform: #{} trace t={:.3f} style {}{}", serial, t, WeaponStyle(chr),
             text.empty() ? " (no weapon dummies)" : text);
}

void StepRun(u8* chr, TickState& t, const MeleeBody& body, s64 now) {
    Run& r = t.run;
    if (!r.active) {
        return;
    }
    const T::Clip& c = *r.clip;
    r.t = T::ClockTime(r.clock, static_cast<float>(now - r.base_ns) * 1e-9f, c);
    // Hard aborts (any time) and the game taking the arm before the switch (a cancel: no change).
    s32 slot = -1;
    const s32 weapon = RightWeapon(chr, &slot);
    if (weapon != r.weapon || slot != r.slot) {
        EndRun(chr, t, "the right weapon changed", false, now);
        return;
    }
    if (Hp(chr) <= 0 || body.reason == B::GateReason::Dead) {
        EndRun(chr, t, "dead", false, now);
        return;
    }
    if (!r.switched && !r.switch_asked) {
        const char* cancel = nullptr;
        if (GameHasArm(body)) {
            cancel = "the game took the right arm";
        } else if (body.reason == B::GateReason::Throw) {
            cancel = "visceral or grab";
        } else if (body.reason == B::GateReason::Event) {
            cancel = "event";
        } else if (body.reason == B::GateReason::Ladder) {
            cancel = "ladder";
        } else if (VR::CinematicActive()) {
            cancel = "cinematic";
        }
        if (cancel != nullptr) {
            EndRun(chr, t, cancel, false, now);
            return;
        }
    }
    const T::CueStep step = T::StepCues(c, r.start_t, r.t, r.fired);
    PlayCues(chr, r, step, now);
    TraceBlade(chr, r.serial, r.t, r.last_trace_t);
    // The form switch: site A calls the game's own event 32 on the next update.
    if (!r.switch_asked && r.t >= c.t_style) {
        r.switch_asked = true;
        r.switch_asked_ns = now;
        if (g_dry_run) {
            r.switched = true;
            r.switched_ns = now;
            LOG_INFO(Core, "VR transform: #{} DRY RUN: style {} -> {} at t={:.3f} not written",
                     r.serial, r.from, c.style, r.t);
        } else {
            std::scoped_lock lock{g_switch_mutex};
            g_switch_req = {r.id, chr, c.style, r.from, r.weapon, now};
            g_switch_due.store(r.id, std::memory_order_release);
        }
    }
    if (r.switch_asked && !r.switched) {
        SwitchResult res{};
        {
            std::scoped_lock lock{g_switch_mutex};
            res = g_switch_res;
        }
        if (res.run == r.id) {
            if (!res.ok) {
                EndRun(chr, t, res.why, false, now);
                return;
            }
            r.switched = true;
            r.switched_ns = now;
            Event(Combat::TransformEventKind::Locked, r.serial, static_cast<float>(c.style), r.t,
                  now);
            if (r.flick) {
                LOG_INFO(Core, "VR transform: #{} flick switched {:.0f} ms after the squeeze",
                         r.serial, static_cast<double>(now - r.start_ns) * 1e-6);
            }
        } else if (now - r.switch_asked_ns > SwitchWaitNs) {
            EndRun(chr, t, "site A did not switch the form", false, now);
            return;
        }
    }
    if (r.t >= c.length) {
        if (r.switched) {
            EndRun(chr, t, "", true, now);
        } else if (now - r.base_ns >
                   static_cast<s64>(T::ClockSecondsTo(r.clock, c.length, c) * 1e9f) + EndGraceNs) {
            EndRun(chr, t, "the form never switched", false, now);
        }
    }
}

// A swing joined the run before its switch (physical melee): the rest at the flick's pace.
void MakeFlick(u8* chr, TickState& t, u32 window, s64 now) {
    Run& r = t.run;
    if (!r.active || r.flick || r.switch_asked) {
        return;
    }
    const T::Clip& c = *r.clip;
    r.t = T::ClockTime(r.clock, static_cast<float>(now - r.base_ns) * 1e-9f, c);
    const float speed_was = T::ClockSpeedAt(r.clock, r.t);
    r.clock = T::FlickFrom(c, r.t, speed_was, CurrentFlick());
    r.base_ns = now;
    r.flick = true;
    r.window = window;
    PublishHook(r, chr);
    PublishInfo(r);
    LOG_INFO(Core,
             "VR transform: #{} swing #{} joined at t={:.3f}: a FLICK from here, x{:.2f} -> x{:.2f} "
             "(switch in {:.3f} s)",
             r.serial, window, r.t, speed_was, r.clock.speed,
             T::ClockSecondsTo(r.clock, c.t_style, c));
}

void LogWeapon(TickState& t) {
    if (t.weapon == t.logged_weapon && t.style == t.logged_style && t.choice.why == t.logged_why) {
        return;
    }
    t.logged_weapon = t.weapon;
    t.logged_style = t.style;
    t.logged_why = t.choice.why;
    const T::Clip* c = t.choice.clip;
    LOG_INFO(Core, "VR transform: right weapon {} ({}) style {}: {}",
             FamilyName(B::Weapons::FamilyOf(t.weapon)), t.weapon, t.style,
             c != nullptr ? fmt::format("native a000_{} ({:.3f} s, style {} at t={:.3f}{}{})",
                                        c->clip, c->length, c->style, c->t_style,
                                        c->weapon_anim ? "" : ", no weapon clip",
                                        c->support == T::Support::Untested ? ", untested" : "")
                          : fmt::format("L1 ({})", t.choice.why != nullptr ? t.choice.why : "?"));
}

} // Anonymous namespace

void InstallTransform(VAddr base, u64 size) {
    if (g_base != nullptr) {
        return;
    }
    auto* const module = reinterpret_cast<u8*>(base);
    const auto& ts = T::GetTransformSettings();
    bool native = ts.native;
    if (const char* env = std::getenv("VR_TRANSFORM"); env != nullptr) {
        native = std::string_view{env} != "vanilla" && std::string_view{env} != "0";
    }
    if (const char* value = std::getenv("VR_TRANSFORM_LOG"); value != nullptr) {
        g_log = std::max(std::atoi(value), 1);
    }
    g_dry_run = std::getenv("VR_TRANSFORM_DRYRUN") != nullptr;
    if (size <= WorldChrManRva + 8) {
        LOG_INFO(Core,
                 "VR transform: eboot too small ({:#x}) for Bloodborne 1.09; the right grip "
                 "is L1",
                 size);
        return;
    }
    if (!native && g_log < 2) {
        g_vanilla = true;
        LOG_INFO(Core, "VR transform: transform=vanilla: the right grip is L1 (the game's "
                       "transform animation)");
        return;
    }
    struct Site {
        const char* name;
        u64 rva;
        const u8* sig;
        size_t length;
    };
    const std::array<Site, 10> sites = {{
        {"weapon-animation site", WeaponAnimSiteRva, WeaponAnimSig.data(), WeaponAnimSig.size()},
        {"its context", WeaponAnimCtxRva, WeaponAnimCtxSig.data(), WeaponAnimCtxSig.size()},
        {"its loop choice", WeaponAnimNextRva, WeaponAnimNextSig.data(), WeaponAnimNextSig.size()},
        {"loop 3", Loop3Rva, Loop3Sig.data(), Loop3Sig.size()},
        {"TAE listener ctor", ListenerCtorRva, ListenerCtorSig.data(), ListenerCtorSig.size()},
        {"TAE handler", TaeHandlerRva, TaeHandlerSig.data(), TaeHandlerSig.size()},
        {"event 32 dispatch", Case32HeadRva, Case32HeadSig.data(), Case32HeadSig.size()},
        {"event 32 write", Case32BodyRva, Case32BodySig.data(), Case32BodySig.size()},
        {"event 32 save sync", Case32SyncRva, Case32SyncSig.data(), Case32SyncSig.size()},
        {"GetWeaponId", WeaponIdRva, WeaponIdSig.data(), WeaponIdSig.size()},
    }};
    bool ok =
        std::memcmp(module + IsMainPlayerRva, IsMainPlayerSig.data(), IsMainPlayerSig.size()) == 0;
    std::string bad = ok ? "" : " IsMainPlayer";
    if (ok) {
        s32 disp{};
        std::memcpy(&disp, module + IsMainPlayerRva + 13, sizeof(disp));
        if (IsMainPlayerRva + 17 + static_cast<s64>(disp) != WorldChrManRva) {
            ok = false;
            bad += " WorldChrMan proof";
        }
    }
    for (const Site& s : sites) {
        if (std::memcmp(module + s.rva, s.sig, s.length) != 0) {
            ok = false;
            bad += fmt::format(" {} at {:#x} (live {})", s.name, s.rva,
                               Hook::HexBytes(module + s.rva, std::min<size_t>(s.length, 24)));
        }
    }
    if (ok) {
        s32 disp{};
        std::memcpy(&disp, module + ListenerCtorRva + 3, sizeof(disp));
        if (ListenerCtorRva + 7 + static_cast<s64>(disp) != ListenerVtableRva) {
            ok = false;
            bad += " listener vtable proof";
        }
    }
    const bool site_a = IsPlayerHookInstalled();
    if (ok && !site_a) {
        ok = false;
        bad += " (the player update hook, site A, is not installed)";
    }
    if (!ok) {
        LOG_WARNING(Core,
                    "VR transform: weapon-only transform NOT installed:{}; the right grip is "
                    "L1",
                    bad);
        return;
    }
    g_base = module;
    g_tae_event = reinterpret_cast<TaeEventFn>(module + TaeHandlerRva);
    g_sound = std::memcmp(module + PlaySoundRva, PlaySoundSig.data(), PlaySoundSig.size()) == 0 &&
              std::memcmp(module + SoundTypeRva, SoundTypeSig.data(), SoundTypeSig.size()) == 0;
    if (g_sound) {
        g_play_sound = reinterpret_cast<PlaySoundFn>(module + PlaySoundRva);
        s32 disp{};
        std::memcpy(&disp, module + SoundTypeRva + 3, sizeof(disp));
        g_sound_table = module + SoundTypeRva + 7 + disp;
    }
    g_ffx = std::memcmp(module + SfxSlotRequestRva, SfxSlotRequestSig.data(),
                        SfxSlotRequestSig.size()) == 0 &&
            std::memcmp(module + CtlCtorRva, CtlCtorSig.data(), CtlCtorSig.size()) == 0;
    if (g_ffx) {
        g_sfx_slot = reinterpret_cast<SfxSlotRequestFn>(module + SfxSlotRequestRva);
    }
    if (std::memcmp(module + DmyPolyRva, DmyPolySig.data(), DmyPolySig.size()) == 0) {
        g_dmy_poly = reinterpret_cast<DmyPolyFn>(module + DmyPolyRva);
    }
    try {
        g_code = std::make_unique<Xbyak::CodeGenerator>(1024);
        g_hooked = InstallHook(module);
    } catch (const Xbyak::Error& error) {
        LOG_ERROR(Core, "VR transform: code emission failed ({}); the right grip is L1",
                  error.what());
        g_hooked = false;
    }
    if (!g_hooked) {
        g_base = nullptr;
        return;
    }
    g_installed = true;
    g_native = native;
    u32 tested = 0;
    u32 untested = 0;
    u32 vanilla = 0;
    for (const T::Clip& c : T::Clips) {
        (c.support == T::Support::Tested     ? tested
         : c.support == T::Support::Untested ? untested
                                             : vanilla) += 1;
    }
    LOG_INFO(Core,
             "VR transform: weapon-only transform {} (detour at {:#x}, TAE listener and event 32 "
             "ok, GetWeaponId ok, WorldChrMan ok, site A ok; sounds {}, effects {}, dummies {}): "
             "{} clips ({} tested, {} untested, {} kept on L1) | {}{}{}",
             native ? "installed" : "OBSERVING ONLY (transform=vanilla)", WeaponAnimSiteRva,
             g_sound ? "ok" : "MISMATCH (none)", g_ffx ? "ok" : "MISMATCH (none)",
             g_dmy_poly != nullptr ? "ok" : "MISMATCH (no sound position)", T::Clips.size(), tested,
             untested, vanilla, T::TransformSettingsText(),
             g_log > 0 ? fmt::format(" log={}", g_log) : std::string{},
             g_dry_run ? " DRY RUN (the game untouched)" : "");
}

bool SetSawWeaponPose(u8* chr, float time, bool active) {
    const bool available = g_installed && g_hooked && !g_dry_run;
    g_saw_pose.Store({chr, NowNs(), time, available && active});
    return available;
}

void TransformPoseTick(u8* chr, const MeleeBody& body) {
    if (chr == nullptr) {
        return;
    }
    const s64 now = NowNs();
    auto& t = g_tick;
    Combat::TransformStatus status{};
    status.ns = now;
    status.installed = g_installed;
    if (!g_installed || !g_native) {
        RetireDetached(t, now, "native transform unavailable");
        Combat::TransformPull refused{};
        if (Combat::ClaimTransformPull(refused))
            Event(Combat::TransformEventKind::Denied, refused.serial, 0, 0, now,
                  "native transform unavailable");
        status.why = g_vanilla || g_installed ? "transform=vanilla" : "not installed";
        g_swing_block = nullptr;
        Combat::PublishTransformStatus(status);
        if (g_installed && g_log >= 2) {
            // Observing vanilla L1: the blade through the game's own clip (serial 0).
            static float last_t = -1.0f;
            const s32 seen = g_seen_id.load(std::memory_order_relaxed);
            if (seen >= 0 && seen % 1'000'000 >= 19000 && seen % 1'000'000 <= 19999) {
                TraceBlade(chr, 0, g_seen_t.load(std::memory_order_relaxed), last_t);
            }
        }
        return;
    }
    if (chr != t.chr) {
        RetireDetached(t, now, "character changed");
        const T::PullLatch latch = t.latch;
        const u32 runs = t.runs;
        const u64 generation = t.character_generation + 1;
        t = {};
        t.chr = chr;
        t.character_generation = generation;
        t.latch = latch;
        t.latch.pending = 0;
        t.runs = runs;
    }
    if (!t.first_logged) {
        t.first_logged = true;
        LOG_INFO(Core, "VR transform: first pose tick on thread '{}'",
                 Common::GetCurrentThreadName());
    }
    t.weapon = RightWeapon(chr, &t.slot);
    t.style = WeaponStyle(chr);
    t.choice =
        T::Choose(B::Weapons::FamilyOf(t.weapon), t.style, T::GetTransformSettings().untested);
    LogWeapon(t);

    StepRun(chr, t, body, now);

    const char* block = StartBlock(chr, body, t, now);
    Combat::TransformPull pull{};
    if (!t.have_claimed_pull)
        t.have_claimed_pull = Combat::ClaimTransformPull(t.claimed_pull);
    bool have = t.have_claimed_pull || Combat::LatestTransformPull(pull);
    if (t.have_claimed_pull) {
        pull = t.claimed_pull; // retain all request facts while StepPull buffers the claim
        if (!Combat::TransformContextMatches(pull, t.character_generation, t.weapon, t.style,
                                             t.slot)) {
            Event(Combat::TransformEventKind::Denied, pull.serial, 0, 0, now,
                  "character or equipment changed before execution");
            t.have_claimed_pull = false;
            t.latch.handled = pull.serial;
            t.latch.pending = 0;
            have = false;
        }
    }
    T::PullInput in{};
    in.have = have;
    in.serial = pull.serial;
    in.press_ns = pull.press_ns;
    in.now = now;
    in.buffer_ns = static_cast<s64>(T::GetTransformSettings().buffer_ms * 1e6f);
    if (block == GameAttackBlock) {
        in.buffer_ns = std::max(in.buffer_ns, GameAttackWaitNs); // until its cancel window
    }
    in.can_start = block == nullptr;
    // The squeeze this step is about (a new one replaces one still waiting).
    const u32 serial = have && pull.serial != t.latch.handled ? pull.serial : t.latch.pending;
    const T::PullStep step = T::StepPull(t.latch, in);
    switch (step) {
    case T::PullStep::Start:
        t.have_claimed_pull = false; // the running animation now owns the claim
        StartRun(chr, t, pull.serial, now, pull.flick && T::GetTransformSettings().flick,
                 pull.window, pull.press_ns != 0 ? pull.press_ns : now, pull.no_join);
        t.last_wait = nullptr;
        break;
    case T::PullStep::Deny:
        t.have_claimed_pull = false;
        Event(Combat::TransformEventKind::Denied, serial, 0.0f, 0.0f, now,
              block != nullptr ? block : "stale");
        LOG_INFO(Core, "VR transform: #{} refused ({}) after {:.0f} ms", serial,
                 block != nullptr ? block : "stale",
                 static_cast<double>(now - t.latch.pending_ns) * 1e-6);
        t.last_wait = nullptr;
        break;
    case T::PullStep::Wait:
        if (block != t.last_wait) {
            t.last_wait = block;
            LOG_INFO(Core, "VR transform: #{} waits ({})", t.latch.pending,
                     block != nullptr ? block : "?");
        }
        break;
    case T::PullStep::Idle:
        break;
    }

    // Physical melee: with the flick a swing during a transform joins it (TransformRun: the
    // stroke becomes the transform attack); without it no swing before the switch has settled.
    g_swing_block = nullptr;
    if (t.run.active && !T::GetTransformSettings().flick) {
        const T::Clip& c = *t.run.clip;
        if (!t.run.switched || t.run.t < c.t_style + SwingSettleS) {
            g_swing_block = "transforming";
        }
    }
    if (t.run.active || t.run.id != g_info.id) {
        PublishInfo(t.run);
    }

    status.ok = body.body && body.driven && t.choice.clip != nullptr;
    status.busy = t.run.active;
    status.weapon_id = t.weapon;
    status.style = t.style;
    status.character_generation = t.character_generation;
    status.weapon_slot = t.slot;
    status.clip = t.choice.clip != nullptr ? t.choice.clip->clip : -1;
    status.handled = t.latch.handled;
    status.why = !body.body                 ? "body off"
                 : !body.driven             ? "not first person"
                 : t.choice.clip == nullptr ? (t.choice.why != nullptr ? t.choice.why : "no clip")
                                            : "";
    Combat::PublishTransformStatus(status);
}

void TransformSiteA(u8* player) {
    if (!g_installed || player == nullptr) {
        return;
    }
    const u32 due = g_switch_due.load(std::memory_order_acquire);
    if (due == 0) {
        return;
    }
    SwitchRequest req{};
    {
        std::scoped_lock lock{g_switch_mutex};
        if (g_switch_res.run == due || g_switch_req.run != due) {
            return;
        }
        req = g_switch_req;
    }
    const s64 now = NowNs();
    SwitchResult res{};
    res.run = due;
    s32 slot = -1;
    const char* refuse = nullptr;
    u8* const listener = Read<u8*>(player + OffsetListener);
    if (player != req.chr) {
        refuse = "not the main player any more";
    } else if (WeaponStyle(player) != req.from) {
        refuse = "the form changed meanwhile";
    } else if (RightWeapon(player, &slot) != req.weapon) {
        refuse = "the right weapon changed";
    } else if (!Plausible(listener) || Read<u8*>(listener) != g_base + ListenerVtableRva ||
               Read<u8*>(listener + OffsetListenerOwner) != player) {
        refuse = "no TAE listener";
    } else if (Read<u8*>(Read<u8*>(listener) + SlotTaeEvent) != g_base + TaeHandlerRva) {
        refuse = "the TAE listener's handler moved";
    } else if (Hp(player) <= 0) {
        refuse = "dead";
    }
    if (refuse == nullptr) {
        // The game's own TAE event 32 SetWeaponStyle, as the transform clip would send it: the
        // handler reads the type and the params pointer only (first-frame byte set as for a real
        // event).
        alignas(16) std::array<s32, 4> params{req.style, 0, 0, 0};
        alignas(16) std::array<u8, 0x40> event{};
        const s32 type = EventSetWeaponStyle;
        std::memcpy(event.data(), &type, sizeof(type));
        const s32* params_ptr = params.data();
        std::memcpy(event.data() + 8, &params_ptr, sizeof(params_ptr));
        event[0x18] = 1;
        g_tae_event(listener, event.data());
        res.e = WeaponStyle(player);
        res.pgd = PgdStyle(player);
        res.mirror = MirrorStyle(player);
        res.ok = res.e == req.style && res.pgd == req.style && res.mirror == req.style;
        res.why = res.ok ? "" : "the style did not take";
    } else {
        res.why = refuse;
    }
    res.ns = NowNs();
    {
        std::scoped_lock lock{g_switch_mutex};
        g_switch_res = res;
    }
    g_switch_due.store(0, std::memory_order_release);
    LOG_INFO(Core,
             "VR transform: run {} style {} -> {}: {} (E {} PGD {} mirror {}) {:.1f} ms after "
             "due, in {:.0f} us (site A, thread '{}')",
             due, req.from, req.style, res.ok ? "SWITCHED" : fmt::format("FAILED ({})", res.why),
             res.e, res.pgd, res.mirror, static_cast<double>(now - req.due_ns) * 1e-6,
             static_cast<double>(res.ns - now) * 1e-3, Common::GetCurrentThreadName());
}

const char* TransformSwingBlock() {
    return g_swing_block;
}

const TransformRunInfo& TransformRun() {
    return g_info;
}

void TransformMakeFlick(u32 id, u32 window) {
    auto& t = g_tick;
    if (!g_installed || !g_native || t.chr == nullptr || t.run.id != id ||
        !T::GetTransformSettings().flick) {
        return;
    }
    MakeFlick(const_cast<u8*>(t.chr), t, window, NowNs());
}

} // namespace VR::Bloodborne
