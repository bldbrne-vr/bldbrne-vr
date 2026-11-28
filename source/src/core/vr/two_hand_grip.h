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
// handle and, while you hold the grip, the weapon's direction comes from both hands (the front
// hand leads wrist roll; the positional reference stays fixed throughout the hold).
// Leading (2026-10-28): a grab starts with the right hand leading, or the left one when
// it grabbed in front of the right (toward the blade). The weapon never
// slides in the lead fist; the other hand always slides along it. Slide mode lets both hands slide
// and whichever hand is in front takes the lead; leaving it keeps that hand leading.
// Hold B for 160 ms for slide mode; release to end it. The old toggle setting is ignored.
// A short tap keeps B's normal action.
// Y held on a long-handled weapon (2026-10-28): the other hand grips and the lead fist
// loosens: push the weapon through it like a spear, or slide the lead hand along it. Y wins over B.
// two_hand_relative=1 (the default): the weapon keeps the
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
// reviewed transformed meshes supply the rail for known straight handles. Other weapons retain
// the measured native axis; the Wheel retains its separate native support grip.
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
    bool optimized_controls{false}; // opt-in: stick transforms, trigger toggles, grips slide
    bool trigger_hold{true}; // optimized scheme: hold left trigger; false toggles attachment
    bool hold_b_reposition{true}; // runtime always true; false supports automatic-slide test rigs
    // two_hand_zone_cm: the handle zone's radius (9 -> 11 cm, the tester 2026-10-28; 11 -> 11.3 cm,
    // the tester 2026-11-25, to make bringing the hands together and squeezing a little easier: "up
    // the area trigger by 0.5cm", then 11.34, then "actually do 11.3cm"; the grip guide's glow
    // keeps its own size).
    float zone_m{0.113f};
    bool zone_seen{false};       // two_hand_zone_seen: the 9 -> 11 cm migration ran (0.8.3)
    bool zone_1125_seen{false};  // two_hand_zone_1125_seen: the 2026-11-25 11 -> 11.3 cm migration ran
    float release_m{0.25f};      // legacy two_hand_release_cm (no geometric release)
    float twist_deg{75.0f};      // legacy two_hand_twist_deg (no geometric release)
    // two_hand_damage: a two-handed swing's hits x this. 0.90 (2026-11-04: two-handed
    // weapons hit hard and swing quickly in VR, -10%); a +5% bonus (1.05) before.
    float damage{0.90f};
    bool damage_seen{false};     // two_hand_damage_seen: the 1.05 -> 0.90 change ran (2026-11-04)
    float blend_s{0.10f};        // two_hand_blend_ms: release/re-grab ease, not transform animation
    float attach_s{0.06f};       // two_hand_attach_ms: a fresh hand attachment
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
    // How far the weapon turns onto the hands' line, at most: the rest stays where the lead
    // hand alone points it, however far apart the hands are (2026-10-28: the other
    // hand's influence down more, "especially when it's far"; it was the whole line; 0.6, then
    // "a tiny tiny bit more").
    float rear_max_say{0.55f};
};

// Own B from its press while attached: a short release replays the normal button;
// reaching 160 ms toggles slide mode or holds it, never an interaction. Losing eligibility
// cancels the gesture but retains ownership until release (no held-button leak).
class RepositionButton {
public:
    static constexpr float HoldS = 0.16f;
    struct Out { bool consume{}, active{}, tap{}, changed{}; };
    Out Step(bool eligible, bool down, float dt, bool hold_mode = true, bool attached = true) {
        if (!attached || hold_mode != hold_mode_) {
            locked_ = true; // every grab starts out of slide mode
            if (owned_) cancelled_ = true;
        }
        hold_mode_ = hold_mode;
        if (down && !down_) {
            owned_ = eligible;
            cancelled_ = false;
            held_s_ = 0;
            held_ = false;
        }
        Out out{};
        if (owned_) {
            out.consume = true;
            if (!eligible || !std::isfinite(dt) || dt < 0 || dt > 0.1f)
                cancelled_ = true;
            if (down && !cancelled_) {
                held_s_ += dt;
                if (!held_ && held_s_ >= HoldS) {
                    held_ = true;
                    if (!hold_mode) locked_ = !locked_;
                    out.changed = true;
                }
                out.active = held_;
            }
            if (!down) {
                out.tap = !cancelled_ && !held_;
                owned_ = false;
            }
        }
        down_ = down;
        if (!hold_mode) out.active = attached && eligible && !locked_;
        return out;
    }
    void Reset(bool down = false) { *this = {}; down_ = down; }
private:
    bool down_{}, owned_{}, cancelled_{}, held_{};
    bool locked_{true}, hold_mode_{true};
    float held_s_{};
};

// Y's push is a thrust attack too (2026-10-28): while it holds, a clutchless stroke that
// hardly turns the blade commits from this lever speed (m/s) instead of the stab's 3 m/s
// (swing_detector.h StabSpeed): the push through the lead fist rarely peaks that fast. The
// detector's own floor (2 m/s over 15 cm, forward, not a recovery) still applies.
constexpr float PushStabSpeed = 2.0f;
// ... and from this much travel above the floor's speed (the detector's 15 cm: the push only has
// the hands' spacing to travel; a 20 cm push at 2.5 m/s spends ~13 cm above 2 m/s).
constexpr float PushMinTravelM = 0.10f;

// Y's push (SupportGrip::Input::push): active from the press (a thrust cannot wait); a short press
// that moved neither hand on the weapon replays Y's own action. Losing eligibility ends the push
// but keeps the press until release (no held-button leak).
class PushButton {
public:
    static constexpr float TapS = 0.16f;
    static constexpr float TapMoveM = 0.02f;
    struct Out { bool consume{}, active{}, tap{}, started{}; };
    Out Step(bool eligible, bool down, float dt, float moved_m) {
        Out out{};
        if (down && !down_) {
            owned_ = eligible;
            held_s_ = 0;
            moved_ = 0;
            out.started = owned_;
        }
        if (owned_) {
            out.consume = true;
            if (down) {
                held_s_ += std::isfinite(dt) && dt > 0 && dt < 0.1f ? dt : 0.0f;
                if (std::isfinite(moved_m)) moved_ = std::max(moved_, moved_m);
                out.active = eligible;
            } else {
                out.tap = held_s_ < TapS && moved_ < TapMoveM;
                owned_ = false;
            }
        }
        down_ = down;
        return out;
    }
    void Reset(bool down = false) { *this = {}; down_ = down; }
private:
    bool down_{}, owned_{};
    float held_s_{}, moved_{};
};

// The bow owns its string triggers in either hand; never treat it as a rigid handle grab.
inline bool OptimizedHandle(int family) { return family >= 0 && family != 320; }

// A trigger press owns its entire lifetime, even if the weapon/context disappears meanwhile.
// Attachment itself is latched, independently of the physical grip buttons.
class TriggerAttachment {
public:
    struct Out { bool held{}, consume{}; };
    Out Step(bool context, bool can_attach, bool near_handle, bool down, int family,
             bool hold_mode = false) {
        if (hold_mode != hold_mode_) {
            held_ = false;
            down_ = down; // changing modes while held must not invent a grab
            hold_mode_ = hold_mode;
        }
        if (!context || family != family_) held_ = false;
        if (family_ != -1 && family != family_) down_ = down;
        family_ = family;
        if (down && !down_ && context && (held_ || (can_attach && near_handle))) {
            held_ = !held_;
            owned_ = true;
        }
        const bool consume = owned_;
        if (hold_mode && !down) held_ = false;
        if (!down) owned_ = false;
        down_ = down;
        return {held_, consume};
    }
    void Cancel() { held_ = false; }
    void Reset(bool down = false) { *this = {}; down_ = down; }
private:
    bool held_{}, owned_{}, down_{}, hold_mode_{};
    int family_{-1};
};

