// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include "core/vr/body_frame.h"
#include "core/vr/hand_grip.h"

namespace VR::Lever {
// Estimate the centre enclosed by the closed middle/ring fingers in wrist-local
// space. This is a skeletal contact estimate; mesh thickness still needs VR QA.
// Calculate once from the loaded skeleton, never by scanning a rendered mesh.
inline Body::Vec3 GripCentre(const Body::C0000::Skeleton& sk, Body::C0000::Side side) {
    namespace B = Body;
    const auto& fingers = side == B::C0000::Right ? B::Grip::RightFingers : B::Grip::LeftFingers;
    const auto shape = B::Grip::ShapeOf(B::Grip::Kind::Handle);
    B::Vec3 centre{};
    for (int f : {2, 3}) {
        B::QsTransform joint{};
        B::Vec3 base{};
        for (int j = 0; j < 3; ++j) {
            const auto& ref = sk.local[fingers[f][j]];
            B::QsTransform local{{ref.t[0], ref.t[1], ref.t[2]},
                B::Grip::ShapeLocal(shape, f, j, 1.0f, 1.0f, sk, side), {1, 1, 1}};
            joint = B::Compose(joint, local);
            if (j == 0) base = joint.t;
        }
        centre = centre + (base + joint.t) * 0.25f;
    }
    return centre;
}

// IK targets a weapon bone. Translate that target so the fist's contact centre,
// rather than the animated weapon attachment, lands on the handle. Keeping the
// live attachment here also cancels weapon-specific attachment translations.
inline Body::ModelTarget ContactTarget(Body::ModelTarget handle,
                                      const Body::QsTransform& attachment,
                                      Body::Vec3 centre) {
    const auto wrist = Body::Normalize(handle.q * Body::Conjugate(Body::Normalize(attachment.q)));
    handle.p = handle.p + Body::Rotate(wrist, attachment.t - centre);
    return handle;
}
} // namespace VR::Lever
