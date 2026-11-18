// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cmath>
#include "core/vr/eye_cant.h"
#include "core/vr/eye_fov_cache.h"
#include "core/vr/vr_session.h"
#include "video_core/renderer_vulkan/vr_foveation.h"

namespace Vulkan {

// Each eye's lens centre in its image, as fractions from the left / top edge: where the eye's
// own axis lands (a canted eye's image is its view seen from the head, core/vr/eye_cant.h), else
// where its frustum's tangent is zero. False, and the image centres, until the headset's angles
// are known. Foveation's rate maps (vk_scheduler.cpp) and DLSS focus (vr_dlss_focus.h) use it.
inline bool EyeAxes(const VR::HeadState& head, std::array<float, 2>& cx,
                    std::array<float, 2>& cy) {
    cx = {0.5f, 0.5f};
    cy = {0.5f, 0.5f};
    if (!VR::EyeFov::Valid(head.eye_fov[0]) || !VR::EyeFov::Valid(head.eye_fov[1]))
        return false;
    for (unsigned eye = 0; eye < 2; ++eye) {
        const auto& q = head.eye_pose[eye].orientation;
        const auto f = VR::Cant::Cover(head.eye_fov[eye], q);
        const auto axis = VR::Cant::AxisTangent(q);
        const float l = std::tan(f.angle_left), r = std::tan(f.angle_right);
        const float u = std::tan(f.angle_up), d = std::tan(f.angle_down);
        cx[eye] = VR::Cant::Canted(q) && r > l ? (axis[0] - l) / (r - l)
                                               : Foveation::AxisU(f.angle_left, f.angle_right);
        cy[eye] = VR::Cant::Canted(q) && u > d ? (u - axis[1]) / (u - d)
                                               : Foveation::AxisV(f.angle_up, f.angle_down);
    }
    return true;
}

} // namespace Vulkan
