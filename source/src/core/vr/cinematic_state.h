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
#include <cstddef>

#include "common/types.h"

// Cinematic flat mode: while the hunter plays a scripted full-body animation (a visceral or a
// backstab, being grabbed, opening a door, pulling a lever, lighting a lamp, the Shaman Bone
// Blade's stab used from the quick-item arc, ...) the game's own third-person camera is shown on
// the virtual screen, and first-person VR resumes when it ends.
//
// This is the decision logic only. It is pure (no emulator state): the game integration
// (core/vr/bloodborne_cinematic.cpp) reads the signals once per main-player update and steps the
// Machine with steady-clock times, so unit tests can drive it (tests/test_cinematic.cpp).
//
//   Off       --signal, VR shown-->             FadeOut (Flat when the fade is 0)
//             --signal within ArrivalHoldNs of a
//               load (a new main player), the
//               screen still up-->              Flat (no fade: the loading page's screen goes
//                                               straight on to the arrival animation)
//   FadeOut   --fade done-->                    Flat      VR still driven, dims to black
//             --escape hatch, ladder cut,
//               death (no death hold)-->        Returning (aborted: it brightens from there)
//   Flat      --control back (the game's cancel
//               window open exit_ns), or the
//               signals off exit_ns; >= min_ns,
//               no death hold; a ladder exit or
//               a death without the death hold
//               (at once); escape hatch-->      Returning (return_fade_ns >= 0: the screen cuts
//                                               to black, VR fades in; re-sync the heading), or
//                                               FadeBack (the old way, and ladders)
//   FadeBack  --signal--> Flat; --fade done-->  Returning the screen dims; re-sync the heading
//   Returning --signal--> FadeOut (Flat when VR is not shown: the screen is still up);
//             --VR shown for the fade (at most
//               the fade + 0.5 s)-->            Off       VR driven again, fades in
//   any       --no update for 5 s (loading), a new main player, VR off, or the screen chosen
//               by hand--> Off (no fade)
// A shorter gap in the updates (a shader-compile hitch) changes nothing: the phase carries on
// with the time that passed, so a flat period survives it with its death hold.
// Control back: a grab, a visceral or an event animation plays on for a while after the game
// hands the hunter control back (its TAE opens the attack and move requests: the get-up of a
// grab, 0.4-0.7 s before the throw state clears). The flat period ends there; the rest of that
// animation's signals are ignored until they end (no second flat period from its tail). Never
// on a ladder; with HP at 0 the death decides (below).
// Death (2026-10-07: the tester saw YOU DIED on the flat screen after a fatal grab): without the
// death hold (Config::death_hold, the default) HP reaching 0 ends the flat period at once, the way
// the cancel window does: every animation signal playing then is ignored until it ends (the
// beast's kill plays on for ~7 s with the throw state set), none starts a flat period while HP is
// 0, and the death screen comes up in first-person VR (the world-locked YOU DIED). With the hold,
// a fatal grab stays flat until the respawn, as before.
// Ladders (LadderGate below) are a category with their own entry and exit rules and fade.
namespace VR::Cinematic {

enum class Phase : u8 {
    Off,       // first-person VR
    FadeOut,   // VR still driven, the projection dims to black
    Flat,      // the game's camera on the virtual screen
    FadeBack,  // still flat, the screen dims to black
    Returning, // VR driven again, the projection fades in
};

// What started the flat period; each has a vr.ini toggle.
enum class Category : u8 {
    None,
    Visceral, // the hunter throws: visceral, riposte, backstab (ThrowState 1/3/5)
    Grabbed,  // the hunter is thrown: enemy grabs (ThrowState 2/4/6)
    Objects,  // doors, gates, levers, elevators, "Operate Device", chests, coffins
    Lamps,    // lighting / using a lamp
    Pickup,   // picking up an item
    Scripted, // NPC talk, scripted event animations, revival, unknown event animations
    Ladder,   // climbing a long ladder (LadderGate)
    Item,     // a canned item use through our own Square: the Shaman Bone Blade's stab (ItemUseGate)
    LevelUp,  // the Doll's Level Up: the hunter kneels before her while the menu is up
    FogGate,  // walking through a fog gate (a000_101130; the tester, 2026-10-18: in VR)
};
constexpr size_t CategoryCount = 11;

constexpr s64 Millis = 1'000'000; // nanoseconds
// Returning lasts the fade plus this, so the projection can fade in before Off.
constexpr s64 ReturnHoldNs = 500 * Millis;
// No main-player update for this long (loading): Off at once. Shorter gaps are hitches.
constexpr s64 LongGapNs = 5000 * Millis;
// Black is never held longer than this while waiting for the shown layer to switch.
constexpr s64 BlackLimitNs = 1000 * Millis;
// As a load ends the hunter's arrival plays: the get-up at the lamp after a warp or a death,
// 101163 / 101200, or in the Hunter's Dream after a death its scene (remo s21_00_0000, 0.3 - 0.5 s
// after the loading page; 2026-10-22: the loading page, then VR for a moment, then the flat
// arrival or scene, then VR again). For this long after the load (the very first update, a new
// main player, and again when the loading page ends: Signals::load_end) the camera is not driven
// yet (ArrivalHoldUntil), so the screen stays up and a signal starts the flat period straight from
// it: one switch, the screen to VR, at the arrival's end. Only a load: a long cutscene (a boss's
// entrance) stops the player's updates too, and the fight after it starts in VR.
constexpr s64 ArrivalHoldNs = 1500 * Millis;
// The cancel-window requests that hand control back (the action request module's mask, see
// bloodborne_layout.h): R1 / R2. A move cancelling the clip counts too (Signals::control).
constexpr u64 ControlAllowMask = 0x3;
// A throw counts after this many consecutive updates with a known state and no alignment
// (a throw whose alignment fails is zeroed without an animation, so it never counts).
constexpr u32 ThrowEnterUpdates = 2;
// An unknown ThrowState value (above 6) counts after this many consecutive updates.
constexpr u32 ThrowFallbackUpdates = 3;

inline const char* PhaseName(Phase phase) {
    switch (phase) {
    case Phase::Off:
        return "Off";
    case Phase::FadeOut:
        return "FadeOut";
    case Phase::Flat:
        return "Flat";
    case Phase::FadeBack:
        return "FadeBack";
    case Phase::Returning:
        return "Returning";
    }
    return "?";
}

inline const char* CategoryName(Category category) {
    switch (category) {
    case Category::None:
        return "none";
    case Category::Visceral:
        return "visceral";
    case Category::Grabbed:
        return "grabbed";
    case Category::Objects:
        return "objects";
    case Category::Lamps:
        return "lamps";
    case Category::Pickup:
        return "pickup";
    case Category::Scripted:
        return "scripted";
    case Category::Ladder:
        return "ladder";
    case Category::Item:
        return "item";
    case Category::LevelUp:
        return "levelup";
    case Category::FogGate:
        return "foggate";
    }
    return "?";
}

constexpr size_t CategoryIndex(Category category) {
    return static_cast<size_t>(category);
}

// The hunter's event animations are a000_100000 .. a000_103999 (category 0, so the full id
// is the 6-digit number).
constexpr bool IsEventAnimId(s32 id) {
    return id >= 100000 && id <= 103999;
}

// The event animation being played: a pending TimeAct entry that is an event animation (the
// clip the behaviour step activated), else the last requested event animation (sticky).
inline s32 PickEventId(const s32* pending, size_t count, s32 requested) {
    for (size_t i = 0; i < count; ++i) {
        if (IsEventAnimId(pending[i])) {
            return pending[i];
        }
    }
    return requested;
}

// The Doll's Level Up ("Channel Blood Echoes"): the hunter kneels before her (101290), stays down
// while the Level Up menu is up (101270 / 101271) and rises (101272). The menu opens 0.15 s into
// the kneel (mock 2026-10-07, the Dream save: `opening 'LevelUp'` right after id=101290), so a
// flat period would put the game's most used menu on the virtual screen (66 x 40 deg, text 0.83
// deg, the world gone). Its own category (cinematic_levelup, off by default): the kneel stays in
// first-person VR and Level Up is on the menu panel like every other menu.
constexpr bool IsLevelUpKneel(s32 id) {
    return id == 101290 || (id >= 101270 && id <= 101272);
}

// Event animation id -> category (ObjActParam playerAnimId and the Lua lamp constants). Unknown
// ids are scripted.
constexpr Category ClassifyEvent(s32 id) {
    if (!IsEventAnimId(id)) {
        return Category::Scripted;
    }
    if (IsLevelUpKneel(id)) {
        return Category::LevelUp;
    }
    if ((id >= 101100 && id <= 101121) || (id >= 101141 && id <= 101144)) {
        return Category::Objects; // doors and gates; chests and coffins
    }
    if (id == 101150 || id == 101151 || id == 101200 || id == 101230 || id == 101240) {
        return Category::Objects; // Operate Device (levers, elevators); 101200 is also revival
    }
    if (id == 101140) {
        return Category::Pickup;
    }
    if (id == 101130) {
        return Category::FogGate; // through a fog gate (the Blood-starved Beast's, 2026-10-18 log)
    }
    if ((id >= 101160 && id <= 101171) || id == 101190) {
        return Category::Lamps;
    }
    return Category::Scripted;
}

// The ThrowState role: odd = the hunter is the attacker, even = the hunter is the victim.
constexpr Category ThrowCategory(u8 throw_state) {
    return (throw_state & 1) != 0 ? Category::Visceral : Category::Grabbed;
}

// ---- Canned item uses (Category::Item, cinematic_item) -------------------------------------
// An item whose game use is a full-body attack that only lands as the game plays it (the Shaman
// Bone Blade's stab, a000_100370; core/vr/quick_items.h CannedOf), used through our own Square
// (the quick-item hold ring's), is shown flat like a visceral and comes back at the game's cancel
// window. The use has no event bit or throw state of its own, so our Square arms it (the
// presenter's VR::Quick::NoteCannedUse, a serial and its time) and the action flags' item-use bit
// (0x40, set on every update of an item-use state) plays it:
//  - started by the bit's rising edge after the arm, or the item's own clip pushed after it (the
//    bit already on: the tail of another use), within ItemArmNs of the arm; an arm the game never
//    answered (busy, refused: the Square dropped) expires;
//  - on while the bit stays on (at most ItemMaxNs: a bit that never clears is no use), off with it;
//    back on within ItemRejoinNs (a state chain's one-update gap) it is the same use.
// The Machine treats it as the other categories: entered from first-person VR, back to VR once
// the game's cancel window has stayed open exit_ns (the rest of the use is ignored until the bit
// clears), or exit_ns after the bit clears.
constexpr s64 ItemArmNs = 1000 * Millis;
constexpr s64 ItemMaxNs = 10'000 * Millis;
constexpr s64 ItemRejoinNs = 100 * Millis;

struct ItemSignals {
    bool valid{false};  // the action flags were read
    bool use{false};    // the item-use bit (0x40)
    bool clip{false};   // the armed item's own clip among the pending TimeAct entries
    u32 arm_serial{0};  // our Square for a canned item (0: never)
    s64 arm_ns{0};      // ... when it went out (steady ns)
};

enum class ItemEvent : u8 { None, Start, Rejoin, End, Expired, TimedOut };

inline const char* ItemEventName(ItemEvent e) {
    switch (e) {
    case ItemEvent::None:
        return "none";
    case ItemEvent::Start:
        return "start";
    case ItemEvent::Rejoin:
        return "rejoin";
    case ItemEvent::End:
        return "end";
    case ItemEvent::Expired:
        return "expired";
    case ItemEvent::TimedOut:
        return "timed out";
    }
    return "?";
}

struct ItemOut {
    bool on{false}; // the canned use is playing (Signals::item)
    ItemEvent event{ItemEvent::None};
    s64 arm_age_ns{0};  // Start / Expired: since the arm
    s64 use_age_ns{0};  // End / TimedOut: how long it played
    bool by_clip{false}; // Start: by the clip (the bit was already on)
};

class ItemUseGate {
public:
    ItemOut Update(s64 now, const ItemSignals& s) {
        ItemOut out{};
        const bool bit = s.valid && s.use;
        if (s.arm_serial != 0 && s.arm_serial != serial) {
            // A new Square (seen once; one during a use waits for its end: the bit off, then on).
            serial = s.arm_serial;
            armed = true;
            arm_ns = s.arm_ns;
            off_seen = false;
        }
        if (active) {
            if (!bit || now - start_ns > ItemMaxNs) {
                active = false;
                out.event = bit ? ItemEvent::TimedOut : ItemEvent::End;
                out.use_age_ns = now - start_ns;
                stale = bit; // ignored until it clears
                ended_ns = bit ? 0 : now;
            }
        } else if (bit && !stale && ended_ns != 0 && now - ended_ns <= ItemRejoinNs) {
            // The bit back after a short gap (a state chain): the same use goes on.
            active = true;
            out.event = ItemEvent::Rejoin;
            out.use_age_ns = now - start_ns;
        }
        if (!bit) {
            stale = false;
        }
        if (!active && armed) {
            off_seen = off_seen || !bit;
            if (now - arm_ns > ItemArmNs) {
                armed = false;
                if (out.event == ItemEvent::None) {
                    out.event = ItemEvent::Expired;
                    out.arm_age_ns = now - arm_ns;
                }
            } else if (bit && !stale && (off_seen || s.clip)) {
                armed = false;
                active = true;
                start_ns = now;
                ended_ns = 0;
                out.event = ItemEvent::Start;
                out.arm_age_ns = now - arm_ns;
                out.by_clip = !off_seen;
            }
        }
        out.on = active;
        return out;
    }

