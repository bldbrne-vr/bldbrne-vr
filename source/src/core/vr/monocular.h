// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace VR::Monocular {
// Maximum magnification with the trigger's coarse ring held, shared by the session,
// native eye projection and DIBR crop.
inline constexpr float MaxZoom = 6.75f;
// Wrist-only maximum. Past it, the plain twist zooms out
// but never further in; a zoom the trigger left beyond it stays.
inline constexpr float FineMaxZoom = 4.05f;

// Entry/release hysteresis prevents flicker. A clear move toward the other eye can still
// transfer the optic without first taking it away from the headset.
inline int SelectEye(const std::array<float, 2>& distance, int previous) {
    if (!std::isfinite(distance[0]) || !std::isfinite(distance[1])) return -1;
    const int nearest = distance[0] <= distance[1] ? 0 : 1;
    if (previous >= 0 && previous < 2 && distance[previous] < 0.17f) {
        if (nearest != previous && distance[nearest] < 0.12f &&
            distance[nearest] + 0.012f < distance[previous]) return nearest;
        return previous;
    }
    return distance[nearest] < 0.12f ? nearest : -1;
}

// ---- The stabilized optic ------------------------------------------------------------------------
// Moving the head while zoomed in can be nausea inducing (2026-10-30): at 5x every
// degree of head wobble moves the image five. A low-pass filter on the head (the first try, a One
// Euro filter) could not be felt: the head's sway is slow, below any cutoff that still lets the
// view follow a turn, so it passed nearly whole.
//
// Instead, for small moves the zoomed eye's aim takes 1/zoom of every head step: the magnified
// image then moves exactly as far as the real world does for the same head turn. The eyes'
// reflex (the vestibulo-ocular reflex) turns them against the head by just that much, so the
// image holds still on the retina, as the unzoomed world does. The runtime's reprojection, which
// assumes an unzoomed image, also becomes exact. The other eye sees the world move the same way.
//
// To look somewhere else:
// - Past a few degrees the aim is dragged along progressively (outward only, so swaying back is
//   never dragged).
// - A fast deliberate turn follows 1:1 and catches up, so no offset is left when it ends.
//
// A turn about the view axis is not magnified, so only the direction is handled. The renderer
// slides the eye's zoom window to the aim (WindowShift, ZoomWindow): no extra camera, view or
// pass. Fixed-size values only.

using Dir = std::array<float, 3>; // a direction in tracking space (OpenXR: -Z forward, +Y up)

// The user's headset pick (2026-10-30: 1 and 4 degrees were "a bit too strong", softened "only a
// tiny bit" to 1.1 and 3.5; 2026-11-02, still "a bit too strong", softened "slightly" again): the
// image a fifth faster than the world, dragged sooner.
struct StabilizerTune {
    // How fast the zoomed image moves for small head moves, in real-world units: 1 with the world
    // (the aim takes world/zoom of each step), 0 held still, the zoom itself unstabilized.
    float world{1.2f};
    float max_lag_deg{3.0f}; // the aim never further than this from the head's forward
};

// The drag starts at this share of max_lag and is complete at max_lag.
inline constexpr float StabilizerEdge = 0.35f;
// Head speeds (degrees a second) from which a turn counts as a deliberate pan, and at which it
// follows fully; sway stays under the first.
inline constexpr float PanFromDps = 8.0f;
inline constexpr float PanFullDps = 30.0f;
// While panning, the time constant (s) of the aim's catch-up with the head.
inline constexpr float PanCatchS = 0.25f;

