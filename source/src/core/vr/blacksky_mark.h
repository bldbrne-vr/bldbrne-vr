// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// The Blacksky Eye's mark (2026-11-25: "users don't know how to target with it ... would
// we have the eye cast a faint dot ... so that we know where it would be targetting"; "SUPER
// LIGHTWEIGHT", no setting): a faint cold mote where its meteor would land, along the line its shot
// takes (bloodborne_gun.cpp ToolAim): pointed, the left controller's aim ray tilted down
// blacksky_aim_down_deg, from its pupil; held to your eye, where you look, from your eye. The first
// wall, floor or enemy within the meteor's reach (bullet 460: 16 m), else dimmer where it ends.
// Only while the eye is held: one map sphere cast and one enemy gather a game frame
// (bloodborne_quickitems.cpp AddBlackskyMark), one small quad (vr_inframe.frag, uv.x -2120).
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/vr/visceral_clearance.h"

namespace VR::BlackskyMark {
inline constexpr float RangeM = 16.0f;        // the meteor's reach (BulletParam 460)
inline constexpr float MapRadiusM = .05f;     // the cast's sphere against the map
inline constexpr float EnemyRadiusM = .08f;   // ... and against the enemies' hurt capsules
inline constexpr float PullBackM = .03f;      // the mote sits this far before what it hits
inline constexpr float DepthPullM = .30f;     // drawn over the surface it rests on
// The quad as seen (its glow ~half a degree), at any distance, and its light at full: the user,
// 2026-11-26, "make the blacksky eye dot/target a little smaller and a tiny bit fainter" (5 deg and
// 1.0 until then; then "make it 0.85 ... and 16% smaller": 16% smaller, 15% fainter). Brightness
// goes to the shader on its own (the mark's uv.z, vr_inframe.frag) and scales the light after its
// core's cap: the core is over 1 at full strength, so a lower Strength alone would dim only the
// glow round it.
inline constexpr float AngularDeg = 4.2f;
inline constexpr float Brightness = .85f;
inline constexpr float NoHitStrength = .5f;   // nothing in reach: dimmer, where it ends
inline constexpr std::int64_t RecastNs = 6'000'000; // a cast serves both eyes of a frame
inline constexpr std::int64_t PairReuseNs = 40'000'000; // ... and a stereo pair's second eye
// A held aim keeps its map cast (the map stands still) while its line moves less than this, at
// most MapKeepNs: 1 mm and 0.05 degrees (under a centimetre at its reach).
inline constexpr float MapKeepM = .001f;
inline constexpr float MapKeepCos = .99999962f; // cos(0.05 degrees)
inline constexpr std::int64_t MapKeepNs = 100'000'000;

// The first touch along from -> to (0 .. 1) of a sphere of the path's against a capsule of
// radius `r` (the two radii added) from a to b; above 1: none.
inline float FirstTouch(Body::Vec3 from, Body::Vec3 to, Body::Vec3 a, Body::Vec3 b, float r,
                        float limit = 1.0f) {
    const auto path = to - from;
    const float length_sq = Body::LengthSq(path);
    if (!(length_sq > 1e-12f) || !(r > 0.0f)) return 2.0f;

    // Reject capsules outside the segment ending at the nearest hit so far.
    // For a sphere enclosing the capsule, in doubled coordinates,
    // (length(b-a) + 2*r)^2 <= 2*length(b-a)^2 + 8*r^2. This rejection needs no square root.
    const auto centre2 = (a - from) + (b - from);
    const float bound2 = 2.0f * Body::LengthSq(b - a) + 8.0f * r * r;
    // Check the infinite line first, independent of hits on earlier limbs. Then reject
    // spheres wholly before the origin or beyond the closest hit along the line.
    const auto offset = Body::Cross(centre2, path);
    const float scaled_bound2 = bound2 * length_sq;
    if (Body::LengthSq(offset) > scaled_bound2) return 2.0f;
    const float along = Body::Dot(centre2, path);
    const float excess = std::max(-along, along - 2.0f * limit * length_sq);
    if (excess > 0.0f && excess * excess > scaled_bound2) return 2.0f;

    const float inv_length = 1.0f / std::sqrt(length_sq);
    const auto hit = GrabCamera::RayCapsule(from, path * inv_length, {a, b, r});
    if (!hit) return 2.0f;
    const float fraction = *hit * inv_length;
    return fraction <= limit ? std::max(0.0f, fraction) : 2.0f;
}

// Its light (0 .. 1): full on what it hits, dimmer with nothing in reach, a slow uneven flicker.
inline float Strength(std::int64_t now_ns, bool hit) {
    const float s = float(now_ns % 60'000'000'000LL) * 1e-9f;
    const float flicker = .88f + .07f * std::sin(s * 6.2831853f * 1.7f) +
                          .05f * std::sin(s * 6.2831853f * 5.3f + 1.1f);
    return (hit ? 1.0f : NoHitStrength) * flicker;
}

// By its distance along the line from the pupil (metres; the tester, 2026-11-25: "fade in between
// 40 and 65cm -- and also fade it out when it's far too"): none within NearHideM (up close it
// would only clutter the hand), full from NearFullM, fading again from FarFadeM to nothing at the
// meteor's reach (so it ends softly). Its cast gives the distance, so this is free, and a hidden
// mark draws nothing.
inline constexpr float NearHideM = .40f, NearFullM = .65f, FarFadeM = 11.0f;
inline float DistanceFade(float metres) {
    const auto smooth = [](float x) {
        x = std::clamp(x, 0.0f, 1.0f);
        return x * x * (3.0f - 2.0f * x);
    };
    return smooth((metres - NearHideM) / (NearFullM - NearHideM)) *
           (1.0f - smooth((metres - FarFadeM) / (RangeM - FarFadeM)));
}

// The quad's side (the units of `distance`) that keeps it AngularDeg wide as seen from there.
inline float QuadSize(float distance) {
    return 2.0f * distance * std::tan(AngularDeg * .5f * 3.14159265f / 180.0f);
}
} // namespace VR::BlackskyMark
