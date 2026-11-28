// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <cstdint>

#include "common/types.h"
#include "core/vr/arm_ik.h"

// VR combat: data handed between the OpenXR frame thread and the game (no guest code), like the
// body and quick-item channels.
//  - Gun pull (presenter -> game): every left-trigger edge in gameplay, with the left grip of that
//    frame. `direct`: the game's pose-copy wrapper fires it (core/vr/bloodborne_gun.h); otherwise
//    the presenter pressed L2 and the barrel-ray hook only takes the pull's grip for the game's own
//    shot. Single-writer seqlock (the newest pull; the serial tells a new one).
//  - Gun status (game -> presenter): after every main-player update, what a pull would do now (the
//    route and the predicted haptics). Seqlock.
//  - Gun events (game -> presenter): each pull's fate and every shot of the game's own (vanilla)
//    route, in a small mutex ring the presenter drains every XR frame (haptics, logs).
// Physical melee, the weapon transform and physical evasion follow the same pattern (below).
namespace VR::Combat {

// Either trigger on the two-hand saw. Refreshed every gameplay frame; stale input stops it.
struct SawRequest {
    std::int64_t ns{0};
    float rev{0.0f};
    bool held{false};
};
void PublishSawRequest(const SawRequest& request);
bool LatestSawRequest(SawRequest& out);

// Logarius' Wheel's spin (core/vr/wheel_rev.h): a step the left trigger started, played on the
// weapon only by the game's update (core/vr/bloodborne_transform.cpp). Seqlock (the serial tells
// a new one).
struct WheelSpin {
    std::uint32_t serial{0}; // +1 per step
    std::int32_t step{0};    // 1..WheelRev::Steps
    std::int64_t ns{0};      // steady clock
};
void PublishWheelSpin(const WheelSpin& spin);
bool LatestWheelSpin(WheelSpin& out);

struct GunPull {
    std::uint32_t serial{0};  // +1 per edge
    bool direct{false};       // the game fires it (else the game's own L2 path)
    std::int64_t press_ns{0}; // steady clock at the edge
    bool grip_valid{false}; // the left grip of that frame (OpenXR LOCAL, as published to the body)
    Body::Quat grip_q{};
    Body::Vec3 grip_p{};
    std::uint32_t space_serial{0}; // the tracking space it is in (recenters)
};

struct GunStatus {
    std::int64_t player_since_ns{0};
    std::uint32_t player_epoch{0}; // changes on character load; clears the old reload state
    std::int64_t ns{0};      // steady clock of the update
    std::uint8_t family{0};  // VR::Gun::Family of the left weapon (0 = no gun)
    std::int32_t attach{-1}; // its L_Weapon attach dummy
    bool aim{false};         // the barrel-ray hook is installed
    bool in_hand{false};     // the gun is in the hunter's left hand (one hand, arm on IK)
    bool direct_ok{false};   // a pull now goes to the direct path (supported gun, hooks, in hand)
    bool ready{false};       // ... and would fire at once (no cooldown, arm free, bullets)
    bool can_fire{false};    // enough bullets for one shot
    float cooldown_left_s{0.0f};
    float cadence_s{0.0f};    // bang to bang
    std::uint32_t handled{0}; // the newest pull serial the game has seen
};

enum class GunEventKind : std::uint8_t {
    Fired,    // a direct shot (a = launched ok 1/0)
    Dry,      // a direct pull without bullets: the click
    Denied,   // a direct pull dropped (why)
    Vanilla,  // a direct pull handed to the game's L2 (the presenter presses it once)
    GameShot, // the game's own (vanilla-timed) shot left along the barrel ray (a = idx 0 only)
    ToolBurst, // the Augur's burst left (tool_aim): the left hand feels it go
};

struct GunEvent {
    GunEventKind kind{GunEventKind::Fired};
    std::uint32_t serial{0};
    std::uint8_t family{0};
    float a{0.0f};
    std::int64_t ns{0};
    const char* why{""}; // a static string
};

// Presenter (one writer).
void PublishGunPull(const GunPull& pull);
// Any thread. False before the first pull.
bool LatestGunPull(GunPull& out);

// The pose-copy wrapper (one writer).
void PublishGunStatus(const GunStatus& status);
// Any thread. False before the first status.
bool LatestGunStatus(GunStatus& out);

// Any thread. A full ring (64) drops the new event.
void PushGunEvent(const GunEvent& event);
// Latest successful chambered gun shot (excludes sprayers/Gatling), retained even if
// the feedback queue overflows. Continuous-fire emissions still produce ordinary feedback.
// Steady-clock timestamp; compare against GunStatus::player_since_ns after a load.
s64 LatestReloadShotNs();
// Presenter: up to `max` queued events, oldest first.
u32 PollGunEvents(GunEvent* out, u32 max);

// ---- Physical melee (core/vr/melee_window.h, core/vr/bloodborne_melee.h) ------------------------
//  - Melee window (presenter -> game): the newest swing's window, published every XR frame while
//    one is open or just closed (the serial tells a new one). Seqlock.
//  - Melee status (game -> presenter): after every main-player update, whether a swing would be
//    the game's own attack now (the right weapon has physical moves in its form, the hooks are
//    in): otherwise the presenter keeps the R1 / R2 buttons. Seqlock.
//  - Melee events (game -> presenter): what each window did (opened, denied, hit, wall, visceral),
//    for haptics and logs. Mutex ring.

struct MeleeWindow {
    std::uint32_t serial{0};
    std::uint8_t kind{0}; // VR::Melee::MoveKind
    bool open{false};
    bool committed{false};        // through the swing gate (else opened at the stroke's start)
    bool aborted{false};          // closed without committing
    bool verdict_known{false};    // the classifier's light / heavy for a heavy window
    std::uint8_t verdict{0};      // ... (MoveKind)
    float tip_speed{0.0f};        // m/s, the blade's far end now
    float tip_peak{0.0f};         // ... the window's peak so far
    float hand_peak{0.0f};        // m/s, the weapon hand's peak (head-relative): the bonus's arm
    float arc_peak{0.0f};         // rad, the widest its stroke swept (a Beast Claw cut's "wide")
    float since_counted_s{10.0f}; // from the last window that counted to this one's opening
    std::int64_t open_ns{0};      // steady clock
    std::int64_t close_ns{0};     // 0 while open
    std::int64_t ns{0};           // this publication
    // The window before it (the game may never see that one closed when both fall into one game
    // frame): its serial and whether it was aborted.
    std::uint32_t prev_serial{0};
    bool prev_aborted{false};
    // The combo budget (melee_budget; core/vr/melee_window.h ComboBudget): `unpaid`, a swing it
    // could not pay: no attack (its whoosh and trail only) unless a transform flick joins it (the
    // transform attack never needs charges); else the charges it holds, those left after it and
    // the charge count (0: no budget), when they come back (s), paid only at its commit.
    bool unpaid{false};
    std::uint8_t budget_used{0};
    std::uint8_t budget_left{0};
    std::uint8_t budget_cap{0};
    float budget_refill_s{0.0f};
    bool budget_late{false};
    bool budget_downgraded{false}; // wound up with one charge left: a light swing
    // A two-handed swing (core/vr/two_hand_grip.h SwingMark: the left grip held the handle and
    // the left hand moved with it): its hits x this (two_hand_damage); 1 = one-handed.
    float two_hand{1.0f};
    bool two_handed{false}; // both hands moved; independent of the configured damage bonus
    // Optimized controls: unsupported shared-handle form. -1 means no penalty.
    std::int32_t unsupported_family{-1};
    bool powered{false}; // released Stake Driver charge; the mechanism supplies the impact
    bool contact_hold{false}; // Cane/Cutter whip sweep: pace from first damaging contact
    std::uint16_t carried_lashes{0}; // ... the lashes it carried after its own (each pays stamina)
    // Its hits x this (core/vr/dual_blades.h DamageScale: the split blades', a honed one's), taken
    // when its attack starts; 1: untouched.
    float damage_scale{1.0f};
    std::int32_t chikage_draw_weapon{-1}; // exact equipped right weapon at the draw
    bool blood_blast{false}; // the Bloodletter's blast without its animation (core/vr/blood_blast.h)
    std::int64_t chikage_coat{}; // expiry identifying the clean blade coat this window may spend
};

struct MeleeStatus {
    std::int64_t ns{0};
    bool installed{false};      // the physical-melee functions matched
    bool physical{false};       // melee=physical
    bool ok{false};             // a swing now would be the game's attack (else: R1 / R2)
    std::int32_t variation{-1}; // the right weapon's behaviour variation
    bool form_b{false};
    bool charged_melee{false}; // a verified charged attack row in this form (stake trigger routing)
    bool blade_tip_valid{false};
    Body::Vec3 blade_tip{}; // native hit tip in R_Weapon's frame, game units
    float blade_strike_near{0.0f}; // the striking part begins this far from the grip (game units)
    std::uint32_t handled{0}; // the newest window serial the game has seen
    const char* why{""};      // when !ok (a static string)
    // Simon's Bowblade in its bow form (vr.ini bow, core/vr/bow.h): the hand its bow hangs on
    // (0 left, 1 right; -1: not the bow form), and whether its arrows leave at the release
    // (bow_direct: BladeShot) or with the game's own shot clips (R1 / R2).
    std::int8_t bow_hand{-1};
    bool bow_direct{false};
    // The left hand's swings (melee_left, core/vr/trick_weapons.h): a left swing would be the
    // game's attack now (a left part of the right weapon, or a left-hand melee weapon), else why.
    bool left_ok{false};
    const char* left_why{""};
    bool left_learning{false}; // ... a left part or weapon with no row yet: a swing is denied
    std::uint32_t left_handled{0}; // the newest left window serial the game has seen
    // ... the right weapon's left part (the dual blades, the claw, the parasite: both hands swing
    // together; else a left swing with a right one is the off hand's counter-motion), and a
    // handgun's punch (the bare fist's, the gun in the hand: a harder swing, never at the trigger).
    bool left_dual{false};
    bool left_gun{false};
    bool left_shield{false}; // a shield uses the punch detector, but is not a gun aim
    // The hunter falling (melee_plunge's air tracker: in the air and going down), and how far so
    // far (m): a blade held point down then is a plunge (the presenter opens its window).
    bool falling{false};
    float fall_m{0.0f};
};

enum class MeleeEventKind : std::uint8_t {
    Opened,   // the attack exists (a = stamina taken, b = kind)
    Denied,   // a window the game refused (why)
    Hit,      // an enemy took damage (a = damage, b = hit stop s)
    Wall,     // the blade hit the map (a = 1 if the game's bounce was suppressed)
    Visceral, // a visceral, riposte or backstab started (a = ThrowParam id)
    Refunded, // stamina given back (a = amount)
    // The window's stroke became a flick's transform attack before its own attack hit anything:
    // the combo budget gives its charges back (transforms never use them).
    BudgetRefund,
    // A plunge's landing (melee_plunge, core/vr/plunge.h): a = the fall's depth (m), b = 1 if it
    // hit something on the way down or as it landed.
    Plunge,
    // A right-hand shot's fate (BladeShot): a = its VR::Tricks::Shot, b = 1 fired, 0 no bullets
    // (the click), -1 refused (why).
    Shot,
    // A left swing's attack hit an enemy (melee_left; serial: its left window): a = damage.
    LeftHit,
    // A left window the game refused (melee_left; serial: its left window; why): its charges come
    // back, the left hand gets the deny tick.
    LeftDenied,
    // A bullet sent back at its shooter (bullet deflection, core/vr/bloodborne_deflect.cpp): a = 0
    // as the blade sends its blast back (b = a swing's return's chance, 1 for straight back), 1
    // when the blast first hits him (b = that hit's damage).
    Ricochet,
    // A bare hand on a corpse (core/vr/bloodborne_corpse.cpp; serial: the hand, 0 left, 1 right):
    // a = 0 it touched one (b = how fast it moved into it, m/s), 1 it took hold (b = the corpse's
    // mass, kg), 2 the corpse slipped out, 3 still holding (b = the share of its strength used, every
    // 0.1 s), 4 it let go.
    Corpse,
    // A contact-held sweep's attack (Cane/Cutter whip) ended (why): its window closes too.
    Ended,
    // The lit Boom Hammer exploded (core/vr/boom_blast.h; serial: its swing): its fire is spent,
    // whatever the blast hit (an enemy, a wall, the ground). a = 1 for the charged's.
    Blast,
    // A cut came off (core/vr/bloodborne_sever.cpp, as its piece is first drawn): the weapon hand
    // feels the tear and the weight leaving it (sever_feel.h SeverTear), after its bite's blow,
    // never over a newer hit. serial: the cut's (its SeverBite's); a = 1 for a charged cut's;
    // b = the same striking hands as SeverBite (sever_feel.h StrikeHands).
    Sever,
    // A killing blow that cuts (bloodborne_sever.cpp SeverKill, at the hit): its bite, the hit's
    // own impact made sharper and fuller (sever_feel.h SeverBite). serial: the cut's (never 0);
    // a = 1 for a charged cut's; b = 1 the left claw's (the left hand), 2 a two-handed swing's
    // (both hands, as its hit's impact).
    SeverBite,
    // The game confirmed an object broke. serial = matched attack; a = hand (0 left / 1
    // right), b = the right attack's shared-grip flag. Feedback only, never an enemy hit.
    ObjectBreak,
    BloodBlast, // Bloodletter: the game created the physical slam's attack and started its blood FX.
};

// A shot refused for want of stamina (Shot's why): no arrow, and no game shot in its place.
inline constexpr char NoStamina[] = "no stamina";

struct MeleeEvent {
    MeleeEventKind kind{MeleeEventKind::Opened};
    std::uint32_t serial{0};
    float a{0.0f};
    float b{0.0f};
    std::int64_t ns{0};
    const char* why{""}; // a static string
    // Hit / LeftHit: the damage scale its attack took when it started (MeleeWindow::damage_scale:
    // the split blades', a honed one's), so a hone is spent only by a hit that carried it.
    float scale{1.0f};
};

// The telegraph (presenter -> game; core/vr/melee_window.h TelegraphTracker): a swing the enemies
// may read. The game's main-player update sends the AIs the game's own attack notice for each new
// serial. Seqlock.
struct MeleeTelegraph {
    std::uint32_t serial{0}; // +1 per telegraph
    std::uint32_t window{0}; // the window it announces (0: a wind-up, before its window)
    std::int64_t ns{0};      // steady clock
    const char* why{""};     // a static string
};

// Presenter (one writer).
void PublishMeleeWindow(const MeleeWindow& window);
// Any thread. False before the first window.
bool LatestMeleeWindow(MeleeWindow& out);
// The left hand's swings (melee_left): the same record, its own serials. Presenter (one writer).
void PublishMeleeWindowLeft(const MeleeWindow& window);
// Any thread. False before the first left window.
bool LatestMeleeWindowLeft(MeleeWindow& out);

// Presenter (one writer).
void PublishMeleeTelegraph(const MeleeTelegraph& telegraph);
// Any thread. False before the first telegraph.
bool LatestMeleeTelegraph(MeleeTelegraph& out);

// The pose-copy wrapper (one writer).
void PublishMeleeStatus(const MeleeStatus& status);
bool LatestMeleeStatus(MeleeStatus& out);

// Any thread. A full ring (64) drops the new event.
void PushMeleeEvent(const MeleeEvent& event);
// Presenter: up to `max` queued events, oldest first.
u32 PollMeleeEvents(MeleeEvent* out, u32 max);

// ---- The right hand's shots (core/vr/trick_weapons.h Shot, core/vr/bloodborne_gun.cpp) ---------
//  - Blade shot (presenter -> game): a gun-blade's trigger, the Holy Moonlight Sword's wave, the
//    Stake Driver's blast: the game's update fires the weapon's own bullet row with no animation
//    (the answer: MeleeEventKind::Shot). Seqlock (the serial tells a new one).

struct BladeShot {
    std::uint32_t serial{0};  // +1 per shot
    std::uint8_t shot{0};     // VR::Tricks::Shot
    bool charged{false};      // the charged row (the big wave)
    std::int64_t press_ns{0}; // steady clock
    // The flight (LOCAL, unit; the wave, the punch), else along the weapon's own line.
    bool dir_valid{false};
    Body::Vec3 dir{};
    std::uint32_t space_serial{0}; // the tracking space `dir` is in (recenters)
    std::uint32_t window{0};       // the swing it belongs to (0: the trigger's)
    Body::Vec3 slash{}; // MoonWave only: blade travel at peak speed, in dir's tracking space
};

// Presenter (one writer).
void PublishBladeShot(const BladeShot& shot);
// Any thread. False before the first shot.
bool LatestBladeShot(BladeShot& out);

// ---- The weapon-only transform (core/vr/transform.h, core/vr/bloodborne_transform.h) ------------
//  - Transform pull (presenter -> game): every right-grip squeeze the presenter sends to the
//    native transform instead of L1. Seqlock (the serial tells a new one).
//  - Transform status (game -> presenter): after every main-player update, whether a squeeze now
//    goes native (the right weapon has a clip in its form, the hooks are in) or stays L1. Seqlock.
//  - Transform events (game -> presenter): started, each mechanical sound, the lock (the form
//    switched), done, refused or aborted, for haptics and logs. Mutex ring.

struct TransformPull {
    std::uint32_t serial{0};  // +1 per squeeze
    std::int64_t press_ns{0}; // steady clock
    // Squeezed during a swing or with the weapon hand moving (transform_flick): the weapon snaps
    // into its new form mid-stroke (core/vr/transform.h FlickClock).
    bool flick{false};
    std::uint32_t window{0}; // the melee window open at the squeeze (0: none; logs)
    // A hand transform (hand_transform, core/vr/trick_weapons.h): no swing ever joins its run
    // (no flick, no transform attack).
    bool no_join{false};
};

// The Boom Hammer's fire spent (the presenter: boom_fire_hits landed while lit; the user,
// 2026-11-06: "can we have the fire go out after doing a hit?", as the base game's one-hit
// buff): back to form A, the weapon only, once the swing that spent it lets go of the arm
// (core/vr/bloodborne_transform.cpp).
struct FurnaceQuench {
    std::uint32_t serial{0}; // +1 per spent fire
    std::int64_t ns{0};      // steady clock (the hit)
};

struct TransformStatus {
    std::int64_t ns{0};
    bool installed{false}; // the hook and the form switch matched
    // The weapon-only transform runs (installed, transform=native): a FurnaceQuench is carried
    // out (else it is dropped, and the lit Boom Hammer's squeeze stays the game's L1).
    bool native{false};
    bool ok{false};        // a squeeze now goes native (else: L1)
    bool busy{false};      // a transform is running
    std::int32_t weapon_id{-1};
    std::int32_t style{-1};
    std::int32_t clip{-1};    // the clip a squeeze would play
    std::uint32_t handled{0}; // the newest squeeze serial the game has seen
    const char* why{""};      // when !ok (a static string)
};

enum class TransformEventKind : std::uint8_t {
    Started, // a = the clip id, b = seconds to the switch
    Detent,  // a mechanical sound of the clip (a = its id)
    Locked,  // the form switched (a = the new style)
    BladeSeated, // the Saif reached its mechanical stop (a = the new style)
    Done,    // the clip ended
    Denied,  // a squeeze refused (why)
    Aborted, // a running transform stopped (why; a = 1 after the switch)
};

struct TransformEvent {
    TransformEventKind kind{TransformEventKind::Started};
    bool saif{false}; // detent/lock from the Saif's own clip, even if the weapon changes on delivery
    std::uint32_t serial{0};
    float a{0.0f};
    float b{0.0f};
    std::int64_t ns{0};
    const char* why{""}; // a static string
};

// Presenter (one writer).
void PublishTransformPull(const TransformPull& pull);
// Any thread. False before the first squeeze.
bool LatestTransformPull(TransformPull& out);
// Presenter (one writer); any thread. False before the first.
void PublishFurnaceQuench(const FurnaceQuench& quench);
bool LatestFurnaceQuench(FurnaceQuench& out);

// The Chikage's scabbard left blood on the clean blade (the presenter, core/vr/chikage.h): the
// game shows it until then (steady clock; 0: none), core/vr/bloodborne_transform.cpp.
struct ChikageBlood {
    std::int64_t until_ns{0};
};
void PublishChikageBlood(const ChikageBlood& blood);
bool LatestChikageBlood(ChikageBlood& out);
bool SpendChikageBlood(std::int64_t coat, std::int64_t now, float damage);

// The pose-copy wrapper (one writer).
void PublishTransformStatus(const TransformStatus& status);
bool LatestTransformStatus(TransformStatus& out);

// Any thread. A full ring (64) drops the new event.
void PushTransformEvent(const TransformEvent& event);
// Presenter: up to `max` queued events, oldest first.
u32 PollTransformEvents(TransformEvent* out, u32 max);

// ---- Physical evasion (core/vr/bloodborne_evasion.h) --------------------------------------------
//  - Evade events (game -> presenter): a wall stopped your lean (a faint knock on both hands), a
//    blow that would have hit you without the lean went past (a whoosh on the side it passed).
//    Mutex ring.

enum class EvadeEventKind : std::uint8_t {
    Wall,     // the wall limit engaged (a = how much of the lean it held back, m)
    NearMiss, // an attack missed you because you leaned (side: C0000 Left 0 / Right 1)
};

struct EvadeEvent {
    EvadeEventKind kind{EvadeEventKind::Wall};
    std::int32_t side{-1}; // -1 = both
    float a{0.0f};
    std::int64_t ns{0};
};

// Any thread. A full ring (64) drops the new event.
void PushEvadeEvent(const EvadeEvent& event);
// Presenter: up to `max` queued events, oldest first.
u32 PollEvadeEvents(EvadeEvent* out, u32 max);

} // namespace VR::Combat
