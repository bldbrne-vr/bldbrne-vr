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
#include <cstddef>
#include <cstdint>

#include "core/vr/c0000_anim_lengths.h"
#include "core/vr/c0000_transform_lengths.h"
#include "core/vr/cinematic_state.h"

// VR body: who owns each of the hunter's arms (phantom-hand gating). Your tracked arms by
// default; the game's own animation while it plays a move that needs the hunter's arms (an
// attack, a dodge, a hit, an item, a door, a gesture), and both arms during a visceral or a grab,
// on a ladder, during an event animation and when dead. The pose hook eases each arm's IK weight
// toward the answer (in arm_blend_in_ms, out arm_blend_out_ms).
//
// Animation ids are TimeAct ring ids: TAE group * 1000000 + animation. The group is a weapon
// (a1xx: attacks) or a motion variant (a002..a233); the animation number alone decides the class.
// The ring gets an entry for every playing clip on every behaviour step (the clip's time in its
// floats), so a clip that plays on is seen again every frame; the newest one is the move playing
// now. An animated class holds the arms until an id of an IK class (idle, locomotion, falling, a
// pickup, a left-hand attack) replaces it ("next id", where the class allows it), or at the latest
// until its length + 0.1 s (capped per class) has passed since it was last seen. Unknown ids keep
// your arms and never end a held move. The length is the TAE's (c0000_anim_lengths.h), except for
// the game's weapon transform (L1, a000_019xxx): every transform clip has a TAE event over
// 0 .. 4.567 s, so it holds by the clip's own length (c0000_transform_lengths.h; the Saw
// Cleaver's 1.167 s).
//
// The game's own cancel windows end a hit, a guard reaction, a dodge, an item use or the game's
// transform as soon as the game frees the hunter: the TAE "JumpTable" events of the playing clip
// set the requests it accepts in the action request module ([PlayerIns+0x3B0]+0x80: +0x48 a mask
// of the request types, bit 0/1 = R1/R2 (event 4), bit 3 = L2 (event 16); +0xA0 bit 1 = a move
// cancels it, event 11), every frame. The right arm comes back when R1 would be taken, the left
// when L2 would, and the whole hold ends when a move would cancel the clip (or the next id). The
// transform (L1: transform=vanilla, and the weapons the weapon-only transform leaves on L1) opens
// them after its form switch: the Saw Cleaver's switches at 0.467 s, hands the arms back at 0.5 s
// and ends at 0.7 s of its 1.167 s. A weapon switch keeps both arms on your controllers (the
// weapons swap in your hand) and only refuses swings and shots until the game takes R1 again.
//
// The ladder gate has a dead latch (as cinematic_state.h's LadderGate): the death states never
// write the HKS ladder state, so after a death on a ladder it keeps a climbing value through the
// respawn. After HP <= 0 or a death clip the byte is ignored until it changes or a climbing clip
// (getting on, a step up or down) arrives while alive.
//
// Pure (no emulator state): core/vr/bloodborne_body.cpp reads the ring and the hard gates at the
// pose copy and steps AnimGate; tests/test_body_solver.cpp drives it. Times are steady-clock
// nanoseconds.
namespace VR::Body {

enum class AnimClass : std::uint8_t {
    Idle,          // 0-2999: idle and its variants
    Locomotion,    // 3000-6999: walk, run, dash, sprint, turns
    Dodge,         // 7000-7999 except the jump: backstep, quickstep, roll, wake-up roll
    FallJump,      // 7800 jump, 8000-8999 fall and landing
    Grabbed,       // 9000-9999: thrown by an enemy [U]
    GuardReaction, // 10000-10999: guard and defence reactions
    WeaponSwitch,  // 11000-12999: drawing the next weapon (right 110xx, left 111xx, 112xx;
                   // 120xx: 12001 and 12011 right after right-hand switches, 2026-10-15)
    Unverified,    // 103000-103999 [U]: the game's arms, logged
    Transform,     // 19000-19999: the game's weapon transform (L1), a full-body clip
    Damage,        // 20000-23999: damage reactions and knockdowns
    Death,         // 24000-24999
    AttackRight,   // 30000-39999: right-hand weapon attacks (form A), the hunter's throws
    AttackBoth,    // 40000-49999: form B and two-handed attacks
    AttackLeft,    // 50000-59999: left-hand attacks, guns (the gun stays in your hand)
    ItemUse,       // 100000-100999
    Event,         // 101000-101999 except the pickup: doors, levers, lamps, ladders
    Pickup,        // 101140: your arms stay (your hand is already at the item)
    Gesture,       // 104000-104999
    Unknown,       // anything else: your arms, logged
};
constexpr int AnimClassCount = 19;

inline const char* AnimClassName(AnimClass c) {
    switch (c) {
    case AnimClass::Idle:
        return "idle";
    case AnimClass::Locomotion:
        return "locomotion";
    case AnimClass::Dodge:
        return "dodge";
    case AnimClass::FallJump:
        return "fall_jump";
    case AnimClass::Grabbed:
        return "grabbed";
    case AnimClass::GuardReaction:
        return "guard_reaction";
    case AnimClass::WeaponSwitch:
        return "weapon_switch";
    case AnimClass::Unverified:
        return "unverified";
    case AnimClass::Transform:
        return "transform";
    case AnimClass::Damage:
        return "damage";
    case AnimClass::Death:
        return "death";
    case AnimClass::AttackRight:
        return "attack_right";
    case AnimClass::AttackBoth:
        return "attack_both";
    case AnimClass::AttackLeft:
        return "attack_left";
    case AnimClass::ItemUse:
        return "item_use";
    case AnimClass::Event:
        return "event";
    case AnimClass::Pickup:
        return "pickup";
    case AnimClass::Gesture:
        return "gesture";
    case AnimClass::Unknown:
        return "unknown";
    }
    return "?";
}

constexpr std::int32_t PickupAnimId = 101140;
constexpr std::int32_t JumpAnimBase = 7800;

constexpr AnimClass ClassifyAnim(std::int32_t id) {
    if (id < 0) {
        return AnimClass::Unknown;
    }
    const std::int32_t base = id % 1'000'000;
    if (base <= 2999) {
        return AnimClass::Idle;
    }
    if (base <= 6999) {
        return AnimClass::Locomotion;
    }
    if (base == JumpAnimBase) {
        return AnimClass::FallJump;
    }
    if (base <= 7999) {
        return AnimClass::Dodge;
    }
    if (base <= 8999) {
        return AnimClass::FallJump;
    }
    if (base <= 9999) {
        return AnimClass::Grabbed;
    }
    if (base <= 10999) {
        return AnimClass::GuardReaction;
    }
    if (base <= 12999) {
        // 120xx too: as the game's arms they held both arms for 4.7 s after a right-hand switch.
        return AnimClass::WeaponSwitch;
    }
    if (base >= 103000 && base <= 103999) {
        return AnimClass::Unverified;
    }
    if (base >= 19000 && base <= 19999) {
        return AnimClass::Transform;
    }
    if (base >= 20000 && base <= 23999) {
        return AnimClass::Damage;
    }
    if (base >= 24000 && base <= 24999) {
        return AnimClass::Death;
    }
    if (base >= 30000 && base <= 39999) {
        return AnimClass::AttackRight;
    }
    if (base >= 40000 && base <= 49999) {
        return AnimClass::AttackBoth;
    }
    if (base >= 50000 && base <= 59999) {
        return AnimClass::AttackLeft;
    }
    if (base >= 100000 && base <= 100999) {
        return AnimClass::ItemUse;
    }
    if (base == PickupAnimId) {
        return AnimClass::Pickup;
    }
    if (base >= 101000 && base <= 101999) {
        return AnimClass::Event;
    }
    if (base >= 104000 && base <= 104999) {
        return AnimClass::Gesture;
    }
    return AnimClass::Unknown;
}

struct AnimPolicy {
    bool right_ik{true};         // true: your arm (IK); false: the game's animation
    bool left_ik{true};          //
    bool torso{true};            // the torso alignment stays (false: a full-body animation)
    bool release_on_next{false}; // an id of a releasing class hands the arms back early
    bool releases{false};        // an id of this class ends a held animation ("next id")
    float cap_s{0.0f};           // held at most min(length + 0.1 s, cap_s) (HoldLengthSeconds)
    bool cancel_release{false};  // the game's cancel windows hand the arms back (GateSignals)
};

// A ladder clip that starts or goes on with a climb: getting on, a step up (also the fast one) or
// down. The ladder dead latch's release.
constexpr bool IsClimbClip(std::int32_t id) {
    using Cinematic::LadderClip;
    const LadderClip clip = Cinematic::ClassifyLadderClip(id % 1'000'000);
    return clip == LadderClip::Mount || clip == LadderClip::Up || clip == LadderClip::FastUp ||
           clip == LadderClip::Down;
}

constexpr AnimPolicy PolicyOf(AnimClass c) {
    switch (c) {
    case AnimClass::Idle:
    case AnimClass::Locomotion:
    case AnimClass::FallJump:
    case AnimClass::AttackLeft:
    case AnimClass::Pickup:
        return {true, true, true, false, true, 0.0f};
    case AnimClass::Unknown:
    case AnimClass::WeaponSwitch: // your arms; AnimGate refuses swings and shots meanwhile
        return {true, true, true, false, false, 0.0f};
    case AnimClass::Dodge:
        return {false, false, false, true, false, 2.1f, true};
    case AnimClass::Grabbed:
    case AnimClass::Unverified:
        return {false, false, false, false, false, 5.0f};
    case AnimClass::Transform: // the longest clip (Bloodletter, 3.167 s) fits under the cap
        return {false, false, false, true, false, 3.5f, true};
    case AnimClass::GuardReaction:
        return {false, false, false, true, false, 2.0f, true};
    case AnimClass::Damage:
        return {false, false, false, true, false, 3.0f, true};
    case AnimClass::Death:
        return {false, false, false, true, false, 30.0f};
    case AnimClass::AttackRight:
        return {false, true, true, true, false, 3.0f};
    case AnimClass::AttackBoth:
        return {false, false, true, true, false, 3.0f};
    case AnimClass::ItemUse:
        return {false, false, false, false, false, 5.0f, true};
    case AnimClass::Event:
        return {false, false, false, true, false, 10.0f};
    case AnimClass::Gesture:
        return {false, false, false, true, false, 30.0f};
    }
    return {};
}

constexpr bool Animates(const AnimPolicy& p) {
    return !p.right_ik || !p.left_ik;
}

// arm_keep_on_hit / arm_keep_on_dodge: the clips whose arms stay on your controllers although
// their class animates them. Hit: the on-feet staggers (a000_020000 .. 020399: the flinch, the
// small, medium and large reactions in every direction); the knockdowns and launches (20400 and
// up, 21000-23999: the hunter on the floor) keep the game's arms. Dodge: the backsteps (7000,
// 7050), the quicksteps (7100-7107, Old Hunter Bone 7150-7157) and the rolls (7200, 7250); the
// backstep and the roll up from lying (7300, 7350, 7400-7457: after a knockdown) keep the game's.
// The game still decides when an attack is taken: AnimGate refuses swings and shots from a hand
// until the clip's cancel window takes that hand's attack (GateOut::attack_ok).
// Bow (vr.ini bow, core/vr/bow.h): Simon's Bowblade's bow-form clips (a149_040000 .. 049999, the
// shots the bow presses): your hands are the bow and its string, the game only fires the arrow.
// Guard (vr.ini shield_raise, core/vr/trick_weapons.h): the guard reactions the shield holds
// (10000 .. 10029, a blow on the hunter's shield): the shield stays where you hold it up. A blow
// that breaks the guard (10030 and up: trick_weapons.h GuardBlowSize 3) is the game's stagger.
// Transform (vr.ini hand_transform): the game's own transform animations (19000 .. 19999: an L1
// transform, Tonitrus' charge, the Chikage's blood, the Bloodletter's wound, the Boom Hammer's
// closing): your hands did it; the clip still runs underneath (its effects, its form switch).
enum class KeepKind : std::uint8_t { None, Hit, Dodge, Bow, Guard, Transform };
constexpr std::int32_t BowbladeGroup = 149; // Simon's Bowblade's TAE group (a149)

constexpr KeepKind KeepKindOf(std::int32_t id) {
    if (id < 0) {
        return KeepKind::None;
    }
    const std::int32_t base = id % 1'000'000;
    if (base >= 20000 && base <= 20399) {
        return KeepKind::Hit;
    }
    if (base == 7000 || base == 7050 || (base >= 7100 && base <= 7107) ||
        (base >= 7150 && base <= 7157) || base == 7200 || base == 7250) {
        return KeepKind::Dodge;
    }
    if (id / 1'000'000 == BowbladeGroup && base >= 40000 && base <= 49999) {
        return KeepKind::Bow;
    }
    if (base >= 10000 && base <= 10029) {
        return KeepKind::Guard;
    }
    if (base >= 19000 && base <= 19999) {
        return KeepKind::Transform;
    }
    return KeepKind::None;
}

inline const char* KeepKindName(KeepKind k) {
    switch (k) {
    case KeepKind::None:
        return "none";
    case KeepKind::Hit:
        return "hit";
    case KeepKind::Dodge:
        return "dodge";
    case KeepKind::Bow:
        return "bow";
    case KeepKind::Guard:
        return "guard";
    case KeepKind::Transform:
        return "transform";
    }
    return "?";
}

// Whether the arms of `id` stay yours with these settings.
constexpr bool KeepsArms(std::int32_t id, bool keep_hit, bool keep_dodge, bool keep_bow = false,
                         bool keep_guard = false, bool keep_transform = false) {
    const KeepKind k = KeepKindOf(id);
    return (k == KeepKind::Hit && keep_hit) || (k == KeepKind::Dodge && keep_dodge) ||
           (k == KeepKind::Bow && keep_bow) || (k == KeepKind::Guard && keep_guard) ||
           (k == KeepKind::Transform && keep_transform);
}

constexpr std::int64_t AnimMillis = 1'000'000; // nanoseconds
// Added to a TAE length before the arms come back.
constexpr std::int64_t AnimExtraNs = 100 * AnimMillis;

// A pickup keeps your arms through the event flag it sets for this long at most.
constexpr float PickupCapS = 5.0f;

// The action request module's accepted requests ([[PlayerIns+0x3B0]+0x80]+0x48): set every frame
// by the playing clip's TAE JumpTable events (0x1A2173A: event 4 -> R1 | R2, 16 -> L2 | 0x400000,
// 22 -> L1, 26 -> the dodges, 31 -> items, 32 -> switches and 0x800000).
constexpr std::uint64_t AllowRightAttack = 0x3; // R1, R2
constexpr std::uint64_t AllowLeftAttack = 0x8;  // L2
// A cancel window counts this long after the clip started at the earliest (the first frame of a
// clip may still carry the last clip's requests); the earliest real ones open at 0.23 s.
constexpr std::int64_t CancelMinNs = 100 * AnimMillis;
// A weapon switch refuses swings and shots until the game takes R1 again, at most this long after
// its last clip (without the cancel signal: WeaponSwitchFallbackNs, the switch's 0.74 s).
constexpr std::int64_t WeaponSwitchCapNs = 2000 * AnimMillis;
constexpr std::int64_t WeaponSwitchFallbackNs = 750 * AnimMillis;

// The length an animation of `id` holds the arms by (s): a transform clip's own length, else its
// TAE length; negative when unknown.
inline float HoldLengthSeconds(std::int32_t id) {
    const float clip = C0000::TransformLengthSeconds(id);
    return clip > 0.0f ? clip : C0000::AnimLengthSeconds(id);
}

// How long an animation of `id` holds the arms: its length + 0.1 s, at most `cap_s` (the cap alone
// when the length is unknown).
inline std::int64_t AnimHoldNs(std::int32_t id, float cap_s) {
    // Whole milliseconds: the lengths have three decimals.
    const auto ns = [](float seconds) { return std::llround(seconds * 1000.0f) * AnimMillis; };
    const std::int64_t cap = ns(cap_s);
    const float length = HoldLengthSeconds(id);
    if (!(length > 0.0f)) {
        return cap;
    }
    return std::min(cap, ns(length) + AnimExtraNs);
}

// What took the arms (highest first).
enum class GateReason : std::uint8_t {
    None,   // both arms yours
    Dead,   // HP <= 0
    Throw,  // a visceral, riposte or backstab, or an enemy grab (ThrowState != 0)
    Ladder, // the HKS ladder state (on a ladder)
    Event,  // the event-animation action flag (doors, levers, lamps, NPC talk, ...)
    Anim,   // the class of the animation that is playing
};

inline const char* GateReasonName(GateReason r) {
    switch (r) {
    case GateReason::None:
        return "none";
    case GateReason::Dead:
        return "dead";
    case GateReason::Throw:
        return "throw";
    case GateReason::Ladder:
        return "ladder";
    case GateReason::Event:
        return "event";
    case GateReason::Anim:
        return "anim";
    }
    return "?";
}

constexpr std::size_t MaxNewIds = 10; // the TimeAct ring's size

// One pose copy's readings. Each signal has its own validity (a failed check turns it off).
struct GateSignals {
    // The ids the TimeAct ring received since the last step, oldest first, and each clip's time
    // before that step (s): 0 on a clip's first step, so a later frame of a clip playing on can be
    // told from a new start of the same id.
    bool ring_valid{false};
    std::array<std::int32_t, MaxNewIds> new_ids{};
    std::array<float, MaxNewIds> new_from{};
    std::uint32_t new_count{0};
    bool event_valid{false};
    bool event{false}; // action flag 0x02
    bool throw_valid{false};
    std::uint8_t throw_state{0};
    bool ladder_valid{false};
    std::int8_t ladder{-1};
    bool hp_valid{false};
    std::int32_t hp{1};
    // The game's cancel windows this frame (the action request module, see AllowRightAttack).
    bool cancel_valid{false};
    std::uint64_t allow{0}; // accepted requests (+0x48)
    bool move_ok{false};    // a move cancels the clip (+0xA0 bit 1)
    bool anim_gate{true};   // vr.ini arm_anim_gate: the animation classes take the arms
    // vr.ini arm_keep_on_hit / arm_keep_on_dodge: those clips leave the arms yours (KeepsArms),
    // and `keep_torso` keeps the torso alignment meanwhile (the shoulders stay under your head).
    bool keep_hit{false};
    bool keep_dodge{false};
    bool keep_bow{false};   // vr.ini bow: the Bowblade's bow clips (KeepKind::Bow)
    bool keep_guard{false}; // vr.ini shield_raise with a shield: its guard reactions
    bool keep_transform{false}; // vr.ini hand_transform: the game's transform animations
    bool keep_torso{true};
    // vr.ini dodge_attack: a kept dodge takes swings and shots at any moment (GateOut::dodge_attack).
    bool dodge_attack{false};
    // A grab's or a visceral's throw state hands the arms back at the game's cancel window, as
    // the cinematic returns to VR (vr.ini cinematic_release, cinematic_exit_ms); off: the whole
    // throw state keeps them (the get-up's 0.7-0.9 s after VR is back).
    bool visceral_recovery{false}; // verified outgoing rip: visual arms only, attacks stay gated
    bool visceral_freehand{false}; // outgoing interactive throw: tracked arms and torso
    bool throw_release{true};
    std::int64_t throw_release_ns{50'000'000};
};

struct GateOut {
    std::array<bool, 2> ik{true, true}; // C0000::Left, C0000::Right: true = your arm
    bool torso{true};                   // the torso alignment stays
    GateReason reason{GateReason::None};
    // The animation holding the arms (held = true), or the last one that did.
    bool held{false};
    AnimClass owner{AnimClass::Unknown};
    std::int32_t owner_id{-1};
    std::int64_t owner_age_ns{0};
    std::int64_t hold_ns{0};
    const char* released{nullptr}; // this step ended the held animation: "next id", "length",
                                   // "move cancel"
    bool changed{false};           // ik, torso or reason differ from the last step
    bool throw_released{false};    // this step the throw state's cancel window gave the arms back
    bool ladder_ignored{false};    // a ladder state left over from a death (the dead latch)
    // The held animation handed this arm back at its attack window (R1 / L2 taken again).
    std::array<bool, 2> cancelled{};
    // Swings (right) and shots (left) may start: false while a weapon switch has not given R1 back
    // or a kept clip (below) has not opened that hand's attack window (the arms stay yours
    // meanwhile).
    std::array<bool, 2> attack_ok{true, true};
    bool switching{false};
    // The held clip is a kept one (arm_keep_on_hit / arm_keep_on_dodge): it plays on the body,
    // the arms stay yours, attack_ok follows its cancel windows.
    bool kept{false};
    // A kept dodge with vr.ini dodge_attack: a physical swing or a gun shot goes now whatever its
    // attack window (the dodge plays on; the attack ends its i-frames, dodge_attack_iframes=0).
    // attack_ok stays the game's window (items, the crank and the transform still wait for it).
    bool dodge_attack{false};
};

// A clip of the ring, so its later frames can be told from a new start: the same id that does not
// start at 0 is the same clip playing on.
struct ClipMark {
    std::int32_t id{-1};
    bool Continues(std::int32_t other, float from) const {
        return other == id && from > 0.0f;
    }
};

class AnimGate {
public:
    GateOut Step(const GateSignals& s, std::int64_t now) {
        GateOut out{};
        if (s.ring_valid) {
            Take(s, now, out);
        }
        LadderDeadLatch(s);
        Cancels(s, now, out);
        if (held && now - start >= hold) {
            Release(out, "length");
        }
        // A freehand throw must not hand an old extraction tail back to the animation when
        // its throw state clears. A newly arrived hit/event/attack keeps its own policy.
        if (s.throw_valid && s.throw_state == 0 && freehand_clip >= 0 && held &&
            owner_id == freehand_clip) {
            Release(out, "freehand throw ended");
        }
        if (s.throw_valid) {
            freehand_clip = s.visceral_freehand && s.throw_state > 0 &&
                                    (s.throw_state & 1) != 0 && owner == AnimClass::AttackRight
                                ? owner_id
                                : -1;
        }
        out.held = held;
        out.owner = owner;
        out.owner_id = owner_id;
        out.owner_age_ns = owner_id >= 0 ? now - clip_start : 0;
        out.hold_ns = hold;

        if (s.anim_gate && held) {
            const AnimPolicy p = PolicyOf(owner);
            out.cancelled = cancelled;
            out.reason = GateReason::Anim;
            if (KeepsArms(owner_id, s.keep_hit, s.keep_dodge, s.keep_bow, s.keep_guard,
                          s.keep_transform)) {
                // Your arms through the clip; the game's attack windows still decide when a
                // swing or a shot is taken (the same moment the game would hand the arm back).
                out.kept = true;
                out.attack_ok = cancelled;
                out.torso = s.keep_torso || p.torso;
                out.dodge_attack = s.dodge_attack && KeepKindOf(owner_id) == KeepKind::Dodge;
            } else {
                out.ik = {p.left_ik || cancelled[0], p.right_ik || cancelled[1]};
                out.torso = p.torso;
            }
        }
        if (switching) {
            out.attack_ok = {false, false};
            out.dodge_attack = false;
            out.switching = true;
        }
        const auto take_both = [&out](GateReason reason) {
            out.ik = {false, false};
            out.torso = false;
            out.reason = reason;
            out.kept = false;
            out.dodge_attack = false;
        };
        // A pickup sets the event flag too, but your hand is already at the item.
        if (s.event_valid && s.event && now >= pickup_until) {
            take_both(GateReason::Event);
        }
        if (s.ladder_valid && s.ladder != -1) {
            if (ladder_dead) {
                out.ladder_ignored = true;
            } else {
                take_both(GateReason::Ladder);
            }
        }
        if (ThrowHolds(s, now, out)) {
            take_both(GateReason::Throw);
            if ((s.visceral_freehand || s.visceral_recovery) && (s.throw_state & 1) != 0) {
                out.ik = {true, true}; // the ordinary arm weights blend back to tracked hands
                out.attack_ok = {false, false}; // the throw still owns combat
                if (s.visceral_freehand) {
                    out.torso = true;
                    out.kept = true;
                }
            }
        } else if (out.throw_released && out.reason == GateReason::Anim) {
            out.ik = {true, true}; // the held clip was released with it (this step's policy)
            out.cancelled = {};
            out.torso = true;
            out.reason = GateReason::None;
            out.held = false;
            out.kept = false;
            out.dodge_attack = false;
            out.attack_ok = {!switching, !switching};
        }
        if (s.hp_valid && s.hp <= 0) {
            take_both(GateReason::Dead);
        }
        out.changed = !have_last || out.ik != last_ik || out.torso != last_torso ||
                      out.reason != last_reason || out.kept != last_kept ||
                      out.attack_ok != last_attack_ok || out.dodge_attack != last_dodge_attack;
        have_last = true;
        last_ik = out.ik;
        last_torso = out.torso;
        last_reason = out.reason;
        last_kept = out.kept;
        last_attack_ok = out.attack_ok;
        last_dodge_attack = out.dodge_attack;
        return out;
    }

