// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include "core/vr/arm_ik.h"
#include "core/vr/body_frame.h"
#include "core/vr/pouch_armor_fits.h"

namespace VR::BodyMounts {
using Body::Vec3;
// Model-space rigid frames; no headset-relative lag or per-item simulation.
struct Frame {
    Vec3 origin{}, right{-1, 0, 0}, up{0, 1, 0}, back{0, 0, 1};
};
struct Pose {
    std::array<Frame, 2> frame{}; // pelvis, chest
    std::uint32_t space{};
    bool valid{};
    Body::ChrFrame character{}; // same pose update as the bones, not the newer live ChrCtrl
};
inline Frame OfBone(const Body::QsTransform& posed, Body::Quat reference) {
    const auto turn = Body::Normalize(posed.q * Body::Conjugate(Body::Normalize(reference)));
    return {posed.t, Body::Rotate(turn, {-1, 0, 0}), Body::Rotate(turn, {0, 1, 0}),
            Body::Rotate(turn, {0, 0, 1})};
}
// The surface on which the back of the object rests. Position and orientation follow
// the same bone, including torso lean and animated pelvis rotation. Adjustments are
// bone-local, so they do not drift when the player turns or looks down.
inline Frame Surface(const Pose& pose, int slot, float height, float width, float forward) {
    const bool chest = slot == 2;
    const auto& bone = pose.frame[chest ? 1 : 0];
    Frame out = bone;
    const float side = chest ? -.055f : (slot == 0 ? .120f + width : -.160f - width);
    const float ahead = (chest ? .125f : .067f) + forward;
    // Rib-height mounts obstruct the arms and thighs in first person. Seat the right
    // pouch at the belt and the left item at the pants pocket; hit radii stay unchanged.
    const float up = chest ? .035f : (slot == 0 ? -.105f : -.145f);
    out.origin = bone.origin + bone.right * side + bone.up * (height + up) - bone.back * ahead;
    // Both hip mounts lie tangent to the front diagonal of the waist.
    const float sine = chest ? 0.f : (slot == 0 ? .5735764f : -.5735764f);
    const float cosine = chest ? 1.f : .8191520f;
    out.right = bone.right * cosine + bone.back * sine;
    out.back = bone.back * cosine - bone.right * sine;
    return out;
}
// Use the character transform captured WITH these bones. Mixing in a later root
// transform makes the attachments lead the skinned body during locomotion/turns.
inline Frame WorldSurface(const Pose& pose, int slot, float height, float width, float forward) {
    auto out = Surface(pose, slot, height, width, forward);
    out.origin = Body::ToWorld(pose.character, out.origin);
    out.right = Body::RotateToWorld(pose.character, out.right);
    out.up = Body::RotateToWorld(pose.character, out.up);
    out.back = Body::RotateToWorld(pose.character, out.back);
    return out;
}
inline Frame FitVialPouch(Frame surface, std::int32_t chest_armor, float scale) {
    surface.origin = surface.origin - surface.back * (PouchOutwardM(chest_armor) * scale);
    return surface;
}
// The model's back face meets the body, with 2 mm of overlap hiding a floating seam.
inline Vec3 Seat(Vec3 surface, Vec3 back, float back_extent, float overlap) {
    return surface - back * std::max(back_extent - overlap, 0.f);
}
} // namespace VR::BodyMounts