    void Reset() {
        *this = ItemUseGate{};
    }

    [[nodiscard]] bool Active() const {
        return active;
    }
    [[nodiscard]] bool Armed() const {
        return armed;
    }

private:
    u32 serial{0};
    bool armed{false};
    s64 arm_ns{0};
    bool off_seen{false}; // the bit read off since the arm
    bool active{false};
    s64 start_ns{0};
    s64 ended_ns{0};   // the last use's end (a rejoin within ItemRejoinNs), 0 = none
    bool stale{false}; // a timed-out bit, ignored until it clears
};

// ---- Ladders --------------------------------------------------------------------------------
// Ladders never set the event bit or the throw state, so they have a signal of their own: the
// HKS ladder action state every Ladder_* state writes on each update (-1 off the ladder), the
// ladder object's rung counter (how much climb is left) and the ladder clips pushed to the
// TimeAct ring. The LadderGate decides when a climb is shown flat:
//  - getting on stays in VR: the mount (0.9 s from below, 2.0 s from above) and enter_delay of
//    climbing after it;
//  - the climb goes flat only when the predicted climb left gives at least min_flat of it, so
//    short ladders stay in VR (only while the ladder object cannot be read at all: after
//    unknown_min_steps steps in one direction);
//  - VR comes back exit_lead before the predicted dismount (about one step early), when the
//    ladder object resets (the dismount or a slide's landing starts), and at once on a dismount,
//    a fall or death, and (combat_exit) an attack, a blocked climb or damage: an HP drop of at
//    least damage_pct of the maximum in one update, or the damage clip (a slow drain such as
//    poison is not damage);
//  - a damage bind (the state reads -1 for a moment while the hunter stays on the ladder) keeps
//    the climb (its direction, steps and escape hatch); it ends the climb after LadderBindMaxNs;
//  - after a return, or a cut before the flat period, a flat period needs a new step and reenter;
//  - getting on a ladder always starts a new climb (the escape hatch of the last one is over).
// The Machine returns from a ladder at once (exit_ns and min_ns would show the dismount flat),
// with the ladder fade.

// The hunter's ladder clips (a000_1010xx: the TimeAct id is the plain number) and the deaths a
// ladder plays.
enum class LadderClip : u8 {
    None,
    Mount,      // getting on: 101010 from below (0.90 s), 101020 from above (2.00 s)
    Up,         // a step up: 101011, 101012 (0.433 s, chained at 0.37-0.40 s)
    FastUp,     // a fast step up: 101015, 101016 (0.333 s)
    Down,       // a step down: 101021, 101022 (0.433 s)
    Idle,       // 101030, 101040
    Slide,      // sliding down: 101000, 101001 (0.133 s a rung), 101003, 101004
    Hit,        // blocked by a character above or below: 101037, 101038, 101047, 101048
    Attack,     // 101035, 101036, 101045, 101046, 101072-101075
    Damage,     // 101060-101062
    ExitTop,    // getting off at the top: 101013, 101014 (2.00 s)
    ExitBottom, // getting off at the bottom: 101023, 101024 (1.07 s)
    SlideEnd,   // a slide's landing: 101002 (0.83 s)
    Fall,       // falling off: 101050-101052
    Die,        // 24700, 24701, 24710, 24720
    Unexpected, // 101005, 101031, 101033, 101041, 101043, 101070, 101071 (no HKS state plays them)
};

constexpr LadderClip ClassifyLadderClip(s32 id) {
    if (id == 101010 || id == 101020) {
        return LadderClip::Mount;
    }
    if (id == 101011 || id == 101012) {
        return LadderClip::Up;
    }
    if (id == 101015 || id == 101016) {
        return LadderClip::FastUp;
    }
    if (id == 101021 || id == 101022) {
        return LadderClip::Down;
    }
    if (id == 101030 || id == 101040) {
        return LadderClip::Idle;
    }
    if (id == 101000 || id == 101001 || id == 101003 || id == 101004) {
        return LadderClip::Slide;
    }
    if (id == 101037 || id == 101038 || id == 101047 || id == 101048) {
        return LadderClip::Hit;
    }
    if (id == 101035 || id == 101036 || id == 101045 || id == 101046 ||
        (id >= 101072 && id <= 101075)) {
        return LadderClip::Attack;
    }
    if (id >= 101060 && id <= 101062) {
        return LadderClip::Damage;
    }
    if (id == 101013 || id == 101014) {
        return LadderClip::ExitTop;
    }
    if (id == 101023 || id == 101024) {
        return LadderClip::ExitBottom;
    }
    if (id == 101002) {
        return LadderClip::SlideEnd;
    }
    if (id >= 101050 && id <= 101052) {
        return LadderClip::Fall;
    }
    if (id == 24700 || id == 24701 || id == 24710 || id == 24720) {
        return LadderClip::Die;
    }
    if (id == 101005 || id == 101031 || id == 101033 || id == 101041 || id == 101043 ||
        id == 101070 || id == 101071) {
        return LadderClip::Unexpected;
    }
    return LadderClip::None;
}

inline const char* LadderClipName(LadderClip clip) {
    switch (clip) {
    case LadderClip::None:
        return "none";
    case LadderClip::Mount:
        return "mount";
    case LadderClip::Up:
        return "up";
    case LadderClip::FastUp:
        return "fast_up";
    case LadderClip::Down:
        return "down";
    case LadderClip::Idle:
        return "idle";
    case LadderClip::Slide:
        return "slide";
    case LadderClip::Hit:
        return "hit";
    case LadderClip::Attack:
        return "attack";
    case LadderClip::Damage:
        return "damage";
    case LadderClip::ExitTop:
        return "exit_top";
    case LadderClip::ExitBottom:
        return "exit_bottom";
    case LadderClip::SlideEnd:
        return "slide_end";
    case LadderClip::Fall:
        return "fall";
    case LadderClip::Die:
        return "die";
    case LadderClip::Unexpected:
        return "unexpected";
    }
    return "?";
}

// The clips that end a climb: the dismounts, a fall, death.
constexpr bool IsLadderEnd(LadderClip clip) {
    return clip == LadderClip::ExitTop || clip == LadderClip::ExitBottom ||
           clip == LadderClip::SlideEnd || clip == LadderClip::Fall || clip == LadderClip::Die;
}

// The ladder clip the last behaviour step pushed (its pending TimeAct entries): the newest one,
// a clip that ends the climb before any other; -1 = none.
inline s32 PickLadderClip(const s32* pending, size_t count) {
    s32 picked = -1;
    bool end = false;
    for (size_t i = 0; i < count; ++i) {
        const LadderClip clip = ClassifyLadderClip(pending[i]);
        if (clip != LadderClip::None && (IsLadderEnd(clip) || !end)) {
            picked = pending[i];
            end = end || IsLadderEnd(clip);
        }
    }
    return picked;
}

// The ladder action state (s8): -1 off the ladder (also during the dismounts, a slide's landing,
// a fall and a damage bind), 0/1 getting on from below/above, 2/3 a step up, 4/5 a step down,
// 8/9 idle (also while blocked), 10-13 attacks, 14/16/19 sliding. The fast steps, the slide loop
// and the death states do not write it: it keeps the value before.
constexpr bool IsLadderMountState(s8 state) {
    return state == 0 || state == 1;
}
constexpr bool IsLadderStepState(s8 state) {
    return (state >= 2 && state <= 5) || state == 14;
}
constexpr bool IsLadderAttackState(s8 state) {
    return state >= 10 && state <= 13;
}
constexpr bool IsKnownLadderState(s8 state) {
    return (state >= -1 && state <= 5) || state == 8 || state == 9 || IsLadderAttackState(state) ||
           state == 14 || state == 16 || state == 19;
}

// Time per step for the length prediction (tuned from the logs).
constexpr s64 LadderStepNs = 400 * Millis;      // a step up or down
constexpr s64 LadderFastStepNs = 300 * Millis;  // a fast step up
constexpr s64 LadderSlideStepNs = 133 * Millis; // a rung while sliding
// A step's clip and its state byte can arrive a few updates apart (a slow frame, a late byte):
// one step when the second comes within this time or this many updates of the first. Two clips,
// or two state changes, are always two steps.
constexpr s64 LadderStepMergeNs = 250 * Millis;
constexpr u64 LadderStepMergeUpdates = 2;
// A damage bind (the state -1 while the hunter stays on the ladder) lasting longer than this ends
// the climb.
constexpr s64 LadderBindMaxNs = 1500 * Millis;
constexpr s32 LadderMaxRungs = 256;
constexpr s32 LadderMaxHp = 100'000; // a larger maximum HP reading is not plausible

struct LadderConfig {
    bool combat_exit{true};           // an attack, a blocked climb or damage returns to VR
    s64 enter_delay_ns{500 * Millis}; // climbing this long after getting on before going flat
    s64 min_flat_ns{1500 * Millis};   // a climb predicted to be flat for less stays in VR
    s64 exit_lead_ns{450 * Millis};   // back to VR this long before the dismount (0 = at it)
    s64 reenter_ns{1000 * Millis};    // no flat period sooner than this after the last one
    s64 fade_ns{100 * Millis};        // the fade of a ladder flat period (0 = hard cut)
    u32 unknown_min_steps{2};         // length unknown: steps in one direction before going flat
    float damage_pct{3.0f}; // an HP drop is damage from this % of the maximum HP (0 = any drop)
};

// The smallest HP drop in one update that counts as damage: damage_pct of the maximum HP (of the
// HP before when the maximum is not plausible), at least 1.
inline s32 LadderDamageMin(float damage_pct, s32 hp_max, s32 hp_before) {
    const s32 reference =
        hp_max > 0 && hp_max >= hp_before && hp_max <= LadderMaxHp ? hp_max : hp_before;
    const double min = static_cast<double>(std::max(reference, 0)) *
                       static_cast<double>(std::clamp(damage_pct, 0.0f, 100.0f)) / 100.0;
    const s32 whole = static_cast<s32>(min);
    return std::max(static_cast<double>(whole) < min ? whole + 1 : whole, 1);
}

// One update's ladder readings.
struct LadderSignals {
    bool valid{false};        // the ladder state was read (false: the ladder signal is off)
    s8 state{-1};             // the ladder action state, -1 = off the ladder
    bool object_valid{false}; // the ladder object was read (length prediction)
    s32 object_state{0};      // 1 = on the ladder (rung and top set), 2 = getting on, 0 = none
    s32 rung{0};              // current rung, 0 = bottom
    s32 top{0};               // top rung
    s32 clip{-1};             // the ladder clip the last behaviour step pushed (PickLadderClip)
};

enum class LadderEvent : u8 {
    None,
    Enter, // the climb goes flat
    Exit,  // back to VR (`reason`; `cut`: at once, not ahead of the predicted dismount)
    Skip,  // too short to go flat (once per arm)
};

// What one gate update did (for the Machine and the log).
struct LadderOut {
    LadderEvent event{LadderEvent::None};
    // Exit: lead, reset, dismount_top, dismount_bottom, slide_end, dismount, fall, death, attack,
    // hit, damage, mount, invalid, off. A disarm before going flat (attack, ...) leaves the
    // reason too.
    const char* reason{""};
    bool cut{false};
    bool flat{false};          // the gate asks for the flat screen
    bool session{false};       // on a ladder (state != -1)
    bool mounting{false};      // getting on
    bool armed{false};         // the climb timer runs (cleared by an exit, a fight, the end)
    bool suppressed{false};    // escape hatch: this climb stays in VR
    s32 dir{0};                // +1 up, -1 down, 0 unknown
    u32 steps{0};              // steps in `dir` since the gate armed
    s32 steps_left{-1};        // -1 = unknown length
    s64 pred_ns{-1};           // predicted climb time left, -1 = unknown
    s64 step_ns{LadderStepNs}; // time per step the prediction uses
    s64 climb_ns{-1};          // time since the gate armed, -1 = not armed
    s32 clip{-1};              // the last ladder clip of this climb
    bool clip_pushed{false};   // `clip` was pushed this update
    bool unknown_state{false}; // an unexpected state value (counted as idle); first time only
    bool bind{false};          // a damage bind started this update (the climb goes on)
    bool drain{false};         // an HP drop below damage_min this update (not damage)
    s32 hp_before{0};          // HP at the update before (a damage exit)
    s32 damage_min{1};         // the smallest HP drop that counted as damage this update
};

class LadderGate {
public:
    // `enabled`: cinematic and cinematic_ladder (off ends the climb). `may_enter`: a flat period
    // can start now (VR allowed and shown); the gate never goes flat otherwise. `hp_max`: the
    // maximum HP (0 = unknown), for the damage threshold.
    LadderOut Update(s64 now, const LadderSignals& in, bool hp_valid, s32 hp, s32 hp_max,
                     const LadderConfig& cfg, bool enabled, bool may_enter) {
        LadderOut out{};
        ++updates;
        const bool valid = enabled && in.valid;
        const s8 state = valid ? in.state : s8{-1};
        const LadderClip pushed =
            valid && in.clip >= 0 ? ClassifyLadderClip(in.clip) : LadderClip::None;
        // HP: a drop of at least damage_pct of the maximum since the update before is damage (a
        // slow drain such as poison loses a little every tick and never counts), 0 is death.
        const s32 drop = hp_valid && have_hp ? last_hp - hp : 0;
        out.damage_min = LadderDamageMin(cfg.damage_pct, hp_max, last_hp);
        const bool hp_drop = drop > 0 && drop >= out.damage_min;
        out.drain = drop > 0 && !hp_drop;
        out.hp_before = last_hp;
        have_hp = hp_valid;
        if (hp_valid) {
            last_hp = hp;
        }
        const bool hp_dead = hp_valid && hp <= 0;
        // The first reading after a restart is the baseline, not a change (a stale climbing value
        // is not a step).
        const bool state_changed = have_state && state != last_state;
        have_state = true;
        last_state = state;
        // The dead latch: the death states never write the ladder state, so after a death on a
        // ladder it keeps a climbing value through the respawn. Only a new state or a new ladder
        // clip while alive counts again.
        if (hp_dead) {
            dead = true;
        } else if (dead &&
                   (state_changed || (pushed != LadderClip::None && pushed != LadderClip::Die))) {
            dead = false;
        }
        // The ladder object reads "on the ladder" (the rung counter is set).
        const bool object_on = in.object_valid && in.object_state == 1;

        if (state == -1) {
            const s32 last_clip = pushed != LadderClip::None ? in.clip : clip;
            const LadderClip last = ClassifyLadderClip(last_clip);
            // A damage bind: hit on the ladder, the state reads -1 for a moment while the hunter
            // stays on it (the ladder object still on the ladder, the damage clip or damage, no
            // dismount, fall or death). The climb goes on, with its direction, steps and escape
            // hatch; it ends when the bind lasts too long.
            const bool still_on = in.object_valid ? object_on : !object_seen;
            const bool bind = valid && session && still_on && !hp_dead && !dead &&
                              !IsLadderEnd(last) &&
                              (binding ? now - bind_since < LadderBindMaxNs
                                       : pushed == LadderClip::Damage || hp_drop);
            if (bind) {
                if (!binding) {
                    binding = true;
                    bind_since = now;
                    out.bind = true;
                    if (cfg.combat_exit) {
                        Cut(out, now, "damage");
                    }
                }
                if (pushed != LadderClip::None) {
                    clip = in.clip;
                }
                Fill(out, now, -1, -1, pushed, in.clip);
                return out;
            }
            // Off the ladder (a dismount, a slide's landing, a fall, a knock-off), or no signal.
            if (flat) {
                Exit(out, now, true,
                     !valid ? (enabled ? "invalid" : "off")
                            : EndReason(last, hp_dead, hp_drop || binding));
            }
            EndSession();
            Fill(out, now, -1, -1, pushed, in.clip);
            out.clip = last_clip;
            return out;
        }
        binding = false; // a bind is over: the climb goes on

        // Getting on: always a new climb, also straight from a stale state (the death states never
        // write it, so the next ladder after a death on one starts without a -1 in between).
        const bool mount = IsLadderMountState(state) || pushed == LadderClip::Mount;
        if (!session || (mount && !mounting)) {
            if (flat) {
                Exit(out, now, true, "mount");
            }
            StartSession();
        }
        if (object_on) {
            object_seen = true;
        }
        if (pushed != LadderClip::None) {
            clip = in.clip;
        }
        if (!IsKnownLadderState(state) && !unknown_logged) {
            unknown_logged = true; // counted as idle
            out.unknown_state = true;
        }

        // Getting on: the gate arms on the first update after the mount.
        if (mount) {
            mounting = true;
            armed = false;
            dir = state == 1 || (state != 0 && in.clip == 101020) ? -1 : 1;
        } else if (mounting) {
            mounting = false;
            Arm(now);
        }

        // Steps: a new step clip, or the state changing to a step. Either re-arms the gate. The
        // clip and the state change of one step are merged when the second comes soon after the
        // first (either order); a second clip, or a second state change, is a new step.
        const bool step_clip = pushed == LadderClip::Up || pushed == LadderClip::FastUp ||
                               pushed == LadderClip::Down || pushed == LadderClip::Slide;
        const bool step_state = state_changed && IsLadderStepState(state);
        if (!mount && (step_clip || step_state)) {
            const s32 step_dir =
                step_clip ? (pushed == LadderClip::Up || pushed == LadderClip::FastUp ? 1 : -1)
                          : (state <= 3 ? 1 : -1);
            const bool soon = now - last_step < LadderStepMergeNs ||
                              updates - last_step_update <= LadderStepMergeUpdates;
            const bool same_step = have_step && soon && step_dir == dir &&
                                   !(step_clip && step_has_clip) && !(step_state && step_has_state);
            // The climb timer runs from the first step (at once after a mount climbed on
            // without a pause).
            if (!armed || steps == 0) {
                Arm(now);
            }
            if (!same_step) {
                steps = step_dir == dir && steps > 0 ? steps + 1 : 1;
                last_step = now;
                last_step_update = updates;
                step_has_clip = step_has_state = false;
            }
            step_has_clip = step_has_clip || step_clip;
            step_has_state = step_has_state || step_state;
            if (step_clip || !same_step) {
                step_ns = pushed == LadderClip::FastUp ? LadderFastStepNs
                          : pushed == LadderClip::Slide || (!step_clip && state == 14)
                              ? LadderSlideStepNs
                              : LadderStepNs;
            }
            dir = step_dir;
            have_step = true;
        }

        // The length left: steps to the top going up, to the bottom going down. It is unknown
        // only while the ladder object cannot be read (and was not read on this climb); an object
        // that reads "off the ladder" mid-climb was reset by the dismount or the slide's landing,
        // a few updates before the state shows it.
        s32 steps_left = -1;
        if (object_on && in.top >= 1 && in.top <= LadderMaxRungs && in.rung >= 0 &&
            in.rung <= in.top && dir != 0) {
            steps_left = dir > 0 ? in.top - in.rung : in.rung;
        }
        const s64 pred = steps_left >= 0 ? steps_left * step_ns : -1;
        const bool length_unknown = !in.object_valid && !object_seen;
        const bool object_reset = object_seen && !object_on;

        // What returns to VR at once (or, before the flat period, disarms the gate).
        const LadderClip current = ClassifyLadderClip(clip);
        const bool death = hp_dead || dead || current == LadderClip::Die;
        const bool end = IsLadderEnd(current);
        const bool attack = IsLadderAttackState(state) || pushed == LadderClip::Attack;
        const bool hit = pushed == LadderClip::Hit;
        const bool damage = hp_drop || pushed == LadderClip::Damage;
        const char* cut = death              ? "death"
                          : end              ? EndReason(current, false, false)
                          : !cfg.combat_exit ? nullptr
                          : attack           ? "attack"
                          : hit              ? "hit"
                          : damage           ? "damage"
                                             : nullptr;
        if (cut != nullptr) {
            Cut(out, now, cut);
        }
        // One step ahead of the dismount, or the ladder object reset (the dismount starts).
        if (flat && cfg.exit_lead_ns > 0 && pred >= 0 && pred <= cfg.exit_lead_ns) {
            Exit(out, now, false, "lead");
        }
        if (flat && object_reset) {
            Exit(out, now, false, "reset");
        }
        // Going flat: climbing (at least one step) long enough, not right after a return or a
        // cut, and enough climb left.
        if (!flat && armed && steps > 0 && !mounting && !suppressed && may_enter &&
            cut == nullptr && now - climb_since >= cfg.enter_delay_ns &&
            (!have_exit || now - last_exit >= cfg.reenter_ns)) {
            if (pred >= 0) {
                if (pred - cfg.exit_lead_ns >= cfg.min_flat_ns) {
                    Enter(out);
                } else if (!skip_logged) {
                    skip_logged = true;
                    out.event = LadderEvent::Skip;
                }
            } else if (length_unknown && steps >= cfg.unknown_min_steps) {
                Enter(out);
            }
        }
        Fill(out, now, steps_left, pred, pushed, in.clip);
        return out;
    }

