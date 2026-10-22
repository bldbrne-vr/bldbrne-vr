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
#include <optional>

#include "core/vr/body_frame.h"

// The left-hand gun in VR, pure math and decisions (no emulator state): shared by the game side
// (core/vr/bloodborne_gun.cpp), the presenter (laser, haptics) and the unit tests
// (tests/test_gun_aim.cpp).
//
// Where the gun points. Every left gun part has its barrel along the part's -Z (the FLVER muzzle
// dummy 215 and barrel dummy 206 face (0, 0, -1) on every gun); the part hangs on L_Weapon at its
// attach dummy (c0000_skeleton.h DmyIdLeftGun*: 345, 347 or 348) with part +Z = the dummy's forward
// (convention A, checked at runtime against the muzzle dummy). With the left arm on IK the
// L_Weapon bone is the controller times DefaultGrip, so the barrel comes straight from the grip
// pose: 43.8 degrees below grip -Z for the pistols, 24.6 for the long guns, 61.6 for the cannons.
//
// The shot. The game builds every bullet's emitter matrix in 0x18C7800 (its column 2 = -the
// flight direction, the translation = the start); the barrel-ray hook replaces the rotation by a
// look-at of the barrel ray (WriteLookRows), keeps the start (the palm, after the game's wall
// pull-back) or moves it sideways onto the barrel line (OriginOnBarrel), and locked on turns the
// ray at most about 1.2 degrees toward the lock point (GentleAssist). Spread guns fan around it.
namespace VR::Gun {

using Body::Quat;
using Body::Vec3;

enum class Family : std::uint8_t {
    None,
    Pistol,       // Hunter Pistol, Evelyn (behaviour variation 1400)
    Repeating,    // Repeating Pistol (1410)
    Blunderbuss,  // Hunter Blunderbuss (600)
    Rifle,        // Ludwig's Rifle (610)
    Piercing,     // Piercing Rifle (3600)
    Cannon,       // Cannon (1500)
    ChurchCannon, // Church Cannon (3500)
    Gatling,      // Gatling Gun (3300)
    Sprayer,      // Rosmarinus, Flamesprayer (1800, 1810)
};

inline const char* FamilyName(Family f) {
    switch (f) {
    case Family::None:
        return "none";
    case Family::Pistol:
        return "pistol";
    case Family::Repeating:
        return "repeating";
    case Family::Blunderbuss:
        return "blunderbuss";
    case Family::Rifle:
        return "rifle";
    case Family::Piercing:
        return "piercing";
    case Family::Cannon:
        return "cannon";
    case Family::ChurchCannon:
        return "church_cannon";
    case Family::Gatling:
        return "gatling";
    case Family::Sprayer:
        return "sprayer";
    }
    return "?";
}

// These weapons keep the game's L2 held for a stream, not one chambered shot.
constexpr bool ContinuousFire(Family family) {
    return family == Family::Sprayer || family == Family::Gatling;
}

struct GunInfo {
    Family family{Family::None};
    std::int32_t attach{-1}; // L_Weapon attach dummy (EquipParamWeapon dmypolyId_Slot0LeftFormA)
    std::int32_t cost{0};    // bullets a shot takes (static fallback for bulletConsumeNum)
    bool gravity{false};     // the bullet falls (the assist turns the yaw only)
    bool sustained{false};   // a held stream (Gatling, sprayers)
};

constexpr std::int32_t BehaviorBase = 100000000;

// The gun of a behaviour variation (BehaviorParam id = 1e8 + variation * 1000 + judge).
inline GunInfo GunOfVariation(std::int32_t variation) {
    switch (variation) {
    case 1400:
        return {Family::Pistol, 347, 1, false, false};
    case 1410:
        return {Family::Repeating, 347, 2, false, false};
    case 600:
        return {Family::Blunderbuss, 345, 1, false, false};
    case 610:
        return {Family::Rifle, 345, 1, false, false};
    case 3600:
        return {Family::Piercing, 345, 1, false, false};
    case 1500:
        return {Family::Cannon, 348, 12, false, false};
    case 3300:
        return {Family::Gatling, 348, 1, false, ContinuousFire(Family::Gatling)};
    case 3500:
        return {Family::ChurchCannon, 350, 10, true, false};
    case 1800:
    case 1810:
        return {Family::Sprayer, 347, 1, false, ContinuousFire(Family::Sprayer)};
    default:
        return {};
    }
}

inline std::int32_t VariationOf(std::int32_t bid) {
    return bid >= BehaviorBase ? (bid - BehaviorBase) / 1000 : -1;
}

inline GunInfo GunOfBehavior(std::int32_t bid) {
    return GunOfVariation(VariationOf(bid));
}

// ---- Direct fire (the bang at the trigger pull) -------------------------------------------------

// What the vanilla gun clip does around its TAE event 318, per gun (TAE / params of the 1.09
// game): its cadence (bang to bang under L2 spam = the clip's left-attack cancel), the muzzle
// flash (event 96: FFX on the part's muzzle dummy, renewed for flash_s), the bang and the empty
// click (event 128, SoundType 1) and the stamina regen pause (event 225).
struct DirectGun {
    Family family{Family::None};
    float cadence_s{0.0f};
    std::int32_t flash_ffx{-1};
    std::int32_t flash_ffx_ash{-1}; // under Bone Marrow Ash (judge 810)
    std::int32_t flash_dummy{20215};
    float flash_s{1.0f};
    std::int32_t bang{-1};
    std::int32_t dry{-1};
    std::int32_t shot_dummy{3}; // the bullet's dummy (TAE 318 param): L_Hand's palm
    float regen_pause_s{0.0f};
};

// The guns that fire at the pull: the single-shot pistols and long guns. The others (cannons,
// Piercing Rifle, sustained guns) stay on the game's own L2, still aimed along the barrel.
inline const DirectGun* DirectGunOf(Family f) {
    static constexpr std::array<DirectGun, 4> table{{
        {Family::Pistol, 0.867f, 6102, 6103, 20215, 1.0f, 720, 721, 3, 0.433f},
        {Family::Repeating, 0.867f, 6102, 6103, 20215, 1.0f, 720, 721, 3, 0.433f},
        {Family::Blunderbuss, 1.167f, 6302, 6303, 20215, 1.0f, 700, 712, 3, 0.467f},
        {Family::Rifle, 1.400f, 6302, 6303, 20215, 1.0f, 700, 712, 3, 0.467f},
    }};
    for (const DirectGun& g : table) {
        if (g.family == f) {
            return &g;
        }
    }
    return nullptr;
}

// ---- The barrel ---------------------------------------------------------------------------------

struct AttachFrame {
    std::int32_t id;
    Vec3 forward; // the attach dummy's forward (the part's +Z) on L_Weapon
    Vec3 up;      // its up (the part's +Y, the top of the gun)
};

// The attach dummy's frame from the hunter's skeleton read at runtime (core/vr/c0000_skeleton.h:
// c0000.flver's dummies 345, 347 and 348, and the Church Cannon's 350 when it has one); nullopt
// for another id or until the skeleton is read.
inline std::optional<AttachFrame> AttachOf(std::int32_t attach) {
    const Body::C0000::Skeleton* const sk = Body::C0000::Active();
    if (sk == nullptr || attach <= 0) {
        return std::nullopt;
    }
    for (const Body::C0000::DummyPoly* d :
         {&sk->left_gun, &sk->left_gun2, &sk->left_gun3, &sk->left_gun4}) {
        if (d->id == attach) {
            return AttachFrame{d->id, {d->forward[0], d->forward[1], d->forward[2]},
                               {d->up[0], d->up[1], d->up[2]}};
        }
    }
    return std::nullopt;
}

// The barrel (the part's -Z) on L_Weapon; zero for an attach without a known frame (the Church
// Cannon's 350 when c0000.flver has no such dummy: the shot stays the game's) and until the
// skeleton is read.
inline Vec3 BarrelInWeaponBone(std::int32_t attach) {
    const auto f = AttachOf(attach);
    return f ? Body::Normalize(-f->forward) : Vec3{};
}

// The top of the gun on L_Weapon (the kick raises the muzzle toward it).
inline Vec3 UpInWeaponBone(std::int32_t attach) {
    const auto f = AttachOf(attach);
    return f ? Body::Normalize(f->up) : Vec3{};
}

// The barrel in OpenXR grip axes, through the body's left grip (the same DefaultGrip the left IK
// target uses, core/vr/body_frame.h): P * Rotate(DefaultGrip(Left, ...), barrel) with P =
// diag(1, 1, -1) (the controller frame is the P-conjugated grip). Zero when the attach is unknown.
inline Vec3 BarrelInGrip(std::int32_t attach, float blade_pitch_deg, float yaw_deg = 0.0f,
                         float pitch_deg = 0.0f, float roll_deg = 0.0f) {
    const Vec3 barrel = BarrelInWeaponBone(attach);
    if (Body::LengthSq(barrel) == 0.0f) {
        return {};
    }
    const Quat grip =
        Body::DefaultGrip(Body::C0000::Left, blade_pitch_deg, yaw_deg, pitch_deg, roll_deg);
    const Vec3 c = Body::Rotate(grip, barrel);
    return Body::Normalize(Vec3{c.x, c.y, -c.z});
}

// The world ray (unit) of a grip-frame direction for a grip orientation in tracking space and the
// camera's base yaw (core/vr/body_frame.h TrackingToWorld).
inline Vec3 BarrelRayWorld(const Quat& grip_q, Vec3 barrel_in_grip, float base_yaw) {
    return Body::Normalize(Body::TrackingToWorld(
        Body::Rotate(Body::Normalize(grip_q), barrel_in_grip), base_yaw, 1.0f));
}

// The grip orientation rotated so its -Z runs along the barrel (the laser's pose).
inline Quat BarrelPoseInGrip(Vec3 barrel_in_grip) {
    return Body::FromTo(Vec3{0.0f, 0.0f, -1.0f}, barrel_in_grip);
}

// Yaw (radians, the game's atan2(x, z) convention) and pitch of a world direction.
inline float YawOf(Vec3 d) {
    return std::atan2(d.x, d.z);
}
inline float PitchOf(Vec3 d) {
    return std::atan2(d.y, std::hypot(d.x, d.z));
}

// ---- The emitter matrix -------------------------------------------------------------------------

// The game's emitter (0x18C7800's out): a 3x4 row-major [R | t]. The bullet flies along -column 2
// of R (the launch composes M * Ry(pi + spread) and takes column 2). A look-at like the game's
// LookAt (0x1545F40): column 2 = -dir, column 1 = world up made perpendicular (`fallback_up`, e.g.
// the hunter's forward, when the ray is within 5.7 degrees of vertical), column 0 = y x z.
// Writes rows[r * 4 + 0..2]; the translation rows[r * 4 + 3] is left alone. False (nothing
// written) for a zero or non-finite ray.
inline bool WriteLookRows(float* rows12, Vec3 dir, Vec3 fallback_up) {
    const Vec3 d = Body::Normalize(dir);
    if (Body::LengthSq(d) == 0.0f || !Body::IsFinite(d)) {
        return false;
    }
    const Vec3 z = -d;
    Vec3 up = std::abs(d.y) < 0.995f ? Vec3{0.0f, 1.0f, 0.0f} : fallback_up;
    Vec3 y = Body::Normalize(Body::Perp(up, z));
    if (Body::LengthSq(y) == 0.0f) {
        y = Body::AnyPerpendicular(z);
    }
    const Vec3 x = Body::Cross(y, z);
    const std::array<Vec3, 3> cols{x, y, z};
    for (int c = 0; c < 3; ++c) {
        rows12[0 * 4 + c] = cols[static_cast<size_t>(c)].x;
        rows12[1 * 4 + c] = cols[static_cast<size_t>(c)].y;
        rows12[2 * 4 + c] = cols[static_cast<size_t>(c)].z;
    }
    return true;
}

// The flight direction the game takes from an emitter (no spread): -column 2.
inline Vec3 FlightOfRows(const float* rows12) {
    return Body::Normalize(Vec3{-rows12[2], -rows12[6], -rows12[10]});
}

// The bullet's start moved sideways onto the barrel line: P' = P + (I - D D^T)(M - P) for the
// muzzle M, never along the ray (no point-blank start inside a wall or a target). `shift` gets the
// move. False (P unchanged) when the move would be larger than `max_m` (a dummy that is not the
// gun's) or anything is not finite.
inline bool OriginOnBarrel(Vec3& p, Vec3 dir, Vec3 muzzle, float max_m, Vec3* shift = nullptr) {
    const Vec3 d = Body::Normalize(dir);
    if (Body::LengthSq(d) == 0.0f || !Body::IsFinite(p) || !Body::IsFinite(muzzle)) {
        return false;
    }
    const Vec3 s = Body::Perp(muzzle - p, d);
    if (!Body::IsFinite(s) || Body::Length(s) > max_m) {
        return false;
    }
    p = p + s;
    if (shift != nullptr) {
        *shift = s;
    }
    return true;
}

// ---- The very gentle lock-on assist -------------------------------------------------------------

struct AssistCfg {
    float strength{0.27f}; // share of the aiming error removed at most (gun_assist)
    float cone_deg{8.0f};  // no help beyond this; full strength inside half of it
    bool unlocked{false};  // also without lock-on (the game's aim point)
    float max_range_m{25.0f};
};

struct AssistOut {
    Vec3 dir{};
    float weight{0.0f};    // the share applied (0 = none)
    float angle_deg{0.0f}; // the ray's error against the target
    float moved_deg{0.0f}; // how far the ray turned
};

// Locked on and the ray within the cone of the target T (seen from the start P): the ray turns
// `strength` of the way toward it inside half the cone, fading to nothing at its edge (a shot 4
// degrees off turns 1.2 degrees; one 8 degrees off is untouched). Gravity guns: the yaw only (and
// only the yaw's error counts). No
// target, not locked (unless cfg.unlocked), T behind P, nearer than 0.5 m or beyond max_range_m,
// or anything not finite: the ray unchanged.
inline AssistOut GentleAssist(Vec3 D, Vec3 P, Vec3 T, bool have_target, bool locked, bool gravity,
                              const AssistCfg& cfg) {
    AssistOut out{};
    out.dir = Body::Normalize(D, D);
    if (!have_target || !(locked || cfg.unlocked) || cfg.strength <= 0.0f || !Body::IsFinite(D) ||
        !Body::IsFinite(P) || !Body::IsFinite(T) || Body::LengthSq(out.dir) == 0.0f) {
        return out;
    }
    const Vec3 v = T - P;
    const float distance = Body::Length(v);
    if (distance < 0.5f || distance > cfg.max_range_m) {
        return out;
    }
    const Vec3 t = v * (1.0f / distance);
    // A gravity gun aims above the target on purpose: only its heading's error counts.
    const float angle =
        gravity ? std::abs(Body::WrapPi(YawOf(t) - YawOf(out.dir))) : Body::Angle(out.dir, t);
    out.angle_deg = angle / Body::Deg;
    const float cone = std::max(cfg.cone_deg, 0.1f);
    if (out.angle_deg >= cone || Body::Dot(out.dir, t) <= 0.0f) {
        return out;
    }
    const float w = std::clamp(cfg.strength, 0.0f, 1.0f) *
                    (1.0f - Body::Smoothstep(0.5f * cone, cone, out.angle_deg));
    if (w <= 0.0f) {
        return out;
    }
    Vec3 next{};
    if (gravity) {
        // The yaw only: turn about world up by w of the horizontal angle.
        const float yaw = Body::WrapPi(YawOf(t) - YawOf(out.dir));
        next = Body::RotateAbout(out.dir, {0.0f, 1.0f, 0.0f}, w * yaw);
    } else {
        const Quat full = Body::FromTo(out.dir, t);
        next = Body::Rotate(Body::ScaleRotation(full, w), out.dir);
    }
    next = Body::Normalize(next, out.dir);
    out.moved_deg = Body::Angle(out.dir, next) / Body::Deg;
    out.dir = next;
    out.weight = w;
    return out;
}

// ---- Recoil -------------------------------------------------------------------------------------

// The visual kick of the hunter's hand at the bang (the IK target turned muzzle-up about Barrel x
// Up and pushed back along the barrel; the controller stays still).
struct Kick {
    float pitch_deg{0.0f};
    float back_m{0.0f};
    float attack_ms{25.0f};
    float tau_ms{45.0f};
};

inline Kick KickOf(Family f) {
    switch (f) {
    case Family::Pistol:
        return {10.0f, 0.025f, 25.0f, 45.0f};
    case Family::Repeating:
        return {12.0f, 0.03f, 25.0f, 50.0f};
    case Family::Blunderbuss:
    case Family::Rifle:
    case Family::Piercing:
        return {14.0f, 0.04f, 25.0f, 60.0f};
    case Family::Cannon: // (2026-10-13: shoved back as far as the Rifle Spear's blast, and more)
        return {22.0f, 0.10f, 30.0f, 90.0f};
    case Family::ChurchCannon: // (2026-10-13: a mortar, well above the blunderbuss)
        return {20.0f, 0.07f, 30.0f, 80.0f};
    case Family::Gatling:
        return {2.5f, 0.006f, 10.0f, 30.0f};
    case Family::Sprayer:
    case Family::None:
        break;
    }
    return {0.0f, 0.0f, 25.0f, 45.0f};
}

// 0 .. 1: up linearly over attack_ms, then a critically damped return (1 + s/tau) e^(-s/tau).
inline float KickEnvelope(const Kick& k, float t_ms) {
    if (!(t_ms >= 0.0f)) {
        return 0.0f;
    }
    if (t_ms < k.attack_ms) {
        return t_ms / std::max(k.attack_ms, 1e-3f);
    }
    const float s = (t_ms - k.attack_ms) / std::max(k.tau_ms, 1e-3f);
    return (1.0f + s) * std::exp(-s);
}

// The kick applied to an L_Weapon target (model space): rotated about the bone's Barrel x Up by
// `e * pitch_deg` (the muzzle toward the gun's top) and moved back along the barrel by `e *
// back_m`.
inline Body::ModelTarget ApplyKick(const Body::ModelTarget& target, std::int32_t attach,
                                   const Kick& k, float e) {
    const Vec3 b = BarrelInWeaponBone(attach);
    const Vec3 u = UpInWeaponBone(attach);
    if (e <= 0.0f || Body::LengthSq(b) == 0.0f) {
        return target;
    }
    Body::ModelTarget out = target;
    const Vec3 axis = Body::Normalize(Body::Cross(b, u));
    out.q = Body::Normalize(target.q * Body::AxisAngle(axis, e * k.pitch_deg * Body::Deg));
    out.p = target.p - Body::Rotate(out.q, b) * (e * k.back_m);
    return out;
}

// The sword hand's kick at a blade's shot, by the right weapon's behaviour variation: the
// Reiterpallasch's shot a pistol's flick, the Rifle Spear's blast a heavy shove, the Stake
// Driver's blast a jolt straight back up the arm.
inline Kick BladeKickOf(std::int32_t variation) {
    switch (variation) {
    case 1010:
        return {9.0f, 0.03f, 25.0f, 50.0f};
    case 1000:
        return {22.0f, 0.08f, 30.0f, 90.0f};
    case 1100: // a big jolt straight back up the arm, its tip thrown up (2026-10-13)
        return {14.0f, 0.20f, 30.0f, 130.0f};
    default:
        return {0.0f, 0.0f, 25.0f, 45.0f};
    }
}

// The kick applied to an R_Weapon target (c0000 model space: metres, +Y up; the blade along the
// bone's -Y): its tip thrown up by `e * pitch_deg` about the grip and the hand pushed back along
// the blade by `e * back_m`. The throw fades as the blade nears vertical (within 30 degrees of
// straight up or down: no flip across it, no side to side as the hand trembles there) and never
// carries the tip past straight up.
inline Body::ModelTarget ApplyBladeKick(const Body::ModelTarget& target, const Kick& k, float e) {
    if (!(e > 0.0f)) {
        return target;
    }
    const Vec3 blade = Body::Rotate(Body::Normalize(target.q), Vec3{0.0f, -1.0f, 0.0f});
    if (Body::LengthSq(blade) < 0.5f) {
        return target;
    }
    Body::ModelTarget out = target;
    const Vec3 side = Body::Cross(blade, Vec3{0.0f, 1.0f, 0.0f});
    const float level = std::sqrt(Body::LengthSq(side)); // sin of the angle from vertical
    const float to_up = std::acos(std::clamp(blade.y, -1.0f, 1.0f));
    const float angle =
        std::min(e * k.pitch_deg * Body::Deg * std::min(1.0f, level / 0.5f), to_up);
    if (level > 1e-4f && angle > 0.0f) {
        out.q = Body::Normalize(Body::AxisAngle(side * (1.0f / level), angle) * target.q);
    }
    out.p = target.p - blade * (e * k.back_m);
    return out;
}

// Both hands on the weapon (the two-handed grip, weight `w`): the other hand's target carried
// rigidly by the sword hand's kick (`before` -> `after`), so the hand on the shaft stays on it.
inline Body::ModelTarget CarryKick(const Body::ModelTarget& before, const Body::ModelTarget& after,
                                   const Body::ModelTarget& other, float w) {
    w = std::clamp(std::isfinite(w) ? w : 0.0f, 0.0f, 1.0f);
    if (w <= 0.0f) {
        return other;
    }
    const Quat turn = Body::Normalize(after.q * Body::Conjugate(Body::Normalize(before.q)));
    Body::ModelTarget moved{after.p + Body::Rotate(turn, other.p - before.p),
                            Body::Normalize(turn * other.q)};
    return {Body::Lerp(other.p, moved.p, w), Body::Slerp(other.q, moved.q, w)};
}

// ---- A pull's fate (the game's update, every frame while one is pending) ------------------------

enum class PullAction : std::uint8_t {
    Wait,    // keep it (cooldown about to end, the arm coming back, the vial stepping aside)
    Fire,    // now
    Deny,    // dropped (the "not ready" tick)
    Vanilla, // handed to the game's own L2 (gun_busy=vanilla)
};

enum class BusyMode : std::uint8_t { Buffer, Vanilla, Drop };

struct PullInputs {
    float age_ms{0.0f};          // since the trigger broke
    bool first{true};            // the first look at this pull
    float cooldown_left_s{0.0f}; // until the gun is ready again (game time)
    float buffer_s{0.12f};       // a pull this close to the end of the cooldown waits for it
    const char* hard{nullptr};   // a reason no shot can come (dead, menu, cinematic, grab, ...)
    bool busy{false};            // the game owns the left arm (dodge, damage, two hands)
    bool hidden{false};          // the gun is not in the hand yet (the vial stepping aside)
    bool stamina{true};          // stamina > 0 (the game refuses attacks without)
    float busy_ms{400.0f};       // the longest wait for the arm or stamina
    float hidden_ms{200.0f};     // ... for the gun to be back in the hand
    BusyMode busy_mode{BusyMode::Buffer};
};

struct PullDecision {
    PullAction action{PullAction::Wait};
    const char* why{""};
};

// One pull's verdict this update. A pull is never doubled: it fires, waits, or ends (deny or one
// vanilla L2) exactly once.
inline PullDecision DecidePull(const PullInputs& in) {
    if (in.hard != nullptr) {
        return {PullAction::Deny, in.hard};
    }
    if (in.first && in.cooldown_left_s > in.buffer_s) {
        return {PullAction::Deny, "cooldown"};
    }
    if (in.busy || !in.stamina) {
        if (in.age_ms <= in.busy_ms) {
            return {PullAction::Wait, in.busy ? "arm" : "stamina"};
        }
        const char* why = in.busy ? "arm busy" : "no stamina";
        return {in.busy && in.busy_mode == BusyMode::Vanilla ? PullAction::Vanilla
                                                             : PullAction::Deny,
                why};
    }
    if (in.hidden) {
        return in.age_ms <= in.hidden_ms ? PullDecision{PullAction::Wait, "hidden"}
                                         : PullDecision{PullAction::Deny, "gun not in hand"};
    }
    if (in.cooldown_left_s > 0.0f) {
        // Buffered at the first look; it fires the update the cooldown ends.
        return in.age_ms <= in.busy_ms + in.buffer_s * 1000.0f
                   ? PullDecision{PullAction::Wait, "cooldown"}
                   : PullDecision{PullAction::Deny, "cooldown"};
    }
    return {PullAction::Fire, ""};
}

} // namespace VR::Gun
