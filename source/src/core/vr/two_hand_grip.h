// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "core/vr/arm_ik.h"

// Two-handed grip (vr.ini two_hand_grip=1). With the right weapon in one of the game's two-handed
// forms (arm style 3: the Hunter Axe, Kirkhammer, Ludwig's Holy Blade, Burial Blade, Holy
// Moonlight Sword, Church Pick, Whirligig Saw ... B; the gun is holstered there), reach the left
// hand onto the weapon's handle and squeeze the left grip: the hunter's left hand closes on the
// handle and, while you hold the grip, the weapon's direction comes from both hands (the right
// hand keeps the position; wrist roll blends 70% right / 30% left). two_hand_relative=1 (the default): the weapon keeps the
// angle it had at the grab and turns as the line between your hands turns from there (no jump at
// the grab); 0: the handle runs from the right hand to the left one.
//
// Never by accident:
//  - Only a squeeze counts (the grip's edge, the quick items' 0.75 / 0.45 hysteresis), with the
//    left hand inside the handle zone at that moment: a grip already held that slides onto the
//    handle never grabs it.
//  - Not with an item in the hand, a Blood Vial armed or going in, a grip
//    held through a menu or the screen, a hand not tracked, or a one-handed form (there the left
//    hand stays the gun hand and the zone does nothing).
//  - A squeeze it takes is its own until you let go: the quick items never see it (no arc, no
//    belt vial). An open, empty arc closes on acquisition. Elsewhere the left grip is unchanged
//    (the arc, the belt vial at the hip).
// Release: both grip buttons open. Distance and twist never break a held grip during a swing.
// A form change, an item taking the hand, a context switch or prolonged tracking loss still
// clears the attachment.
//
// The handle: the game's own two-handed grip, L_Weapon in R_Weapon's frame from the game's
// two-handed idle / walk animation (measured each pose copy by the VR body, smoothed), or
// DefaultGrip until a plausible one was seen. A continuous grip rail extends through the
// right-hand anchor in both directions. The left hand grabs and slides along it (SupportGrip);
// the native pose supplies its axis and approximate extent, never a mandatory attachment point.
//
// Pure (no OpenXR, Vulkan or emulator state): unit-tested in tests/test_two_hand_grip.cpp; the
// presenter runs the tracker on the input thread and Apply on the published hands
// (openxr_presenter.cpp), the body measures the game's grip (bloodborne_body.cpp,
// core/vr/two_hand_channel.h), the melee damage hook applies the two-handed swing bonus.
namespace VR::TwoHand {

using Body::Quat;
using Body::Vec3;

struct Settings {
    bool enabled{true};          // two_hand_grip
    bool guide{true};            // two_hand_guide: right-grip handle cue
    float zone_m{0.09f};         // two_hand_zone_cm: the handle zone's radius
    float release_m{0.25f};      // legacy two_hand_release_cm (no geometric release)
    float twist_deg{75.0f};      // legacy two_hand_twist_deg (no geometric release)
    float damage{1.05f};         // two_hand_damage: a two-handed swing's hits x this
    float blend_s{0.10f};        // two_hand_blend_ms: the hand onto / off the handle
    float swing_left_mps{0.8f};  // two_hand_swing_speed: the left hand this fast in the swing
    bool haptics{true};          // two_hand_haptics
    float swing_hum{1.0f};       // two_hand_swing_hum: 0 off .. 1 faint full strength
    // two_hand_relative (the feel review, 2026-10-06): the weapon keeps the angle it had against
    // the right hand at the grab, and the left hand steers it from there (as Blade & Sorcery's
    // second hand on a shaft). 0: the handle snaps onto the line between the hands at the grab,
    // which turned the blade by the hand's offset off the handle (the mock's eye dump: 17 deg;
    // a hand 6 cm off the line 10-15 cm down it: 22-31 deg, 9 cm off 5 cm down: 61 deg).
    bool relative{true};
    float press{0.75f};          // the grip's squeeze (as the quick items')
    float release{0.45f};        // ... and its let-go
    float lost_s{0.5f};          // the weapon hand lost this long ends it
};

// ---- The game's two-handed grip -----------------------------------------------------------------

// The left weapon bone (L_Weapon) in the right one's (R_Weapon) frame: translation in game units,
// rotation.
struct GameGrip {
    bool valid{false};
    Vec3 t{};
    Quat q{};
};

// Before the game's grip was measured: 14 cm down the handle (R_Weapon +Y: the pommel side; the
// blade runs along -Y), the left fist turned as the right one (the two hands mirror each other
// round the handle: the left bone is the right one's mirror image).
inline constexpr GameGrip DefaultGrip{true, {0.0f, 0.14f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};

constexpr float NearExtraM = 0.06f;   // a near miss: this much outside the zone (Input::hold_near)
constexpr float AxisFadeM = 0.03f;    // the hands this close: the right hand's own direction
constexpr float AxisFullM = 0.06f;    // ... from here the hands' line
// The crossing zone round the right fist (SupportGrip): the left hand this close to the right one
// is going round it (the controllers cannot pass through each other), not steering: a 9 cm
// detour at that distance would swing the weapon 60-90 degrees. The weapon keeps its angle
// there, whichever side the hand goes round (lean the weapon to make room on that side);
// steering takes over again from where the hand comes out, fully from CrossOuterM.
constexpr float CrossInnerM = 0.10f;
constexpr float CrossOuterM = 0.14f;
constexpr float GameGripMinM = 0.06f; // a plausible game grip: the left fist this far ...
constexpr float GameGripMaxM = 0.75f; // ... to this far from the right one
constexpr float GameGripConeDeg = 40.0f; // ... along the handle (R_Weapon +-Y)

// Families whose two-handed form has no single handle for both hands: dual blades (Blade of
// Mercy 40, Rakuyo 270), the bow (Simon's Bowblade 320), claws and fists; and the Bloodletter
// (290), whose transformed form is held in one hand in VR (design choice, 2026-10-13).
constexpr bool SharedHandle(std::int32_t family) {
    switch (family) {
    case 10:  // Bare Fists
    case 40:  // Blade of Mercy
    case 90:  // Beast Claw
    case 270: // Rakuyo
    case 290: // Bloodletter
    case 320: // Simon's Bowblade
    case 340: // Fist of Gratia
    case 370:
    case 380: // Kos Parasite
    case 390:
    case 400:
    case 410:
        return false;
    default:
        return family >= 0;
    }
}

// The arm style of TAE event 32 for a two-handed form B (core/vr/transform.h StyleFormBTwoHanded).
constexpr std::int32_t StyleTwoHanded = 3;

constexpr bool TwoHandedForm(std::int32_t family, std::int32_t style) {
    return style == StyleTwoHanded && SharedHandle(family);
}

// A grip read from the game's animation is the left hand on the handle: 6-75 cm from the right
// fist (game units / `scale`), within 40 degrees of the handle's line (R_Weapon +-Y).
inline bool Plausible(const GameGrip& g, float scale, bool axial = true) {
    if (!g.valid || !Body::IsFinite(g.t) || !Body::IsFinite(g.q) || !(scale > 1e-3f)) {
        return false;
    }
    const float length = Body::Length(g.t) / scale;
    if (length < GameGripMinM || length > GameGripMaxM) {
        return false;
    }
    const float cos_cone = std::cos(GameGripConeDeg * Body::Deg);
    return (!axial || std::abs(g.t.y) / Body::Length(g.t) >= cos_cone) &&
           Body::Dot(g.q, g.q) > 0.5f;
}

// The measurement smoothed: each new plausible reading moves it by 1 - exp(-dt / tau).
struct GripFilter {
    GameGrip grip{};
    std::uint32_t readings{0};

    void Add(const GameGrip& reading, float dt, float tau = 0.3f) {
        if (!grip.valid) {
            grip = reading;
            grip.q = Body::Normalize(reading.q);
            readings = 1;
            return;
        }
        const float k = tau > 0.0f ? 1.0f - std::exp(-std::max(dt, 0.0f) / tau) : 1.0f;
        grip.t = Body::Lerp(grip.t, reading.t, k);
        grip.q = Body::Slerp(grip.q, Body::Normalize(reading.q), k);
        ++readings;
    }
    void Reset() {
        *this = {};
    }
};

// Remember the native grip of each weapon independently. The first samples after a form
// switch are blended animations, so only accept a stable run of native poses. No axial cone:
// curved and offset handles (notably the Wheel) are valid native two-handed grasps too.
class GripCache {
public:
    void Select(std::int32_t family, std::int32_t style, float dt) {
        if (family != family_ || style != style_) {
            family_ = family;
            style_ = style;
            settle_s_ = 0.0f;
            candidate_.Reset();
        }
        settle_s_ += std::max(dt, 0.0f);
    }
    void Observe(const GameGrip& reading, float scale, float dt, bool eligible) {
        if (!eligible || settle_s_ < 0.35f || !Plausible(reading, scale, false)) {
            candidate_.Reset();
            return;
        }
        if (candidate_.grip.valid &&
            (Body::Length(candidate_.grip.t - reading.t) > 0.015f * scale ||
             Body::AngleBetween(candidate_.grip.q, reading.q) > 10.0f * Body::Deg)) {
            candidate_.Reset();
        }
        candidate_.Add(reading, dt);
        if (candidate_.readings >= 5) {
            for (auto& entry : entries_) {
                if (entry.family == family_ || entry.family < 0) {
                    entry = {family_, candidate_.grip};
                    return;
                }
            }
        }
    }
    GameGrip Get(float scale, bool& measured) const {
        for (const auto& entry : entries_) {
            if (entry.family == family_ && family_ >= 0) {
                measured = true;
                return entry.grip;
            }
        }
        measured = false;
        GameGrip fallback = DefaultGrip;
        // DefaultGrip is metres; measured poses are game units.
        fallback.t = fallback.t * std::max(scale, 1e-3f);
        return fallback;
    }
    std::uint32_t Readings() const { return candidate_.readings; }
private:
    struct Entry { std::int32_t family{-1}; GameGrip grip{}; };
    std::array<Entry, 48> entries_{};
    std::int32_t family_{-1};
    std::int32_t style_{-1};
    float settle_s_{0.0f};
    GripFilter candidate_{};
};

// ---- Frames ---------------------------------------------------------------------------------------

// OpenXR grip frame <-> the game's controller frame (core/vr/body_frame.h DefaultGrip: +Z = grip
// -Z): the z flip, and the same conjugation of a rotation (TrackingToWorldRotation's P).
inline Vec3 FlipZ(Vec3 v) {
    return {v.x, v.y, -v.z};
}
inline Quat PConj(const Quat& q) {
    return {-q.x, -q.y, q.z, q.w};
}

// A weapon bone's frame on its controller: `grip` (weapon bone -> controller frame, DefaultGrip
// with the trims) and `offset` (metres, OpenXR grip frame: TrimOffset).
struct BoneOnGrip {
    Quat grip{};
    Vec3 offset{};
};

// A point `x` in the weapon bone's frame (game units) in its controller's OpenXR grip frame (m).
inline Vec3 BoneToGripLocal(const BoneOnGrip& b, Vec3 x, float scale) {
    const float inv = scale > 1e-6f ? 1.0f / scale : 1.0f;
    return b.offset + FlipZ(Body::Rotate(b.grip, x)) * inv;
}

struct Pose {
    Vec3 p{}; // tracking space (m)
    Quat q{};
};

// The weapon bone's origin of a controller pose (tracking).
inline Vec3 BoneOrigin(const Pose& hand, const BoneOnGrip& b) {
    return hand.p + Body::Rotate(Body::Normalize(hand.q), b.offset);
}

// ---- The handle ---------------------------------------------------------------------------------

// The handle for this right controller pose (tracking space): from the right fist (R_Weapon's
// origin) to the game's left grip point.
struct Handle {
    bool valid{false};
    Vec3 right{};    // the right weapon bone's origin
    Vec3 left{};     // the game's left grip point (where the hunter's left fist goes)
    Vec3 dir{};      // unit, right -> left
    float length{0}; // m
    // How far past the right fist toward the blade the support hand may hold (m): the bare
    // shaft before the striking part (BladeClearCache). 0: none (a sword's blade, or not known).
    float blade_clear{0.0f};
};

inline Handle HandleAt(const Pose& right, const BoneOnGrip& r, const GameGrip& g, float scale) {
    Handle h{};
    const GameGrip& use = g.valid ? g : DefaultGrip;
    const Quat q = Body::Normalize(right.q);
    h.right = BoneOrigin(right, r);
    const Vec3 local = BoneToGripLocal(r, use.t, scale) - r.offset;
    const Vec3 to_left = Body::Rotate(q, local);
    h.length = Body::Length(to_left);
    if (!(h.length > 1e-3f) || !Body::IsFinite(to_left)) {
        return h;
    }
    h.left = h.right + to_left;
    h.dir = to_left * (1.0f / h.length);
    h.valid = true;
    return h;
}

// The grip rail in metres along Handle::dir. The native grip gives the shaft's direction and
// the pommel-side end (the game's left fist sits at the end of the grip: a fist's width past it
// is air). Toward the blade the rail runs only over bare shaft (Handle::blade_clear, at most
// 30 cm): a polearm's haft lets the left hand lead, a sword's blade never takes a hand.
inline constexpr float RailPommelM = 0.04f;
inline constexpr float RailBladeMaxM = 0.30f;
struct RailRange { float lo{}, hi{}; };
inline RailRange RailOf(const Handle& h) {
    return {-std::clamp(h.blade_clear, 0.0f, RailBladeMaxM),
            std::clamp(h.length + RailPommelM, 0.10f, GameGripMaxM + RailPommelM)};
}

// A hand's half width: the rail's blade side stops this short of the striking part.
inline constexpr float HandHalfM = 0.06f;

// The blade-side reach from the striking part's distance from the grip (m, player scale).
inline float BladeClearOf(float strike_near_m) {
    return std::isfinite(strike_near_m) ? std::max(0.0f, strike_near_m - HandHalfM) : 0.0f;
}

// Each weapon's blade-side reach, learned from its two-handed form (core/vr/combat_channel.h
// MeleeStatus::blade_strike_near) and kept for its form A (a grab that transforms).
class BladeClearCache {
public:
    void Set(std::int32_t family, float clear_m) {
        if (family < 0 || !std::isfinite(clear_m)) {
            return;
        }
        for (auto& e : entries_) {
            if (e.family == family || e.family < 0) {
                e = {family, std::max(clear_m, 0.0f)};
                return;
            }
        }
    }
    float Get(std::int32_t family) const {
        for (const auto& e : entries_) {
            if (e.family == family && family >= 0) {
                return e.clear;
            }
        }
        return 0.0f;
    }
private:
    struct Entry { std::int32_t family{-1}; float clear{0.0f}; };
    std::array<Entry, 48> entries_{};
};

// `along` is signed distance along the rail; `off` is distance to its bounded segment.
struct Zone {
    bool inside{false};
    float along{0.0f};
    float off{0.0f};
};

inline Zone ZoneOf(const Handle& h, Vec3 point, float zone_m) {
    Zone z{};
    if (!h.valid || !Body::IsFinite(point)) {
        z.off = 1e9f;
        return z;
    }
    const Vec3 d = point - h.right;
    z.along = Body::Dot(d, h.dir);
    const auto range = RailOf(h);
    z.off = Body::Length(point - (h.right + h.dir * std::clamp(z.along, range.lo, range.hi)));
    // Not on the right fist itself: the hands together there steer nothing (AxisFullM), and a
    // left squeeze next to the right hand stays the quick items'.
    z.inside = z.off <= zone_m && std::abs(z.along) >= AxisFullM;
    return z;
}

// ---- The two-handed poses -------------------------------------------------------------------------

struct Applied {
    Pose right{};          // the right controller pose to publish (its bone stays where it was)
    Pose left{};           // the left controller pose that puts the left bone on the handle
    float turn_deg{0.0f};  // how far the right hand's handle was turned onto the hands' line
    float hands_m{0.0f};   // the two hands' distance (bone origins)
    Quat turn_local{};     // the turn in the right controller's own frame (right.q * turn_local)
};

// The poses for the right controller turned by `turn_local` (its own frame, about its weapon
// bone's origin) with weight `w`: the left one placed where the game's grip puts it on that
// turned handle, the real left hand `left` blended toward it by `w`.
inline Applied Place(const Pose& right, const Pose& left, const BoneOnGrip& r, const BoneOnGrip& l,
                     const GameGrip& g, float scale, float w, const Quat& turn_local) {
    Applied out{};
    out.right = {right.p, Body::Normalize(right.q)};
    out.left = {left.p, Body::Normalize(left.q)};
    w = std::clamp(std::isfinite(w) ? w : 0.0f, 0.0f, 1.0f);
    const GameGrip& use = g.valid ? g : DefaultGrip;
    const Vec3 bone_r = BoneOrigin(right, r);
    out.hands_m = Body::Length(BoneOrigin(left, l) - bone_r);
    if (w <= 0.0f) {
        return out;
    }
    const Quat qr = Body::Normalize(out.right.q * Body::ScaleRotation(Body::Normalize(turn_local), w));
    out.turn_local = Body::Normalize(turn_local);
    out.turn_deg = Body::AngleOf(out.turn_local) / Body::Deg;
    out.right.q = qr;
    out.right.p = bone_r - Body::Rotate(qr, r.offset);
    // The left bone on the turned handle, turned as the game's grip turns it against the right.
    const Vec3 local = BoneToGripLocal(r, use.t, scale) - r.offset;
    const Vec3 bone_l = bone_r + Body::Rotate(qr, local);
    const Quat ql = Body::Normalize(qr * PConj(Body::Normalize(r.grip * use.q *
                                                               Body::Conjugate(l.grip))));
    const Vec3 pl = bone_l - Body::Rotate(ql, l.offset);
    out.left.p = Body::Lerp(left.p, pl, w);
    out.left.q = Body::Slerp(out.left.q, ql, w);
    return out;
}

// Both hands are on the weapon and either can slide along it: the hand that moves along the weapon
// slides, the one that stays still holds it. A change of the hands' spacing goes to each hand in
// proportion to how much it moved along the weapon in the room (smoothed: no switching, nothing to
// flicker; both moving together, a swing or a step, slides nothing). The left hand's slide is
// `Slide` (its distance from the right one); the right hand's place on the weapon is `RightSlide`
// (from the game's own grip point): the weapon moves in the hunter's right fist (WeaponShift),
// staying where it is in the room. Walls, never springs: both hands stay on the rail, the upper
// hand leaves UpperRoomM for the lower one at the pommel end, and on a sword the right hand goes
// no lower than the game's own left grip. While the weapon swings, nothing slides. Direction is
// an unoriented line: crossing the right fist changes the signed slide coordinate, not the
// blade's direction. Grip buttons still own release.
class SupportGrip {
public:
    struct Input {
        bool held{false};
        bool ready{false}; // transformed form settled and native rail known
        bool tracked{false};
        std::int32_t family{-1};
        GameGrip target{};
        float scale{1.0f};
        float blade_clear{0.0f}; // Handle::blade_clear
        Pose right{}, left{};
        BoneOnGrip right_bone{}, left_bone{};
        float swing_mps{0.0f}; // the weapon's speed (the blade's lever point, head-relative)
    };
    // Each hand's speed along the weapon (which of them slides) is smoothed over this.
    static constexpr float MotionTauS = 0.03f;
    // Room the upper hand leaves for the lower one at the rail's pommel end.
    static constexpr float UpperRoomM = 0.10f;
    // The weapon swinging: no hand slides (it is the swing, not a reposition). On from SwingOnMps,
    // off again once slower than SwingOffMps for SwingCalmS.
    static constexpr float SwingOnMps = 0.8f;
    static constexpr float SwingOffMps = 0.5f;
    static constexpr float SwingCalmS = 0.15f;
    static constexpr float SlideDeadbandM = 0.002f;
    static constexpr float SlideTauS = 0.018f;
    // One Euro steering: quiet aiming retains roughly the old 35 ms time constant;
    // angular speed (rad/s) raises the cutoff for responsive push-pull swings.
    static constexpr float SteerMinHz = 4.5f;
    static constexpr float SteerSpeedBeta = 4.0f; // Hz per rad/s
    static constexpr float SteerDerivativeHz = 10.0f;
    static constexpr float SteerMaxHz = 60.0f;
    // The left wrist's share of the blade's roll, and its bound: a re-grip or a wrist turned
    // round never takes the blade further than this from the right hand's own roll.
    static constexpr float RollShare = 0.3f;
    static constexpr float RollMaxRad = 25.0f * Body::Deg;

    bool Update(const Settings& s, const Input& in, float dt) {
        if (family_ != in.family) {
            Reset();
            family_ = in.family;
        }
        dt_ = std::isfinite(dt) ? std::clamp(dt, 0.0f, 0.05f) : 0.0f;
        if (!in.held || !in.ready || !in.tracked || !std::isfinite(dt) || dt <= 1e-6f ||
            dt > 0.05f) {
            ResetSteeringMotion();
            roll_sample_ok_ = false;
        }
        const float rate = s.blend_s > 1e-3f ? dt_ / s.blend_s : 1.0f;
        const float speed = std::isfinite(in.swing_mps) ? in.swing_mps : 0.0f;
        if (speed >= SwingOnMps) {
            swinging_ = true;
            calm_s_ = 0.0f;
        } else if (swinging_) {
            calm_s_ = speed < SwingOffMps ? calm_s_ + dt_ : 0.0f;
            swinging_ = calm_s_ < SwingCalmS;
            if (!swinging_) {
                // The swing's own hand motion says nothing about who slides: the hands' catch-up
                // after it is the left hand's (the weapon never creeps in the right fist).
                right_motion_ = left_motion_ = 0.0f;
            }
        }
        if (!in.held) {
            seated_ = false;
            blend_ = std::max(0.0f, blend_ - rate);
            return false;
        }
        if (!in.ready) {
            seated_ = false;
            blend_ = 0.0f;
            return false;
        }
        bool grabbed = false;
        if (!seated_ && in.tracked && Plausible(in.target, in.scale, false)) {
            Handle rail = HandleAt(in.right, in.right_bone, in.target, in.scale);
            rail.blade_clear = in.blade_clear;
            const Vec3 hand = BoneOrigin(in.left, in.left_bone);
            const Zone zone = ZoneOf(rail, hand, s.zone_m);
            if (zone.inside) {
                // A re-grab while the last hold still eases out keeps the weapon where it is
                // shown: steering then takes it on from there (no snap back to the controller).
                const Quat carried = blend_ > 0.0f ? Combined() : Quat{};
                // ... and the weapon where it is in the right fist (else the game's grip point).
                right_slide_ = blend_ > 0.0f ? right_slide_ : 0.0f;
                seated_ = grabbed = true;
                grip_ = in.target;
                scale_ = in.scale;
                range_ = RailOf(rail);
                bone_axis_ = Body::Normalize(in.target.t);
                axis_ = Body::Normalize(BoneToGripLocal(in.right_bone, bone_axis_, 1.0f) -
                                        in.right_bone.offset);
                const Quat back = Body::Conjugate(Body::Normalize(in.right.q));
                const Vec3 local = Body::Rotate(back, hand - rail.right);
                slide_ = std::clamp(zone.along, range_.lo - right_slide_, range_.hi - right_slide_);
                grip_.t = bone_axis_ * (slide_ * scale_);
                // The upper end of the rail (toward the blade, R_Weapon -Y) and the walls.
                const Vec3 blade = BoneToGripLocal(in.right_bone, {0, -1, 0}, 1.0f) -
                                   in.right_bone.offset;
                pommel_hi_ = Body::Dot(axis_, blade) <= 0.0f; // the rail's hi end is the pommel
                native_m_ = rail.length;
                sword_ = in.blade_clear < AxisFadeM; // no bare shaft toward the blade
                motion_ok_ = false;
                right_motion_ = left_motion_ = 0.0f;
                // Preserve the grab's lateral offset when steering: sliding straight along
                // an off-centre grab must not slowly rotate the weapon as separation changes.
                relative_ = s.relative;
                lateral_ = relative_ ? local - axis_ * zone.along : Vec3{};
                turn_ = carried;
                roll_baseline_ = Body::Normalize(back * Body::Normalize(in.left.q));
                roll_delta_ = roll_previous_ = 0.0f;
                roll_sample_ok_ = true;
                ResetSteeringMotion();
                grip_.q = Body::Normalize(Body::Conjugate(in.right_bone.grip) *
                    PConj(back * Body::Normalize(in.left.q)) * in.left_bone.grip);
            }
        }
        blend_ = seated_ ? std::min(1.0f, blend_ + rate) : 0.0f;
        return grabbed;
    }
    // `dt`: since the last ApplyTracked (the published poses' own clock, not the input's).
    Applied ApplyTracked(const Pose& right, const Pose& left, const BoneOnGrip& r,
                         const BoneOnGrip& l, float dt) {
        const bool dt_ok = std::isfinite(dt) && dt > 1e-6f && dt <= 0.05f;
        apply_dt_ = dt_ok ? dt : 0.0f;
        if (!dt_ok) {
            ResetSteeringMotion();
        }
        const Quat q = Body::Normalize(right.q);
        const Vec3 local = Body::Rotate(Body::Conjugate(q),
                                        BoneOrigin(left, l) - BoneOrigin(right, r));
        if (seated_ && Body::IsFinite(local)) {
            const Vec3 previous_axis = Body::Rotate(turn_, axis_);
            if (relative_) {
                // Round the right fist the hand's offset off the shaft becomes its new grab
                // offset (fully inside CrossInnerM): going round the right hand turns nothing,
                // and steering resumes from wherever the hand comes out.
                // The hands' own distance, not along the weapon: the weapon turns to follow the
                // hand, so its distance along the shaft would never shrink on the way round.
                const Vec3 hand = Body::Rotate(Body::Conjugate(turn_), local);
                const float along_now = Body::Dot(hand, axis_);
                const float hold = 1.0f - Body::Smoothstep(CrossInnerM, CrossOuterM,
                                                           Body::Length(local));
                if (hold > 0.0f) {
                    lateral_ = Body::Lerp(lateral_, hand - axis_ * along_now, hold);
                }
            }
            const Vec3 aim = local - Body::Rotate(turn_, lateral_);
            const float along = Body::Dot(aim, previous_axis);
            // Suppress the singularity when the hands meet or cross perpendicular to the
            // shaft. Keep the previous turn there; never choose a new 180-degree direction.
            const float steer = Body::Smoothstep(AxisFadeM, AxisFullM, std::abs(along));
            if (steer > 0.0f && Body::Length(aim) > 1e-5f) {
                const Vec3 directed = Body::Normalize(aim) * (along < 0.0f ? -1.0f : 1.0f);
                const Quat desired = Body::FromTo(axis_, directed);
                // Ambiguous hand spacing must not boost the adaptive cutoff. Measure
                // speed only after choosing the same direction on the unoriented line.
                float alpha;
                if (steer < 1.0f) {
                    ResetSteeringMotion();
                    alpha = LowPassAlpha(SteerMinHz, apply_dt_);
                } else {
                    alpha = SteeringAlpha(directed);
                }
                turn_ = Body::Slerp(turn_, desired, alpha * steer);
            } else {
                ResetSteeringMotion();
            }
            const Vec3 displayed_axis = Body::Rotate(Body::ScaleRotation(turn_, Weight()), axis_);
            // Each hand's motion along the weapon in the room (its share of a change of spacing).
            const Vec3 right_at = BoneOrigin(right, r), left_at = BoneOrigin(left, l);
            const Vec3 along_w = Body::Rotate(q, displayed_axis);
            if (motion_ok_ && apply_dt_ > 0.0f) {
                const float k = 1.0f - std::exp(-apply_dt_ / MotionTauS);
                const float vr = std::abs(Body::Dot(right_at - right_prev_, along_w)) / apply_dt_;
                const float vl = std::abs(Body::Dot(left_at - left_prev_, along_w)) / apply_dt_;
                right_motion_ += (vr - right_motion_) * k;
                left_motion_ += (vl - left_motion_) * k;
            }
            right_prev_ = right_at;
            left_prev_ = left_at;
            motion_ok_ = Body::IsFinite(right_at) && Body::IsFinite(left_at);
            if (!swinging_) {
                // The spacing follows the hands (the slide filter: no creep from jitter) ...
                const float wanted = Body::Dot(local, displayed_axis);
                const float before = slide_;
                const float delta = wanted - slide_;
                float spaced = slide_;
                if (std::abs(delta) > SlideDeadbandM) {
                    const float quiet_delta = delta - std::copysign(SlideDeadbandM, delta);
                    spaced += quiet_delta * (1.0f - std::exp(-apply_dt_ / SlideTauS));
                }
                // ... and its change is the moving hand's: the right hand's share moves its place
                // on the weapon (the weapon stays in the room), the left hand's moves the left's.
                const float share = right_motion_ / (right_motion_ + left_motion_ + 1e-3f);
                PlaceHands(right_slide_ - share * (spaced - before), spaced);
            }
            grip_.t = bone_axis_ * (slide_ * scale_);
        } else {
            ResetSteeringMotion();
        }
        if (seated_ && Body::IsFinite(right.q) && Body::IsFinite(left.q)) {
            // Compare wrists in the steered handle frame, relative to their grab pose.
            // Right roll already contributes 100%; adding 30% of (left - right) gives 70/30.
            const Quat relative = Body::Normalize(Body::Conjugate(q * turn_) *
                Body::Normalize(left.q) * Body::Conjugate(roll_baseline_));
            const float along = Body::Dot(Vec3{relative.x, relative.y, relative.z}, axis_);
            if (along * along + relative.w * relative.w > 1e-4f) {
                const float raw = Body::TwistAngle(relative, axis_);
                if (roll_sample_ok_) {
                    // Saturating: turning back undoes the roll at once, however far past
                    // the bound the wrist went.
                    const float bound = RollMaxRad / RollShare;
                    roll_delta_ = std::clamp(roll_delta_ + Body::WrapPi(raw - roll_previous_),
                                             -bound, bound);
                }
                roll_previous_ = raw;
                roll_sample_ok_ = true;
            } else {
                roll_sample_ok_ = false; // undefined swing/twist split; resume without a jump
            }
        }
        const Quat combined = Combined();
        Applied a = Place(right, left, r, l, grip_, scale_, Weight(), combined);
        // Keep the real left wrist while its axial twist contributes to the blade's roll.
        const Vec3 bone = BoneOrigin(a.left, l);
        a.left.q = Body::Normalize(left.q);
        a.left.p = bone - Body::Rotate(a.left.q, l.offset);
        if (Weight() > 0.0f) {
            grip_.q = Body::Normalize(Body::Conjugate(r.grip) *
                PConj(Body::Conjugate(a.right.q) * a.left.q) * l.grip);
        }
        return a;
    }
    void Reset() { *this = {}; }
    bool Seated() const { return seated_; }
    float Weight() const { return Body::Smoothstep(0.0f, 1.0f, blend_); }
    const GameGrip& Grip() const { return grip_; }
    float Scale() const { return scale_; }
    float Slide() const { return slide_; }
    float RightSlide() const { return right_slide_; }
    bool Swinging() const { return swinging_; }
    // How much of a change of the hands' spacing the right hand takes now (0: the left slides).
    float RightShare() const { return right_motion_ / (right_motion_ + left_motion_ + 1e-3f); }
    // The weapon's offset in the hunter's right fist, eased with the hold: in the right weapon
    // bone's own frame (game units), and in the right controller's grip frame (metres).
    Vec3 WeaponShift() const { return bone_axis_ * (-right_slide_ * scale_ * Weight()); }
    Vec3 WeaponShiftGrip() const { return axis_ * (-right_slide_ * Weight()); }
private:
    // The hands' places on the weapon (from the game's grip point along axis_): the right one at
    // `right_on`, the left one `spacing` from it, against the walls. The right hand at a wall stops
    // sliding and carries the weapon on (the spacing, so the left hand's place, follows the real
    // hands); the left hand at a wall stops there on the weapon.
    void PlaceHands(float right_on, float spacing) {
        const float lo = range_.lo, hi = range_.hi;
        // The upper hand (the one further from the pommel end) leaves room for the lower one.
        const float room_at = pommel_hi_ ? hi - UpperRoomM : lo + UpperRoomM;
        const auto above = [&](float a, float b) { return pommel_hi_ ? a < b : a > b; };
        const auto past_room = [&](float on) { return pommel_hi_ ? on > room_at : on < room_at; };
        float r_on = std::clamp(right_on, lo, hi);
        if (above(r_on, r_on + spacing) && past_room(r_on)) {
            r_on = room_at;
        }
        // A sword: the right hand no lower down its handle than the game's own left grip.
        if (sword_) {
            r_on = pommel_hi_ ? std::min(r_on, native_m_) : std::max(r_on, -native_m_);
        }
        float l_on = std::clamp(r_on + spacing, lo, hi);
        if (above(l_on, r_on) && past_room(l_on)) {
            l_on = room_at;
        }
        right_slide_ = r_on;
        slide_ = l_on - r_on;
    }
    static float LowPassAlpha(float hz, float dt) {
        const float rate = 2.0f * Body::Pi * hz * dt;
        return rate / (1.0f + rate);
    }
    // The steering turn with the left wrist's share of the roll (right controller's frame).
    Quat Combined() const {
        return Body::Normalize(turn_ * Body::AxisAngle(axis_, RollShare * roll_delta_));
    }
    void ResetSteeringMotion() {
        steer_sample_ok_ = false;
        steer_velocity_ = {};
    }
    float SteeringAlpha(Vec3 direction) {
        if (apply_dt_ <= 1e-6f) {
            ResetSteeringMotion();
            return 0.0f;
        }
        if (steer_sample_ok_) {
            const Vec3 cross = Body::Cross(steer_previous_, direction);
            const float sine = Body::Length(cross);
            const float angle = std::atan2(sine, Body::Dot(steer_previous_, direction));
            const Vec3 velocity = sine > 1e-6f ? cross * (angle / (sine * apply_dt_)) : Vec3{};
            // Filter the signed angular velocity before taking its magnitude; alternating
            // tracking noise should cancel instead of being mistaken for a fast swing.
            steer_velocity_ = Body::Lerp(steer_velocity_, velocity,
                                        LowPassAlpha(SteerDerivativeHz, apply_dt_));
        }
        steer_previous_ = direction;
        steer_sample_ok_ = true;
        const float cutoff = std::min(
            SteerMaxHz, SteerMinHz + SteerSpeedBeta * Body::Length(steer_velocity_));
        return LowPassAlpha(cutoff, apply_dt_);
    }

    std::int32_t family_{-1};
    bool seated_{false};
    bool relative_{true};
    float blend_{0.0f};
    float scale_{1.0f};
    float dt_{0.0f};       // Update's (the blend)
    float right_slide_{0.0f}; // the right hand's place on the weapon (m, from the game's grip)
    bool pommel_hi_{true};     // the rail's hi end is the pommel (else its lo end)
    bool sword_{false};        // no bare shaft toward the blade (PlaceHands' sword wall)
    float native_m_{0.0f};     // the game's own left grip along the rail (m)
    bool motion_ok_{false};    // right_prev_ / left_prev_ hold the last frame's hands
    Vec3 right_prev_{}, left_prev_{};
    float right_motion_{0.0f}, left_motion_{0.0f}; // each hand's speed along the weapon (m/s)
    bool swinging_{false};
    float calm_s_{0.0f};
    float apply_dt_{0.0f}; // ApplyTracked's (steering, sliding)
    float slide_{0.0f};
    RailRange range_{};
    GameGrip grip_{DefaultGrip};
    Vec3 bone_axis_{0, 1, 0}, axis_{0, 0, 1}, lateral_{};
    Quat turn_{};
    bool steer_sample_ok_{false};
    Vec3 steer_previous_{}, steer_velocity_{};
    Quat roll_baseline_{};
    float roll_previous_{}, roll_delta_{};
    bool roll_sample_ok_{false};
};

// The angle (deg) between where the right hand points the handle and the line to the left hand.
inline float TwistDeg(const Handle& h, Vec3 left_bone) {
    if (!h.valid) {
        return 0.0f;
    }
    const Vec3 v = left_bone - h.right;
    if (!(Body::Length(v) > 1e-4f)) {
        return 0.0f;
    }
    return Body::Angle(h.dir, v) / Body::Deg;
}

// two_hand_relative: the angle (deg) between the grab's line (`ref_local`, the right controller's
// frame `right_q`) and the line to the left hand now: how far the left hand has steered.
inline float SteerDeg(const Handle& h, const Quat& right_q, Vec3 ref_local, Vec3 left_bone) {
    if (!h.valid) {
        return 0.0f;
    }
    const Vec3 v = left_bone - h.right;
    if (!(Body::Length(v) > 1e-4f) || !(Body::Length(ref_local) > 0.5f)) {
        return 0.0f;
    }
    return Body::Angle(Body::Rotate(Body::Normalize(right_q), Body::Normalize(ref_local)), v) /
           Body::Deg;
}

// ---- The grab ---------------------------------------------------------------------------------

enum class Why : std::uint8_t {
    None,
    Grip,     // both grips let go
    Apart,    // the hands moved apart (or together) by more than release_m
    Twist,    // the hands' line strayed more than twist_deg off the handle
    Form,     // the weapon is no longer in a two-handed form
    Busy,     // the arc, an item or a vial took the hand
    Context,  // a menu, the screen, a cinematic, the headset's focus
    Tracking, // the right hand lost for over lost_s
    Off,      // the setting or the body went off
};

constexpr const char* WhyName(Why why) {
    switch (why) {
    case Why::None:
        return "none";
    case Why::Grip:
        return "both grips let go";
    case Why::Apart:
        return "the hands apart";
    case Why::Twist:
        return "the hands off the handle's line";
    case Why::Form:
        return "a one-handed form now";
    case Why::Busy:
        return "an item or a vial";
    case Why::Context:
        return "a menu, the screen or a cinematic";
    case Why::Tracking:
        return "a hand lost";
    case Why::Off:
        return "off";
    }
    return "?";
}

// The soft tick on a release you did not make with the grip (the hands apart, twisted).
constexpr bool SoftTick(Why why) {
    return why == Why::Apart || why == Why::Twist;
}

struct Input {
    float dt{0.0f};
    bool enabled{false};    // two_hand_grip, motion controls, the first-person body drives the arms
    bool gameplay{false};   // not a menu, the virtual screen, a cinematic; the headset focused
    bool two_handed{false}; // the right weapon in a two-handed form with one handle (fresh)
    std::int32_t family{-1}; // changing weapons clears an existing attachment
    // A one-handed form whose grab turns it into its two-handed one (core/vr/trick_weapons.h
    // GrabFamily, the left hand freed by the right grip; and while that transform runs): grabbed
    // and held as a two-handed form is.
    bool grab_transforms{false};
    // A grip still held (its squeeze kept from the quick items: a slip off the handle, a squeeze
    // held near it) may take the handle now, without a new squeeze: the hand back on it within
    // the caller's window, or the right grip freeing it a moment after the left one closed.
    bool grab_held{false};
    // A squeeze that cannot grab (yet) within NearExtraM of the zone is kept from the quick items
    // (`held_near`): a near miss of the handle never opens the arc.
    bool hold_near{false};
    bool busy{false};       // an item/action owns the hand, a Blood Vial armed or going in
    bool blocked{false};    // the grip held through a context switch
    float grip{0.0f};       // the left grip, 0..1
    float right_grip{0.0f}; // either grip retains an acquired handle
    bool right_tracked{false};
    bool left_tracked{false};
    Handle handle{};  // from this frame's right controller (HandleAt)
    Vec3 left_bone{}; // the left hand's weapon bone origin (tracking)
    bool right_q_ok{false}; // two_hand_relative: the right controller's orientation (tracking)
    Quat right_q{};
};

inline bool HandleAvailable(const Settings& s, const Input& in, bool engaged) {
    return s.enabled && in.enabled && in.gameplay && !engaged && !in.busy &&
           !in.blocked && in.right_tracked && in.handle.valid &&
           (in.two_handed || in.grab_transforms);
}

inline bool GuideAvailable(const Settings& s, const Input& in, bool engaged) {
    return s.guide && HandleAvailable(s, in, engaged);
}

// On an already two-handed handle, reserve the squeeze before transform dispatch. A short
// tap still transforms on release; a hold or any intervening grab/context change consumes it.
// The visual toggle never changes this gesture. Existing weapon-specific grip uses take priority.
class GuideGesture {
public:
    struct Out { bool owns{false}; bool tap{false}; };
    Out Step(bool down, bool eligible, std::int32_t family, float dt) {
        if (down && !down_) {
            pending_ = eligible;
            cancelled_ = false;
            family_ = family;
            held_s_ = 0.0f;
        }
        Out out{};
        if (pending_) {
            out.owns = true;
            cancelled_ = cancelled_ || !eligible || family != family_;
            held_s_ += std::isfinite(dt) ? std::max(dt, 0.0f) : TapS;
            if (!down) {
                out.tap = !cancelled_ && held_s_ < TapS;
                pending_ = false;
            }
        }
        down_ = down;
        return out;
    }
    void Reset(bool down = false) {
        *this = {};
        down_ = down;
    }
    static constexpr float TapS = 0.25f;
private:
    bool down_{false};
    bool pending_{false};
    bool cancelled_{false};
    std::int32_t family_{-1};
    float held_s_{0.0f};
};

struct Output {
    bool edge{false};          // a squeeze this frame
    bool engaged{false};       // the grip holds the handle
    bool owns_grip{false};     // the quick items see no squeeze (it was the grab's)
    bool grabbed{false};       // event: engaged this frame
    Why released{Why::None};   // event: released this frame
    const char* passed{nullptr}; // a squeeze that was no grab: why (nullptr: none or a grab)
    bool held_near{false};     // ... kept from the quick items (Input::hold_near): a near miss
    Zone zone{};               // the left hand against the zone (every frame)
    float weight{0.0f};        // the poses' blend 0..1
    float apart_m{0.0f};       // the hands' distance minus the grab's
    float twist_deg{0.0f};
    float hands_m{0.0f};
    // two_hand_relative: the hands' line at the newest grab in the right controller's frame
    // (Apply's ref_local; kept after a release for the ease off the handle).
    bool ref_ok{false};
    Vec3 ref_local{};
};

class Tracker {
public:
    void Update(const Settings& s, const Input& in, Output& out) {
        out = {};
        const bool was = grip_on_;
        grip_on_ = grip_on_ ? in.grip > s.release : in.grip >= s.press;
        right_grip_on_ = right_grip_on_ ? in.right_grip > s.release : in.right_grip >= s.press;
        out.edge = grip_on_ && !was;
        const bool tracked = in.left_tracked && in.right_tracked;
        out.zone = ZoneOf(in.handle, in.left_bone, s.zone_m);
        const float hands = in.handle.valid ? Body::Length(in.left_bone - in.handle.right) : 0.0f;
        out.hands_m = hands;
        if (!grip_on_) {
            owns_ = false;
        } else if (engaged_) {
            // A re-squeeze during a right-grip-retained hold also belongs to this grab,
            // including the frame a weapon/context change forces the attachment to drop.
            owns_ = true;
        }
        if (engaged_) {
            Why why = Why::None;
            // The support controller often disappears behind the weapon hand. Keep its
            // attachment and last steering turn while the weapon hand is still tracked.
            lost_s_ = in.right_tracked ? 0.0f : lost_s_ + std::max(in.dt, 0.0f);
            if (!grip_on_ && !right_grip_on_) {
                why = Why::Grip;
            } else if (!in.enabled) {
                why = Why::Off;
            } else if (!in.gameplay) {
                why = Why::Context;
            } else if (in.family != family_ || (!in.two_handed && !in.grab_transforms)) {
                why = Why::Form;
            } else if (in.busy) {
                why = Why::Busy;
            } else if (lost_s_ > s.lost_s) {
                why = Why::Tracking;
            } else if (tracked && in.handle.valid) {
                apart_m_ = hands - d0_;
                twist_deg_ = Twist(in);
                // Diagnostic only: broad swings and crossed wrists do not release a grip.
            }
            if (why != Why::None) {
                engaged_ = false;
                out.released = why;
            }
        } else if (out.edge || (in.grab_held && grip_on_ && owns_)) {
            const char* pass = nullptr;
            if (!in.enabled) {
                pass = "two_hand_grip off";
            } else if (!in.gameplay) {
                pass = "not in gameplay";
            } else if (!in.two_handed && !in.grab_transforms) {
                pass = "a one-handed form";
            } else if (in.busy) {
                pass = "an item or a vial";
            } else if (in.blocked) {
                pass = "held through a context switch";
            } else if (!tracked || !in.handle.valid) {
                pass = "a hand not tracked";
            } else if (!out.zone.inside) {
                pass = "not on the handle";
            }
            if (pass == nullptr) {
                engaged_ = true;
                family_ = in.family;
                owns_ = true;
                d0_ = hands;
                apart_m_ = 0.0f;
                // two_hand_relative: the hands' line now, in the right controller's frame.
                ref_ok_ = false;
                const Vec3 line = in.left_bone - in.handle.right;
                if (s.relative && in.right_q_ok && Body::IsFinite(in.right_q) &&
                    Body::Length(line) > 1e-3f) {
                    ref_ = Body::Rotate(Body::Conjugate(Body::Normalize(in.right_q)),
                                        Body::Normalize(line));
                    ref_ok_ = true;
                }
                twist_deg_ = Twist(in);
                lost_s_ = 0.0f;
                out.grabbed = true;
            } else if (out.edge) {
                out.passed = pass;
                if (in.hold_near && in.enabled && in.gameplay && !in.busy && !in.blocked &&
                    tracked && in.handle.valid &&
                    Body::Length(in.left_bone - in.handle.left) <= s.zone_m + NearExtraM) {
                    // (Round the game's grip point only, not the whole rail: a squeeze beside
                    // the right hand or down the blade stays the quick items'.)
                    owns_ = true; // never the quick items' (until let go)
                    out.held_near = true;
                }
            }
        }
        const float rate = s.blend_s > 1e-3f ? std::max(in.dt, 0.0f) / s.blend_s : 1.0f;
        blend_ = std::clamp(blend_ + (engaged_ ? rate : -rate), 0.0f, 1.0f);
        out.engaged = engaged_;
        out.owns_grip = owns_ || engaged_;
        out.weight = Body::Smoothstep(0.0f, 1.0f, blend_);
        out.apart_m = engaged_ ? apart_m_ : 0.0f;
        out.twist_deg = engaged_ ? twist_deg_ : 0.0f;
        out.ref_ok = ref_ok_;
        out.ref_local = ref_;
    }

    // Everything off at once (no ease).
    void Reset() {
        *this = {};
    }

    // Lets go at once (no ease) without forgetting the grip: a grip still squeezed stays the
    // grab's (no arc) and grabs nothing again until it is let go and squeezed anew. For a gap in
    // the gameplay input (a menu, the virtual screen, a long hitch). True if it was holding.
    // Not holding: nothing changes hands. A grip that is the quick items' (the arc, an item held
    // with the grip in left_scheme=1) stays theirs: taking it here would read to them as the grip
    // opening (a held throwable thrown, an item put away) after every hitch over 1 s.
    bool Drop() {
        blend_ = 0.0f;
        if (!engaged_) {
            return false;
        }
        engaged_ = false;
        owns_ = grip_on_;
        return true;
    }

    [[nodiscard]] bool Engaged() const {
        return engaged_;
    }
    [[nodiscard]] float GrabDistance() const {
        return d0_;
    }

private:
    // The release's twist: from the grab's line (two_hand_relative), else from the handle.
    [[nodiscard]] float Twist(const Input& in) const {
        return ref_ok_ && in.right_q_ok ? SteerDeg(in.handle, in.right_q, ref_, in.left_bone)
                                        : TwistDeg(in.handle, in.left_bone);
    }

    bool grip_on_{false};
    bool right_grip_on_{false};
    std::int32_t family_{-1};
    bool owns_{false};
    bool engaged_{false};
    bool ref_ok_{false};
    Vec3 ref_{};
    float d0_{0.0f};
    float apart_m_{0.0f};
    float twist_deg_{0.0f};
    float lost_s_{0.0f};
    float blend_{0.0f};
};

// ---- The two-handed swing -------------------------------------------------------------------------

// A melee window is two-handed when the grip held the handle at some point while it was open and
// the left hand moved with the swing (its speed, the head's taken out, reached swing_left_mps).
struct SwingMark {
    std::uint32_t serial{0};
    bool held{false};
    float left_peak{0.0f};

    // Every input frame with the newest window's serial (0: none) and whether it is open.
    void Update(std::uint32_t window, bool open, bool engaged, float left_speed) {
        if (window != serial) {
            serial = window;
            held = false;
            left_peak = 0.0f;
        }
        if (window == 0 || !open) {
            return;
        }
        if (engaged) {
            held = true;
            left_peak = std::max(left_peak, std::isfinite(left_speed) ? left_speed : 0.0f);
        }
    }
    [[nodiscard]] bool TwoHanded(const Settings& s) const {
        return serial != 0 && held && left_peak >= s.swing_left_mps;
    }
    // The damage factor the window carries to the game (1: one-handed).
    [[nodiscard]] float Factor(const Settings& s) const {
        return TwoHanded(s) ? std::clamp(s.damage, 1.0f, 2.0f) : 1.0f;
    }
};

} // namespace VR::TwoHand