    void Reset() {
        *this = AnimGate{};
    }

private:
    // A grab or a visceral (the throw state) takes both arms until the game hands control back:
    // its cancel window (R1 / R2 or a move accepted, the cinematic's control-back rule) seen
    // closed first in this throw, then open for throw_release_ns. The rest of that throw state
    // (the get-up plays on 0.7-0.9 s) then leaves the arms yours and its held clip is released
    // for good; a fatal throw stays the game's (HP 0: Dead). False while the signal is missing.
    bool ThrowHolds(const GateSignals& s, std::int64_t now, GateOut& out) {
        if (!s.throw_valid) {
            return false;
        }
        if (s.throw_state == 0) {
            throw_armed = false;
            throw_open_since = -1;
            throw_released = false;
            return false;
        }
        if (throw_released) {
            return false;
        }
        if (!s.throw_release) {
            return true;
        }
        const bool alive = !s.hp_valid || s.hp > 0;
        const bool window = s.cancel_valid && ((s.allow & AllowRightAttack) != 0 || s.move_ok);
        if (!alive || !window) {
            throw_armed = alive && s.cancel_valid; // seen closed: the next opening counts
            throw_open_since = -1;
            return true;
        }
        if (!throw_armed) {
            return true; // still the last clip's requests
        }
        if (throw_open_since < 0) {
            throw_open_since = now;
        }
        if (now - throw_open_since < s.throw_release_ns) {
            return true;
        }
        throw_released = true;
        out.throw_released = true;
        if (held) {
            Release(out, "grab cancel");
        }
        return false;
    }

