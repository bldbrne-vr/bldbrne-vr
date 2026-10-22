// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>

#include "core/vr/gfx_tile.h"

// The VR tile's code side: the patch sites in the v1.09 eboot (EU CUSA03173) and their byte
// signatures (header-only; tests/test_vr_tile.cpp checks them against the real eboot).
//
// The Options grid (menu L"IngameTop") is built by 0x1BDE180 on every open: 24 cells, row 0 =
// Inventory, Stats, System, then cell 3, which only FromSoft's debug builds fill (a "Debug" tile
// gated by the debug parameter L"MenuMan.EnableTopMenuDebug", read by 0x176DE30, called only
// there); retail adds an empty, unselectable cell. The VR tile takes that branch with our own
// label, help line and handler:
//   gate     0x176DE30: `mov al, 1; ret` (the debug parameter reads as on);
//   label    0x1BDE5DF `lea rax, [L"Debug"]`        -> our L"VR" (the 6 other users of L"Debug"
//                                                        keep it);
//   help     0x1BDE664 `lea rax, [L"(Japanese)"]`   -> our help line;
//   handler  0x1BDE7F2 `lea rax, [0x1BE29C0]`        -> our trampoline to the host handler.
// The handler is a plain function pointer wrapped by 0x1B8FBE0 (functor vtable 0x533EF00), whose
// call slot 0x1B8FC60 runs `handler(out, arg + 0x48)` and returns out: the job the grid opens
// (null = no sub-menu, as System's own handler 0x1BDB9E0 writes when its list fails).
namespace VR::TilePatch {

using u8 = std::uint8_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;

constexpr u64 GateRva = 0x176DE30;
constexpr std::array<u8, 13> GateSig = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41,
                                        0x56, 0x53, 0x48, 0x83, 0xEC, 0x48};
constexpr std::array<u8, 3> GatePatch = {0xB0, 0x01, 0xC3}; // mov al, 1; ret

// The builder's whole debug branch (call gate .. both branches' AddEmpty), FNV-1a 64 before any
// patch: every instruction between the gate and the AddItem call is the one read statically.
constexpr u64 BranchRva = 0x1BDE5D2;
constexpr u64 BranchEnd = 0x1BDE934;
constexpr u64 BranchFnv = 0xbf78a8ef516980edull;

constexpr std::size_t LeaLength = 7;
constexpr u64 LabelLeaRva = 0x1BDE5DF;
constexpr std::array<u8, LeaLength> LabelLeaSig = {0x48, 0x8D, 0x05, 0xBC, 0xFF, 0xDE, 0x02};
constexpr u64 HelpLeaRva = 0x1BDE664;
constexpr std::array<u8, LeaLength> HelpLeaSig = {0x48, 0x8D, 0x05, 0x59, 0xAB, 0xDB, 0x02};
constexpr u64 HandlerLeaRva = 0x1BDE7F2;
constexpr std::array<u8, LeaLength> HandlerLeaSig = {0x48, 0x8D, 0x05, 0xC7, 0x41, 0x00, 0x00};

constexpr u64 FunctorCallRva = 0x1B8FC60;
constexpr std::array<u8, 29> FunctorCallSig = {
    0x55, 0x48, 0x89, 0xE5, 0x53, 0x50, 0x48, 0x89, 0xF0, 0x48, 0x89, 0xFB, 0x48, 0x8D, 0x72,
    0x48, 0xFF, 0x50, 0x08, 0x48, 0x89, 0xD8, 0x48, 0x83, 0xC4, 0x08, 0x5B, 0x5D, 0xC3};

// The probe: the IngameTop controller's refresh (vtable 0x539C680 slot 3, rdi = the controller;
// base refresh 0x1BE8CF0, then the Row label 0x1BDF810 and the icons 0x1BDFB10). Its first 9
// bytes are position independent; the 10th starts `call 0x1BE8CF0`, which the stub replays as an
// absolute call.
constexpr u64 RefreshRva = 0x1BE0220;
constexpr std::array<u8, 14> RefreshSig = {0x55, 0x48, 0x89, 0xE5, 0x53, 0x50, 0x48,
                                           0x89, 0xFB, 0xE8, 0xC2, 0x8A, 0x00, 0x00};
