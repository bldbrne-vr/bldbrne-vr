// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include <span>
#include "core/vr/body_frame.h"
#include "core/vr/visceral_clearance.h"

namespace VR::VisceralStage {
using Body::Vec3;

// Remove the sharp ~0.7 s excursion while smoothly joining the initial push
// (0.4828 units) to the later resting level (0.69 units). Same stateless,
// stack-output-only policy and conservative scope as the late correction below.
inline bool AttenuateMiddleKick(float* effect, float elapsed, float duration) {
    if (!effect || !std::isfinite(elapsed) || duration != 27.0f ||
        elapsed <= 0.55f || elapsed >= 1.75f) return false;
    for (int i=0; i<16; ++i) if (!std::isfinite(effect[i])) return false;
    if (std::abs(effect[12]) > 0.002f || std::abs(effect[13]) > 0.025f ||
        effect[14] < 0.40f || effect[14] > 1.50f) return false;
    const auto smooth = [](float x) {
        x = std::clamp(x, 0.0f, 1.0f);
        return x*x*(3.0f-2.0f*x);
    };
    const float baseline = 0.4828f + (0.69f-0.4828f)*smooth((elapsed-0.65f)/1.05f);
    const float excess = std::max(0.0f, effect[14]-baseline);
    const float weight = smooth((elapsed-0.55f)/0.10f)*smooth((1.75f-elapsed)/0.15f);
    const float rounded = excess >= 0.00005f ? excess-0.000025f : excess*excess/0.0001f;
    effect[14] -= 0.999f*weight*rounded;
    return excess > 0;
}

// The captured visceral curve has a second forward excursion at native effect
// time ~1.9--2.4 s, above its 0.69-unit resting level. Keep only 0.1% of that excess.
// This edits only the caller's freshly sampled stack transform: no cached pose,
// shared animation data, wall clock, or cross-frame mutable state.
// Scope is deliberately conservative until a stable asset ID is available:
// active visceral (caller), captured duration, time window and translation range.
inline bool AttenuateLateKick(float* effect, float elapsed, float duration) {
    if (!effect || !std::isfinite(elapsed) || duration != 27.0f ||
        elapsed <= 1.75f || elapsed >= 2.65f) return false;
    for (int i=0; i<16; ++i) if (!std::isfinite(effect[i])) return false;
    if (std::abs(effect[12]) > 0.002f || std::abs(effect[13]) > 0.002f ||
        effect[14] < 0.60f || effect[14] > 1.0f) return false;
    constexpr float Rest = 0.69f;
    const float excess = std::max(0.0f, effect[14]-Rest);
    const auto smooth = [](float x) {
        x = std::clamp(x, 0.0f, 1.0f);
        return x*x*(3.0f-2.0f*x);
    };
    const float weight = smooth((elapsed-1.75f)/0.15f) * smooth((2.65f-elapsed)/0.15f);
    // Smooth the first 0.00005 units above rest to avoid a slope discontinuity.
    const float rounded = excess >= 0.00005f ? excess-0.000025f : excess*excess/0.0001f;
    effect[14] -= 0.999f * weight * rounded;
    return excess > 0;
}

// Native effect matrices are rebuilt each update. Reduce only camera-local
// forward translation; preserve rotation, lateral/vertical motion and all W lanes.
inline bool ReduceForwardLunge(float* effect) {
    if (!effect) return false;
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(effect[i])) return false;
    if (effect[14] <= 0) return false;
    effect[14] *= 0.20f;
    return true;
}

// Half-angle rotation, not a linear blend of matrix entries (which introduces
// scale). Accept only small rigid affine effects; unknown transforms stay intact.
inline bool SoftenShake(float* m) {
    if (!m) return false;
    for (int i=0; i<16; ++i) if (!std::isfinite(m[i])) return false;
    if (std::abs(m[3]) > 1e-5f || std::abs(m[7]) > 1e-5f ||
        std::abs(m[11]) > 1e-5f || std::abs(m[15]-1) > 1e-5f) return false;
    for (int i=0; i<3; ++i) for (int j=i; j<3; ++j) {
        float dot=0;
        for (int k=0; k<3; ++k) dot += m[4*i+k]*m[4*j+k];
        if (std::abs(dot-(i==j ? 1.0f : 0.0f)) > 0.001f) return false;
    }
    const float det = m[0]*(m[5]*m[10]-m[6]*m[9]) -
                      m[1]*(m[4]*m[10]-m[6]*m[8]) +
                      m[2]*(m[4]*m[9]-m[5]*m[8]);
    const float trace=m[0]+m[5]+m[10];
    if (std::abs(det-1) > 0.002f || trace < 1) return false;
    float w=0.5f*std::sqrt(1+trace);
    float x=(m[9]-m[6])/(4*w), y=(m[2]-m[8])/(4*w), z=(m[4]-m[1])/(4*w);
    const float norm=std::sqrt(w*w+x*x+y*y+z*z);
    w/=norm; x/=norm; y/=norm; z/=norm;
    w=std::sqrt((1+w)*0.5f);
    x/=2*w; y/=2*w; z/=2*w;
    m[0]=1-2*(y*y+z*z); m[1]=2*(x*y-z*w); m[2]=2*(x*z+y*w);
    m[4]=2*(x*y+z*w); m[5]=1-2*(x*x+z*z); m[6]=2*(y*z-x*w);
    m[8]=2*(x*z-y*w); m[9]=2*(y*z+x*w); m[10]=1-2*(x*x+y*y);
    m[12]*=0.5f; m[13]*=0.5f;
    if (m[14]<0) m[14]*=0.5f; // positive Z belongs to the lunge control
    return true;
}

