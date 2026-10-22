// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include "core/vr/arm_ik.h"
#include "core/vr/quick_items.h"

namespace VR::Quick {
// Model origin and unit axes in the palm's space. Shared by special item drawing and contact.
struct ItemFrame { Body::Vec3 origin, x, y, z; };
template <size_t N>
ItemFrame ItemFrameOf(const std::array<float, N>& m, const HeldLook& look,
                      float scale_multiplier = 1.0f) {
    using namespace Body;
    const Vec3 dx = Normalize(Vec3{m[0], m[4], m[8]});
    const Vec3 dy = Normalize(Vec3{m[1], m[5], m[9]});
    const Vec3 dz = Normalize(Vec3{m[2], m[6], m[10]});
    const auto dir = [&](Vec3 v) { return dx*v.x + dy*v.y + dz*v.z; };
    constexpr float deg = 3.14159265358979f / 180.0f;
    const Quat turn = AxisAngle({1,0,0}, look.pitch_deg*deg) *
                      AxisAngle({0,0,1}, look.roll_deg*deg) *
                      AxisAngle({0,1,0}, look.yaw_deg*deg);
    const Vec3 centre{GripCentre[0]+look.side_m, GripCentre[1]+look.lift_m,
                      GripCentre[2]+look.out_m};
    const Vec3 pivot{look.hold_x, look.hold_y, look.hold_z};
    return {Vec3{m[3],m[7],m[11]} + dir(centre - Body::Rotate(turn, pivot)*
                                     (look.scale*scale_multiplier)),
            dir(Body::Rotate(turn, {1,0,0})), dir(Body::Rotate(turn, {0,1,0})),
            dir(Body::Rotate(turn, {0,0,1}))};
}
} // namespace VR::Quick
