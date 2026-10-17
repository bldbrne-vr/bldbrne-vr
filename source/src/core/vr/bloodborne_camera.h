// SPDX-License-Identifier: GPL-2.0-or-later

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