// One entry snapshot owns both the view and native pair placement. No pose-update
// availability, collision correction or comfort spring can change this anchor.
struct Entry {
    Vec3 base{}, root{}, physics_root{}, eye{}, forward{};
    float scale{1};
};

inline bool Valid(const Entry& e) {
    return Body::IsFinite(e.base) && Body::IsFinite(e.root) &&
           Body::IsFinite(e.physics_root) && Body::IsFinite(e.eye) &&
           Body::IsFinite(e.forward) && std::isfinite(e.scale) && e.scale > 0 &&
           std::abs(e.forward.y) < 1e-5f &&
           std::abs(Body::LengthSq(e.forward) - 1) < 0.001f;
}

// The native destination can contain a sentinel meaning "no translation".
// Preserve those values and the native vertical placement on slopes/stairs.
// The victim's root is kept at least `minimum_root_m` from the eyes, level: pushed out along the
// line from the eyes to where the game puts it, never through the hunter. (The entry's view yaw
// is the camera's, which points the other way for the game's own facing: pushing along it put the
// victim behind the hunter and the game dropped the throw, 2026-10-27 mock trace.) A destination
// at the eyes falls back to the line from the hunter's root, else it is left alone.
inline bool Constrain(const Entry& e, bool hunter, Vec3& destination,
                      float minimum_root_m = 1.25f) {
    if (!Valid(e) || !Body::IsFinite(destination) ||
        Body::LengthSq(destination - e.physics_root) > 36 * e.scale * e.scale ||
        !std::isfinite(minimum_root_m) || minimum_root_m < 0)
        return false;
    if (hunter) {
        destination.x = e.physics_root.x;
        destination.z = e.physics_root.z;
    } else {
        const float need = minimum_root_m * e.scale;
        Vec3 away{destination.x - e.eye.x, 0.0f, destination.z - e.eye.z};
        float gap = Body::Length(away);
        if (gap < 1e-3f) {
            away = {destination.x - e.physics_root.x, 0.0f, destination.z - e.physics_root.z};
            if (Body::Length(away) < 1e-3f) return true;
        }
        away = away * (1.0f / Body::Length(away));
        if (gap < need) {
            destination.x = e.eye.x + away.x * need;
            destination.z = e.eye.z + away.z * need;
        }
    }
    return true;
}

// The size-aware hold: how far (world units, at most `max_push`) the victim moves past where the
// game puts it, straight away from the eyes (`away`, level, unit), so that no part of its body
// (its capsules now, moved by `to_destination` = the game's destination - its root now) comes
// nearer the eyes than `keep`. 0 when none does: an enemy that would not clip stays where the
// game puts it, a big one goes back only as far as its body needs.
inline float BodyPush(Vec3 eye, Vec3 away, std::span<const GrabCamera::CollisionCapsule> body,
                      Vec3 to_destination, float keep, float max_push) {
    if (body.empty() || !Body::IsFinite(eye) || !Body::IsFinite(away) ||
        !Body::IsFinite(to_destination) || !std::isfinite(keep) || !std::isfinite(max_push) ||
        std::abs(Body::LengthSq(away) - 1.0f) > 0.001f || std::abs(away.y) > 1e-5f)
        return 0.0f;
    float push = 0.0f;
    // Moving away by d opens the nearest gap by at most d: a few steps reach it.
    for (int i = 0; i < 8; ++i) {
        const float gap = GrabCamera::NearestGap(eye, body, to_destination + away * push);
        if (!std::isfinite(gap) || gap >= keep - 1e-4f) break;
        push = std::min(max_push, push + (keep - gap));
        if (push >= max_push) break;
    }
    return push;
}

class Lifetime {
public:
    void Begin(std::int64_t now) { begin_ = updated_ = now; active_ = true; seen_ = false; }
    void Reject() { active_ = false; }
    void Update(bool native_or_finishing, bool cancelled, std::int64_t now) {
        if (!active_) return;
        if (now < updated_ || now - updated_ > 500'000'000LL ||
            now - begin_ > 30'000'000'000LL || cancelled) {
            active_ = false;
            return;
        }
        updated_ = now;
        seen_ |= native_or_finishing;
        if (!native_or_finishing && (seen_ || now - begin_ > 750'000'000LL))
            active_ = false;
    }
    bool Active(std::int64_t now) const {
        return active_ && now >= updated_ && now - updated_ <= 500'000'000LL &&
               now >= begin_ && now - begin_ <= 30'000'000'000LL;
    }
private:
    std::int64_t begin_{}, updated_{};
    bool active_{}, seen_{};
};
} // namespace VR::VisceralStage
