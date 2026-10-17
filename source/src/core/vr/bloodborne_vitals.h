// SPDX-License-Identifier: GPL-2.0-or-later

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

// Camera hook installation, before any gauge callback; retains the private visual context.
void InstallVitalsVisual();

// Render-camera hook (loop thread), every driven frame: the gauge's list for this camera.
void VitalsRenderTick(u64 record_id, const float* rows, const float* position);

} // namespace VR::Bloodborne
