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

#include "core/vr/arm_ik.h"
#include "core/vr/c0000_skeleton.h"

// VR body: posture measurements of a c0000 model-space pose (the hunter's body shape: where the
// waist, chest and eye are, how the trunk leans and twists). Pure math, shared by the pose hook's
// VR_BODY_POSTURE_LOG (core/vr/bloodborne_body.cpp) and the unit tests / record replays
// (tests/test_body_solver.cpp). Model space: metres, +Y up, the hunter faces -Z, his right on -X.
namespace VR::Body {

struct Posture {
    Vec3 pelvis{}; // Pelvis (8): the hips, carries the legs
    Vec3 waist{};  // Spine (44): the bottom of the trunk above the hips
    Vec3 chest{};  // Spine2 (48): the top of the spine, under the collar
    Vec3 neck{};   // Neck (77)
    Vec3 eye{};    // the midpoint of the eyes (Skeleton::eye_center_on_head on Head)
    // The trunk, pelvis -> neck: its length and its lean from vertical (degrees; forward = toward
    // -Z, right = toward -X).
    float trunk{0.0f};
    float lean_fwd{0.0f};
    float lean_right{0.0f};
    // Headings (degrees, about +Y, 0 = model forward, + = to the left) of the hips (Pelvis) and
    // of the chest (Spine2): where each bone's rotation from its bind pose turns the model's
    // forward; the waist twist between them (chest - hips). The arms' clavicles (the IK's shrug
    // and reach) do not count.
    float hips_yaw{0.0f};
    float chest_yaw{0.0f};
    float twist{0.0f};
    // The chest's offset from the hips (chest - pelvis, cm-scale shear checks) in model axes.
    Vec3 chest_from_pelvis{};
};

// The heading (degrees) a bone's model rotation `q` gives the model's forward, against its bind
// pose model rotation `bind`.
inline float BoneHeadingDeg(const Quat& q, const std::array<float, 4>& bind) {
    const Quat r = Normalize(q) * Conjugate(Normalize(Quat{bind[0], bind[1], bind[2], bind[3]}));
    const Vec3 f = Rotate(r, Vec3{0.0f, 0.0f, -1.0f});
    return std::atan2(f.x, -f.z) / Deg;
}

// `sk`: the hunter's skeleton (the runtime one; the caller has checked C0000::Active()).
inline Posture MeasurePosture(const QsTransform* model,
                              const C0000::Skeleton& sk = C0000::Ref()) {
    namespace C = C0000;
    Posture p{};
    p.pelvis = model[C::Pelvis].t;
    p.waist = model[C::Spine].t;
    p.chest = model[C::Spine2].t;
    p.neck = model[C::Neck].t;
    const QsTransform& head = model[C::Head];
    p.eye = head.t + Rotate(Normalize(head.q), Vec3{sk.eye_center_on_head[0],
                                                    sk.eye_center_on_head[1],
                                                    sk.eye_center_on_head[2]});
    const Vec3 trunk = p.neck - p.pelvis;
    p.trunk = Length(trunk);
    p.lean_fwd = std::atan2(-trunk.z, trunk.y) / Deg;
    p.lean_right = std::atan2(-trunk.x, trunk.y) / Deg;
    p.hips_yaw = BoneHeadingDeg(model[C::Pelvis].q, sk.model[C::Pelvis].q);
    p.chest_yaw = BoneHeadingDeg(model[C::Spine2].q, sk.model[C::Spine2].q);
    p.twist = WrapPi((p.chest_yaw - p.hips_yaw) * Deg) / Deg;
    p.chest_from_pelvis = p.chest - p.pelvis;
    return p;
}

} // namespace VR::Body
