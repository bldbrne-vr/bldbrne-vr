// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <cmath>

// A gap in the camera updates (vr.ini camera_stale_ms: no camera tick for that long) and what the
// next tick does with the view's heading and the 6DoF origin.
//
// Until 2026-10-08 every such gap was a camera takeover: the game heading of tracking forward was
// taken from the game camera again and the 6DoF origin (the point your head's lean and duck are
// measured from) from your head where it was at that moment. The takeover is for the game's own
// camera taking over (a load, a teleport, a cutscene with its own camera): the hunter and his
// camera are somewhere else afterwards. But a hitch is a gap too (a shader compile, a stall of the
// game, the mock's eye dumps; the tester's vr.ini has camera_stale_ms=150): ducked 0.5 m during a
// 0.2 s hitch, the origin went to the ducked head, so standing up put the view 0.5 m above the
// hunter until a recenter (the duck track's runs/before: 2.068 m against 1.568).
//
// Now a gap resumes the view as it was (the heading, the orbit and both origins kept) when only
// time passed: the game camera is where it was left (its anchor, the follow camera's pivot on the
// hunter, within JumpM, and its yaw within TurnRad of what the last update left it at). A load,
// teleport or area change moves the hunter and his camera: that is still a takeover. So is the
// very first update (no origin yet), a return from a cinematic (VR::UpdateCinematic asks for it),
// the virtual screen or the headset coming back (those are not gaps: the bridge stopped driving,
// and the takeover after them is unchanged). A recenter takes the origin as always.
namespace VR::CameraGap {

// The game camera moved by more than this in a gap: not a hitch (game units per world_scale, so
// metres; the comfort camera's teleport threshold is 2 game units).
constexpr float JumpM = 2.0f;
// ... or turned by more than this (radians). The game camera's yaw is what the last update
// wrote (first person) or the game's own (lock-on, the aim camera): a hitch leaves it there.
constexpr float TurnRad = 20.0f * 3.14159265358979323846f / 180.0f;

// What the last driven update left the game camera at.
struct Last {
    bool valid{false};
    float yaw{0.0f}; // radians, game convention
    bool anchor_valid{false};
    float anchor[3]{}; // the follow camera's pivot (world, game units)
};

enum class Verdict : unsigned char {
    First,  // no driven update yet: take over (the first origin)
    Resume, // only time passed: keep the heading and the origin
    Jumped, // the game camera moved more than JumpM: take over
    Turned, // ... or turned more than TurnRad: take over
};

struct Decision {
    Verdict verdict{Verdict::First};
    float moved_m{-1.0f};  // the anchor's move in the gap (m; -1 unknown)
    float turned_rad{0.0f}; // |the yaw's change|
    [[nodiscard]] bool Resume() const {
        return verdict == Verdict::Resume;
    }
};

inline float WrapPi(float a) {
    constexpr float Pi = 3.14159265358979323846f;
    if (!std::isfinite(a)) {
        return 0.0f;
    }
    a = std::fmod(a + Pi, 2.0f * Pi);
    if (a < 0.0f) {
        a += 2.0f * Pi;
    }
    return a - Pi;
}

// After a gap: `game_yaw` the game camera's yaw now, `anchor` its pivot now (nullptr: unknown, the
// yaw decides alone), `world_scale` game units per metre.
inline Decision Decide(const Last& last, float game_yaw, const float* anchor, float world_scale) {
    Decision d{};
    if (!last.valid || !std::isfinite(game_yaw)) {
        d.verdict = Verdict::First;
        return d;
    }
    if (anchor != nullptr && last.anchor_valid) {
        const float dx = anchor[0] - last.anchor[0];
        const float dy = anchor[1] - last.anchor[1];
        const float dz = anchor[2] - last.anchor[2];
        const float scale = std::isfinite(world_scale) && world_scale > 1e-3f ? world_scale : 1.0f;
        d.moved_m = std::sqrt(dx * dx + dy * dy + dz * dz) / scale;
        if (!(d.moved_m <= JumpM)) { // NaN counts as a jump
            d.verdict = Verdict::Jumped;
            return d;
        }
    }
    d.turned_rad = std::abs(WrapPi(game_yaw - last.yaw));
    d.verdict = d.turned_rad > TurnRad ? Verdict::Turned : Verdict::Resume;
    return d;
}

inline const char* VerdictName(Verdict v) {
    switch (v) {
    case Verdict::First:
        return "first";
    case Verdict::Resume:
        return "resumed";
    case Verdict::Jumped:
        return "the game camera jumped";
    case Verdict::Turned:
        return "the game camera turned";
    }
    return "?";
}

} // namespace VR::CameraGap
