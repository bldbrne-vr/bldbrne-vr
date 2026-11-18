// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace VR::Bloodborne {

// OnEbootLoaded (VR enabled; before the game opens its event scripts): with vr_interactions in
// first person, serves the lamp event scripts with lighting a lamp at once, without the hunter's
// animation (core/vr/lamp_light.h). Once per run; logs one line.
void LampLightEbootLoaded();

} // namespace VR::Bloodborne
