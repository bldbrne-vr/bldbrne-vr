// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

// Instant item pickups (vr.ini pickup_instant, core/vr/bloodborne_pickup.h): pure decisions and log
// text, unit-tested in tests/test_pickup_gate.cpp.
//
// The game grants an item the moment Cross is accepted on its prompt (ItemDropMan::PickUp: the
// glow is removed, its event flag set, the lot added or sent to storage, the "item get" pop-up and
// its sound queued). The crouch is a separate step after it: an event (4010) whose handlers play
// the pickup animation. An instant pickup is the same pickup with that event left out.
namespace VR::Pickup {

// The item checker's three blocks (the ActionButton categories they register).
enum class Kind : std::uint8_t {
    Drop = 0,     // uid 9: what a killed enemy leaves on the ground (ItemDropMan list 2)
    World = 1,    // uid 8: a world item (list 1)
    Treasure = 2, // uid 10: a placed treasure, e.g. on a body (list 3; its own animation id)
};

inline const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::Drop:
        return "enemy drop";
    case Kind::World:
        return "world item";
    case Kind::Treasure:
        return "treasure";
    }
    return "?";
}

// Why a pickup plays the game's animation (Instant = it does not).
enum class Reason : std::uint8_t {
    Instant = 0,
    SettingOff,   // pickup_instant=0
    VrOff,        // VR not running
    ScreenChosen, // the virtual screen chosen by hand (Tab)
    NotImmersive, // first-person VR not shown (loading, the virtual screen, a menu camera)
    ThirdPerson,  // first_person=0: the animation is part of the view
    Flat,         // a cinematic flat period is showing the game's own camera
};

struct GateInputs {
    bool setting{true};      // pickup_instant (or VR_PICKUP_INSTANT)
    bool vr_enabled{true};   // VR::IsEnabled()
    bool display_auto{true}; // the display mode is Auto (not the screen chosen by hand)
    bool immersive{true};    // first-person VR frames are being shown
    bool first_person{true}; // first_person != 0
    bool flat{false};        // a cinematic flat period (the game's camera on the screen)
};

constexpr Reason Decide(const GateInputs& in) {
    if (!in.setting) {
        return Reason::SettingOff;
    }
    if (!in.vr_enabled) {
        return Reason::VrOff;
    }
    if (!in.display_auto) {
        return Reason::ScreenChosen;
    }
    if (!in.first_person) {
        return Reason::ThirdPerson;
    }
    if (in.flat) {
        return Reason::Flat;
    }
    if (!in.immersive) {
        return Reason::NotImmersive;
    }
    return Reason::Instant;
}

inline const char* ReasonText(Reason reason) {
    switch (reason) {
    case Reason::Instant:
        return "instant: no pickup animation";
    case Reason::SettingOff:
        return "the game's pickup animation (pickup_instant=0)";
    case Reason::VrOff:
        return "the game's pickup animation (VR off)";
    case Reason::ScreenChosen:
        return "the game's pickup animation (the virtual screen chosen by hand)";
    case Reason::NotImmersive:
        return "the game's pickup animation (first-person VR not shown)";
    case Reason::ThirdPerson:
        return "the game's pickup animation (third person)";
    case Reason::Flat:
        return "the game's pickup animation (a flat period is showing)";
    }
    return "?";
}

// Item ids carry their category in the top 4 bits (0 weapon, 1 armour, 2 rune/accessory,
// 4 goods). 0xFFFFFFFF / a 0x0FFFFFFF id = no item.
constexpr bool IsNoItem(std::uint32_t item) {
    return (item & 0x0FFFFFFFu) == 0x0FFFFFFFu;
}

inline const char* GoodsName(std::int32_t id) {
    switch (id) {
    case 900:
        return "Quicksilver Bullets";
    case 1000:
        return "Blood Vial";
    case 1100:
        return "Antidote";
    case 1101:
        return "Sedative";
    case 1110:
        return "Beast Blood Pellet";
    case 1120:
        return "Blue Elixir";
    case 1200:
        return "Molotov Cocktail";
    case 1210:
        return "Poison Knife";
    case 1220:
        return "Oil Urn";
    case 1230:
        return "Pebble";
    case 1240:
        return "Throwing Knife";
    case 1250:
        return "Rope Molotov Cocktail";
    case 1260:
        return "Pungent Blood Cocktail";
    case 1270:
        return "Numbing Mist";
    case 1300:
        return "Fire Paper";
    case 1320:
        return "Bolt Paper";
    case 1330:
        return "Bone Marrow Ash";
    case 1400:
        return "Bold Hunter's Mark";
    case 1410:
        return "Shining Coins";
    case 1500:
        return "Madman's Knowledge";
    case 1510:
        return "Coldblood Dew (1)";
    case 2030:
        return "Lead Elixir";
    default:
        return nullptr;
    }
}

inline std::string ItemText(std::uint32_t item) {
    if (IsNoItem(item)) {
        return "none";
    }
    const std::int32_t id = static_cast<std::int32_t>(item & 0x0FFFFFFFu);
    const std::string number = std::to_string(id);
    switch (item >> 28) {
    case 0:
        return "weapon " + number;
    case 1:
        return "armour " + number;
    case 2:
        return "rune " + number;
    case 4: {
        const char* const name = GoodsName(id);
        return name != nullptr ? std::string{name} + " (" + number + ")" : "goods " + number;
    }
    default: {
        char text[24];
        std::snprintf(text, sizeof(text), "item %#x", static_cast<unsigned>(item));
        return text;
    }
    }
}

} // namespace VR::Pickup
