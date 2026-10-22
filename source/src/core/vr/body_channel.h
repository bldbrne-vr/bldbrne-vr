// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <array>
#include <cstdint>

#include "common/types.h"
#include "core/vr/body_frame.h"
#include "core/vr/grab_camera.h"
#include "core/vr/body_mounts.h"
#include "core/vr/vial_contact.h"

// VR body: data shared between threads (no guest code).
//  - Hands: every XR frame the OpenXR frame thread locates both grips for the head's target time
//    (display time + pose_prediction_ms, the time the head is located for) and publishes them; the
//    pose hook on the game's loop thread reads the newest. The runtime's own prediction thus puts
//    hands and head at one instant and the game side adds no extrapolation. Single-writer seqlock:
//    the frame thread never waits for the game.
//  - Anchor: the render-camera hook (loop thread) publishes the view anchor of every rendered
//    frame; the next frame's pose hook reads it. Also a seqlock, so readers on other threads (site
//    A, usually a job worker) are safe too.
//  - Pair lock: the other way, the pose hook publishes a stereo pair's second-eye lock as it
//    decided it (it places the hands for that eye); the comfort camera's second-eye tick follows
//    it (core/vr/bloodborne_motion.cpp).
//  - MARK: the presenter's MARK combo (Menu + left stick click); VR_BODY_LOG=2 logs every frame
//    for 2 s after one.
//  - Body status and events, the other way: the pose hook publishes what the presenter needs for
//    haptics, ghost hands and the grip adjust after every main-player pose copy (seqlock; the
//    copies are serialized by the hook's busy flag), and queues one-off events (a hand detached,
//    the body turned off) in a small mutex ring the presenter drains every XR frame.
namespace VR::Body {

// After every main-player pose copy (pose hook). A status older than about 250 ms means the hook
// is not running (loading, third person, the body off).
struct BodyStatus {
    GrabCamera::ThrowPlacement throw_placement{};
    GrabCamera::Sample grab_eye{}; // unmodified animation, before VR body/visibility edits
    VialContact::Anatomy injection_body{};
    BodyMounts::Pose mounts{};
    std::uint64_t seq{0};
    std::int64_t steady_ns{0};
    bool driven{false}; // the body is written into the hunter's pose (first-person VR view)
    // C0000::Left, C0000::Right: 1 = your hand, 0 = the game's. For using the hand (items, the
    // crank, the grip adjust): 0 while a kept hit or dodge (arm_keep_on_hit / arm_keep_on_dodge)
    // has not opened that hand's attack window, as if the game had the arm.
    std::array<float, 2> arm_weight{};
    // How much of the drawn arm is yours (the IK weight; the ghost hands, the late body, the
    // hurtboxes): 1 through a kept hit or dodge.
    std::array<float, 2> drawn_weight{};
    std::array<bool, 2> detached{};    // the hand is off the controller (out of the arm's reach)
    std::array<bool, 2> tracked{};     // tracked or held (not lost)
    std::int32_t weapon_id{-1};        // right hand's EquipParamWeapon id, -1 unknown
    std::int32_t weapon_slot{-1};      // selected right-hand equipment slot, -1 unknown
    std::int32_t left_weapon_slot{-1}; // selected left-hand equipment slot, -1 unknown
    std::int32_t weapon_family{-1};    // core/vr/bb_weapons.h
    std::int32_t left_weapon_family{-1}; // the left hand's, -1 unknown
    // The guard reactions the hunter played (a blow on his shield), +1 each: vr.ini shield_raise;
    // and the newest one's clip (its size: core/vr/trick_weapons.h GuardBlowSize).
    std::uint32_t guard_serial{0};
    std::int32_t guard_clip{-1};
    // Physical evasion (core/vr/bloodborne_evasion.cpp): the hurtboxes may follow your lean (no
    // dodge, grab, damage, death, event animation, ladder or throw), else what blocks it (a
    // static string). `upper_shift`: the UpperPos shift in this pose (world axes, game units),
    // which already moves the spine, head and arm hurtboxes; `upper_lean`: how far the trunk lean
    // (body_posture=lean) moved the eye in this pose (world axes, game units), which moves them by
    // their share of it (core/vr/evasion.h LeanReach).
    bool roomscale_ok{false}; // idle/locomotion only, with verified game movement permission
    const char* roomscale_why{""}; // why not (a string literal: the room-scale walk's log)
    float roomscale_yaw{0.0f}; // hunter heading for the directional lean allowance
    bool evade_ok{false};
    const char* evade_block{"body not driven"};
    std::array<float, 3> upper_shift{};
    std::array<float, 3> upper_lean{};
    // A kept hit or dodge: upper_shift / upper_lean are the arms' look only (the torso alignment
    // under the game's clip); the hurtboxes cancel them fully (core/vr/evasion.h Input::look_only).
    bool look_only{false};
    // The stagger vignette's level (core/vr/daze.h Level): a blow's stagger or a knockdown holds
    // the hunter before its attack window.
    std::uint8_t daze{0};
    // The hit-stun vignette's clip (core/vr/hit_stun.h StunClip): a blow's on-feet hit reaction
    // before the game takes R1 again (a swing now is refused), -1 = none.
    std::int32_t stun_clip{-1};
    // The camera against his eye after the body's alignment (metres; body_solver.h BodyOutput
    // cam_ahead / cam_above): ahead (+) / behind (-) along his torso's facing, above (+) / below
    // (-). The body cutaway's boost (core/vr/body_cut_boost.h). 0 while not driven.
    float cam_ahead_m{0.0f};
    float cam_above_m{0.0f};
    float cam_side_m{0.0f};
    // The VR heading (core/vr/bloodborne_facing.cpp) leaves his facing to the game while an
    // animation places him itself (an event animation, a ladder, a visceral or grab, death): what
    // (a static string), nullptr = the heading may drive him.
    const char* facing_block{nullptr};
};

enum class BodyEventKind : std::uint8_t {
    Detached, // `side`'s hand just left the arm's reach (at most one per 0.5 s per hand)
    Disabled, // the body turned itself off (the log says why)
};

struct BodyEvent {
    BodyEventKind kind{BodyEventKind::Detached};
    std::int32_t side{-1}; // C0000::Left / Right, -1 = both
};

[[nodiscard]] s64 SteadyNowNs();

// OpenXR frame thread only (one writer).
void PublishHands(const HandsSample& sample);
// Any thread: the newest sample. In --vr-sim with VR_BODY_SIM_HANDS=<presets> a scripted sample
// (core/vr/body_presets.h) relative to the synthetic head. False when there is none.
bool LatestHands(HandsSample& out);

// Render-camera hook only (one writer).
void PublishAnchor(const Anchor& anchor);
// Any thread. False before the first anchor.
bool LatestAnchor(Anchor& out);

// Pose hook only (one writer): a second eye's pair lock, decided at its pose copy.
void PublishPairLock(const PairLock& lock);
// Any thread. False before the first decision.
bool LatestPairLock(PairLock& out);

// Presenter input thread.
void NoteMark();
[[nodiscard]] u32 MarkCount();

// Pose hook (main-player copies, one at a time).
void PublishBodyStatus(const BodyStatus& status);
// Any thread. False before the first status.
bool LatestBodyStatus(BodyStatus& out);

// Any thread. A full queue (64 events) drops the new one.
void PushBodyEvent(const BodyEvent& event);
// Presenter: up to `max` queued events, oldest first; returns how many.
u32 PollBodyEvents(BodyEvent* out, u32 max);

} // namespace VR::Body
