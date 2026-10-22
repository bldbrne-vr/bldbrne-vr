// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
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