    // The escape hatch (Menu hold) on a ladder: back to VR, and this climb stays in VR until the
    // hunter is off the ladder (a damage bind does not count) or gets on another one.
    void Suppress(s64 now) {
        if (!session) {
            return;
        }
        suppressed = true;
        if (flat) {
            flat = false;
            armed = false;
            last_exit = now;
            have_exit = true;
        }
    }

    void Reset() {
        *this = LadderGate{};
    }
    // Loading (no player update for a while) or a new main player: everything but the dead latch,
    // which exists for exactly that path (a death on a ladder, loading, the respawn).
    void Restart() {
        const bool was_dead = dead;
        Reset();
        dead = was_dead;
    }

    [[nodiscard]] bool Flat() const {
        return flat;
    }
    [[nodiscard]] bool InSession() const {
        return session;
    }
    [[nodiscard]] bool Suppressed() const {
        return suppressed;
    }
    [[nodiscard]] bool Dead() const {
        return dead;
    }
    [[nodiscard]] s32 Clip() const {
        return clip;
    }

private:
    static const char* EndReason(LadderClip clip, bool hp_dead, bool hp_drop) {
        if (hp_dead || clip == LadderClip::Die) {
            return "death";
        }
        switch (clip) {
        case LadderClip::Fall:
            return "fall";
        case LadderClip::ExitTop:
            return "dismount_top";
        case LadderClip::ExitBottom:
            return "dismount_bottom";
        case LadderClip::SlideEnd:
            return "slide_end";
        default:
            return hp_drop ? "damage" : "dismount"; // knocked off, or a bind that lasted too long
        }
    }

