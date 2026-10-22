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
#include <optional>
#include <openxr/openxr.h>

namespace Vulkan::VrUiGeometry {
// Value-only description shared by XR layers and the eye compositor. No tracking or GPU state.
struct Piece {
    XrPosef pose{{0, 0, 0, 1}, {0, 0, 0}};
    XrRect2Di rect{};
    float width{}, height{}, radius{}, angle{};
    std::array<float, 4> tint{1, 1, 1, 1};
    bool view_space{};
};
inline std::optional<Piece> FromLayer(const XrCompositionLayerBaseHeader& layer,
                                    XrSpace view_space) {
    Piece p{};
    p.view_space = layer.space == view_space && view_space != XR_NULL_HANDLE;
    if (layer.type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
        const auto& q = reinterpret_cast<const XrCompositionLayerQuad&>(layer);
        p.pose = q.pose;
        p.rect = q.subImage.imageRect;
        p.width = q.size.width;
        p.height = q.size.height;
    } else if (layer.type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) {
        const auto& c = reinterpret_cast<const XrCompositionLayerCylinderKHR&>(layer);
        if (!(c.radius > 0 && c.centralAngle > 0 && c.aspectRatio > 0)) return {};
        p.pose = c.pose;
        p.rect = c.subImage.imageRect;
        p.radius = c.radius;
        p.angle = c.centralAngle;
        p.width = c.radius * c.centralAngle;
        p.height = p.width / c.aspectRatio;
    } else {
        return {};
    }
    if (p.rect.extent.width <= 0 || p.rect.extent.height <= 0 ||
        !(p.width > 0 && p.height > 0)) return {};
    for (auto* next = static_cast<const XrBaseInStructure*>(layer.next); next;
         next = next->next) {
        if (next->type == XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR) {
            const auto& c = *reinterpret_cast<const XrCompositionLayerColorScaleBiasKHR*>(next);
            p.tint = {c.colorScale.r, c.colorScale.g, c.colorScale.b, c.colorScale.a};
        }
    }
    return p;
}
// OpenXR cylinder coordinates: centre points along -Z, height is arc length / aspect.
// UV Y grows down; local Y grows up. The GPU vertex shader implements this same surface.
inline std::array<float, 3> Surface(const Piece& p, float u, float v) {
    const float a = (u - 0.5f) * p.angle;
    return {p.radius > 0 ? p.radius * std::sin(a) : (u - 0.5f) * p.width,
            (0.5f - v) * p.height, p.radius > 0 ? -p.radius * std::cos(a) : 0.0f};
}
} // namespace Vulkan::VrUiGeometry