namespace Detail {
inline float Dot(const Dir& a, const Dir& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
// False (and `v` unchanged) for a zero, tiny or non-finite vector.
inline bool Normalize(Dir& v) {
    const float n = std::sqrt(Dot(v, v));
    if (!std::isfinite(n) || n < 1e-6f) return false;
    for (float& c : v) c /= n;
    return true;
}
// The angle between two unit vectors, well conditioned at small angles.
inline float Angle(const Dir& a, const Dir& b) {
    const Dir c{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    return std::atan2(std::sqrt(Dot(c, c)), Dot(a, b));
}
// `a + k * (b - a)`, normalized; `fallback` when that is degenerate.
inline Dir Toward(const Dir& a, const Dir& b, float k, const Dir& fallback) {
    Dir v{a[0] + k * (b[0] - a[0]), a[1] + k * (b[1] - a[1]), a[2] + k * (b[2] - a[2])};
    return Normalize(v) ? v : fallback;
}
// The direction `angle` radians from unit `from` on the great circle toward `toward`; `from`
// itself when `toward` is (anti)parallel.
inline Dir OnArc(const Dir& from, const Dir& toward, float angle) {
    const float d = Dot(toward, from);
    Dir side{toward[0] - from[0] * d, toward[1] - from[1] * d, toward[2] - from[2] * d};
    if (!Normalize(side)) return from;
    const float c = std::cos(angle), s = std::sin(angle);
    return {from[0] * c + side[0] * s, from[1] * c + side[1] * s, from[2] * c + side[2] * s};
}
// A first-order low-pass's smoothing factor for a step of `dt` s at `cutoff_hz` (the speed).
inline float Alpha(float dt, float cutoff_hz) {
    const float tau = 1.0f / (6.2831853f * cutoff_hz);
    return 1.0f / (1.0f + tau / dt);
}
// Clamped to [0, 1] first; 0 for NaN, so a bad value never drags or pans.
inline float Smoothstep(float x) {
    const float t = x > 0.0f ? (x < 1.0f ? x : 1.0f) : 0.0f;
    return t * t * (3.0f - 2.0f * t);
}
// std::clamp without its precondition: a non-finite value or an empty range gives `fallback`.
inline float ClampOr(float v, float lo, float hi, float fallback) {
    if (!std::isfinite(v) || !(lo <= hi)) return fallback;
    return std::min(std::max(v, lo), hi);
}
} // namespace Detail

// Once per head sample on the presenter's frame thread: about a dozen multiplies, two square
// roots and, in the common case (inside the edge, not panning), one arctangent.
class Stabilizer {
public:
    void Reset() {
        valid_ = false;
    }

    // `forward`: the head's forward this sample (tracking space), `dt`: seconds since the last
    // sample. False (aim untouched) for an invalid forward; a first sample, or one after a gap
    // or a reset, starts the aim at the head.
    bool Update(const Dir& forward_in, float zoom, float dt, const StabilizerTune& tune,
                Dir& aim) {
        Dir forward = forward_in;
        if (!Detail::Normalize(forward)) {
            valid_ = false;
            return false;
        }
        const StabilizerTune defaults{};
        const float z = Detail::ClampOr(zoom, 1.0f, MaxZoom, 1.0f);
        const float base = Detail::ClampOr(
            Detail::ClampOr(tune.world, 0.0f, MaxZoom, defaults.world) / z, 0.0f, 1.0f, 1.0f);
        if (!valid_ || !(dt > 0.0f) || dt > 0.25f) {
            aim_ = last_ = forward;
            speed_ = lag_ = 0.0f;
            gain_ = base;
            valid_ = true;
            aim = aim_;
            return true;
        }
        constexpr float Rad = 3.14159265f / 180.0f;
        constexpr float PanFrom = PanFromDps * Rad;
        constexpr float PanScale = 1.0f / ((PanFullDps - PanFromDps) * Rad);
        const float max_lag =
            Detail::ClampOr(tune.max_lag_deg, 0.0f, 10.0f, defaults.max_lag_deg) * Rad;
        // The head's step. Its chord stands in for its angle (within 0.03% below 5 degrees a
        // sample); the speed is capped so a near-zero `dt` cannot blow it up.
        const Dir step{forward[0] - last_[0], forward[1] - last_[1], forward[2] - last_[2]};
        const float step2 = Detail::Dot(step, step);
        speed_ += Detail::Alpha(dt, 3.0f) * (std::min(std::sqrt(step2) / dt, 50.0f) - speed_);
        const float pan = Detail::Smoothstep((speed_ - PanFrom) * PanScale);
        // 1. The aim takes its share of the head's step: world speed for small moves, all of it
        // on a deliberate pan.
        const float gain = base + (1.0f - base) * pan;
        Dir next{aim_[0] + gain * step[0], aim_[1] + gain * step[1], aim_[2] + gain * step[2]};
        if (!Detail::Normalize(next)) next = forward;
        // 2. Moving away from the aim past the edge: dragged along progressively. Swaying back is
        // not, so the sway after a slow turn ratchets the aim back toward the middle. `lag_` is
        // the aim's angle from the last head; inside the edge nothing here runs.
        const float edge = StabilizerEdge * max_lag;
        if (lag_ > edge && max_lag > 0.0f) {
            const float after = Detail::Angle(next, forward);
            if (after > lag_) {
                const float drag = Detail::Smoothstep((lag_ - edge) / (max_lag - edge));
                next = Detail::OnArc(forward, next, lag_ + (after - lag_) * (1.0f - drag));
            }
        }
        // 3. While panning the aim catches up, so no offset is left when the turn ends.
        if (pan > 0.0f) {
            next = Detail::Toward(next, forward, pan * (1.0f - std::exp(-dt / PanCatchS)),
                                  forward);
        }
        // 4. Never more than max_lag behind.
        lag_ = Detail::Angle(forward, next);
        if (lag_ > max_lag) {
            next = Detail::OnArc(forward, next, max_lag);
            lag_ = max_lag;
        }
        // The share of this step the aim took, for Extrapolate.
        const Dir moved{next[0] - aim_[0], next[1] - aim_[1], next[2] - aim_[2]};
        gain_ = step2 > 1e-12f
                    ? Detail::ClampOr(Detail::Dot(moved, step) / step2, 0.0f, 1.0f, gain)
                    : gain;
        aim_ = next;
        last_ = forward;
        aim = aim_;
        return true;
    }

    // The share of the head's last step the aim took (Extrapolate's `gain`).
    float Gain() const {
        return gain_;
    }

private:
    Dir aim_{0.0f, 0.0f, -1.0f};
    Dir last_{0.0f, 0.0f, -1.0f};
    float speed_{0.0f}; // rad/s, filtered
    float lag_{0.0f};   // rad: aim_ from last_
    float gain_{1.0f};
    bool valid_{false};
};

// The aim carried from the head forward it was computed against to another head sample's (the
// renderer's may be a frame newer or older): `gain` of the difference, as the Stabilizer would
// have taken it. Without the carry, a fast turn would show that difference magnified. The aim
// unchanged when either head is invalid; the head itself past 10 degrees (a recenter).
inline Dir Extrapolate(const Dir& aim, const Dir& head_then_in, const Dir& head_now_in,
                       float gain) {
    Dir then = head_then_in, now = head_now_in;
    if (!Detail::Normalize(then) || !Detail::Normalize(now)) return aim;
    if (!(Detail::Dot(then, now) >= 0.98480775f)) return now; // cos(10 degrees)
    const float k = Detail::ClampOr(gain, 0.0f, 1.0f, 1.0f);
    Dir out{aim[0] + k * (now[0] - then[0]), aim[1] + k * (now[1] - then[1]),
            aim[2] + k * (now[2] - then[2])};
    return Detail::Normalize(out) ? out : aim;
}

// The forward (-Z) of a head orientation `q` (x, y, z, w); {0, 0, 0} when `q` is invalid.
inline Dir HeadForward(const std::array<float, 4>& q_in) {
    std::array<float, 4> q = q_in;
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!std::isfinite(n) || n < 1e-6f) return {0.0f, 0.0f, 0.0f};
    for (float& c : q) c /= n;
    return {-2.0f * (q[3] * q[1] + q[0] * q[2]), 2.0f * (q[3] * q[0] - q[1] * q[2]),
            -1.0f + 2.0f * (q[0] * q[0] + q[1] * q[1])};
}

// The aim in the frame of a head orientation `q` (x, y, z, w) as a shift of the eye's view window
// in tangent units (x right, y up). {0, 0} when either is invalid or the aim is not ahead.
inline std::array<float, 2> WindowShift(const std::array<float, 4>& q_in, const Dir& aim_in) {
    std::array<float, 4> q = q_in;
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    Dir aim = aim_in;
    if (!std::isfinite(n) || n < 1e-6f || !Detail::Normalize(aim)) return {0.0f, 0.0f};
    for (float& c : q) c /= n;
    // The inverse rotation (conjugate): v' = v + w t + u x t, t = 2 u x v, u = -q.xyz.
    const Dir u{-q[0], -q[1], -q[2]};
    const Dir t{2.0f * (u[1] * aim[2] - u[2] * aim[1]), 2.0f * (u[2] * aim[0] - u[0] * aim[2]),
                2.0f * (u[0] * aim[1] - u[1] * aim[0])};
    const Dir local{aim[0] + q[3] * t[0] + (u[1] * t[2] - u[2] * t[1]),
                    aim[1] + q[3] * t[1] + (u[2] * t[0] - u[0] * t[2]),
                    aim[2] + q[3] * t[2] + (u[0] * t[1] - u[1] * t[0])};
    if (!(local[2] < -0.5f)) return {0.0f, 0.0f};
    return {local[0] / -local[2], local[1] / -local[2]};
}

// The zoomed eye's window in tangent units {left, right, up, down} (OpenXR signs: left and down
// negative): the eye's own window divided by the zoom and slid by `shift`, the slide kept within
// the eye's own window (so the view never leaves what the game drew for it). A non-finite or
// impossible input gives the plain narrowed window.
inline std::array<float, 4> ZoomWindow(const std::array<float, 4>& tan_lrud, float zoom,
                                       const std::array<float, 2>& shift) {
    const float z = Detail::ClampOr(zoom, 1.0f, MaxZoom, 1.0f);
    const float k = 1.0f - 1.0f / z;
    const float sx = Detail::ClampOr(shift[0], tan_lrud[0] * k, tan_lrud[1] * k, 0.0f);
    const float sy = Detail::ClampOr(shift[1], tan_lrud[3] * k, tan_lrud[2] * k, 0.0f);
    return {tan_lrud[0] / z + sx, tan_lrud[1] / z + sx, tan_lrud[2] / z + sy,
            tan_lrud[3] / z + sy};
}

// ---- One window per frame ------------------------------------------------------------------------
// Game:Main places the next camera update while a frame is still being drawn, so the frame's later
// draws can take a newer update than its first (vr_session DrawFrameRecord). Without the optic
// that changes no eye projection (the headset FOV stays the same), but the zoom window and its
// slide move with every head sample: two windows in one frame. The temporal upscalers then refuse
// the frame (two scene cameras) and objects drawn with either window jump against each other, the
// more the higher the zoom (2026-10-31: DLSS off through the monocle, things jumping up
// and down, most at max zoom). So the frame's first draw takes the window and every later draw of
// that frame gets the same one.
struct Window {
    int eye{-1};
    float zoom{1.0f};
    float strength{0.0f};
    std::array<float, 2> shift{};
};

class FrameWindow {
public:
    // `frame`: the frame being drawn (moved on at every flip). `take`: the caller draws the frame,
    // so its first call in a frame sets the window; any caller before that gets its own `own`.
    Window Get(std::uint64_t frame, bool take, const Window& own) {
        if (!set_ || frame_ != frame) {
            if (!take) return own;
            frame_ = frame;
            window_ = own;
            set_ = true;
        }
        return window_;
    }

private:
    std::uint64_t frame_{};
    Window window_{};
    bool set_{false};
};
} // namespace VR::Monocular
