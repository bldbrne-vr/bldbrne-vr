// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include "common/types.h"

// Bloodborne (v1.09 eboot) instant Blood Vial injection.
//
// Hooks PlayerIns::Update once per character update. For the main player it takes a pending
// VR::RequestInject() and calls the game's own PlayerIns::UseGoods(1000), the function the
// vial animation's event runs, so the heal (rune modifiers, rally trimming), the consume and
// the HUD update are exactly vanilla, just without the animation. Every site and called
// function is signature-checked; on any mismatch nothing is patched and a stab is refused
// (never the game's animated vial in its place; inject=0 makes X the game's vial button).
//
// The same hook drives the cinematic flat mode (core/vr/bloodborne_cinematic.h), the dodge
// signals and the in-game menu detection (core/vr/bloodborne_menu.h), so it is installed when any
// of them is on (vr.ini inject, cinematic, dodge_camera, menu_mode) and its own checks pass.
namespace VR::Bloodborne {

// Called from OnEbootLoaded (VR enabled only).
void InstallPlayerHook(VAddr base, u64 size);

// The instant injection is installed (it may still disable itself later).
[[nodiscard]] bool IsHealInstalled();

// The player update hook is installed (the menu detection samples in it).
[[nodiscard]] bool IsPlayerHookInstalled();

// The Blood Vials carried, as the last main-player update read them (vial_inject_drain only); -1
// when unknown (the drain off, no instant injection).
[[nodiscard]] s32 CarriedBloodVials();

} // namespace VR::Bloodborne
