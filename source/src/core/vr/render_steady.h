// SPDX-License-Identifier: GPL-2.0-or-later

// Render-camera steadiness (vr.ini render_steady and render_steady_*).
//
// Standing "still" in VR, your head never is: it sways, corrects and trembles by a few tenths of a
// degree and a few millimetres (about 1.3 deg/s). Every game frame used to be rendered from that
// micro-moved head, so its aliasing (thin twigs, leaves, railings, SMAA's edge blends, candle and
// metal glints) came out a little different every frame: the "constant jitter, things blinking or
// shining on / off" while standing still (flicker study 2026-10-07: with a perfectly still head
// nothing blinks; with a realistic micro-motion ~10 % of the Dream's pixels blink even after the
// eye TAA, and the game frame is the source, not the late warp).
//
// With render_steady the game's RENDER camera holds its pose while your head stays within a small
// band of it (render_steady_deg, render_steady_mm), so the game renders the same frame again and
// again; beyond the band it is pulled along at the band's edge, and while the head really moves
// (faster than render_steady_follow_deg_s or _mm_s) it follows the head exactly, as before. Nothing
// you see lags or moves: every frame's camera record carries the render camera's own pose, and
// everything that re-sees a frame from your head (the late warp, the eye TAA's history, the UI's
// rest quad, the compositor) starts from that record and ends at your true, latest head pose -
// the same path render_pitch_bias_deg and eye_taa_jitter use. Only the viewpoint the game renders
// from stays put through the tremor. The body, the arms and the hand items are placed through the
// same record (tracking <-> world map), so they stay where your controllers are; the neck (physical
// evasion) reads the head's own sample.
//
// Native stereo reuses only the angular hold with UpdateRotation. Position is passed
// through exactly: no positional hold without depth-aware late reprojection. The
// submitted eye orientation must describe this held render camera, not the live head.
//
// Header-only and dependency-free (unit tests: tests/test_render_steady.cpp).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace VR::Steady {

struct Q {
    float x, y, z, w;
};
struct V {
    float x, y, z;
};

constexpr float RadToDeg = 57.2957795f;

struct Config {
    float band_deg{0.8f};      // the render camera holds while the head stays this close (angle)
    float band_mm{5.0f};       // ... and this close (position)
    float follow_deg_s{8.0f};  // a head turning faster than this: followed exactly
    float follow_mm_s{50.0f};  // a head moving faster than this: followed exactly
    float speed_hold_s{0.15f}; // the speed's peak is held (decays) this long
    float gap_s{0.25f};        // samples further apart than this start over (a pause, a load)
};

enum class Step : uint8_t { Start, Follow, Hold, Drag };

struct State {
    bool valid{false};
    Q hold_q{0.0f, 0.0f, 0.0f, 1.0f};
    V hold_p{0.0f, 0.0f, 0.0f};
    Q last_q{0.0f, 0.0f, 0.0f, 1.0f};
    V last_p{0.0f, 0.0f, 0.0f};
    int64_t last_ns{0};
    float speed_deg_s{0.0f};
    float speed_mm_s{0.0f};
};

