// SPDX-License-Identifier: GPL-2.0-or-later
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
