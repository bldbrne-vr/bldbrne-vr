// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <span>
#include <string_view>
#include "core/vr/solid_weapon.h"
#include "core/vr/weapon_mesh.h"

// Contact only, not attack geometry. Five slim capsules follow the knuckles and
// distal bones of wp_a_1001_2. No mesh access, allocation or physics simulation.
namespace VR::BeastClawHand {
namespace B = Body;
namespace W = WeaponMesh;
constexpr int MaxBones = 32;
inline constexpr std::array<std::string_view, 11> Names{
    "B_Master", "B_middlefinger01", "B_middlefinger03",
    "B_indexfinger01", "B_indexfinger03", "B_ringfinger01", "B_ringfinger02",
    "B_littlefinger01", "B_littlefinger02", "B_thumb01", "B_thumb02"};
using Indices = std::array<int, Names.size()>;

// Root-attached dummy 110: forward -Y, up -Z, position (0,-.16699788,0).
// Its inverse gives the rendered B_Master frame without assuming that the
// native animation skeleton and FLVER have identical root transforms.
inline constexpr W::Mat34 MarkerToMaster{
    -1,0,0,0, 0,0,-1,0, 0,-1,0,-.16699788f};
inline constexpr B::Vec3 CheckMarker{-.0524177775f, -.385255754f, 0}; // dummy 100

inline bool RootFrame(const W::Mat34& grip, const W::Mat34& marker,
                       B::Vec3 check_world, float scale, W::Mat34& out) {
    if (!B::IsFinite(check_world) || !std::isfinite(scale) || scale < .1f || scale > 10.f)
        return false;
    const auto valid = [](const W::Mat34& m) {
        for (float v : m) if (!std::isfinite(v)) return false;
        const B::Vec3 x{m[0],m[4],m[8]}, y{m[1],m[5],m[9]}, z{m[2],m[6],m[10]};
        return std::abs(B::LengthSq(x)-1) < .02f && std::abs(B::LengthSq(y)-1) < .02f &&
               std::abs(B::LengthSq(z)-1) < .02f && std::abs(B::Dot(x,y)) < .01f &&
               B::Dot(B::Cross(x,y),z) > .98f;
    };
    if (!valid(grip) || !valid(marker)) return false;
    const auto back = W::Inverse(grip);
    const auto frame = W::Mul(W::Mul(back,marker),MarkerToMaster);
    if (B::LengthSq(W::Apply(frame,CheckMarker)-W::Apply(back,check_world)) >
        .000225f*scale*scale) return false;
    out = frame;
    return true;
}

inline bool ValidTransform(const B::QsTransform& t) {
    const float q2 = B::Dot(t.q, t.q);
    return B::IsFinite(t) && q2 > .99f && q2 < 1.01f &&
           t.s.x > .99f && t.s.x < 1.01f && t.s.y > .99f && t.s.y < 1.01f &&
           t.s.z > .99f && t.s.z < 1.01f && B::LengthSq(t.t) < 16.f;
}

inline bool ModelPose(std::span<const B::HkQsTransform> local,
                      std::span<const short> parents, std::span<B::QsTransform> model) {
    if (local.empty() || local.size() > MaxBones || parents.size() != local.size() ||
        model.size() != local.size()) return false;
    for (size_t i = 0; i < local.size(); ++i) {
        const int p = parents[i];
        if (p < -1 || p >= static_cast<int>(i)) return false;
        const auto t = B::FromHavok(local[i]);
        if (!ValidTransform(t)) return false;
        model[i] = p < 0 ? t : B::Compose(model[p], t);
        if (!ValidTransform(model[i])) return false;
    }
    return true;
}

inline bool Build(std::span<const B::QsTransform> model, const Indices& indices,
                  const W::Mat34& master_to_grip, float scale, Solid::Shape& out) {
    if (model.empty() || model.size() > MaxBones || !std::isfinite(scale) ||
        scale < .1f || scale > 10.f) return false;
    for (int i : indices)
        if (i < 0 || static_cast<size_t>(i) >= model.size() || !ValidTransform(model[i]))
            return false;
    for (float v : master_to_grip) if (!std::isfinite(v)) return false;
    // Middle, index, ring, little, thumb. End offsets are measured along the
    // last bones' +X axes, inset by the radius from the model's claw tips.
    constexpr std::array<float, 5> tips{.145f,.100f,.055f,.045f,.080f};
    constexpr std::array<float, 5> radii{.024f,.023f,.022f,.018f,.025f};
    // Extend the existing starts into the palm along each proximal bone's -X.
    // This follows the hand's pose, without more capsules or longer claw tips.
    constexpr std::array<float, 5> palm_overlap{.03f,.03f,.03f,.02f,.02f};
    Solid::Shape shape{};
    shape.hand_r = .05f * scale;
    shape.line = true;
    shape.extra = 4;
    const auto& master = model[indices[0]];
    const auto into_master = B::Conjugate(master.q);
    const auto point = [&](int index, B::Vec3 offset) {
        const auto& bone = model[index];
        return W::Apply(master_to_grip,
                        B::Rotate(into_master,bone.t-master.t+B::Rotate(bone.q,offset)));
    };
    for (int finger = 0; finger < 5; ++finger) {
        const B::Vec3 a = point(indices[1 + finger*2], {-palm_overlap[finger],0,0});
        const B::Vec3 b = point(indices[2 + finger*2], {tips[finger],0,0});
        if (!B::IsFinite(a) || !B::IsFinite(b) || B::LengthSq(a) > scale*scale ||
            B::LengthSq(b) > scale*scale || B::LengthSq(b-a) > .36f*scale*scale)
            return false;
        if (finger == 0) {
            shape.base = a; shape.tip = b; shape.line_r = radii[0]*scale;
        } else {
            // Each finger starts independently; never assume another finger's
            // sweep cleared its knuckle. Free tips get continuous checks too.
            shape.more[finger-1] = {a,b,radii[finger]*scale,true,true,true};
        }
    }
    out = shape; // fail without publishing a partial hand
    return true;
}
} // namespace VR::BeastClawHand
