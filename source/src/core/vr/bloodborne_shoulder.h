// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/types.h"
namespace VR::Bloodborne {
void InstallShoulderEquipment(VAddr address, u64 size);
bool RequestShoulderEquipment(unsigned side, bool unarmed);
// The shoulder tap: the hand's other weapon slot selected in the equipment update (as the
// equipment menu does: no switch clip, the hunter stays yours). False when it cannot (the other
// slot holds no weapon, the hook is not in, VR_SHOULDER_NATIVE_CYCLE=0): press the D-pad then.
bool RequestShoulderCycle(unsigned side);
bool ShoulderUnarmed(unsigned side);
int ShoulderRestoreSlot(unsigned side);
void CancelShoulderEquipmentRequests();
} // namespace VR::Bloodborne
