// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

// Bloodborne (v1.09 eboot): physical gestures, the game's side (vr.ini pose_gestures; the pure
// logic is core/vr/pose_gestures.h, the recogniser runs in the presenter: openxr_presenter.cpp
// UpdatePoseGestures).
//
// Only two places in the game react to a gesture, and only through its marker SpEffect: the fallen
// Brain of Mensis (164) and the Doll (161 / 162 / 163). In the main player's update (site A,
// bloodborne_heal.cpp) this module
//  - finds the zone: the Doll's observe flag 12100118 (her AI sets it while you stand close in
//    front of her), or the fallen Brain's flag 12600027 with the hunter near region 2602022 - one
//    flag read and one position read a second outside a zone (the game's own GetEventFlag,
//    SprjEventFlagMan *(base+0x553B100));
//  - reads the learned gestures (GestureGameData, [PlayerGameData+0x5B8]) and the gates (host,
//    alive, idle or walking, no ladder, no menu, no cinematic);
//  - applies the marker with the game's own ChrIns::AddSpEffect (PlayerIns vt+0x3F0, 0x18C6DB0),
//    called as the HKS act does (the character as its own source, local only), when the
//    presenter's recognised pose has been held long enough (pose_gestures.h Trigger).
// No gesture is requested (MenuMan+0x2BC is never written) and no animation plays.
namespace VR::Bloodborne {

// OnEbootLoaded, after InstallPlayerHook: checks the signatures. Logs one line.
void InstallPose(VAddr base, u64 size);

// The main player's update (site A): the zone poll, the gates and the markers.
void PoseSiteA(u8* player);

} // namespace VR::Bloodborne