    // A cut: back to VR at once when flat; before the flat period the gate disarms. Either way the
    // next flat period needs a new step and reenter after this.
    void Cut(LadderOut& out, s64 now, const char* reason) {
        if (flat) {
            Exit(out, now, true, reason);
        } else if (armed) {
            armed = false;
            last_exit = now;
            have_exit = true;
            out.reason = reason;
        }
    }

    void Arm(s64 now) {
        armed = true;
        climb_since = now;
        steps = 0;
        skip_logged = false;
    }

    void Enter(LadderOut& out) {
        flat = true;
        out.event = LadderEvent::Enter;
    }

    void Exit(LadderOut& out, s64 now, bool cut, const char* reason) {
        flat = false;
        armed = false;
        last_exit = now;
        have_exit = true;
        out.event = LadderEvent::Exit;
        out.cut = cut;
        out.reason = reason;
    }

    void StartSession() {
        session = true;
        mounting = false;
        armed = false;
        suppressed = false;
        dir = 0;
        steps = 0;
        step_ns = LadderStepNs;
        have_step = false;
        step_has_clip = step_has_state = false;
        clip = -1;
        object_seen = false;
        binding = false;
    }

    void EndSession() {
        StartSession();
        session = false;
    }

    void Fill(LadderOut& out, s64 now, s32 steps_left, s64 pred, LadderClip pushed,
              s32 pushed_id) const {
        out.flat = flat;
        out.session = session;
        out.mounting = mounting;
        out.armed = armed;
        out.suppressed = suppressed;
        out.dir = dir;
        out.steps = steps;
        out.steps_left = steps_left;
        out.pred_ns = pred;
        out.step_ns = step_ns;
        out.climb_ns = armed ? now - climb_since : -1;
        out.clip_pushed = pushed != LadderClip::None;
        out.clip = out.clip_pushed ? pushed_id : clip;
    }

