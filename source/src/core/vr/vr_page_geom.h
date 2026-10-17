// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cmath>

// Where the right controller's ray meets the VR settings page's layer (header-only, pure; unit
// tested in tests/test_vr_page.cpp): an OpenXR cylinder layer (its axis along the pose's +Y, the
// image centred on the pose's -Z, `central_angle` wide at `radius`, as tall as radius *
// central_angle / aspect) or a quad (its plane through the pose, facing +Z, quad_w x quad_h).
// The result is the image coordinate u (0 left .. 1 right), v (0 top .. 1 bottom).
namespace VR::Page {

struct Surface {
    float pos[3]{0.0f, 0.0f, 0.0f};
    float rot[4]{0.0f, 0.0f, 0.0f, 1.0f}; // x y z w
    bool curved{true};
    float radius{1.8f};
    float central_angle{1.0f}; // radians
    float aspect{1.0f};        // image width / height
    float quad_w{1.0f};
    float quad_h{1.0f};
};

namespace detail {
// v rotated by the inverse of the unit quaternion q.
inline void RotateInverse(const float q[4], const float v[3], float out[3]) {
    const float x = -q[0], y = -q[1], z = -q[2], w = q[3];
    // t = 2 * cross(q.xyz, v); v' = v + w * t + cross(q.xyz, t)
    const float tx = 2.0f * (y * v[2] - z * v[1]);
    const float ty = 2.0f * (z * v[0] - x * v[2]);
    const float tz = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}
} // namespace detail

inline bool RayHit(const Surface& s, const float origin[3], const float dir[3], float& u,
                   float& v) {
    const float rel[3]{origin[0] - s.pos[0], origin[1] - s.pos[1], origin[2] - s.pos[2]};
    float o[3], d[3];
    detail::RotateInverse(s.rot, rel, o);
    detail::RotateInverse(s.rot, dir, d);
    if (s.curved) {
        const float a = d[0] * d[0] + d[2] * d[2];
        if (a < 1e-9f) {
            return false;
        }
        const float b = 2.0f * (o[0] * d[0] + o[2] * d[2]);
        const float c = o[0] * o[0] + o[2] * o[2] - s.radius * s.radius;
        const float disc = b * b - 4.0f * a * c;
        if (disc < 0.0f) {
            return false;
        }
        const float t = (-b + std::sqrt(disc)) / (2.0f * a); // the far root: from inside, ahead
        if (t <= 0.0f) {
            return false;
        }
        const float x = o[0] + t * d[0], y = o[1] + t * d[1], z = o[2] + t * d[2];
        const float theta = std::atan2(x, -z);
        const float height = s.radius * s.central_angle / std::max(s.aspect, 1e-3f);
        u = 0.5f + theta / s.central_angle;
        v = 0.5f - y / height;
    } else {
        if (std::abs(d[2]) < 1e-9f) {
            return false;
        }
        const float t = -o[2] / d[2];
        if (t <= 0.0f) {
            return false;
        }
        const float x = o[0] + t * d[0], y = o[1] + t * d[1];
        u = 0.5f + x / s.quad_w;
        v = 0.5f - y / s.quad_h;
    }
    return u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f;
}

// Where the page hangs (radians; yaw + = left, pitch + = up). It hangs on the menu canvas's place,
// with two limits so that it always opens fully in view (the play test of 2026-10-09: after Auto
// height measures the canvas sat 12-15 deg up and the 42.8 deg tall page's top was ~36 deg up):
// - its centre never above PageMaxPitchDeg or below PageMinPitchDeg: at +5 its top is ~26 deg up,
//   at -15 its bottom ~36 deg down, both inside the Quest's view while you look at its middle;
// - when it opens, its centre within PageOpenYawDeg of where you look (the canvas lags your gaze
//   by up to the menus' follow angle): the 67 deg wide page starts in front of you. The offset is
//   kept while it is up (it stays where it opened);
// - when it opens looking far up or down, its centre within the view (PageViewPitchRange).
inline constexpr float PageMinPitchDeg = -15.0f;
inline constexpr float PageMaxPitchDeg = 5.0f;
inline constexpr float PageOpenYawDeg = 12.0f;

namespace detail {
inline float WrapPi(float a) {
    constexpr float Pi = 3.14159265358979f;
    a = std::fmod(a + Pi, 2.0f * Pi);
    return (a < 0.0f ? a + 2.0f * Pi : a) - Pi;
}
} // namespace detail

inline float ClampPagePitch(float pitch) {
    constexpr float DegToRad = 3.14159265358979f / 180.0f;
    return std::clamp(pitch, PageMinPitchDeg * DegToRad, PageMaxPitchDeg * DegToRad);
}

// The yaw added to the anchor's when the page opens: 0 when the anchor is within PageOpenYawDeg of
// the head's yaw, else what brings it to that limit (the shorter way round).
inline float PageOpenYawOffset(float anchor_yaw, float head_yaw) {
    constexpr float Limit = PageOpenYawDeg * 3.14159265358979f / 180.0f;
    const float d = detail::WrapPi(anchor_yaw - head_yaw);
    return std::clamp(d, -Limit, Limit) - d;
}

// The pitches (radians) of the page's centre that keep all of it in the view when it opens: you
// look at `head_pitch`, the view reaches `up` above and `down` below the gaze (both > 0; the
// narrower eye's: on the Quest 3 over Link 44 up, 55 down), the page is `half` tall either side
// of its centre. Within -15..+5 (ClampPagePitch) this changes nothing unless you look more than
// ~33 deg up or ~38 down when it opens (a page at +5 seen from 45 deg up lost its bottom 7 deg); a
// view shorter than the page centres it in the view.
struct PitchRange {
    float lo{-3.14159265f};
    float hi{3.14159265f};
};
inline PitchRange PageViewPitchRange(float head_pitch, float up, float down, float half) {
    PitchRange r{head_pitch - down + half, head_pitch + up - half};
    if (r.lo > r.hi) {
        r.lo = r.hi = head_pitch + 0.5f * (up - down);
    }
    return r;
}

// The page's centre pitch: the canvas's (`anchor_pitch`) within -15..+5, then within the view it
// opened in (PageViewPitchRange).
inline float PagePitch(float anchor_pitch, const PitchRange& view) {
    return std::clamp(ClampPagePitch(anchor_pitch), view.lo, view.hi);
}

} // namespace VR::Page
