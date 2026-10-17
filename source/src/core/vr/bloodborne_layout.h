// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>

#include "common/types.h"

// Bloodborne (v1.09 eboot) layout shared by the readers of the main player's state: the cinematic
// detection (bloodborne_cinematic.cpp) and the motion signals of the dodge comfort camera
// (bloodborne_motion.cpp) in the main player's update, and the VR body's animation gate at the pose
// copy (bloodborne_body.cpp). Addresses are module-relative (patch file address - 0x400000).
// Engine code is only compared, never patched: a mismatch turns the signal that depends on it off.
namespace VR::Bloodborne::Layout {

// Compares `code` with a signature in which -1 matches any byte.
template <size_t N>
bool MatchMasked(const u8* code, const std::array<s16, N>& signature) {
    for (size_t i = 0; i < N; ++i) {
        if (signature[i] >= 0 && code[i] != static_cast<u8>(signature[i])) {
            return false;
        }
    }
    return true;
}

// PlayerIns -> module bag (+0x3B0) and the modules in it.
constexpr u32 OffsetChrModules = 0x3B0;
constexpr u32 OffsetActionFlagModule = 0x08;
constexpr u32 OffsetTimeActModule = 0x18;
constexpr u32 OffsetDataModule = 0x20;
constexpr u32 OffsetBehaviorModule = 0x30;
constexpr u32 OffsetOwner = 0x08;              // module -> owner ChrIns
constexpr u64 ActionFlagVtableRva = 0x53342C0; // SprjChrActionFlagModule
constexpr u64 TimeActVtableRva = 0x5335190;    // SprjChrTimeActModule

// TimeAct history push: 16-byte entries at +0x20, write index at +0xC0.
constexpr u64 TimeActPushRva = 0x1A28173;
constexpr std::array<u8, 16> TimeActPushSig = {
    0x49, 0x63, 0x87, 0xC0, 0x00, 0x00, 0x00, // movsxd rax, dword [r15+0xC0]
    0x48, 0xC1, 0xE0, 0x04,                   // shl rax, 4
    0x41, 0x89, 0x5C, 0x07, 0x20,             // mov [r15+rax+0x20], ebx
};
// TimeAct history: 10 entries {s32 anim id, f32, f32, f32}; entries read..write-1 were activated
// by the last behaviour step (read = write is set later in the frame, at 0x15144B9).
constexpr u32 OffsetTimeActRing = 0x20;
constexpr u32 OffsetTimeActWrite = 0xC0;
constexpr u32 OffsetTimeActRead = 0xC4;
constexpr s32 TimeActRingSize = 10;
constexpr u32 TimeActEntrySize = 16;

// Action flags (module +0x08): reset every frame (0x1A0DF40, from PlayerIns::Update after site A)
// before the behaviour step sets them again, so at site A they hold the last step's values and at
// the pose copy this frame's. 0x02 = event animation (0x01 idle, 0x04 turn, 0x20 damage, 0x40 item
// use, e.g. the vial: these never go flat). HKS act 8 ("in event animation") is the only writer of
// the bit.
constexpr u32 OffsetActionFlags = 0x10;
constexpr u8 FlagEventAnim = 0x02;
constexpr u64 Act8Rva = 0x1A11557;
constexpr std::array<u8, 15> Act8Sig = {
    0x49, 0x8B, 0x87, 0xB0, 0x03, 0x00, 0x00, // mov rax, [r15+0x3B0]
    0x48, 0x8B, 0x40, 0x08,                   // mov rax, [rax+0x08]
    0x80, 0x48, 0x10, 0x02,                   // or byte [rax+0x10], 2
};
// The action request module (+0x80, its owner at OffsetOwner): the requests the playing clip
// accepts this frame, set by its TAE JumpTable events (event 0, jump table 0x1A246F0 indexed by
// id - 1): +0x48 a mask of the request types (bit 0/1 = R1/R2 from event 4, bit 3 = L2 from
// event 16, ...), +0xA0 bit 1 = a move cancels the clip (event 11; HKS env 15 reads it). Only
// read (the body's cancel windows, core/vr/anim_classes.h).
constexpr u32 OffsetActionRequestModule = 0x80;
constexpr u32 OffsetRequestAllow = 0x48;
constexpr u32 OffsetRequestMove = 0xA0;
constexpr u8 RequestMoveBit = 0x02;
constexpr u64 JumpTableRva = 0x1A2173A; // event 0: mov esi, [r12]; dec esi; cmp esi, 0x64; ja
constexpr std::array<u8, 15> JumpTableSig = {
    0x41, 0x8B, 0x34, 0x24, 0xFF, 0xCE, 0x83, 0xFE, 0x64, 0x0F, 0x87, 0x80, 0x2E, 0x00, 0x00,
};
constexpr u64 JumpTable4Rva = 0x1A22B48; // event 4: [bag+0x80]+0x48 |= 3 (R1, R2)
constexpr std::array<u8, 16> JumpTable4Sig = {
    0x48, 0x8B, 0x8A, 0x80, 0x00, 0x00, 0x00, // mov rcx, [rdx+0x80]
    0x48, 0x85, 0xC9, 0x74, 0x04,             // test rcx, rcx; jz
    0x80, 0x49, 0x48, 0x03,                   // or byte [rcx+0x48], 3
};
constexpr u64 JumpTable11Rva = 0x1A22BA9; // event 11: [bag+0x80]+0xA0 |= 2 (a move cancels)
constexpr std::array<u8, 30> JumpTable11Sig = {
    0x49, 0x8B, 0x46, 0x10,                   // mov rax, [r14+0x10]
    0x48, 0x8B, 0x80, 0xB0, 0x03, 0x00, 0x00, // mov rax, [rax+0x3B0]
    0x48, 0x8B, 0x80, 0x80, 0x00, 0x00, 0x00, // mov rax, [rax+0x80]
    0x48, 0x85, 0xC0, 0x74, 0x07,             // test rax, rax; jz
    0x80, 0x88, 0xA0, 0x00, 0x00, 0x00, 0x02, // or byte [rax+0xA0], 2
};
constexpr u64 JumpTable16Rva = 0x1A22BD0; // event 16: [bag+0x80]+0x48 |= 0x400008 (L2)
constexpr std::array<u8, 23> JumpTable16Sig = {
    0x48, 0x8B, 0x82, 0x80, 0x00, 0x00, 0x00,             // mov rax, [rdx+0x80]
    0x48, 0x85, 0xC0, 0x0F, 0x84, 0xE9, 0x19, 0x00, 0x00, // test rax, rax; jz
    0x81, 0x48, 0x48, 0x08, 0x00, 0x40, 0x00,             // or dword [rax+0x48], 0x400008
};

// SprjChrBehaviorModule (+0x30): the "update pose" byte (the behaviour graph runs).
constexpr u32 OffsetBehaviorReady = 0x384;
// ChrDataModule (+0x20): HP (s32).
constexpr u32 OffsetHp = 0xF8;

// SprjChrThrowModule (+0x88; its owner at OffsetOwner) -> ThrowNode (+0x28, owner at OffsetOwner
// too): ThrowState byte +0x20 (0 = none; odd = the hunter throws, even = the hunter is thrown).
// ChrThrowModule::IsInThrow reads exactly this (a duplicate exists at 0x1A61610, so only this
// address counts).
constexpr u64 ThrowVtableRva = 0x5335A20;
constexpr u32 OffsetThrowModule = 0x88;
constexpr u32 OffsetThrowNode = 0x28;
constexpr u32 OffsetThrowState = 0x20;
constexpr u64 IsInThrowRva = 0x1A61550;
constexpr std::array<u8, 20> IsInThrowSig = {
    0x48, 0x8B, 0x47, 0x28,       // mov rax, [rdi+0x28]
    0x48, 0x85, 0xC0, 0x74, 0x08, // test rax, rax; jz
    0x80, 0x78, 0x20, 0x00,       // cmp byte [rax+0x20], 0
    0x0F, 0x95, 0xC0, 0xC3,       // setnz al; ret
    0x31, 0xC0, 0xC3,             // xor eax, eax; ret
};

// HKS act 26 (set ladder action state), the only per-update writer of the ladder state byte:
// PlayerIns -> ChrCtrl (+0x58) -> HKS action state (+0x38) -> +0x60.
constexpr u64 Act26Rva = 0x1A11A67;
constexpr std::array<u8, 14> Act26Sig = {
    0x48, 0x8B, 0x0B,       // mov rcx, [rbx]
    0x48, 0x8B, 0x49, 0x58, // mov rcx, [rcx+0x58]
    0x48, 0x8B, 0x49, 0x38, // mov rcx, [rcx+0x38]
    0x88, 0x41, 0x60,       // mov [rcx+0x60], al
};
// The HKS action state (0xB0 bytes, its +0x08 is the ChrCtrl) holds the ladder state byte.
constexpr u64 ActionStateVtableRva = 0x5325FD0;
constexpr u32 OffsetChrCtrl = 0x58;     // PlayerIns -> ChrCtrl
constexpr u32 OffsetActionState = 0x38; // ChrCtrl -> HKS action state
constexpr u32 OffsetLadderState = 0x60; // s8, -1 = off the ladder

} // namespace VR::Bloodborne::Layout
