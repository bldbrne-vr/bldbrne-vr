// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include "common/types.h"
#include "core/vr/grab_camera.h"

namespace VR::Bloodborne {
void InstallVisceralStage(VAddr base, u64 size);
bool VisceralStageEnabled();
// Called on either side of the native TryThrowAll call, before it can move the pair.
u64 BeginVisceralStage(u8* player);
void ConfirmVisceralStage(u64 token, bool accepted);
void UpdateVisceralStage(u8* player, u8* victim, bool owns_camera, bool cancelled, s64 now);
bool VisceralStagePlacement(s64 now, float scale, GrabCamera::ThrowPlacement& out);
void TraceVisceralCamera(u64 record, const float* position, const float* tracked_offset);
void TraceVisceralDraw(u64 record, const float* expected, const float* actual, const float* scene);
void TraceVisceralView(const char* kind, u64 record, const float* expected, const float* actual);
} // namespace VR::Bloodborne