    // The held clip hands the arms back; its later frames (it may play on) never take them again.
    void Release(GateOut& out, const char* why) {
        held = false;
        out.released = why;
        released_clip = {owner_id};
    }

    // The game's cancel windows: a held clip whose class allows it hands each arm back once the
    // game takes that hand's attack, and ends once a move would cancel it; a weapon switch ends
    // its swing lock once R1 is taken again.
    void Cancels(const GateSignals& s, std::int64_t now, GateOut& out) {
        const bool window = s.cancel_valid && now - clip_start >= CancelMinNs;
        if (held && window && PolicyOf(owner).cancel_release) {
            cancelled[1] = cancelled[1] || (s.allow & AllowRightAttack) != 0;
            cancelled[0] = cancelled[0] || (s.allow & AllowLeftAttack) != 0;
            if (s.move_ok) {
                Release(out, "move cancel");
            }
        }
        if (switching) {
            const std::int64_t age = now - switch_start;
            const bool free = s.cancel_valid
                                  ? (age >= CancelMinNs && (s.allow & AllowRightAttack) != 0) ||
                                        age >= WeaponSwitchCapNs
                                  : age >= WeaponSwitchFallbackNs;
            if (free) {
                switching = false;
            }
        }
    }

    // HP <= 0 or a death clip latches; a new ladder state value or a climbing clip while alive
    // releases (the first reading is the baseline, not a change).
    void LadderDeadLatch(const GateSignals& s) {
        bool died = s.hp_valid && s.hp <= 0;
        bool climb = false;
        if (s.ring_valid) {
            const std::uint32_t count = std::min<std::uint32_t>(s.new_count, MaxNewIds);
            for (std::uint32_t i = 0; i < count; ++i) {
                died = died || ClassifyAnim(s.new_ids[i]) == AnimClass::Death;
                climb = climb || IsClimbClip(s.new_ids[i]);
            }
        }
        const bool changed = s.ladder_valid && have_ladder && s.ladder != last_ladder;
        if (died) {
            ladder_dead = true;
        } else if (ladder_dead && (changed || climb)) {
            ladder_dead = false;
        }
        if (s.ladder_valid) {
            have_ladder = true;
            last_ladder = s.ladder;
        }
    }

