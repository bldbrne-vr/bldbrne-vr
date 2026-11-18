// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/types.h"
#include "core/vr/bloodborne_corpse.h"
namespace VR::Bloodborne {
void InstallGehrmanChair(VAddr base,u64 size);
void GehrmanChairHands(const CorpseHandsIn& hands);
void GehrmanChairPose(u8* chr);
bool GehrmanChairGiftReady(const u8* player);
bool GehrmanChairEnabled();
bool GehrmanChairGrabAvailable(unsigned hand,s64 now);
// While a hand holds the chair: where it is drawn (its grip on the chair as it now stands, world
// game units). False before any hold.
bool LatestChairGrips(std::array<CorpseGrip,2>& out);
}
