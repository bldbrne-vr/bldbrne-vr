// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>

#include "core/vr/pickup_gate.h"

// Chests, coffins, doors and gates opened in first-person VR without the hunter's animation (vr.ini
// vr_interactions, default 1, the one setting for chests, doors and lamps; the tester, 2026-11-06:
// "open without playing the hunter's animation and locking us into place (but having the chest
// open in VR)", then "make the doors and gates work as well"). Pure decisions, unit-tested in
// tests/test_chest_open.cpp; the game side is core/vr/bloodborne_levers.cpp (Chest::RewriteEvent).
//
// A chest is an object action (ObjActParam), as doors and levers are. When the game accepts its
// prompt it builds one event record and hands it to its event receiver (0x13AE600): the hunter's
// event animation, the object's own clip (the lid, the door) and how the hunter is placed (0:
// walked onto the object's dummy first). The hunter's clips carry no object or item event (only his
// steps, cloth, the weapon hidden and his cancel windows), so the record is changed as the physical
// levers change theirs: no hunter animation and no placement, the object's own clip kept, so the
// lid or the door opens in front of you while you keep moving; the item inside a chest is then an
// ordinary treasure (instant pickup). Only the main player's own record (not one received from
// another player) is changed. A locked door, or one that does not open from this side, never gets
// that far: the game answers its prompt with its message and builds no record.
//
// Which animation is which, from the mock (Pthumeru layer 1, 2026-11-06): a000_101142 is a chest
// (ObjActParam 9941: the lid open about a second after B, goods inside), 101144 the gilded Chalice
// coffin (2923000: its lid slides away); 101141 / 101143 (9942 / 9943, and 2000 / 2100 / 2200 in
// the main game) are the same rows' other chests (their clips match 101142's; not opened in the
// mock). From the game's ObjActParam rows and map layouts: 101100..101121 are doors and gates (101100
// a single door, 101111 the clinic's double doors, 101110 / 101112 the heavy iron gates, 101120 a
// Chalice stone door, 101121 a Chalice iron gate); every one's ObjActParam row runs its event at the
// action's start (eventKickTiming 1), and the scripts that wait on them move only the doors, never
// the hunter. 101116 is not one of them: the Nightmare Lecture Building's two great doors, whose
// script waits 6 s and then sends the hunter to another map while his clip fades the screen; without
// the clip he would walk free for those 6 s and be sent away without the fade, so they keep the
// game's way. Doors opened by a script of their own (the door after Father Gascoigne: action button
// 7030 and a cutscene) are no object action and never reach the receiver.
namespace VR::ChestOpen {

enum class Kind : std::uint8_t { None, Chest, Coffin, Door };

// By the hunter's event animation in the game's event record (its ObjActParam row's +0x0C).
constexpr Kind KindOf(std::int32_t hunter_anim) {
    if (hunter_anim >= 101141 && hunter_anim <= 101143) {
        return Kind::Chest;
    }
    if (hunter_anim == 101144) {
        return Kind::Coffin;
    }
    if (hunter_anim >= 101100 && hunter_anim <= 101121 && hunter_anim != 101116) {
        return Kind::Door;
    }
    return Kind::None;
}

constexpr const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::Chest:
        return "chest";
    case Kind::Coffin:
        return "coffin";
    case Kind::Door:
        return "door";
    case Kind::None:
        break;
    }
    return "object";
}

// The event record's fields (s32).
constexpr int EvIndex = 1;      // the action's index in its block's rows
constexpr int EvActor = 3;      // the actor's handle
constexpr int EvHunterAnim = 4; // the hunter's event animation (-1: none)
constexpr int EvObjectClip = 6; // the object's own clip (kept)
constexpr int EvPlacement = 7;  // 0: the hunter walked onto the object's dummy; 2: where he stands
constexpr std::int32_t NoAnimation = -1;
constexpr std::int32_t InPlace = 2;

using Record = std::array<std::int32_t, 8>;

// The hunter neither animates nor is placed; the object's clip plays as the game chose it.
constexpr Record Rewrite(Record event) {
    event[EvHunterAnim] = NoAnimation;
    event[EvPlacement] = InPlace;
    return event;
}

