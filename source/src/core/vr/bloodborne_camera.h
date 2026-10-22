// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include "common/types.h"

// Bloodborne (v1.09 eboot) camera bridge.
//
// Hooks the game's per-frame camera update so the headset can drive the camera
// orientation and FOV (see VR::OnCameraTick). Hook sites follow the research of
// BBMCC (github.com/Mob100percent/BBMCC); every site is signature-checked so other
// games and versions are left untouched.
namespace VR::Bloodborne {

// Called once by the module loader after eboot.bin is mapped and pre-patched.
void OnEbootLoaded(VAddr base, u64 size);

[[nodiscard]] bool IsInstalled();

} // namespace VR::Bloodborne
