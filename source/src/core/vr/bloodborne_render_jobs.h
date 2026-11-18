// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/types.h"

namespace VR::Bloodborne {
// Repair the 1.09 render-job pool's failure/cancellation exits before guest threads run.
void RenderJobsEbootLoaded(VAddr base, u64 size);
} // namespace VR::Bloodborne