// A door's own clip is timed to the hunter's push it was made for: it stands still (or barely
// creeps) until his hands would reach it, about 0.2 s for a house door and 0.7 to 1 s for the
// clinic's double doors, the iron gates and the great doors. Without the push that wait is all you
// see after the press (2026-11-10: "a 0.5s delay before the door starts opening"). So
// once its first frame has played (any sound at 0 s with it), the clip goes on from just before
// the earlier of the door's first 2 degrees (or 2 cm) and its clip's next sound event: nothing you
// would see moving and no sound is skipped. Measured per ObjActParam row from the user's dump: the
// row's object clip (a000_<its +0x1C>) and the next one (the other side's, or the door closing
// again) of every model placed with it, their first frames sampled and their TAE's events read;
// the smallest over them, seconds, and only for those two clips (a door's other clips keep theirs).
// Rows where that gains under 0.1 s, rows not measured (the Chalice stone doors and iron gates,
// whose placements are not in the map files) and the generic rows (1, 10, 30, 31: any map may
// place them, with any model) wait as the game made them.
struct Lead {
    std::int32_t row{};
    std::int32_t clip{}; // the row's object clip (its +0x1C); clip + 1 is measured with it
    float start{};       // seconds into the door's clip
};
inline constexpr std::array<Lead, 35> Leads{{
    {2200010, 1, 0.50f}, // Hemwick's great doors (o220000)
    {2200030, 0, 0.30f}, // Hemwick's door after the boss (o220200)
    {2300000, 1, 0.30f}, // Old Yharnam's one-way gates (o232110, o232100)
    {2300001, 1, 0.37f},
    {2300010, 1, 0.63f}, // Old Yharnam's great doors (o232000, o232200)
    {2300011, 1, 0.73f},
    {2400090, 1, 0.80f}, // Cathedral Ward's double doors (o240040)
    {2400091, 1, 0.80f},
    {2400100, 1, 0.80f},
    {2400110, 1, 0.30f}, // the Cathedral Ward door to Old Yharnam (o243012)
    {2410010, 1, 0.73f}, // the clinic's double doors (o240100, o240104)
    {2410030, 1, 0.70f}, // the clinic garden's iron gate (o240502)
    {2410050, 1, 0.17f}, // Central Yharnam's house doors (o241013)
    {2410070, 1, 0.17f},
    {2410080, 1, 0.70f}, // the iron gate to Gascoigne (o240501)
    {2410090, 1, 0.70f}, // the iron gate by the ladder shortcut (o240500)
    {2420010, 1, 0.17f}, // the orphanage's doors (o241013) and gates (o241011, o241012)
    {2420020, 1, 0.77f},
    {2420030, 1, 0.77f},
    {2420040, 1, 0.17f},
    {2600010, 1, 0.63f}, // Mensis: the great doors (o260052) and the bridge gate (o260050)
    {2600020, 0, 1.03f},
    {2600021, 0, 1.03f},
    {2700010, 1, 0.30f}, // the Forbidden Woods' one-way doors (o270004, o270103)
    {2700030, 1, 0.30f},
    {2800020, 1, 0.83f}, // Yahar'gul's double doors (o282050)
    {2901900, 0, 0.57f}, // the Chalice great doors (o291900, o291910; by their names)
    {2901910, 0, 0.57f},
    {2901920, 0, 0.47f}, // the Chalice boss doors, Beckoner only (o291920)
    {3200040, 1, 0.13f}, // the Lecture Building's doors (o321100)
    {3400010, 1, 0.80f}, // the Hunter's Nightmare's double doors (o342000)
    {3500030, 1, 0.80f}, // the Research Hall's great door (o351010)
    {3500040, 1, 0.17f}, // the Research Hall's one-way doors (o351001) and door (o351003)
    {3500041, 1, 0.17f},
    {3500070, 1, 0.17f},
}};

// Where a door of ObjActParam `row` playing object clip `clip` (its event's) goes on from
// (seconds; 0: it waits as the game made it).
constexpr float LeadStart(std::int32_t row, std::int32_t clip) {
    for (const auto& lead : Leads) {
        if (lead.row == row) {
            return clip == lead.clip || clip == lead.clip + 1 ? lead.start : 0.0f;
        }
    }
    return 0.0f;
}

// Its clip's time `time` (seconds) is moved on to `start` once, when its first frame has played
// (past 0: the sounds at 0 s fired) and it has not got there by itself.
constexpr bool MoveOn(float time, float start) {
    return start > 0.0f && time > 0.0f && time < start;
}

// When it opens without him: the same moments as an instant pickup (core/vr/pickup_gate.h:
// first-person VR shown, Auto display, first person, no flat period), with vr_interactions.
using Pickup::Reason;
constexpr Reason Decide(bool setting, Pickup::GateInputs in) {
    in.setting = setting;
    return Pickup::Decide(in);
}

inline const char* WhyAnimated(Reason reason) {
    switch (reason) {
    case Reason::Instant:
        return "opens in VR";
    case Reason::SettingOff:
        return "the game's animation (vr_interactions=0)";
    case Reason::VrOff:
        return "the game's animation (VR off)";
    case Reason::ScreenChosen:
        return "the game's animation (the virtual screen chosen by hand)";
    case Reason::NotImmersive:
        return "the game's animation (first-person VR not shown)";
    case Reason::ThirdPerson:
        return "the game's animation (third person)";
    case Reason::Flat:
        return "the game's animation (a flat period is showing)";
    }
    return "?";
}

} // namespace VR::ChestOpen
