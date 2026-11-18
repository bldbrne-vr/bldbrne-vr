// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace VR::Bloodborne {

// Moon shadows in the Forbidden Woods (core/vr/woods_shadows.h): registers the patched map file
// and draw parameters with the file override, once, before the game opens them.
void WoodsShadowsEbootLoaded();

// The fog's lamps and the walls round them (core/vr/map_lights.h Reach): once a rendered frame on
// the game's loop thread, the hunter's place in the world. One lamp of his map near him that has
// not been measured yet is (14 casts of the game's own collision query: some 50 us, and nothing
// once the lamps near him are done). The ground under the air's mist too (core/vr/mist_ground.h:
// a few casts while a cell of its lattice near him is not known yet).
void FogLampReachTick(float x, float y, float z);

} // namespace VR::Bloodborne
