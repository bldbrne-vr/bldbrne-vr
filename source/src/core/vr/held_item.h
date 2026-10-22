// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <cstdint>

#include "core/vr/anim_classes.h"

// Held items in the hunter's own hand (vr.ini vial_in_hand=game): the policy behind
// core/vr/bloodborne_held.cpp, pure so tests/test_held_item.cpp can drive it.
//
// While a Blood Vial is armed (X), the presenter publishes a held request every input frame; the
// main player's update asks the game for its own Blood Bullet vial (FFX 173, model s10002) on the
// left palm (dummy 3) and hides the left weapon like the vanilla clip 100141 does (TAE event 322),
// both rewritten every update. Decide() says whether this update shows it: only for a fresh
// request, and never while the game has the left hand busy with something that would clash
// (its own item clips, events, ladders, throws, two-handed attacks, the gun) or the view is not
// first-person gameplay (a cinematic, a menu). Not shown = not refreshed: the game ends the effect
// within one update and the gun comes back at the next weapon update.
namespace VR::Held {

enum class Kind : std::uint8_t {
    None,
    Vial, // the armed Blood Vial (left hand)
    Item, // a quick item taken from the arc (core/vr/quick_items.h): its own held effect, or none
          // (our in-frame mesh); the same hide and suppress rules
};

// Request flags (presenter -> game).
constexpr std::uint8_t FlagGun = 0x01; // the left trigger is pulled (or was just released): the
                                       // gun is wanted, the vial steps aside
// An injection's drain (core/vr/vial_drain.h, vial_inject_drain=1): the vial in the hand is ours
// (the in-frame renderer's model s10002 with its level): the game's effect is not refreshed, the
// left weapon stays hidden and the vial grip closed.
constexpr std::uint8_t FlagDrain = 0x02;

enum class State : std::uint8_t {
    Off,         // no vial requested
    Pending,     // requested; the game's effect is not confirmed yet
    Shown,       // the game's vial is in the hunter's hand
    Suppressed,  // requested, but not now (Decide: an item clip, a ladder, a menu, the gun, ...)
    Failed,      // the game path failed for this session: the overlay vial is the fallback
    Unavailable, // not installed (a signature mismatch): the overlay vial is the fallback
};

constexpr const char* StateName(State s) {
    switch (s) {
    case State::Off:
        return "off";
    case State::Pending:
        return "pending";
    case State::Shown:
        return "shown";
    case State::Suppressed:
        return "suppressed";
    case State::Failed:
        return "failed";
    case State::Unavailable:
        return "unavailable";
    }
    return "?";
}

// The request word (one atomic u64): kind (8 bits) | flags (8 bits) | steady-clock ms (48 bits).
struct Request {
    Kind kind{Kind::None};
    std::uint8_t flags{0};
    std::int64_t ms{0};
};

constexpr std::uint64_t MsMask = (std::uint64_t{1} << 48) - 1;

constexpr std::uint64_t PackRequest(const Request& r) {
    return (static_cast<std::uint64_t>(r.kind) << 56) |
           (static_cast<std::uint64_t>(r.flags) << 48) |
           (static_cast<std::uint64_t>(r.ms) & MsMask);
}

constexpr Request UnpackRequest(std::uint64_t word) {
    return {static_cast<Kind>(word >> 56), static_cast<std::uint8_t>((word >> 48) & 0xFF),
            static_cast<std::int64_t>(word & MsMask)};
}

// The request's age at `now_ms` (the same clock, 48-bit wrap safe; a request stamped just after
// `now_ms` was read is slightly negative).
constexpr std::int64_t RequestAgeMs(const Request& r, std::int64_t now_ms) {
    const std::uint64_t diff =
        (static_cast<std::uint64_t>(now_ms) - static_cast<std::uint64_t>(r.ms)) & MsMask;
    return diff >= (MsMask >> 1)
               ? static_cast<std::int64_t>(diff) - static_cast<std::int64_t>(MsMask + 1)
               : static_cast<std::int64_t>(diff);
}

// One main-player update's view of the game.
struct Inputs {
    Request request{};
    std::int64_t age_ms{0}; // of the request
    bool cinematic{false};  // VR::CinematicActive(): a scripted animation (fade or flat)
    bool menu{false};       // an in-game menu is open
    // The arm gate of this update (core/vr/anim_classes.h); without it (no body hook) only the
    // hard signals below decide.
    bool gate_valid{false};
    Body::GateReason reason{Body::GateReason::None};
    Body::AnimClass owner{Body::AnimClass::Unknown};
    bool held{false}; // `owner` is playing now
    std::int32_t hp{1};
    std::int32_t item_use_state{0}; // PlayerIns +0x490: a quick item is being used
};

struct Policy {
    std::int64_t max_age_ms{250}; // an older request: the presenter stopped (no vial)
    std::uint8_t hide_left{1};    // vr.ini vial_hide_left: 0 none, 1 left part 0 (vanilla), 2 all
    bool hide_two_handed{true};   // vr.ini vial_hide_two_handed: no vial in two-handed attacks
};

struct Decision {
    bool requested{false};           // a vial is wanted (not shown = suppressed)
    bool show{false};                // request the game's vial this update
    std::uint8_t hide_left_mask{0};  // bit i: hide the left weapon's part i this update
    bool ours{false}; // shown, as our drained vial (FlagDrain): no game effect this update
    const char* reason{"not armed"}; // why it is not shown ("" when shown)
};

// Left weapon part bytes to hide (ChrAsm form override bytes 0-3).
constexpr std::uint8_t HideMask(std::uint8_t hide_left) {
    return hide_left == 0 ? 0 : hide_left == 1 ? 0x01 : 0x0F;
}

// Classes whose animation needs the left hand for something else, or shows its own item.
constexpr const char* ClashOf(Body::AnimClass c, bool hide_two_handed) {
    switch (c) {
    case Body::AnimClass::ItemUse:
        return "item clip"; // vanilla 100080 / 100141 hold their own vial
    case Body::AnimClass::Event:
        return "event clip";
    case Body::AnimClass::Gesture:
        return "gesture";
    case Body::AnimClass::Grabbed:
        return "grabbed";
    case Body::AnimClass::Death:
        return "death clip";
    case Body::AnimClass::AttackLeft:
        return "gun attack"; // the gun is in that hand
    case Body::AnimClass::AttackBoth:
        return hide_two_handed ? "two-handed attack" : nullptr;
    default:
        return nullptr; // idle, locomotion, dodges, falls, guard and damage reactions, your
                        // right-hand attacks, pickups, the game's transform, unverified and
                        // unknown ids: it follows the hand
    }
}

constexpr Decision Decide(const Inputs& in, const Policy& p) {
    Decision d{};
    if (in.request.kind != Kind::Vial && in.request.kind != Kind::Item) {
        d.reason = "not armed";
        return d;
    }
    if (in.age_ms > p.max_age_ms || in.age_ms < -p.max_age_ms) {
        d.reason = "request stale";
        return d;
    }
    d.requested = true;
    const auto suppress = [&d](const char* why) {
        d.reason = why;
        return d;
    };
    if (in.hp <= 0) {
        return suppress("dead");
    }
    if (in.cinematic) {
        return suppress("cinematic");
    }
    if (in.menu) {
        return suppress("menu");
    }
    if ((in.request.flags & FlagGun) != 0) {
        return suppress("gun");
    }
    if (in.item_use_state != 0) {
        return suppress("item use");
    }
    if (in.gate_valid) {
        switch (in.reason) {
        case Body::GateReason::Dead:
            return suppress("dead");
        case Body::GateReason::Throw:
            return suppress("throw");
        case Body::GateReason::Ladder:
            return suppress("ladder");
        case Body::GateReason::Event:
            return suppress("event");
        default:
            break;
        }
        if (in.held) {
            if (const char* clash = ClashOf(in.owner, p.hide_two_handed); clash != nullptr) {
                return suppress(clash);
            }
        }
    }
    d.show = true;
    d.hide_left_mask = HideMask(p.hide_left);
    d.ours = in.request.kind == Kind::Vial && (in.request.flags & FlagDrain) != 0;
    d.reason = "";
    return d;
}

// Private effect slots (TAE events use 0-12, 21-23 and 10001; the node constructor's 0xBAD = 2989
// is avoided): the vial rotates through 30000-30007 when the game stopped its effect but kept the
// node (a stop-all), so a fresh node spawns it again.
constexpr std::int32_t FirstVialSlot = 30000;
constexpr std::int32_t VialSlotCount = 8;

constexpr std::int32_t NextVialSlot(std::int32_t slot) {
    const std::int32_t index = slot - FirstVialSlot;
    return FirstVialSlot + ((index >= 0 && index < VialSlotCount ? index + 1 : 0) % VialSlotCount);
}

} // namespace VR::Held
