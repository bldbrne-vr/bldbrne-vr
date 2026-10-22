// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include "core/vr/holsters.h"
#include "core/vr/reviewed_item_grips.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

// Quick items in VR: the pure parts (no emulator, OpenXR or Vulkan), shared by the presenter
// (the arc's controller below), the game side (core/vr/bloodborne_quickitems.cpp) and the unit
// tests (tests/test_quick_items.cpp).
//
// Y opens a small arc of the quick items equipped in the game's Equipment menu (the same six
// slots in the same order, with their icons and counts) above the hunter's left hand, drawn into
// the game frame. Hovering a card with the hunter's hand (the one you see) lights it; the left
// grip takes the item: the game's own held model in his hand where it has one. A throwable flies
// when you swing and let go of the grip (the game's projectile, with your release velocity); a
// usable is used with a deliberate pull of the left trigger (a tap does nothing). A new squeeze
// within a second of a throw or a use takes the next one of the same item, without the arc. Y
// again puts it away; the gun is hidden while an item is in the hand and back at once after.
// The gun: the left trigger never fires it while an item is in the hand (held, being used, or
// waiting for the game's own use); with the arc open and the hand empty the gun stays in it and
// the trigger fires it at the pull, as without the arc (the arc closes), unless the pull comes
// with a squeeze of the grip (a whole-hand grab).
//
// Threads:
//  - the presenter's input thread runs Controller (buttons, grip, trigger, haptics, pad);
//  - the main player's update (site A) publishes the Snapshot of the six slots and carries out
//    the Requests (select, use, throw) through the game's own functions;
//  - the render-camera hook (loop thread) places the arc when it opens, decides which card the
//    hunter's palm hovers (Hover) and builds the in-frame draw list for that camera.
namespace VR::Quick {

constexpr int SlotCount = 6;

// ---- Small math (LOCAL tracking space, metres) -----------------------------------------------

struct V3 {
    float x{0.0f}, y{0.0f}, z{0.0f};
};
inline V3 operator+(V3 a, V3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline V3 operator-(V3 a, V3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline V3 operator*(V3 a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}
inline float Dot(V3 a, V3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline V3 Cross(V3 a, V3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float Length(V3 a) {
    return std::sqrt(Dot(a, a));
}
inline V3 Normalize(V3 a, V3 fallback = {0.0f, 0.0f, 1.0f}) {
    const float l = Length(a);
    return l > 1e-6f ? a * (1.0f / l) : fallback;
}
// A unit quaternion (x, y, z, w; Hamilton, as OpenXR's).
struct Qt {
    float x{0.0f}, y{0.0f}, z{0.0f}, w{1.0f};
};
inline V3 Rotate(Qt q, V3 v) {
    const V3 u{q.x, q.y, q.z};
    const V3 t = Cross(u, v) * 2.0f;
    return v + t * q.w + Cross(u, t);
}
// The angular velocity (rad/s, the frame both orientations are in) that turns `from` into `to`
// in `dt` seconds: the shorter way round.
inline V3 AngularVelocity(Qt from, Qt to, float dt) {
    // d = to * conj(from)
    Qt d{to.w * -from.x + to.x * from.w + to.y * -from.z - to.z * -from.y,
         to.w * -from.y - to.x * -from.z + to.y * from.w + to.z * -from.x,
         to.w * -from.z + to.x * -from.y - to.y * -from.x + to.z * from.w,
         to.w * from.w - to.x * -from.x - to.y * -from.y - to.z * -from.z};
    if (d.w < 0.0f) {
        d = {-d.x, -d.y, -d.z, -d.w};
    }
    const float n = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (n < 1e-7f || dt <= 1e-5f) {
        return {};
    }
    const float angle = 2.0f * std::atan2(n, d.w);
    return V3{d.x, d.y, d.z} * (angle / (n * dt));
}

// ---- Goods -------------------------------------------------------------------------------------

// How an item is used from the arc.
enum class UseKind : std::uint8_t {
    None,    // empty
    Instant, // the game's UseGoods at once (the effect is a SpEffect: no animation needed)
    Bullet,  // the game's UseGoods at once (a drink whose effect is a bullet at the hunter)
    Square,  // select it and press Square: the game's own animation (marks, bells, tools, ...)
    Throw,   // a throwable: swing and let go
};

// Release speed classes of throwables (plan_throwables.md 1.3).
enum class ThrowClass : std::uint8_t { None, Bottle, Knife, Stone, Coin };

// Our own mesh for an item the game has no held model for (in-frame fallback).
enum class MeshKind : std::uint8_t {
    Token,
    Bottle,
    Knife,
    Pebble,
    Flask,
    Pellet,
    Crystal,
    Paper,
    Ash,
    Count,
};

struct GoodsClass {
    UseKind use{UseKind::None};
    ThrowClass throw_class{ThrowClass::None};
    std::int16_t bullet{-1}; // the flying bullet id of a throwable (matched at the launch)
    std::int8_t dmy{-1};     // the dummy poly UseGoods gets (vanilla TAE event 65's)
    MeshKind mesh{MeshKind::Token};
};

// EquipParamGoods ids of Bloodborne 1.09 (quickitems_mech.md 2.2, plan_throwables.md 2.1). Items
// not listed take the game's own animation (select + Square).
constexpr GoodsClass Classify(std::int32_t goods) {
    using enum UseKind;
    switch (goods) {
    // SpEffect items: UseGoods applies the effect, the animation adds nothing gameplay-relevant.
    case 1120: // Blue Elixir
        return {Instant, ThrowClass::None, -1, 17, MeshKind::Flask};
    case 1110: // Beast Blood Pellet
        return {Instant, ThrowClass::None, -1, 1, MeshKind::Pellet};
    case 1500: // Madman's Knowledge
    case 1501: // Great One's Wisdom
        return {Instant, ThrowClass::None, -1, 1, MeshKind::Crystal};
    case 1300: // Fire Paper
    case 1310: // Empty Phantasm Shell
    case 1320: // Bolt Paper
        return {Instant, ThrowClass::None, -1, 17, MeshKind::Paper};
    case 1330: // Bone Marrow Ash
        return {Instant, ThrowClass::None, -1, 17, MeshKind::Ash};
    case 701:  // Blood of Arianna
    case 702:  // Blood of Adella
    case 703:  // Iosefka's Blood Vial
    case 705:  // Queenly Blood
    case 706:  // Blood of Adeline
        return {Instant, ThrowClass::None, -1, 0, MeshKind::Flask};
    // Drinks whose effect is a bullet at the hunter (Antidote, Sedative, Lead Elixir).
    case 1100:
        return {Bullet, ThrowClass::None, -1, 1, MeshKind::Flask};
    case 1101:
    case 2030:
        return {Bullet, ThrowClass::None, -1, 17, MeshKind::Flask};
    // Throwables (flying bullet ids; rope molotovs: the child of the spawner).
    case 1200:
        return {Throw, ThrowClass::Bottle, 100, 3, MeshKind::Bottle};
    case 1201:
        return {Throw, ThrowClass::Bottle, 105, 3, MeshKind::Bottle};
    case 1210:
        return {Throw, ThrowClass::Knife, 121, 3, MeshKind::Knife};
    case 1220:
        return {Throw, ThrowClass::Bottle, 150, 3, MeshKind::Bottle};
    case 1230:
        return {Throw, ThrowClass::Stone, 170, 3, MeshKind::Pebble};
    case 1240:
        return {Throw, ThrowClass::Knife, 120, 3, MeshKind::Knife};
    case 1250:
        return {Throw, ThrowClass::Bottle, 111, 3, MeshKind::Bottle};
    case 1251:
        return {Throw, ThrowClass::Bottle, 115, 3, MeshKind::Bottle};
    case 1260:
        return {Throw, ThrowClass::Bottle, 160, 3, MeshKind::Bottle};
    case 1270:
        return {Throw, ThrowClass::Bottle, 180, 3, MeshKind::Bottle};
    case 1410: // Shining Coins
        return {Throw, ThrowClass::Coin, 300, 3, MeshKind::Pebble};
    default:
        break;
    }
    if ((goods >= 1510 && goods <= 1519) || (goods >= 1590 && goods <= 1594)) {
        return {Instant, ThrowClass::None, -1, 1, MeshKind::Crystal}; // Coldbloods
    }
    return {goods >= 0 ? Square : None, ThrowClass::None, -1, -1, MeshKind::Token};
}

// The game's own held effect (an FFX, with a small model for most, spawned on a hand dummy) for
// goods with that goodsUseAnim (EquipParamGoods +0x3E) and sfxVariationId (+0x04): vanilla item
// clips spawn effect 200 * 100 + variation there with TAE event 114 (throws, drinks, crushes).
// -1 = none.
constexpr std::int32_t HeldFfx(std::int32_t use_anim, std::int32_t variation) {
    switch (use_anim) {
    case 2:  // knives
    case 3:  // bottles, urn, pebble, cocktail, mist
    case 9:  // crush (coldbloods, Madman's Knowledge: a glow)
    case 10: // drinks (Blue Elixir, Sedative, Lead Elixir)
    case 13: // rope molotovs
    case 34: // delayed molotov
        return variation >= 0 && variation < 100 ? 20000 + variation : -1;
    default:
        return -1;
    }
}

// Which of those effects go into the hunter's left palm (dummy 3) next to the game model we draw
// there (HeldLookOf): none. They are authored for the right hand's item dummies and for the
// moment of use: the drinks' would put a second bottle into the fist, the Molotov's flares its
// rag's spawn flash and fire where its bottle would be on dummy 17 (not at the neck of the bottle
// in his hand), and the coldbloods' "glow" is the crush's cloud of mist round the hunter (all
// checked in the mock).
constexpr bool UseHeldEffect(std::int32_t goods, std::int32_t use_anim) {
    (void)goods;
    (void)use_anim;
    return false;
}

// How the hunter holds a quick item: the game's own model of it (its held effect's model from the
// sfx archive, life size, drawn in-frame with the game's camera, lit like the hand holding it) or,
// for unmapped items, a fallback card with the item's own menu icon; the grip his fingers close into; and where
// it sits in the
// fist. Placement in the palm dummy's frame (dummy 3 on L_Hand, metres): the model's point
// (0, hold_y, 0) goes to the grip's centre (GripCentre), its +Y along the fist's channel (dummy
// +Y, out on the thumb's side), then turned by `pitch` (about dummy X: a lean out of the channel),
// `yaw` (about the channel) and `roll` (about dummy Z). `scale` x quick_held_scale is its size:
// the game's models are life size (a Molotov is 30 cm with its neck); a cut-out is 1 = 9 cm.
struct HeldLook {
    std::int32_t model{-1}; // sfx sNNNNN; -1: the icon cut-out
    std::uint8_t grip{0};   // VR::Body::Grip::Kind (core/vr/hand_grip.h)
    float scale{1.0f};
    float hold_y{0.0f};
    float pitch_deg{0.0f};
    float yaw_deg{0.0f};
    float roll_deg{0.0f};
    float lift_m{0.0f}; // dummy +Y, across the palm toward the thumb
    float out_m{0.0f};  // dummy +Z, along the palm toward the fingers
    float side_m{0.0f}; // dummy +X, outward from the palm
    float fit_m{0.0f}; // optional longest dimension; centre native bounds before fitting
    float hold_x{0.0f}, hold_z{0.0f};
};
// Grip kinds (core/vr/hand_grip.h Kind) without including the body headers here.
inline constexpr std::uint8_t GripNeck = 1, GripHandle = 2, GripBall = 3, GripWide = 4,
                              GripFlask = 5, GripPinch = 6, GripVial = 7, GripBottle = 8,
                              GripCup = 9, GripTray = 10, GripCradle = 11, GripBook = 12,
                              GripHookPinch = 13, GripRubPinch = 14;
// The fist's channel in dummy 3 (FFX 173's offset: where the game's own vial sits).
inline constexpr std::array<float, 3> GripCentre{0.0f, -0.02f, 0.025f};

constexpr HeldLook HeldLookOf(std::int32_t goods) {
    if (const auto* p = ItemGrips::Find(goods)) {
        return {p->model, p->kind, p->scale, p->hold, p->pitch, p->yaw, p->roll,
                p->lift, p->out, p->side, p->fit, p->pivot_x, p->pivot_z};
    }
    return {-1, GripPinch, 1, 0};
}

// Launch speed per class: a factor on the gain (a light knife or stone leaves the fingers faster
// than a bottle: knives fly straighter and faster, a pebble is light), the soft cap's fallback
// when the item's own BulletParam row cannot be read: its best-arc cap (m/s; ThrowCapOf derives it
// from the row: these are its values, at the default throw_range_factor 1.6, for the Molotov, the
// throwing knife, the pebble and the coins) and the game's own range of it (m; ThrowCapAt's
// reach; 0: an item the game drops, its cap fixed). No minimum: real ballistics, a weak flick
// drops short (the 12:30 spec).
struct ThrowLimits {
    float factor{1.0f};
    float max_speed{14.0f};
    float range{13.42f};
};
constexpr ThrowLimits LimitsOf(ThrowClass c) {
    switch (c) {
    case ThrowClass::Knife:
        return {1.3f, 17.7f, 20.9f};
    case ThrowClass::Stone:
        return {1.2f, 16.0f, 17.33f};
    case ThrowClass::Coin:
        return {0.8f, 6.0f, 0.0f};
    default:
        return {1.0f, 14.0f, 13.42f};
    }
}

inline float SmoothStep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// The launch speed curve (throw_gain_curve): the hand's speed through a power `curve` about
// LaunchCurveRef (3 m/s, a natural throw in the headset): 1 = linear; below 1 a natural flick
// leaves a little faster than linear (2 m/s: +8% at 0.8) and a hard throw a little slower
// (5 m/s: -10%), so the gain need not be pushed up to make a relaxed throw carry, and the soft cap
// has more room for a hard one.
inline constexpr float LaunchCurveRef = 3.0f;
inline float LaunchCurve(float speed, float curve) {
    if (speed <= 0.0f) {
        return 0.0f;
    }
    return LaunchCurveRef * std::pow(speed / LaunchCurveRef, std::clamp(curve, 0.3f, 1.5f));
}

// The launch velocity for a release velocity `v` (m/s): gain times the class factor times the
// speed curve, the direction kept. Uncapped: the projectile hook caps it with the item's own range
// (SoftCap, ThrowCapAt).
inline V3 LaunchVelocity(V3 v, ThrowClass c, float gain, float curve = 1.0f) {
    const float speed = Length(v);
    if (speed < 1e-6f) {
        return {};
    }
    return v * (gain * LimitsOf(c).factor * LaunchCurve(speed, curve) / speed);
}

// The launch's pitch (degrees above the horizontal) and the share of throw_lift_deg it gets: all
// of it from flat up to 15 degrees up and down to 30 degrees down, fading to none at 40 degrees up
// (a lob keeps its own arc) and at 60 degrees down (a throw at the feet stays one).
inline float PitchOf(V3 v) {
    return std::atan2(v.y, std::hypot(v.x, v.z)) * (180.0f / 3.14159265f);
}
inline float LiftShare(float pitch_deg) {
    return SmoothStep(40.0f, 15.0f, pitch_deg) * SmoothStep(-60.0f, -30.0f, pitch_deg);
}

// The throw tilted up by `lift_deg` x LiftShare (throw_lift_deg; the speed and the yaw kept). In
// the headset a flat throw at something at eye level drops short (real gravity, no weight in the
// hand to judge the arc by, the distance underestimated): the game's own throw leaves 10 degrees
// above the hunter's facing for the same reason; a few degrees make a throw aimed at a target
// carry to it. `lifted_deg`: the degrees added.
inline V3 LiftPitch(V3 v, float lift_deg, float* lifted_deg = nullptr) {
    if (lifted_deg != nullptr) {
        *lifted_deg = 0.0f;
    }
    const float h = std::hypot(v.x, v.z);
    const float speed = Length(v);
    if (lift_deg == 0.0f || h < 1e-4f || speed < 1e-4f) {
        return v;
    }
    const float pitch = PitchOf(v);
    const float add = lift_deg * LiftShare(pitch);
    if (lifted_deg != nullptr) {
        *lifted_deg = add;
    }
    const float p1 = (pitch + add) * (3.14159265f / 180.0f);
    const float k = speed * std::cos(p1) / h;
    return {v.x * k, speed * std::sin(p1), v.z * k};
}

// `from`'s direction turned toward `to`'s by at most `max_deg` (a unit vector; `from`'s when
// either is zero).
inline V3 TurnToward(V3 from, V3 to, float max_deg) {
    const V3 a = Normalize(from);
    if (Length(from) < 1e-6f || Length(to) < 1e-6f) {
        return a;
    }
    const V3 b = Normalize(to);
    const float angle = std::acos(std::clamp(Dot(a, b), -1.0f, 1.0f));
    const float limit = std::max(max_deg, 0.0f) * (3.14159265f / 180.0f);
    if (angle <= limit || angle < 1e-5f) {
        return b;
    }
    if (angle > 3.1415f) {
        return a; // opposite: no defined turn
    }
    const float s = std::sin(angle);
    return Normalize(a * (std::sin(angle - limit) / s) + b * (std::sin(limit) / s), a);
}

// Above `knee_frac` of `cap` (throw_cap_knee) the speed bends smoothly toward it (never past it,
// never down); the direction is kept. A low knee (0.5) keeps a hard throw a little faster than a
// firm one all the way up instead of flattening everything from 75% of the cap.
inline V3 SoftCap(V3 v, float cap, float knee_frac = 0.5f) {
    const float speed = Length(v);
    if (speed < 1e-4f || cap <= 0.0f) {
        return v;
    }
    const float knee = std::clamp(knee_frac, 0.0f, 0.95f) * cap;
    if (speed <= knee) {
        return v;
    }
    const float room = cap - knee;
    return v * ((knee + room * std::tanh((speed - knee) / room)) / speed);
}

// ---- The game's own throw (BulletParam) -------------------------------------------------------

// The fields of a BulletParam row the projectile's ballistic update (mode 2, 0x19F9F90) reads,
// plus the launch's pitch (plan_throwables.md 2.1 / 2.3).
struct BulletFlight {
    float life{4.0f};        // +0x10 s
    float dist{0.0f};        // +0x14 m travelled before the "out of range" values apply
    float gravity_in{0.0f};  // +0x1C m/s^2
    float gravity_out{0.0f}; // +0x20
    float init_speed{0.0f};  // +0x28 m/s
    float accel_in{0.0f};    // +0x2C m/s^2 along the velocity
    float accel_out{0.0f};   // +0x30
    float max_speed{0.0f};   // +0x34
    float min_speed{0.0f};   // +0x38
    float accel_time{0.0f};  // +0x3C s without acceleration
    float pitch_deg{0.0f};   // +0x90 shootAngleXZ (s8): up from the thrower's facing
};

// How far the game's own throw of that bullet flies on flat ground: launched with its initial
// speed along the thrower's facing pitched by `pitch_deg`, from `h` m above the ground, stepped
// like the game's mode-2 update (v += dt (a dir(v) - g up), |v| clamped into [min, max], then
// p += v dt; the "out" gravity and acceleration once `dist` m are travelled). Metres; 0 when it
// never comes back down within its life.
inline float VanillaRange(const BulletFlight& b, float h, float dt = 1.0f / 60.0f) {
    const float pitch = b.pitch_deg * (3.14159265f / 180.0f);
    float vx = b.init_speed * std::cos(pitch);
    float vy = b.init_speed * std::sin(pitch);
    float x = 0.0f, y = 0.0f, travelled = 0.0f;
    for (float t = 0.0f; t < std::min(b.life, 10.0f); t += dt) {
        const bool out = travelled > b.dist;
        const float g = out ? b.gravity_out : b.gravity_in;
        const float a = t < b.accel_time ? 0.0f : (out ? b.accel_out : b.accel_in);
        float s = std::hypot(vx, vy);
        const float dx = s > 1e-6f ? vx / s : 1.0f;
        const float dy = s > 1e-6f ? vy / s : 0.0f;
        vx += dt * a * dx;
        vy += dt * (a * dy - g);
        s = std::hypot(vx, vy);
        const float clamped = b.min_speed > s ? b.min_speed : std::min(b.max_speed, s);
        if (s > 1e-6f) {
            vx *= clamped / s;
            vy *= clamped / s;
        }
        const float nx = x + vx * dt;
        const float ny = y + vy * dt;
        travelled += std::hypot(vx, vy) * dt;
        if (ny <= -h) {
            return x + (nx - x) * ((-h - y) / (ny - y));
        }
        x = nx;
        y = ny;
    }
    return 0.0f;
}

// The slowest launch that reaches `range` m on flat ground `h` m below the release under real
// gravity `g` (at the best angle): v^2 = g (sqrt(range^2 + h^2) - h).
inline float SpeedForRange(float range, float h, float g) {
    return std::sqrt(std::max(g, 0.1f) * std::max(std::hypot(range, h) - h, 0.0f));
}

// Where SoftCap bends a throw of this item: the speed whose best real-gravity arc reaches
// `range_factor` times the game's own throw of it (VanillaRange from `h`), so a firm throw at a
// throwing angle lands about where the game would throw it and a hard one clearly further (1.6:
// its best arc 1.6 x the game's), never across the map. An
// item the game drops or slams rather than throws (pitched down, or under 3 m: coins, the rope and
// delayed Molotovs) keeps its class's cap.
inline float ThrowCapOf(const BulletFlight& b, ThrowClass c, float range_factor, float h,
                        float g, float* vanilla_range = nullptr) {
    const float range = VanillaRange(b, h);
    if (vanilla_range != nullptr) {
        *vanilla_range = range;
    }
    if (b.pitch_deg < -20.0f || range < 3.0f) {
        return LimitsOf(c).max_speed;
    }
    return SpeedForRange(range * range_factor, h, g);
}

// The launch that reaches `range` m on flat ground `h` m below the release under real gravity `g`
// at the pitch `pitch_deg` (y = h + x tan p - g x^2 / (2 v^2 cos^2 p) = 0 at x = range); 0 when no
// launch at that pitch gets there (pitched at or below the line to that point).
inline float SpeedForRangeAt(float range, float h, float g, float pitch_deg) {
    const float p = pitch_deg * (3.14159265f / 180.0f);
    const float c = std::cos(p);
    const float rise = h + range * std::tan(p);
    if (range <= 0.0f || c < 1e-3f || rise <= 1e-4f) {
        return 0.0f;
    }
    return std::sqrt(std::max(g, 0.1f) * range * range / (2.0f * c * c * rise));
}

// The soft cap of a throw pitched `pitch_deg` (throw_cap_flat_deg = `flat_deg`): the speed whose
// real-gravity arc at that pitch lands `reach` m away (throw_range_factor x the game's own throw
// of the item), so a hard throw at any angle lands at most there: a lob is held to the best arc's
// speed (ThrowCapOf's, from the best angle up: just under 45 degrees from a raised hand), a flat
// throw may leave much faster (punchy, flat, realistic for a hard throw) up to the speed at
// `flat_deg` (flatter and downward throws keep that one, which would otherwise be unbounded).
// `flat_deg` >= 45: the best arc's speed at every pitch. `reach` 0 (an item the game drops):
// `fixed`.
inline float ThrowCapAt(float reach, float h, float g, float pitch_deg, float flat_deg,
                        float fixed) {
    if (reach <= 0.0f) {
        return fixed;
    }
    const float best = SpeedForRange(reach, h, g);
    if (flat_deg >= 45.0f) {
        return best;
    }
    // The best arc's angle: tan = v / sqrt(v^2 + 2 g h) at its speed.
    const float best_deg =
        std::atan(best / std::sqrt(best * best + 2.0f * std::max(g, 0.1f) * std::max(h, 0.0f))) *
        (180.0f / 3.14159265f);
    const float at =
        SpeedForRangeAt(reach, h, g, std::clamp(pitch_deg, std::min(flat_deg, best_deg), best_deg));
    return std::max(at, best);
}

// The reach a throw of this item is capped to (ThrowCapAt): `range_factor` x the game's own throw
// of it (VanillaRange from `h`), or 0 for an item the game drops or slams rather than throws
// (pitched down, or under 3 m: coins, the rope and delayed Molotovs; their class's fixed cap).
inline float ThrowReachOf(const BulletFlight& b, float range_factor, float h) {
    const float range = VanillaRange(b, h);
    if (b.pitch_deg < -20.0f || range < 3.0f) {
        return 0.0f;
    }
    return range * range_factor;
}

// ---- Lock-on assist ---------------------------------------------------------------------------

// The very gentle lock-on assist (the tester's spec): only the yaw of the launch, turned
// `strength` of the way toward the target while the throw already points within `cone_deg` of it
// (full strength to 60% of the cone, fading to none at its edge: a clear miss is never pulled
// onto the target). The pitch, the horizontal speed and so the range are kept. `turn_deg`: the
// yaw it added (+ = counter-clockwise seen from above), `off_deg`: how far off the throw was.
inline V3 YawAssist(V3 v, V3 to_target, float strength, float cone_deg, float& turn_deg,
                    float& off_deg) {
    turn_deg = 0.0f;
    off_deg = 0.0f;
    const float h = std::hypot(v.x, v.z);
    const float th = std::hypot(to_target.x, to_target.z);
    if (h < 1e-3f || th < 0.3f) {
        return v;
    }
    constexpr float Deg = 180.0f / 3.14159265f;
    const float yaw = std::atan2(v.x, v.z);
    float delta = std::atan2(to_target.x, to_target.z) - yaw;
    delta = std::remainder(delta, 2.0f * 3.14159265f);
    off_deg = std::abs(delta) * Deg;
    if (strength <= 0.0f || cone_deg <= 0.0f) {
        return v;
    }
    const float weight = strength * SmoothStep(cone_deg, 0.6f * cone_deg, off_deg);
    if (weight <= 0.0f) {
        return v;
    }
    const float turn = weight * delta;
    turn_deg = turn * Deg;
    const float yaw1 = yaw + turn;
    return {h * std::sin(yaw1), v.y, h * std::cos(yaw1)};
}

// The unlocked yaw assist's target (throw_assist_unlocked: a throw that is not a knife, not locked
// on): of `points` (the game's lock-on candidates' lock points, the knife assist's list; `from` =
// where the throw starts), the one whose heading is nearest the throw's (the yaw only, what
// YawAssist turns), strictly within `cone_deg` of it, 0.5 .. `max_dist` away and at least 0.5
// away horizontally (straight above or below has no heading). -1 when none; `off_deg` its yaw off.
inline int PickYawTarget(V3 v, V3 from, const V3* points, int count, float cone_deg, float max_dist,
                         float& off_deg) {
    off_deg = 0.0f;
    const float h = std::hypot(v.x, v.z);
    if (points == nullptr || cone_deg <= 0.0f || !(h >= 1e-3f)) {
        return -1;
    }
    constexpr float Deg = 180.0f / 3.14159265f;
    const float yaw = std::atan2(v.x, v.z);
    int best = -1;
    float best_off = cone_deg;
    for (int i = 0; i < count; ++i) {
        const V3 to = points[i] - from;
        const float dist = Length(to);
        if (!(dist >= 0.5f && dist <= max_dist) || !(std::hypot(to.x, to.z) >= 0.5f)) {
            continue; // (NaN fails every test)
        }
        const float off =
            std::abs(std::remainder(std::atan2(to.x, to.z) - yaw, 2.0f * 3.14159265f)) * Deg;
        if (off < best_off) {
            best = i;
            best_off = off;
        }
    }
    if (best >= 0) {
        off_deg = best_off;
    }
    return best;
}

// ---- Knife assist (knife_assist) -------------------------------------------------------------

// The launch direction (unit) at `speed` whose arc under gravity `g` (along -Y) passes through
// `to` (relative to the launch), on the low arc; false when no launch at that speed reaches it or
// it is (nearly) straight above or below.
inline bool BallisticDir(V3 to, float speed, float g, V3& dir) {
    const float x = std::hypot(to.x, to.z);
    if (x < 1e-3f || speed < 1e-3f || !std::isfinite(x) || !std::isfinite(to.y)) {
        return false;
    }
    if (g <= 1e-4f) {
        dir = Normalize(to);
        return true;
    }
    const float s2 = speed * speed;
    const float disc = s2 * s2 - g * (g * x * x + 2.0f * to.y * s2);
    if (disc < 0.0f) {
        return false;
    }
    const float tan_p = (s2 - std::sqrt(disc)) / (g * x);
    const float c = 1.0f / std::sqrt(1.0f + tan_p * tan_p);
    dir = {to.x / x * c, tan_p * c, to.z / x * c};
    return true;
}

// Where a throw `v` should go to land on `to` (relative to the launch) at its own speed: the arc
// that drops onto it (gravity compensated) or, beyond the throw's reach, the target's heading at
// the throw's own pitch (it still drops short: a weak flick stays one). `off_deg`: the throw's
// angle from that direction.
struct KnifeAim {
    bool ok{false};
    bool reachable{false};
    V3 ideal{};
    float off_deg{180.0f};
};
inline KnifeAim AimFor(V3 v, V3 to, float g) {
    KnifeAim a{};
    const float speed = Length(v);
    if (speed < 1e-3f || Length(to) < 0.5f) {
        return a;
    }
    V3 d{};
    a.reachable = BallisticDir(to, speed, g, d);
    if (!a.reachable) {
        const float h = std::hypot(v.x, v.z);
        const float th = std::hypot(to.x, to.z);
        if (h < 1e-4f || th < 1e-4f) {
            return a;
        }
        const float k = h / speed;
        d = {to.x / th * k, v.y / speed, to.z / th * k};
    }
    a.ok = true;
    a.ideal = d;
    a.off_deg = std::acos(std::clamp(Dot(Normalize(v), d), -1.0f, 1.0f)) * (180.0f / 3.14159265f);
    return a;
}

// The knife assist's share at `off_deg`: `strength` within `cone_deg`, fading smoothly to none at
// 4/3 of it (no edge to feel, a clear miss stays a miss).
inline float KnifeWeight(float off_deg, float strength, float cone_deg) {
    if (strength <= 0.0f || cone_deg <= 0.0f) {
        return 0.0f;
    }
    return std::clamp(strength, 0.0f, 1.0f) *
           SmoothStep(cone_deg * (4.0f / 3.0f), cone_deg, off_deg);
}

struct KnifeOut {
    int target{-1};          // the chosen point's index (-1: none)
    bool reachable{false};   // the throw's speed reaches it (else the yaw only)
    float off_deg{0.0f};     // the throw's angle from the arc onto it, before ...
    float after_deg{0.0f};   // ... and after the assist
    float weight{0.0f};      // the share applied
    float distance{0.0f};    // to the target (the launch's units)
};

// The Throwing / Poison Knife assist (the tester's 2026-10-06 spec: "much stronger just for the
// throwing knives"): `points` are the targets' lock-on points (`count`, launch-relative after
// subtracting `from`); `lock` (-1 = none) is the lock-on target's index and wins while the throw
// is within the fade's edge (4/3 cone_deg) of it, else the point the throw is nearest in angle
// within that edge. The throw's direction (yaw and pitch) turns `strength` x KnifeWeight of the
// way toward the arc that drops onto that point at the throw's own speed under `g`; the speed is
// kept (the flight stays a real arc from the hand, just better aimed). Points nearer than 0.5 or
// further than `max_dist` (the launch's units) are skipped.
inline V3 KnifeAssist(V3 v, V3 from, const V3* points, int count, int lock, float g, float strength,
                      float cone_deg, float max_dist, KnifeOut& out) {
    out = {};
    if (strength <= 0.0f || cone_deg <= 0.0f || Length(v) < 1e-3f || points == nullptr) {
        return v;
    }
    const float edge = cone_deg * (4.0f / 3.0f);
    int best = -1;
    KnifeAim best_aim{};
    for (int i = 0; i < count; ++i) {
        const V3 to = points[i] - from;
        const float dist = Length(to);
        if (!(dist >= 0.5f && dist <= max_dist)) {
            continue;
        }
        const KnifeAim aim = AimFor(v, to, g);
        if (!aim.ok || aim.off_deg >= edge) {
            continue;
        }
        if (i == lock) {
            best = i;
            best_aim = aim;
            break;
        }
        if (best < 0 || aim.off_deg < best_aim.off_deg) {
            best = i;
            best_aim = aim;
        }
    }
    if (best < 0) {
        return v;
    }
    out.target = best;
    out.reachable = best_aim.reachable;
    out.off_deg = best_aim.off_deg;
    out.distance = Length(points[best] - from);
    out.weight = KnifeWeight(best_aim.off_deg, strength, cone_deg);
    out.after_deg = best_aim.off_deg;
    if (out.weight <= 0.0f) {
        return v;
    }
    const float speed = Length(v);
    const V3 d = TurnToward(v, best_aim.ideal, out.weight * best_aim.off_deg);
    out.after_deg =
        std::acos(std::clamp(Dot(d, best_aim.ideal), -1.0f, 1.0f)) * (180.0f / 3.14159265f);
    return d * speed;
}

// How much of the gain a throw gets (the 12:30 spec): a proper throw (a wind-up with the
// elbow bent and the hand back near or behind the shoulder, the forearm whipping through, the
// hand's path curving over the shoulder) the full gain; a straight push from in front of the
// chest `push_factor` of it. Blended continuously from the measured motion, no gesture classes:
//  - `windup_m`: how far in front of the shoulder, along the throw's horizontal direction, the
//    hand was at its farthest back during the wind-up (level with the shoulder or behind it: a
//    full wind-up; 20 cm in front of it, a hand starting in front of the chest: none);
//  - `omega`: the controller's peak angular speed (rad/s) just before the release (>= 18: a whip
//    of the forearm and wrist, as the tester's real overhand throws: 19-37 rad/s; <= 8: none, so a
//    push with some turn of the wrist stays a push);
//  - `turn_deg`: how far the hand's path turned on its way to the release (VelocityHistory::Turn;
//    >= 60 degrees: an arm swung round the shoulder or elbow; <= 25: a straight push).
// Any one of them makes a full throw.
inline float ThrowPower(float windup_m, float omega, float turn_deg, float push_factor) {
    const float windup = SmoothStep(0.20f, 0.0f, windup_m);
    const float whip = SmoothStep(8.0f, 18.0f, omega);
    const float curve = SmoothStep(25.0f, 60.0f, turn_deg);
    const float s = 1.0f - (1.0f - windup) * (1.0f - whip) * (1.0f - curve);
    return push_factor + (1.0f - push_factor) * s;
}

// Where the thrown item is relative to the controller's grip (OpenXR grip space of the left hand:
// -Z from the little finger to the thumb, +X the way the palm faces, metres): the centre of the
// item the hunter holds (HeldLookOf: a Molotov hangs by its neck below the fist, a knife's blade
// stands out of the thumb's side, a pebble sits in the fingers). The release velocity is taken
// there: the hand's linear velocity plus the controller's angular velocity x this offset.
constexpr V3 ReleaseOffsetOf(std::int32_t goods) {
    switch (goods) {
    case 1200: // Molotovs: the bottle's middle, ~12 cm below the fist
    case 1201:
    case 1250:
    case 1251:
        return {0.02f, 0.0f, 0.12f};
    case 1270: // Numbing Mist: the flask below its neck
        return {0.02f, 0.0f, 0.08f};
    case 1260: // Pungent Blood Cocktail: held by its shoulder
        return {0.02f, 0.0f, 0.05f};
    case 1210: // knives: the blade's middle, out of the thumb's side
    case 1240:
        return {0.02f, 0.0f, -0.06f};
    case 1220: // Oil Urn: cupped on the palm
        return {0.07f, 0.0f, 0.0f};
    case 1230: // Pebble: in the fingers
        return {0.06f, 0.0f, -0.01f};
    default:
        return {};
    }
}

// The spin part of the release velocity: angular velocity `w` (rad/s, LOCAL) x the item's offset
// `r` (grip space, ReleaseOffsetOf) turned by the grip's orientation `q`, at most `max_mps`.
inline V3 SpinVelocity(V3 w, Qt q, V3 r, float max_mps = 2.5f) {
    const V3 s = Cross(w, Rotate(q, r));
    const float l = Length(s);
    return l > max_mps ? s * (max_mps / l) : s;
}

// ---- Release velocity (HIGGS style) ------------------------------------------------------------

// The hand's recent velocities (LOCAL, m/s) at the times they are for: the grip located at the
// input frame's own time (the runtime's freshest estimate, Controller::fresh_) or the poses
// predicted for the display time (41-50 ms ahead over Link: the fallback, Controller::history_).
// The release velocity is the peak speed sample of the `window_s` up to the release averaged with
// its neighbours that are nearly as fast (85%: a broad peak is smoothed, a hand still speeding up
// is not averaged down), so the slowing of the hand as the fingers open does not count. Samples
// for after the release (display-time poses predicted past it) never count: they are the
// runtime's guess of the follow-through.
class VelocityHistory {
public:
    void Push(double t, V3 v) {
        samples[head] = {t, v};
        head = (head + 1) % samples.size();
        count = std::min<std::size_t>(count + 1, samples.size());
    }
    void Clear() {
        count = 0;
    }
    // Any sample for the `window_s` up to `now`.
    [[nodiscard]] bool Has(double now, double window_s) const {
        for (std::size_t i = 0; i < count; ++i) {
            const double t = At(i).t;
            if (t <= now + 1e-4 && now - t <= window_s) {
                return true;
            }
        }
        return false;
    }
    // The hand still speeding up at `now` (Valve's release rule: a grip opened before the hand's
    // peak waits a frame or two for it): the newest sample at or before `now` (within `window_s`)
    // is at least `gain` x the one before it.
    [[nodiscard]] bool Rising(double now, float gain = 1.02f, double window_s = 0.05) const {
        int newest = -1;
        for (std::size_t i = 0; i < count; ++i) {
            if (At(i).t <= now + 1e-4) {
                newest = static_cast<int>(i); // oldest first: the last one is the newest
            }
        }
        if (newest < 1 || now - At(static_cast<std::size_t>(newest)).t > window_s) {
            return false;
        }
        const float s1 = Length(At(static_cast<std::size_t>(newest)).v);
        const float s0 = Length(At(static_cast<std::size_t>(newest - 1)).v);
        return s1 > 1e-3f && s1 >= gain * s0;
    }
    // A one-frame tracking jump: over 5 m/s and more than twice as fast as both neighbours up to
    // `now` (a real hand never doubles its speed and drops back between frames at throwing
    // speeds). Release and PeakBefore never read one (a jump 230 ms before an ordinary lob once
    // became the throw). The newest sample has no later neighbour yet: only throw_max_hand_speed
    // guards it (a throw that waits for the hand's peak sees it with both).
    [[nodiscard]] bool Spike(std::size_t i, double now) const {
        const float s = Length(At(i).v);
        if (s < 5.0f || i == 0 || i + 1 >= count || At(i + 1).t > now + 1e-4) {
            return false;
        }
        return s > 2.0f * std::max(Length(At(i - 1).v), Length(At(i + 1).v));
    }
    // Zero when no sample lies in the window (the hand was not tracked just before the release):
    // a stale velocity never throws, the item goes back.
    V3 Release(double now, double window_s = 0.12) const {
        int peak = -1;
        float best = -1.0f;
        for (std::size_t i = 0; i < count; ++i) {
            const Sample& s = At(i);
            const float speed = Length(s.v);
            if (now - s.t <= window_s && s.t <= now + 1e-4 && speed >= best &&
                !Spike(i, now)) { // the latest peak
                best = speed;
                peak = static_cast<int>(i);
            }
        }
        if (peak < 0) {
            return {};
        }
        V3 sum{};
        int n = 0;
        for (int i = peak - 1; i <= peak + 1; ++i) {
            if (i >= 0 && i < static_cast<int>(count) &&
                At(static_cast<std::size_t>(i)).t <= now + 1e-4 &&
                !Spike(static_cast<std::size_t>(i), now) &&
                Length(At(static_cast<std::size_t>(i)).v) >= 0.85f * best) {
                sum = sum + At(static_cast<std::size_t>(i)).v;
                ++n;
            }
        }
        return sum * (1.0f / static_cast<float>(n));
    }
    // The hand peaks before it is fully extended and is already slowing down when the fingers
    // open (the tester's 2026-10-04 observation; HIGGS and SteamVR also look back for the peak): the
    // strongest velocity of the last `window_s` (averaged like Release), counted only while it
    // points within `max_angle_deg` of `release` (a wind-up away from the target never throws)
    // and only when the hand is still moving at release (`min_release` m/s: the controller passes
    // throw_min_speed, so a hand slowing to a drift as it puts the item back never throws). Zero when nothing qualifies. The controller takes its speed and turns
    // the release's direction at most throw_peak_turn_deg toward it (TurnToward): the earlier
    // sample is often further up the swing (the tester's two such throws: 46-56 degrees up).
    V3 PeakBefore(double now, double window_s, V3 release, float max_angle_deg = 75.0f,
                  float min_release = 0.6f) const {
        const float rel_speed = Length(release);
        if (rel_speed < min_release) {
            return {};
        }
        const V3 dir = release * (1.0f / rel_speed);
        const float min_cos = std::cos(max_angle_deg * (3.14159265f / 180.0f));
        int peak = -1;
        float best = -1.0f;
        for (std::size_t i = 0; i < count; ++i) {
            const Sample& s = At(i);
            const float speed = Length(s.v);
            if (now - s.t <= window_s && s.t <= now + 1e-4 && speed > 1e-3f &&
                Dot(s.v, dir) / speed >= min_cos && speed >= best && !Spike(i, now)) {
                best = speed;
                peak = static_cast<int>(i);
            }
        }
        if (peak < 0) {
            return {};
        }
        V3 sum{};
        int n = 0;
        for (int i = peak - 1; i <= peak + 1; ++i) {
            if (i >= 0 && i < static_cast<int>(count)) {
                const Sample& s = At(static_cast<std::size_t>(i));
                if (s.t <= now + 1e-4 && Length(s.v) >= 0.85f * best &&
                    !Spike(static_cast<std::size_t>(i), now) &&
                    Dot(s.v, dir) / Length(s.v) >= min_cos) {
                    sum = sum + s.v;
                    ++n;
                }
            }
        }
        return n > 0 ? sum * (1.0f / static_cast<float>(n)) : V3{};
    }
    // The fastest velocity among the poses already predicted for after `now` (the runtime's
    // guess of the follow-through, which Release leaves out): logged with a throw, so a headset
    // session shows whether the grip opens while the hand is still speeding up.
    float PeakAfter(double now) const {
        float best = 0.0f;
        for (std::size_t i = 0; i < count; ++i) {
            if (At(i).t > now + 1e-4) {
                best = std::max(best, Length(At(i).v));
            }
        }
        return best;
    }
    // How far the hand's path turned on its way to a release with velocity `release` (degrees):
    // the largest angle between it and the velocity of any sample of the last `window_s` moving
    // at least `frac` of its speed (and 1 m/s). About 0 for a straight push; an arm swung round
    // the shoulder or the elbow turns 60 degrees and more.
    float Turn(double now, V3 release, double window_s = 0.3, float frac = 0.35f) const {
        const float speed = Length(release);
        if (speed < 1e-3f) {
            return 0.0f;
        }
        const V3 dir = release * (1.0f / speed);
        float lowest = 1.0f;
        for (std::size_t i = 0; i < count; ++i) {
            const Sample& s = At(i);
            const float l = Length(s.v);
            if (now - s.t <= window_s && s.t <= now + 1e-4 && l >= std::max(1.0f, frac * speed)) {
                lowest = std::min(lowest, Dot(s.v, dir) / l);
            }
        }
        return std::acos(std::clamp(lowest, -1.0f, 1.0f)) * (180.0f / 3.14159265f);
    }

private:
    struct Sample {
        double t{0.0};
        V3 v{};
    };
    // i = 0 is the oldest.
    const Sample& At(std::size_t i) const {
        return samples[(head + samples.size() - count + i) % samples.size()];
    }
    std::array<Sample, 48> samples{};
    std::size_t head{0};
    std::size_t count{0};
};

// The throw's wind-up: the hand relative to the shoulder (LOCAL, m) and the controller's angular
// speed (rad/s) over the last 0.7 s, for ThrowPower.
class WindupHistory {
public:
    void Push(double t, V3 rel, float omega) {
        samples[head] = {t, rel, omega};
        head = (head + 1) % samples.size();
        count = std::min<std::size_t>(count + 1, samples.size());
    }
    void Clear() {
        count = 0;
    }
    // At a release at `now` with the throw's direction `dir`: the hand's farthest-back position
    // in front of the shoulder along the horizontal part of `dir` over `window_s` (`fallback`
    // when the direction is mostly vertical or nothing was recorded) and the peak angular speed
    // of the last `omega_window_s`.
    void Measure(double now, V3 dir, float fallback, float& windup_m, float& omega,
                 double window_s = 0.7, double omega_window_s = 0.15) const {
        V3 h{dir.x, 0.0f, dir.z};
        const float hl = Length(h);
        const bool horizontal = hl > 0.35f * std::max(Length(dir), 1e-4f);
        h = horizontal ? h * (1.0f / hl) : V3{};
        windup_m = 1e9f;
        omega = 0.0f;
        for (std::size_t i = 0; i < count; ++i) {
            const Sample& s = samples[(head + samples.size() - count + i) % samples.size()];
            if (now - s.t <= window_s && horizontal) {
                windup_m = std::min(windup_m, Dot(s.rel, h));
            }
            if (now - s.t <= omega_window_s) {
                omega = std::max(omega, s.omega);
            }
        }
        if (windup_m > 1e8f) {
            windup_m = fallback;
        }
    }

private:
    struct Sample {
        double t{0.0};
        V3 rel{};
        float omega{0.0f};
    };
    std::array<Sample, 96> samples{};
    std::size_t head{0};
    std::size_t count{0};
};

// VR_THROW_TRACE (Config::throw_trace; diagnostics for headset sessions): every input frame's hand
// sample, so a release of a throwable logs the last throw_trace_s of them. It shows how the hand's
// speed rises and falls round the grip's opening, the poses predicted for the display time (what
// the throw uses) against the grip located at the input's own time (the runtime's freshest
// estimate), the item's spin against the grip's own velocity, the angular velocity from the
// orientations against the runtime's, the hand from the shoulder and the grip's travel.
class ThrowTrace {
public:
    struct Sample {
        double t{0.0};    // the input frame (s)
        float lead{0.0f}; // its pose is predicted for t + lead (the display time)
        V3 p{};           // the grip at the display time (m, LOCAL)
        V3 v{};           // ... its velocity (m/s, LOCAL)
        V3 spin{};        // the spin part added at the item (throw_spin x w x offset)
        V3 w{};           // the angular velocity from the orientations (rad/s, LOCAL)
        bool w_rt_valid{false};
        V3 w_rt{}; // the runtime's own angular velocity at the display time
        bool rel_valid{false};
        V3 rel{}; // the hand from the shoulder estimate (m, LOCAL)
        bool now_valid{false};
        V3 p_now{}; // the grip located at `t` itself (m, LOCAL)
        V3 v_now{};
        float grip{0.0f}; // the grip's analog value (left_scheme=2: the trigger's, which holds it)
    };
    void Push(const Sample& s) {
        samples[head] = s;
        head = (head + 1) % samples.size();
        count = std::min<std::size_t>(count + 1, samples.size());
    }
    void Clear() {
        count = 0;
    }
    [[nodiscard]] std::size_t Count() const {
        return count;
    }
    // i = 0 is the oldest.
    [[nodiscard]] const Sample& At(std::size_t i) const {
        return samples[(head + samples.size() - count + i) % samples.size()];
    }
    // What a release from the grip located at the input's own time would read (each with its
    // sample's spin): the newest such sample at or before `now` and the fastest of the last
    // `window_s`. Zero without such samples.
    void NowVelocity(double now, double window_s, float& at_release, float& peak) const {
        at_release = 0.0f;
        peak = 0.0f;
        for (std::size_t i = 0; i < count; ++i) {
            const Sample& s = At(i);
            if (!s.now_valid || s.t > now + 1e-4 || now - s.t > window_s) {
                continue;
            }
            const float speed = Length(s.v_now + s.spin);
            at_release = speed; // oldest first: the last one kept is the newest
            peak = std::max(peak, speed);
        }
    }
    // One line per sample whose input frame lies from `before_s` before the release at `t0` to
    // `after_s` after it (oldest first), each starting with `tag`: f = its input frame - `t0`,
    // d = its display time - `t0` (s; the poses the release ignores have d > 0), then the
    // sample's values.
    [[nodiscard]] std::string Lines(double t0, double before_s, double after_s,
                                    const std::string& tag) const {
        std::string text;
        char line[512];
        for (std::size_t i = 0; i < count; ++i) {
            const Sample& s = At(i);
            if (s.t > t0 + after_s + 1e-4 || t0 - s.t > before_s + 1e-4) {
                continue;
            }
            char rt[64] = "-";
            if (s.w_rt_valid) {
                std::snprintf(rt, sizeof(rt), "%.1f,%.1f,%.1f", s.w_rt.x, s.w_rt.y, s.w_rt.z);
            }
            char rel[64] = "-";
            if (s.rel_valid) {
                std::snprintf(rel, sizeof(rel), "%.3f,%.3f,%.3f", s.rel.x, s.rel.y, s.rel.z);
            }
            char fresh[128] = "-";
            if (s.now_valid) {
                std::snprintf(fresh, sizeof(fresh), "v %.3f,%.3f,%.3f |%.2f| p %.3f,%.3f,%.3f",
                              s.v_now.x, s.v_now.y, s.v_now.z, Length(s.v_now), s.p_now.x,
                              s.p_now.y, s.p_now.z);
            }
            std::snprintf(line, sizeof(line),
                          "%s f %+.4f d %+.4f p %.3f,%.3f,%.3f v %.3f,%.3f,%.3f |%.2f| s "
                          "%.2f,%.2f,%.2f w %.1f,%.1f,%.1f |%.1f| rt %s r %s g %.2f now %s",
                          tag.c_str(), s.t - t0, s.t + s.lead - t0, s.p.x, s.p.y, s.p.z, s.v.x,
                          s.v.y, s.v.z, Length(s.v), s.spin.x, s.spin.y, s.spin.z, s.w.x, s.w.y,
                          s.w.z, Length(s.w), rt, rel, s.grip, fresh);
            if (!text.empty()) {
                text += '\n';
            }
            text += line;
        }
        return text;
    }

private:
    std::array<Sample, 128> samples{}; // 1.0 s at 124 input frames/s (0.55 s are logged)
    std::size_t head{0};
    std::size_t count{0};
};

// ---- To the mouth (quick_use_mouth) ----------------------------------------------------------------

// The items the hunter eats or drinks (the tester's 2026-10-06 request: "bring a pellet or elixir to
// your mouth"): taken from the arc they stay in his hand and are used when the hand brings them to
// the mouth; let go of away from it they go back. From the goods params' goodsUseAnim (+0x3E; the
// 1.09 goods dump): 0 = the eating clip (a00 100000: Antidote, Beast Blood Pellet), 10 = the
// drinking clip (100050: Sedative, Blue Elixir, Lead Elixir). Everything else keeps its use:
// the coldbloods, Madman's Knowledge and the Blood Dreg are crushed in the fist (anim 9: used at
// the pull, they never come near the mouth), the papers rubbed on the blade (1), the ash on the
// gun (5). Bottled NPC blood and Iosefka's vial are drunk in VR despite native anim 28;
// throwables thrown, bells / marks / tools with the game's own animation. Anim 0 is also every
// gesture and upgrade material: the table is by goods id, the anim is the runtime's cross-check.
inline constexpr std::int32_t QueenlyBlood = 705;
constexpr bool BloodDrinkOf(std::int32_t goods) {
    return goods == 701 || goods == 702 || goods == 703 || goods == 706;
}
enum class MouthKind : std::uint8_t { None, Eat, Drink };
constexpr MouthKind MouthOf(std::int32_t goods) {
    if (BloodDrinkOf(goods)) return MouthKind::Drink;
    switch (goods) {
    case 1100: // Antidote
    case 1110: // Beast Blood Pellet
        return MouthKind::Eat;
    case 1101: // Sedative
    case 1120: // Blue Elixir
    case 2030: // Lead Elixir
        return MouthKind::Drink;
    default:
        return MouthKind::None;
    }
}
constexpr MouthKind MouthKindOfAnim(std::int32_t use_anim) {
    return use_anim == 0 ? MouthKind::Eat : use_anim == 10 ? MouthKind::Drink : MouthKind::None;
}
constexpr const char* MouthName(MouthKind k) {
    return k == MouthKind::Eat ? "eat" : k == MouthKind::Drink ? "drink" : "none";
}

// The mouth zone (LOCAL, metres): a ball round a point below and in front of the eyes, in the
// head's own frame (its pitch counts: a head tilted back to drink brings the mouth forward), that
// the left grip (the centre of the fist, where the item is) must reach. The fist holding a pellet
// or a bottle to the lips is a few cm in front of them, under the headset's lower edge.
struct MouthConfig {
    float down_m{0.08f};    // below the eyes along the head's up
    float ahead_m{0.08f};   // in front of the eyes along the view
    float radius_m{0.10f};  // the grip within this of that point: at the mouth ...
    float exit_m{0.14f};    // ... and away from it again past this (hysteresis)
    float dwell_s{0.15f};   // kept there this long, slowly ...
    float max_speed{0.6f};  // ... under this (m/s, the hand against the head): a hand passing the
                            // face is faster, one bringing something to the lips stops there
    float min_hold_s{0.20f}; // and the item in the hand at least this long
};

// The mouth point from the eyes (the view's position) and the view's forward (with its pitch).
inline V3 MouthPoint(V3 eye, V3 forward, float down_m, float ahead_m) {
    const V3 f = Normalize(forward, {0.0f, 0.0f, -1.0f});
    // The head's up: world up made perpendicular to the view (no roll).
    const V3 up = Normalize(V3{0.0f, 1.0f, 0.0f} - f * f.y, {0.0f, 1.0f, 0.0f});
    return eye + f * ahead_m - up * down_m;
}

// Deliberately brought to the mouth: the hand was away from it since the item was taken (it is
// taken on the arc, at least 32 cm from the eyes), reached the zone and stayed in it dwell_s
// moving slower than max_speed, with the arm the player's (not the game's animation sweeping it
// past the face), the item held min_hold_s. Fires once; the hand must leave (exit_m) and come
// back for another (a refused use, the next one of the stack taken at the mouth).
class MouthGate {
public:
    void Reset(double held_since) {
        *this = {};
        held_since_ = held_since;
    }
    // Every input frame while an item to eat or drink is in the hand. `rel_v`: the hand's
    // velocity against the head's. True on the frame it is at the mouth (Consume() it once used).
    bool Update(const MouthConfig& c, double t, bool valid, V3 hand, V3 rel_v, V3 mouth,
                bool arm) {
        const double dt = last_t_ >= 0.0 ? std::clamp(t - last_t_, 0.0, 0.1) : 0.0;
        last_t_ = t;
        if (!valid) {
            return false; // a lost frame neither counts nor breaks a dwell
        }
        distance_ = Length(hand - mouth);
        if (distance_ > c.exit_m) {
            armed_ = true;
            in_ = false;
            dwell_ = 0.0;
            return false;
        }
        if (!in_ && armed_ && distance_ <= c.radius_m) {
            in_ = true;
            dwell_ = 0.0;
        }
        if (!in_) {
            return false;
        }
        speed_ = Length(rel_v);
        if (speed_ <= c.max_speed && arm) {
            dwell_ += dt;
        } else {
            dwell_ = 0.0; // passing by (or the game's arm): the dwell starts again
        }
        return dwell_ >= c.dwell_s - 1e-6 && t - held_since_ >= c.min_hold_s - 1e-6;
    }
    // Used (or refused) here: the hand has to leave the zone and come back for the next.
    void Consume() {
        armed_ = false;
        in_ = false;
        dwell_ = 0.0;
    }
    [[nodiscard]] float Distance() const {
        return distance_;
    }
    [[nodiscard]] float Speed() const {
        return speed_;
    }
    [[nodiscard]] double Dwell() const {
        return dwell_;
    }
    [[nodiscard]] bool Armed() const {
        return armed_;
    }
    [[nodiscard]] double HeldSince() const {
        return held_since_;
    }

private:
    double held_since_{0.0};
    double last_t_{-1.0};
    double dwell_{0.0};
    float distance_{1e9f};
    float speed_{0.0f};
    bool armed_{false};
    bool in_{false};
};

// ---- Wiped along the blade (quick_use_wipe) ----------------------------------------------------

// The items the hunter rubs along his weapon's blade (the tester's 2026-10-06 request: "having to
// apply the firepaper on the weapon itself"): taken from the arc they stay in his LEFT hand and are
// applied when you wipe them along the right weapon's blade; let go of away from it they go back.
// From the goods params' goodsUseAnim (+0x3E; the 1.09 goods dump): 1 = the blade-wipe clip, used
// by exactly Fire Paper, Bolt Paper and the Empty Phantasm Shell (and their unused 113xx copies);
// the ash (5) is put on the gun, not the blade.
constexpr bool WipeOf(std::int32_t goods) {
    return goods == 1300 || goods == 1310 || goods == 1320; // Fire Paper, Empty Phantasm Shell, Bolt
}
constexpr bool WipeOfAnim(std::int32_t use_anim) {
    return use_anim == 1;
}

// The stroke (LOCAL metres, seconds): the paper (the item point in the hunter's left hand) within
// reach_m of the blade's segment (its hit capsule's axis, base -> tip) and moving along it, one
// way, at least `share` of the blade's length (at least min_travel_m, at most max_travel_m: a
// greatsword's 40% would be a half-metre stroke) within window_s; a turn back further than
// backtrack_m starts a new stroke. Not a tap (no travel), not a cross-cut (the travel across the
// blade at most 1 / along_ratio of the travel along it, past lateral_slack_m; and the stroke at
// least min_stroke_s: a slash through the blade is quicker), not from too far.
// The reach (2026-10-06 feel check): the point is the FIST's centre (GripCentre), and a fist laid
// on the flat of a 7 cm wide blade has it 3-4 cm off the flat and up to 3.5 cm off the axis
// sideways: ~5 cm at the edge, so 5 cm asked for a fist sunk into the blade; 7 cm (10 off) lets
// the paper you see on the blade count. The window: 0.6 s turned a slow, deliberate 20 cm stroke
// (0.3 m/s) down; nothing but a wipe moves a held paper 20 cm along the blade within 7 cm of it.
// Easier (the tester's 2026-10-08 "make applying the fire paper a little easier (like easier to
// find the spot where we need to rub)"; their playtest: first touches at 5.5-6.9 cm after up to
// 12 s of searching, the strokes finishing 7.6-10 cm off): a wider zone, 12 cm of the blade's
// axis (16 off; the Saw Cleaver's flat is 1-2 cm from it, its edge 3.5 cm), 6 cm past its ends,
// a shorter stroke (30% of the blade, at least 7 cm) within 1.0 s, and a find cue before the
// touch (cue_m: Output::wipe_near). What keeps it from happening by accident is now also WHO
// moves: the paper's own motion along the blade must make at least own_share of the stroke (a
// thrust or a swing of the blade past a paper held still is no wipe: the tester's playtest had a
// 10 cm thrust along the blade at 1.5 m/s with the paper in the hand). The merge's skeptic
// (2026-10-08): a forehand with the paper held by the hilt (6-12 cm beside the fist, 10 cm up the
// blade) and the torso turning with it went through as a 15 cm stroke, 45% of it the paper's own
// (the torso carries the paper hand round too; the first version's 7 cm / 20 cm held): the blade
// may turn at most max_turn_deg over the stroke (a swing turns it 60-120 degrees in a quarter
// second; a wipe holds it). And the long forms (the Saw Cleaver's B 109 cm, the Hunter Axe's B
// haft 160 cm) asked for 25 cm where the tester's own strokes were 20-21 cm: at most 20 now.
// WipeClassic(): the 2026-10-06 values (quick_wipe_rub=0 quick_wipe_reach_cm=7
// quick_wipe_share=0.4 quick_wipe_window_ms=800 quick_wipe_min_cm=8 quick_wipe_max_cm=25
// quick_wipe_own=0 quick_wipe_turn_deg=0 quick_wipe_cue=0); WipeEasier(): the morning's
// 2026-10-08 ones (quick_wipe_rub=0, the rest the defaults).
// The RUB (the defaults since the tester's 2026-10-08 "just make it feel good, I don't want it to
// trigger when I am not rubbing the actual blade -- I want it to trigger when I RUB the blade.
// Right now sometimes it doesn't trigger even when I am doing it right"; quick_wipe_rub=1). Their
// playtest: the paper you see is its CARD, pinched in the fingers 6 cm out of the fist and pushed
// 4.5 cm toward the eye (BuildHeld), 7-8 cm from the fist's centre the zone was measured from, so
// a card laid on the blade left the fist anywhere from 0 to 15 cm off the axis (every touch began
// at 5.5-6.9 cm: just where the 7 cm reach let it, and a 12 cm reach also counted a card held a
// hand's width beside the blade); and short strokes back and forth never added up to the one
// one-way stroke asked for. Now:
//  - the contact is the card on the blade: the card (a rectangle, half_w x half_h, WipeContact
//    `gap`: its nearest point from the blade's axis) within touch_m of the blade's surface (the
//    axis with blade_m round it; off past touch_exit_m), its centre at most end_margin_m past
//    either end;
//  - a rub is the travel along the blade while on it, back and forth: strokes (one-way runs, a
//    turn back past backtrack_m ends one) of at least stroke_m, each along the blade (not a
//    cross-cut: along_ratio), not a slash (stroke_max_mps), the paper's own move (own_share), the
//    blade turning at most max_turn_deg and never moving faster than blade_max_mps at the paper
//    (a swing or a thrust past a still paper), adding up to rub_m within rub_window_s, rubbed at
//    least rub_min_s; or one long stroke of the old size (share of the blade, min_travel_m ..
//    max_travel_m) within rub_window_s, at once.
struct WipeConfig {
    bool rub{true};            // the rub (above); false: the one-way stroke from the fist (reach_m)
    float reach_m{0.12f};      // touching within this of the segment ... (2026-10-06: 0.07)
    float exit_m{0.16f};       // ... until past this (hysteresis) (0.10)
    float end_margin_m{0.06f}; // past either end of the blade along its axis still on it (0.04)
    float share{0.30f};        // a stroke covers this share of the blade ... (0.40)
    float min_travel_m{0.07f}; // ... and at least this (0.08)
    float max_travel_m{0.20f}; // ... and at most this (0.25)
    float window_s{1.00f};     // ... within this (0.80)
    // The rub's own (rub true; reach_m / exit_m / window_s are the one-way stroke's).
    float blade_m{0.04f};       // the blade's half-width round its axis (quick_wipe_blade_cm)
    float touch_m{0.03f};       // the card within this of the blade's surface ... (quick_wipe_touch_cm)
    float touch_exit_m{0.055f}; // ... until past this
    float rub_m{0.18f};         // the travel along it, back and forth, ... (quick_wipe_rub_cm)
    float rub_window_s{1.5f};   // ... within this (quick_wipe_rub_ms)
    float rub_min_s{0.25f};     // ... rubbed at least this long
    float stroke_m{0.04f};      // each stroke at least this (quick_wipe_stroke_cm)
    float stroke_max_mps{2.0f}; // a stroke faster than this on average is a slash
    float blade_max_mps{0.8f};  // the blade's own speed at the paper: past this it moves (quick_wipe_blade_mps)
    float near_log_m{0.08f};    // the near-miss log: the card within this beyond touch_m
    float backtrack_m{0.02f};
    float along_ratio{1.5f};
    float lateral_slack_m{0.02f};
    float min_stroke_s{0.10f};
    float min_hold_s{0.20f};   // the item in the hand at least this long
    float min_length_m{0.10f}; // a blade shorter than this is none
    float tooth_m{0.03f};      // the rasp: a tick every this much along the blade
    float stale_s{0.15f};      // a contact older than this is lost (the buzz stops)
    // The paper's own share of the stroke: its own move along the blade (in your play space) at
    // least this share of the stroke's travel along it (0: not asked; a sample without the
    // paper's place skips it).
    float own_share{0.40f};
    // A swing turns the blade, a wipe holds it: the blade's direction at the stroke's start
    // against the newest sample's at most this (degrees; 0: not asked; a sample without the
    // blade's direction skips it).
    float max_turn_deg{35.0f};
    // The find cue: from this far outside the zone (beyond reach_m) to its edge, wipe_near rises
    // 0 -> 1 (0: no cue).
    float cue_m{0.30f};
};

// The morning's one-way stroke (2026-10-08, before the rub), selectable: quick_wipe_rub=0.
constexpr WipeConfig WipeEasier() {
    WipeConfig c{};
    c.rub = false;
    return c;
}

// The first version's stroke (2026-10-06), selectable through the settings.
constexpr WipeConfig WipeClassic() {
    WipeConfig c{};
    c.rub = false;
    c.reach_m = 0.07f;
    c.exit_m = 0.10f;
    c.end_margin_m = 0.04f;
    c.share = 0.40f;
    c.min_travel_m = 0.08f;
    c.max_travel_m = 0.25f;
    c.window_s = 0.80f;
    c.own_share = 0.0f;
    c.max_turn_deg = 0.0f;
    c.cue_m = 0.0f;
    return c;
}

// Render-camera hook -> presenter (seqlock): the paper against the blade, every rendered frame
// while a wipe item is in the hunter's hand (the hunter's own pose, as you see it: both from his
// model's dummies in the same frame, in LOCAL metres).
struct WipeContact {
    std::uint32_t serial{0}; // + 1 per sample
    std::int64_t steady_ns{0};
    std::int32_t goods{-1}; // measured for this held item
    bool valid{false};      // the blade and the paper were found
    bool arms{false};       // both arms are the player's (the game animates neither)
    float along{0.0f};      // the paper's projection on the blade from its base (m)
    float dist{1e9f};       // the paper from the segment (m)
    float length{0.0f};     // the blade (m)
    std::array<float, 3> off{}; // the paper from its closest point on the segment (LOCAL)
    // The paper itself and the blade's direction (base -> tip, unit), LOCAL: who moved (the
    // stroke's own_share). `placed` false: not known (the rule is skipped).
    bool placed{false};
    std::array<float, 3> paper{};
    std::array<float, 3> dir{};
    // The rub (WipeConfig::rub): the paper as you see it, its card (WipeMeasureCard; `card`
    // false: not measured, the fist's values above stand in): `gap` the card's nearest point from
    // the blade's axis segment (0: the axis runs through the card), its centre's projection along
    // the blade, offset from the axis and place (LOCAL), the blade's base (LOCAL: where the blade
    // itself moves at the paper).
    bool card{false};
    float gap{1e9f};
    float card_along{0.0f};
    std::array<float, 3> card_off{};
    std::array<float, 3> card_p{};
    std::array<float, 3> base{};
    // Curved weapons: actual blade-chain points, never a chord across the curve.
    // The edge/fraction identify a material point for the moving-blade safeguard.
    static constexpr int MaxPathPoints = 40;
    std::array<V3, MaxPathPoints> path{};
    int path_count{0};
    int path_edge{0};
    float path_fraction{0.0f};
};

// The distance between the segments p0 -> p1 and q0 -> q1 (their closest points; Ericson's
// Real-Time Collision Detection 5.1.9).
inline float SegmentDistance(V3 p0, V3 p1, V3 q0, V3 q1) {
    const V3 d1 = p1 - p0;
    const V3 d2 = q1 - q0;
    const V3 r = p0 - q0;
    const float a = Dot(d1, d1);
    const float e = Dot(d2, d2);
    const float f = Dot(d2, r);
    float s = 0.0f;
    float t = 0.0f;
    if (a <= 1e-12f && e <= 1e-12f) {
        return Length(r);
    }
    if (a <= 1e-12f) {
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = Dot(d1, r);
        if (e <= 1e-12f) {
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = Dot(d1, d2);
            const float denom = a * e - b * b;
            s = denom > 1e-12f ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = std::clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = std::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    return Length((p0 + d1 * s) - (q0 + d2 * t));
}

// The distance from the segment p0 -> p1 to the rectangle round `c` (unit axes u, v; half sizes
// hu, hv): 0 when it passes through it; else the nearest of its ends to the rectangle and of it
// to the rectangle's four edges (a convex polygon: one of those holds the nearest pair).
inline float SegmentRectDistance(V3 p0, V3 p1, V3 c, V3 u, V3 v, float hu, float hv) {
    const auto to_rect = [&](V3 p) {
        const V3 d = p - c;
        const float a = std::clamp(Dot(d, u), -hu, hu);
        const float b = std::clamp(Dot(d, v), -hv, hv);
        return Length(p - (c + u * a + v * b));
    };
    const V3 n = Cross(u, v);
    const float d0 = Dot(p0 - c, n);
    const float d1 = Dot(p1 - c, n);
    if ((d0 <= 0.0f && d1 >= 0.0f) || (d0 >= 0.0f && d1 <= 0.0f)) {
        const float s = std::abs(d0 - d1) > 1e-9f ? d0 / (d0 - d1) : 0.0f;
        const V3 x = p0 + (p1 - p0) * s - c;
        if (std::abs(Dot(x, u)) <= hu && std::abs(Dot(x, v)) <= hv) {
            return 0.0f;
        }
    }
    float best = std::min(to_rect(p0), to_rect(p1));
    const std::array<V3, 4> k{c + u * hu + v * hv, c - u * hu + v * hv, c - u * hu - v * hv,
                              c + u * hu - v * hv};
    for (std::size_t i = 0; i < 4; ++i) {
        best = std::min(best, SegmentDistance(p0, p1, k[i], k[(i + 1) % 4]));
    }
    return best;
}

// The paper against the segment base -> tip (LOCAL): its projection along it from the base, its
// distance from the nearest point of it and the offset from there.
inline WipeContact WipeMeasure(V3 paper, V3 base, V3 tip) {
    WipeContact w{};
    const V3 axis = tip - base;
    w.length = Length(axis);
    if (w.length < 1e-4f) {
        return w;
    }
    const V3 dir = axis * (1.0f / w.length);
    w.along = Dot(paper - base, dir);
    const V3 closest = base + dir * std::clamp(w.along, 0.0f, w.length);
    const V3 off = paper - closest;
    w.dist = Length(off);
    w.off = {off.x, off.y, off.z};
    w.placed = true;
    w.paper = {paper.x, paper.y, paper.z};
    w.dir = {dir.x, dir.y, dir.z};
    w.base = {base.x, base.y, base.z};
    w.valid = true;
    return w;
}

// ... and the paper's card (the rub): WipeMeasure's from the fist (`fist`), and the card round
// `centre` (unit axes right / up in its plane, half sizes half_w / half_h) against the segment:
// its nearest point from it (`gap`) and its centre's projection, offset and place.
inline WipeContact WipeMeasureCard(V3 fist, V3 centre, V3 right, V3 up, float half_w,
                                   float half_h, V3 base, V3 tip) {
    WipeContact w = WipeMeasure(fist, base, tip);
    if (!w.valid) {
        return w;
    }
    const WipeContact m = WipeMeasure(centre, base, tip);
    w.card = true;
    w.gap = SegmentRectDistance(base, tip, centre, right, up, half_w, half_h);
    w.card_along = m.along;
    w.card_off = m.off;
    w.card_p = m.paper;
    return w;
}

// Measure the nearest actual chain link. Along-distance is continuous across joints;
// the tangent is local, while length covers the whole chain (no tiny-link wipe threshold).
inline WipeContact WipeMeasurePath(V3 fist, V3 centre, V3 right, V3 up, float half_w,
                                   float half_h, const V3* points, int count) {
    if (!points || count < 2 || count > WipeContact::MaxPathPoints) return {};
    float total = 0.0f, best = 1e9f, best_gap = 1e9f, prefix = 0.0f;
    int edge = -1;
    for (int i = 0; i < count; ++i) {
        if (!std::isfinite(points[i].x) || !std::isfinite(points[i].y) ||
            !std::isfinite(points[i].z)) return {};
        if (i == count - 1) break;
        const auto m = WipeMeasure(centre, points[i], points[i + 1]);
        if (!m.valid) return {}; // collapsed link: material correspondence is undefined
        const float gap = SegmentRectDistance(points[i], points[i + 1], centre, right, up,
                                               half_w, half_h);
        // Contact uses the whole paper. If it spans several links, prefer the one
        // nearest its centre among equally close surfaces to avoid joint jitter.
        if (gap < best_gap - 1e-5f || (std::abs(gap - best_gap) <= 1e-5f && m.dist < best)) {
            best_gap = gap;
            best = m.dist;
            prefix = total;
            edge = i;
        }
        total += m.length;
    }
    if (edge < 0) return {};
    auto w = WipeMeasureCard(fist, centre, right, up, half_w, half_h,
                             points[edge], points[edge + 1]);
    const float link = w.length;
    // Only the outer ends have an end margin. Internal joints share one coordinate.
    const auto along = [&](float u) {
        if (edge > 0) u = std::max(0.0f, u);
        if (edge < count - 2) u = std::min(link, u);
        return prefix + u;
    };
    w.along = along(w.along);
    w.card_along = along(w.card_along);
    w.path_fraction = (w.card_along - prefix) / link;
    w.length = total;
    const V3 dir{w.dir[0], w.dir[1], w.dir[2]};
    const V3 origin = points[edge] - dir * prefix;
    w.base = {origin.x, origin.y, origin.z};
    w.path_count = count;
    w.path_edge = edge;
    std::copy_n(points, count, w.path.begin());
    return w;
}

// The rub's contact (WipeConfig::rub): the card's gap to the blade's surface (the axis less the
// blade's half-width; the fist's distance when no card was measured), and its centre along it.
inline float WipeSurface(const WipeConfig& c, const WipeContact& w) {
    return (w.card ? w.gap : w.dist) - c.blade_m;
}
inline float WipeAlong(const WipeContact& w) {
    return w.card ? w.card_along : w.along;
}

// The find cue (quick_wipe_cue): 0 while the paper is `cue_m` or more outside the zone, rising to
// 1 at its edge (and 1 inside it, not yet touching: past the blade's ends). The distance is from
// the segment (past an end, from that end), less the reach (the rub: the card's gap to the
// blade's surface less touch_m).
inline float WipeNear(const WipeConfig& c, const WipeContact& w) {
    if (!w.valid || c.cue_m <= 0.0f || w.length < c.min_length_m) {
        return 0.0f;
    }
    const float outside =
        c.rub ? std::max(0.0f, WipeSurface(c, w) - c.touch_m) : std::max(0.0f, w.dist - c.reach_m);
    return std::clamp(1.0f - outside / c.cue_m, 0.0f, 1.0f);
}

// The paper within the zone's entry distance (the glow at its brightest; WipeDetector's own
// contact also asks the blade's ends and hysteresis).
inline bool WipeInZone(const WipeConfig& c, const WipeContact& w) {
    return w.valid && (c.rub ? WipeSurface(c, w) <= c.touch_m : w.dist <= c.reach_m);
}

// Why a stroke of the rub was not counted (WipeDetector::LastWhy; the near-miss log).
enum class RubWhy : std::uint8_t {
    None,
    Across,      // across the blade more than along it (a cut)
    Slash,       // faster than stroke_max_mps
    BladeMoving, // the blade moved faster than blade_max_mps at the paper (a swing, a thrust)
    BladeTurned, // the blade turned past max_turn_deg over it (a swing)
    NotOwn,      // the paper's own move under own_share of it (the blade slid past it)
};
inline const char* RubWhyName(RubWhy w) {
    switch (w) {
    case RubWhy::Across:
        return "across the blade, not along it";
    case RubWhy::Slash:
        return "too fast: a slash";
    case RubWhy::BladeMoving:
        return "the blade moving";
    case RubWhy::BladeTurned:
        return "the blade turning";
    case RubWhy::NotOwn:
        return "the blade moved, not the paper";
    default:
        return "counted";
    }
}

// Carry a reference direction by the rotation of the SAME material link between
// poses. Moving the paper to a differently oriented link is not weapon rotation.
inline V3 WipeTransportDirection(V3 reference, V3 from, V3 to) {
    const float cosine = std::clamp(Dot(from, to), -1.0f, 1.0f);
    if (cosine < -0.9999f) {
        const V3 axis = Normalize(Cross(from, std::abs(from.y) < .9f ? V3{0,1,0} : V3{1,0,0}));
        return axis * (2.0f * Dot(axis, reference)) - reference;
    }
    const V3 cross = Cross(from, to);
    return Normalize(reference + Cross(cross, reference) +
                     Cross(cross, Cross(cross, reference)) * (1.0f / (1.0f + cosine)));
}

// The paper's stroke along the blade, one sample per rendered frame (WipeContact). True on the
// sample whose stroke completes (Consume() it once used): the item is applied.
class WipeDetector {
public:
    void Reset(double held_since) {
        *this = {};
        held_since_ = held_since;
    }
    // One sample (WipeMeasure's / WipeMeasureCard's, with the paper's own place for own_share).
    bool Update(const WipeConfig& c, double t, const WipeContact& w, bool arms) {
        if (c.rub) {
            const auto v = [](const std::array<float, 3>& a) { return V3{a[0], a[1], a[2]}; };
            float material_speed = -1.0f;
            V3 turn_dir = v(w.dir);
            float stroke_along = WipeAlong(w);
            if (w.path_count != prev_path_count_) {
                End();
                Consume(); // never carry progress from a rigid blade to a chain or back
            }
            if (w.path_count >= 2 && w.path_count <= WipeContact::MaxPathPoints &&
                w.path_edge >= 0 && w.path_edge < w.path_count - 1) {
                if (have_prev_ && t > prev_t_ && prev_path_edge_ < w.path_count - 1) {
                    const V3 edge = w.path[prev_path_edge_ + 1] - w.path[prev_path_edge_];
                    const V3 now = w.path[prev_path_edge_] + edge * prev_path_fraction_;
                    const V3 motion = now - prev_path_point_;
                    material_speed = Length(motion) / static_cast<float>(t - prev_t_);
                    // Measure from the old contact material point in THIS pose.
                    // Upstream payout/retraction changes arc length, not paper travel.
                    float old_along_now = Length(edge) * prev_path_fraction_;
                    for (int i = 0; i < prev_path_edge_; ++i)
                        old_along_now += Length(w.path[i + 1] - w.path[i]);
                    const float travel = WipeAlong(w) - old_along_now;
                    const float relative_move = Length(v(w.card ? w.card_p : w.paper) - prev_p_ - motion);
                    if (std::abs(travel) > 2.0f * relative_move + .01f) {
                        // A folded chain can switch the nearest contact to a distant
                        // link. That jump is not a stroke along the intervening chain.
                        End();
                        Consume();
                    } else {
                        path_travel_ += travel;
                        path_turn_dir_ = WipeTransportDirection(path_turn_dir_, prev_path_tangent_,
                                                               Normalize(edge));
                    }
                }
                if (!have_prev_) {
                    path_travel_ = WipeAlong(w);
                    path_turn_dir_ = v(w.dir);
                }
                stroke_along = path_travel_;
                turn_dir = path_turn_dir_;
                prev_path_edge_ = w.path_edge;
                prev_path_fraction_ = w.path_fraction;
                prev_path_point_ = w.path[w.path_edge] * (1.0f - w.path_fraction) +
                                   w.path[w.path_edge + 1] * w.path_fraction;
                prev_path_tangent_ = v(w.dir);
            }
            prev_path_count_ = w.path_count;
            return StepRub(c, t, w.valid, arms, WipeAlong(w), w.card ? w.gap : w.dist, w.length,
                           v(w.card ? w.card_off : w.off), w.placed,
                           v(w.card ? w.card_p : w.paper), v(w.dir), v(w.base), material_speed,
                           turn_dir, stroke_along);
        }
        return Step(c, t, w.valid, arms, w.along, w.dist, w.length, {w.off[0], w.off[1], w.off[2]},
                    w.placed, {w.paper[0], w.paper[1], w.paper[2]}, {w.dir[0], w.dir[1], w.dir[2]});
    }
    // One sample without the paper's own place (own_share not asked).
    bool Update(const WipeConfig& c, double t, bool valid, bool arms, float along, float dist,
                float length, V3 off) {
        if (c.rub) {
            return StepRub(c, t, valid, arms, along, dist, length, off, false, {}, {}, {});
        }
        return Step(c, t, valid, arms, along, dist, length, off, false, {}, {});
    }
    // Off the blade (or the contact lost): the stroke is over (the rub: the strokes made stay
    // counted for their window; a touch again within it goes on adding).
    void End() {
        if (touching_ && rub_.live) {
            CloseStroke(rub_cfg_);
        }
        rub_ = {};
        have_prev_ = false;
        touching_ = false;
        count_ = 0;
        progress_ = 0.0f;
        travel_ = 0.0f;
        stroke_s_ = 0.0;
        own_ = 0.0f;
    }
    // Applied (or refused) here: the next stroke starts from scratch.
    void Consume() {
        count_ = 0;
        progress_ = 0.0f;
        rub_ = {};
        have_prev_ = false;
        done_n_ = 0;
        total_ = 0.0f;
    }
    // The rasp's teeth since the last call.
    int TakeTeeth() {
        const int n = teeth_;
        teeth_ = 0;
        return n;
    }
    [[nodiscard]] bool Touching() const {
        return touching_;
    }
    [[nodiscard]] float Progress() const {
        return progress_;
    }
    [[nodiscard]] float Travel() const {
        return travel_;
    }
    [[nodiscard]] float Required() const {
        return required_;
    }
    [[nodiscard]] double StrokeS() const {
        return stroke_s_;
    }
    [[nodiscard]] float Distance() const {
        return dist_;
    }
    [[nodiscard]] float BladeLength() const {
        return length_;
    }
    [[nodiscard]] double HeldSince() const {
        return held_since_;
    }
    // The paper's own share of the best stroke's travel (1: all its own; -1: not known).
    [[nodiscard]] float OwnShare() const {
        return own_;
    }
    // Strokes turned down because the blade, not the paper, moved along (own_share) or the blade
    // turned (max_turn_deg) since the take (a thrust or a swing past the paper; the log says so
    // once).
    [[nodiscard]] int BladeMoves() const {
        return blade_moves_;
    }
    // The rub: the travel along the blade counted within the window (back and forth), the strokes
    // it is made of, since when, and how it completed (0 not, 1 one long stroke, 2 the rub).
    [[nodiscard]] float RubTotal() const {
        return total_;
    }
    [[nodiscard]] int RubStrokes() const {
        return strokes_n_;
    }
    [[nodiscard]] double RubSpanS() const {
        return span_s_;
    }
    [[nodiscard]] int DoneKind() const {
        return done_kind_;
    }
    // The strokes of 'stroke_m' or more not counted since the take, the newest's why, length and
    // measure (RubWhy: Across the cm across, Slash and BladeMoving m/s, BladeTurned degrees,
    // NotOwn the paper's share) and the touch's own start.
    [[nodiscard]] int Refused() const {
        return refused_;
    }
    [[nodiscard]] RubWhy LastWhy() const {
        return last_why_;
    }
    [[nodiscard]] float LastLength() const {
        return last_len_;
    }
    [[nodiscard]] float LastValue() const {
        return last_value_;
    }
    [[nodiscard]] double TouchSince() const {
        return touch_t_;
    }

private:
    // ---- The rub (WipeConfig::rub) ----
    struct RubStroke {
        bool live{false};
        int way{0}; // +1 toward the tip, -1 toward the base, 0 not yet known
        float u0{0.0f};
        float u_ext{0.0f};
        double t0{0.0};
        double t_ext{0.0};
        V3 off0{};
        V3 off_ext{};
        V3 dir0{0.0f, 0.0f, -1.0f};
        V3 dir_ext{0.0f, 0.0f, -1.0f};
        V3 turn0{0.0f, 0.0f, -1.0f};
        V3 turn_ext{0.0f, 0.0f, -1.0f};
        bool placed{true};
        float own{0.0f};     // the paper's own move along the blade, the stroke's way
        float own_ext{0.0f}; // ... at its furthest point
        float own_back{0.0f};   // ... since then, the other way
        float own_signed{0.0f}; // ... before its way was known
    };
    struct RubDone {
        double t0{0.0};
        double t1{0.0};
        float length{0.0f};
    };

    // A stroke judged (its length into `length`): counted (None) or why not.
    static RubWhy Judge(const WipeConfig& c, const RubStroke& s, float& length, float& value) {
        length = std::abs(s.u_ext - s.u0);
        value = 0.0f;
        // (The slack a quarter of a short stroke: a 5 cm scrub at 45 degrees is across.) Across is
        // the move off the blade's line only: past an end (the end margin) the card's offset from
        // its nearest point there also runs along the blade (the merge skeptic's find, 2026-10-08:
        // a 5 cm rub at the guard or the tip with the card's centre past the base or the tip was
        // refused as "across").
        V3 moved = s.off_ext - s.off0;
        const V3 line = Normalize(s.dir0 + s.dir_ext, s.dir_ext);
        moved = moved - line * Dot(moved, line);
        const float across = Length(moved);
        const float slack = std::min(c.lateral_slack_m, 0.25f * length);
        if (length < c.along_ratio * std::max(0.0f, across - slack)) {
            value = across;
            return RubWhy::Across;
        }
        const double dur = s.t_ext - s.t0;
        if (length >= c.stroke_m && (dur <= 1e-6 || length / dur > c.stroke_max_mps)) {
            value = dur > 1e-6 ? static_cast<float>(length / dur) : 99.0f;
            return RubWhy::Slash;
        }
        if (s.placed && c.max_turn_deg > 0.0f &&
            Dot(s.turn0, s.turn_ext) < std::cos(c.max_turn_deg * 0.017453293f)) {
            value = std::acos(std::clamp(Dot(s.turn0, s.turn_ext), -1.0f, 1.0f)) * 57.29578f;
            return RubWhy::BladeTurned;
        }
        if (s.placed && c.own_share > 0.0f && length > 1e-4f && s.own_ext < c.own_share * length) {
            value = std::max(0.0f, s.own_ext) / length;
            return RubWhy::NotOwn;
        }
        return RubWhy::None;
    }

    // The stroke under way is over (a turn back, the blade moving, the contact lost): counted or
    // remembered as refused (the near-miss log).
    void CloseStroke(const WipeConfig& c) {
        float length = 0.0f;
        float value = 0.0f;
        const RubWhy why = rub_.way != 0 ? Judge(c, rub_, length, value) : RubWhy::None;
        if (rub_.way == 0 || length < c.stroke_m) {
            return; // a jitter, not a stroke
        }
        if (why != RubWhy::None) {
            ++refused_;
            last_why_ = why;
            last_len_ = length;
            last_value_ = value;
            if (why == RubWhy::BladeMoving || why == RubWhy::BladeTurned || why == RubWhy::NotOwn) {
                ++blade_moves_;
            }
            return;
        }
        if (done_n_ == done_.size()) {
            std::copy(done_.begin() + 1, done_.end(), done_.begin());
            --done_n_;
        }
        done_[done_n_++] = {rub_.t0, rub_.t_ext, length};
    }

    // A new stroke from this sample.
    void StartStroke(double t, float u, V3 off, V3 dir, bool placed, V3 turn_dir) {
        rub_ = {};
        rub_.live = true;
        rub_.u0 = rub_.u_ext = u;
        rub_.t0 = rub_.t_ext = t;
        rub_.off0 = rub_.off_ext = off;
        rub_.dir0 = rub_.dir_ext = dir;
        rub_.turn0 = rub_.turn_ext = turn_dir;
        rub_.placed = placed;
    }

    bool StepRub(const WipeConfig& c, double t, bool valid, bool arms, float along, float gap,
                 float length, V3 off, bool placed, V3 paper, V3 dir_in, V3 base,
                 float material_speed = -1.0f, V3 turn_dir = {}, float stroke_along = NAN) {
        rub_cfg_ = c;
        const float surf = gap - c.blade_m;
        dist_ = surf;
        length_ = length;
        along_ = along;
        const float limit = touching_ ? c.touch_exit_m : c.touch_m;
        const bool inside = valid && arms && length >= c.min_length_m && surf <= limit &&
                            along >= -c.end_margin_m && along <= length + c.end_margin_m;
        if (!inside) {
            End();
            Totals(c, t);
            return false;
        }
        const V3 dir = Normalize(dir_in, {0.0f, 0.0f, -1.0f});
        turn_dir = Normalize(turn_dir, dir);
        if (std::isfinite(stroke_along)) along = stroke_along;
        if (!touching_) {
            touching_ = true;
            touch_t_ = t;
            tooth_u_ = along;
            have_prev_ = false;
            fast_len_ = 0.0f;
            fast_v_ = 0.0f;
        }
        // The rasp: one tooth per tooth_m along the blade, either way.
        while (std::abs(along - tooth_u_) >= c.tooth_m) {
            tooth_u_ += along > tooth_u_ ? c.tooth_m : -c.tooth_m;
            ++teeth_;
        }
        if (!have_prev_ || t - prev_t_ <= 1e-6) {
            if (!have_prev_) {
                StartStroke(t, along, off, dir, placed, turn_dir);
            }
            Remember(t, along, placed, paper, dir, base);
            Totals(c, t);
            return false;
        }
        const double dt = t - prev_t_;
        // The paper's own move along the blade (in your play space) and the blade's own speed
        // where the paper is on it (its point under the paper then, now). Without the paper's
        // place (a test's bare sample) all of the move is its own.
        const bool both = placed && prev_placed_;
        const float own = both ? Dot(paper - prev_p_, dir) : along - prev_u_;
        float blade_v = 0.0f;
        if (both) {
            const V3 was = prev_base_ + prev_dir_ * prev_u_;
            const V3 now = base + dir * prev_u_;
            blade_v = Length(now - was) / static_cast<float>(dt);
        }
        // On a curved blade, changing tangent as the paper crosses a joint is not
        // blade motion. Compare the SAME material point between poses instead.
        if (material_speed >= 0.0f) blade_v = material_speed;
        if (blade_v > c.blade_max_mps) {
            // The blade moves (a swing, a thrust): the stroke up to the last still sample is
            // over, the paper's place along the blade meanwhile counts for nothing, and a new
            // stroke starts once it is still again. A stroke's worth slid by like that is logged.
            CloseStroke(c);
            fast_len_ += std::abs(along - prev_u_);
            fast_v_ = std::max(fast_v_, blade_v);
            if (fast_len_ >= c.stroke_m) {
                ++refused_;
                ++blade_moves_;
                last_why_ = RubWhy::BladeMoving;
                last_len_ = fast_len_;
                last_value_ = fast_v_;
                fast_len_ = 0.0f;
                fast_v_ = 0.0f;
            }
            StartStroke(t, along, off, dir, placed, turn_dir);
            Remember(t, along, placed, paper, dir, base);
            return Totals(c, t);
        }
        RubStroke& s = rub_;
        s.placed = s.placed && placed;
        if (s.way == 0) {
            s.own_signed += own;
            if (std::abs(along - s.u0) >= c.backtrack_m) {
                s.way = along > s.u0 ? 1 : -1;
                s.own = s.own_signed * static_cast<float>(s.way);
                s.own_ext = s.own;
                s.u_ext = along;
                s.t_ext = t;
                s.off_ext = off;
                s.dir_ext = dir;
                s.turn_ext = turn_dir;
            }
        } else {
            const float w = static_cast<float>(s.way);
            s.own += own * w;
            if ((along - s.u_ext) * w >= 0.0f) {
                s.u_ext = along;
                s.t_ext = t;
                s.off_ext = off;
                s.dir_ext = dir;
                s.turn_ext = turn_dir;
                s.own_ext = s.own;
                s.own_back = 0.0f;
            } else {
                s.own_back -= own * w;
                if ((s.u_ext - along) * w > c.backtrack_m) {
                    // Turned back: this stroke is over, the next one runs from its far end.
                    CloseStroke(c);
                    const RubStroke was = s;
                    StartStroke(was.t_ext, was.u_ext, was.off_ext, was.dir_ext, placed, was.turn_ext);
                    s.way = -was.way;
                    s.u_ext = along;
                    s.t_ext = t;
                    s.off_ext = off;
                    s.dir_ext = dir;
                    s.turn_ext = turn_dir;
                    s.own = s.own_ext = was.own_back;
                }
            }
        }
        Remember(t, along, placed, paper, dir, base);
        return Totals(c, t);
    }

    void Remember(double t, float u, bool placed, V3 p, V3 dir, V3 base) {
        have_prev_ = true;
        prev_t_ = t;
        prev_u_ = u;
        prev_placed_ = placed;
        prev_p_ = p;
        prev_dir_ = dir;
        prev_base_ = base;
    }

    // The rub's totals now (the counted strokes within the window and the one under way) and
    // whether it is done.
    bool Totals(const WipeConfig& c, double t) {
        required_ = std::max(std::min(c.share * length_, c.max_travel_m), c.min_travel_m);
        float total = 0.0f;
        int n = 0;
        double first = t;
        for (std::size_t i = 0; i < done_n_; ++i) {
            if (t - done_[i].t1 <= c.rub_window_s + 1e-6) {
                total += done_[i].length;
                first = std::min(first, done_[i].t0);
                ++n;
            }
        }
        float now_len = 0.0f;
        float now_value = 0.0f;
        bool single = false;
        if (touching_ && rub_.live && rub_.way != 0 &&
            Judge(c, rub_, now_len, now_value) != RubWhy::None) {
            now_len = 0.0f; // not (yet) a stroke that counts
        }
        if (touching_ && rub_.live && rub_.way != 0 && now_len >= c.stroke_m) {
            total += now_len;
            first = std::min(first, rub_.t0);
            ++n;
            const double dur = rub_.t_ext - rub_.t0;
            single = now_len >= required_ - 1e-5f && dur >= c.min_stroke_s - 1e-6 &&
                     dur <= c.rub_window_s + 1e-6;
            travel_ = now_len;
            stroke_s_ = dur;
            own_ = rub_.placed && now_len > 1e-4f ? std::max(0.0f, rub_.own_ext) / now_len : -1.0f;
        } else {
            now_len = 0.0f;
            travel_ = 0.0f;
            stroke_s_ = 0.0;
        }
        total_ = total;
        strokes_n_ = n;
        span_s_ = n > 0 ? t - first : 0.0;
        const bool rub = total >= c.rub_m - 1e-5f && span_s_ >= c.rub_min_s - 1e-6;
        progress_ = touching_ ? std::clamp(std::max(now_len / required_, total / c.rub_m), 0.0f, 1.0f)
                              : 0.0f;
        const bool held = t - held_since_ >= c.min_hold_s - 1e-6;
        done_kind_ = !touching_ || !held ? 0 : single ? 1 : rub ? 2 : 0;
        return done_kind_ != 0;
    }

    int prev_path_count_{0};
    int prev_path_edge_{0};
    float prev_path_fraction_{0.0f};
    V3 prev_path_point_{};
    V3 prev_path_tangent_{};
    V3 path_turn_dir_{};
    float path_travel_{0.0f};
    RubStroke rub_{};
    std::array<RubDone, 16> done_{};
    std::size_t done_n_{0};
    WipeConfig rub_cfg_{};
    bool have_prev_{false};
    double prev_t_{0.0};
    float prev_u_{0.0f};
    bool prev_placed_{false};
    V3 prev_p_{};
    V3 prev_dir_{0.0f, 0.0f, -1.0f};
    V3 prev_base_{};
    float total_{0.0f};
    int strokes_n_{0};
    double span_s_{0.0};
    int done_kind_{0};
    int refused_{0};
    RubWhy last_why_{RubWhy::None};
    float last_len_{0.0f};
    float last_value_{0.0f};
    double touch_t_{0.0};
    float along_{0.0f};
    float fast_len_{0.0f}; // slid along the blade while it moved (this touch)
    float fast_v_{0.0f};

public:
    // The paper's place along the blade, the newest sample's (m from its base).
    [[nodiscard]] float Along() const {
        return along_;
    }

private:
    bool Step(const WipeConfig& c, double t, bool valid, bool arms, float along, float dist,
              float length, V3 off, bool placed, V3 paper, V3 dir) {
        dist_ = dist;
        length_ = length;
        along_ = along;
        const float reach = touching_ ? c.exit_m : c.reach_m;
        const bool inside = valid && arms && length >= c.min_length_m && dist <= reach &&
                            along >= -c.end_margin_m && along <= length + c.end_margin_m;
        if (!inside) {
            End();
            return false;
        }
        if (!touching_) {
            touching_ = true;
            tooth_u_ = along;
            count_ = 0;
        }
        // The rasp: one tooth per tooth_m along the blade.
        while (std::abs(along - tooth_u_) >= c.tooth_m) {
            tooth_u_ += along > tooth_u_ ? c.tooth_m : -c.tooth_m;
            ++teeth_;
        }
        // The samples of this touch within the window (the newest last).
        if (count_ == samples_.size()) {
            std::copy(samples_.begin() + 1, samples_.end(), samples_.begin());
            --count_;
        }
        samples_[count_++] = {t, along, off, paper, placed, Normalize(dir, {0.0f, 0.0f, -1.0f})};
        std::size_t drop = 0;
        while (drop + 1 < count_ && t - samples_[drop].t > c.window_s + 1e-6) {
            ++drop;
        }
        if (drop > 0) {
            std::copy(samples_.begin() + static_cast<std::ptrdiff_t>(drop),
                      samples_.begin() + static_cast<std::ptrdiff_t>(count_), samples_.begin());
            count_ -= drop;
        }
        // The longest one-way run ending now: back from the newest sample while every sample
        // between stays within backtrack_m of the run's span, the blade's way dominating.
        required_ = std::max(std::min(c.share * length, c.max_travel_m), c.min_travel_m);
        const Sample& now = samples_[count_ - 1];
        const V3 axis = now.dir;
        // max_turn_deg as a cosine (the turn rule asked only when both samples know the blade).
        const float turn_cos =
            c.max_turn_deg > 0.0f ? std::cos(c.max_turn_deg * 0.017453293f) : -2.0f;
        float best = 0.0f;
        double best_s = 0.0;
        float best_own = -1.0f;
        bool blade_moved = false;
        float lo = now.u;
        float hi = now.u;
        for (std::size_t k = count_ - 1; k-- > 0;) {
            const Sample& s = samples_[k];
            lo = std::min(lo, s.u);
            hi = std::max(hi, s.u);
            const bool up = now.u >= s.u;
            const bool monotone = up ? hi <= now.u + c.backtrack_m && lo >= s.u - c.backtrack_m
                                     : lo >= now.u - c.backtrack_m && hi <= s.u + c.backtrack_m;
            if (!monotone) {
                break; // an older start would hold this turn too
            }
            const float travel = std::abs(now.u - s.u);
            const float lateral = Length(now.off - s.off);
            if (travel < c.along_ratio * std::max(0.0f, lateral - c.lateral_slack_m)) {
                continue; // across the blade more than along it (a cross-cut)
            }
            // Who moved: the paper's own move along the blade (in your play space), the same
            // way as the stroke. A thrust or a swing of the blade past a paper held still moves
            // the paper along the blade just as much, but not the paper.
            // A swing: the blade turned past max_turn_deg since this start (the paper's place
            // along it moved with the turn, and the torso carries the paper hand round too).
            if (now.placed && s.placed && Dot(now.dir, s.dir) < turn_cos) {
                blade_moved |= travel >= required_ - 1e-5f;
                continue;
            }
            float own = -1.0f;
            if (now.placed && s.placed && travel > 1e-4f) {
                const float moved = Dot(now.p - s.p, axis) * (up ? 1.0f : -1.0f);
                own = std::max(0.0f, moved) / travel;
                if (c.own_share > 0.0f && own < c.own_share) {
                    blade_moved |= travel >= required_ - 1e-5f;
                    continue;
                }
            }
            if (travel > best) {
                best = travel;
                best_s = now.t - s.t;
                best_own = own;
            }
        }
        travel_ = best;
        stroke_s_ = best_s;
        own_ = best_own;
        progress_ = std::clamp(best / required_, 0.0f, 1.0f);
        const bool done = best >= required_ - 1e-5f && best_s >= c.min_stroke_s - 1e-6 &&
                          t - held_since_ >= c.min_hold_s - 1e-6;
        if (blade_moved && !done) {
            ++blade_moves_;
        }
        return done;
    }

    struct Sample {
        double t{0.0};
        float u{0.0f};
        V3 off{};
        V3 p{};          // the paper (LOCAL)
        bool placed{false};
        V3 dir{0.0f, 0.0f, -1.0f}; // the blade's direction (unit; known when placed)
    };
    // The window's samples: quick_wipe_window_ms goes to 2 s, at the game's 60 rendered frames a
    // second (bb-vr's 60 FPS patch; 48 held 0.8 s there and quietly cut a longer window short).
    // The walk back over them is a few ns each: < 1 us a sample.
    std::array<Sample, 128> samples_{};
    std::size_t count_{0};
    double held_since_{0.0};
    bool touching_{false};
    float tooth_u_{0.0f};
    int teeth_{0};
    float progress_{0.0f};
    float travel_{0.0f};
    float required_{0.0f};
    double stroke_s_{0.0};
    float dist_{1e9f};
    float length_{0.0f};
    float own_{0.0f};
    int blade_moves_{0};
};

// ---- Hold to use (quick_hold_use) ------------------------------------------------------------------

// The misc items used by holding them (the tester's 2026-10-06 request: "grab them with trigger and
// just be activated like 2s after holding them ... it allows dropping an item and cancelling it
// being used by mistake within that grace period"): taken from the arc they stay in the hunter's
// hand while the button that took them is held (left_scheme=2: the left trigger; 1: the grip), a
// ring fills round the item over quick_hold_use_s and the controller's vibration rises with it;
// full, the item is used (a flash on the item, a solid thump); let go of before, it goes back,
// nothing used. Which: the ones whose use cannot be taken back (consumed, or an act with lasting
// consequences) that are neither eaten / drunk (quick_use_mouth), rubbed on the blade (the paper
// wipe takes its own first), thrown, nor the Blood Vial. From the 1.09 goods params
// (goodsUseAnim +0x3E and the consume flag; the runtime logs the cross-check at start):
//  - Consume (UseGoods at once, no animation; Square where the game latched it): the coldbloods
//    (anim 9: the blood echoes), Madman's Knowledge and Great One's Wisdom (9: insight), the Blood
//    Dreg (9), the NPC bloods and Iosefka's Blood Vial (28: one of each), the Umbilical Cords (35),
//    Vermin (33), and the papers, the Empty Phantasm Shell and the Bone Marrow Ash (1, 5) where no
//    wipe takes them.
//  - Rite (the game's own act: select + Square, its animation): the Hunter's Marks (16: back to
//    the lamp, the echoes left behind), the bells (14, 15: Beckoning, Small Resonant, Sinister,
//    the Silencing Blank), the Tiny Music Box (27) and the Shaman Bone Blade (25: its stab, the
//    user's 2026-10-06 "make it switch to 3rd person/flat view, use it, and then back in VR": a
//    stray pull never wastes it; its use is shown flat, CannedOf below).
// Everything else keeps its use at the pull: the other hunter tools (combat and quicksilver: the
// Augur, A Call Beyond, ...), the Monocular, the Hand Lantern and the Notebook (nothing spent),
// throwables, the mouth's items, the Blood Vial.
enum class HoldKind : std::uint8_t { None, Consume, Rite };
constexpr HoldKind HoldOf(std::int32_t goods) {
    switch (goods) {
    case 100:  // Hunter's Mark
    case 1400: // Bold Hunter's Mark
    case 111:  // Silencing Blank
    case 200:  // Beckoning Bell
    case 205:  // Small Resonant Bell
    case 225:  // Sinister Resonant Bell
    case 700:  // Tiny Music Box
    case 2090: // Shaman Bone Blade (a canned use: shown flat, CannedOf)
        return HoldKind::Rite;
    case 705:  // Queenly Blood
    case 704:  // Blood Dreg
    case 1500: // Madman's Knowledge
    case 1501: // Great One's Wisdom
    case 1300: // Fire Paper (the paper wipe's where it is on)
    case 1310: // Empty Phantasm Shell
    case 1320: // Bolt Paper
    case 1330: // Bone Marrow Ash
    case 1700: // Vermin
    case 4320: // One Third of Umbilical Cord (x4)
    case 4321:
    case 4322:
    case 4323:
        return HoldKind::Consume;
    default:
        break;
    }
    if ((goods >= 1510 && goods <= 1519) || (goods >= 1590 && goods <= 1594)) {
        return HoldKind::Consume; // the coldbloods
    }
    return HoldKind::None;
}
constexpr const char* HoldName(HoldKind k) {
    return k == HoldKind::Consume ? "consume" : k == HoldKind::Rite ? "rite" : "none";
}
// The params' goodsUseAnim agrees with the table (the start's cross-check).
constexpr bool HoldAnimAgrees(HoldKind k, std::int32_t use_anim) {
    switch (k) {
    case HoldKind::Rite:
        return use_anim == 14 || use_anim == 15 || use_anim == 16 || use_anim == 25 ||
               use_anim == 27;
    case HoldKind::Consume:
        return use_anim == 1 || use_anim == 5 || use_anim == 9 || use_anim == 28 ||
               use_anim == 33 || use_anim == 35;
    default:
        return true;
    }
}

// ---- Canned item uses: shown flat (cinematic_item) ------------------------------------------------

// The items whose game use is a full-body attack that only lands as the game plays it: the Shaman
// Bone Blade (goodsUseAnim 25, the behaviour's UseItem_25: a000_100370, a stab at the enemy in
// front; its effect, bullet 310, lands only if the stab hits). Their use is never ours (no
// UseGoods at once: the game's own select + Square, its animation), and the tester's 2026-10-06
// "make it switch to 3rd person/flat view, use it, and then back in VR" shows that animation the
// way a visceral is shown: our Square for one arms the cinematic (VR::Quick::NoteCannedUse), whose
// Category::Item goes flat while the game's item-use state plays it (core/vr/cinematic_state.h
// ItemUseGate) and returns at the game's cancel window. Everything else keeps its own handling (the
// marks, the bells, the music box: in VR).
constexpr bool CannedOf(std::int32_t goods) {
    return goods == 2090; // Shaman Bone Blade
}
// Its use clip (a00 TAE id: the behaviour's UseItem_<goodsUseAnim>), -1 for one not canned.
constexpr std::int32_t CannedClipOf(std::int32_t goods) {
    return goods == 2090 ? 100370 : -1;
}
// The params' goodsUseAnim agrees with the table (the start's cross-check).
constexpr bool CannedAnimAgrees(std::int32_t goods, std::int32_t use_anim) {
    return !CannedOf(goods) || (goods == 2090 && use_anim == 25);
}
// Every goods of 1.09 a quick slot can hold and use (the goods dump: type 0, a use), for the
// start's classification log and the tests (130, 131: the Vileblood Register and the League
// Staff, the covenants' own: used at the pull).
inline constexpr std::array<std::int32_t, 73> QuickUsableGoods{
    100,  111,  120,  130,  131,  200,  205,  225,  700,  701,  702,  703,  704,  705,  706,  1000,
    1100, 1101, 1110, 1120, 1200, 1201, 1210, 1220, 1230, 1240, 1250, 1251, 1260, 1270, 1300, 1310,
    1320, 1330, 1400, 1410, 1411, 1412, 1500, 1501, 1510, 1511, 1512, 1513, 1514, 1515, 1516, 1517,
    1518, 1519, 1590, 1591, 1592, 1593, 1594, 1700, 2000, 2010, 2020, 2030, 2050, 2060, 2070, 2080,
    2090, 2110, 2120, 2130, 2140, 4320, 4321, 4322, 4323};

// The vibration rising while the ring fills (Output::hold_hum: the left hand's continuous hum,
// core/vr/haptic_output.h): a gentle 0.22 at the take (a hum is not raised to haptic_floor as a
// tick is: lower would hardly be felt on Touch controllers) to a firm 0.62 at full, eased in so
// most of the rise comes near the end ("nearly there"); the thump after it is 0.95. The runtime
// is called when it changed by 0.05 (or 15%) or every 0.1 s: about fifteen calls over the fill.
inline float HoldHum(float progress) {
    const float p = std::clamp(progress, 0.0f, 1.0f);
    return 0.22f + 0.40f * p * p;
}

// ---- Gestures: crushed, rung, stabbed (quick_use_crush / quick_use_shake / quick_use_stab) ------

// The tester's 2026-10-06 "yes and yes": natural gestures as the faster
// way to use some held items, the 1.5 s hold staying their fallback (a gesture finishes through
// the same completion as the ring: the game's UseGoods at once, or its own animation):
//  - CRUSH: what the game's own clip crushes in the fist (goodsUseAnim 9: the coldbloods,
//    Madman's Knowledge, Great One's Wisdom, the Blood Dreg; 33: Vermin) is crushed by squeezing
//    the hand that holds it hard (the other button: left_scheme=2 the grip, 1 the trigger).
//  - SHAKE: the bells (14: Beckoning, Small Resonant, Sinister Resonant; 15: the Silencing Blank;
//    opt-in 18: the Choir Bell, a hunter tool otherwise used at the pull) ring with three
//    back-and-forth strokes of the hand.
// Bottled NPC blood / Iosefka's Blood Vial are drunk; Queenly Blood is squeeze-only.
// The regular Blood Vial keeps its separate injection path. The legacy quick-stab
// implementation has no mapped goods.
// The Hunter's Marks, the Tiny Music Box, the Bone Marrow Ash and the Umbilical Cords keep the
// plain hold (the cords on purpose: a once-a-playthrough act that changes the ending).
enum class GestureKind : std::uint8_t { None, Crush, Shake };
inline constexpr std::int32_t ChoirBell = 2050;
constexpr bool IsBellItem(std::int32_t goods) {
    return goods == 200 || goods == 205 || goods == 225 || goods == ChoirBell;
}
constexpr GestureKind GestureOf(std::int32_t goods) {
    switch (goods) {
    case QueenlyBlood: // squeezed; its native injection animation is bypassed
    case 704:  // Blood Dreg
    case 1500: // Madman's Knowledge
    case 1501: // Great One's Wisdom
    case 1700: // Vermin (the game's own crush clip: its League SpEffects)
        return GestureKind::Crush;
    case 111:  // Silencing Blank
    case 200:  // Beckoning Bell
    case 205:  // Small Resonant Bell
    case 225:  // Sinister Resonant Bell
    case 2050: // Choir Bell (Config::shake_choir)
        return GestureKind::Shake;
    default:
        break;
    }
    if ((goods >= 1510 && goods <= 1519) || (goods >= 1590 && goods <= 1594)) {
        return GestureKind::Crush; // the coldbloods
    }
    return GestureKind::None;
}
constexpr const char* GestureName(GestureKind k) {
    return k == GestureKind::Crush ? "crush" : k == GestureKind::Shake ? "shake" : "none";
}
// The params' goodsUseAnim agrees with the table (the start's cross-check).
constexpr bool GestureAnimAgrees(GestureKind k, std::int32_t use_anim, std::int32_t goods = -1) {
    if (goods == QueenlyBlood && k == GestureKind::Crush) return use_anim == 28;
    return k == GestureKind::Crush   ? use_anim == 9 || use_anim == 33
           : k == GestureKind::Shake ? use_anim == 14 || use_anim == 15 || use_anim == 18
                                     : true;
}
// Legacy quick-stab route retained for configuration compatibility; no quick-slot
// goods use it now. Bottled blood is drunk and Queenly Blood is squeezed.
constexpr bool StabOf(std::int32_t goods) {
    (void)goods;
    return false; // Only regular Blood Vials inject, via their separate belt/heal path.
}
constexpr bool StabAnimAgrees(std::int32_t use_anim) {
    return use_anim == 28;
}

// ---- The use sounds (quick_use_sounds) ----------------------------------------------------------

// The tester's 2026-10-06 "and they should play a sound when activated": an item used at once (the
// game's UseGoods without its use animation: at the mouth, wiped, crushed, held to use, stabbed,
// used at the pull, the Blood Vial's X / belt) plays the game's own sound of that use, which lives
// in the use clip's TAE (event 128, PlaySound) and is skipped with the clip. The clip of a
// goodsUseAnim (EquipParamGoods +0x3E) is the player behaviour's "UseItem_<anim>" selector
// (c0000.hkx); its item sounds are the clip's SoundType 1 ('c') events in a00.tae. The armour's
// rustle (type 9) and the footsteps (8) are the body's (your own arm moves in VR) and the SFX
// sounds (type 5) are left out. Times are the TAE's, from the clip's start:
//   anim clip   the clip's item sounds                                  played at once
//    0  100000  none: the coat's rustle only (eat)                      c100050, the drink's
//                                                                       swallow (closest)
//    1  100130  none (the blade wipe; its buff's own effect)            c750, the vial clip's
//                                                                       arm whoosh (closest)
//    5  100140  c710 0.367, c793 0.400 (the ash into the gun)           c710, c793 +33 ms
//    9  100020  none: effects 200/201/202 only (the crush)              c100180, the Vermin's
//                                                                       crush (closest)
//   10  100050  c8000 0.433 (the bottle out), c100050 0.833 (the lips)  c100050
//   26  100080  c8100 0.033, c750 0.667 (the arm), c100080 0.833 (the   c100080, c5480 +33 ms
//               stab), c5480 0.867 (the blood): the Blood Vial
//   28  100081  as 100080 (the NPC bloods, Iosefka's vial)              c100080, c5480 +33 ms
//   33  100180  c100180 1.067 (the crush), c100181 1.733 (Vermin)       c100180, c100181 +667
//   35  100021  none: as 100020 (the Umbilical Cords)                   c100180 (closest)
// The ones used with the game's own animation (select + Square) keep the clip's own sounds and
// get nothing added: 14 100110 (Beckoning, Small / Sinister Resonant Bell: no PlaySound in the
// clip), 15 100090 (Silencing Blank: c10000100 0.800), 16 100120 (the Marks: SFX 121), 18 100300
// (Choir Bell: c5480 1.667 and SFX 7020 / 7030 / 7031), 27 100170 (Tiny Music Box: c100171,
// c100170 1.0); the throws (2, 3, 13, 34: 100031 / 100030 / 100032 / 100033) play c751 with
// throw_sound.
struct UseSound {
    std::int32_t id{-1};      // SoundType 1 ('c'): PlaySound's type 1 (0xA with the table's flag)
    std::int32_t after_ms{0}; // after the use's first sound
};
struct UseSounds {
    std::int32_t clip{-1}; // the use clip (a00.tae animation id), -1 unknown
    bool own{false};       // the clip's own sound (false: none in it, the closest game sound)
    std::uint8_t count{0};
    std::array<UseSound, 2> sound{};
};
// The behaviour's UseItem_<anim> selector -> its clip (a00), -1 for an anim it has none for.
constexpr std::int32_t UseClipOf(std::int32_t use_anim) {
    switch (use_anim) {
    case 0: return 100000;
    case 1: return 100130;
    case 2: return 100031;
    case 3: return 100030;
    case 4: return 100160;
    case 5: return 100140;
    case 6: return 100150;
    case 7: return 100040;
    case 8: return 100060;
    case 9: return 100020;
    case 10: return 100050;
    case 13: return 100032;
    case 14: return 100110;
    case 15: return 100090;
    case 16: return 100120;
    case 17: return 100100;
    case 18: return 100300;
    case 19: return 100310;
    case 20: return 100320;
    case 21: return 100330;
    case 22: return 100340;
    case 23: return 100350;
    case 24: return 100360;
    case 25: return 100370;
    case 26: return 100080;
    case 27: return 100170;
    case 28: return 100081;
    case 29: return 101180;
    case 30: return 100380;
    case 31: return 100390;
    case 32: return 100400;
    case 33: return 100180;
    case 34: return 100033;
    case 35: return 100021;
    case 255: return 100010;
    default: return -1;
    }
}
inline constexpr std::int32_t SwallowSound = 100050; // the drinking clip's, at the lips
inline constexpr std::int32_t InjectSound = 100080;  // the Blood Vial clip's stab
inline constexpr std::int32_t BloodSound = 5480;     // ... and its blood, 33 ms later
inline constexpr std::int32_t CrushSound = 100180;   // the Vermin clip's crush in the fist
inline constexpr std::int32_t ArmWhooshSound = 750;  // the Blood Vial clip's arm whoosh
// What an item used at once with that goodsUseAnim plays (count 0: nothing).
constexpr UseSounds UseSoundsOf(std::int32_t use_anim) {
    UseSounds u{};
    u.clip = UseClipOf(use_anim);
    switch (use_anim) {
    case 0: // eat: no sound of its own
        u.count = 1;
        u.sound[0] = {SwallowSound, 0};
        break;
    case 1: // the blade wipe: no sound of its own
        u.count = 1;
        u.sound[0] = {ArmWhooshSound, 0};
        break;
    case 5: // the Bone Marrow Ash
        u.own = true;
        u.count = 2;
        u.sound[0] = {710, 0};
        u.sound[1] = {793, 33};
        break;
    case 9:  // the crush: no sound of its own
    case 35: // the Umbilical Cords (the crush's clip)
        u.count = 1;
        u.sound[0] = {CrushSound, 0};
        break;
    case 10: // drunk
        u.own = true;
        u.count = 1;
        u.sound[0] = {SwallowSound, 0};
        break;
    case 26: // the Blood Vial
    case 28: // the NPC bloods, Iosefka's vial
        u.own = true;
        u.count = 2;
        u.sound[0] = {InjectSound, 0};
        u.sound[1] = {BloodSound, 33};
        break;
    case 33: // Vermin
        u.own = true;
        u.count = 2;
        u.sound[0] = {CrushSound, 0};
        u.sound[1] = {100181, 667};
        break;
    default:
        break;
    }
    return u;
}

// Physical VR actions can differ from the native animation without changing item effects.
constexpr UseSounds UseSoundsForGoods(std::int32_t goods, std::int32_t use_anim) {
    return UseSoundsOf(BloodDrinkOf(goods) ? 10 : goods == QueenlyBlood ? 9 : use_anim);
}

// CRUSH: the squeeze (left_scheme=2: the grip; 1: the trigger) at least `press` and above
// `release` for hold_s. Never the take's own fist: it arms only once that button was seen under
// open_below after the take (a whole-hand grab, a grip still held from opening the arc, a trigger
// pulled with the grab's squeeze). On Touch a firm squeeze bottoms out at 1.0, a resting finger
// reads 0..0.3 and a nervous half squeeze stays under 0.75.
struct CrushConfig {
    float press{0.90f};
    float release{0.75f};
    float hold_s{0.15f};
    float open_below{0.50f};
    float min_hold_s{0.20f}; // the item in the hand at least this long
};

class CrushGate {
public:
    void Reset(double held_since) {
        *this = {};
        held_since_ = held_since;
    }
    // Every input frame while a crushable is in the hand (`arm`: the hunter's arm is yours). True
    // on the frame it is crushed.
    bool Update(const CrushConfig& c, double t, float squeeze, bool arm) {
        squeeze_ = squeeze;
        if (!armed_) {
            armed_ = squeeze < c.open_below;
            if (armed_) {
                armed_t_ = t;
            }
            return false;
        }
        on_ = on_ ? squeeze > c.release : squeeze >= c.press;
        if (!on_ || !arm) {
            since_ = -1.0; // let go (or the game's arm): the confirm starts again
            return false;
        }
        if (since_ < 0.0) {
            since_ = t;
        }
        peak_ = std::max(peak_, squeeze);
        return t - since_ >= c.hold_s - 1e-6 && t - held_since_ >= c.min_hold_s - 1e-6;
    }
    // 0..1 through the confirm (the strain hum); < 0: not squeezing hard.
    [[nodiscard]] float Strain(const CrushConfig& c, double t) const {
        return since_ < 0.0 ? -1.0f
                            : std::clamp(static_cast<float>((t - since_) / c.hold_s), 0.0f, 1.0f);
    }
    // 0..1 from open_below up to press while armed (the pressure building before it).
    [[nodiscard]] float Pressure(const CrushConfig& c) const {
        return armed_ && c.press > c.open_below
                   ? std::clamp((squeeze_ - c.open_below) / (c.press - c.open_below), 0.0f, 1.0f)
                   : 0.0f;
    }
    // Used (or refused): let go and squeeze again.
    void Consume() {
        armed_ = on_ = false;
        since_ = -1.0;
    }
    [[nodiscard]] bool Armed() const {
        return armed_;
    }
    [[nodiscard]] double ArmedAt() const {
        return armed_t_;
    }
    [[nodiscard]] double Since() const {
        return since_;
    }
    [[nodiscard]] float Peak() const {
        return peak_;
    }
    [[nodiscard]] double HeldSince() const {
        return held_since_;
    }

private:
    double held_since_{0.0};
    double armed_t_{0.0};
    double since_{-1.0};
    float squeeze_{0.0f};
    float peak_{0.0f};
    bool armed_{false};
    bool on_{false};
};

// SHAKE: strokes, back and forth, on two channels: the wrist (the controller's angular velocity,
// rad/s) and the forearm (the hand's velocity against the head's, m/s: walking, leaning and a
// dodge's lean do not count). A stroke is one lobe of the signal: it starts at `lo` (the second
// on pointing against the one before), keeps its peak, and ends where it falls to end_frac of the
// peak along it (the deceleration just before the reversal, where a real clapper strikes). It
// counts with its peak at least `hi`, lasting min_stroke_s .. max_stroke_s (longer: a wave, a
// walking swing; shorter: a glitch), begun within max_gap_s of the last one's end and pointing
// back against it (cos <= reverse_cos: a tumbling or circling wrist never counts). Rung: the last
// `strokes` counted from the first one's start to the last one's end within window_s, the weakest
// peak at least min_ratio of the strongest (rhythmic: a throw's wind-up, fling and recoil are
// lopsided). Samples above `max` are tracking jumps (no count, no break). Realistic numbers: a
// handbell rung from the wrist +-30 degrees at 3 Hz peaks near 10 rad/s, a gentle 2 Hz ring 5.5;
// a stiff-wrist forearm shake +-6 cm at 3 Hz about 1.1 m/s; a walking arm swing under 2 rad/s
// with 0.55 s half-cycles; turning to look at the item 2-4 rad/s with one reversal at most.
struct ShakeConfig {
    int strokes{3};
    float window_s{0.90f};
    float ang_hi{5.0f};
    float ang_lo{1.5f};
    float ang_max{40.0f};
    float lin_hi{0.9f};
    float lin_lo{0.25f};
    float lin_max{25.0f};
    float end_frac{0.35f};
    float min_stroke_s{0.03f};
    float max_stroke_s{0.30f}; // (0.35 let a brisk 1.2 Hz walking swing's lobes count)
    float max_gap_s{0.20f};
    float reverse_cos{-0.5f};
    float min_ratio{0.30f};
    float min_hold_s{0.25f};   // the bell in the hand at least this long (not the take's jerk)
    float refractory_s{0.06f}; // the strike feel at most this often (both channels)
    float lost_s{0.10f};       // tracking lost longer than this breaks the chain
};

// One signal's strokes. Update: 1 on the sample whose stroke counts (a strike), else 0.
class ShakeChannel {
public:
    struct Stroke {
        double start{0.0};
        double end{0.0};
        float peak{0.0f};
        V3 dir{};
    };
    void Reset() {
        *this = {};
    }
    int Update(const ShakeConfig& c, double t, V3 x, float hi, float lo, float max) {
        const float m = Length(x);
        if (!(m <= max)) {
            return 0; // a tracking jump: neither counted nor breaking
        }
        int counted = 0;
        if (in_) {
            if (m > peak_m_ && Dot(x, peak_) > 0.0f) {
                peak_ = x;
                peak_m_ = m;
            }
            if (Dot(x, peak_) < c.end_frac * peak_m_ * peak_m_) {
                in_ = false;
                counted = End(c, t, hi);
            }
        }
        // The chain is over once no stroke can still join it (none began within max_gap_s, or
        // the one under way is already too long).
        if (count_ > 0 && (in_ ? start_t_ - last_.end > c.max_gap_s + 1e-6 ||
                                     t - start_t_ > c.max_stroke_s + 1e-6
                               : t - last_.end > c.max_gap_s + 1e-6)) {
            count_ = 0;
        }
        // A lobe starts rising through `lo` (never mid-motion: the hold opening on a walking
        // swing's peak), or as the reversal of the last one.
        const bool reversal = prev_t_ >= 0.0 && t - prev_t_ <= c.max_gap_s && Dot(x, prev_) < 0.0f;
        if (m < lo) {
            quiet_ = true;
        } else if (!in_ && (quiet_ || reversal)) {
            quiet_ = false;
            in_ = true;
            start_t_ = t;
            peak_ = x;
            peak_m_ = m;
        }
        return counted;
    }
    [[nodiscard]] int Count() const {
        return count_;
    }
    // The last c.strokes counted strokes fit the window, rhythmic.
    [[nodiscard]] bool Rung(const ShakeConfig& c) const {
        const int n = std::clamp(c.strokes, 1, static_cast<int>(ring_.size()));
        if (count_ < n) {
            return false;
        }
        float lo = 1e9f;
        float hi = 0.0f;
        double first = last_.end;
        for (int k = 0; k < n; ++k) {
            const Stroke& s = Back(k);
            lo = std::min(lo, s.peak);
            hi = std::max(hi, s.peak);
            first = s.start;
        }
        return last_.end - first <= c.window_s + 1e-6 && lo >= c.min_ratio * hi - 1e-6f;
    }
    // The k-th newest counted stroke (0: the last).
    [[nodiscard]] const Stroke& Back(int k) const {
        return ring_[(head_ + ring_.size() - 1 - static_cast<std::size_t>(k) % ring_.size()) %
                     ring_.size()];
    }

private:
    int End(const ShakeConfig& c, double t, float hi) {
        const double d = t - start_t_;
        if (d < c.min_stroke_s - 1e-6) {
            return 0; // a jitter: ignored
        }
        prev_ = peak_;
        prev_t_ = t;
        if (peak_m_ < hi || d > c.max_stroke_s + 1e-6) {
            count_ = 0; // weak or slow: the chain is broken
            return 0;
        }
        const bool next = count_ > 0 && start_t_ - last_.end <= c.max_gap_s + 1e-6 &&
                          Dot(peak_, last_.dir) <= c.reverse_cos * peak_m_ * last_.peak;
        if (!next) {
            count_ = 0; // a new chain starts with this one
        }
        last_ = {start_t_, t, peak_m_, peak_};
        ring_[head_] = last_;
        head_ = (head_ + 1) % ring_.size();
        ++count_;
        return 1;
    }

    std::array<Stroke, 8> ring_{};
    std::size_t head_{0};
    Stroke last_{};
    int count_{0};
    bool in_{false};
    double start_t_{0.0};
    V3 peak_{};
    float peak_m_{0.0f};
    V3 prev_{};          // the last finished lobe (counted or not): the next starts against it
    bool quiet_{false};  // under `lo` since the last lobe began (a new one may rise from there)
    double prev_t_{-1.0};
};

// Both channels, the hold time, the arm and the strike's refractory.
class ShakeGate {
public:
    enum class Event : std::uint8_t { None, Stroke, Rung };
    void Reset(double held_since) {
        *this = {};
        held_since_ = held_since;
    }
    // Every input frame while a bell is in the hand: `w` the controller's angular velocity (rad/s),
    // `v_rel` the hand's velocity against the head's (m/s), `valid` the hand tracked.
    Event Update(const ShakeConfig& c, double t, bool valid, V3 w, V3 v_rel, bool arm) {
        // Also expire on recovery: there may have been no input frames during the gap.
        if (last_valid_t_ >= 0.0 && t - last_valid_t_ > c.lost_s) {
            ang_.Reset();
            lin_.Reset();
        }
        if (!valid) {
            return Event::None;
        }
        last_valid_t_ = t;
        if (!arm || t - held_since_ < c.min_hold_s - 1e-6) {
            ang_.Reset(); // the game's arm, or the take's own jerk: nothing counts
            lin_.Reset();
            return Event::None;
        }
        const int a = ang_.Update(c, t, w, c.ang_hi, c.ang_lo, c.ang_max);
        const int l = lin_.Update(c, t, v_rel, c.lin_hi, c.lin_lo, c.lin_max);
        if ((a != 0 && ang_.Rung(c)) || (l != 0 && lin_.Rung(c))) {
            by_wrist_ = a != 0 && ang_.Rung(c);
            return Event::Rung;
        }
        if ((a != 0 || l != 0) && (last_strike_ < 0.0 || t - last_strike_ >= c.refractory_s)) {
            last_strike_ = t;
            return Event::Stroke;
        }
        return Event::None;
    }
    // The strokes of the longer live chain (the ring's steps).
    [[nodiscard]] int Strokes() const {
        return std::max(ang_.Count(), lin_.Count());
    }
    // The rung chain, for the log: "3 strokes in 0.47 s by the wrist: 8.1 / 9.4 / 7.7 rad/s".
    [[nodiscard]] std::string Detail(const ShakeConfig& c) const {
        const ShakeChannel& ch = by_wrist_ ? ang_ : lin_;
        const int n = std::clamp(c.strokes, 1, 8);
        std::string peaks;
        char text[32];
        for (int k = n - 1; k >= 0; --k) {
            std::snprintf(text, sizeof(text), "%s%.1f", peaks.empty() ? "" : " / ",
                          ch.Back(k).peak);
            peaks += text;
        }
        char line[160];
        std::snprintf(line, sizeof(line), "%d strokes in %.2f s by the %s: %s %s", n,
                      ch.Back(0).end - ch.Back(n - 1).start, by_wrist_ ? "wrist" : "forearm",
                      peaks.c_str(), by_wrist_ ? "rad/s" : "m/s");
        return line;
    }
    [[nodiscard]] double HeldSince() const {
        return held_since_;
    }

private:
    ShakeChannel ang_{};
    ShakeChannel lin_{};
    double held_since_{0.0};
    double last_strike_{-1.0};
    double last_valid_t_{-1.0};
    bool by_wrist_{false};
};

// ---- Shared records ------------------------------------------------------------------------------

struct Slot {
    std::int32_t inv_idx{-1}; // inventory index the slot points at (-1 empty)
    std::int32_t goods{-1};   // EquipParamGoods id (-1 empty or used up)
    std::int32_t qty{0};
    std::int32_t icon{-1};    // EquipParamGoods iconId
    std::int32_t ffx{-1};     // the game's held model (HeldFfx), -1 none
    bool usable{false};       // the game's CanUseGoods
    [[nodiscard]] bool Empty() const {
        return goods < 0 || qty <= 0;
    }
};

// Site A -> everyone (seqlock), every main-player update.
struct Snapshot {
    std::uint64_t seq{0};
    std::int64_t steady_ns{0};
    bool valid{false};
    std::array<Slot, SlotCount> slot{};
    std::int32_t selected{-1};     // slot of the game's selection (the HUD's), -1 none
    std::int32_t override_idx{-1}; // an item picked in the inventory menu is pending (>= 0)
    bool busy{false};              // an item is being used (the game's own, or a menu flow)
    bool alive{true};
    [[nodiscard]] int Count() const {
        int n = 0;
        for (const Slot& s : slot) {
            n += s.Empty() ? 0 : 1;
        }
        return n;
    }
};

enum class Action : std::uint8_t { None, Use, Throw };

// Presenter -> site A: one request at a time (a new serial replaces an untaken one).
struct Request {
    std::uint32_t serial{0};
    std::int64_t steady_ms{0};
    std::int8_t select{-1}; // slot to select in the game (its HUD), -1 none
    Action action{Action::None};
    std::int32_t goods{-1};
    std::int32_t slot{-1};
    std::int8_t dmy{-1};
    std::array<float, 3> v_world{}; // throw: launch velocity, game world units per second
    std::uint8_t throw_class{0};
    std::uint8_t mouth{0}; // a use at the mouth (MouthKind): the game side plays its drink sound
    std::uint8_t wipe{0};  // a use wiped along the blade: the game side checks the buff
    std::uint8_t stab{0};  // a blood stabbed in (quick_use_stab): refused at full HP, as the vial
    // quick_music_box_crank: the Tiny Music Box played by its crank (core/vr/music_box.h): the
    // game's UseGoods at once (its bullet, no animation) and its tune.
    std::uint8_t crank{0};
};

enum class Outcome : std::uint8_t {
    None,
    Selected, // the select went through (no action asked)
    Used,     // UseGoods ran and one was consumed (or the item is not consumed)
    Thrown,   // UseGoods ran with the bullet, one consumed
    Denied,   // a gate said no (reason)
    Latched,  // UseGoods ran but nothing was consumed: this goods goes to Square from now on
    Expired,  // too old when the player update saw it
};
constexpr const char* OutcomeName(Outcome o) {
    switch (o) {
    case Outcome::None:
        return "none";
    case Outcome::Selected:
        return "selected";
    case Outcome::Used:
        return "used";
    case Outcome::Thrown:
        return "thrown";
    case Outcome::Denied:
        return "denied";
    case Outcome::Latched:
        return "latched";
    case Outcome::Expired:
        return "expired";
    }
    return "?";
}

// Site A -> presenter (seqlock): the answer to request `serial`.
struct Result {
    std::uint32_t serial{0};
    Outcome outcome{Outcome::None};
    const char* reason{""}; // static text
    std::int32_t goods{-1};
    std::int32_t qty_before{0};
    std::int32_t qty_after{0};
};

enum class SceneState : std::uint8_t { Closed, Open, Holding };

// Presenter -> render-camera hook (seqlock), every input frame.
struct Scene {
    Holsters::Layout holsters{};
    bool holsters_enabled{false};
    std::int64_t holsters_ns{0};
    std::uint32_t open_serial{0}; // + 1 per open: the arc is placed again
    SceneState state{SceneState::Closed};
    std::int64_t state_ns{0};   // steady clock of the last state change (animations)
    std::uint8_t cards{0};      // bit k: slot k is on the arc (non-empty when it opened)
    // The last item remains absent from its mount while a throw/use awaits the game reply.
    std::int8_t reserved_last_slot{-1};
    std::int32_t reserved_last_goods{-1};
    std::int8_t held_slot{-1};
    std::int32_t held_goods{-1};
    std::int8_t pulse_slot{-1}; // a grab / deny flash on this slot ...
    bool pulse_deny{false};
    std::int64_t pulse_ns{0};   // ... since then
    // quick_hold_use: the ring round the held item fills from hold_from_ns (steady ns; 0: no
    // ring) over hold_s; it was full (the item used: its flash) at hold_done_ns (0: never).
    std::int64_t hold_from_ns{0};
    float hold_s{0.0f};
    std::int64_t hold_done_ns{0};
};

inline bool ShowStoredItem(const Scene& scene, const Slot& slot, int index) {
    return !slot.Empty() && !(slot.qty <= 1 && scene.reserved_last_slot == index &&
                             scene.reserved_last_goods == slot.goods);
}


// Render-camera hook -> presenter (seqlock): what the hunter's hand hovers.
struct Hover {
    std::uint32_t open_serial{0}; // the arc this belongs to
    std::int64_t steady_ns{0};
    bool placed{false};       // the arc is placed (the hand was found)
    // When it was placed (steady ns; 0 = unknown): at the open, or later when the game had the
    // arm then (the arc waits for it). Its unfolding counts from here.
    std::int64_t placed_ns{0};
    std::int8_t slot{-1};     // hovered slot, -1 none
    // The hover point (LOCAL): the hunter's grip, L_Weapon's dummy 340 (the centre of his fist,
    // where your controller is: the left arm's IK puts it there), or his palm (dummy 3, 3.1 cm
    // toward the wrist and the palm) where the grip cannot be read.
    std::array<float, 3> hand{};
    std::array<float, 3> centre{}; // where the arc opened (LOCAL)
    std::array<float, 3> eye{};    // the eye the hover was measured from (LOCAL)
    float scale{1.0f};             // the quick_arc_hover_scale it was measured with
    std::array<std::int8_t, SlotCount> card_slot{-1, -1, -1, -1, -1, -1}; // layout index -> slot
    // The cards' centres (LOCAL) and diameter (m): what the presenter keeps clear of the UI
    // layers drawn over the eye images.
    std::uint8_t card_count{0};
    float card_m{0.0f};
    std::array<std::array<float, 3>, SlotCount> cards{};
};

// ---- The arc's layout (LOCAL) ----------------------------------------------------------------

struct LayoutConfig {
    float radius_m{0.18f};
    float card_m{0.064f};      // quick_arc_card_m (drawn by the presenter; not used to place)
    float min_eye_m{0.32f};    // DIBR's nearest apparent distance is 0.3 m
    float span_deg{150.0f};    // end card to end card (six cards: 30 degrees, 9.3 cm apart)
    float max_step_deg{32.0f}; // never further apart than this (a few cards)
};

struct Layout {
    int n{0};
    std::array<V3, SlotCount> card{}; // centres
    V3 pivot{};                       // the hand the arc opened around
    V3 right{1.0f, 0.0f, 0.0f};       // arc plane axes
    V3 up{0.0f, 1.0f, 0.0f};          // 12 o'clock in the arc's plane
    V3 toward{0.0f, 0.0f, 1.0f};      // the plane's normal, from the arc toward the eyes
    float step_rad{0.0f};             // between two neighbours (radians, about the pivot)
};

// n cards on a crescent around the hand, centred at 12 o'clock, in a plane facing the eyes: the
// hand is usually well below them (looked at 45-55 degrees down), so the plane leans back by up
// to MaxTiltDeg toward the line of sight and the end cards keep their spacing on screen instead of
// crowding (a vertical fan seen from above shrinks to ~65% of its height). The whole arc is pushed
// back so no card is nearer than min_eye_m, and kept between 0.55 m and 0.05 m below the eyes.
inline Layout PlaceArc(int n, V3 palm, V3 eye, const LayoutConfig& c) {
    constexpr float MaxTiltDeg = 50.0f;
    Layout l{};
    l.n = std::clamp(n, 0, SlotCount);
    V3 pivot = palm;
    pivot.y = std::clamp(pivot.y, eye.y - 0.55f - c.radius_m * 0.5f, eye.y - 0.05f - c.radius_m);
    V3 flat = eye - pivot;
    flat.y = 0.0f;
    flat = Normalize(flat, {0.0f, 0.0f, 1.0f});
    // The line of sight's elevation (the eye above the pivot), limited.
    const V3 sight = eye - pivot;
    const float level = std::max(Length(V3{sight.x, 0.0f, sight.z}), 1e-3f);
    const float tilt =
        std::clamp(std::atan2(sight.y, level), 0.0f, MaxTiltDeg * (3.14159265f / 180.0f));
    l.toward = flat * std::cos(tilt) + V3{0.0f, 1.0f, 0.0f} * std::sin(tilt);
    l.right = Normalize(Cross(V3{0.0f, 1.0f, 0.0f}, flat), {1.0f, 0.0f, 0.0f}); // viewer's right
    l.up = Cross(l.toward, l.right);
    const float step_deg =
        l.n > 1 ? std::min(c.max_step_deg, c.span_deg / static_cast<float>(l.n - 1)) : 0.0f;
    l.step_rad = step_deg * (3.14159265f / 180.0f);
    for (int i = 0; i < l.n; ++i) {
        const float a = (static_cast<float>(i) - 0.5f * static_cast<float>(l.n - 1)) * step_deg *
                        (3.14159265f / 180.0f);
        l.card[static_cast<std::size_t>(i)] =
            pivot + l.right * (std::sin(a) * c.radius_m) + l.up * (std::cos(a) * c.radius_m);
    }
    // Not too near the eyes: pushed away along the plane's normal (-toward) until every card is.
    for (int pass = 0; pass < 16; ++pass) {
        float push = 0.0f;
        for (int i = 0; i < l.n; ++i) {
            const float distance = Length(l.card[static_cast<std::size_t>(i)] - eye);
            if (distance < c.min_eye_m) {
                push = std::max(push, c.min_eye_m - distance + 0.005f);
            }
        }
        if (push <= 0.0f) {
            break;
        }
        pivot = pivot - l.toward * push;
        for (int i = 0; i < l.n; ++i) {
            l.card[static_cast<std::size_t>(i)] = l.card[static_cast<std::size_t>(i)] -
                                                  l.toward * push;
        }
    }
    l.pivot = pivot;
    return l;
}

// The hover volume round a card, for the hunter's grip point: in the card's plane its half size
// (3 cm) plus the fist's (3-4 cm), along the line of sight the depth error of reaching into a
// card seen in the headset (+-3 cm) plus the fist's own depth. Entered at a metric <= 1, kept to
// HoverKeep (8.1 cm in the plane), left for a neighbour only when that one is HoverSwitch nearer.
// quick_arc_hover_scale (1.12 since 2026-10-08) widens the volume and the keep; HoverSwitch is of
// the unscaled volume (1.5 cm in the plane at any scale), so a wider volume makes a card easier
// to light but never stickier to leave for its neighbour.
inline constexpr float HoverRadiusM = 0.060f;
inline constexpr float HoverDepthM = 0.10f;
inline constexpr float HoverKeep = 1.35f;
inline constexpr float HoverSwitch = 0.25f;

// The hover metric of the hand against a card: within the card's plane HoverRadiusM and
// HoverDepthM in front of / behind it, scaled; <= 1 inside.
inline float HoverMetric(V3 hand, V3 card, V3 normal, float scale) {
    const V3 d = hand - card;
    const float along = Dot(d, normal);
    const V3 in_plane = d - normal * along;
    const float a = Length(in_plane) / (HoverRadiusM * scale);
    const float b = along / (HoverDepthM * scale);
    return std::sqrt(a * a + b * b);
}

// The hovered card (index into the layout): the nearest card, with hysteresis (enter at <= 1,
// keep until > HoverKeep, switch to another card only when it is HoverSwitch nearer), so the hand
// between two neighbours never flickers between them. The metrics are of the scaled volume; the
// switch's margin is HoverSwitch / scale of it, i.e. HoverSwitch of the unscaled one: the hand
// moving from card to card switches at the same point whatever quick_arc_hover_scale is.
inline int PickHover(const Layout& l, V3 hand, V3 eye, int current, float scale) {
    const float switch_margin = HoverSwitch / std::max(scale, 1e-3f);
    int best = -1;
    float best_m = 1e9f;
    float current_m = 1e9f;
    for (int i = 0; i < l.n; ++i) {
        const V3 card = l.card[static_cast<std::size_t>(i)];
        const float m = HoverMetric(hand, card, Normalize(eye - card), scale);
        if (i == current) {
            current_m = m;
        }
        if (m < best_m) {
            best_m = m;
            best = i;
        }
    }
    if (current >= 0 && current < l.n && current_m <= HoverKeep) {
        return best != current && best_m + switch_margin < current_m && best_m <= 1.0f ? best
                                                                                       : current;
    }
    return best_m <= 1.0f ? best : -1;
}

// The card nearest the hand of a published hover (by the hover metric) and the runner-up's
// metric: a squeeze with no lit card takes the nearest one when it is near and clearly nearer
// than any other (Controller::UpdateOpen).
struct Nearest {
    int index{-1}; // layout index
    int slot{-1};
    float metric{1e9f};
    float second{1e9f}; // the next card's metric
};
inline Nearest NearestOf(const Hover& h) {
    const V3 hand{h.hand[0], h.hand[1], h.hand[2]};
    const V3 eye{h.eye[0], h.eye[1], h.eye[2]};
    Nearest n{};
    for (int i = 0; i < std::min<int>(h.card_count, SlotCount); ++i) {
        const auto& p = h.cards[static_cast<std::size_t>(i)];
        const V3 card{p[0], p[1], p[2]};
        const float m = HoverMetric(hand, card, Normalize(eye - card), h.scale);
        if (m < n.metric) {
            n.second = n.metric;
            n.metric = m;
            n.index = i;
        } else if (m < n.second) {
            n.second = m;
        }
    }
    n.slot = n.index >= 0 ? h.card_slot[static_cast<std::size_t>(n.index)] : -1;
    return n;
}

// Pie selection (an option, quick_arc_pick=sector): the direction of the hand from where the arc
// opened picks the card, wherever along that direction the hand is (dead_m .. far_m from the
// pivot in the arc's plane, within depth_m of it); the end cards' sectors reach end_extra_deg
// further out. A hovered card is kept until the hand is hyst_m (an arc length) into the
// neighbour's sector. Layout index, -1 none.
inline int PickSector(const Layout& l, V3 hand, int current, float dead_m = 0.07f,
                      float far_m = 0.30f, float depth_m = 0.12f, float hyst_m = 0.012f,
                      float end_extra_deg = 30.0f) {
    if (l.n <= 0) {
        return -1;
    }
    const V3 d = hand - l.pivot;
    const float along = Dot(d, l.toward);
    const float x = Dot(d, l.right);
    const float y = Dot(d, l.up);
    const float r = std::sqrt(x * x + y * y);
    const bool keep = current >= 0 && current < l.n;
    if (r < dead_m - (keep ? 0.01f : 0.0f) || r > far_m || std::abs(along) > depth_m) {
        return -1;
    }
    const float step = l.n > 1 ? l.step_rad : 1.0f;
    const float a = std::atan2(x, y); // 0 = 12 o'clock, + = the viewer's right
    const auto angle = [&l, step](int i) {
        return (static_cast<float>(i) - 0.5f * static_cast<float>(l.n - 1)) * step;
    };
    const float extra = end_extra_deg * (3.14159265f / 180.0f);
    if (a < angle(0) - 0.5f * step - extra || a > angle(l.n - 1) + 0.5f * step + extra) {
        return -1;
    }
    int k = 0;
    for (int i = 1; i < l.n; ++i) {
        if (std::abs(a - angle(i)) < std::abs(a - angle(k))) {
            k = i;
        }
    }
    if (keep && k != current &&
        std::abs(a - angle(current)) < 0.5f * step + std::min(hyst_m / r, 0.5f * step)) {
        return current;
    }
    return k;
}

// ---- The arc's controller (presenter input thread) -------------------------------------------

enum class Haptic : std::uint8_t {
    None,
    Unfurl,
    Close,
    Detent,      // the hand moved onto another card
    FirstDetent, // ... onto the first card of this open (a little stronger)
    Empty,
    Grab,
    LastOne,
    Throw,
    PutBack,
    GunBack,
    Squeeze,
    Use,
    Deny,
    Pick, // left_scheme=2: the trigger took a usable (a light tick; Use follows the game's answer)
    Gulp, // quick_use_mouth: a drink used at the mouth (two soft swallows)
    Bite, // ... a pellet or an antidote eaten there (a crunch, then a swallow)
    HoldDone, // quick_hold_use: the ring full, the item used (a solid thump)
    Ignite,   // quick_use_wipe: the paper wiped along the blade took (a thump on both hands)
    Crunch,     // quick_use_crush: crushed in the fist (a crack, crackle, crumble)
    BellStrike, // quick_use_shake: a stroke of the bell counted (the clapper's clack)
    BellToll,   // ... rung (a full strike, then a decaying hum with a beat in it)
    Inject,     // quick_use_stab: the game took the stabbed blood (the vial's THUMP, both hands)
};

struct Config {
    bool enabled{true}; // quick_arc and quick_items
    float hold_press{0.75f};
    float hold_release{0.45f};
    float use_press{0.75f};
    float use_commit_s{0.10f}; // the trigger held this long uses (a shorter tap does nothing)
    float use_cooldown_s{0.5f};
    float throw_min_speed{1.5f};
    float throw_gain{4.0f};
    float throw_gain_curve{0.8f};     // LaunchCurve's power (1 = linear)
    float throw_lift_deg{6.0f};       // LiftPitch: flat throws leave this much higher
    float throw_peak_window_s{0.30f}; // PeakBefore: look back this far for the hand's peak
    float throw_peak_turn_deg{20.0f}; // ... its direction counts this far from the release's
    float throw_push_factor{0.85f}; // ThrowPower: a straight push gets this share of the gain
    float throw_spin{0.6f};         // share of the angular velocity x the item's offset
    // A hand sample faster than this is a tracking jump (a pose re-acquired after the controller
    // went behind the head), never a throw: it is not kept (logged). Real throws peak at 3-12 m/s.
    float throw_max_hand_speed{25.0f};
    // The release velocity from the grip located at each input frame's own time (Input::hand_*_now,
    // the runtime's freshest estimate) rather than from the poses predicted for the display time
    // (41-50 ms ahead over Link: the newest of those up to the release were read from tracking
    // data about 45 ms older than the grip's opening, and the tester's real throws were all faster
    // after it). Falls back to the display-time poses when there are none.
    bool throw_fresh{true};
    // The grip opened while the hand still sped up (Valve's rule): the throw waits up to this long
    // (the item still in the hand) for the hand's peak, then leaves with it. 0 = at the release.
    float throw_rise_wait_s{0.025f};
    // VR_THROW_TRACE (diagnostics): every release of a throwable also logs its hand samples from
    // throw_trace_s before it to throw_trace_post_s after it (the follow-through: whether the
    // hand was still speeding up when the grip opened), throw_trace_post_s after the release
    // (Output::trace, ThrowTrace).
    bool throw_trace{false};
    float throw_trace_s{0.40f};
    float throw_trace_post_s{0.15f};
    bool throws{true};
    // After a throw or a use the same item again with a new squeeze of the grip within
    // regrab_s, without the arc (while that slot has any left).
    bool stack{true};
    float regrab_s{1.0f};
    float arm_min{0.9f};    // the hunter's left arm is yours from this weight (hover, grab, use)
    float hover_delay_s{0.08f}; // no hover just after the open (the arc unfolds)
    // A squeeze just after the hand left a card takes it (the squeeze's own pull of the hand) ...
    float late_hover_s{0.15f};
    // ... and one with no card lit takes the card clearly nearest the hand (its metric at most
    // nearest_max: 10 cm in the card's plane at the hover scale 1.12, the next card's at least
    // nearest_margin more; 0 = off), never in the first nearest_after_s of the open.
    float nearest_max{1.5f};
    float nearest_margin{0.3f};
    float nearest_after_s{0.12f};
    // A squeeze the gates hold up (the game has the arm for a moment, no fresh hover, the arc
    // still unfolding) waits this long while the grip stays squeezed, then takes what the hand
    // is on: a gate never eats a deliberate grab.
    float grab_wait_s{0.30f};
    // A squeeze this shortly before Y counts as one just after it (Y and the grip pressed
    // together: a quick draw), not as a grip held through the open.
    float grip_lead_s{0.15f};
    bool instant{true};
    bool select{true};
    bool quick_draw{true};
    // The gun with no item in the hand (the arc open, or the stack's next one a squeeze away):
    // the trigger's own latch fires it at once. A pull that comes within fist_s of the grip's
    // squeeze, or with the grip still squeezed, is the fist (a whole-hand squeeze is how a hand
    // grabs on Touch controllers): never the gun, and the trigger must be let go first. A grip
    // already moving at the latch (at least fist_grip) holds the shot up to fist_s for a squeeze
    // that may still come; a grip at rest never delays it.
    float fist_s{0.12f};
    float fist_grip{0.15f};
    // Opt-in (quick_arc_trigger_parry, off: the gun never fires while an item is in the hand):
    // a deliberate pull of the trigger alone with a throwable in hand, at least parry_press deep
    // and the hand slower than a throw, or a tap of it with a usable, puts the item away and
    // fires the gun.
    bool trigger_parry{false};
    float parry_press{0.9f};
    float idle_close_s{6.0f};
    float quick_draw_s{0.6f};
    float quick_draw_dist_m{0.05f};
    float suspend_s{2.5f};
    float square_wait_s{0.8f};
    float result_wait_s{0.6f};
    float hover_max_age_s{0.25f};
    float snapshot_max_age_s{0.3f};
    // left_scheme=2 (vr.ini, the tester's 2026-10-05 layout; 1 = everything above as before): the
    // left grip opens the arc (a squeeze again with nothing taken closes it); the left trigger
    // with the hand on a card lit dwell_s takes it (a pull on a card lit for less waits for it
    // while the trigger stays down and the hand on that card; sweeping across the arc never takes
    // anything): a throwable stays in the hand while the trigger is held past trigger_release
    // and flies when it is let go (the grip's release path: the fresh poses, the rise wait, the
    // look-back, the spike filter; a slow release puts it back on the arc), a usable is used at
    // that pull. The trigger off the cards is the gun (the arc closes); the grip with an item in
    // the hand puts it back (the arc stays open); after a throw the arc closes (opt-in reopen_s >
    // 0 and stack: it is back that long for the next one). belt_vial: the grip at the left hip
    // (Input::at_hip) draws a Blood Vial (Output::draw_vial) and never opens the arc. No stack
    // squeeze, no quick draw, no nearest-card grab, no fist rule for the gun (the grip is not a
    // grab here).
    int left_scheme{1};
    float dwell_s{0.10f};
    float trigger_release{0.35f};
    float reopen_s{0.0f};
    bool belt_vial{false};
    int holster_mode{0};
    bool holster_haptics{true};
    // y_menu (the presenter: SteamVR by default): Y's tap is the game's menu, not the arc, so the
    // grip opens the arc in holster_mode=1 too (there it otherwise only draws from the slots).
    bool y_menu{false};
    // left_scheme=2, after the review of 2026-10-05:
    //  - The grip with an item in the hand puts it back only with the hand still: not when the
    //    hand moved at putback_max_speed or faster in the 100 ms up to the squeeze, and only
    //    putback_wait_s later if meanwhile the hand stayed under it and the trigger was not let go
    //    (a clench during a throw's wind-up is no put-back).
    //  - A pull just after the hand left a card it had lit dwell_s (late_hover_s) takes it only
    //    within late_move_m of where the hand left it (the pull's own jerk); a pull on a card lit
    //    under dwell_s that then slips off it by more than that is the gun's (the arc closes): a
    //    pull is never eaten.
    //  - The arc closes after idle_close2_s with no card lit (the hand moving does not keep it
    //    open: a nervous squeeze in a fight) and on Input::combat (a swing, a leap, a dodge).
    //  - After a throw (reopen_s > 0) the arc that is back takes a card only once it has been lit
    //    reopen_dwell_s (the hand coming back to rest is no reach for the next one) and closes at
    //    reopen_s whatever the hand lit.
    float putback_max_speed{0.8f};
    float putback_wait_s{0.12f};
    float late_move_m{0.03f};
    float hover_lag_s{0.10f}; // the hover's loss is seen this late (the game's frame, the 8 cm hold)
    float idle_close2_s{3.0f};
    float reopen_dwell_s{0.25f};
    // Update not called for longer than this (a menu, the screen, the headset off): the grip,
    // trigger and Y read then are what they were left at, no new press (the presenter's gate holds
    // the held ones back too).
    float resync_gap_s{0.3f};
    // quick_use_mouth (the tester's 2026-10-06 request): an item to eat or drink (MouthOf) taken from
    // the arc stays in the hand (left_scheme=2: while the trigger that took it is held, as a
    // throwable; left_scheme=1: the grip) and is used when the hand brings it to the mouth
    // (Input::mouth, MouthGate: the game's own use at once, a gulp or bite on the hand, the
    // game's drink sound); let go of away from the mouth it goes back (scheme 2: on the arc;
    // scheme 1: away), nothing used. Throwables never: one brought near the face stays in the
    // hand. Off (as before): used at the pull.
    bool mouth{false};
    MouthConfig mouth_cfg{};
    // quick_use_wipe (the tester's 2026-10-06 request): a blade-wipe item (WipeOf: Fire / Bolt
    // Paper, the Empty Phantasm Shell) taken from the arc stays in the hand as one to eat does
    // (left_scheme=2: while the trigger that took it is held; left_scheme=1: the grip) and is
    // applied when wiped along the right weapon's blade (Input::wipe, WipeDetector: the game's own
    // use at once, the rasp while it touches, Haptic::Ignite on both hands); let go of away from
    // the blade, it goes back, nothing used. Off: used at the pull (or held, quick_hold_use).
    bool wipe{false};
    WipeConfig wipe_cfg{};
    // quick_hold_use (the tester's 2026-10-06 request): a misc item to use (HoldOf; never one the
    // mouth takes) taken from the arc stays in the hand while the button that took it is held
    // (left_scheme=2: the trigger; 1: the grip, its trigger then does nothing) and is used once
    // held hold_use_s (the ring full: Scene::hold_*, the rising Output::hold_hum, then
    // Haptic::HoldDone and the game's use: UseGoods at once, or its own animation for the marks
    // and bells); let go of before, it goes back with the put-back tick, nothing used. A hit's
    // stagger, a dodge or a guard (the game holding the arm hold_arm_grace_s) and a dodge or leap
    // (Input::dodge) cancel it the same way. Off: used at the pull (as before).
    bool hold_use{false};
    float hold_use_s{1.5f};
    float hold_arm_grace_s{0.08f};
    // quick_use_crush / quick_use_shake (the tester's 2026-10-06 "yes and yes"): a held item the
    // game crushes in the fist (GestureOf Crush) is also used by squeezing the other button hard
    // (left_scheme=2: the grip, which then never puts it back; 1: the trigger; CrushGate), a bell
    // (Shake) by three back-and-forth strokes (ShakeGate): the ring jumps to full with its flash,
    // Haptic::Crunch / BellToll (a BellStrike per stroke), and the hold's own completion (the
    // game's use at once, or its animation). The hold stays the fallback. shake_choir (default on):
    // the Choir Bell, a hunter tool otherwise used at the pull, is held and rung the same way.
    bool crush{false};
    CrushConfig crush_cfg{};
    bool shake{false};
    bool shake_choir{true};
    ShakeConfig shake_cfg{};
    // Legacy quick-stab configuration retained for compatibility. StabOf maps no
    // goods: bottled blood is drunk, Queenly Blood squeezed, regular vials use heal.
    bool stab{false};
    // quick_music_box_crank (core/vr/music_box.h): the Tiny Music Box taken from the arc stays in
    // the hand while the button that took it is held (no ring); the presenter's crank plays it
    // (Input::crank_play: the game's UseGoods at once, Request::crank) and Output::crank_box frees
    // the right hand meanwhile. Off: held to use (the ring, then the game's animation).
    bool crank{false};
};

struct Input {
    double t{0.0}; // seconds, monotonic
    float dt{0.0f};
    // Gameplay in VR with the motion controls: no game menu, virtual screen, cinematic, Menu held
    // or grip adjust. False closes the arc and puts an item away (silently).
    bool gameplay{false};
    // The hunter's arms follow the controllers (first person, the body on): the arc needs his
    // hand. False: Y is Square.
    bool body{false};
    float arm_weight{0.0f}; // left arm: 1 = yours
    bool y{false};          // Y held
    bool l3{false};         // left stick click held (the modifier): Y = vanilla Square
    // A tap of the left stick click this frame (quick_arc_l3, core/vr/stick_click.h): a Y press
    // for the arc (open, close, put away), never Square. Output::tap_taken says whether the arc
    // took it; the caller sends L3 to the game when it did not.
    bool arc_tap{false};
    const char* arc_tap_by{"L3"}; // ... and its name for the log (left_scheme=2: "Y", its tap)
    bool x_edge{false};     // X pressed this frame (the vial)
    bool trigger_latch{false}; // the gun trigger's own latch (L2) is on
    float grip{0.0f};
    float trigger{0.0f};
    bool hand_valid{false};
    V3 hand_p{};
    V3 hand_v{};
    float hand_w{0.0f};       // the left controller's angular speed (rad/s)
    float pose_lead_s{0.0f};  // the hand's pose is predicted this far ahead of `t` (display time)
    V3 hand_wv{};             // ... as a vector (rad/s, LOCAL)
    Qt hand_q{};              // the left grip's orientation (LOCAL)
    bool shoulder_valid{false};
    V3 shoulder{};            // the player's left shoulder, estimated from the head (LOCAL)
    // The left grip located at the input's own time `t` as well (the runtime's freshest estimate,
    // not predicted ahead to the display time: Config::throw_fresh, and VR_THROW_TRACE), with its
    // orientation and the angular velocity between those (rad/s, LOCAL); and (VR_THROW_TRACE) the
    // runtime's own angular velocity at the display time (rad/s, LOCAL).
    bool hand_now_valid{false};
    V3 hand_p_now{};
    V3 hand_v_now{};
    Qt hand_q_now{};
    V3 hand_wv_now{};
    bool hand_wv_rt_valid{false};
    V3 hand_wv_rt{};
    bool vial_armed{false};
    // left_scheme=2: the left grip is at the left hip (core/vr/belt_vial.h; false when the belt
    // vial is off or cannot be drawn), and a vial is in the hand (armed, being injected or held
    // after a stab: a squeeze then is the needle's fist, never the arc nor another vial).
    bool at_hip{false};
    int holster_assignment{0}; // hovered case: 0 none, 1 vial, 2..7 equipped slots
    bool holster_entered{false};
    int carried_vials{-1}; // unknown leaves the existing injection check authoritative
    bool vial_busy{false};
    // left_scheme=2: this frame a swing went out or a leap / dodge started (`combat_why` names it):
    // the open arc closes. `trigger_raw`: the trigger's analog value even while a context switch
    // holds it back (`trigger` reads 0 then): a trigger held is a clench, the grip opens nothing.
    bool combat{false};
    const char* combat_why{""};
    // A dodge or a leap started this frame (quick_hold_use: the ring filling is cancelled).
    bool dodge{false};
    float trigger_raw{0.0f};
    // quick_use_mouth: the mouth point (MouthPoint, LOCAL) and the head's velocity (the hand's
    // speed at the mouth is measured against it); false: no head pose, nothing is eaten.
    bool mouth_valid{false};
    V3 mouth{};
    V3 head_v{};
    // quick_use_wipe: the newest paper-against-blade sample (WipeContact; `wipe_fresh`: younger
    // than WipeConfig::stale_s), and the two-handed grip holding the handle (the left hand is on
    // it: no wipe until it lets go).
    bool wipe_fresh{false};
    WipeContact wipe{};
    bool two_hand{false};
    // A successful handle/hilt acquisition this frame. An empty arc yields to the weapon
    // before processing item selection or another menu-open input.
    bool weapon_grab{false};
    // quick_use_stab: the stab item in the hand went into a stab zone this frame (the presenter's
    // tracking of the Blood Vial's zones), and which ("wrist", "torso", "lap").
    bool stab{false};
    const char* stab_zone{""};
    // quick_music_box_crank: the crank in the right hand went round play_turns this frame (the
    // presenter's VR::MusicBox::Crank): the box plays.
    bool crank_play{false};
    bool snapshot_fresh{false};
    Snapshot snapshot{};
    bool hover_fresh{false};
    Hover hover{};
    bool have_result{false};
    Result result{};
};

struct Output {
    bool square_hold{false};  // Square held this frame (Y as Square)
    bool square_press{false}; // one Square press (the game's own use)
    // ... of this goods (-1: none): a canned one (CannedOf) arms the cinematic's flat view of
    // its use (the presenter's VR::Quick::NoteCannedUse once the press went out).
    std::int32_t square_goods{-1};
    // Input::arc_tap was taken as a Y press (gameplay, the arc on, and an item phase or the
    // hunter's hand and the slots for an open); false: the caller sends L3 to the game instead.
    bool tap_taken{false};
    // ... the tap came outside gameplay (a cinematic, Menu held, grip adjust): nobody's (the caller
    // sends nothing for it in left_scheme=2, where an untaken tap is otherwise Square).
    bool tap_outside{false};
    bool block_l2{false};     // the left trigger must not reach the game this frame
    bool l2_press{false};     // one L2 press: a trigger tap too short for a use (gun / parry)
    bool send{false};
    Request request{};
    std::array<Haptic, 4> haptics{};
    int haptic_count{0};
    // Haptic::Throw's strength (0..1): the launch speed, from a lob (5 m/s and slower: 0) to a hard
    // throw (16 m/s and faster: 1).
    float throw_strength{0.0f};
    Scene scene{};
    std::int32_t held_goods{-1}; // the hunter's left hand should hold this goods ...
    std::int32_t held_ffx{-1};   // ... with this effect (-1: our mesh only)
    bool disarm_vial{false};
    // left_scheme=2: the grip at the left hip drew a Blood Vial (the presenter arms it as X does),
    // and the grip's squeeze edge this frame (the presenter logs where the hand was).
    bool draw_vial{false};
    // >=0 only for a physical pouch draw: retain its release threshold until put away.
    float vial_grip_release{-1.0f};
    bool grip_edge{false};
    // quick_use_wipe: the paper touches the blade this frame (the rasp's hum on both hands,
    // rising with `wipe_progress`, 0..1 of the stroke) and `wipe_teeth` teeth of it went by.
    bool wipe_touch{false};
    float wipe_progress{0.0f};
    int wipe_teeth{0};
    // ... and the find cue before the touch (WipeNear: 0 far, 1 at the zone's edge; 0 while it
    // touches, the rasp's): the presenter's proximity ticks on the paper hand.
    float wipe_near{0.0f};
    std::string log; // one line of what happened (empty: nothing)
    // VR_THROW_TRACE: a release's hand samples, lines separated by '\n' (empty: nothing).
    std::string trace;
    // quick_hold_use: the ring's fill (0..1; -1: none) and the left hand's rising hum for it
    // (HoldHum; 0: none: the presenter's continuous level for the hand while it is above 0).
    float hold_progress{-1.0f};
    float hold_hum{0.0f};
    float use_whoosh{-1.0f}; // < 0: silent; 0..1: hold/squeeze audio build-up
    // quick_use_stab: a stab item is in the hand, waiting for a stab (the presenter tracks the
    // zones while this is on), and + 1 per take (its zones start again: not after a refusal).
    bool stab_item{false};
    std::uint32_t grabs{0};
    // quick_music_box_crank: the Tiny Music Box is in the hand to be cranked (the right hand is
    // free: the presenter hides the right weapon, no swings, no transform), and the game's answer
    // to a play this frame (1: played, -1: refused, 0: none).
    bool crank_box{false};
    int crank_answer{0};
    // Physical gesture audio is separate from vibration gain and ordinary item use.
    Haptic bell_audio{Haptic::None};
    void BellSound(Haptic h) {
        if (h == Haptic::BellToll || (h == Haptic::BellStrike && bell_audio == Haptic::None))
            bell_audio = h; // at most one cue per update; completion wins
    }
    void Haptics(Haptic h) {
        if (h != Haptic::None && haptic_count < static_cast<int>(haptics.size())) {
            haptics[static_cast<std::size_t>(haptic_count++)] = h;
        }
    }
    void Log(const std::string& line) {
        log += log.empty() ? line : " | " + line;
    }
};

enum class Phase : std::uint8_t {
    Idle,
    Open,       // the arc is shown
    Holding,    // an item in the hand
    Committing, // the trigger is down on a usable (use_commit_s)
    Waiting,    // a use or throw went out, the answer is pending
    SquareWait, // select sent; Square once the game shows it selected
};
// An open menu has no item in the hand and may yield to a weapon grab. A held item or
// an in-flight item action keeps ownership until its normal completion.
constexpr bool WeaponGrabAvailable(Phase phase) {
    return phase == Phase::Idle || phase == Phase::Open;
}

constexpr const char* PhaseName(Phase p) {
    switch (p) {
    case Phase::Idle:
        return "idle";
    case Phase::Open:
        return "open";
    case Phase::Holding:
        return "holding";
    case Phase::Committing:
        return "committing";
    case Phase::Waiting:
        return "waiting";
    case Phase::SquareWait:
        return "square";
    }
    return "?";
}

class Controller {
public:
    void Update(const Config &config, const Input &in, Output &out) {
        Config c = config;
        // Ownership is latched when taken: changing settings while an item is held must not
        // change its release button. A pouch item uses the established grip-held item path.
        if (holster_held_) {
            c.left_scheme = 1;
            c.stack = false;
            c.trigger_parry = false;
        }

        out = {};
        steady_ns_ = static_cast<std::int64_t>(in.t * 1e9);
        // Not updated for a while (a menu, the screen, the headset off): what is held now was held
        // then; no edge from it (the arc would open, a vial be drawn, on the first frame back).
        if (last_t_ >= 0.0 && in.t - last_t_ > c.resync_gap_s) {
            grip_on_ = grip_was_ = in.grip >= c.hold_press;
            trigger_on_ = trigger_was_ = in.trigger >= c.use_press;
            l2_was_ = in.trigger_latch;
            y_down_ = in.y;
            putback_pending_ = false;
        }
        last_t_ = in.t;
        if (!in.gameplay || !in.hand_valid)
            holster_grip_ready_ = false;
        else if (in.grip <= c.hold_release)
            holster_grip_ready_ = true;

        const bool y_press = in.y && !y_down_;
        y_down_ = in.y;
        // quick_arc_l3: a tap of the left stick click is a Y press for the arc wherever the arc
        // can answer one (an open needs the hunter's hand and the slots, as Y's does); elsewhere
        // it is not taken and the caller sends L3 to the game. Never Square.
        const bool tap = in.arc_tap && in.gameplay && c.enabled &&
                         (phase_ != Phase::Idle || (in.body && in.snapshot_fresh));
        out.tap_taken = tap;
        out.tap_outside = in.arc_tap && !in.gameplay;
        const bool y_edge = y_press || tap;
        toggle_by_ = y_press ? "Y" : in.arc_tap_by;
        // A sample faster than any hand (throw_max_hand_speed: a tracking jump) is not kept; the
        // first few while an item is in the hand are logged.
        const auto plausible = [&](V3 v, const char* which) {
            const float speed = Length(v);
            if (!(speed <= c.throw_max_hand_speed)) {
                if (held_goods_ >= 0 && implausible_logged_ < 3) {
                    ++implausible_logged_;
                    out.Log("implausible hand speed " + std::to_string(speed).substr(0, 5) +
                            " m/s ignored (" + which + " pose; a tracking jump, not a throw)");
                }
                return false;
            }
            return true;
        };
        if (in.hand_now_valid) {
            // The item's velocity from the grip located at the input's own time (throw_fresh):
            // the grip's plus its spin at the item's offset, both from the fresh poses.
            const V3 spin_now =
                held_goods_ >= 0 && c.throw_spin > 0.0f
                    ? SpinVelocity(in.hand_wv_now, in.hand_q_now, ReleaseOffsetOf(held_goods_)) *
                                          c.throw_spin
                                    : V3{};
            if (plausible(in.hand_v_now + spin_now, "fresh")) {
                fresh_.Push(in.t, in.hand_v_now + spin_now);
                fresh_spin_.Push(in.t, spin_now);
            }
        }
        if (in.hand_valid) {
            // The velocity of the item in the hand: the grip's plus its spin at the item's offset.
            const V3 offset = ReleaseOffsetOf(held_goods_);
            const V3 spin = held_goods_ >= 0 && c.throw_spin > 0.0f
                                ? SpinVelocity(in.hand_wv, in.hand_q, offset) * c.throw_spin
                                : V3{};
            // At the time the pose was predicted for: the grip's release is read at `t`, the
            // pose of the same frame is already pose_lead_s ahead (latency compensation).
            if (plausible(in.hand_v + spin, "display-time")) {
                history_.Push(in.t + in.pose_lead_s, in.hand_v + spin);
                spin_.Push(in.t + in.pose_lead_s, spin);
            }
            if (in.shoulder_valid) {
                windup_.Push(in.t, in.hand_p - in.shoulder, in.hand_w);
            }
            if (c.throw_trace) {
                ThrowTrace::Sample s{};
                s.t = in.t;
                s.lead = in.pose_lead_s;
                s.p = in.hand_p;
                s.v = in.hand_v;
                s.spin = spin;
                s.w = in.hand_wv;
                s.w_rt_valid = in.hand_wv_rt_valid;
                s.w_rt = in.hand_wv_rt;
                s.rel_valid = in.shoulder_valid;
                s.rel = in.hand_p - in.shoulder;
                s.now_valid = in.hand_now_valid;
                s.p_now = in.hand_p_now;
                s.v_now = in.hand_v_now;
                s.grip = c.left_scheme == 2 ? in.trigger : in.grip; // what lets go of the item
                trace_.Push(s);
            }
        }
        if (in.hand_valid) {
            hand_track_.Push(in.t, in.hand_p);
        }
        // The hand moving (left_scheme=2: a grip squeeze then is a clench, not a put-back).
        if ((in.hand_valid && Length(in.hand_v) >= c.putback_max_speed) ||
            (in.hand_now_valid && Length(in.hand_v_now) >= c.putback_max_speed)) {
            fast_t_ = in.t;
        }
        // VR_THROW_TRACE: a release's samples go out once its follow-through is in.
        if (trace_pending_ && (!c.throw_trace || in.t - trace_t0_ >= c.throw_trace_post_s - 1e-4)) {
            EmitTrace(c, out);
        }
        // Grip and trigger hysteresis (analog, LOCAL to this machine).
        grip_on_ = grip_on_ ? in.grip > c.hold_release : in.grip >= c.hold_press;
        bool grip_edge = grip_on_ && !grip_was_;
        const bool grip_release = !grip_on_ && grip_was_;
        grip_was_ = grip_on_;
        if (grip_edge) {
            grip_edge_t_ = in.t;
        }
        trigger_on_ = trigger_on_ ? in.trigger > c.hold_release : in.trigger >= c.use_press;
        bool trigger_edge = trigger_on_ && !trigger_was_;
        trigger_was_ = trigger_on_;
        if (!trigger_on_) {
            trigger_fist_ = false;
        }
        // A use pull that comes with the grab's squeeze (a whole-hand squeeze) is the fist: no
        // use until the trigger is let go and pulled again.
        if (trigger_edge && in.t - grip_edge_t_ <= c.fist_s) {
            trigger_fist_ = true;
        }
        if (trigger_fist_) {
            trigger_edge = false;
        }
        const bool l2_edge = in.trigger_latch && !l2_was_;
        l2_was_ = in.trigger_latch;
        latch_now_ = in.trigger_latch;
        if (!in.trigger_latch) {
            l2_fresh_needed_ = false; // the gun trigger was let go: the next pull is the gun's
        }
        if (in.weapon_grab && in.gameplay && WeaponGrabAvailable(phase_)) {
            if (phase_ == Phase::Open) {
                Close(out, "the weapon took the hand");
            }
            // The grab wins over a simultaneous Y/tap, item pick, pouch or belt draw. Update
            // button latches above first so held buttons cannot reopen it on the next frame.
            regrab_slot_ = -1;
            regrab_l2_wait_ = false;
            y_square_ = false;
            HoldTriggerBack();
            out.block_l2 = l2_fresh_needed_;
            Publish(out);
            return;
        }
        // left_scheme=2: a throwable taken with the trigger is held until the trigger is let go
        // below trigger_release (its pull, the latch, is at 0.45: the hysteresis between); that
        // is its release, exactly as the grip's opening in scheme 1.
        const bool scheme2 = c.left_scheme == 2;
        bool trigger_let_go = false;
        if (trigger_hold_ && in.trigger <= c.trigger_release) {
            trigger_hold_ = false;
            trigger_let_go = true;
        }
        out.grip_edge = scheme2 && grip_edge;
        // left_scheme=2, the belt vial: a squeeze with the hand at the left hip draws a Blood
        // Vial and never opens the arc, the hand free (the arc may be open, a throw in flight);
        // with a vial already in the hand, or the trigger held (a clench), nothing.
        if (scheme2 && grip_edge && c.belt_vial && in.at_hip && in.gameplay &&
            (phase_ == Phase::Idle || phase_ == Phase::Open ||
             (phase_ == Phase::Waiting && thrown_))) {
            grip_edge = false;
            if (in.vial_busy) {
                out.Haptics(Haptic::Empty);
                out.Log("grip at the hip: a vial is already in the hand");
            } else if (in.trigger_latch || in.trigger_raw >= 0.45f) {
                out.Log("grip at the hip with the trigger held (a clench): no vial");
            } else {
                out.draw_vial = true;
                out.Log("BELT VIAL: the grip at the left hip draws a Blood Vial");
                if (phase_ == Phase::Open) {
                    Close(out, "the belt vial");
                }
            }
        }

        // Y as Square: decided at the press, kept until Y is released. quick_arc=0 and L3 + Y:
        // always; without the hunter's hand (body off, third person) or without the slots: as
        // before. Outside gameplay (a cinematic, Menu held) Y does nothing, so a reflex press
        // after a visceral never uses an item. (An L3 tap is never Square.)
        if (y_press) {
            y_square_ = !c.enabled || in.l3;
            if (!y_square_ && in.gameplay && phase_ == Phase::Idle &&
                (!in.body || !in.snapshot_fresh)) {
                y_square_ = true;
                out.Log(!in.body ? "Y = Square (the body is not driven)"
                                 : "Y = Square (no quick-item state)");
            }
        }
        if (!in.y) {
            y_square_ = false;
        }
        if (y_square_ && in.y) {
            out.square_hold = true;
            if (y_press && phase_ != Phase::Idle && phase_ != Phase::SquareWait) {
                Reset(out, "L3 + Y");
            }
        }

        if (!in.gameplay || !c.enabled) {
            if (phase_ != Phase::Idle) {
                Reset(out, in.gameplay ? "quick_arc off" : "context");
                l2_fresh_needed_ = l2_fresh_needed_ || in.trigger_latch; // the item's pull
            }
            // The trigger still held from an item's use stays the item's here too: a frame out of
            // gameplay (a hitch: the game not immersive for a frame) must not let the gun see it
            // as a new pull when gameplay comes back (the pistol fired after a drink at the mouth
            // with the trigger still held, the 2026-10-06 use-sound mock runs).
            out.block_l2 = l2_fresh_needed_ || regrab_l2_wait_;
            Publish(out);
            return;
        }
        if (in.snapshot_fresh && !in.snapshot.alive && phase_ != Phase::Idle) {
            Reset(out, "dead");
        }

        if (holster_held_ && !in.hand_valid &&
            (phase_ == Phase::Holding || phase_ == Phase::Committing)) {
            Reset(out, "pouch item tracking lost");
            out.block_l2 = l2_fresh_needed_;
            Publish(out);
            return; // never throw using a stale velocity after losing the controller
        }
        const bool free_hand = phase_ == Phase::Idle || phase_ == Phase::Open;
        if (c.holster_mode && in.holster_entered && c.holster_haptics && free_hand && in.hand_valid &&
            in.body && !in.vial_busy && !in.combat && in.arm_weight >= c.arm_min) {
            out.Haptics(Haptic::Detent);
        }
        if (c.holster_mode && grip_edge && in.holster_assignment > 0 && free_hand) {
            grip_edge = false; // an empty/unavailable case never opens the menu
            regrab_slot_ = -1;
            if (!holster_grip_ready_ || !in.body || !in.hand_valid || !in.snapshot_fresh ||
                !in.snapshot.alive || in.snapshot.busy || in.vial_busy || in.combat || in.two_hand ||
                in.arm_weight < c.arm_min || in.trigger_latch || in.trigger_raw >= 0.45f) {
                out.Haptics(Haptic::Deny);
            } else if (in.holster_assignment == 1 && in.carried_vials == 0) {
                out.Haptics(Haptic::Empty);
            } else if (in.holster_assignment == 1) {
                out.draw_vial = true;
                out.vial_grip_release = c.hold_release;
                if (phase_ == Phase::Open)
                    Close(out, "blood vial case");
            } else {
                const int slot = in.holster_assignment - 2;
                if (slot < 0 || slot >= SlotCount || in.snapshot.slot[slot].Empty()) {
                    out.Haptics(Haptic::Empty);
                } else if (!in.snapshot.slot[slot].usable) {
                    out.Haptics(Haptic::Deny);
                } else {
                    holster_held_ = true;
                    c.left_scheme = 1;
                    c.stack = false;
                    c.trigger_parry = false;
                    Grab(c, in, out, slot, "body pouch", "held by grip");
                }
            }
            holster_grip_ready_ = false;
            out.block_l2 = holster_held_ || l2_fresh_needed_;
            Publish(out);
            return; // no use or release in the same frame as the grab
        }
        // Slots replaces grip-to-menu; the Y/L3 menu remains an explicit fallback (with y_menu Y
        // is the game's menu: the grip stays the arc's opener).
        if (c.holster_mode == 1 && phase_ == Phase::Idle && !c.y_menu)
            grip_edge = false;

        switch (phase_) {
        case Phase::Idle:
            if (y_edge && !y_square_) {
                regrab_slot_ = -1;
                Open(c, in, out);
            } else if (scheme2) {
                UpdateIdle2(c, in, out, grip_edge);
            } else {
                UpdateRegrab(c, in, out);
            }
            break;
        case Phase::Open:
            if (scheme2) {
                UpdateOpen2(c, in, out, y_edge, grip_edge, l2_edge);
            } else {
                UpdateOpen(c, in, out, y_edge, grip_edge, l2_edge);
            }
            break;
        case Phase::Holding:
        case Phase::Committing:
            // left_scheme=2: the grip puts the item back on the arc (it stays open), with the hand
            // still (PutBack2: a clench during a throw is none); the trigger let go is the
            // release, through the same path as the grip's opening in scheme 1. (quick_use_crush:
            // on an item to crush the grip is the crush, never a put-back: CrushGate in HoldTick.
            // quick_use_stab: on a blood to stab it is the needle's fist, as with the Blood Vial
            // (a clench before the stab, the hand still, never puts it back: let go of the
            // trigger for that).)
            if (scheme2 &&
                PutBack2(c, in, out, grip_edge && gesture_ != GestureKind::Crush && !stab_item_,
                                    trigger_let_go)) {
                break;
            }
            // (left_scheme=2: the trigger holds the item, its pull is no use: an item to eat or
            // drink is used at the mouth.)
            UpdateHolding(c, in, out, y_edge, scheme2 ? trigger_let_go : grip_release,
                          scheme2 ? false : trigger_edge, l2_edge);
            break;
        case Phase::Waiting:
            UpdateWaiting(c, in, out);
            break;
        case Phase::SquareWait:
            UpdateSquare(c, in, out);
            break;
        }

        // The gun: no L2 while the arc is open (until a pull is the gun's: UpdateOpen closes it)
        // or an item is in the hand, being used or waiting for the game's own use (the trigger
        // is the use), and after an item action not until the trigger was let go. A thrown item
        // has left the hand: the gun is free while its answer is pending. While the stack's next
        // one may be taken, a pull with the grip already moving waits up to fist_s for its
        // squeeze (a whole-hand squeeze takes the next item, it does not fire).
        const bool item_phase = phase_ != Phase::Idle && !(phase_ == Phase::Waiting && thrown_);
        if (l2_edge && phase_ == Phase::Idle && regrab_slot_ >= 0) {
            regrab_l2_t_ = in.t;
            regrab_l2_wait_ = in.grip >= c.fist_grip;
        }
        if (regrab_l2_wait_ && (phase_ != Phase::Idle || regrab_slot_ < 0 || !in.trigger_latch ||
                                in.grip < c.fist_grip || in.t - regrab_l2_t_ >= c.fist_s)) {
            regrab_l2_wait_ = false; // once over, this pull is the gun's (never held back again)
        }
        out.block_l2 = (item_phase && !panic_l2_) || l2_fresh_needed_ || regrab_l2_wait_;
        panic_l2_ = false;
        Publish(out);
    }

    [[nodiscard]] Phase GetPhase() const {
        return phase_;
    }
    [[nodiscard]] std::uint32_t OpenSerial() const {
        return open_serial_;
    }
    [[nodiscard]] std::int32_t HeldSlot() const {
        return held_slot_;
    }
    // A context switch (screen, menu, recenter, profile): close and put away silently; held
    // buttons are the caller's business.
    void Cancel(Output& out, const char* why) {
        if (phase_ != Phase::Idle) {
            Reset(out, why);
        }
    }

private:
    void Open(const Config& c, const Input& in, Output& out) {
        const int n = in.snapshot.Count();
        if (n == 0) {
            out.Haptics(Haptic::Empty);
            out.Log(std::string{toggle_by_} + ": no quick items equipped");
            return;
        }
        if (in.vial_armed) {
            out.disarm_vial = true; // the newest intent wins
        }
        cards_ = 0;
        for (int k = 0; k < SlotCount; ++k) {
            if (!in.snapshot.slot[static_cast<std::size_t>(k)].Empty()) {
                cards_ |= static_cast<std::uint8_t>(1u << k);
            }
        }
        ++open_serial_;
        Enter(Phase::Open);
        open_t_ = in.t;
        last_activity_ = in.t;
        open_hand_ = activity_hand_ = in.hand_p;
        // A grip held through the open must be released first; one squeezed with Y (just
        // before it) is a squeeze now (a quick draw).
        grip_armed_ = !grip_on_;
        // left_scheme=2: the grip is no grab (the squeeze that opened the arc is just that).
        squeeze_ = c.left_scheme != 2 && grip_on_ && in.t - grip_edge_t_ <= c.grip_lead_s;
        squeeze_t_ = in.t;
        squeeze_waited_ = false;
        hover_slot_ = -1;
        hovered_any_ = false;
        parry_t_ = -1.0; // a trigger already down is not a pull of this open
        pull_slot_ = -1;
        last_dwelled_ = false;
        reopen_until_ = 0.0;
        out.Haptics(Haptic::Unfurl);
        out.Log("OPEN n=" + std::to_string(n) + " sel=" + std::to_string(in.snapshot.selected) +
                (toggle_by_[0] == 'Y' ? "" : std::string{" by "} + toggle_by_));
    }

    // What a squeeze saw, for the log: `+<ms since the open> ms | hover <slot>, nearest <slot> m
    // <metric> (2nd <metric>), hand <cm> from the pivot`.
    std::string SqueezeDetail(const Input& in, std::int32_t hovered) const {
        char text[160];
        const int ms = static_cast<int>(std::lround((squeeze_t_ - open_t_) * 1000.0));
        if (!in.hover_fresh || in.hover.open_serial != open_serial_ || !in.hover.placed) {
            std::snprintf(text, sizeof(text), "+%d ms | hover %d (no fresh hover)", ms, hovered);
            return text;
        }
        const Nearest n = NearestOf(in.hover);
        const V3 hand{in.hover.hand[0], in.hover.hand[1], in.hover.hand[2]};
        const V3 pivot{in.hover.centre[0], in.hover.centre[1], in.hover.centre[2]};
        std::snprintf(text, sizeof(text),
                      "+%d ms | hover %d, nearest %d m %.2f (2nd %.2f), hand %.1f cm from the "
                      "pivot",
                      ms, hovered, n.slot, n.metric, n.second < 1e8f ? n.second : 0.0f,
                      100.0f * Length(hand - pivot));
        return text;
    }

    // The gun from the open arc (no item in the hand, the gun in it): true once a fresh pull of
    // the trigger (its latch rose in this phase) is the gun's, at the latch like any shot. A pull
    // within fist_s of the grip's squeeze or with the grip squeezed is the fist (a whole-hand
    // squeeze grabs): never the gun, and the trigger must be let go first. A grip already moving
    // at the latch (fist_grip) holds the shot up to fist_s: squeezed meanwhile it is the fist,
    // else the shot goes once the grip settles back or fist_s is over.
    bool GunPull(const Config& c, const Input& in, bool l2_edge, Output& out) {
        if (l2_edge) {
            parry_t_ = in.t;
            parry_fist_ = in.t - grip_edge_t_ <= c.fist_s || grip_on_;
            if (parry_fist_) {
                out.Log("trigger with the grip (a fist): not the gun");
            }
        }
        if (!in.trigger_latch || parry_t_ < 0.0) {
            parry_t_ = -1.0; // let go, or held since before this phase: not a fresh pull
            return false;
        }
        if (!parry_fist_ && grip_on_) {
            parry_fist_ = true;
            out.Log("trigger, then the grip (a fist): not the gun");
        }
        if (parry_fist_ || (in.grip >= c.fist_grip && in.t - parry_t_ < c.fist_s)) {
            return false;
        }
        parry_t_ = -1.0;
        return true;
    }

    // The opt-in reflex parry with a throwable in hand (quick_arc_trigger_parry): true once a
    // fresh pull of the trigger alone is parry_press deep. A pull within fist_s of the grip's
    // squeeze or while the hand moves at throwing speed (a brushed trigger mid-throw) is the fist:
    // never the gun, and the trigger must be let go first.
    bool ParryPull(const Config& c, const Input& in, bool l2_edge, Output& out) {
        if (l2_edge) {
            parry_t_ = in.t;
            parry_fist_ = in.t - grip_edge_t_ <= c.fist_s;
        }
        if (!in.trigger_latch || parry_t_ < 0.0) {
            parry_t_ = -1.0; // let go, or held since before this phase: not a fresh pull
            return false;
        }
        const bool fast = in.hand_valid && Length(in.hand_v) >= c.throw_min_speed;
        if (!parry_fist_ && fast) {
            parry_fist_ = true;
            out.Log("trigger while the hand moves fast: not the gun");
        }
        if (parry_fist_ || in.trigger < c.parry_press) {
            return false;
        }
        parry_t_ = -1.0;
        return true;
    }

    void UpdateOpen(const Config& c, const Input& in, Output& out, bool y_edge, bool grip_edge,
                    bool l2_edge) {
        if (!grip_on_) {
            grip_armed_ = true;
        }
        // Only your own hand hovers and takes: never the game's animation sweeping the arm
        // through the cards (a dodge, a hit), nor the hand as the arc unfolds around it.
        const bool arm = in.arm_weight >= c.arm_min;
        const bool live = in.hover_fresh && in.hover.open_serial == open_serial_ && in.hover.placed;
        // The arc unfolds from its placement (later than the open when it waited for the arm).
        const double placed_t =
            live && in.hover.placed_ns > 0
                ? std::max(open_t_, static_cast<double>(in.hover.placed_ns) * 1e-9)
                : open_t_;
        const bool settled = in.t - placed_t >= c.hover_delay_s;
        const std::int32_t hovered = live && arm && settled ? in.hover.slot : -1;
        if (hovered != hover_slot_) {
            if (hovered >= 0) {
                out.Haptics(hovered_any_ ? Haptic::Detent : Haptic::FirstDetent);
                hovered_any_ = true;
            }
            hover_slot_ = hovered;
            hover_lost_t_ = in.t;
        }
        // Reading a card or moving the hand keeps the arc open.
        if (hovered >= 0) {
            last_activity_ = in.t;
        }
        if (in.hand_valid && Length(in.hand_p - activity_hand_) > 0.03f) {
            activity_hand_ = in.hand_p;
            last_activity_ = in.t;
        }
        if (y_edge) {
            Close(out, toggle_by_);
            return;
        }
        if (in.x_edge) {
            Close(out, "X (the vial)");
            return;
        }
        if (GunPull(c, in, l2_edge, out)) {
            // Combat first: the gun in the empty hand fires at the pull and the arc closes in the
            // same frame.
            panic_l2_ = true;
            Close(out, "trigger (gun)");
            return;
        }
        // A squeeze: a fresh edge of the grip (one held through the open was let go first), or
        // one the gates still hold up (it waits while the grip stays squeezed).
        if (grip_edge && grip_armed_) {
            squeeze_ = true;
            squeeze_t_ = in.t;
            squeeze_waited_ = false;
        }
        if (squeeze_ && !grip_on_) {
            squeeze_ = false; // let go while it waited: no grab
            out.Log("grip let go while it waited");
        }
        if (squeeze_) {
            std::int32_t slot = -1;
            const char* via = "";
            if (arm) {
                // The card the hand is on; else the one it left within late_hover_s (the
                // squeeze's own pull); else the one clearly nearest it.
                if (hovered >= 0) {
                    slot = hovered;
                    via = "hover";
                } else if (last_hover_ >= 0 && in.t - hover_lost_t_ <= c.late_hover_s) {
                    slot = last_hover_;
                    via = "late hover";
                } else if (c.nearest_max > 0.0f && live &&
                           in.t - placed_t >= std::max(c.hover_delay_s, c.nearest_after_s)) {
                    const Nearest n = NearestOf(in.hover);
                    if (n.slot >= 0 && n.metric <= c.nearest_max &&
                        n.second >= n.metric + c.nearest_margin) {
                        slot = n.slot;
                        via = "nearest";
                    }
                }
                // Y and the grip at once, the hand still where it opened: the game's selected
                // item.
                if (slot < 0 && c.quick_draw && squeeze_t_ - open_t_ <= c.quick_draw_s &&
                    Length(in.hand_p - open_hand_) <= c.quick_draw_dist_m &&
                    in.snapshot.selected >= 0) {
                    slot = in.snapshot.selected;
                    via = "quick draw";
                }
            }
            const bool have = slot >= 0 && slot < SlotCount &&
                              !in.snapshot.slot[static_cast<std::size_t>(slot)].Empty();
            const char* gate = !arm      ? "the game has the arm"
                               : !live   ? "no fresh hover"
                               : !settled ? "the arc is unfolding"
                                          : nullptr;
            if (have) {
                squeeze_ = false;
                Grab(c, in, out, slot, via, SqueezeDetail(in, hovered));
                return;
            }
            if (gate != nullptr && in.t - squeeze_t_ < c.grab_wait_s) {
                if (!squeeze_waited_) {
                    squeeze_waited_ = true;
                    out.Log(std::string{"grip waits ("} + gate + ")");
                }
            } else {
                squeeze_ = false;
                out.Haptics(Haptic::Empty);
                out.Log(std::string{"grip on no card"} + (gate != nullptr ? " (" : "") +
                        (gate != nullptr ? gate : "") + (gate != nullptr ? ")" : "") + " " +
                        SqueezeDetail(in, hovered));
            }
        }
        if (hovered >= 0) {
            last_hover_ = hovered;
        }
        if (c.idle_close_s > 0.0f && in.t - last_activity_ >= c.idle_close_s) {
            Close(out, "idle");
        }
    }

    // `feel`: the grab's haptics (left_scheme=2's use at the pull plays its own light tick).
    void Grab(const Config& c, const Input& in, Output& out, std::int32_t slot, const char* via,
              const std::string& detail = {}, bool feel = true) {
        const Slot& s = in.snapshot.slot[static_cast<std::size_t>(slot)];
        held_slot_ = slot;
        held_goods_ = s.goods;
        held_last_ = s.qty <= 1;
        held_ffx_ = s.ffx;
        held_class_ = Classify(s.goods);
        ++grabs_;
        implausible_logged_ = 0;
        suspended_ = 0.0f;
        release_pending_ = false;
        putback_pending_ = false;
        regrab_slot_ = -1;
        // quick_use_mouth: an item to eat or drink waits in the hand for the mouth.
        mouth_item_ = MouthItem(c, s.goods);
        mouth_use_ = false;
        mouth_gate_.Reset(in.t);
        // quick_use_wipe: a blade-wipe item waits in the hand for the blade.
        wipe_item_ = WipeItem(c, s.goods);
        wipe_use_ = false;
        wipe_blade_ = false;
        wipe_cue_logged_ = false;
        wipe_moves_logged_ = 0;
        rub_touch_refused_ = rub_refused_logged_ = 0;
        rub_refused_log_t_ = rub_near_since_ = rub_near_log_t_ = -1.0;
        wipe_.Reset(in.t);
        // quick_hold_use: a misc item's ring starts filling now (the button that took it is
        // held).
        // quick_music_box_crank: the Tiny Music Box waits in the hand for its crank (no ring).
        crank_item_ = CrankItem(c, s.goods);
        crank_serial_ = 0;
        crank_played_ = false;
        hold_item_ = !mouth_item_ && !crank_item_ && HoldItem(c, s.goods);
        hold_use_ = hold_spent_ = false;
        hold_from_ = hold_from_base_ = in.t;
        hold_from_ns_ = hold_from_base_ns_ = steady_ns_;
        hold_len_ = std::max(c.hold_use_s, 0.1f);
        hold_lost_ = 0.0;
        hold_lost_frames_ = 0;
        // quick_use_crush / quick_use_shake: the held item's faster way (the hold its fallback).
        gesture_ = hold_item_ ? GestureFor(c, s.goods) : GestureKind::None;
        gesture_pending_ = false;
        crush_.Reset(in.t);
        shake_.Reset(in.t);
        shake_shown_ = 0;
        // quick_use_stab: a blood waits in the hand for a stab (no ring).
        stab_item_ = !mouth_item_ && StabItem(c, s.goods);
        stab_use_ = false;
        stab_pending_t_ = -1.0;
        windup_.Clear(); // the throw's wind-up starts in the hand
        parry_t_ = -1.0; // the squeeze's own trigger is the fist
        Enter(Phase::Holding);
        pulse(slot, false);
        if (feel) {
            out.Haptics(Haptic::Grab);
        }
        if (s.qty == 1) {
            out.Haptics(Haptic::LastOne);
        }
        if (c.select && in.snapshot.selected != slot) {
            Send(out, slot, Action::None, in);
        }
        out.Log("GRAB k=" + std::to_string(slot) + " goods=" + std::to_string(s.goods) +
                " qty=" + std::to_string(s.qty) + " via=" + via +
                (detail.empty() ? "" : " " + detail));
        if (mouth_item_ && in.mouth_valid) {
            // Where the mouth is (the hand to bring there; headset tuning).
            char text[160];
            std::snprintf(text, sizeof(text),
                          "to the mouth (%s): the mouth at (%.3f,%.3f,%.3f) LOCAL, the hand "
                          "(%.3f,%.3f,%.3f) %.0f cm from it",
                          MouthName(MouthOf(s.goods)), in.mouth.x, in.mouth.y, in.mouth.z,
                          in.hand_p.x, in.hand_p.y, in.hand_p.z,
                          100.0f * Length(in.hand_p - in.mouth));
            out.Log(text);
        }
        if (wipe_item_) {
            out.Log("WIPE it along the blade to apply it (let go away from the blade: back)");
        }
        if (hold_item_ && s.goods == QueenlyBlood) {
            out.Log(c.left_scheme == 2 ? "SQUEEZE the grip to use Queenly Blood; holding alone never uses it"
                                       : "SQUEEZE the trigger to use Queenly Blood; holding alone never uses it");
        } else if (hold_item_) {
            char text[200];
            std::snprintf(text, sizeof(text),
                          "HOLD to use (%s): the ring fills over %.2f s, let go to put it back%s",
                          HoldName(HoldKindFor(c, s.goods)), hold_len_,
                          gesture_ == GestureKind::Crush
                              ? (c.left_scheme == 2 ? "; or CRUSH it: squeeze the grip hard"
                                                    : "; or CRUSH it: squeeze the trigger hard")
                          : gesture_ == GestureKind::Shake ? "; or SHAKE it: three strokes"
                                                           : "");
            out.Log(text);
        }
        if (stab_item_) {
            out.Log("STAB it into your wrist or body to inject it (let go away from them: back)");
        }
        if (crank_item_) {
            out.Log("MUSIC BOX in the left hand: the right hand is free; take the crank with the "
                    "right grip and turn it to play (let go to put it back)");
        }
    }

    void UpdateHolding(const Config& c, const Input& in, Output& out, bool y_edge,
                       bool grip_release, bool trigger_edge, bool l2_edge) {
        // The item is still in that slot with at least one left.
        if (in.snapshot_fresh) {
            const Slot& s = in.snapshot.slot[static_cast<std::size_t>(held_slot_)];
            if (s.goods != held_goods_ || s.qty <= 0) {
                PutAway(out, s.goods != held_goods_ ? "the slot changed" : "none left", false);
                return;
            }
        }
        // A throw waiting for the hand's peak (throw_rise_wait_s): it goes once the hand stops
        // speeding up or the wait is over, whatever else happens meanwhile (a few frames).
        if (release_pending_) {
            if (in.t - release_t_ >= c.throw_rise_wait_s - 1e-4 ||
                !ReleaseHistory(c, in.t).Rising(in.t)) {
                release_pending_ = false;
                Release(c, in, out);
            }
            return;
        }
        if (y_edge) {
            PutAway(out, toggle_by_, true);
            return;
        }
        if (in.x_edge) {
            PutAway(out, "X (the vial)", false);
            return;
        }
        // The game owns the arm (a dodge, a hit, a guard): the item stays in his animated hand;
        // uses and throws wait; too long and it goes back.
        const bool arm = in.arm_weight >= 0.9f;
        suspended_ = arm ? 0.0f : suspended_ + in.dt;
        if (suspended_ >= c.suspend_s) {
            PutAway(out, "the game kept the arm", true);
            return;
        }
        const bool throwable = held_class_.use == UseKind::Throw && c.throws;
        if (throwable) {
            // The trigger is not the gun while it is in the hand (opt-in: the reflex parry; never
            // in left_scheme=2, where the trigger holds the item and `grip_release` is its
            // letting go).
            if (c.trigger_parry && c.left_scheme != 2 && ParryPull(c, in, l2_edge, out)) {
                panic_l2_ = true;
                PutAway(out, "trigger (gun)", false);
                return;
            }
            if (grip_release) {
                release_t_ = in.t;
                release_arm_ = arm;
                // The grip opened while the hand still speeds up (and already fast enough to
                // throw): the item stays in the hand a frame or two more, until its peak (Valve's
                // rule; never turns a put-back into a throw).
                const VelocityHistory& h = ReleaseHistory(c, in.t);
                if (c.throw_rise_wait_s > 0.0f && arm && !latched(held_goods_) &&
                    Length(h.Release(in.t)) >= c.throw_min_speed && h.Rising(in.t)) {
                    release_pending_ = true;
                    return;
                }
                Release(c, in, out);
            }
            return;
        }
        // A usable: the grip holds it, the trigger uses it (quick_use_mouth: an item to eat or
        // drink also at the mouth; left_scheme=2: only there, the trigger holds it).
        if (phase_ == Phase::Holding) {
            if (grip_release) {
                // quick_hold_use: let go of before the ring filled, nothing used.
                // (quick_use_wipe: let go of on the blade or away from it, nothing used.)
                const std::string early =
                    hold_item_ && !hold_spent_ ? " before the ring filled (" + HoldText(in) + ")"
                    : wipe_item_ ? (wipe_.Touching() ? " on the blade, the stroke unfinished"
                                                     : " away from the blade")
                    : stab_item_  ? " without a stab"
                    : crank_item_ ? " (the music box)"
                                  : std::string{};
                if (c.left_scheme == 2 && CrankPlayed()) {
                    // The music box let go after it played: used, so put away with the arc
                    // closed, as after any use (2026-10-15: the arc coming back after
                    // the music box felt weird; let go unplayed, it is a cancel: back below).
                    PutAway(out, "trigger let go: the music box played", true);
                } else if (c.left_scheme == 2) {
                    // The trigger that took it let go away from the mouth: back on the arc.
                    ToArc(in, out,
                          mouth_item_ ? "trigger let go away from the mouth: back"
                                      : "trigger let go" + early + ": back",
                          0.0, true);
                } else {
                    PutAway(out,
                            mouth_item_ ? "grip released away from the mouth"
                                        : "grip released" + early,
                            true);
                }
                return;
            }
            if (mouth_item_ && AtMouth(c, in, out, arm)) {
                return;
            }
            // quick_use_wipe: applied by a stroke along the blade (left_scheme=1: the trigger's
            // deliberate pull below still uses it, as an item to eat).
            if (wipe_item_ && AtBlade(c, in, out, arm)) {
                return;
            }
            if (stab_item_) {
                // quick_use_stab: injected by a stab (left_scheme=1: the trigger does nothing
                // meanwhile: no use, no gun).
                AtStab(c, in, out, arm);
                return;
            }
            if (crank_item_) {
                // quick_music_box_crank: played by the crank in the right hand (the trigger,
                // left_scheme=1, does nothing meanwhile: no use, no gun).
                CrankTick(c, in, out);
                return;
            }
            if (hold_item_) {
                // quick_hold_use: the ring fills while the button stays held; the trigger
                // (left_scheme=1) does nothing meanwhile (no use, no gun).
                HoldTick(c, in, out, arm);
                return;
            }
            if (trigger_edge) {
                if (!arm) {
                    Deny(out, "the game has the arm");
                } else if (in.t - last_use_ < c.use_cooldown_s) {
                    Deny(out, "cooldown");
                } else if (in.snapshot_fresh &&
                           !in.snapshot.slot[static_cast<std::size_t>(held_slot_)].usable) {
                    Deny(out, "the game says it cannot be used now");
                } else {
                    Enter(Phase::Committing);
                    commit_t_ = in.t;
                    out.Haptics(Haptic::Squeeze);
                }
            }
            return;
        }
        // Committing: a use is a deliberate pull, held use_commit_s. A shorter tap does nothing:
        // the item stays in the hand and the gun never hears it (opt-in quick_arc_trigger_parry:
        // the item goes away and the gun gets one L2 press; the tap itself never reached the
        // game).
        if (grip_release) {
            PutAway(out, "grip released during the use", true);
            return;
        }
        if (!trigger_on_) {
            if (c.trigger_parry) {
                out.l2_press = true;
                PutAway(out, "trigger tap (gun)", false);
            } else {
                Enter(Phase::Holding);
                out.Log("use cancelled (trigger tap)");
            }
            return;
        }
        if (in.t - commit_t_ < c.use_commit_s) {
            return;
        }
        last_use_ = in.t;
        const bool instant = c.instant && (held_class_.use == UseKind::Instant ||
                                           held_class_.use == UseKind::Bullet);
        if (instant && !latched(held_goods_)) {
            Send(out, c.select && in.snapshot.selected != held_slot_ ? held_slot_ : -1,
                 Action::Use, in);
            Enter(Phase::Waiting);
            wait_t_ = in.t;
            out.Log("USE goods=" + std::to_string(held_goods_) + " (instant)");
        } else {
            StartSquare(c, in, out);
        }
    }

    // quick_use_mouth: an item to eat or drink (MouthOf) used through the game's own UseGoods
    // (never a throwable, never one latched to the game's animation).
    [[nodiscard]] bool MouthItem(const Config& c, std::int32_t goods) const {
        const UseKind u = Classify(goods).use;
        return c.mouth && c.instant && MouthOf(goods) != MouthKind::None &&
               (u == UseKind::Instant || u == UseKind::Bullet) && !latched(goods);
    }

    // The item to eat or drink brought to the mouth (MouthGate): used there, at once (the game's
    // own use, no animation). A use within the cooldown waits at the mouth for it; one the game
    // says cannot be used now is refused (the deny tick; it stays in the hand, and the hand has
    // to leave the mouth and come back). True: handled this frame.
    bool AtMouth(const Config& c, const Input& in, Output& out, bool arm) {
        if (!in.mouth_valid) {
            return false;
        }
        const bool at = mouth_gate_.Update(c.mouth_cfg, in.t, in.hand_valid, in.hand_p,
                                           in.hand_v - in.head_v, in.mouth, arm);
        if (!at || in.t - last_use_ < c.use_cooldown_s) {
            return false;
        }
        const MouthKind kind = MouthOf(held_goods_);
        char detail[200];
        std::snprintf(detail, sizeof(detail),
                      "the hand %.1f cm from the mouth, %.2f m/s, there %.0f ms, held %.2f s",
                      100.0f * mouth_gate_.Distance(), mouth_gate_.Speed(),
                      mouth_gate_.Dwell() * 1000.0, in.t - mouth_gate_.HeldSince());
        mouth_gate_.Consume();
        if (in.snapshot_fresh && !in.snapshot.slot[static_cast<std::size_t>(held_slot_)].usable) {
            Deny(out, std::string{"at the mouth: the game says it cannot be used now ("} + detail +
                          ")");
            return true;
        }
        last_use_ = in.t;
        Request& r = Send(out, c.select && in.snapshot.selected != held_slot_ ? held_slot_ : -1,
                          Action::Use, in);
        r.mouth = static_cast<std::uint8_t>(kind);
        mouth_use_ = true;
        Enter(Phase::Waiting);
        wait_t_ = in.t;
        out.Log("USE goods=" + std::to_string(held_goods_) + " at the mouth (" + MouthName(kind) +
                "): " + detail);
        return true;
    }

    // quick_use_wipe: a blade-wipe item (WipeOf) used through the game's own UseGoods (never one
    // latched to the game's animation).
    [[nodiscard]] bool WipeItem(const Config& c, std::int32_t goods) const {
        const UseKind u = Classify(goods).use;
        return c.wipe && c.instant && WipeOf(goods) &&
               (u == UseKind::Instant || u == UseKind::Bullet) && !latched(goods);
    }

    // The blade-wipe item against the right weapon's blade (WipeDetector on the newest contact
    // sample, Input::wipe): the rasp while it touches (Output::wipe_*), applied once a stroke along
    // the blade completes: the game's own use at once (no animation; its buff on the weapon), the
    // ignite thump when the game takes it. A stroke within the cooldown waits on the blade for it;
    // one the game says cannot be used now is refused (the deny tick; it stays in the hand, a new
    // stroke tries again). The two-handed grip on the handle: nothing until it lets go. True:
    // handled this frame.
    bool AtBlade(const Config& c, const Input& in, Output& out, bool arm) {
        // No blade to wipe it on (fists, a weapon whose hit capsule is not known, a whip's handle
        // too short, no sample from the hunter's model): once WipeNoBladeS after the take passed
        // without one, the paper is not left unusable in the hand (NoBlade).
        if (in.wipe_fresh && in.wipe.goods == held_goods_ && in.wipe.valid &&
            in.wipe.length >= c.wipe_cfg.min_length_m) {
            wipe_blade_ = true;
        }
        if (!wipe_blade_ && in.t - wipe_.HeldSince() >= WipeNoBladeS) {
            NoBlade(c, in, out);
            return false;
        }
        const bool was = wipe_.Touching();
        bool done = false;
        // A hitch of the game (no rendered frame, no sample) up to WipeGapS mid-stroke keeps the
        // stroke (its samples carry their own times): only the rasp pauses. (The 2026-10-06 feel
        // check: a 0.15 s gap ended it, and a mock eye dump's stall broke every stroke.)
        const double age = in.t - static_cast<double>(in.wipe.steady_ns) * 1e-9;
        const bool gap = !in.wipe_fresh && in.wipe.goods == held_goods_ && age <= WipeGapS;
        if (in.two_hand) {
            wipe_.End();
        } else if (gap) {
            // the stroke waits for the next sample
        } else if (!in.wipe_fresh || in.wipe.goods != held_goods_) {
            wipe_.End(); // no contact (stale, another item's): off the blade
        } else if (in.wipe.serial != wipe_serial_) {
            wipe_serial_ = in.wipe.serial;
            done = wipe_.Update(c.wipe_cfg, static_cast<double>(in.wipe.steady_ns) * 1e-9,
                                in.wipe, in.wipe.arms && arm);
        }
        // The find cue (the tester's 2026-10-08 "easier to find the spot where we need to rub"):
        // nearing the zone, before the touch (your own arms, not the two-handed grip).
        out.wipe_near = 0.0f;
        if (!wipe_.Touching() && !in.two_hand && in.wipe_fresh && in.wipe.goods == held_goods_ &&
            in.wipe.arms && arm) {
            out.wipe_near = WipeNear(c.wipe_cfg, in.wipe);
            if (out.wipe_near > 0.0f && !wipe_cue_logged_) {
                wipe_cue_logged_ = true;
                char text[200];
                if (c.wipe_cfg.rub) {
                    std::snprintf(text, sizeof(text),
                                  "WIPE cue: the paper nears the blade (its card %.1f cm off the "
                                  "blade's surface, %.0f cm along a %.0f cm blade; on within %.0f "
                                  "cm)",
                                  100.0f * WipeSurface(c.wipe_cfg, in.wipe),
                                  100.0f * WipeAlong(in.wipe), 100.0f * in.wipe.length,
                                  100.0f * c.wipe_cfg.touch_m);
                } else {
                    std::snprintf(text, sizeof(text),
                                  "WIPE cue: the paper nears the blade (%.1f cm from it, %.0f cm "
                                  "along a %.0f cm blade; the zone %.0f cm)",
                                  100.0f * in.wipe.dist, 100.0f * in.wipe.along,
                                  100.0f * in.wipe.length, 100.0f * c.wipe_cfg.reach_m);
                }
                out.Log(text);
            }
        }
        if (wipe_.BladeMoves() > 0 && wipe_moves_logged_ == 0) {
            // A stroke turned down: the blade moved along the paper, not the paper along it.
            wipe_moves_logged_ = wipe_.BladeMoves();
            out.Log("WIPE: the blade moved past the paper (a thrust or a swing, not a wipe): "
                    "nothing used");
        }
        if (c.wipe_cfg.rub) {
            RubLog(c, in, out, was, arm);
        } else if (wipe_.Touching() != was) {
            char text[160];
            if (wipe_.Touching()) {
                std::snprintf(text, sizeof(text),
                              "WIPE the paper on the blade (%.1f cm from it, %.0f cm along a %.0f "
                              "cm blade)",
                              100.0f * in.wipe.dist, 100.0f * in.wipe.along,
                              100.0f * in.wipe.length);
            } else {
                std::snprintf(text, sizeof(text), "WIPE off the blade (%s)",
                              in.two_hand        ? "the two-handed grip"
                              : !in.wipe_fresh   ? "no contact"
                              : !in.wipe.arms    ? "the game has an arm"
                                                 : "lifted");
            }
            out.Log(text);
        }
        out.wipe_touch = wipe_.Touching() && !gap;
        out.wipe_progress = wipe_.Progress();
        out.wipe_teeth = wipe_.TakeTeeth();
        if (!done || in.t - last_use_ < c.use_cooldown_s) {
            return false;
        }
        char detail[260];
        if (c.wipe_cfg.rub && wipe_.DoneKind() == 2) {
            std::snprintf(detail, sizeof(detail),
                          "rubbed %.0f of %.0f cm %s (%d stroke%s) along a %.0f cm blade in %.0f "
                          "ms, its card %.1f cm off the blade's surface, held %.2f s",
                          100.0f * wipe_.RubTotal(), 100.0f * c.wipe_cfg.rub_m,
                          wipe_.RubStrokes() > 1 ? "back and forth" : "one way",
                          wipe_.RubStrokes(), wipe_.RubStrokes() > 1 ? "s" : "",
                          100.0f * wipe_.BladeLength(),
                          wipe_.RubSpanS() * 1000.0, 100.0f * wipe_.Distance(),
                          in.t - wipe_.HeldSince());
        } else {
            std::snprintf(detail, sizeof(detail),
                          "%s%.0f of %.0f cm along a %.0f cm blade in %.0f ms, %.1f cm %s, held "
                          "%.2f s, the paper's own move %.0f%%",
                          c.wipe_cfg.rub ? "one stroke " : "", 100.0f * wipe_.Travel(),
                          100.0f * wipe_.Required(), 100.0f * wipe_.BladeLength(),
                          wipe_.StrokeS() * 1000.0, 100.0f * wipe_.Distance(),
                          c.wipe_cfg.rub ? "off the blade's surface" : "from it",
                          in.t - wipe_.HeldSince(), 100.0f * std::max(wipe_.OwnShare(), 0.0f));
        }
        wipe_.Consume();
        out.wipe_touch = false;
        out.wipe_progress = 0.0f;
        if (in.snapshot_fresh && !in.snapshot.slot[static_cast<std::size_t>(held_slot_)].usable) {
            Deny(out, std::string{"on the blade: the game says it cannot be used now ("} + detail +
                          ")");
            return true;
        }
        last_use_ = in.t;
        Request& r = Send(out, c.select && in.snapshot.selected != held_slot_ ? held_slot_ : -1,
                          Action::Use, in);
        r.wipe = 1;
        wipe_use_ = true;
        Enter(Phase::Waiting);
        wait_t_ = in.t;
        out.Log("USE goods=" + std::to_string(held_goods_) + " wiped along the blade: " + detail);
        return true;
    }

    // quick_use_wipe's rub (WipeConfig::rub): what the log says, so a session shows what failed
    // (the tester's 2026-10-08 "sometimes it doesn't trigger even when I am doing it right"): the
    // card on the blade, the contact lost and why (off the blade by how much, past its tip or
    // base, the two-handed grip, the game's arm) with what was rubbed by then; a stroke not
    // counted and why (across, a slash, the blade moving or turning, the blade moving, not the
    // paper; at most one line every 0.5 s, the rest counted into the next); and near the blade but
    // not on it for half a second (at most every 2 s).
    void RubLog(const Config& c, const Input& in, Output& out, bool was, bool arm) {
        const WipeConfig& w = c.wipe_cfg;
        const bool sample = in.wipe_fresh && in.wipe.goods == held_goods_ && in.wipe.valid;
        const float surf = sample ? WipeSurface(w, in.wipe) : 1e9f;
        const float along = sample ? WipeAlong(in.wipe) : 0.0f;
        char text[300];
        if (wipe_.Touching() && !was) {
            rub_touch_t_ = in.t;
            rub_touch_refused_ = wipe_.Refused();
            rub_near_since_ = -1.0;
            std::snprintf(text, sizeof(text),
                          "WIPE the paper on the blade (its card %.1f cm off the blade's surface, "
                          "%.0f cm along a %.0f cm blade; the fist %.1f cm from its axis)",
                          100.0f * surf, 100.0f * along, 100.0f * in.wipe.length,
                          100.0f * in.wipe.dist);
            out.Log(text);
        } else if (!wipe_.Touching() && was) {
            char why[120];
            if (in.two_hand) {
                std::snprintf(why, sizeof(why), "the two-handed grip");
            } else if (!in.wipe_fresh || in.wipe.goods != held_goods_) {
                std::snprintf(why, sizeof(why), "no sample of the paper");
            } else if (!in.wipe.arms || !arm) {
                std::snprintf(why, sizeof(why), "the game has an arm");
            } else if (along > in.wipe.length + w.end_margin_m) {
                std::snprintf(why, sizeof(why), "past the tip by %.0f cm",
                              100.0f * (along - in.wipe.length));
            } else if (along < -w.end_margin_m) {
                std::snprintf(why, sizeof(why), "below the blade's base by %.0f cm",
                              -100.0f * along);
            } else {
                std::snprintf(why, sizeof(why), "lifted: off the blade by %.1f cm", 100.0f * surf);
            }
            const int refused = wipe_.Refused() - rub_touch_refused_;
            std::snprintf(text, sizeof(text),
                          "rub: contact lost (%s) after %.2f s: %.0f of %.0f cm rubbed within "
                          "%.1f s (%d strokes counted%s%s%s)",
                          why, in.t - rub_touch_t_, 100.0f * wipe_.RubTotal(), 100.0f * w.rub_m,
                          w.rub_window_s, wipe_.RubStrokes(), refused > 0 ? ", " : "",
                          refused > 0 ? std::to_string(refused).c_str() : "",
                          refused > 0 ? " not" : "");
            out.Log(text);
        }
        // A stroke not counted (the newest; the ones in between counted into the line).
        if (wipe_.Refused() > rub_refused_logged_) {
            if (rub_refused_log_t_ < 0.0 || in.t - rub_refused_log_t_ >= 0.5) {
                const int more = wipe_.Refused() - rub_refused_logged_ - 1;
                const RubWhy y = wipe_.LastWhy();
                char value[60] = "";
                if (y == RubWhy::Across) {
                    std::snprintf(value, sizeof(value), ": %.1f cm across it",
                                  100.0f * wipe_.LastValue());
                } else if (y == RubWhy::Slash || y == RubWhy::BladeMoving) {
                    std::snprintf(value, sizeof(value), ": %.2f m/s", wipe_.LastValue());
                } else if (y == RubWhy::BladeTurned) {
                    std::snprintf(value, sizeof(value), ": %.0f deg", wipe_.LastValue());
                } else if (y == RubWhy::NotOwn) {
                    std::snprintf(value, sizeof(value), ": the paper's own move %.0f%%",
                                  100.0f * wipe_.LastValue());
                }
                std::snprintf(text, sizeof(text), "rub: a stroke not counted (%.0f cm: %s%s)%s",
                              100.0f * wipe_.LastLength(), RubWhyName(y), value,
                              more > 0 ? (" (+" + std::to_string(more) + " more)").c_str() : "");
                out.Log(text);
                rub_refused_logged_ = wipe_.Refused();
                rub_refused_log_t_ = in.t;
            }
        }
        // Near the blade, not on it.
        // (Not `near`: a macro in the Windows headers.)
        const bool by_it = !wipe_.Touching() && sample && in.wipe.arms && arm && !in.two_hand &&
                           in.wipe.length >= w.min_length_m && surf <= w.touch_m + w.near_log_m;
        if (!by_it) {
            rub_near_since_ = -1.0;
        } else if (rub_near_since_ < 0.0) {
            rub_near_since_ = in.t;
        } else if (in.t - rub_near_since_ >= 0.5 &&
                   (rub_near_log_t_ < 0.0 || in.t - rub_near_log_t_ >= 2.0)) {
            rub_near_log_t_ = in.t;
            char where[80] = "";
            if (along > in.wipe.length + w.end_margin_m) {
                std::snprintf(where, sizeof(where), ", past the tip by %.0f cm",
                              100.0f * (along - in.wipe.length));
            } else if (along < -w.end_margin_m) {
                std::snprintf(where, sizeof(where), ", below the blade's base by %.0f cm",
                              -100.0f * along);
            }
            std::snprintf(text, sizeof(text),
                          "rub: near the blade, not on it for %.1f s (off the blade by %.1f cm, on "
                          "within %.0f; %.0f cm along a %.0f cm blade%s)",
                          in.t - rub_near_since_, 100.0f * surf, 100.0f * w.touch_m,
                          100.0f * along, 100.0f * in.wipe.length, where);
            out.Log(text);
        }
    }

    // quick_use_wipe with no blade to wipe on (AtBlade): the paper is used as with quick_use_wipe=0
    // instead: the hold ring (quick_hold_use; it starts filling now), else the trigger's
    // deliberate pull (left_scheme=1; left_scheme=2 without the ring: let go, back).
    void NoBlade(const Config& c, const Input& in, Output& out) {
        const char* why = !in.wipe_fresh || in.wipe.goods != held_goods_ ? "no sample of it"
                          : !in.wipe.valid                              ? "no blade known"
                                                                        : "the blade too short";
        wipe_item_ = false;
        wipe_.End();
        out.wipe_touch = false;
        out.wipe_progress = 0.0f;
        if (c.hold_use && HoldOf(held_goods_) != HoldKind::None) {
            hold_item_ = true;
            hold_use_ = hold_spent_ = false;
            hold_from_ = hold_from_base_ = in.t;
            hold_from_ns_ = hold_from_base_ns_ = steady_ns_;
            hold_len_ = std::max(c.hold_use_s, 0.1f);
            hold_lost_ = 0.0;
            hold_lost_frames_ = 0;
            char text[160];
            std::snprintf(text, sizeof(text),
                          "WIPE: nothing to wipe it on (%s): HOLD to use instead, the ring fills "
                          "over %.2f s",
                          why, hold_len_);
            out.Log(text);
            return;
        }
        out.Log(std::string{"WIPE: nothing to wipe it on ("} + why +
                (c.left_scheme == 2 ? "): let go, back" : "): the trigger's pull uses it"));
    }

    // quick_hold_use: a misc item used by holding it (HoldOf), never one the mouth or the paper
    // wipe takes (with quick_use_wipe off the papers and the shell are held instead), nor a
    // throwable.
    [[nodiscard]] bool HoldItem(const Config& c, std::int32_t goods) const {
        return (c.hold_use || goods == QueenlyBlood) && HoldKindFor(c, goods) != HoldKind::None && !MouthItem(c, goods) &&
               !WipeItem(c, goods) && !StabItem(c, goods) &&
               !(c.throws && Classify(goods).use == UseKind::Throw);
    }
    // HoldOf, and the Choir Bell where it is rung (quick_use_shake with shake_choir).
    [[nodiscard]] static HoldKind HoldKindFor(const Config& c, std::int32_t goods) {
        return goods == ChoirBell && c.shake && c.shake_choir ? HoldKind::Rite : HoldOf(goods);
    }
    // The held item's gesture where it is on (the Choir Bell only with shake_choir).
    [[nodiscard]] static GestureKind GestureFor(const Config& c, std::int32_t goods) {
        const GestureKind k = GestureOf(goods);
        if (k == GestureKind::Crush) {
            return c.crush ? k : GestureKind::None;
        }
        if (k == GestureKind::Shake) {
            return c.shake && (goods != ChoirBell || c.shake_choir) ? k : GestureKind::None;
        }
        return GestureKind::None;
    }
    // quick_use_stab: a blood injected by a stab (StabOf) through the game's own UseGoods (never
    // one latched to the game's animation).
    [[nodiscard]] bool StabItem(const Config& c, std::int32_t goods) const {
        return c.stab && c.instant && StabOf(goods) && Classify(goods).use == UseKind::Instant &&
               !latched(goods);
    }

    // "0.53 of 1.50 s" (the log): the time held (the shake's steps of the ring not counted).
    [[nodiscard]] std::string HoldText(const Input& in) const {
        char text[48];
        std::snprintf(text, sizeof(text), "%.2f of %.2f s",
                      std::clamp(in.t - hold_from_base_, 0.0, 99.0), hold_len_);
        return text;
    }

    // The held misc item while its button stays held: the ring fills (Output::hold_progress,
    // the rising hum); full, the item is used (the game's own use: UseGoods at once, or select +
    // Square for the marks and bells), with the thump and the ring's flash. The game taking the
    // arm for hold_arm_grace_s (a hit's stagger, a dodge, a guard) or a dodge / leap cancels it:
    // back (scheme 2: on the arc, closed: a fight; scheme 1: away) with the put-back tick,
    // nothing used. A use the cooldown or the game's hold on the arm holds up waits at full; one
    // the game refuses there is a deny (scheme 2: back on the arc; scheme 1: it stays in the hand,
    // spent, until let go).
    void HoldTick(const Config& c, const Input& in, Output& out, bool arm) {
        if (hold_spent_) {
            return;
        }
        // The game holding the arm: counted only while the body's state is fresh (Input::body;
        // after a hitch of a quarter second it reads 0 for a frame: no hit), each frame at most
        // 50 ms, and over at least 3 frames (a hit's stagger lasts hundreds of ms).
        if (arm) {
            hold_lost_ = 0.0;
            hold_lost_frames_ = 0;
        } else if (in.body) {
            hold_lost_ += std::clamp(static_cast<double>(in.dt), 0.0, 0.05);
            ++hold_lost_frames_;
        }
        const bool hit = hold_lost_ >= c.hold_arm_grace_s - 1e-6 && hold_lost_frames_ >= 3;
        if (hit || in.dodge) {
            out.Haptics(Haptic::PutBack);
            out.Log("HOLD cancelled (" +
                    std::string{in.dodge ? "a dodge or leap" : "the game took the arm: a hit, a "
                                                               "dodge or a guard"} +
                    ", " + HoldText(in) + "): back, nothing used");
            if (c.left_scheme == 2 && trigger_hold_) {
                Idle(out, true); // the arc stays closed: a fight
            } else {
                PutAway(out, "hold cancelled", false);
            }
            return;
        }
        // The time held (the fallback; the shake's steps of the ring never shorten it).
        const float progress = held_goods_ == QueenlyBlood ? 0.0f :
            static_cast<float>((in.t - hold_from_base_) / hold_len_);
        out.hold_hum = held_goods_ == QueenlyBlood ? 0.0f : HoldHum(progress);
        // quick_use_crush / quick_use_shake: the faster way, through the same completion.
        Haptic feel = Haptic::None;
        std::string how;
        if (gesture_pending_) {
            feel = pending_feel_;
            how = pending_how_;
        } else if (gesture_ == GestureKind::Crush) {
            CrushTick(c, in, out, arm, progress, feel, how);
        } else if (gesture_ == GestureKind::Shake) {
            ShakeTick(c, in, out, arm, feel, how);
        }
        out.hold_progress = held_goods_ == QueenlyBlood ? std::max(0.0f, crush_.Strain(c.crush_cfg, in.t)) :
            std::min(static_cast<float>((in.t - hold_from_) / hold_len_), 1.0f);
        if (held_goods_ == QueenlyBlood) ShowRing(in.t - out.hold_progress*hold_len_);
        // Match the hold and the crush's extra strain. Bell strokes keep their own sound.
        if (gesture_ != GestureKind::Shake && arm && in.hand_valid) {
            out.use_whoosh = std::clamp(std::max(progress, (out.hold_hum - 0.22f) / 0.48f),
                                      0.0f, 1.0f);
        }
        if (held_goods_ == QueenlyBlood && crush_.Pressure(c.crush_cfg) <= 0.0f &&
            crush_.Strain(c.crush_cfg,in.t) < 0.0f) out.use_whoosh = -1.0f;
        if (feel != Haptic::None) {
            if (!arm || in.t - last_use_ < c.use_cooldown_s) {
                // Within the cooldown (or the game holding the arm a moment): kept, the ring
                // full, done the first frame it may be (never dropped).
                if (!gesture_pending_) {
                    gesture_pending_ = true;
                    pending_feel_ = feel;
                    pending_how_ = how;
                    out.Log(how + ": waits for the " + (arm ? "cooldown" : "arm"));
                }
                ShowRing(in.t - hold_len_);
                out.hold_progress = 1.0f;
                return;
            }
            Complete(c, in, out, feel, how,
                     gesture_ == GestureKind::Crush ? "crushed" : "rung");
            return;
        }
        if (held_goods_ == QueenlyBlood) return; // No timed auto-use: deliberately squeeze.
        if (progress < 1.0f || !arm || in.t - last_use_ < c.use_cooldown_s) {
            // Filling; or full, waiting out the cooldown, or for the arm: the game holding it as
            // the ring fills is never used through (a hit's first frames; within
            // hold_arm_grace_s it is back, or the hold is cancelled above).
            return;
        }
        Complete(c, in, out, Haptic::HoldDone, "HOLD full (" + HoldText(in) + ")",
                 "the ring full");
    }

    // The held item used, by the ring filled (`feel` HoldDone) or a gesture (Crunch, BellToll):
    // the game's use (UseGoods at once, or select + Square: its own animation), the feel and the
    // ring's flash. One the game says cannot be used now (`what`: "the ring full", "crushed",
    // "rung") is a deny instead (left_scheme=2: back on the arc; 1: it stays in the hand, spent,
    // until let go): a bell the game refuses gives no toll.
    void Complete(const Config& c, const Input& in, Output& out, Haptic feel,
                  const std::string& how, const char* what) {
        gesture_pending_ = false;
        out.use_whoosh = -1.0f; // completion/denial replaces the build-up
        if (in.snapshot_fresh && !in.snapshot.slot[static_cast<std::size_t>(held_slot_)].usable) {
            out.hold_hum = 0.0f;
            hold_spent_ = true;
            // (a gesture's own line with it: how it was crushed or rung, for tuning)
            const std::string why =
                std::string{what} + (feel == Haptic::HoldDone ? "" : " (" + how + ")");
            if (c.left_scheme == 2) {
                out.Haptics(Haptic::Deny);
                pulse(held_slot_, true);
                ToArc(in, out, why + ", the game says it cannot be used now: back", 0.0, false);
            } else {
                Deny(out, why + ", the game says it cannot be used now (let go)");
            }
            return;
        }
        out.hold_hum = 0.0f;
        out.hold_progress = 1.0f;
        out.Haptics(feel);
        if (in.gameplay && in.body && IsBellItem(held_goods_) && feel == Haptic::BellToll)
            out.BellSound(feel);
        hold_use_ = true;
        hold_done_ns_ = steady_ns_;
        last_use_ = in.t;
        out.Log(how + ": used");
        const bool instant =
            c.instant && (held_class_.use == UseKind::Instant || held_class_.use == UseKind::Bullet);
        if (instant && !latched(held_goods_)) {
            Send(out, c.select && in.snapshot.selected != held_slot_ ? held_slot_ : -1,
                 Action::Use, in);
            Enter(Phase::Waiting);
            wait_t_ = in.t;
            out.Log("USE goods=" + std::to_string(held_goods_) + " (instant, held " +
                    HoldText(in) + ")");
        } else {
            StartSquare(c, in, out);
        }
    }

    // The ring (Scene::hold_from_ns) filling from `from` (the shake's steps, a gesture's full).
    void ShowRing(double from) {
        hold_from_ = from;
        hold_from_ns_ = hold_from_base_ns_ -
                        static_cast<std::int64_t>(std::llround((hold_from_base_ - from) * 1e9));
    }

    // quick_use_crush: the squeeze of the other button (left_scheme=2: the grip; 1: the trigger)
    // on an item the game crushes (CrushGate): the pressure building in the hum as it closes,
    // the strain through the confirm (up from 0.35 to 0.70), then crushed (`feel`: Crunch).
    void CrushTick(const Config& c, const Input& in, Output& out, bool arm, float progress,
                   Haptic& feel, std::string& how) {
        const float squeeze = c.left_scheme == 2 ? in.grip : in.trigger;
        if (crush_.Update(c.crush_cfg, in.t, squeeze, arm)) {
            char text[200];
            std::snprintf(text, sizeof(text),
                          "CRUSH (the %s squeezed to %.2f, held %.0f ms; armed %.2f s after the "
                          "take, the item held %.2f s)",
                          c.left_scheme == 2 ? "grip" : "trigger", crush_.Peak(),
                          (in.t - crush_.Since()) * 1000.0, crush_.ArmedAt() - crush_.HeldSince(),
                          in.t - crush_.HeldSince());
            how = text;
            feel = Haptic::Crunch;
            crush_.Consume();
            return;
        }
        const float strain = crush_.Strain(c.crush_cfg, in.t);
        if (strain >= 0.0f) {
            out.hold_hum = std::max(HoldHum(progress), 0.35f + 0.35f * strain);
        } else if (const float p = crush_.Pressure(c.crush_cfg); p > 0.0f) {
            out.hold_hum = std::max(out.hold_hum, 0.22f + 0.13f * p);
        }
    }

    // quick_use_shake: the bell's strokes (ShakeGate): a strike per counted stroke, the hold's
    // hum ducked under them while a chain lives, the ring stepping a stroke's share up at each
    // (filling on from there; back where the time puts it once the strokes stop: the hold is
    // never shortened by them); rung (`feel`: BellToll).
    void ShakeTick(const Config& c, const Input& in, Output& out, bool arm, Haptic& feel,
                   std::string& how) {
        const ShakeGate::Event ev = shake_.Update(c.shake_cfg, in.t, in.hand_valid, in.hand_wv,
                                                  in.hand_v - in.head_v, arm);
        if (ev == ShakeGate::Event::Rung) {
            char text[48];
            std::snprintf(text, sizeof(text), ", the bell held %.2f s",
                          in.t - shake_.HeldSince());
            how = "SHAKE (" + shake_.Detail(c.shake_cfg) + text + ")";
            feel = Haptic::BellToll;
            shake_shown_ = 0;
            return;
        }
        if (ev == ShakeGate::Event::Stroke) {
            out.Haptics(Haptic::BellStrike);
            if (in.gameplay && in.body && IsBellItem(held_goods_)) out.BellSound(Haptic::BellStrike);
        }
        const int strokes = std::max(c.shake_cfg.strokes, 1);
        const int shown = std::min(shake_.Strokes(), strokes - 1);
        if (shown != shake_shown_) {
            if (shown > shake_shown_) {
                out.Log("SHAKE: stroke " + std::to_string(shown) + " of " +
                        std::to_string(strokes));
            } else {
                out.Log("SHAKE: the strokes stopped after " + std::to_string(shake_shown_) +
                        (shown > 0 ? " (a new chain)" : ": the ring back"));
                ShowRing(hold_from_base_);
            }
            if (shown > 0) {
                ShowRing(std::min(hold_from_,
                                  in.t - static_cast<double>(shown) / strokes * hold_len_));
            }
            shake_shown_ = shown;
        }
        if (shown > 0) {
            out.hold_hum = std::min(out.hold_hum, 0.10f); // the strikes stand out
        }
    }

    // quick_use_stab: the blood stabbed into a zone (Input::stab: the presenter's tracking of the
    // Blood Vial's zones, re-armed only once the hand has been out of them): used there, at once
    // (the game's own UseGoods, no animation; Request::stab: the game side refuses it at full
    // HP, as the vial). A stab within the cooldown waits for it; with the game's arm nothing; one
    // the game says cannot be used now is a deny (it stays in the hand: stab again).
    void AtStab(const Config& c, const Input& in, Output& out, bool arm) {
        if (in.stab && stab_pending_t_ < 0.0) {
            const std::string zone = in.stab_zone != nullptr ? in.stab_zone : "?";
            if (!arm) {
                out.Log("STAB into the " + zone + " while the game has the arm: nothing");
                return;
            }
            stab_pending_t_ = in.t;
            stab_zone_ = zone;
        }
        if (stab_pending_t_ < 0.0 || in.t - last_use_ < c.use_cooldown_s) {
            return;
        }
        stab_pending_t_ = -1.0;
        if (in.snapshot_fresh && !in.snapshot.slot[static_cast<std::size_t>(held_slot_)].usable) {
            Deny(out, "stabbed into the " + stab_zone_ + ": the game says it cannot be used now");
            return;
        }
        last_use_ = in.t;
        Request& r = Send(out, c.select && in.snapshot.selected != held_slot_ ? held_slot_ : -1,
                          Action::Use, in);
        r.stab = 1;
        stab_use_ = true;
        Enter(Phase::Waiting);
        wait_t_ = in.t;
        char text[120];
        std::snprintf(text, sizeof(text), "USE goods=%d stabbed into the %s (held %.2f s)",
                      held_goods_, stab_zone_.c_str(), in.t - mouth_gate_.HeldSince());
        out.Log(text);
    }

    // The samples a release reads: the grip located at the input times (throw_fresh) when there
    // are any for the last 120 ms, else the display-time poses.
    [[nodiscard]] bool UseFresh(const Config& c, double now) const {
        return c.throw_fresh && fresh_.Has(now, 0.12);
    }
    [[nodiscard]] const VelocityHistory& ReleaseHistory(const Config& c, double now) const {
        return UseFresh(c, now) ? fresh_ : history_;
    }

    // The grip opened at release_t_ (with the arm release_arm_), now at in.t (later when the throw
    // waited for the hand's peak): throw it, or put it back.
    void Release(const Config& c, const Input& in, Output& out) {
        // Two readings of the same release: the grip located at the input times up to now (the
        // freshest) and the display-time poses up to the grip's opening (as before 2026-10-05);
        // the stronger counts. A runtime that holds the velocity in its prediction leaves the
        // latter ~45 ms behind (the tester's throws read ~0.7 of the hand's peak); one that
        // extrapolates the acceleration lets them run a little past the peak when the grip opens
        // at or after it (then the fresh one alone would read less than before).
        const V3 v_display = history_.Release(release_t_);
        const V3 v_fresh = UseFresh(c, in.t) ? fresh_.Release(in.t) : V3{};
        const bool fresh = Length(v_fresh) > 0.0f && Length(v_fresh) >= Length(v_display);
        const VelocityHistory& h = fresh ? fresh_ : history_;
        const double at = fresh ? in.t : release_t_;
        // The release velocity, or the stronger peak of the last throw_peak_window_s when the
        // hand was already slowing down as the grip opened (its speed; the release's direction
        // turned at most throw_peak_turn_deg toward it).
        const V3 v_release = fresh ? v_fresh : v_display;
        // Only a release that is itself a throw (throw_min_speed) looks back: a hand slowing to a
        // drift as it puts the item back never throws from the reach before it.
        const V3 v_peak = c.throw_peak_window_s > 0.0f
                              ? h.PeakBefore(at, c.throw_peak_window_s, v_release, 75.0f,
                                             c.throw_min_speed)
                              : V3{};
        const bool use_peak = Length(v_peak) > Length(v_release);
        const V3 v = use_peak ? TurnToward(v_release, v_peak, c.throw_peak_turn_deg) *
                                    Length(v_peak)
                              : v_release;
        const float speed = Length(v);
        const bool arm = release_arm_;
        if (use_peak) {
            out.Log("release " + std::to_string(Length(v_release)).substr(0, 4) +
                    " m/s, earlier peak " + std::to_string(speed).substr(0, 4) + " m/s within " +
                    std::to_string(static_cast<int>(c.throw_peak_window_s * 1000.0f)) +
                    " ms: the peak counts");
        }
        if (c.throw_trace) {
            TraceRelease(c, in, out, v_release, v_peak, v, fresh,
                         speed < c.throw_min_speed ? "released slowly: put back"
                         : !arm                    ? "the game has the arm: put back"
                         : latched(held_goods_)    ? "latched: the game's own throw"
                                                   : "thrown");
        }
        if (speed >= c.throw_min_speed && arm && latched(held_goods_)) {
            // UseGoods did not throw this item before (Latched): the game's own throw.
            out.Log("THROW goods=" + std::to_string(held_goods_) +
                    " latched: the game's own throw");
            StartSquare(c, in, out);
        } else if (speed >= c.throw_min_speed && arm) {
            Throw(c, in, out, v, fresh, Length(v_fresh), Length(v_display));
        } else if (c.left_scheme == 2) {
            // left_scheme=2: back on the arc, which stays open for the next pick.
            ToArc(in, out,
                  !arm ? "trigger let go while the game had the arm: put back"
                       : "trigger let go slowly (" + std::to_string(speed).substr(0, 4) +
                             " m/s): put back",
                  0.0, true);
        } else {
            PutAway(out,
                    !arm ? "released while the game had the arm"
                         : "released slowly (" + std::to_string(speed).substr(0, 4) + " m/s)",
                    true);
        }
    }

    void Throw(const Config& c, const Input& in, Output& out, V3 v, bool fresh, float fresh_speed,
               float display_speed) {
        const VelocityHistory& h = fresh ? fresh_ : history_;
        float windup = 0.0f;
        float omega = 0.0f;
        windup_.Measure(in.t, v, 0.0f, windup, omega);
        const float turn = h.Turn(fresh ? in.t : release_t_, v);
        const float power = ThrowPower(windup, omega, turn, c.throw_push_factor);
        float lifted = 0.0f;
        const V3 launch = LiftPitch(LaunchVelocity(v, held_class_.throw_class, c.throw_gain * power,
                                                   c.throw_gain_curve),
                                    c.throw_lift_deg, &lifted);
        Request& r = Send(out, c.select && in.snapshot.selected != held_slot_ ? held_slot_ : -1,
                          Action::Throw, in);
        r.v_world = {launch.x, launch.y, launch.z}; // LOCAL here; the presenter maps it
        r.throw_class = static_cast<std::uint8_t>(held_class_.throw_class);
        Enter(Phase::Waiting);
        wait_t_ = in.t;
        thrown_ = true;
        if (c.left_scheme == 2) {
            // The trigger that held it may still be past the gun's latch (0.25) as it opens: the
            // gun waits for it to be let go, as after any item action.
            HoldTriggerBack();
        }
        out.throw_strength = std::clamp((Length(launch) - 5.0f) / 11.0f, 0.0f, 1.0f);
        out.Haptics(Haptic::Throw);
        const auto f = [](float x, int n) { return std::to_string(x).substr(0, n); };
        const float spin = Length(fresh ? fresh_spin_.Release(in.t) : spin_.Release(release_t_));
        const int waited = static_cast<int>(std::lround((in.t - release_t_) * 1000.0));
        out.Log("THROW goods=" + std::to_string(held_goods_) + " hand " + f(Length(v), 4) +
                " m/s (" + f(v.x, 5) + ", " + f(v.y, 5) + ", " + f(v.z, 5) + "; spin " +
                f(spin, 4) + ") -> " + f(Length(launch), 4) + " m/s (power " + f(power, 4) +
                ": wind-up " + f(windup, 5) + " m, whip " + f(omega, 4) + " rad/s, path turn " +
                f(turn, 4) + " deg; poses " +
                std::to_string(static_cast<int>(std::lround(in.pose_lead_s * 1000.0f))) +
                " ms ahead, predicted past the release up to " +
                f(history_.PeakAfter(release_t_), 4) + " m/s) | read " +
                (fresh ? "fresh" : "display-time") + ": fresh " + f(fresh_speed, 4) +
                ", display-time " + f(display_speed, 4) + " m/s; pitch " + f(PitchOf(launch), 5) +
                " deg (lift +" + f(lifted, 4) + ")" +
                (waited > 0 ? "; waited " + std::to_string(waited) + " ms for the peak" : ""));
    }

    // VR_THROW_TRACE: the release's summary now; its hand samples from throw_trace_s before it to
    // throw_trace_post_s after it go out with it then (EmitTrace, into out.trace).
    void TraceRelease(const Config& c, const Input& in, Output& out, V3 v_release, V3 v_peak,
                      V3 v, bool fresh, const char* what) {
        if (trace_pending_) {
            EmitTrace(c, out); // the previous release's, with what follow-through it has
        }
        ++trace_id_;
        float now_at = 0.0f;
        float now_peak = 0.0f;
        trace_.NowVelocity(release_t_, 0.12, now_at, now_peak);
        char head[900];
        std::snprintf(
            head, sizeof(head),
            "THROWTRACE #%u goods %d %s: release %.2f m/s (%s poses), the peak of the last %.0f "
            "ms %.2f, used %.2f (%.3f,%.3f,%.3f) %.0f ms after the grip opened; the display-time "
            "poses: %.2f at the release, predicted past it up to %.2f; the grip located at the "
            "input time (with the spin): %.2f at the release, %.2f the fastest of the last 120 ms; "
            "poses %.0f ms ahead | TT lines: f = input frame - the release (s), d = its pose's "
            "display time - the release (s), p/v = the grip at the display time (m, m/s, LOCAL), "
            "s = the spin part at the item, w = angular velocity from the orientations (rad/s), "
            "rt = the runtime's, r = hand - shoulder (m), g = grip, now = the grip located at the "
            "input time",
            trace_id_, held_goods_, what, Length(v_release), fresh ? "fresh" : "display-time",
            c.throw_peak_window_s * 1000.0f, Length(v_peak), Length(v), v.x, v.y, v.z,
            (in.t - release_t_) * 1000.0, Length(history_.Release(release_t_)),
            history_.PeakAfter(release_t_), now_at, now_peak, in.pose_lead_s * 1000.0f);
        trace_head_ = head;
        trace_t0_ = release_t_;
        trace_pending_ = true;
    }

    void EmitTrace(const Config& c, Output& out) {
        trace_pending_ = false;
        if (!out.trace.empty()) {
            out.trace += '\n';
        }
        out.trace += trace_head_;
        const std::string lines = trace_.Lines(trace_t0_, c.throw_trace_s, c.throw_trace_post_s,
                                               "TT#" + std::to_string(trace_id_));
        if (!lines.empty()) {
            out.trace += '\n';
            out.trace += lines;
        }
    }

    void StartSquare(const Config& c, const Input& in, Output& out) {
        // The game's own use: select the slot, then Square once the game shows it selected. The
        // trigger that asked for it never reaches the game as the gun meanwhile.
        if (in.snapshot.selected != held_slot_) {
            Send(out, held_slot_, Action::None, in);
        }
        l2_fresh_needed_ = in.trigger_latch;
        Enter(Phase::SquareWait);
        wait_t_ = in.t;
        out.Log("USE goods=" + std::to_string(held_goods_) + " (the game's animation)");
        UpdateSquare(c, in, out);
    }

    void UpdateSquare(const Config& c, const Input& in, Output& out) {
        if (in.snapshot_fresh && in.snapshot.selected == held_slot_ &&
            in.snapshot.slot[static_cast<std::size_t>(held_slot_)].goods == held_goods_) {
            out.square_press = true;
            out.square_goods = held_goods_;
            l2_fresh_needed_ = in.trigger_latch;
            out.Log("Square for goods " + std::to_string(held_goods_) +
                    (CannedOf(held_goods_) ? " (a canned use: its animation shown flat)" : ""));
            Idle(out, false);
            return;
        }
        if (in.t - wait_t_ >= c.square_wait_s) {
            out.Haptics(Haptic::Deny);
            out.Log("Square path: the game did not select slot " + std::to_string(held_slot_));
            l2_fresh_needed_ = in.trigger_latch;
            Idle(out, false);
        }
    }

    void UpdateWaiting(const Config& c, const Input& in, Output& out) {
        if (in.have_result && in.result.serial == serial_ && in.result.outcome != Outcome::None &&
            in.result.outcome != Outcome::Selected) {
            const Result& r = in.result;
            out.Log(std::string{"result "} + OutcomeName(r.outcome) + " goods=" +
                    std::to_string(r.goods) + " qty " + std::to_string(r.qty_before) + "->" +
                    std::to_string(r.qty_after) + (r.reason[0] != 0 ? " (" : "") + r.reason +
                    (r.reason[0] != 0 ? ")" : ""));
            l2_fresh_needed_ = in.trigger_latch;
            if (c.left_scheme == 2) {
                Settle2(c, in, out, r.outcome);
                return;
            }
            if (r.outcome == Outcome::Used || r.outcome == Outcome::Thrown) {
                if (c.stack) {
                    // The next one with a new squeeze (a throw's release was the grip's last).
                    regrab_slot_ = held_slot_;
                    regrab_goods_ = held_goods_;
                    regrab_from_ = thrown_ ? wait_t_ : in.t;
                    regrab_until_ = regrab_from_ + c.regrab_s;
                }
                if (r.outcome == Outcome::Used) {
                    out.Haptics(MouthHaptic(Haptic::Use));
                }
                Idle(out, true);
            } else {
                if (r.outcome == Outcome::Latched) {
                    latch(held_goods_);
                }
                out.Haptics(Haptic::Deny);
                if (!thrown_ && grip_on_ && r.outcome != Outcome::Latched) {
                    Enter(Phase::Holding); // still in the hand
                    mouth_use_ = false;    // (refused at the mouth: the hand leaves and comes back)
                    wipe_use_ = false;     // (refused on the blade: a new stroke tries again)
                    stab_use_ = false;     // (refused stabbed in: out of the zone, stab again)
                    // (refused at the ring's end: spent until the grip lets go)
                    hold_spent_ = hold_spent_ || hold_use_;
                    hold_use_ = false;
                    pulse(held_slot_, true);
                } else {
                    Idle(out, !thrown_);
                }
            }
            return;
        }
        if (in.t - wait_t_ >= c.result_wait_s) {
            out.Haptics(Haptic::Deny);
            out.Log("no answer from the game");
            Idle(out, true);
        }
    }

    // Idle after a throw or a use: a new squeeze of the grip within regrab_s puts the next one of
    // that item in the hand, without the arc (quick_arc_stack). Your own arm only; the window
    // closes on Y, the vial, a context switch, or when that slot runs out or changes.
    void UpdateRegrab(const Config& c, const Input& in, Output& out) {
        if (regrab_slot_ < 0) {
            return;
        }
        if (!c.stack || in.t > regrab_until_ || in.x_edge || in.vial_armed || !in.body) {
            regrab_slot_ = -1;
            return;
        }
        if (!grip_on_ || grip_edge_t_ <= regrab_from_ || !in.snapshot_fresh ||
            in.arm_weight < c.arm_min) {
            return;
        }
        const std::int32_t slot = regrab_slot_;
        regrab_slot_ = -1;
        const Slot& s = in.snapshot.slot[static_cast<std::size_t>(slot)];
        if (s.goods != regrab_goods_ || s.Empty()) {
            out.Haptics(Haptic::Empty);
            out.Log("stack: none left in slot " + std::to_string(slot));
            return;
        }
        Grab(c, in, out, slot, "stack");
    }

    // ---- left_scheme=2: the grip opens the arc, the trigger takes ----------------------------

    // Idle: a squeeze of the grip (Update drew the vial with one at the hip) opens the arc where
    // Y's open would (the hunter's hand, the slots); not with the trigger held (a clench round
    // the gun) nor with a vial in the hand (the needle's fist).
    void UpdateIdle2(const Config& c, const Input& in, Output& out, bool grip_edge) {
        if (!grip_edge) {
            return;
        }
        // Refused: a soft buzz (Empty), so a squeeze that opens nothing is felt.
        if (in.trigger_latch || in.trigger_raw >= 0.45f) {
            out.Haptics(Haptic::Empty);
            out.Log("grip with the trigger held (a clench): no arc");
        } else if (in.vial_busy) {
            out.Haptics(Haptic::Empty);
            out.Log("grip with the vial in the hand (the needle's fist): no arc; X or a Y tap puts "
                    "the vial back");
        } else if (!in.body || !in.snapshot_fresh) {
            out.Haptics(Haptic::Empty);
            out.Log(!in.body ? "grip: no arc (the body is not driven)"
                             : "grip: no arc (no quick-item state)");
        } else {
            toggle_by_ = "grip";
            regrab_slot_ = -1;
            Open(c, in, out);
        }
    }

    // Open: the trigger on a card lit dwell_s takes it (TakeByTrigger); a pull on a card lit for
    // less waits for it while the trigger stays down and the hand on it (or just off it: the
    // pull's own jerk, within late_move_m); a hand that slips off it further makes the pull the
    // gun's (a pull is never eaten); a pull just after the hand left a card it had lit dwell_s
    // (late_hover_s, within late_move_m of where it left) takes that one. Off the cards the pull
    // is the gun's, at once (the arc closes). The grip again (once the one that opened it was let
    // go), Y or X closes it; so do a swing, a leap or a dodge (Input::combat) and idle_close2_s
    // with no card lit. The arc that is back after a throw (reopen_s) closes at its time and
    // takes a card only once lit reopen_dwell_s.
    void UpdateOpen2(const Config& c, const Input& in, Output& out, bool y_edge, bool grip_edge,
                     bool l2_edge) {
        if (!grip_on_) {
            grip_armed_ = true;
        }
        // Your own hand only, once the arc has unfolded (as UpdateOpen).
        const bool arm = in.arm_weight >= c.arm_min;
        const bool live = in.hover_fresh && in.hover.open_serial == open_serial_ && in.hover.placed;
        const double placed_t =
            live && in.hover.placed_ns > 0
                ? std::max(open_t_, static_cast<double>(in.hover.placed_ns) * 1e-9)
                : open_t_;
        const bool settled = in.t - placed_t >= c.hover_delay_s;
        const std::int32_t hovered = live && arm && settled ? in.hover.slot : -1;
        // The arc back after a throw wants a longer look at a card (the hand coming back to rest
        // after the follow-through lands on one).
        const double dwell = reopen_until_ > 0.0
                                 ? static_cast<double>(std::max(c.dwell_s, c.reopen_dwell_s))
                                 : static_cast<double>(c.dwell_s);
        if (hovered != hover_slot_) {
            if (hover_slot_ >= 0) {
                last_hover_ = hover_slot_;
                last_dwelled_ = in.t - hover_since_ >= dwell - 1e-6;
                hover_lost_t_ = in.t;
                left_card_p_ = in.hand_p;
            }
            if (hovered >= 0) {
                out.Haptics(hovered_any_ ? Haptic::Detent : Haptic::FirstDetent);
                hovered_any_ = true;
                hover_since_ = in.t;
            }
            hover_slot_ = hovered;
        }
        // Only a lit card keeps the arc open (the hand moving does not: a fight).
        if (hovered >= 0) {
            last_activity_ = in.t;
        }
        if (y_edge) {
            Close(out, toggle_by_);
            return;
        }
        if (in.x_edge) {
            Close(out, "X (the vial)");
            return;
        }
        if (grip_edge && grip_armed_) {
            Close(out, "grip");
            return;
        }
        if (in.combat) {
            panic_l2_ = l2_edge; // a pull in the same frame is still the gun's
            Close(out, in.combat_why[0] != 0 ? in.combat_why : "combat");
            return;
        }
        // The hand nearly still around the card's loss (the pull's own jerk of the controller, or
        // a drift off its edge), not a hand that moved on: its travel from hover_lag_s before the
        // loss was seen (the hover comes from the game's frame, a frame or two late, and holds
        // to 9 cm) up to now, at most late_move_m (the 2026-10-05 mock: a hand moved 10 cm off a
        // card in 80 ms and stopped; the loss was seen with the hand already there).
        const bool still_since_left =
            in.hand_valid && hand_track_.TravelSince(hover_lost_t_ - c.hover_lag_s, in.hand_p) <=
                                 c.late_move_m;
        if (l2_edge) {
            if (hovered >= 0) {
                pull_slot_ = hovered; // taken once it has been lit `dwell` (below)
                pull_t_ = in.t;
                pull_lit_from_ = hover_since_;
            } else if (arm && last_hover_ >= 0 && last_dwelled_ &&
                       in.t - hover_lost_t_ <= c.late_hover_s && still_since_left) {
                TakeByTrigger(c, in, out, last_hover_, "late hover");
                return;
            } else {
                // Combat first: off the cards the gun in the empty hand fires at the pull and the
                // arc closes in the same frame.
                panic_l2_ = true;
                Close(out, "trigger (gun)");
                return;
            }
        }
        if (pull_slot_ >= 0) {
            const bool on_it = hovered == pull_slot_;
            // Off it, but the hand moved at most late_move_m since the pull: the pull's own jerk.
            const bool jerk = !on_it && in.hand_valid &&
                              hand_track_.TravelSince(pull_t_, in.hand_p) <= c.late_move_m &&
                              in.t - pull_t_ <= c.late_hover_s + c.dwell_s;
            const int lit_ms = static_cast<int>(std::lround((in.t - pull_lit_from_) * 1000.0));
            if (!in.trigger_latch) {
                out.Log("trigger let go on slot " + std::to_string(pull_slot_) + " lit " +
                        std::to_string(lit_ms) + " ms: nothing taken");
                pull_slot_ = -1;
            } else if (!on_it && !jerk) {
                // The hand slipped off before the card was lit long enough: the pull is the gun's
                // (a pull is never eaten), the arc closes.
                out.Log("the hand left slot " + std::to_string(pull_slot_) + " after " +
                        std::to_string(lit_ms) + " ms (under the dwell): the gun");
                panic_l2_ = true;
                Close(out, "trigger (gun)");
                return;
            } else if (in.t - pull_lit_from_ >= dwell - 1e-6) {
                const std::int32_t slot = pull_slot_;
                pull_slot_ = -1;
                TakeByTrigger(c, in, out, slot,
                              !on_it                   ? "hover (just off it: the pull's jerk)"
                              : in.t - pull_t_ > 1e-6 ? "hover (waited for the dwell)"
                                                       : "hover");
                return;
            }
        }
        if (pull_slot_ < 0 && reopen_until_ > 0.0 && in.t >= reopen_until_) {
            Close(out, "the next one was not taken");
            return;
        }
        const float idle = c.idle_close_s > 0.0f ? std::min(c.idle_close_s, c.idle_close2_s)
                                                 : c.idle_close2_s;
        if (pull_slot_ < 0 && idle > 0.0f && in.t - last_activity_ >= idle) {
            Close(out, "idle: no card lit");
        }
    }

    // left_scheme=2, an item in the hand: a squeeze of the grip puts it back on the arc, with the
    // hand still. A squeeze while the hand moves (putback_max_speed in the last 100 ms) is a
    // clench, not a put-back; one with the hand still goes putback_wait_s later, unless the hand
    // starts moving (putback_max_speed: a wind-up) or the trigger is let go meanwhile (a throw:
    // UpdateHolding's release). True: put back this frame (nothing else to do).
    bool PutBack2(const Config& c, const Input& in, Output& out, bool grip_edge,
                  bool trigger_let_go) {
        if (grip_edge && !release_pending_ && !putback_pending_) {
            if (in.t - fast_t_ <= 0.1) {
                out.Log("grip squeezed with the hand moving: a clench, no put-back");
            } else {
                putback_pending_ = true;
                putback_t_ = in.t;
            }
        }
        if (!putback_pending_) {
            return false;
        }
        const float speed = std::max(in.hand_valid ? Length(in.hand_v) : 0.0f,
                                     in.hand_now_valid ? Length(in.hand_v_now) : 0.0f);
        if (trigger_let_go || release_pending_ || speed >= c.putback_max_speed) {
            putback_pending_ = false;
            out.Log(trigger_let_go ? "grip put-back dropped: the trigger let go"
                                   : "grip put-back dropped: the hand moves (a throw's wind-up)");
            return false;
        }
        if (in.t - putback_t_ >= c.putback_wait_s - 1e-6) {
            putback_pending_ = false;
            if (CrankPlayed()) {
                PutAway(out, "grip: the music box played", true); // used: the arc closed
            } else {
                ToArc(in, out, "grip: put back", 0.0, true);
            }
            return true;
        }
        return false;
    }

    // The trigger took slot `slot`: a throwable is held while the trigger stays down (its
    // release throws, UpdateHolding); a usable is used at once (the game's UseGoods, or its own
    // animation), the arc closing once the game answers. Refused (the game has the arm, the
    // cooldown, the game says no): a deny, the arc stays open.
    void TakeByTrigger(const Config& c, const Input& in, Output& out, std::int32_t slot,
                       const char* via) {
        const int lit_ms = static_cast<int>(std::lround((in.t - hover_since_) * 1000.0));
        const std::string detail = "by the trigger, lit " + std::to_string(lit_ms) + " ms";
        if (slot < 0 || slot >= SlotCount || in.snapshot.slot[static_cast<std::size_t>(slot)].Empty()) {
            out.Haptics(Haptic::Empty);
            out.Log("trigger on an empty card (slot " + std::to_string(slot) + ")");
            return;
        }
        const Slot& s = in.snapshot.slot[static_cast<std::size_t>(slot)];
        if (Classify(s.goods).use == UseKind::Throw && c.throws) {
            Grab(c, in, out, slot, via, detail);
            trigger_hold_ = true;
            return;
        }
        const bool arm = in.arm_weight >= c.arm_min;
        // quick_use_mouth: an item to eat or drink stays in the hand while the trigger that took
        // it is held, as a throwable, and is used at the mouth (AtMouth; the cooldown is the
        // mouth's); let go of away from it, it goes back on the arc.
        // quick_use_wipe: a blade-wipe item the same way, applied along the blade (AtBlade).
        // quick_use_stab: a blood the same way, injected by a stab (AtStab).
        const bool mouth = MouthItem(c, s.goods) || WipeItem(c, s.goods) || StabItem(c, s.goods);
        // quick_hold_use: a misc item stays in the hand while the trigger that took it is held,
        // the ring filling; it is used once full (HoldTick; the cooldown waits there, as the
        // mouth's), let go of before it goes back on the arc. (The game refusing it now is a
        // deny at the take.)
        // quick_music_box_crank: the music box the same way, played by its crank (CrankTick).
        const bool hold = !mouth && (HoldItem(c, s.goods) || CrankItem(c, s.goods));
        const char* why = !arm ? "the game has the arm"
                          : !mouth && !hold && in.t - last_use_ < c.use_cooldown_s ? "cooldown"
                          : !s.usable ? "the game says it cannot be used now"
                                      : nullptr;
        if (mouth && why == nullptr) {
            Grab(c, in, out, slot, via, detail); // (logs where the mouth is)
            trigger_hold_ = true;
            return;
        }
        if (hold && why == nullptr) {
            Grab(c, in, out, slot, via, detail);
            trigger_hold_ = true;
            return;
        }
        if (why != nullptr) {
            out.Haptics(Haptic::Deny);
            pulse(slot, true);
            out.Log("deny slot " + std::to_string(slot) + " goods=" + std::to_string(s.goods) +
                    ": " + why);
            return;
        }
        Grab(c, in, out, slot, via, detail, false);
        out.Haptics(Haptic::Pick);
        last_use_ = in.t;
        const bool instant =
            c.instant && (held_class_.use == UseKind::Instant || held_class_.use == UseKind::Bullet);
        if (instant && !latched(held_goods_)) {
            Send(out, c.select && in.snapshot.selected != held_slot_ ? held_slot_ : -1,
                 Action::Use, in);
            Enter(Phase::Waiting);
            wait_t_ = in.t;
            out.Log("USE goods=" + std::to_string(held_goods_) + " (instant, at the pull)");
        } else {
            StartSquare(c, in, out);
        }
    }

    // The arc open again where it was (its placement kept: the same open serial; it fades in) with
    // the hand empty and the gun back in it: an item put back, a refused use or throw, or after a
    // throw (`until` > 0: it closes then unless the hand reaches a card).
    void ToArc(const Input& in, Output& out, const std::string& why, double until,
               bool put_back) {
        if (put_back) {
            out.Haptics(Haptic::PutBack);
        }
        out.Haptics(Haptic::GunBack);
        out.Log("ARC (" + why + ")");
        held_slot_ = -1;
        held_goods_ = -1;
        held_ffx_ = -1;
        held_class_ = {};
        thrown_ = false;
        release_pending_ = false;
        trigger_hold_ = false;
        putback_pending_ = false;
        mouth_item_ = mouth_use_ = false;
        wipe_item_ = wipe_use_ = false;
        hold_item_ = hold_use_ = hold_spent_ = false;
        ClearGestures();
        Enter(Phase::Open);
        open_t_ = in.t; // the hover waits hover_delay_s again (no take as it fades in)
        last_activity_ = in.t;
        open_hand_ = activity_hand_ = in.hand_p;
        grip_armed_ = !grip_on_;
        squeeze_ = false;
        pull_slot_ = -1;
        hover_slot_ = last_hover_ = -1;
        last_dwelled_ = false;
        reopen_until_ = until;
        HoldTriggerBack();
    }

    // left_scheme=2: the game's answer to a use or a throw. A use closes the arc (done); after a
    // throw it is back for the next one (reopen_s; stack=0 or 0 s: closed); a refusal puts the
    // item back on it. Never with a vial in the hand.
    void Settle2(const Config& c, const Input& in, Output& out, Outcome o) {
        const bool done = o == Outcome::Used || o == Outcome::Thrown;
        if (o == Outcome::Latched) {
            latch(held_goods_);
        }
        if (!done || o == Outcome::Used) {
            out.Haptics(done ? MouthHaptic(Haptic::Use) : Haptic::Deny);
        }
        // Refused at the mouth (or on the blade) with the trigger still held: it stays in the hand
        // (the hand leaves the mouth and comes back to try again, MouthGate; a new stroke).
        if (!done && (mouth_use_ || wipe_use_ || stab_use_) && trigger_hold_ &&
            o != Outcome::Latched && !in.vial_busy) {
            mouth_use_ = wipe_use_ = stab_use_ = false;
            Enter(Phase::Holding);
            pulse(held_slot_, true);
            return;
        }
        const bool again = !done || (thrown_ && c.stack && c.reopen_s > 0.0f);
        if (again && !in.vial_busy && !out.draw_vial) {
            if (!done) {
                pulse(held_slot_, true);
            }
            ToArc(in, out,
                  done ? "the arc is back for the next one" : "refused: back on the arc",
                  done ? in.t + c.reopen_s : 0.0, false);
        } else {
            Idle(out, true);
        }
    }

    Request& Send(Output& out, std::int32_t select, Action action, const Input& in) {
        out.send = true;
        Request& r = out.request;
        r = {};
        r.serial = ++serial_;
        r.steady_ms = static_cast<std::int64_t>(in.t * 1000.0);
        r.select = static_cast<std::int8_t>(select);
        r.action = action;
        r.goods = held_goods_;
        r.slot = held_slot_;
        r.dmy = held_class_.dmy;
        return r;
    }

    void Deny(Output& out, const std::string& why) {
        out.Haptics(Haptic::Deny);
        pulse(held_slot_, true);
        out.Log("deny: " + why);
    }

    // The answer's feel: a use at the mouth is a gulp (a drink) or a bite (eaten), else `h`.
    [[nodiscard]] Haptic MouthHaptic(Haptic h) const {
        if (hold_use_ && h == Haptic::Use) {
            return Haptic::None; // quick_hold_use: the thump at the ring's end was its feel
        }
        if (wipe_use_ && h == Haptic::Use) {
            return Haptic::Ignite; // quick_use_wipe: the blade took it
        }
        if (stab_use_ && h == Haptic::Use) {
            return Haptic::Inject; // quick_use_stab: injected (the presenter's THUMP, both hands)
        }
        if (!mouth_use_) {
            return h;
        }
        return MouthOf(held_goods_) == MouthKind::Drink ? Haptic::Gulp : Haptic::Bite;
    }

    void Close(Output& out, const std::string& why) {
        out.Haptics(Haptic::Close);
        out.Log("CLOSE (" + why + ")");
        Enter(Phase::Idle);
        hover_slot_ = last_hover_ = -1;
        pull_slot_ = -1;
        reopen_until_ = 0.0;
        HoldTriggerBack();
    }

    // The arc closed or the item left the hand: a trigger still down (resting past its latch, or
    // part of the fist) is not the gun until it is let go; a reflex parry fires this frame.
    void HoldTriggerBack() {
        if (latch_now_ && !panic_l2_) {
            l2_fresh_needed_ = true;
        }
        parry_t_ = -1.0;
    }

    void PutAway(Output& out, const std::string& why, bool haptic) {
        if (haptic) {
            out.Haptics(Haptic::PutBack);
        }
        out.Log("PUT (" + why + ")");
        Idle(out, true);
    }

    void Idle(Output& out, bool gun_back) {
        if (gun_back) {
            out.Haptics(Haptic::GunBack);
        }
        Enter(Phase::Idle);
        held_slot_ = -1;
        held_goods_ = -1;
        held_ffx_ = -1;
        held_class_ = {};
        thrown_ = false;
        release_pending_ = false;
        trigger_hold_ = false;
        putback_pending_ = false;
        mouth_item_ = mouth_use_ = false;
        wipe_item_ = wipe_use_ = false;
        hold_item_ = hold_use_ = hold_spent_ = false;
        ClearGestures();
        pull_slot_ = -1;
        reopen_until_ = 0.0;
        hover_slot_ = last_hover_ = -1;
        HoldTriggerBack();
    }

    // quick_music_box_crank: the Tiny Music Box played by its crank (core/vr/music_box.h), through
    // the game's own UseGoods (never latched to its animation: the crank never presses Square).
    [[nodiscard]] static bool CrankItem(const Config& c, std::int32_t goods) {
        return c.crank && goods == 700;
    }

    // The music box in the hand: the presenter's crank went round (Input::crank_play): the game's
    // use at once (Request::crank: UseGoods, its bullet, the tune; nothing consumed), the box stays
    // in the hand (no Waiting: the crank keeps turning). The answer comes back as
    // Output::crank_answer (the presenter's feel).
    void CrankTick(const Config& c, const Input& in, Output& out) {
        if (crank_serial_ != 0 && in.have_result && in.result.serial == crank_serial_) {
            const bool used = in.result.outcome == Outcome::Used;
            out.crank_answer = used ? 1 : -1;
            out.Log(std::string{"MUSIC BOX answer: "} + OutcomeName(in.result.outcome) +
                    (in.result.reason != nullptr && in.result.reason[0] != 0
                         ? std::string{" ("} + in.result.reason + ")"
                         : std::string{}));
            if (!used) {
                out.Haptics(Haptic::Deny);
                pulse(held_slot_, true);
            }
            crank_played_ = crank_played_ || used;
            crank_serial_ = 0;
        }
        if (!in.crank_play) {
            return;
        }
        if (in.snapshot_fresh && !in.snapshot.slot[static_cast<std::size_t>(held_slot_)].usable) {
            out.crank_answer = -1;
            Deny(out, "MUSIC BOX: the game says it cannot be used now");
            return;
        }
        Request& r = Send(out, c.select && in.snapshot.selected != held_slot_ ? held_slot_ : -1,
                          Action::Use, in);
        r.crank = 1;
        r.dmy = 17; // the music box clip's (a00 100170) UseGoods dummy
        crank_serial_ = r.serial;
        last_use_ = in.t;
        out.Log("MUSIC BOX PLAY: goods " + std::to_string(held_goods_) +
                " used by the crank (the game's UseGoods, no animation)");
    }

    // The music box in the hand has played since it was taken (the game's answer Used, or a play
    // still waiting for it): put down, it is a use (the arc closed), not a cancel (2026-10-15).
    [[nodiscard]] bool CrankPlayed() const {
        return crank_item_ && (crank_played_ || crank_serial_ != 0);
    }

    void ClearGestures() {
        crank_item_ = false;
        crank_serial_ = 0;
        crank_played_ = false;
        gesture_ = GestureKind::None;
        gesture_pending_ = false;
        shake_shown_ = 0;
        stab_item_ = stab_use_ = false;
        stab_pending_t_ = -1.0;
    }

    bool holster_held_{false};
    bool holster_grip_ready_{false};

    void Reset(Output& out, const std::string& why) {
        out.Log("RESET (" + why + ") in " + PhaseName(phase_));
        Idle(out, false);
        regrab_slot_ = -1;
    }

    void Enter(Phase p) {
        if (p == Phase::Idle)
            holster_held_ = false;
        if (p != phase_) {
            phase_ = p;
            state_ns_ = steady_ns_;
        }
    }

    void pulse(std::int32_t slot, bool deny) {
        pulse_slot_ = static_cast<std::int8_t>(slot);
        pulse_deny_ = deny;
        pulse_ns_ = steady_ns_;
    }

    bool latched(std::int32_t goods) const {
        for (const std::int32_t g : latched_) {
            if (g == goods) {
                return true;
            }
        }
        return false;
    }
    void latch(std::int32_t goods) {
        latched_[latched_count_++ % latched_.size()] = goods;
    }

    void Publish(Output& out) {
        Scene& s = out.scene;
        s.open_serial = open_serial_;
        s.state = phase_ == Phase::Open ? SceneState::Open
                  : phase_ == Phase::Idle || phase_ == Phase::SquareWait
                      ? SceneState::Closed
                      : SceneState::Holding;
        s.state_ns = state_ns_;
        // The game shows the item in the hand while it is held, until the throw or use is
        // answered (a thrown item leaves the hand at once).
        const bool in_hand = (phase_ == Phase::Holding || phase_ == Phase::Committing ||
                              (phase_ == Phase::Waiting && !thrown_));
        s.cards = cards_;
        const bool reserved = held_last_ && (in_hand || phase_ == Phase::Waiting);
        s.reserved_last_slot = static_cast<std::int8_t>(reserved ? held_slot_ : -1);
        s.reserved_last_goods = reserved ? held_goods_ : -1;
        s.held_slot = static_cast<std::int8_t>(in_hand ? held_slot_ : -1);
        s.held_goods = in_hand ? held_goods_ : -1;
        s.pulse_slot = pulse_slot_;
        s.pulse_deny = pulse_deny_;
        s.pulse_ns = pulse_ns_;
        // quick_hold_use: the ring while the held item's button fills it; the flash once full.
        s.hold_from_ns = phase_ == Phase::Holding && hold_item_ && !hold_spent_ ? hold_from_ns_ : 0;
        s.hold_s = hold_len_;
        // (The flash is the last hold's: never on an item taken since, which would show it full.)
        s.hold_done_ns = hold_done_ns_ >= hold_from_ns_ ? hold_done_ns_ : 0;
        out.held_goods = in_hand ? held_goods_ : -1;
        out.held_ffx = in_hand ? held_ffx_ : -1;
        out.stab_item = phase_ == Phase::Holding && stab_item_;
        out.crank_box = phase_ == Phase::Holding && crank_item_;
        out.grabs = grabs_;
    }

    Phase phase_{Phase::Idle};
    std::int64_t steady_ns_{0};
    std::int64_t state_ns_{0};
    std::uint32_t open_serial_{0};
    std::uint32_t serial_{0};
    std::uint8_t cards_{0};
    bool y_down_{false};
    // What toggled the arc this frame, for the log: "Y" or "L3" (the left stick click's tap).
    const char* toggle_by_{"Y"};
    bool y_square_{false};
    bool grip_on_{false};
    bool grip_was_{false};
    bool grip_armed_{false};
    bool trigger_on_{false};
    bool trigger_was_{false};
    bool l2_was_{false};
    bool l2_fresh_needed_{false};
    bool panic_l2_{false};
    bool latch_now_{false};    // the trigger's latch this frame
    bool trigger_fist_{false}; // the use pull came with the grab's squeeze: no use until let go
    double parry_t_{-1.0};     // a fresh pull's latch (GunPull, ParryPull), -1 = none
    bool parry_fist_{false};   // ... is the fist, not the gun
    double regrab_l2_t_{-100.0}; // a pull while the stack's next one may be taken ...
    bool regrab_l2_wait_{false}; // ... with the grip moving: held back (up to fist_s)
    double open_t_{0.0};
    double last_activity_{0.0};
    double hover_lost_t_{0.0};
    double commit_t_{0.0};
    double wait_t_{0.0};
    double last_use_{-100.0};
    double grip_edge_t_{-100.0}; // the grip's last squeeze
    V3 open_hand_{};
    V3 activity_hand_{};
    // The stack (UpdateRegrab): that slot's next one for a squeeze after regrab_from_ until
    // regrab_until_.
    std::int32_t regrab_slot_{-1};
    std::int32_t regrab_goods_{-1};
    double regrab_from_{0.0};
    double regrab_until_{0.0};
    std::int32_t hover_slot_{-1};
    std::int32_t last_hover_{-1};
    bool hovered_any_{false}; // a card was lit since the open (the first detent is stronger)
    // A squeeze of the grip while the arc is open, from squeeze_t_ (UpdateOpen: it waits up to
    // grab_wait_s for a gate).
    bool squeeze_{false};
    bool squeeze_waited_{false};
    double squeeze_t_{0.0};
    bool held_last_{false};
    std::int32_t held_slot_{-1};
    std::int32_t held_goods_{-1};
    std::int32_t held_ffx_{-1};
    GoodsClass held_class_{};
    float suspended_{0.0f};
    bool thrown_{false};
    int implausible_logged_{0}; // throw_max_hand_speed lines logged for the item in the hand
    std::int8_t pulse_slot_{-1};
    bool pulse_deny_{false};
    std::int64_t pulse_ns_{0};
    std::array<std::int32_t, 8> latched_{-1, -1, -1, -1, -1, -1, -1, -1};
    std::size_t latched_count_{0};
    VelocityHistory history_{}; // the item's velocity (the grip's + its spin), display times
    VelocityHistory spin_{};    // the spin part (the log)
    VelocityHistory fresh_{};      // ... from the grip located at the input times (throw_fresh)
    VelocityHistory fresh_spin_{}; // ... its spin part (the log)
    // The grip opened at release_t_ (the game had the arm then: !release_arm_); the throw waits
    // for the hand's peak while release_pending_ (throw_rise_wait_s).
    double release_t_{0.0};
    bool release_arm_{true};
    bool release_pending_{false};
    WindupHistory windup_{};
    // left_scheme=2: the trigger holds the throwable it took (until trigger_release); a pull on
    // the card pull_slot_ waits for its dwell (hover_since_: when the lit card was reached;
    // last_dwelled_: the card the hand left had been lit dwell_s); after a throw the arc is back
    // until reopen_until_ (0: as one opened).
    bool trigger_hold_{false};
    std::int32_t pull_slot_{-1};
    double pull_t_{0.0};
    double pull_lit_from_{0.0}; // the pull's card was lit from then
    double hover_since_{0.0};
    bool last_dwelled_{false};
    double reopen_until_{0.0};
    V3 left_card_p_{};          // where the hand was when it left the last lit card (the log)
    // The hand's positions of the last ~0.5 s (left_scheme=2: a pull's jerk against a hand that
    // moved on).
    struct HandTrack {
        struct Sample {
            double t{0.0};
            V3 p{};
        };
        std::array<Sample, 64> s{};
        std::size_t head{0};
        std::size_t count{0};
        void Push(double t, V3 p) {
            s[head] = {t, p};
            head = (head + 1) % s.size();
            count = std::min(count + 1, s.size());
        }
        // The farthest the hand was from `now` at any sample since `since` (0: none).
        [[nodiscard]] float TravelSince(double since, V3 now) const {
            float most = 0.0f; // (not "far": a macro in the Windows headers)
            for (std::size_t k = 0; k < count; ++k) {
                const Sample& x = s[(head + s.size() - 1 - k) % s.size()];
                if (x.t < since - 1e-6) {
                    break;
                }
                most = std::max(most, Length(now - x.p));
            }
            return most;
        }
    };
    HandTrack hand_track_{};
    // The grip's put-back (PutBack2): pending since putback_t_; fast_t_: the hand last moved at
    // putback_max_speed or faster then.
    bool putback_pending_{false};
    double putback_t_{0.0};
    double fast_t_{-100.0};
    // quick_use_mouth: the item in the hand waits for the mouth (mouth_item_; MouthGate), and the
    // pending use was asked for there (mouth_use_: its answer is a gulp or a bite).
    bool mouth_item_{false};
    bool mouth_use_{false};
    MouthGate mouth_gate_{};
    // quick_use_wipe: the item in the hand waits for a stroke along the blade (wipe_item_;
    // WipeDetector on the contact samples, wipe_serial_ the last one read), and the pending
    // use was asked for there (wipe_use_: its answer is the ignite thump).
    bool wipe_item_{false};
    bool wipe_use_{false};
    WipeDetector wipe_{};
    std::uint32_t wipe_serial_{0};
    bool wipe_blade_{false}; // a blade to wipe on was measured since the take
    bool wipe_cue_logged_{false}; // the find cue's start logged for this take
    int wipe_moves_logged_{0};    // WipeDetector::BladeMoves() last logged
    // The rub's log (RubLog): this touch's start and its refused strokes then, the refused strokes
    // logged and when, near the blade since.
    double rub_touch_t_{0.0};
    int rub_touch_refused_{0};
    int rub_refused_logged_{0};
    double rub_refused_log_t_{-1.0};
    double rub_near_since_{-1.0};
    double rub_near_log_t_{-1.0};
    static constexpr double WipeNoBladeS = 0.5;
    static constexpr double WipeGapS = 0.4; // a game hitch mid-stroke up to this keeps it
    // quick_hold_use: the item in the hand is used by holding it (hold_item_): its ring fills
    // from hold_from_ (hold_from_ns_) over hold_len_ s; hold_lost_: how long the game has held
    // the arm meanwhile; hold_use_: the pending use is the ring's (its answer has no feel of its
    // own: the thump was it); hold_spent_: refused at full, nothing until let go; hold_done_ns_:
    // the last ring full (the flash).
    bool hold_item_{false};
    bool hold_use_{false};
    bool hold_spent_{false};
    // quick_music_box_crank: the music box in the hand (crank_item_) and its play's request.
    bool crank_item_{false};
    std::uint32_t crank_serial_{0};
    bool crank_played_{false}; // it played (the game's answer Used) since it was taken
    double hold_from_{0.0};
    std::int64_t hold_from_ns_{0};
    float hold_len_{1.5f};
    double hold_lost_{0.0};
    int hold_lost_frames_{0};
    std::int64_t hold_done_ns_{0};
    // quick_use_crush / quick_use_shake: the held item's gesture (None: the hold only), its
    // detectors, the ring's unstepped start (hold_from_base_: the time held counts from it, the
    // shake's steps move hold_from_ only), the strokes the ring shows, and a gesture done within
    // the cooldown (gesture_pending_: completed the first frame it may be, with pending_*).
    GestureKind gesture_{GestureKind::None};
    CrushGate crush_{};
    ShakeGate shake_{};
    int shake_shown_{0};
    double hold_from_base_{0.0};
    std::int64_t hold_from_base_ns_{0};
    bool gesture_pending_{false};
    Haptic pending_feel_{Haptic::None};
    std::string pending_how_;
    // quick_use_stab: the blood in the hand waits for a stab (stab_item_); one stabbed within the
    // cooldown waits for it (stab_pending_t_, into stab_zone_); the pending use was stabbed
    // (stab_use_: its answer is the vial's THUMP).
    bool stab_item_{false};
    bool stab_use_{false};
    double stab_pending_t_{-1.0};
    std::string stab_zone_;
    std::uint32_t grabs_{0}; // + 1 per take (Output::grabs)
    double last_t_{-1.0}; // the last Update's time (-1: none yet; resync_gap_s)
    ThrowTrace trace_{};        // VR_THROW_TRACE only
    std::uint32_t trace_id_{0};
    bool trace_pending_{false}; // a release's samples wait for its follow-through ...
    double trace_t0_{0.0};      // ... (the release's input frame)
    std::string trace_head_;    // ... and its summary line
};

} // namespace VR::Quick