inline float Dot(const Q& a, const Q& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

// The angle between two orientations (degrees), well conditioned for small angles.
inline float AngleDeg(const Q& a, const Q& b) {
    // b = a * d: d = conj(a) * b; angle = 2 atan2(|d.xyz|, |d.w|).
    const float dw = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    const float dx = a.w * b.x - a.x * b.w - a.y * b.z + a.z * b.y;
    const float dy = a.w * b.y + a.x * b.z - a.y * b.w - a.z * b.x;
    const float dz = a.w * b.z - a.x * b.y + a.y * b.x - a.z * b.w;
    const float v = std::sqrt(dx * dx + dy * dy + dz * dz);
    return 2.0f * std::atan2(v, std::abs(dw)) * RadToDeg;
}

// From `a` towards `b` by `t` (0 .. 1) of the angle between them (slerp).
inline Q Toward(const Q& a, Q b, float t) {
    float c = Dot(a, b);
    if (c < 0.0f) {
        b = {-b.x, -b.y, -b.z, -b.w};
        c = -c;
    }
    float ka = 1.0f - t, kb = t;
    if (c < 0.9999f) {
        const float th = std::acos(std::min(c, 1.0f));
        const float s = std::sin(th);
        ka = std::sin((1.0f - t) * th) / s;
        kb = std::sin(t * th) / s;
    }
    Q r{a.x * ka + b.x * kb, a.y * ka + b.y * kb, a.z * ka + b.z * kb, a.w * ka + b.w * kb};
    const float n = std::sqrt(Dot(r, r));
    return n > 0.0f ? Q{r.x / n, r.y / n, r.z / n, r.w / n} : a;
}

inline float DistMm(const V& a, const V& b) {
    const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0f;
}

// One camera update with the head's true pose (q, p: tracking space, metres) sampled for `t_ns`:
// the pose to render from (out_q, out_p) and what happened.
inline Step Update(State& s, const Q& q, const V& p, int64_t t_ns, const Config& c, Q& out_q,
                   V& out_p) {
    const float dt = static_cast<float>(t_ns - s.last_ns) * 1e-9f;
    if (!s.valid || dt > c.gap_s || dt < -c.gap_s) {
        s = State{};
        s.valid = true;
        s.hold_q = s.last_q = q;
        s.hold_p = s.last_p = p;
        s.last_ns = t_ns;
        out_q = q;
        out_p = p;
        return Step::Start;
    }
    if (dt > 0.0f) {
        // The head's speed since the last sample, its peak held for speed_hold_s (a real turn that
        // slows down for a moment keeps being followed).
        const float decay = std::exp(-dt / std::max(c.speed_hold_s, 1e-3f));
        s.speed_deg_s = std::max(AngleDeg(s.last_q, q) / dt, s.speed_deg_s * decay);
        s.speed_mm_s = std::max(DistMm(s.last_p, p) / dt, s.speed_mm_s * decay);
        s.last_q = q;
        s.last_p = p;
        s.last_ns = t_ns;
    }
    Step step = Step::Hold;
    if (s.speed_deg_s > c.follow_deg_s || s.speed_mm_s > c.follow_mm_s) {
        s.hold_q = q;
        s.hold_p = p;
        step = Step::Follow;
    } else {
        const float a = AngleDeg(s.hold_q, q);
        if (a > c.band_deg) {
            s.hold_q = Toward(s.hold_q, q, (a - std::max(c.band_deg, 0.0f)) / a);
            step = Step::Drag;
        }
        const float d = c.band_mm > 0.0f ? DistMm(s.hold_p, p) : 0.0f;
        if (c.band_mm <= 0.0f) {
            s.hold_p = p; // exact passthrough; position alone does not count as a held-axis drag
        } else if (d > c.band_mm) {
            const float k = (d - std::max(c.band_mm, 0.0f)) / d;
            s.hold_p = {s.hold_p.x + (p.x - s.hold_p.x) * k, s.hold_p.y + (p.y - s.hold_p.y) * k,
                        s.hold_p.z + (p.z - s.hold_p.z) * k};
            step = Step::Drag;
        }
    }
    out_q = s.hold_q;
    out_p = s.hold_p;
    return step;
}

// Native eyes already carry their render orientation into the compositor. A small
// angular hold needs no new image pass. Keep translation bit-exact (including tiny
// movement), but still let deliberate translation release the angular hold.
inline Step UpdateRotation(State& s, const Q& q, const V& p, int64_t t_ns, Config c,
                           Q& out_q, V& out_p) {
    if (s.valid && t_ns < s.last_ns)
        s.valid = false;
    c.band_mm = 0.0f;
    const Step step = Update(s, q, p, t_ns, c, out_q, out_p);
    s.hold_p = out_p = p;
    return step;
}

} // namespace VR::Steady
