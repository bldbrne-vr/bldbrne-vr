// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include "core/vr/body_frame.h"
#include "core/vr/hand_grip.h"

namespace VR::MusicBox {
inline Body::Grip::Shape CrankPinch() {
    auto s = Body::Grip::ShapeOf(Body::Grip::Kind::Pinch);
    constexpr float r = 3.14159265359f / 180.0f;
    s.bend[0] = {0, 27*r, 26*r};
    s.bend[1] = {35*r, 55.25f*r, 28.25f*r};
    s.spread = {};
    s.thumb_oppose = 35*r;
    return s;
}
inline constexpr std::array<float, 2> TipLengths{.024f, .020f};
struct PinchContact {
    Body::Vec3 centre{};
    Body::Quat basis{}; // X across pads, Z from wrist toward the pinch
};
inline PinchContact CrankContact(const Body::C0000::Skeleton& sk) {
    namespace B = Body;
    const auto shape = CrankPinch();
    std::array<B::Vec3, 2> tip{};
    for (int f = 0; f < 2; ++f) {
        B::QsTransform joint{};
        for (int j = 0; j < 3; ++j) {
            const auto& ref = sk.local[B::Grip::RightFingers[f][j]];
            joint = B::Compose(joint, {{ref.t[0], ref.t[1], ref.t[2]},
                B::Grip::ShapeLocal(shape, f, j, 1, 1, sk, B::C0000::Right), {1,1,1}});
        }
        tip[f] = joint.t + B::Rotate(joint.q, {TipLengths[f], 0, 0});
    }
    const auto centre = (tip[0] + tip[1]) * .5f;
    const auto x = B::Normalize(tip[1] - tip[0]);
    const auto z = B::Normalize(centre - x*B::Dot(centre, x));
    return {centre, B::FromBasis(x, B::Cross(z, x), z)};
}
// The crank frame puts +X along the box's up direction. The pinch's +X runs
// from thumb to index, so roll it half a turn to keep the thumb above the index.
// Keep +Z pointing toward the knob: the wrist must remain outside the case.
inline constexpr Body::Quat CrankWristRoll{0, 0, 1, 0};
// Contact frame +Z points from the wrist toward the knob. Orient the actual
// hunter pinch, then compensate the live weapon attachment that IK targets.
inline Body::ModelTarget CrankHandTarget(Body::ModelTarget contact,
                                        const Body::QsTransform& attachment,
                                        const PinchContact& pinch) {
    const auto wrist = Body::Normalize(contact.q * CrankWristRoll * Body::Conjugate(pinch.basis));
    return {contact.p + Body::Rotate(wrist, attachment.t - pinch.centre),
            Body::Normalize(wrist * attachment.q)};
}
} // namespace VR::MusicBox