    // The new ids: the strongest animated one takes the arms (both arms over one, a full-body
    // move over one that keeps the torso; the newest on a tie); otherwise an id of a releasing
    // class ends a held move that allows it.
    void Take(const GateSignals& s, std::int64_t now, GateOut& out) {
        const std::uint32_t count = std::min<std::uint32_t>(s.new_count, MaxNewIds);
        int best = -1;
        int best_strength = 0;
        bool release = false;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::int32_t id = s.new_ids[i];
            const AnimClass c = ClassifyAnim(id);
            const AnimPolicy p = PolicyOf(c);
            if (c == AnimClass::Pickup) {
                pickup_until = now + AnimHoldNs(id, PickupCapS);
            }
            if (Animates(p) && released_clip.Continues(id, s.new_from[i])) {
                // Handed back already, playing on.
            } else if (Animates(p)) {
                const int strength = (p.right_ik ? 0 : 2) + (p.left_ik ? 0 : 2) + (p.torso ? 0 : 1);
                if (strength >= best_strength) {
                    best = static_cast<int>(i);
                    best_strength = strength;
                }
            } else if (p.releases) {
                release = true;
            }
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::int32_t id = s.new_ids[i];
            if (ClassifyAnim(id) != AnimClass::WeaponSwitch ||
                switch_clip.Continues(id, s.new_from[i])) {
                continue; // the same switch clip playing on (its lock running or over)
            }
            switching = true;
            switch_clip = {id};
            switch_start = now;
        }
        if (best >= 0) {
            const std::int32_t id = s.new_ids[static_cast<std::size_t>(best)];
            if (!held || id != owner_id || !(s.new_from[static_cast<std::size_t>(best)] > 0.0f)) {
                // A new clip: its cancel windows count from here. The one playing on (seen every
                // frame) only keeps its hold.
                clip_start = now;
                cancelled = {false, false};
                // A move that takes the arms interrupts a switch (and its lock) as in the game.
                switching = false;
            }
            held = true;
            owner_id = id;
            owner = ClassifyAnim(owner_id);
            start = now;
            hold = AnimHoldNs(owner_id, PolicyOf(owner).cap_s);
        } else if (release && held && PolicyOf(owner).release_on_next) {
            Release(out, "next id");
        }
    }

