// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace VR {

// Overlapping directional shadow cascades (bloodborne_shadow_cascades.cpp): at eboot load, with
// the moon shadows on (VR_SHADOW_OVERLAP=0|1 overrides).
void InstallShadowCascadeOverlap(u64 base, u64 size);

} // namespace VR