    bool session{false};
    bool mounting{false};
    bool armed{false};
    bool flat{false};
    bool suppressed{false};
    bool skip_logged{false};
    bool unknown_logged{false};
    s64 climb_since{0};
    bool have_exit{false};
    s64 last_exit{0};
    s32 dir{0};
    u32 steps{0};
    s64 step_ns{LadderStepNs};
    bool have_step{false};
    s64 last_step{0};           // the current step's first clip or state change
    u64 last_step_update{0};    // ... and its update
    bool step_has_clip{false};  // the current step's clip was seen
    bool step_has_state{false}; // the current step's state change was seen
    s32 clip{-1};
    bool object_seen{false}; // the ladder object read "on the ladder" on this climb
    bool binding{false};     // in a damage bind (the state -1, still on the ladder)
    s64 bind_since{0};
    u64 updates{0};
    bool have_state{false};
    s8 last_state{-1};
    bool dead{false};
    bool have_hp{false};
    s32 last_hp{0};
};

struct Config {
    bool enabled{true}; // cinematic=1
    bool vr_grab_attacks{false}; // a victim throw also overrides overlapping event/item signals
    // Per category (index = Category; None is unused, Ladder is cinematic_ladder, Item is
    // cinematic_item, LevelUp is cinematic_levelup: off, the Doll's kneel stays in VR; FogGate is
    // cinematic_foggate: off, the walk through a fog gate stays in VR).
    std::array<bool, CategoryCount> categories{true, true, true, true, true, true,
                                               true, true, true, false, false};
    s64 fade_ns{150 * Millis}; // the fade to black on the way in; 0 = hard cut
    // The way back: the screen cuts to black and first-person VR fades in over this long (0 =
    // hard cut). Negative = the old way: the screen dims over fade_ns, VR fades in over fade_ns,
    // and the gameplay controls wait ReturnHoldNs after that.
    s64 return_fade_ns{80 * Millis};
    s64 exit_ns{50 * Millis}; // all signals off (or the cancel window open) this long
    s64 min_ns{600 * Millis}; // shortest flat period
    bool release{true};       // back to VR when the game hands control back (its cancel window)
    // 1: after a fatal throw, flat while HP <= 0 (until the respawn; the default before
    // 2026-10-07); 0: a death ends the flat period at once (see the top: the death screen in VR).
    bool death_hold{false};
    s64 throw_timeout_ns{20'000 * Millis}; // a throw state this old is ignored until it clears
    LadderConfig ladder{};
};

// One main-player update's readings. A signal that is not valid counts as off.
struct Signals {
    bool throw_valid{false}; // the throw state was read
    u8 throw_state{0};       // ThrowNode+0x20: 1/3/5 the hunter throws, 2/4/6 is thrown
    bool aligning{false};    // ThrowNode+0x64: partners being moved into place, no animation yet
    bool event_valid{false}; // the event-animation bit was read (behaviour module ready)
    bool event{false};       // HKS "in event animation" (set every update of an Event state)
    bool ids_valid{false};   // event_id can be used; without ids every event counts as objects
    s32 event_id{-1};        // PickEventId, -1 = unknown
    bool hp_valid{false};
    s32 hp{0};
    s32 hp_max{0};          // maximum HP (the ladder damage threshold), 0 = unknown
    bool new_player{false}; // the main player object changed since the last update (Off at once)
    // The loading page ended since the last update: the camera's first tick after a gap in which a
    // new main player arrived (the arrival window opens again from here).
    bool load_end{false};
    // The game's cancel window this update (the action request module its TAE fills): `control`
    // = the playing clip takes an attack (ControlAllowMask) or a move cancels it. Without
    // `cancel_valid` nothing is released early (the signals' end returns, as before).
    bool cancel_valid{false};
    bool control{false};
    LadderSignals ladder{};
    // A canned item use through our own Square is playing (ItemUseGate::Update(...).on).
    bool item_valid{false};
    bool item{false};
};

// A flat period's way back (steady-clock ns, 0 = not seen), for the log's `VR cine: RETURN` line.
struct ReturnTimes {
    s64 start{0};       // the flat period was requested (FadeOut or Flat entered from Off)
    s64 control{0};     // the game's cancel window first opened in it: control back
    s64 request_end{0}; // the request ended (the window opened for a release, else signals off)
    s64 signals_off{0}; // the animation's own signals ended (throw state 0, event bit off)
    s64 exit{0};        // the flat view ended (FadeBack, or Returning straight from Flat)
    s64 returning{0};   // the camera follows the head again (Returning entered)
    s64 projection{0};  // first-person VR shown again (the presenter's layer switch)
    s64 off{0};         // Off: the gameplay controls are back
    s64 return_fade{0}; // Returning's fade
    Category source{Category::None};
    const char* why{""}; // what ended the flat view (the reason of the exit)
};

// What one update did (for the log).
struct Step {
    bool changed{false};
    Phase from{Phase::Off};
    Phase to{Phase::Off};
    s64 from_ns{0}; // time spent in `from`
    const char* reason{""};
    bool resync{false};      // Returning entered: re-sync the game heading to the head
    bool throw_stuck{false}; // the throw signal timed out at this update
    bool raw{false};         // a flat request this update (signals, toggles, escape hatch)
    Category category{Category::None};
    s64 fade_ns{0}; // the fade the new phase runs with
    float b0{1.0f}; // FadeOut aborted (-> Returning): the brightness the fade had reached
    // Control back: the cancel window has stayed open exit_ns (release on): from now on the
    // animation's signals are ignored until they end.
    bool control_back{false};
    // A death ended the flat period (death_hold off): HP reached 0 in it this update; the
    // animation's signals are ignored from now on until they end, like control back.
    bool death_back{false};
    // The released animation's signals ended this update, this long after control came back
    // (how much sooner VR came back than at their end).
    bool latch_cleared{false};
    s64 latch_age_ns{0};
    // Returning -> Off this update: `times` is the period's finished timeline.
    bool returned{false};
    ReturnTimes times{};
    LadderOut ladder{};
};

class Machine {
public:
    // `allowed`: VR on and the display mode is Auto. `suppress_request`: Menu held during the
    // cinematic (the escape hatch: first-person VR comes back at once and the rest of this
    // animation stays in VR). `can_enter`: first-person VR is being shown; a flat period only
    // starts from it (with the virtual screen already up it would only dim the screen).
    // `projection_ns`: when the presenter last brought first-person VR (the projection layer) up,
    // -1 while the virtual screen is shown or unknown; Returning ends once VR has been shown for
    // its fade (without it: after the fade + ReturnHoldNs).
    Step Update(const Signals& s, s64 now, const Config& cfg, bool allowed, bool suppress_request,
                bool can_enter = true, s64 projection_ns = -1) {
        Step step{};
        step.from = step.to = phase;
        if (!have_update || s.new_player || s.load_end) {
            load_ns = now; // the arrival window (ArrivalHoldNs)
        } else if (load_ns >= 0 && now - load_ns >= ArrivalHoldNs) {
            load_ns = -1; // no arrival in time
        }
        if (have_update && (now - last_update > LongGapNs || s.new_player)) {
            ResetSignals();
            if (phase != Phase::Off) {
                Enter(step, Phase::Off, now, s.new_player ? "new main player" : "no player update",
                      0);
            }
        }
        have_update = true;
        last_update = now;

        // Throws: entered after a few updates of an animating state (never while aligning),
        // ignored after the time-out until the state clears (a stop the engine refused).
        const u8 ts = s.throw_valid ? s.throw_state : 0;
        if (ts == 0) {
            throw_good = throw_other = 0;
            throw_active = throw_stuck = false;
        } else {
            if (!throw_active) {
                throw_active = true;
                throw_since = now;
            }
            const bool known = ts <= 6;
            throw_good = known && !s.aligning ? throw_good + 1 : 0;
            throw_other = known ? 0 : throw_other + 1;
            if (!throw_stuck && now - throw_since > cfg.throw_timeout_ns) {
                throw_stuck = true;
                step.throw_stuck = true;
            }
        }
        throw_on = ts != 0 && !throw_stuck &&
                   (throw_good >= ThrowEnterUpdates || throw_other >= ThrowFallbackUpdates);
        throw_category = ThrowCategory(ts);

        // Event animations: classified on the rising edge (a chained state with a one-update
        // gap is classified again).
        const bool event_now = s.event_valid && s.event;
        if (event_now && !event_on) {
            event_category = s.ids_valid ? ClassifyEvent(s.event_id) : Category::Objects;
        }
        event_on = event_now;
        // A canned item use (ItemUseGate: our Square, the game's item-use state).
        const bool item_now = s.item_valid && s.item;
        item_on = item_now;

        // Control back (see the top): the released animation's signals stay ignored until they
        // end; a fatal end of a released throw is a death grab again (flat, with the hold).
        const bool alive = !s.hp_valid || s.hp > 0;
        const bool latched = latch_throw || latch_event || latch_item;
        if (cfg.death_hold && !alive) {
            latch_throw = false;
        }
        if (ts == 0) {
            latch_throw = false;
        }
        if (!event_now) {
            latch_event = false;
        }
        if (!item_now) {
            latch_item = false;
        }
        if (latched && !latch_throw && !latch_event && !latch_item) {
            step.latch_cleared = true;
            step.latch_age_ns = now - latch_since;
            if (times.signals_off == 0 && times.start != 0) {
                times.signals_off = now;
            }
        }
        // The window opens in the animation that holds the flat period: armed once it was seen
        // closed in this period (the first updates may carry the last clip's requests), never on
        // a ladder or with HP at 0. With cfg.release, once it has stayed open exit_ns the
        // period's signals are latched (ignored); without it the time is only recorded.
        const bool held_period =
            phase == Phase::FadeOut || phase == Phase::Flat || phase == Phase::FadeBack;
        const bool animation = (ts != 0 && !latch_throw) || (event_now && !latch_event) ||
                               (item_now && !latch_item);
        const bool window = s.cancel_valid && s.control;
        if (!held_period || source == Category::Ladder || !animation || !alive) {
            window_armed = false;
            open_since = -1;
        } else if (!window) {
            window_armed = true;
            open_since = -1;
        } else if (window_armed && open_since < 0) {
            open_since = now;
            if (times.control == 0) {
                times.control = now;
            }
        }
        if (cfg.release && open_since >= 0 && now - open_since >= cfg.exit_ns) {
            latch_throw = ts != 0;
            latch_event = event_now;
            latch_item = item_now;
            latch_since = open_since;
            released = true;
            times.request_end = open_since;
            step.control_back = true;
            window_armed = false;
            open_since = -1;
        }
        // Death without the hold (see the top): every animation signal playing now is latched
        // like a release (ignored until it ends), so a flat period ends at once and none starts
        // while HP is 0. A ladder's own gate handles a death on it.
        const bool dying = !cfg.death_hold && !alive;
        if (dying && ((ts != 0 && !latch_throw) || (event_now && !latch_event) ||
                      (item_now && !latch_item))) {
            latch_throw = latch_throw || ts != 0;
            latch_event = latch_event || event_now;
            latch_item = latch_item || item_now;
            latch_since = now;
            window_armed = false;
            open_since = -1;
            if (held_period && source != Category::Ladder) {
                released = true;
                died = true;
                times.request_end = now;
                step.death_back = true;
            }
        }

        // Escape hatch: Menu held during a cinematic keeps this animation in VR until every signal
        // has been off for exit_ns.
        if (ts != 0 || event_now || item_now) {
            last_signal = now;
        }
        // The hatch acts on this update even when the signals are already off (a death hold).
        const bool escape = suppress_request && phase != Phase::Off;
        if (escape) {
            suppressed = true;
        }
        if (suppressed && ts == 0 && !event_now && !item_now && now - last_signal >= cfg.exit_ns) {
            suppressed = false;
        }

        // Ladders: the gate decides; on a ladder the escape hatch lasts until the hunter is off it.
        step.ladder = ladder.Update(now, s.ladder, s.hp_valid, s.hp, s.hp_max, cfg.ladder,
                                    cfg.enabled && cfg.categories[CategoryIndex(Category::Ladder)],
                                    allowed && (phase != Phase::Off || can_enter));
        if (escape && ladder.InSession()) {
            ladder.Suppress(now);
            step.ladder.flat = false;
            step.ladder.suppressed = true;
        }
        const bool ladder_part = ladder.Flat();

        const bool throw_part =
            throw_on && !latch_throw && cfg.categories[CategoryIndex(throw_category)];
        const bool event_part =
            event_on && !latch_event && cfg.categories[CategoryIndex(event_category)];
        const bool item_part =
            item_now && !latch_item && cfg.categories[CategoryIndex(Category::Item)];
        // Suppress from the first verified victim state, including alignment. Waiting
        // for throw_on's debounce lets an overlapping event start fading to flat first.
        const bool vr_grab = cfg.vr_grab_attacks && !throw_stuck &&
                             (ts == 2 || ts == 4 || ts == 6);
        const bool other = cfg.enabled && !suppressed && !vr_grab &&
                           (throw_part || event_part || item_part);
        raw = !vr_grab && (other || ladder_part);
        if (load_ns >= 0 && !raw && (throw_on || event_on || item_now)) {
            load_ns = -1; // the arrival plays in VR (its category off): nothing to wait for
        }
        // The category that requests the flat period, else the one detected (logs).
        category = throw_part           ? throw_category
                   : event_part         ? event_category
                   : item_part          ? Category::Item
                   : ladder_part        ? Category::Ladder
                   : throw_on           ? throw_category
                   : event_on           ? event_category
                   : item_now           ? Category::Item
                   : ladder.InSession() ? Category::Ladder
                                        : Category::None;
        // The source of the flat period: the category that held it most recently. The other
        // categories win over a ladder (their exit rules apply while one of them holds it).
        if (other) {
            source = throw_part ? throw_category : event_part ? event_category : Category::Item;
        } else if (ladder_part) {
            source = Category::Ladder;
        }
        if (raw) {
            last_raw = now;
            released = false; // a request again: the release no longer stands for the exit hold
        }
        if (phase == Phase::Off) {
            by_throw = false;
        }
        by_throw = by_throw || (other && throw_part);
        step.raw = raw;
        step.category = category;
        // The way back's timeline (the `VR cine: RETURN` line).
        if (held_period || phase == Phase::Returning) {
            if (held_period && raw) {
                times.request_end = 0;
            } else if (held_period && times.request_end == 0) {
                times.request_end = now;
            }
            if (times.signals_off == 0 && ts == 0 && !event_now && !item_now) {
                times.signals_off = now;
            }
            if (phase == Phase::Returning && projection_ns >= 0 && times.projection == 0) {
                times.projection = std::max(projection_ns, phase_start);
            }
        }

        if (!allowed) {
            if (phase != Phase::Off) {
                Enter(step, Phase::Off, now, "VR off or the screen chosen by hand", 0);
            }
            return step;
        }
        if (step.changed) {
            return step; // the stale reset above is this update's transition
        }
        const bool hold = cfg.death_hold && by_throw && s.hp_valid && s.hp <= 0;
        const bool from_ladder = source == Category::Ladder;
        // The fade of a phase entered now, and of the phase in effect.
        const s64 fade = from_ladder ? cfg.ladder.fade_ns : cfg.fade_ns;
        const s64 age = now - phase_start;
        switch (phase) {
        case Phase::Off:
            if (raw && can_enter) {
                Begin(now);
                Enter(step, fade > 0 ? Phase::FadeOut : Phase::Flat, now, "signal", fade);
            } else if (raw && load_ns >= 0 && now - load_ns < ArrivalHoldNs) {
                // The screen is still up after a load (the camera held for the arrival): no dim,
                // the screen goes straight on (as Returning's "signal again").
                Begin(now);
                Enter(step, Phase::Flat, now, "signal after loading", fade);
            }
            break;
        case Phase::FadeOut:
            // VR was still driven: it fades back in from the level reached, no heading re-sync.
            if (escape || suppressed) {
                step.b0 = FadeLevel(age);
                Enter(step, Phase::Returning, now, "escape hatch", fade);
            } else if (!raw && from_ladder) {
                // Getting off (or a fight) during the fade: aborted, back up from here.
                step.b0 = FadeLevel(age);
                Enter(step, Phase::Returning, now, LadderReason(step.ladder, "ladder abort"), fade);
            } else if (!raw && died) {
                // A death during the fade (no death hold): aborted, the death screen in VR.
                step.b0 = FadeLevel(age);
                Enter(step, Phase::Returning, now, "death", fade);
            } else if (age >= phase_fade) {
                Enter(step, Phase::Flat, now, "faded out", fade);
            }
            break;
        case Phase::Flat: {
            // The escape hatch returns at once (minimum time and death hold do not apply), and so
            // does a ladder (the dismount must be in VR). Control back: the cancel window already
            // stayed open for the exit hold.
            const bool ladder_exit = !raw && from_ladder && !hold;
            const bool waited = released || now - last_raw >= cfg.exit_ns;
            // A death (no death hold) returns at once: no minimum time.
            if (escape || suppressed || ladder_exit ||
                (!raw && waited && (age >= cfg.min_ns || died) && !hold)) {
                const char* reason = escape || suppressed ? "escape hatch"
                                     : ladder_exit        ? LadderReason(step.ladder, "ladder exit")
                                     : died               ? "death"
                                     : released           ? "control back"
                                                          : "signals off";
                // Back to first person: the screen cuts to black and VR fades in (return_fade_ns),
                // or the old way (and ladders): the screen dims first (FadeBack).
                const s64 back = from_ladder || cfg.return_fade_ns < 0 ? -1 : cfg.return_fade_ns;
                if (back >= 0) {
                    Enter(step, Phase::Returning, now, reason, back);
                    step.resync = true;
                } else if (fade > 0) {
                    Enter(step, Phase::FadeBack, now, reason, fade);
                } else {
                    Enter(step, Phase::Returning, now, reason, 0);
                    step.resync = true;
                }
            }
            break;
        }
        case Phase::FadeBack:
            if (raw) {
                Enter(step, Phase::Flat, now, "signal again", fade);
            } else if (age >= phase_fade) {
                Enter(step, Phase::Returning, now, "faded back", phase_fade);
                step.resync = true;
            }
            break;
        case Phase::Returning: {
            // Off (the gameplay controls back) once VR has been shown for the fade; the old way,
            // or without the presenter's word, after the fade + ReturnHoldNs.
            const bool shown = cfg.return_fade_ns >= 0 && projection_ns >= 0 &&
                               now - std::max(projection_ns, phase_start) >= phase_fade;
            if (raw && !can_enter) {
                // First-person VR is not shown (it could not come back, or not yet): the virtual
                // screen is still the shown layer, so it stays up without a dim.
                Begin(now);
                Enter(step, Phase::Flat, now, "signal again", fade);
            } else if (raw) {
                Begin(now);
                Enter(step, fade > 0 ? Phase::FadeOut : Phase::Flat, now, "signal again", fade);
            } else if (shown || age >= phase_fade + ReturnHoldNs) {
                Enter(step, Phase::Off, now, "returned", 0);
                times.off = now;
                step.returned = true;
                step.times = times;
            }
            break;
        }
        }
        if (step.changed) {
            if (step.from == Phase::FadeBack && step.to == Phase::Flat) {
                // The same period goes on: its way back starts again.
                times.request_end = times.exit = 0;
                times.why = "";
            }
            if ((step.from == Phase::Flat || step.from == Phase::FadeOut) &&
                (step.to == Phase::FadeBack || step.to == Phase::Returning)) {
                times.exit = now;
                times.why = step.reason;
                times.source = source;
            }
            if (step.to == Phase::Returning) {
                times.returning = now;
                times.return_fade = step.fade_ns;
                times.projection = 0;
            }
        }
        return step;
    }