    bool held{false};
    AnimClass owner{AnimClass::Unknown};
    std::int32_t owner_id{-1};
    std::int64_t start{0};      // the held clip was last seen (its hold counts from here)
    std::int64_t clip_start{0}; // the held clip started
    std::int64_t hold{0};
    std::array<bool, 2> cancelled{}; // C0000::Left, C0000::Right: handed back at the attack window
    ClipMark released_clip{};        // the last clip handed back (it may play on)
    bool switching{false};           // a weapon switch's swing lock
    ClipMark switch_clip{};          // the last switch clip (its lock may be over)
    std::int64_t switch_start{0};
    std::int64_t pickup_until{0};
    bool have_last{false};
    std::array<bool, 2> last_ik{true, true};
    bool last_torso{true};
    GateReason last_reason{GateReason::None};
    bool last_kept{false};
    std::array<bool, 2> last_attack_ok{true, true};
    bool last_dodge_attack{false};
    bool ladder_dead{false};
    bool have_ladder{false};
    std::int8_t last_ladder{-1};
    std::int32_t freehand_clip{-1}; // visual owner of the outgoing freehand throw
    bool throw_armed{false};          // the throw state's cancel window was seen closed
    std::int64_t throw_open_since{-1}; // ... and has been open since
    bool throw_released{false};       // ... long enough: the rest of this throw state is yours
};

} // namespace VR::Body
