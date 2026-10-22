// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

// Directional hit feedback (hit_vignette_directional, hit_haptics_directional): where a blow on
// the hunter came from relative to where your head looks, the red flash's wedge on the edges of
// the view and the share of the controllers' vibration. The damage hook (bloodborne_tuning.cpp)
// resolves the source of each hit on the hunter and publishes it (Channel); the presenter
// (openxr_presenter.cpp) draws the hit vignette weighted toward it (vr_overlay.frag mirrors
// RingWeight) and plays the hit side's controller stronger. Header-only, no engine dependencies
// besides the seqlock (tests/test_hit_direction.cpp).

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/vr/seqlock.h"

namespace VR::HitDir {

constexpr float Pi = 3.14159265358979f;
constexpr float Deg = Pi / 180.0f;

// An angle in (-pi, pi].
inline float Wrap(float a) {
    if (!std::isfinite(a)) {
        return 0.0f;
    }
    a = std::remainder(a, 2.0f * Pi);
    return a <= -Pi ? a + 2.0f * Pi : a;
}

struct V3 {
    float x{}, y{}, z{};
};

inline bool Finite(const V3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

enum class Source : std::uint8_t {
    None = 0,     // no direction: the plain (non-directional) flash
    HitPoint = 1, // the game's hit position (the damage result) off the hunter's centre
    Attacker = 2, // the attacking character's position
};

// The source is the attacker. The damage result's hit position is a point on the hunter's own
// body, only 0.2 m off his physics position, so the body's lean and the capsule's offset swing its
// angle: in the mock (2026-10-06, the scourge beast) it was within 33 degrees of the attacker's
// direction for 3 of 8 blows and 82 to 143 degrees off it, often on the other side, for 4, while
// the attacker's direction matched where the beast was every time. It is only logged
// (`UseHitPoint`: false); without an attacker (his own Molotov, a trap, a grab's close embrace)
// the flash is the plain ring.
constexpr bool UseHitPoint = false;

inline const char* SourceName(Source s) {
    switch (s) {
    case Source::HitPoint:
        return "hit point";
    case Source::Attacker:
        return "attacker";
    default:
        return "none";
    }
}

// The hit position counts when it is at least HitPointMinOffset off the hunter's vertical centre
// line (game units, about metres; nearer, its side is noise), at most HitPointMaxOffset from it
// and within HitPointMaxHeight above or below his feet. The attacker counts from
// AttackerMinDistance to AttackerMaxDistance away (horizontally).
constexpr float HitPointMinOffset = 0.15f;
constexpr float HitPointMaxOffset = 3.0f;
constexpr float HitPointMaxHeight = 3.5f;
constexpr float AttackerMinDistance = 0.30f;
constexpr float AttackerMaxDistance = 100.0f;

// What the damage hook read for one hit on the hunter (world positions, game units).
struct Sample {
    bool center_ok{false};
    V3 center{}; // the hunter (his physics position: his feet)
    bool hit_ok{false};
    V3 hit{}; // the damage result's hit position
    bool attacker_ok{false};
    V3 attacker{}; // the attacking character (none when he hit himself)
};

struct Resolved {
    Source source{Source::None};
    float world_yaw{0.0f}; // radians, the game's heading (forward = (sin yaw, 0, cos yaw))
    float distance{0.0f};  // horizontal, from the hunter's centre (game units)
};

// The game's heading of a horizontal direction (dx, dz): forward = (sin yaw, 0, cos yaw), yaw +
// = turned right (right = (cos yaw, 0, -sin yaw)).
inline float WorldYaw(float dx, float dz) {
    return std::atan2(dx, dz);
}

// The direction of a source from the hunter's centre (the head's height and his eye a few cm in
// front of it do not change which side it is on): the attacker (and the hit position after it
// only with UseHitPoint).
inline Resolved FromPoint(const V3& center, const V3& p, Source source, float min_h, float max_h,
                          float max_dy) {
    const float dx = p.x - center.x;
    const float dz = p.z - center.z;
    const float h = std::sqrt(dx * dx + dz * dz);
    if (!(h >= min_h && h <= max_h && std::abs(p.y - center.y) <= max_dy)) {
        return {};
    }
    return {source, WorldYaw(dx, dz), h};
}

inline Resolved Resolve(const Sample& s, bool use_hit_point = UseHitPoint) {
    if (!s.center_ok || !Finite(s.center)) {
        return {};
    }
    if (s.attacker_ok && Finite(s.attacker)) {
        const Resolved r = FromPoint(s.center, s.attacker, Source::Attacker, AttackerMinDistance,
                                     AttackerMaxDistance, 1e9f);
        if (r.source != Source::None) {
            return r;
        }
    }
    if (use_hit_point && s.hit_ok && Finite(s.hit)) {
        return FromPoint(s.center, s.hit, Source::HitPoint, HitPointMinOffset, HitPointMaxOffset,
                         HitPointMaxHeight);
    }
    return {};
}

// Where the source is relative to where the head looks (`view_yaw`: the render camera's heading,
// the same convention): radians, 0 = straight ahead, + = to the right, +-pi = behind.
inline float Azimuth(float world_yaw, float view_yaw) {
    return Wrap(world_yaw - view_yaw);
}

// ---- The flash: a wedge of the hit vignette's ring --------------------------------------------
// The direction maps onto the edge of the view like a compass seen from above: ahead is the top
// edge, right the right edge, behind the bottom edge (it reaches the lower corners on both sides),
// left the left edge. The ring's weight there is (1 - focus) + focus * ((1 + cos d) / 2)^power,
// d the angle around the view axis from the wedge's centre: with WedgePower 4 it is half at 47
// degrees from the centre and 6% at 90. A hit from ahead keeps FrontFocus of the plain ring (the
// top edge is the least visible part of a Quest 3's view, and what hit you is in front of you);
// from FocusFromDeg off ahead to FocusFullDeg it narrows to the wedge alone.
constexpr float WedgePower = 4.0f;
constexpr float FrontFocus = 0.5f;
constexpr float FocusFromDeg = 20.0f;
constexpr float FocusFullDeg = 60.0f;

struct Wedge {
    float x{0.0f}; // the wedge's centre around the view axis: x right, y up (unit)
    float y{1.0f};
    float focus{0.0f}; // 0 = the plain ring, 1 = only the wedge
    float power{WedgePower};
};

inline float SmoothStep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

inline Wedge WedgeFor(float azimuth) {
    const float a = Wrap(azimuth);
    Wedge w;
    w.x = std::sin(a);
    w.y = std::cos(a);
    const float t = SmoothStep(FocusFromDeg * Deg, FocusFullDeg * Deg, std::abs(a));
    w.focus = FrontFocus + (1.0f - FrontFocus) * t;
    return w;
}

// The ring's weight toward (px, py) around the view axis (x right, y up, any length), 0 .. 1.
// vr_overlay.frag computes the same per pixel.
inline float RingWeight(const Wedge& w, float px, float py) {
    const float len = std::sqrt(px * px + py * py);
    const float c = len > 1e-6f ? (px * w.x + py * w.y) / len : 0.0f;
    const float wedge = std::pow(std::max(0.5f + 0.5f * c, 0.0f), w.power);
    return (1.0f - w.focus) + w.focus * wedge;
}

// ---- The controllers ----------------------------------------------------------------------------
// The hand on the hit's side keeps its full vibration; the other one gets less the more the hit
// came from the side: 1 - (1 - FarHandMin) * |sin azimuth| (FarHandMin straight from the side,
// both full from ahead or behind). The game's own rumble for the blow is weighted by these gains
// (RumbleWeighting); the hit's own feel is a small knock on BOTH hands, a little stronger toward
// the blow (core/vr/haptic_output.h Haptics::Combat::HitTaken takes these gains; since 2026-10-06
// it replaced the thump below, ThumpAmplitude on hands with a gain of ThumpMinGain or more).
constexpr float FarHandMin = 0.3f;
constexpr float ThumpMinGain = 0.5f;

struct Hands {
    float left{1.0f};
    float right{1.0f};
};

// The game's rumble for a blow on the hunter (the large motor, 0.8 on the right hand) starts 0 to
// 0.6 s after the damage event (measured in the mock: 0.0, 0.55 and 0.6 s) and lasts 0.3 to
// 1.6 s. A rumble that is on, or starts, within RumbleStartWindowNs of a directional hit is
// weighted by that hit's gains until it stops (both motors at 0), at most RumbleMaxNs after the
// hit: the near hand gets the stronger motor's level, the other x its gain. Any other rumble is
// left as it is.
constexpr std::int64_t RumbleStartWindowNs = 800'000'000;
constexpr std::int64_t RumbleMaxNs = 2'500'000'000;

struct RumbleWeighting {
    std::int64_t hit_ns{0}; // the newest directional hit (0: none pending)
    Hands gain{};
    bool active{false}; // weighting a rumble now

    void Hit(std::int64_t now, Hands g) {
        hit_ns = now;
        gain = g;
    }

    void Reset() {
        *this = RumbleWeighting{};
    }

    // One frame: `rumble` is the stronger motor's level. True while the rumble is weighted.
    bool Step(std::int64_t now, float rumble) {
        if (hit_ns == 0) {
            active = false;
            return false;
        }
        const std::int64_t age = now - hit_ns;
        if (age > RumbleMaxNs || age < 0) {
            Reset();
            return false;
        }
        if (!(rumble > 0.0f)) {
            active = false; // it stopped: a new one only counts within the start window
            return false;
        }
        if (age <= RumbleStartWindowNs) {
            active = true;
        }
        return active;
    }
};

// This frame's rumble per hand (left = the small motor, right = the large one, as the presenter
// maps them), weighted when `weighted`.
inline Hands WeightRumble(float left, float right, bool weighted, Hands gain) {
    if (!weighted) {
        return {left, right};
    }
    const float both = std::max(left, right);
    return {both * gain.left, both * gain.right};
}

inline Hands HandGains(float azimuth) {
    const float s = std::sin(Wrap(azimuth));
    const float other = 1.0f - (1.0f - FarHandMin) * std::abs(s); // ("far" is a Windows macro)
    return s >= 0.0f ? Hands{other, 1.0f} : Hands{1.0f, other};
}

// The thump of a hit that took `fraction` of max HP: 0.55 for a scratch to 0.9 from a quarter.
inline float ThumpAmplitude(float fraction) {
    return 0.55f + 0.35f * std::clamp(fraction / 0.25f, 0.0f, 1.0f);
}

// "ahead", "right", "behind-left", ... (logs).
inline const char* SideName(float azimuth) {
    const float d = Wrap(azimuth) / Deg;
    if (d >= -22.5f && d <= 22.5f) {
        return "ahead";
    }
    if (d > 22.5f && d <= 67.5f) {
        return "ahead-right";
    }
    if (d > 67.5f && d <= 112.5f) {
        return "right";
    }
    if (d > 112.5f && d <= 157.5f) {
        return "behind-right";
    }
    if (d < -22.5f && d >= -67.5f) {
        return "ahead-left";
    }
    if (d < -67.5f && d >= -112.5f) {
        return "left";
    }
    if (d < -112.5f && d >= -157.5f) {
        return "behind-left";
    }
    return "behind";
}

// ---- The damage hook -> the presenter ---------------------------------------------------------
struct Event {
    std::uint32_t serial{0};   // hits on the hunter resolved so far
    std::int64_t steady_ns{0}; // when (steady clock)
    std::uint8_t source{0};    // Source
    bool view_ok{false};       // a first-person view was known: azimuth is valid
    float world_yaw{0.0f};     // the source's heading from the hunter (radians)
    float view_yaw{0.0f};      // the view's heading then
    float azimuth{0.0f};       // Azimuth(world_yaw, view_yaw)
    float distance{0.0f};      // game units (logs)
};

// A hit (an HP drop paired with the damage event, at most 0.5 s after it or 0.15 s before it)
// takes the newest event within this of it.
constexpr std::int64_t EventMaxAgeNs = 700'000'000;

// One writer at a time: the damage hook holds its own lock around Store.
inline SeqLock<Event>& Channel() {
    static SeqLock<Event> channel;
    return channel;
}

// The direction now: from the view's heading now when it is known (turning your head toward the
// blow moves the wedge toward the top), else the event's own.
inline float CurrentAzimuth(const Event& e, bool view_now_ok, float view_yaw_now) {
    return view_now_ok ? Azimuth(e.world_yaw, view_yaw_now) : e.azimuth;
}

} // namespace VR::HitDir
