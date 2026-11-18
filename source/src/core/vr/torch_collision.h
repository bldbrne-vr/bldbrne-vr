// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "core/vr/solid_weapon.h"

namespace VR::Torch {

// Retail 1.09 EquipParamWeapon: families 200 and 201 both use wp_a_1800 on dummy 340.
// Its solid mesh spans Y [-0.7562, 0.1902] m, X [-0.0455, 0.0419],
// Z [-0.0686, 0.0331]. One capsule approximates the wood and head, not the flame.
// The attachment basis is the skeleton's dummy 340 in L_Weapon coordinates.
inline bool CollisionShape(int family, bool visible, float scale, Body::Vec3 origin,
                           Body::Vec3 forward, Body::Vec3 up, Solid::Shape& out) {
    if ((family != 200 && family != 201) || !visible || !std::isfinite(scale) ||
        scale <= 0 || !Body::IsFinite(origin) || !Body::IsFinite(forward) ||
        !Body::IsFinite(up) || Body::LengthSq(forward) < 0.5f ||
        Body::LengthSq(up) < 0.5f || Body::LengthSq(Body::Cross(up, forward)) < 0.5f)
        return false;
    const auto point = [&](float y) {
        return origin + up * y + forward * -0.018f;
    };
    out = {};
    out.hand_r = 0.05f * scale;
    out.line = true;
    out.base = point(0.140f);
    out.tip = point(-0.706f);
    out.line_r = 0.05f * scale;
    return true;
}

} // namespace VR::Torch
