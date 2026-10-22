// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
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
