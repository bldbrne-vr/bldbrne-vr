// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include "common/types.h"

// Bloodborne (v1.09 eboot): the enemies' HP bars on or off (vr.ini ui_enemy_hp_bars, the VR page's
// Graphics > Show enemy health bars; default on).
//
// What the game does: the bars over the enemies you hit or lock on to are the HUD movie's 8
// EnemyTag%d tags (the HUD's update 0x1BF9910 region keeps them at HUD +0x68..+0xA0, built by
// 0x1B58E40 like the 7 RemotePCTag%d tags at +0xA8..+0xD8 and the 3 boss bars of the BossTop list at
// +0xE0..+0xF0, which share the same tag update; each tag is a clip Fade/ItemTop with HP
// (Current/Bar, Delay/Bar, BarFrame/Bar), Name/Value and Damage/Value: the damage number). Their
// data come from MenuMan +0x10F8 (8 entries of 0x98 bytes, +0 the chr handle, -1 = free) that a
// hit (0x1A2EA90 -> 0x16F9A50) and the lock-on (0x1891FEB -> 0x16F9710) fill. Every frame the HUD
// calls the tag update 0x1B59E20 for each tag; it ends with the tag's visibility, shown when its
// entry is shown and its HP above 0: SetVisible(tag +0x58, al) (0x1ACC810) at 0x1B5A396.
//
// Here: a 15-byte detour at 0x1B5A396 (that call and the next rip-relative load, replayed in the
// stub) clears al while ui_enemy_hp_bars is 0 (read from the live settings at every update) when
// the tag is one of the HUD's 8 EnemyTags (the caller's r15, saved at [rbp-8]): the whole tag is
// hidden, its bar and its damage number (one widget), live both ways, on every view of the UI
// (the UI layer's rest quad, any HUD layout, ui_layer=0, the desktop window). Nothing else
// changes: the entries and the tags' own updates run as the game runs them, the RemotePCTags (other
// players), the boss bar (BossTop), the lock-on dot (TargetCursor), the hunter's own HUD, items,
// messages and banners are other clips. A signature mismatch leaves the bars as the game shows
// them, logged. (The older FrpgMenuDlgFEEnemyGauge element code at 0x1654D80 is never run by
// v1.09: measured, 0 updates with bars on screen.)
namespace VR::Bloodborne {

// OnEbootLoaded (VR enabled). Installed whatever ui_enemy_hp_bars is at the start (the VR page
// switches it live). Logs one line.
void InstallEnemyBars(VAddr base, u64 size);

// Main player update (Site A): logs when the hiding starts or stops (the stub's count of enemy
// tags the game would have shown); with VR_ENEMY_BARS_LOG every second the counts.
void EnemyBarsUpdate();

// The enemy tags on the curved rest panel (ui_rest_curved; the presenter, every UI frame): the
// flat game picture's half width and height in tangent units (the HUD's 1920 x 1080 stage spans
// them). On: every enemy tag the game places gets the x / y scale that undoes the flat picture's
// squeeze where it is (1 + t^2 across, more for high tags), so it has the same size and shape
// anywhere in the view and no longer looks turned. Off: the game's own scale (100 %).
void SetEnemyTagScale(bool on, float half_w, float half_h);

// A boss bar raised near the top (ui_boss_bar = 1) was updated in the last 0.5 s: while it is,
// the HP cluster's cut stops at ui_layout.h StatusLeftBossCutX (any thread).
[[nodiscard]] bool BossBarUp();

} // namespace VR::Bloodborne
