// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include "common/types.h"

// Bloodborne (v1.09 eboot) immersive vitals (vr.ini hud_vitals=wrist, core/vr/vitals.h):
//  - site A (the main player's update, core/vr/bloodborne_heal.cpp) reads HP, max HP, the rally
//    pool, stamina and the bullets (ChrDataModule, the inventory through the game's own
//    FindItemIndex, signature-checked) and publishes them (core/vr/vitals_channel.h);
//  - the render-camera hook builds the Blood Gauge on the hunter's left wrist: placed on his
//    forearm from the game's final pose (GetDmyPolyMtx dummies 3 and 344 on L_Hand, 153 on
//    L_Forearm), fitted to whatever he wears from the game's own depth around it (an in-frame
//    depth probe, VitalsFit), drawn into the game's frame with the game's camera, light and
//    depth (core/vr/inframe.h, its own list: Producer::Vitals).
namespace VR::Bloodborne {

// Site A: the main player's update. `module` = the eboot base.
void VitalsSiteA(u8* module, u8* player);

// Render-camera hook (loop thread), every driven frame: the gauge's list for this camera.
void VitalsRenderTick(u64 record_id, const float* rows, const float* position);

} // namespace VR::Bloodborne
