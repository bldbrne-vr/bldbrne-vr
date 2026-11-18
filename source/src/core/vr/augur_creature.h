// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/vr/augur.h"
namespace VR::AugurCreature {
inline constexpr int Model = 12000;
inline constexpr unsigned TextureSize = 1024, TextureLevels = 9, Bones = 30;
inline constexpr unsigned MaxVertices = 8192, MaxIndices = 24000;
inline constexpr unsigned MaxDecodedBytes = 4 * 1024 * 1024;
// The squeeze (the trigger held), paced as the user asked on 2026-11-04: the hand first closes a
// little round it (the grip, GripSeconds), then crushes it very fast (the squish, to
// WindupSeconds); the tentacles follow DeferredTool::DelayNs (150 ms) after the squish, so press
// to tentacles stays .62 s (.5 s squeeze + 120 ms before).
inline constexpr float GripSeconds = .35f;
inline constexpr float WindupSeconds = .47f;
// How far the grip alone goes: the creature only gives a little, the fingers close part way.
inline constexpr float GripCrush = .12f, GripSqueeze = .35f;
inline constexpr std::array<float,3> RestMin{-.030460963f,-.036732648f,-.062407166f};
inline constexpr std::array<float,3> RestMax{ .030280149f, .036731489f, .062292688f};
inline float Smooth(float x) { x=std::clamp(x,0.0f,1.0f);return x*x*(3-2*x); }
inline float IdleFrame(std::int64_t now) { return 1+119*float(static_cast<std::uint64_t>(std::max(now,std::int64_t{0}))%12'000'000'000ULL)/12e9f; }
struct Visual { float frame{}, squeeze{}, crush{}, blend_frame{}, blend_weight{}; bool visible{true}; };
// The creature's crush and the fingers' squeeze `s` seconds into the squeeze: the grip eases in
// to GripCrush / GripSqueeze, then the squish collapses the creature accelerating into the end
// (ease-in: the pop of a squished thing) while the fingers snap shut (ease-out).
struct Squeeze { float crush{}, squeeze{}; };
inline Squeeze SqueezeAt(float s) {
    if (!(s > 0.0f))
        return {};
    if (s < GripSeconds) {
        const float g = Smooth(s / GripSeconds);
        return {GripCrush * g, GripSqueeze * g};
    }
    const float f = std::clamp((s - GripSeconds) / (WindupSeconds - GripSeconds), 0.0f, 1.0f);
    return {GripCrush + (1 - GripCrush) * f * f,
            GripSqueeze + (1 - GripSqueeze) * (1 - (1 - f) * (1 - f))};
}
inline Visual Evaluate(Augur::Use use, std::int64_t session, std::int64_t charge,
                       std::int64_t now) {
    Visual v{};
    if(session<=0 || now<session)return v;
    v.frame=IdleFrame(now);
    // Only an accepted cast hides the creature or releases blood. Refused casts stay alive.
    if(use.held_since==session && use.ns>=session && now>=use.ns &&
       static_cast<std::uint64_t>(now)-static_cast<std::uint64_t>(use.ns)<Augur::EndNs) {
        if(use.eaten) {
            v.crush=Augur::ConsumeProgress(use,session,now);
            v.frame=121+133*v.crush;v.blend_frame=IdleFrame(use.ns);
            v.blend_weight=1-Smooth(float(now-use.ns)*1e-9f/.10f);v.visible=!Augur::Concealed(v.crush);
        } else {
            const float t=float(now-use.ns)*1e-9f;
            v.frame=254;v.crush=1;v.squeeze=1-Smooth((t-.35f)/.40f);v.visible=false;
        }
        return v;
    }
    if(charge>=session && charge>0 && now>=charge) {
        const float s=float(static_cast<std::uint64_t>(now)-static_cast<std::uint64_t>(charge))*1e-9f;
        const Squeeze q=SqueezeAt(s);
        v.frame=121+133*q.crush;v.crush=q.crush;v.squeeze=q.squeeze;
        v.blend_frame=IdleFrame(charge);v.blend_weight=1-Smooth(s/.15f);
    }
    return v;
}
// Source rig's object scale and displacement, separate from the skeletal palette so
// the existing model-view inverse transpose handles nonuniform squash normals correctly.
inline std::array<float,3> Stretch(float crush) {
    return {1-.30f/.44f*crush,1+.015f/.44f*crush,1-.13f/.44f*crush};
}
inline std::array<float,3> Offset(float crush) {
    return {-.00525f*crush,0,-.002925f*crush};
}
} // namespace VR::AugurCreature
