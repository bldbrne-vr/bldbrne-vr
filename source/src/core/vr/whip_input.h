// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include "core/vr/body_frame.h"
#include "core/vr/whip_chain.h"

namespace VR::Whip {
// Same dummy-poly basis as the game's attachment, built from the current written body pose.
inline bool AttachmentFrame(const B::ChrFrame& character, const B::QsTransform& bone,
                            B::Vec3 offset, B::Vec3 forward, B::Vec3 up, Frame& out) {
    if (!character.valid || !B::IsFinite(bone)) return false;
    const auto q = B::Normalize(bone.q);
    const auto z = B::Normalize(B::RotateToWorld(character, B::Rotate(q, forward)));
    const auto y0 = B::Normalize(B::RotateToWorld(character, B::Rotate(q, up)));
    const auto x = B::Normalize(B::Cross(y0, z));
    if (B::LengthSq(x) < .5f || B::LengthSq(z) < .5f) return false;
    out = {B::ToWorld(character, bone.t + B::Rotate(q, B::Mul(bone.s, offset))),
           B::FromBasis(x, B::Cross(z, x), z)};
    return B::IsFinite(out.t) && B::IsFinite(out.q);
}

// Part animation can be evaluated more than once for one body pose. Advance physics once
// on its source timestamp, never on the time between two callbacks in the same update.
class PoseClock {
public:
    float Step(std::int64_t ns) {
        if (ns <= last_) return 0.0f;
        const float dt = last_ != 0 ? static_cast<float>(ns - last_) * 1e-9f : 0.0f;
        last_ = ns;
        return dt;
    }
private:
    std::int64_t last_{};
};
} // namespace VR::Whip