// ---- The game's two-handed grip -----------------------------------------------------------------

// Remapping a held heavy-attack trigger into a special must not invent a press.
class SpecialTrigger {
public:
    bool Step(bool active, bool down, int family) {
        if (active && (!active_ || family != family_) && down && down_) wait_ = true;
        if (!down) wait_ = false;
        active_ = active;
        family_ = family;
        down_ = down;
        return active && down && !wait_;
    }
    void Reset() { *this = {}; down_ = wait_ = true; }
private:
    bool active_{}, down_{}, wait_{};
    int family_{-1};
};

// The left weapon bone (L_Weapon) in the right one's (R_Weapon) frame: translation in game units,
// rotation.
struct GameGrip {
    bool valid{false};
    Vec3 t{};
    Quat q{};
    // Optional reviewed handle rail, in R_Weapon's model units. Animation supplies wrist
    // rotation, not the slide axis. A zero axis preserves legacy/unknown-weapon behavior.
    Vec3 rail_axis{};
    float rail_lo{}, rail_hi{};
    bool fixed_support{false}; // Wheel: two separate grips, no shaft between them.
};

// Before the game's grip was measured: 14 cm down the handle (R_Weapon +Y: the pommel side; the
// blade runs along -Y), the left fist turned as the right one (the two hands mirror each other
// round the handle: the left bone is the right one's mirror image).
inline constexpr GameGrip DefaultGrip{true, {0.0f, 0.14f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};

constexpr float NearExtraM = 0.06f;   // a near miss: this much outside the zone (Input::hold_near)
constexpr float AxisFadeM = 0.03f;    // the hands this close: the right hand's own direction
constexpr float AxisFullM = 0.06f;    // ... from here the hands' line
// Close hands (2026-10-28: a big weapon wobbled with the hands together): the weapon is
// aimed as if they were at least this far apart along it, so a hand's jitter at a few cm cannot
// swing it (a 1 cm jitter 6 cm apart turned it ~10 degrees; now ~2.6). Further apart: unchanged.
constexpr float MinSteerM = 0.22f;
// ... and the other hand's say in the weapon fades with the hands' distance (the user,
// 2026-10-28: hands very close, only the dominant one): none within SpreadNoneM, full from
// SpreadFullM, for its steering and its wrist's roll (SupportGrip::spread_).
// (10..25 cm, then 10..30 cm "a bit further", then "a tiny tiny bit more": the tester, 2026-10-28)
constexpr float SpreadNoneM = 0.11f;
constexpr float SpreadFullM = 0.33f;
// The crossing zone round the right fist (SupportGrip): the left hand this close to the right one
// is going round it (the controllers cannot pass through each other), not steering: a 9 cm
// detour at that distance would swing the weapon 60-90 degrees. The weapon keeps its angle
// there, whichever side the hand goes round (lean the weapon to make room on that side);
// steering takes over again from where the hand comes out, fully from CrossOuterM.
constexpr float CrossInnerM = 0.10f;
constexpr float CrossOuterM = 0.14f;
constexpr float GameGripMinM = 0.06f; // a plausible game grip: the left fist this far ...
constexpr float GameGripMaxM = 0.75f; // ... to this far from the right one
constexpr float FixedGripMaxM = 1.20f; // ... a separate second handle (the Wheel's) this far
constexpr float GameGripConeDeg = 40.0f; // ... along the handle (R_Weapon +-Y)

// Families whose two-handed form has no single handle for both hands: dual blades (Blade of
// Mercy 40, Rakuyo 270), the bow (Simon's Bowblade 320), claws and fists. (The Bloodletter (290)
// was one of them, its flail held in one hand in VR (the user's choice, 2026-10-13); since
// 2026-11-21 its handle takes the left hand too, if you want: the user, "allow the hunter to place
// the left hand at the hilt and hold grip to hold it with both hands (making it a bit lighter) -
// but releasing left grip ends up single hand wielding the transformed version again".)
constexpr bool SharedHandle(std::int32_t family) {
    switch (family) {
    case 10:  // Bare Fists
    case 40:  // Blade of Mercy
    case 90:  // Beast Claw
    case 270: // Rakuyo
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

// A two-handed form that is as much a one-handed one: the Bloodletter's flail (its left hand is
// a choice, SharedHandle above), never "unsupported" in the optimized scheme.
constexpr bool OneHandAlso(std::int32_t family) { return family == 290; }

// Only weapons with an actual shared two-hand handle need support. Slow supported
// thrusts qualify too: this penalty does not use the swing-speed bonus threshold.
constexpr int UnsupportedFamily(bool optimized, bool attached, int family, int style) {
    return optimized && !attached && TwoHandedForm(family, style) && !OneHandAlso(family) ? family : -1;
}

constexpr float UnsupportedDamage(int unsupported_family, int attack_family,
                                  bool form_b, bool transform_attack) {
    return unsupported_family >= 0 && unsupported_family == attack_family &&
                   form_b && !transform_attack ? 0.5f : 1.0f;
}

// A grip read from the game's animation is the left hand on the handle: 6-75 cm from the right
// fist (game units / `scale`), within 40 degrees of the handle's line (R_Weapon +-Y).
inline bool Plausible(const GameGrip& g, float scale, bool axial = true) {
    if (!g.valid || !Body::IsFinite(g.t) || !Body::IsFinite(g.q) || !(scale > 1e-3f)) {
        return false;
    }
    const float length = Body::Length(g.t) / scale;
    // The Wheel's second handle sits across the wheel (WheelOtherHandle), past a long hilt's reach.
    if (length < GameGripMinM || length > (g.fixed_support ? FixedGripMaxM : GameGripMaxM)) {
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

// Conservative fist-centre limits reviewed against the original transformed weapon meshes.
// The guard/head/pommel themselves are excluded. No game meshes are shipped. These values
// are model metres; HandleAt converts them with the same world scale as the native grip.
struct HandleProfile {
    std::int32_t family;
    const char* name;
    int model, part;
    Vec3 axis;
    float lo, hi;
};
inline constexpr HandleProfile HandleProfiles[] = {
    {20, "Chikage", 1063, 2, {0, 1, 0}, -0.04f, 0.14f},
    {50, "Hunter Axe", 1020, 2, {-0.03f, 1, 0}, -0.65f, 0.24f},
    {51, "Burial Blade", 1021, 2, {0, 1, 0}, -0.52f, 0.25f},
    {80, "Kirkhammer", 1031, 2, {0, 1, 0}, -0.04f, 0.22f},
    {81, "Ludwig's Holy Blade", 1030, 2, {0, 1, 0}, -0.04f, 0.22f},
    // Mirrored 2026-10-28: the game's own two-handed left grip sits 40 cm toward the head
    // (R_Weapon -Y, as the Axe's and the Pick's); -0.13..0.53 put it 27 cm off the handle.
    {100, "Rifle Spear", 1040, 2, {0, 1, 0}, -0.53f, 0.13f},
    {260, "Holy Moonlight Sword", 1708, 2, {0, 1, 0}, -0.02f, 0.21f},
    {300, "Church Pick", 1704, 2, {0, 1, 0}, -0.60f, 0.24f},
    {310, "Whirligig Saw", 1707, 2, {0.02f, 1, 0}, -0.38f, 0.23f},
};
inline const HandleProfile* ProfileOf(std::int32_t family) {
    for (const auto& p : HandleProfiles) {
        if (p.family == family) return &p;
    }
    return nullptr;
}
// Logarius' Wheel's second handle in R_Weapon's frame (game metres). wp_a_1052 has two upright
// handles on opposite sides of the wheel (model x -41 and +41 cm); the right fist holds the first,
// so the second sits across the wheel's diameter: 41.5 + 41 cm along R_Weapon +X (the rim dummies
// 10141 / 10143 at (83.7, 0, -21.8) / (-0.6, 0, -21.8) cm, the same in form B). The game's
// measured left grip, (22.8, 43.2, -6.8) cm, is off the wheel: it put the hunter's left hand in
// the air beside his head (2026-11-06).
inline constexpr Vec3 WheelOtherHandle{0.83f, 0.0f, 0.0f};

inline GameGrip OnHandle(std::int32_t family, GameGrip native,
                         const Body::C0000::DummyPoly* attachment = nullptr) {
    if (!native.valid) return native;
    if (family == 120) {
        native.fixed_support = true;
        native.t = WheelOtherHandle;
        native.q = {0.0f, 0.0f, 0.0f, 1.0f}; // the left fist turned as the right one (DefaultGrip)
    } else if (const auto* p = ProfileOf(family)) {
        native.rail_axis = Body::Normalize(p->axis);
        native.rail_lo = p->lo;
        native.rail_hi = p->hi;
        // These three weapons share the native greatsword grasp and transformed
        // attachment 322. Its shaft is tilted relative to R_Weapon; projecting the
        // left bone onto R_Weapon +Y loses the palm contact (about 4 cm on retail data).
        // Other reviewed contacts, including the user's preferred Axe placement, stay
        // on their existing rail. Do not apply one lateral correction to every weapon.
        const bool native_contact = family == 80 || family == 81 || family == 260;
        if (native_contact && attachment &&
            attachment->id == Body::C0000::DmyIdRightTwoHandBig &&
            attachment->bone == Body::C0000::R_Weapon) {
            const auto vec = [](const auto& a) { return Vec3{a[0], a[1], a[2]}; };
            const Vec3 z = vec(attachment->forward), y = vec(attachment->up);
            const Vec3 x = Body::Cross(y, z), origin = vec(attachment->position);
            if (Body::IsFinite(x) && Body::IsFinite(y) && Body::IsFinite(z) &&
                Body::IsFinite(origin) && Body::LengthSq(x) > 0.5f &&
                Body::LengthSq(z) > 0.5f) {
                const Vec3 zz = Body::Normalize(z), xx = Body::Normalize(x);
                native.rail_axis = Body::Rotate(Body::FromBasis(xx, Body::Cross(zz, xx), zz),
                                               native.rail_axis);
                const float along_origin = Body::Dot(origin, native.rail_axis);
                native.rail_lo += along_origin;
                native.rail_hi += along_origin;
            }
        }
        const float native_along = Body::Dot(native.t, native.rail_axis);
        const Vec3 contact = native_contact ? native.t - native.rail_axis * native_along : Vec3{};
        float along = std::clamp(native_along, native.rail_lo, native.rail_hi);
        // A native off-axis/guard-side pose cannot put the support fist on the right fist.
        if (std::abs(along) < AxisFullM) along = std::min(0.14f, p->hi);
        native.t = contact + native.rail_axis * along;
    }
    return native;
}

// ---- Frames ---------------------------------------------------------------------------------------

// Small visual grip correction for the extended Axe's slanted shaft. Change the
// hand-to-weapon attachment before IK, so the weapon target (and collisions) stays
// fixed while the right palm seats around the shaft. Always start from native data.
inline Body::QsTransform RightHandWeaponLocal(int family, int style,
                                            Body::QsTransform native, float weight) {
    if (family != 50 || style != 3 || !std::isfinite(weight)) return native;
    const float w = std::clamp(weight, 0.0f, 1.0f);
    const Quat inverse_turn = Body::AxisAngle({0, 0, 1}, 5.5f * Body::Deg * w);
    const Vec3 hand_shift = Vec3{0.001f, 0, -0.0065f} * w;
    const Vec3 inverse_shift = Body::Rotate(inverse_turn, hand_shift * -1.0f);
    return Body::Compose(native, {inverse_shift, inverse_turn, {1, 1, 1}});
}

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
    bool reviewed{false}, fixed_support{false};
    float rail_lo{}, rail_hi{};
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
    h.reviewed = Body::LengthSq(use.rail_axis) > 0.5f;
    h.fixed_support = use.fixed_support;
    if (h.reviewed) {
        const Vec3 axis = BoneToGripLocal(r, use.rail_axis, 1.0f) - r.offset;
        h.dir = Body::Normalize(Body::Rotate(q, axis));
        h.rail_lo = use.rail_lo / std::max(scale, 1e-3f);
        h.rail_hi = use.rail_hi / std::max(scale, 1e-3f);
    }
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
    if (h.fixed_support) return {h.length, h.length};
    if (h.reviewed) return {h.rail_lo, h.rail_hi};
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

// Carry the attached hand through the one weapon's wall/clash correction. This never
// chooses a leader: physical grip order is resolved before the collision solver runs.
inline Pose FollowWeaponCorrection(const Pose& weapon_before, const Pose& weapon_after,
                                   const Pose& attached, float weight) {
    const Quat turn = Body::Normalize(weapon_after.q * Body::Conjugate(weapon_before.q));
    const Vec3 held = weapon_after.p + Body::Rotate(turn, attached.p - weapon_before.p);
    return {Body::Lerp(attached.p, held, weight),
            Body::Normalize(Body::ScaleRotation(turn, weight) * attached.q)};
}

// Both hands are on the weapon and either can slide along it: the hand that moves along the weapon
// slides, the one that stays still holds it. A change of the hands' spacing goes to each hand in
// proportion to how much it moved along the weapon in the room (smoothed: no switching, nothing to
// flicker; both moving together, a swing or a step, slides nothing). The left hand's slide is
// `Slide` (its distance from the right one); the right hand's place on the weapon is `RightSlide`
// (from the game's own grip point): the weapon moves in the hunter's right fist (WeaponShift),
// staying where it is in the room. Walls, never springs: both hands stay on the rail, the upper
// hand leaves UpperRoomM for the lower one at the pommel end, and on a sword the right hand goes
// no lower than the game's own left grip. The spacing always follows the hands, through swings
// too (2026-10-28: the reposition after a swing felt glitchy; "let hands slide as we
// attack"): its change is the non-lead hand's, the weapon never creeps in the lead fist. Only in
// slide mode (B), and not mid-swing, does it go to whichever hand moves. Direction is
// an unoriented line: crossing the right fist changes the signed slide coordinate, not the
// blade's direction. Grip buttons still own release.
class SupportGrip {
public:
    struct Input {
        bool held{false};
        bool ready{false}; // transformed form settled and native rail known
        bool transforming{false}; // not ready because the weapon is transforming (the game's
                                  // animation carries the hand), not for a missing calibration
        bool tracked{false};
        bool reposition{false}; // unlocked while B is held in Hold B mode
        bool grip_locked{false}; // toggle mode's persistent lock
        bool push{false}; // Y held (PushButton): the other hand grips, the lead slides (LongHandle)
        bool explicit_slide{false}; // optimized controls: each grip loosens only its own hand
        bool slide_left{false}, slide_right{false};
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
    // A grab that transformed the weapon: the hand has this long, once the form is ready, to reach
    // the handle; else it was let go (the weapon turns back to one hand).
    static constexpr float TransformGraceS = 0.5f;
    // ... and seats from this far off the handle (the squeeze was on the handle; the transform
    // moved it, not the hand): the hunter's hand is glued onto it (2026-10-28: "I'd
    // prefer if the left hand anchors on it"). Ordinary grabs keep the zone (Settings::zone_m).
    static constexpr float TransformGlueM = 0.30f;
    // Carried through the transform (2026-10-28: following the animation made the hand
    // jump up and back down): the hand stays where it squeezed, rigid with the right hand
    // (CarriedLeft), seats there, then slides on to the controller this softly for CarryEaseS.
    static constexpr float CarryEaseS = 0.45f;   // (2026-10-28: 0.6 s a touch slow)
    static constexpr float CarryEaseTauS = 0.13f;
    // Room the upper hand leaves for the lower one at the rail's pommel end.
    static constexpr float UpperRoomM = 0.10f;
    // A handle this long (the rail) takes Y's push: polearms, the scythe, the pick, the saw.
    static constexpr float LongHandleM = 0.45f;
    // Out of slide mode the sliding hand stops this short of the holding one (PlaceKeeping): the
    // grab's own least spacing from the right fist (AxisFullM), so a hold never starts closer.
    static constexpr float FistGapM = AxisFullM;
    // A short hilt's lead handed over: the weapon stays where it is, and what is left of the
    // old hold (the new lead fist's offset from its controller, a leading right controller's
    // frame) settles mostly while the lead hand moves, where it is not seen (the user,
    // 2026-10-28: the weapon visibly moved from one hand to the other; it eased at 1 m/s and
    // 90 deg/s, hands still or not). Rates: still + per the lead hand's speed (m/s, rad/s).
    static constexpr float HandoverStillMps = 0.02f;
    static constexpr float HandoverShare = 0.3f;
    static constexpr float HandoverStillRadS = 10.0f * Body::Deg;
    static constexpr float HandoverTurnShare = 0.3f;
    static constexpr float HandoverTurnPerM = 0.5f; // rad per metre the hand travels
    // A handle shorter than this (Kirkhammer, Ludwig's, Moonlight, Chikage): the lead hand alone
    // steers and rolls the weapon, the other one holds on and slides (2026-10-28: the
    // hands' short line spun these in attacks).
    static constexpr float ShortHiltM = 0.35f;
    // The weapon swinging: only the non-lead hand slides and the lead never changes (it is the
    // swing, not a reposition). On from SwingOnMps, off again once slower than SwingOffMps for
    // SwingCalmS.
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
    // Rear wrist share (the lead wrist takes 90%; 70%, 80%, then 88%, until the tester, 2026-10-28:
    // the other hand's say down "a bit further", "more", "a tiny tiny bit more") and the accumulated correction
    // bound. A re-grip or a wrist turned round cannot wind the blade up without limit.
    static constexpr float RollShare = 0.10f;
    // The most the rear fist slips off its controller by the weapon not following the hands'
    // line (Steered: rear_max_say_ holds below it).
    static constexpr float RearSlipM = 0.05f;
    static constexpr float LeadGapM = 0.06f;
    static constexpr float LeadDwellS = 0.08f;
    static constexpr float LeadBlendS = 0.14f;
    static constexpr float RollMaxRad = 25.0f * Body::Deg;

    bool Update(const Settings& s, const Input& in, float dt) {
        if (family_ != in.family) {
            Reset();
            family_ = in.family;
        }
        tracking_ok_ = in.tracked;
        explicit_slide_ = in.explicit_slide;
        slide_left_ = in.slide_left;
        slide_right_ = in.slide_right;
        rear_max_say_ = std::clamp(s.rear_max_say, 0.0f, 1.0f);
        push_ = in.push && LongHandle(); // Y wins over B
        const bool allow_slide =
            explicit_slide_ ? (slide_left_ || slide_right_) :
            !push_ && !in.grip_locked && (!s.hold_b_reposition || in.reposition);
        if (allow_slide != allow_slide_) {
            // Motion from a swing or a previous hold must not choose the sliding hand.
            motion_ok_ = false;
            right_motion_ = left_motion_ = 0;
            lead_candidate_s_ = 0;
        }
        allow_slide_ = allow_slide;
        if (!in.tracked || !in.ready || !in.held) {
            lead_candidate_s_ = 0.0f;
            anchor_recovery_ = 0.0f;
            motion_ok_ = false;
            right_motion_ = left_motion_ = 0.0f;
        }
        dt_ = std::isfinite(dt) ? std::clamp(dt, 0.0f, 0.05f) : 0.0f;
        if (!in.held || !in.ready || !in.tracked || !std::isfinite(dt) || dt <= 1e-6f ||
            dt > 0.05f) {
            ResetSteeringMotion();
            roll_sample_ok_ = roll_right_ok_ = false;
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
            acquisition_failed_ = false;
            waited_ = carried_ = carry_ok_ = false;
            pending_s_ = 0;
            pending_sample_ = false;
            seated_ = false;
            blend_ = std::max(0.0f, blend_ - rate);
            return false;
        }
        if (!in.ready) {
            pending_s_ = 0; // transformation time never spends the attachment grace
            pending_sample_ = false;
            // A grab that had to wait (the squeeze transformed the weapon, or its grip was not
            // measured yet) moved the handle under the hand: no near-miss rules, the hand gets
            // TransformGraceS to reach it once the form is ready, else it is a let-go and the
            // weapon turns back (2026-10-28: it stayed in its two-handed form).
            waited_ = true;
            carried_ = carried_ || in.transforming;
            if (in.transforming && !carry_ok_ && in.tracked && Body::IsFinite(in.left.p) &&
                Body::IsFinite(in.right.p)) {
                // Where the hand squeezed, in the right hand's frame (it is still the controller).
                const Quat back = Body::Conjugate(Body::Normalize(in.right.q));
                carry_local_ = {Body::Rotate(back, in.left.p - in.right.p),
                                Body::Normalize(back * Body::Normalize(in.left.q))};
                carry_ok_ = true;
            }
            seated_ = false;
            blend_ = 0.0f;
            return false;
        }
        bool grabbed = false;
        if (acquisition_failed_) return false;
        if (!seated_ && waited_) {
            pending_s_ += dt_;
            if (pending_s_ > TransformGraceS) {
                acquisition_failed_ = true;
                blend_ = 0.0f;
                return false;
            }
        } else if (!seated_) {
            pending_s_ += dt_;
            if (pending_s_ > 0.25f) {
                acquisition_failed_ = true;
                blend_ = 0.0f;
                return false;
            }
            // A missing sample cannot be differentiated against the next frame's
            // dt: that would turn a slow reach during occlusion into a fast retreat.
            if (!in.tracked || !Plausible(in.target, in.scale, false)) pending_sample_ = false;
        }
        if (!seated_ && in.tracked && Plausible(in.target, in.scale, false)) {
            Handle rail = HandleAt(in.right, in.right_bone, in.target, in.scale);
            rail.blade_clear = in.blade_clear;
            const Vec3 hand = BoneOrigin(in.left, in.left_bone);
            const Zone zone =
                ZoneOf(rail, hand, waited_ ? std::max(s.zone_m, TransformGlueM) : s.zone_m);
            if (!zone.inside) {
                // Repair small misses while approaching the handle. Only outward
                // separation counts as pulling away; fast inward reaches are welcome.
                const bool valid_dt = std::isfinite(dt) && dt > 1e-6f && dt <= 0.05f;
                const float outward = pending_sample_ && valid_dt
                    ? (zone.off - pending_off_) / dt : 0.0f;
                pending_off_ = zone.off;
                pending_sample_ = valid_dt;
                acquisition_failed_ = waited_ ? pending_s_ >= TransformGraceS
                    : zone.off > s.zone_m + 0.08f ||
                          (zone.off > s.zone_m + 0.02f && outward > 1.0f) || pending_s_ >= 0.25f;
                if (acquisition_failed_) blend_ = 0.0f;
                return false;
            }
            if (zone.inside) {
                pending_s_ = 0;
                pending_sample_ = false;
                // A re-grab while the last hold still eases out keeps the weapon where it is
                // shown: steering then takes it on from there (no snap back to the controller).
                const bool carry_hold = blend_ > 0.0f;
                fresh_attachment_ = !carry_hold;
                const Quat carried = carry_hold ? Combined() : Quat{};
                // ... and the weapon where it is in the right fist (else the game's grip point).
                right_slide_ = blend_ > 0.0f ? right_slide_ : 0.0f;
                seated_ = grabbed = true;
                lead_turn_ok_ = roll_right_ok_ = pressed_ = false;
                if (!carry_hold) lead_frame_ = {};
                grip_ = in.target;
                scale_ = in.scale;
                range_ = RailOf(rail);
                // The weapon's own length (model metres), not the player's scale of it.
                short_hilt_ = rail.reviewed && !rail.fixed_support &&
                              (range_.hi - range_.lo) * scale_ < ShortHiltM;
                bone_axis_ = rail.reviewed ? in.target.rail_axis : Body::Normalize(in.target.t);
                contact_offset_ = in.target.t - bone_axis_ * Body::Dot(in.target.t, bone_axis_);
                axis_ = Body::Normalize(BoneToGripLocal(in.right_bone, bone_axis_, 1.0f) -
                                        in.right_bone.offset);
                const Quat back = Body::Conjugate(Body::Normalize(in.right.q));
                const Vec3 local = Body::Rotate(back, hand - rail.right);
                if (rail.fixed_support) right_slide_ = 0.0f;
                // Carried through a transform: the hand is where the game's animation put it,
                // on the game's own grip; it slides on to the controller from there.
                const bool carry_seat = carried_ && carry_ok_ && !carry_hold;
                const float seat_along = carry_seat
                    ? ZoneOf(rail, BoneOrigin(CarriedLeft(in.right), in.left_bone), s.zone_m).along
                    : zone.along;
                carry_ease_s_ = carry_seat ? CarryEaseS : 0.0f;
                slide_ = std::clamp(seat_along, range_.lo - right_slide_, range_.hi - right_slide_);
                grip_.t = contact_offset_ + bone_axis_ * (slide_ * scale_);
                // The upper end of the rail (toward the blade, R_Weapon -Y) and the walls.
                const Vec3 blade = BoneToGripLocal(in.right_bone, {0, -1, 0}, 1.0f) -
                                   in.right_bone.offset;
                pommel_hi_ = Body::Dot(axis_, blade) <= 0.0f; // the rail's hi end is the pommel
                // A short hilt's lead changes too, with B (2026-10-28), but only by
                // B: out of slide mode no lead ever changes.
                front_leads_ = rail.reviewed && !rail.fixed_support;
                // The right hand leads a grab, unless the left one grabbed in front of it (toward
                // the blade by more than LeadGapM): then the left leads this grab (the user,
                // 2026-10-28).
                left_leads_ = front_leads_ &&
                              (pommel_hi_ ? slide_ < -LeadGapM : slide_ > LeadGapM);
                if (!carry_hold) {
                    left_lead_mix_ = left_leads_ ? 1.0f : 0.0f;
                    anchor_left_ = left_leads_;
                    anchor_local_ = handover_bias_ = {};
                    anchor_recovering_ = handover_pending_ = false;
                }
                lead_candidate_s_ = 0.0f;
                anchor_recovery_ = carry_hold ? 0.0f : 1.0f;
                native_m_ = rail.length;
                sword_ = rail.reviewed ? range_.lo >= -AxisFadeM : in.blade_clear < AxisFadeM; // no bare shaft toward the blade
                motion_ok_ = false;
                right_motion_ = left_motion_ = 0.0f;
                // Preserve the grab's lateral offset when steering: sliding straight along
                // an off-centre grab must not slowly rotate the weapon as separation changes.
                relative_ = s.relative;
                lateral_ = relative_ ? local - axis_ * zone.along : Vec3{};
                turn_ = steer_base_ = carried;
                roll_baseline_ = Body::Normalize(back * Body::Normalize(in.left.q));
                roll_delta_ = roll_previous_ = 0.0f;
                roll_sample_ok_ = true;
                ResetSteeringMotion();
                grip_.q = Body::Normalize(in.target.q);
            }
        }
        const float attach_time = fresh_attachment_ ? std::min(s.attach_s, s.blend_s) : s.blend_s;
        const float attach_rate = attach_time > 1e-3f ? dt_ / attach_time : 1.0f;
        blend_ = seated_ ? std::min(1.0f, blend_ + attach_rate) : 0.0f;
        return grabbed;
    }
    // `dt`: since the last ApplyTracked (the published poses' own clock, not the input's).
    Applied ApplyTracked(const Pose& right, const Pose& left, const BoneOnGrip& r,
                         const BoneOnGrip& l, float dt) {
        const bool dt_ok = std::isfinite(dt) && dt > 1e-6f && dt <= 0.05f;
        apply_dt_ = dt_ok ? dt : 0.0f;
        if (!dt_ok) {
            ResetSteeringMotion();
            lead_candidate_s_ = 0.0f;
        }
        // The weapon turns with its lead hand (2026-10-28: a leading left hand had the
        // right wrist still turning it). It is drawn in a frame that turns, from frame to frame,
        // as the lead blend of both controllers (lead_frame_ * the right controller; the right
        // leading: the right controller's own, unchanged). Steering and both wrists' roll are
        // measured in it, so the rear wrist (either one) has the rear share.
        const Quat qr = Body::Normalize(right.q);
        const Quat ql = Body::Normalize(left.q);
        // The lead hand's own speed (the handover's settling).
        float lead_mps = 0.0f, lead_rads = 0.0f;
        if (lead_turn_ok_ && apply_dt_ > 0.0f) {
            const bool lh = left_lead_mix_ >= 0.5f;
            lead_mps = Body::Length((lh ? left.p : right.p) - (lh ? lead_prev_lp_ : lead_prev_rp_)) /
                       apply_dt_;
            lead_rads = Body::AngleOf(lh ? ql * Body::Conjugate(lead_prev_l_)
                                         : qr * Body::Conjugate(lead_prev_r_)) / apply_dt_;
            if (!std::isfinite(lead_mps) || !std::isfinite(lead_rads)) lead_mps = lead_rads = 0.0f;
            lead_mps = std::min(lead_mps, 10.0f);
            lead_rads = std::min(lead_rads, 30.0f);
        }
        lead_prev_rp_ = right.p;
        lead_prev_lp_ = left.p;
        if (seated_ && tracking_ok_ && lead_turn_ok_ && Body::IsFinite(qr) &&
            Body::IsFinite(ql)) {
            const Quat dr = Body::Normalize(qr * Body::Conjugate(lead_prev_r_));
            const Quat dl = Body::Normalize(ql * Body::Conjugate(lead_prev_l_));
            // The pivot follows the lead too (anchor_left_; a handover keeps the weapon where it
            // is, handover_bias_), so the weapon turns about the hand it turns with, however the
            // lead was taken (2026-10-28: a left lead handed over mid-hold kept the
            // right controller's frame, and the rear right wrist turned the weapon fully).
            const float lead_mix = left_lead_mix_;
            lead_frame_ = Body::Normalize(Body::Slerp(dr, dl, lead_mix) * lead_frame_ *
                                          Body::Conjugate(dr));
            // Led by the right again: the weapon settles back into that controller's frame
            // while the hand moves (no snap). A short hilt also settles slowly held still
            // (HandoverStillRadS); a long haft never turns without a hand moving it.
            const float off = Body::AngleOf(lead_frame_);
            const float rate = (short_hilt_ ? HandoverStillRadS : 0.0f) +
                               HandoverTurnShare * lead_rads + HandoverTurnPerM * lead_mps;
            const float back = (1.0f - lead_mix) * rate * apply_dt_;
            if (off > 1e-5f && back > 0.0f) {
                lead_frame_ = Body::Slerp(lead_frame_, Quat{}, std::min(1.0f, back / off));
            }
        }
        lead_prev_r_ = qr;
        lead_prev_l_ = ql;
        lead_turn_ok_ = seated_ && tracking_ok_ && Body::IsFinite(qr) && Body::IsFinite(ql);
        // The lead frame is part of the attachment too. Fade it out on release;
        // otherwise the last blended pose retains a left-led turn and the caller
        // snaps back to the raw right controller when the blend reaches zero.
        const Quat q = Body::Normalize(Body::ScaleRotation(lead_frame_, Weight()) * qr);
        const Pose rp{right.p, q};
        const Vec3 local = Body::Rotate(Body::Conjugate(q),
                                        BoneOrigin(left, l) - BoneOrigin(rp, r));
        if (seated_ && tracking_ok_ && Body::IsFinite(local)) {
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
            const Vec3 offset = local - Body::Rotate(turn_, lateral_);
            const float along = Body::Dot(offset, previous_axis);
            // By the hands' own distance, not their spacing along the weapon: a weapon turned
            // across the hands' line has no spacing along it, and must still be steered back.
            const float apart = Body::Length(offset);
            steer_apart_ = apart;
            spread_ = pressed_ ? 0.0f : Body::Smoothstep(SpreadNoneM, SpreadFullM, apart);
            // Aimed as if at least MinSteerM apart (on the hands' own side along the weapon).
            const Vec3 aim = offset + previous_axis *
                std::copysign(std::max(0.0f, MinSteerM - apart), along);
            // Suppress the singularity when the hands meet or cross perpendicular to the
            // shaft. Keep the previous turn there; never choose a new 180-degree direction.
            const float steer = Body::Smoothstep(AxisFadeM, AxisFullM, std::abs(along));
            if (steer > 0.0f && Body::Length(aim) > 1e-5f && !short_hilt_) {
                const Vec3 directed = Body::Normalize(aim) * (along < 0.0f ? -1.0f : 1.0f);
                // Transport the existing orientation with the shaft. Rebuilding from the
                // original grab axis loses roll continuity near its antipode (a tiny hand
                // movement can then produce a nearly 180-degree roll). `directed` is in
                // the previous axis's hemisphere, so this incremental arc is nonsingular.
                Quat desired = Body::Normalize(
                    Body::FromTo(previous_axis, directed) * turn_);
                // Ambiguous hand spacing must not boost the adaptive cutoff. Measure
                // speed only after choosing the same direction on the unoriented line.
                float alpha;
                if (steer < 1.0f) {
                    ResetSteeringMotion();
                    alpha = LowPassAlpha(SteerMinHz, apply_dt_);
                } else {
                    alpha = SteeringAlpha(directed);
                }
                turn_ = Body::Slerp(turn_, desired, alpha * steer * spread_);
                // Parallel transport alone accumulates roll around closed paths.
                // Anchor roll to the grab frame where that reference is well-defined;
                // near its antipode retain transport, then recover at a bounded rate.
                // On the turn itself, not its target: a part-way step (the rear hand's fading
                // say, a high frame rate's small filter step) took only that part of the
                // correction, and roll built up round loops (145 deg in 8 at 120 Hz, 25 cm).
                const Vec3 shaft = Body::Rotate(turn_, axis_);
                const float reference_weight = Body::Smoothstep(
                    -0.98f, -0.8f, Body::Dot(axis_, shaft));
                if (reference_weight > 0.0f) {
                    const Quat reference = Body::FromTo(axis_, shaft);
                    const float twist = Body::TwistAngle(
                        Body::Normalize(Body::Conjugate(turn_) * reference), axis_);
                    const float limit = 120.0f * Body::Deg * apply_dt_ * reference_weight;
                    turn_ = Body::Normalize(turn_ * Body::AxisAngle(
                        axis_, std::clamp(twist, -limit, limit)));
                }
            } else {
                ResetSteeringMotion();
            }
            const Vec3 displayed_axis =
                Body::Rotate(Body::ScaleRotation(Steered(), Weight()), axis_);
            // Each hand's motion along the weapon in the room (its share of a change of spacing).
            const Vec3 right_at = BoneOrigin(rp, r), left_at = BoneOrigin(left, l);
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
            if (!grip_.fixed_support && (!explicit_slide_ || slide_left_ || slide_right_)) {
                // The spacing follows the hands (the slide filter: no creep from jitter) ...
                const float wanted = Body::Dot(local, displayed_axis);
                const float before = slide_;
                const float delta = wanted - slide_;
                float spaced = slide_;
                if (std::abs(delta) > SlideDeadbandM) {
                    const float quiet_delta = delta - std::copysign(SlideDeadbandM, delta);
                    const float tau = carry_ease_s_ > 0.0f ? CarryEaseTauS : SlideTauS;
                    spaced += quiet_delta * (1.0f - std::exp(-apply_dt_ / tau));
                }
                carry_ease_s_ = std::max(0.0f, carry_ease_s_ - apply_dt_);
                // ... and its change is the non-lead hand's (the right hand's share moves its
                // place on the weapon, the weapon staying in the left fist; the left's moves the
                // left's); in slide mode, out of a swing, the moving hand's.
                // Y's push: the lead hand slides, the other holds the weapon, swings included
                // (the thrust is the swing).
                const float share = explicit_slide_
                    ? (slide_right_ ? (slide_left_
                        ? right_motion_ / (right_motion_ + left_motion_ + 1e-3f) : 1.0f) : 0.0f)
                    : push_ ? 1.0f - left_lead_mix_
                    : allow_slide_ && !swinging_
                    ? right_motion_ / (right_motion_ + left_motion_ + 1e-3f)
                    : left_lead_mix_;
                // One hand holding the weapon (out of slide mode, a swing, Y's push): it keeps
                // its place, the other one stops at the rail's ends and at the holding hand
                // (PlaceKeeping). Explicit one-hand sliding also preserves the other fist.
                // Both moving (slide mode; a handover's blend): PlaceHands.
                const bool holding = explicit_slide_ ? (slide_left_ != slide_right_)
                                                    : push_ || !allow_slide_ || swinging_;
                if (holding && share >= 1.0f - 1e-4f) {
                    PlaceKeeping(true, right_slide_ - (spaced - before), explicit_slide_);
                } else if (holding && share <= 1e-4f) {
                    PlaceKeeping(false, right_slide_ + spaced, explicit_slide_);
                } else {
                    PlaceHands(right_slide_ - share * (spaced - before), spaced);
                }
            }
            grip_.t = contact_offset_ + bone_axis_ * (slide_ * scale_);
            // Decide from grip positions before collision correction. In the overlap band
            // keep the current leader; require a sustained crossing, never a one-frame flip.
            if (front_leads_ && apply_dt_ > 0.0f) {
                const float front = pommel_hi_ ? -slide_ : slide_;
                // Either scheme: sliding (B, or optimized controls' grips) hands the lead to the
                // front hand (2026-10-28: consistent; the optimized lead was fixed).
                const bool other = allow_slide_ && !swinging_ &&
                    (left_leads_ ? front < -LeadGapM : front > LeadGapM);
                lead_candidate_s_ = other ? lead_candidate_s_ + apply_dt_ : 0.0f;
                if (lead_candidate_s_ >= LeadDwellS) {
                    left_leads_ = !left_leads_;
                    lead_candidate_s_ = 0.0f;
                    // The weapon's pivot goes to the new lead fist. It stays where it is
                    // (handover_bias_), settling onto that fist as the hand moves.
                    anchor_left_ = left_leads_;
                    handover_pending_ = true;
                }
                const float target = left_leads_ ? 1.0f : 0.0f;
                const float step = apply_dt_ / LeadBlendS;
                left_lead_mix_ += std::clamp(target - left_lead_mix_, -step, step);
            }
        } else {
            ResetSteeringMotion();
        }
        if (seated_ && tracking_ok_ && !short_hilt_ && Body::IsFinite(right.q) &&
            Body::IsFinite(left.q)) {
            // Compare wrists in the right controller frame, relative to their grab pose.
            // Weight new wrist movement by the leading hand. Do not reweight accumulated
            // twist during handover: that would turn the weapon without either wrist moving.
            // Sample controller-relative wrist twist, not the transported steering
            // frame. Recovering that frame's roll must not be counted as wrist input.
            const Quat relative = Body::Normalize(Body::Conjugate(q) *
                Body::Normalize(left.q) * Body::Conjugate(roll_baseline_));
            // Each wrist has its own swing/twist singularity. A stable lead wrist must
            // never validate an ambiguous rear wrist (especially with the left leading).
            const auto wrist_step = [&](const Quat& rel, float& previous, bool& valid) {
                const float along = Body::Dot(Vec3{rel.x, rel.y, rel.z}, axis_);
                const float confidence = Body::Smoothstep(
                    0.0025f, 0.04f, along * along + rel.w * rel.w);
                if (!Body::IsFinite(rel) || confidence <= 0.0f) {
                    valid = false;
                    return 0.0f;
                }
                const float raw = Body::TwistAngle(rel, axis_);
                const float step = valid ? Body::WrapPi(raw - previous) * confidence : 0.0f;
                previous = raw;
                valid = true;
                return step;
            };
            const Quat right_rel = Body::Normalize(Body::Conjugate(q) * qr);
            const float left_step = wrist_step(relative, roll_previous_, roll_sample_ok_);
            const float right_step = wrist_step(right_rel, roll_previous_r_, roll_right_ok_);
            const float rear = RollShare * spread_;
            roll_delta_ = std::clamp(roll_delta_ + rear * (left_step + right_step),
                                     -RollMaxRad, RollMaxRad);
        }
        const Quat combined = Combined();
        if (seated_ && tracking_ok_ && Body::IsFinite(local) && apply_dt_ > 0.0f) {
            const Applied full = Place(rp, left, r, l, grip_, scale_, 1.0f, combined);
            // Keep the positional reference chosen on acquisition for this entire hold.
            // Leadership changes only the influence of new wrist motion, not the grip's
            // location. Crossfading position pulls the weapon between mismatched controller
            // anchors; accumulating offsets to hide that pull instead causes repeated-swap
            // drift. Both hands still steer the shaft and B still permits deliberate sliding.
            const Vec3 correction = anchor_left_
                ? Body::Rotate(Body::Conjugate(q),
                    BoneOrigin(left, l) - BoneOrigin(full.left, l)) : Vec3{};
            // Tracking recovery eases out of the last valid attachment instead of snapping
            // to a newly located controller. Fresh positional input has no added lag.
            if (handover_pending_) {
                handover_bias_ = anchor_local_ - correction;
                anchor_recovery_bias_ = {};
                anchor_recovering_ = false;
                handover_pending_ = false;
            } else {
                const float distance = Body::Length(handover_bias_);
                // (A long haft never moves with the hands held still, as its frame.)
                const float step = ((short_hilt_ ? HandoverStillMps : 0.0f) +
                                    HandoverShare * lead_mps) * apply_dt_;
                handover_bias_ = distance > step
                    ? handover_bias_ * ((distance - step) / distance) : Vec3{};
            }
            if (anchor_recovery_ < 1.0f) {
                anchor_recovery_bias_ = anchor_local_ - handover_bias_ - correction;
                anchor_recovering_ = true;
                anchor_recovery_ = 1.0f;
            }
            if (anchor_recovering_) {
                // Fade only the reacquisition error. Fresh tracked movement still follows
                // at full speed, instead of making a fast swing chase a speed-limited hand.
                const float distance = Body::Length(anchor_recovery_bias_);
                const float step = apply_dt_; // remove the recovery offset at 1 m/s
                anchor_recovery_bias_ = distance > step
                    ? anchor_recovery_bias_ * ((distance - step) / distance) : Vec3{};
                anchor_local_ = correction + anchor_recovery_bias_ + handover_bias_;
                anchor_recovering_ = distance > step;
            } else {
                anchor_local_ = correction + handover_bias_;
            }
        }
        Applied a = Place(rp, left, r, l, grip_, scale_, Weight(), combined);
        // The turn against the right controller itself, the lead frame included (combat's
        // thrust judges the blade by right.q * turn_local: a leading left's frame was missing).
        if (Body::IsFinite(qr)) {
            a.turn_local = Body::Normalize(Body::Conjugate(qr) * q * combined);
            a.turn_deg = Body::AngleOf(a.turn_local) / Body::Deg;
        }
        const Vec3 correction = Body::Rotate(q, anchor_local_) * Weight();
        a.right.p = a.right.p + correction;
        a.left.p = a.left.p + correction;
        // Restore the controller-following wrist used before the reviewed-handle change.
        // Keep the calibrated bone contact while rotating: a grip trim must not translate
        // the fist as the controller turns. Cache the relative orientation for occlusion.
        if (tracking_ok_ && Body::IsFinite(left.q)) {
            const Vec3 bone = BoneOrigin(a.left, l);
            a.left.q = Body::Normalize(left.q);
            a.left.p = bone - Body::Rotate(a.left.q, l.offset);
            if (Weight() > 0.0f) {
                grip_.q = Body::Normalize(Body::Conjugate(r.grip) *
                    PConj(Body::Conjugate(a.right.q) * a.left.q) * l.grip);
            }
        }
        return a;
    }
    // A briefly occluded support controller stays attached to the last valid weapon pose.
    Applied ApplyUntracked(const Pose& right, const Pose& left, const BoneOnGrip& r,
                           const BoneOnGrip& l) {
        tracking_ok_ = false;
        lead_candidate_s_ = 0;
        lead_turn_ok_ = false;
        anchor_recovery_ = 0;
        motion_ok_ = roll_sample_ok_ = roll_right_ok_ = false;
        ResetSteeringMotion();
        const Pose rp{right.p, Body::Normalize(Body::ScaleRotation(lead_frame_, Weight()) *
                                             Body::Normalize(right.q))};
        Applied a = Place(rp, left, r, l, grip_, scale_, Weight(), Combined());
        const Vec3 correction = Body::Rotate(rp.q, anchor_local_) * Weight();
        a.right.p = a.right.p + correction;
        a.left.p = a.left.p + correction;
        return a;
    }
    // Combat differentiates the displayed pose. Holding B never disables attacks, and wrist
    // handover no longer relocates the weapon. Only tracking recovery needs a fresh history.
    bool MotionStable() const {
        return !front_leads_ || (anchor_recovery_ >= 1.0f && !anchor_recovering_);
    }
    bool LeftLeads() const { return left_leads_; }
    // A grab that transforms the weapon, waiting for the hand: the game's animation carries it.
    bool Carrying() const { return carried_ && carry_ok_ && !seated_ && !acquisition_failed_; }
    // The carried hand: where it squeezed, rigid with the right hand (valid while CarryKnown).
    bool CarryKnown() const { return carry_ok_; }
    Pose CarriedLeft(const Pose& right) const {
        const Quat q = Body::Normalize(right.q);
        return {right.p + Body::Rotate(q, carry_local_.p), Body::Normalize(q * carry_local_.q)};
    }
    bool AcquisitionFailed() const { return acquisition_failed_; }
    float LeftLeadWeight() const { return left_lead_mix_; }
    void Reset() { *this = {}; }
    bool Seated() const { return seated_; }
    float Weight() const { return Body::Smoothstep(0.0f, 1.0f, blend_); }
    const GameGrip& Grip() const { return grip_; }
    float Scale() const { return scale_; }
    float Slide() const { return slide_; }
    float RightSlide() const { return right_slide_; }
    bool Swinging() const { return swinging_; }
    // Slide mode (B): both hands may slide; the weighted weapon mostly steps aside meanwhile.
    bool SlideMode() const { return allow_slide_; }
    // Y's push in effect (a long handle, seated).
    bool PushActive() const {
        return push_ || (explicit_slide_ && slide_right_ && !slide_left_ && LongHandle());
    }
    bool LongHandle() const {
        return seated_ && (range_.hi - range_.lo) * scale_ >= LongHandleM; // the weapon's own
    }
    bool ShortHilt() const { return seated_ && short_hilt_; }
    // The rear hand's say this frame (0..1: SpreadNoneM..SpreadFullM by distance, 0 pressed).
    float RearSay() const { return spread_; }
    float RailLength() const { return range_.hi - range_.lo; }
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
    // The holding hand keeps its place on the weapon; the other one, wanted at `other_on`,
    // stops at the rail's ends and a fist's width (FistGapM) short of the holding hand on the
    // side it is on: out of slide mode it never pushes the holding hand along nor passes it (the
    // user, 2026-10-28: the right sliding to the hilt pushed the leading left hand, and the
    // hands could swap). `left_holds`: the left keeps its place (else the right).
    void PlaceKeeping(bool left_holds, float other_on, bool allow_pass = false) {
        const float lo = range_.lo, hi = range_.hi;
        const float right_was = right_slide_, left_was = right_slide_ + slide_;
        const float held = std::clamp(left_holds ? left_was : right_was, lo, hi);
        const float was = left_holds ? right_was : left_was;
        float on = std::clamp(other_on, lo, hi);
        // A sword: the right hand no lower down its handle than the game's own left grip.
        if (left_holds && sword_) {
            on = pommel_hi_ ? std::min(on, native_m_) : std::max(on, -native_m_);
        }
        const float side = was >= held ? 1.0f : -1.0f;
        // Wanted at the holding hand or past it: pressed against it, the sliding hand has no say
        // in the weapon (spread_) until its controller is back on its own side (the user,
        // 2026-10-28: a controller gone past the lead, its hand held back, still steered).
        // Only deliberate sliding in the optimized scheme may pass the other fist.
        // The holding hand stays fixed and dominance is still locked until re-grab.
        pressed_ = !allow_pass && (on - held) * side < FistGapM + 1e-4f;
        if (!allow_pass && (on - held) * side < FistGapM) on = held + side * FistGapM;
        on = std::clamp(on, lo, hi);
        // No room on its side (a short handle, the holding hand at its end): it stays put.
        if (!allow_pass && (on - held) * side < FistGapM - 1e-4f)
            on = std::clamp(was, lo, hi);
        right_slide_ = left_holds ? on : held;
        slide_ = (left_holds ? held : on) - right_slide_;
    }
    void PlaceHands(float right_on, float spacing) {
        pressed_ = false; // both hands moving (slide mode): each where its controller is
        const float lo = range_.lo, hi = range_.hi;
        // The upper hand (the one further from the pommel end) leaves room for the lower one.
        const float room = std::min(UpperRoomM, std::max(0.0f, hi - lo));
        const float room_at = pommel_hi_ ? hi - room : lo + room;
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
    // The weapon's turn: rear_max_say_ of the way from where the grab left it (steer_base_) to
    // the hands' line (turn_); the rest is the lead hand's own aim (the weapon is drawn in the
    // lead hand's frame, lead_frame_).
    Quat Steered() const {
        const Quat rel = Body::Normalize(Body::Conjugate(steer_base_) * turn_);
        // The whole line again as it nears half a turn off the lead's aim (only a hand gone
        // round the other): a part of a half turn has no one direction, and would flip there.
        const float far_off = Body::Smoothstep(100.0f * Body::Deg, 170.0f * Body::Deg,
                                               Body::AngleOf(rel));
        float say = 1.0f + (rear_max_say_ - 1.0f) * (1.0f - far_off);
        // The rear fist slips off its controller by about the hands' distance times the part
        // of the line the weapon does not follow: at most RearSlipM, the line taking over past
        // it (2026-10-28: with the left leading, the hunter's right hand, which holds
        // the weapon, was carried up to a metre from the real one, and the arm garbled).
        const float lever = steer_apart_ * Body::AngleOf(rel);
        if (lever > RearSlipM) say = std::max(say, 1.0f - RearSlipM / lever);
        return Body::Normalize(steer_base_ * Body::ScaleRotation(rel, say));
    }
    // The steering turn with the left wrist's share of the roll (right controller's frame).
    Quat Combined() const {
        return Body::Normalize(Steered() * Body::AxisAngle(axis_, roll_delta_));
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
    bool seated_{false}, acquisition_failed_{false};
    bool waited_{false}; // this hold waited for the form (a transform): TransformGraceS
    bool carried_{false}; // ... because the weapon transformed: held where it squeezed
    bool carry_ok_{false};
    Pose carry_local_{}; // that place, in the right hand's frame
    float carry_ease_s_{0.0f}; // CarryEaseS left of the carried seat's soft slide
    bool fresh_attachment_{false};
    float pending_s_{}, pending_off_{};
    bool pending_sample_{false};
    bool allow_slide_{false};
    bool push_{false};
    bool short_hilt_{false}; // ShortHiltM: the lead hand alone steers this hold
    float spread_{1.0f};     // SpreadNoneM..SpreadFullM: the other hand's say, by distance
    bool pressed_{false};    // PlaceKeeping: the sliding hand is held back at the holding one
    Quat lead_prev_r_{}, lead_prev_l_{}; // both controllers' last turns (the lead hand's turn)
    Vec3 lead_prev_rp_{}, lead_prev_lp_{}; // ... and places (the lead hand's speed)
    Vec3 handover_bias_{};               // a short hilt's handover: the old hold, settling
    bool handover_pending_{false};
    Quat lead_frame_{};                  // the weapon's frame against the right controller
    bool lead_turn_ok_{false};
    float roll_previous_r_{};            // the right wrist's twist against the lead frame
    bool roll_right_ok_{false};
    bool explicit_slide_{false}, slide_left_{false}, slide_right_{false};
    bool tracking_ok_{false}, front_leads_{false}, left_leads_{false};
    float left_lead_mix_{0.0f}, lead_candidate_s_{0.0f}, anchor_recovery_{1.0f};
    bool anchor_left_{false};
    Vec3 anchor_local_{}, anchor_recovery_bias_{};
    bool anchor_recovering_{false};
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
    Vec3 contact_offset_{}; // native off-axis contact, retained while either hand slides
    Quat turn_{};       // the hands' line (steering), in the weapon frame
    Quat steer_base_{}; // the turn the grab started from
    float rear_max_say_{0.55f};
    float steer_apart_{0.0f}; // the hands' distance last steered (Steered's slip bound)
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
    MissedGrip, // hand left the handle before support attachment completed
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
    case Why::MissedGrip:
        return "hand left before attachment";
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
            dropped_ = false;
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
        } else if (out.edge || (in.grab_held && grip_on_ && owns_ && !dropped_)) {
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

    // A vial target can take this frame's failed near-handle squeeze. Keep the
    // button history so holding it cannot become a new weapon grab on the next frame.
    bool YieldNearGrab(Output& out) {
        if (engaged_ || !out.held_near || !out.edge || !owns_ || dropped_) return false;
        owns_ = false;
        out.held_near = out.owns_grip = false;
        return true;
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
        dropped_ = true;
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
    bool dropped_{false}; // a cancelled grab requires a fresh left-grip squeeze
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
        return TwoHanded(s) ? std::clamp(s.damage, 0.5f, 2.0f) : 1.0f;
    }
};

// One attack's factor from its windows': a two-handed one's (not 1) wins over a one-handed 1,
// whichever way it scales (two_hand_damage below 1 since 2026-11-04, so not the larger).
constexpr float CombineSwingFactor(float so_far, float window) {
    return window != 1.0f ? window : so_far;
}

} // namespace VR::TwoHand
