// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

// The VR tile in Bloodborne's Options grid (vr.ini vr_menu=1, the default; 0 = nothing patched, the
// game's own grid). A native 4th tile in row 0, right of System, in the slot FromSoft's debug
// builds use for their "Debug" tile: the game's own cursor, highlight, sound, D-pad navigation and
// row label ("VR"), our handler on A. Code side: core/vr/vr_tile_patch.h (signature-checked sites);
// movie side: core/vr/gfx_tile.h (Item_0_3 with our icon, served by the emulator in place of
// game asset, core/file_sys/file_override.h). Any failed check leaves the grid untouched.
// Also with vr_menu=1: a read-only probe of the grid's cursor (the log's `VR tile probe:` lines).
namespace VR::Bloodborne {

// OnEbootLoaded (VR enabled, via MenuEbootLoaded): checks every site, installs or logs why not.
void VrTileEbootLoaded(VAddr base, u64 size);

// MenuUpdate (player update, menus on): follows the cursor while the Options grid is open.
void VrTileMenuUpdate(bool menu_open);

// The tile is in the grid (all patches in, movie served).
bool IsVrTileInstalled();

// The cell the Options grid's cursor is on (0 Inventory, 1 Stats, 2 System, 3 VR, ...; the
// game's override when set, as its own lambdas read it), -1 when not known (vr_menu=0, the probe
// not installed, or no grid refresh seen since the menu opened). Game thread, after
// VrTileMenuUpdate (MenuUpdate publishes it for the UI layer's menu canvas).
s32 VrTileCursor();

// How many times A was pressed on the VR tile (the VR page's trigger, later). The game calls the
// handler twice per press (same frame); those count once.
u32 VrTilePresses();

} // namespace VR::Bloodborne