constexpr std::size_t RefreshPiLength = 9;
constexpr u64 RefreshCallRva = 0x1BE8CF0;
constexpr u64 ControllerVtableRva = 0x539C680;
// The controller's grid. The game's lambdas take +0xAE8 when >= 0, else +0xAE4. Measured (mock
// run vrmenu/runs/t1c, 2026-10-05): +0xAE4 is the selected cell (row * 6 + column, it follows every
// D-pad step, 0 at every open) and +0xAE8 stays -1 (an override nothing set in the runs).
// The refresh runs once per open, not per step: the cursor is polled in the player update.
constexpr std::size_t OffsetCellCount = 0xAE0; // 24
constexpr std::size_t OffsetSelected = 0xAE4;  // the selected cell
constexpr std::size_t OffsetOverride = 0xAE8;  // -1; when >= 0 the lambdas use it instead
constexpr std::size_t OffsetColumns = 0xAEC;   // 6

constexpr u64 MinModuleSize = 0x539C680 + 8;

constexpr char16_t Label[] = u"VR";
constexpr char16_t Help[] = u"Change virtual reality settings.";

// `lea rax, [rip + disp32]` at `lea_address` pointing at `target`, when it reaches.
inline std::optional<std::array<u8, LeaLength>> LeaTo(u64 lea_address, u64 target) {
    const auto delta = static_cast<std::int64_t>(target - (lea_address + LeaLength));
    if (delta < INT32_MIN || delta > INT32_MAX) {
        return std::nullopt;
    }
    const auto d = static_cast<std::uint32_t>(static_cast<s32>(delta));
    return std::array<u8, LeaLength>{0x48,
                                     0x8D,
                                     0x05,
                                     static_cast<u8>(d),
                                     static_cast<u8>(d >> 8),
                                     static_cast<u8>(d >> 16),
                                     static_cast<u8>(d >> 24)};
}

// "" when every tile site holds the retail bytes, else the first site that does not.
inline std::string CheckTileSites(const u8* module, u64 size) {
    if (size < MinModuleSize) {
        return "eboot too small";
    }
    const auto same = [module](u64 rva, std::span<const u8> sig) {
        return std::memcmp(module + rva, sig.data(), sig.size()) == 0;
    };
    if (!same(GateRva, GateSig)) {
        return "gate 0x176DE30";
    }
    if (GfxTile::Fnv1a64({module + BranchRva, BranchEnd - BranchRva}) != BranchFnv) {
        return "builder branch 0x1BDE5D2";
    }
    if (!same(LabelLeaRva, LabelLeaSig) || !same(HelpLeaRva, HelpLeaSig) ||
        !same(HandlerLeaRva, HandlerLeaSig)) {
        return "label / help / handler lea";
    }
    if (!same(FunctorCallRva, FunctorCallSig)) {
        return "functor call 0x1B8FC60";
    }
    return {};
}

inline bool CheckRefreshSite(const u8* module, u64 size) {
    return size >= MinModuleSize &&
           std::memcmp(module + RefreshRva, RefreshSig.data(), RefreshSig.size()) == 0;
}

// The gate and the three leas, as the tile patch leaves them (the vr_menu=1 install log).
inline bool TileSitesPatched(const u8* module, u64 size) {
    return size >= MinModuleSize &&
           std::memcmp(module + GateRva, GatePatch.data(), GatePatch.size()) == 0 &&
           std::memcmp(module + LabelLeaRva, LabelLeaSig.data(), 3) == 0 &&
           std::memcmp(module + LabelLeaRva + 3, LabelLeaSig.data() + 3, 4) != 0 &&
           std::memcmp(module + HelpLeaRva + 3, HelpLeaSig.data() + 3, 4) != 0 &&
           std::memcmp(module + HandlerLeaRva + 3, HandlerLeaSig.data() + 3, 4) != 0;
}

} // namespace VR::TilePatch