    void Reset() {
        *this = Machine{};
    }

    [[nodiscard]] Phase GetPhase() const {
        return phase;
    }
    [[nodiscard]] s64 PhaseStart() const {
        return phase_start;
    }
    [[nodiscard]] Category GetCategory() const {
        return category;
    }
    [[nodiscard]] bool Raw() const {
        return raw;
    }
    [[nodiscard]] bool ThrowOn() const {
        return throw_on;
    }
    [[nodiscard]] bool EventOn() const {
        return event_on;
    }
    [[nodiscard]] bool ItemOn() const {
        return item_on;
    }
    [[nodiscard]] bool Suppressed() const {
        return suppressed;
    }
    [[nodiscard]] bool HeldByThrow() const {
        return by_throw;
    }
    // The fade the phase in effect runs with (the ladder fade in a ladder period).
    [[nodiscard]] s64 PhaseFade() const {
        return phase_fade;
    }
    // The category that held the flat period most recently.
    [[nodiscard]] Category Source() const {
        return source;
    }
    // Until when first-person VR waits after a load for the arrival animation (ArrivalHoldNs),
    // 0 = no wait: none since the last load, or a flat period started.
    [[nodiscard]] s64 ArrivalHoldUntil() const {
        return phase == Phase::Off && load_ns >= 0 ? load_ns + ArrivalHoldNs : 0;
    }
    [[nodiscard]] const LadderGate& Ladder() const {
        return ladder;
    }

private:
    void Enter(Step& step, Phase next, s64 now, const char* reason, s64 fade) {
        if (!step.changed) {
            step.from = phase;
            step.from_ns = now - phase_start;
        }
        step.changed = true;
        step.to = next;
        step.reason = reason;
        step.fade_ns = fade;
        phase = next;
        phase_start = now;
        phase_fade = fade;
    }

