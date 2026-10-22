// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <array>
#include <cmath>

namespace VR::EyeFov {
// Deliberately generic: the same validity contract is used at publication and
// by the game-thread cache, and can be tested without an OpenXR/Vulkan device.
template <class Fov>
bool Valid(const Fov& f) {
    return std::isfinite(f.angle_left) && std::isfinite(f.angle_right) &&
           std::isfinite(f.angle_up) && std::isfinite(f.angle_down) && f.angle_left != 0 &&
           f.angle_right != 0 && f.angle_up != 0 && f.angle_down != 0 &&
           f.angle_left < f.angle_right && f.angle_down < f.angle_up;
}
template <class Fov>
void Update(std::array<Fov, 2>& cached, const std::array<Fov, 2>& sample) {
    for (unsigned eye = 0; eye < 2; ++eye)
        if (Valid(sample[eye]))
            cached[eye] = sample[eye];
}
} // namespace VR::EyeFov
