// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "common/types.h"

namespace VR::Bloodborne {
// Installs the 1.09 physical lever adapter (core/vr/lever_models.h) before guest threads start.
// The live physical_levers setting replaces the normal prompt per lever instance: only where
// that object's measured rig binds and a hand can pull it now (core/vr/lever_prompt.h); every
// other action, a listed row on a model without a rig, and every lever while no hand can pull
// (the virtual screen, third person, untracked) keeps its prompt and button.
// VR_LEVER_PROBE=1 adds diagnostics; VR_LEVER_PHYSICAL_TEST=0 disables the adapter.
// VR_LEVER_MOTION_TEST=1 explicitly enables permission-bypassing rehearsal without
// gameplay dispatch. Ordinary launches respect native permissions and dispatch once.
void InstallLeverProbe(VAddr base, u64 size);
} // namespace VR::Bloodborne
