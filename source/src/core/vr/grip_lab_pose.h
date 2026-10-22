// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <array>
namespace VR::Bloodborne {
// Opt-in grip lab snapshot, read on the game's render thread after GetDmyPolyMtx.
struct GripLabPose {
    std::array<float, 12> world{};
    // translation xyz, quaternion xyzw, scale xyz, in Havok's 108-bone order.
    std::array<std::array<float, 10>, 108> local{}, model{};
};
bool ReadGripLabPose(GripLabPose& out);
}
