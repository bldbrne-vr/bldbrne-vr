// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include "common/types.h"

// Bloodborne (v1.09 eboot) TEST / DEBUG kit: vr.ini give_kit (env VR_GIVE_KIT overrides it), off
// by default. Once per session, the first time the main player is in the world (alive, no game
// menu open), it tops the inventory up to the kit's quantities and adds the kit's weapons that are
// not owned, then (give_kit_equip=1, env VR_GIVE_KIT_EQUIP) equips them into EMPTY weapon slots
// (Bare Fists) and fills EMPTY quick-item slots (VR_GIVE_KIT_EQUIP=quick: only the quick slots,
// the weapon slots are never touched). Everything goes through the game's own code, so
// the game writes a valid save itself at its next autosave:
//   - EquipGameData::AddItem (0x14CDFD0, what the shop purchase and the debug "item get" call),
//     clamped to the room the shop itself computes (0x1B5CA20), no trophy notification;
//   - the Equipment menu's own equip action (0x1B63E00) for weapon slots and quick slots.
// Quantities are top-up targets: only the difference is given, never more, so leaving it on just
// restocks. Nothing the player chose is ever replaced. Every change is logged ("VR kit: ...").
//
// VR_MAX_STATS=1 separately grants all six attributes at 99 (level 544), once per process,
// through the signature-checked level-up request handler (0x1C13D00). It also recalculates HP,
// stamina and derived stats. The private --max-stats launcher backs up the save first; use the
// game's Quit Game to persist. Off by default, and does not depend on give_kit being enabled.
//
// give_kit: 0 = off; 1 (or "kit") = the default kit; or a list "<id>[:<qty>],..." (goods ids;
// ids >= 1000000 are weapons, given once if no weapon of that family is owned), which may include
// "kit" to add the default kit.
namespace VR::Bloodborne {

// The main player's update (site A, core/vr/bloodborne_heal.cpp). `module` = the eboot base.
void GiveKitSiteA(u8* module, u8* player);

} // namespace VR::Bloodborne
