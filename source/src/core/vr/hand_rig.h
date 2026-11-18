// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cmath>

#include "core/vr/hand_grip.h"

// The bare hands' fingers from the controller (2026-11-02, unarmed mode only: "if we
// touch the buttons the thumb is closed, otherwise open ... if we touch or press the grip, then
// hands close"). Per hand three curls, 0 open .. 1 closed: the thumb (resting on a face button,
// the stick or the thumb rest), the index finger (off the trigger it points; resting on it, curled
// round it; pulled, closed) and grip squeeze (ring/little close first, then middle).
// The presenter follows them every XR frame (Target, Follow); the body bends that hand's fingers into ShapeOf over the game's
// own (bloodborne_body.cpp ApplyFingerRig). Pure math on the c0000 skeleton.
namespace VR::Body::Rig {

struct Input {
    bool thumb_touch{false}; // on a face button, the stick or the thumb rest
    bool trigger_touch{false};
    float trigger{0.0f}; // 0..1
    float squeeze{0.0f}; // 0..1
};

// The controller's sensors, as far as it has shown them: one that never reported a touch has
// none (WMR, Vive, a runtime that refused them), and its thumb and index rest on it instead.
struct Senses {
    bool thumb{false};
    bool trigger{false};
    void Note(const Input& in) {
        thumb = thumb || in.thumb_touch;
        trigger = trigger || in.trigger_touch;
    }
};

struct Curl {
    float thumb{0.0f};
    float index{0.0f};
    float grip{0.0f}; // raw squeeze; ShapeOf gives the middle a later closing range
};

constexpr float IndexOnTrigger = 0.35f; // resting on the trigger: curled round it
constexpr float ThumbUnsensed = 0.6f;   // no thumb sensors: resting on the controller
constexpr float FollowS = 0.04f;        // the fingers settle in about 3x this (no snap)
constexpr float OpenShare = 0.12f;      // the open hand keeps this much of the fist: relaxed

[[nodiscard]] inline float Clamp01(float v) {
    return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
}

[[nodiscard]] inline Curl Target(const Input& in, const Senses& has) {
    Curl c{};
    const float pull = Clamp01(in.trigger);
    c.thumb = has.thumb ? (in.thumb_touch ? 1.0f : 0.0f) : ThumbUnsensed;
    const bool on = !has.trigger || in.trigger_touch || pull > 0.02f;
    c.index = on ? IndexOnTrigger + (1.0f - IndexOnTrigger) * pull : 0.0f;
    c.grip = Clamp01(in.squeeze);
    return c;
}

// `now` followed toward `want` over `dt` seconds.
[[nodiscard]] inline Curl Follow(Curl now, Curl want, float dt) {
    const float k = dt > 0.0f ? 1.0f - std::exp(-dt / FollowS) : 0.0f;
    const auto step = [k](float a, float b) { return Clamp01(a + (b - a) * k); };
    return {step(now.thumb, want.thumb), step(now.index, want.index), step(now.grip, want.grip)};
}

// The hand's shape: each finger between the open hand and the fist round the controller (the
// game's own fist round a weapon's grip, Grip::Kind::Handle) by its curl.
[[nodiscard]] inline Grip::Shape ShapeOf(const Curl& c) {
    constexpr Grip::Shape fist = Grip::ShapeOf(Grip::Kind::Handle);
    Grip::Shape out{};
    // Close ring/little by 10% squeeze, leaving room before middle starts at 35%.
    // Keep raw grip in Curl for smoothing and the existing body-channel layout.
    const float grip = Clamp01(c.grip);
    const float lower = Clamp01((grip - 0.02f) / 0.08f);
    const float middle = Clamp01((grip - 0.35f) / 0.50f);
    for (int f = 0; f < 5; ++f) {
        const float curl = f == 0 ? c.thumb : f == 1 ? c.index : f == 2 ? middle : lower;
        const float share = OpenShare + (1.0f - OpenShare) * Clamp01(curl);
        const auto i = static_cast<std::size_t>(f);
        for (std::size_t j = 0; j < 3; ++j) {
            out.bend[i][j] = fist.bend[i][j] * share;
        }
        out.spread[i] = fist.spread[i] * share;
    }
    out.thumb_oppose =
        fist.thumb_oppose * (OpenShare + (1.0f - OpenShare) * Clamp01(c.thumb));
    return out;
}

} // namespace VR::Body::Rig