    // The brightness a fade down has reached after `age`.
    [[nodiscard]] float FadeLevel(s64 age) const {
        return phase_fade <= 0
                   ? 0.0f
                   : std::clamp(1.0f - static_cast<float>(age) / static_cast<float>(phase_fade),
                                0.0f, 1.0f);
    }

    // The ladder's exit reason when it left the flat period this update.
    static const char* LadderReason(const LadderOut& out, const char* fallback) {
        return out.event == LadderEvent::Exit ? out.reason : fallback;
    }

    // A new flat period (from Off, or a new signal during Returning): a fresh timeline.
    void Begin(s64 now) {
        times = ReturnTimes{};
        times.start = now;
        times.source = source;
        released = false;
        died = false;
        load_ns = -1; // the arrival (or whatever came first) had its flat period
    }

    void ResetSignals() {
        throw_good = throw_other = 0;
        throw_active = throw_stuck = throw_on = false;
        event_on = false;
        suppressed = false;
        item_on = false;
        ladder.Restart();
        source = Category::None;
        latch_throw = latch_event = latch_item = released = window_armed = died = false;
        open_since = -1;
    }

    Phase phase{Phase::Off};
    s64 phase_start{0};
    s64 phase_fade{0};
    bool have_update{false};
    s64 last_update{0};
    s64 load_ns{-1}; // the update that followed the last load, -1 = none open
    s64 last_raw{0};
    s64 last_signal{0};
    bool raw{false};
    Category category{Category::None};
    // Throw signal.
    u32 throw_good{0};  // consecutive updates with a known state, not aligning
    u32 throw_other{0}; // consecutive updates with an unknown state
    bool throw_active{false};
    s64 throw_since{0};
    bool throw_stuck{false};
    bool throw_on{false};
    Category throw_category{Category::Grabbed};
    // Event signal.
    bool event_on{false};
    Category event_category{Category::None};
    // Canned item use signal.
    bool item_on{false};
    bool suppressed{false};
    bool by_throw{false}; // this flat period had a throw (death hold)
    bool died{false};     // this flat period was ended by a death (death_hold off)
    // Control back: the cancel window (armed once seen closed in the period, open since), the
    // released animation's signals (ignored until they end) and the period's release.
    bool window_armed{false};
    s64 open_since{-1};
    bool latch_throw{false};
    bool latch_event{false};
    bool latch_item{false};
    s64 latch_since{0};
    bool released{false};
    ReturnTimes times{};
    // Ladders.
    LadderGate ladder{};
    Category source{Category::None};
};

// The brightness a phase starts from in the normal sequence: the fades down start at full
// brightness, the fades up from black.
constexpr float FreshEntry(Phase phase) {
    return phase == Phase::Flat || phase == Phase::Returning ? 0.0f : 1.0f;
}

// Brightness of the submitted layer: 0 = black .. 1. `in_phase_ns`: time spent in `phase`;
// `content_immersive`: the projection (true) or the virtual screen (false) is shown;
// `since_switch_ns`: time since the shown content last switched between the two; `entry`: the
// brightness when `phase` was entered, so a fade that interrupts another one continues from the
// current level instead of jumping; `projection_expected` (Returning): first-person VR can come
// back (camera updates, a head pose); when it cannot, the screen is brought back up instead of
// staying black. The fades go down to black in the old view, hold black until the other layer
// shows, then bring it up.
inline float Brightness(Phase phase, s64 in_phase_ns, s64 fade_ns, bool content_immersive,
                        s64 since_switch_ns, float entry, bool projection_expected = true) {
    const auto ramp = [fade_ns](s64 t) {
        return fade_ns <= 0
                   ? 1.0f
                   : std::clamp(static_cast<float>(t) / static_cast<float>(fade_ns), 0.0f, 1.0f);
    };
    entry = std::clamp(entry, 0.0f, 1.0f);
    switch (phase) {
    case Phase::Off:
        return 1.0f;
    case Phase::FadeOut:
    case Phase::FadeBack:
        return entry * (1.0f - ramp(in_phase_ns));
    case Phase::Flat:
    case Phase::Returning: {
        // The layer this phase brings up: the screen (Flat) or the projection (Returning).
        const bool shown =
            phase == Phase::Flat ? !content_immersive : content_immersive || !projection_expected;
        if (!shown) {
            return in_phase_ns >= BlackLimitNs ? 1.0f : 0.0f; // the old layer is still up
        }
        // Up from the entry level, timed from when the layer showed (or the phase began, when it
        // was already up: a signal during FadeBack, a Returning without VR).
        return std::min(1.0f, entry + ramp(std::min(in_phase_ns, since_switch_ns)));
    }
    }
    return 1.0f;
}

// The same for a phase entered from the phase before it in the normal sequence.
inline float Brightness(Phase phase, s64 in_phase_ns, s64 fade_ns, bool content_immersive,
                        s64 since_switch_ns) {
    return Brightness(phase, in_phase_ns, fade_ns, content_immersive, since_switch_ns,
                      FreshEntry(phase));
}

// The brightness as the presenter applies it, once per submitted frame: Brightness() with each
// phase entered at the level the previous one left, so FadeBack -> Flat (a new signal),
// Returning -> FadeOut, or an aborted FadeOut -> Returning (a ladder cut) continue the fade from
// where it was instead of jumping.
class Fader {
public:
    // `phase_start`: when `phase` was entered (identifies the phase instance); `fade_ns`: the
    // fade that phase runs with (Machine::PhaseFade).
    float Update(Phase phase, s64 phase_start, s64 now, s64 fade_ns, bool content_immersive,
                 s64 since_switch_ns, bool projection_expected = true) {
        if (!seen) {
            entry = FreshEntry(phase);
        } else if (phase != seen_phase || phase_start != seen_start) {
            if (seen_phase == Phase::Flat && phase == Phase::Returning) {
                entry = 0.0f; // straight from the screen: a cut to black, VR fades in from it
            } else if (seen_phase == Phase::FadeOut || seen_phase == Phase::FadeBack) {
                // A fade down depends on time only: its exact level when the next phase began.
                entry = Brightness(seen_phase, phase_start - seen_start, seen_fade,
                                   content_immersive, since_switch_ns, entry);
            } else {
                entry = level; // the level last shown
            }
        }
        seen = true;
        seen_phase = phase;
        seen_start = phase_start;
        seen_fade = fade_ns;
        level = Brightness(phase, now - phase_start, fade_ns, content_immersive, since_switch_ns,
                           entry, projection_expected);
        return level;
    }

    void Reset() {
        *this = Fader{};
    }

private:
    bool seen{false};
    Phase seen_phase{Phase::Off};
    s64 seen_start{0};
    s64 seen_fade{0};
    float entry{1.0f};
    float level{1.0f};
};

} // namespace VR::Cinematic
