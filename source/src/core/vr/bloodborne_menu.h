// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include "common/types.h"

// Bloodborne (v1.09 eboot) in-game menu detection (vr.ini menu_mode=1). The game's own "modal menu
// open" pointer (the one its menu-state reader 0x176B7E0 tests: NewMenuSystem +0xA88, non-zero
// from Open 0x1BB2A55 to Close 0x1BB2706) is sampled on the main player's update (Site A, the
// same function that calls the game's reader) and published to the presenter, which then uses
// the menu controls (both sticks step through the menu, A/X confirm, B/Y back) and shows the
// game image on the menu panel (VR::MenuPanelActive; menu_panel=0: on the HUD panel at
// menu_hud_scale). The inventory, notes, Yes/No boxes, shops, the Doll and the lamp all open
// through it; NPC subtitles, item pickup pop-ups and action prompts do not (they stay on the
// gameplay HUD; VR_UI_LOG=1 logs all of these signals). Nothing is patched; any failed check
// keeps gameplay controls.
namespace VR::Bloodborne {

// OnEbootLoaded (VR enabled): remembers the eboot.
void MenuEbootLoaded(VAddr base, u64 size);

// Main player update (Site A): samples the flag and publishes it (VR::PublishGameMenu).
void MenuUpdate(u8* player);
// Site A only: exact job identity, tied to the modal menu serial.
bool MenuIsMessageBox(u64 serial);

} // namespace VR::Bloodborne
