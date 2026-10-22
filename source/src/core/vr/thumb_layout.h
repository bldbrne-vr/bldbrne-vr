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
#include <string>
#include <string_view>
#include <vector>

// Which control on the two controllers does what in gameplay, as data: the scheme tables of the
// motion layout (vr.ini left_scheme 1 and 2), the classic layout, and "Move with: Right stick"
// (vr.ini swap_sticks=1, the VR page's General > Move with), which moves the sticks and the thumb
// buttons that go with them to the other hand. Header-only and pure: tests/test_thumb_layout.cpp.
//
// The presenter (openxr_presenter.cpp UpdateInput) reads every control into its LOGICAL slot with
// Physical(): the rest of the controls code is written for the standard layout, so the logical A
// is dodge / sprint wherever it sits. Names shown to the player (the log, the VR page, the
// controls view) come from these tables through Label() / Expand(), never written out by hand.
//
// Move with the right stick (the menu design's section 4, chosen 2026-10-05):
// the right stick walks and the left one turns; each stick takes its click along (the
// sprint latch / quick-item tap / jump / modifier with the move stick, lock-on and the weapon switch
// with the turn stick), and the face buttons trade hands, lower with lower and upper with upper:
// dodge / sprint (A) and interact (B) go to X / Y, so the thumb that steers never has to hold the
// sprint, and the Blood Vial (X) and the quick-item arc (Y) go to A / B. Every combo follows its
// controls (left_scheme=1's D-pad is the move stick's click held + the turn stick; the Menu combos
// follow the logical sticks). Unchanged: the triggers and the grips, which belong to the hunter's
// hands (the blade in his right, the gun, the vial and the items in his left: he still holds the
// trick weapon in the right hand, and the right controller still swings it), and Menu (the right
// Touch controller has none). Menus, the virtual screen and the VR page keep the standard layout.
namespace VR::Thumbs {

enum class Control : std::uint8_t {
    A, // the right controller's lower face button (Quest Touch names)
    B, // ... its upper one
    X, // the left controller's lower face button
    Y, // ... its upper one
    StickL,
    StickR,
    ClickL, // the left stick pressed
    ClickR,
    TriggerL,
    TriggerR,
    GripL,
    GripR,
    Menu,
    Count,
    None = Count,
};

inline constexpr std::size_t ControlCount = static_cast<std::size_t>(Control::Count);

enum class Hand : std::uint8_t { Left, Right };

constexpr Hand HandOf(Control c) {
    switch (c) {
    case Control::A:
    case Control::B:
    case Control::StickR:
    case Control::ClickR:
    case Control::TriggerR:
    case Control::GripR:
        return Hand::Right;
    default:
        return Hand::Left;
    }
}

// The control the player's hand presses for `logical` (a control of the standard layout). Its own
// inverse, so it also answers which logical control a physical one carries.
constexpr Control Physical(Control logical, bool swap) {
    if (!swap) {
        return logical;
    }
    switch (logical) {
    case Control::A:
        return Control::X;
    case Control::X:
        return Control::A;
    case Control::B:
        return Control::Y;
    case Control::Y:
        return Control::B;
    case Control::StickL:
        return Control::StickR;
    case Control::StickR:
        return Control::StickL;
    case Control::ClickL:
        return Control::ClickR;
    case Control::ClickR:
        return Control::ClickL;
    default:
        return logical; // triggers, grips, Menu: the hunter's hands, and Menu only on the left
    }
}

// The haptic hand of a logical control (a tick for a button goes to the hand that pressed it).
constexpr Hand HandFor(Control logical, bool swap) {
    return HandOf(Physical(logical, swap));
}

// The Quest Touch name of a physical control, as the docs and the in-game pages write it (static
// text: the presenter asks every frame).
constexpr const char* Label(Control c) {
    switch (c) {
    case Control::A:
        return "A";
    case Control::B:
        return "B";
    case Control::X:
        return "X";
    case Control::Y:
        return "Y";
    case Control::StickL:
        return "left stick";
    case Control::StickR:
        return "right stick";
    case Control::ClickL:
        return "left stick click";
    case Control::ClickR:
        return "right stick click";
    case Control::TriggerL:
        return "left trigger";
    case Control::TriggerR:
        return "right trigger";
    case Control::GripL:
        return "left grip";
    case Control::GripR:
        return "right grip";
    case Control::Menu:
        return "Menu";
    default:
        return "?";
    }
}

// The name of the control carrying `logical`.
constexpr const char* LabelFor(Control logical, bool swap) {
    return Label(Physical(logical, swap));
}

// ---- The scheme tables ----------------------------------------------------------------------

struct Layout {
    int left_scheme{1};  // the motion layout's left hand: 1 or 2 (the classic layout is its own)
    bool classic{false}; // controls=classic
    bool swap{false};    // swap_sticks: move with the right stick
    bool inject{true};   // motion: X arms a Blood Vial to stab (inject=0: the game's vial button)
    // y_menu in effect (left_scheme=2 only; SteamVR by default): Y's tap is Options and its hold
    // the recenter (entry "menu"), and hold Y + X the game's Square where the quick-item arc
    // cannot open (entry "square_no_arc": third person, body=0, quick_arc=0).
    bool y_menu{false};
    // The runtime owns the left Menu button (SteamVR: its dashboard): none of Menu's jobs reach
    // the game (one entry, "runtime_menu", says so).
    bool menu_runtime{false};
    bool operator==(const Layout&) const = default;
};

// How an entry is worked.
enum class How : std::uint8_t {
    Press, // the control itself (a stick: pushed)
    Combo, // hold `first`, then `second`
    Chord, // both together
};

// One thing a control does in gameplay, on LOGICAL controls (Resolve() puts them in the hands).
struct Entry {
    const char* id;   // stable key ("dodge", "dpad", ...): tests, the controls view
    const char* what; // what it does, short, in the docs' words
    Control first{Control::None};
    Control second{Control::None}; // Combo / Chord only
    How how{How::Press};
};

inline std::vector<Entry> Entries(const Layout& l) {
    using C = Control;
    std::vector<Entry> e;
    const bool scheme2 = !l.classic && l.left_scheme == 2;
    const bool y_menu = scheme2 && l.y_menu;
    // The modifier: held, it makes B the gestures (and, outside left_scheme=2, the turn stick a
    // D-pad and Y the game's Square; scheme 2 dropped hold Y + right stick and hold Y + X on
    // 2026-10-15: the shoulders, the right stick and its click, and the arc reach all of it;
    // y_menu keeps hold Y + X where the arc cannot open, "square_no_arc").
    const C modifier = scheme2 ? C::Y : C::ClickL;
    e.push_back({"move", "Move", C::StickL});
    e.push_back({"turn", "Turn", C::StickR});
    e.push_back({"item_next", "Next quick item (flick down)", C::StickR});
    e.push_back({"bullets", "Blood bullets (hold up 0.5 s)", C::StickR});
    if (l.classic) {
        e.push_back({"dodge", "Dodge (tap) / sprint (hold)", C::A});
        e.push_back({"interact", "Interact", C::B});
        e.push_back({"vial", "Blood Vial", C::X});
        e.push_back({"item_use", "Use item", C::Y});
        e.push_back({"l3", "L3; jump while sprinting", C::ClickL});
        e.push_back({"lock_on", "Lock on (while held)", C::ClickR});
        e.push_back({"gun", "Fire gun / parry (L2)", C::TriggerL});
        e.push_back({"light", "Light attack (R1)", C::TriggerR});
        e.push_back({"transform", "Transform (L1)", C::GripL});
        e.push_back({"heavy", "Heavy attack / charge (R2)", C::GripR});
    } else {
        if (scheme2) {
            e.push_back({"dodge", "Dodge / sprint; sprinting: leap, or dodge where you steer",
                         C::A});
        } else {
            e.push_back({"dodge", "Dodge (tap) / sprint (hold)", C::A});
        }
        e.push_back({"interact", "Interact", C::B});
        e.push_back({"vial", l.inject ? "Arm a Blood Vial (then stab to inject)" : "Blood Vial",
                     C::X});
        if (scheme2) {
            if (y_menu) {
                e.push_back({"menu", "Options (tap) / recenter (hold 0.7 s)", C::Y});
                // Y's tap (Square where the arc cannot open) is the menu: X pressed while Y is
                // held is that Square there (the presenter's y_x_combo), and only there.
                e.push_back({"square_no_arc",
                             "Use the item with its animation where the quick-item arc cannot "
                             "open (third person, quick_arc=0)",
                             C::Y, C::X, How::Combo});
            } else {
                e.push_back({"arc", "Quick-item arc (tap); held: the modifier", C::Y});
            }
            e.push_back({"sprint", "Sprint (latched until the stick is centred)", C::ClickL});
            e.push_back({"leap", "Leap (clicked again while sprinting)", C::ClickL});
            e.push_back({"gun", "Fire gun / parry; on a card: take or use the item", C::TriggerL});
            e.push_back({"take", "Quick-item arc; at the left hip: a Blood Vial", C::GripL});
        } else {
            e.push_back({"arc", "Quick-item arc", C::Y});
            e.push_back({"arc_tap", "Quick-item arc (tap); held: the modifier", C::ClickL});
            e.push_back({"jump", "Jump (while sprinting)", C::ClickL});
            e.push_back({"square", "Use the item with its animation", modifier, C::Y, How::Combo});
            e.push_back({"gun", "Fire gun / parry", C::TriggerL});
            e.push_back({"take", "Take the item the hand is on", C::GripL});
        }
        e.push_back({"lock_on", "Lock on (tap)", C::ClickR});
        e.push_back({"weapon_switch",
                     scheme2 ? "D-pad: switch weapon (left / right hand), blood bullets (up), "
                               "next item (down); locked on too"
                             : "Switch weapon (left / right hand)",
                     C::ClickR, C::StickR, How::Combo});
        e.push_back({"heavy", "Arm the next swing: heavy, charge", C::TriggerR});
        e.push_back({"transform", "Transform the weapon", C::GripR});
    }
    if (!scheme2) {
        e.push_back({"dpad", "D-pad (weapons, items)", modifier, C::StickR, How::Combo});
    }
    e.push_back({"gestures", "Gestures", modifier, C::B, How::Combo});
    // A+B: the game's roll (roll_chord, core/vr/roll_chord.h), held still the grip adjust.
    e.push_back({"grip_adjust",
                 l.classic ? "Weapon grip adjust (hold 1.2 s)"
                           : "Roll (tap) / grip adjust (hold 1.2 s)",
                 C::A, C::B, How::Chord});
    if (l.menu_runtime) {
        // SteamVR's dashboard takes the button: none of the rows below reach the game there.
        e.push_back({"runtime_menu", "The runtime's own menu (SteamVR's dashboard)", C::Menu});
    } else {
        e.push_back({"options", "Options (tap) / recenter (hold)", C::Menu});
        e.push_back({"eye_distance", "Eye distance +/- 1 mm", C::Menu, C::StickR, How::Combo});
        e.push_back({"eye_reset", "Eye distance: the headset's", C::Menu, C::ClickR, How::Combo});
        e.push_back({"stereo", "Stereo mode", C::Menu, C::StickL, How::Combo});
        e.push_back({"mark", "MARK (a numbered log line)", C::Menu, C::ClickL, How::Combo});
    }
    e.push_back({"layout", "Motion / classic (hold 1 s)", C::ClickL, C::ClickR, How::Chord});
    return e;
}

// An entry with the controls the player presses.
struct Bound {
    Entry entry;
    Control first{Control::None};
    Control second{Control::None};
};

inline std::vector<Bound> Resolve(const Layout& l) {
    std::vector<Bound> out;
    for (const Entry& e : Entries(l)) {
        out.push_back({e, Physical(e.first, l.swap),
                       e.second == Control::None ? Control::None : Physical(e.second, l.swap)});
    }
    return out;
}

// The entry `id` of the layout (nullptr-free: an entry with first None if there is none).
inline Bound Find(const Layout& l, std::string_view id) {
    for (const Bound& b : Resolve(l)) {
        if (id == b.entry.id) {
            return b;
        }
    }
    return {};
}

// "X", "hold right stick click + left stick", "A + B".
inline std::string Describe(const Bound& b) {
    switch (b.entry.how) {
    case How::Combo:
        return std::string{"hold "} + Label(b.first) + " + " + Label(b.second);
    case How::Chord:
        return std::string{Label(b.first)} + " + " + Label(b.second);
    case How::Press:
        break;
    }
    return Label(b.first);
}

// Every entry on a physical control (as either of its controls): what a callout lists.
inline std::vector<Bound> On(const Layout& l, Control physical) {
    std::vector<Bound> out;
    for (const Bound& b : Resolve(l)) {
        if (b.first == physical || b.second == physical) {
            out.push_back(b);
        }
    }
    return out;
}

// One line for the log: "move: right stick | turn: left stick | dodge: X | ...".
inline std::string Summary(const Layout& l) {
    std::string s;
    for (const Bound& b : Resolve(l)) {
        if (!s.empty()) {
            s += " | ";
        }
        s += b.entry.id;
        s += ": ";
        s += Describe(b);
    }
    return s;
}

// Text with the controls of the current layout: {move} {turn} the sticks, {move_click}
// {turn_click} their clicks, {A} {B} {X} {Y} the logical face buttons (the VR page's rows: "How
// the {turn} turns you.", "Grip adjust ({A}+{B})"). Unknown braces stay as they are.
template <typename Labeler>
inline std::string Expand(std::string_view text, bool swap, Labeler label) {
    struct Token {
        std::string_view name;
        Control logical;
    };
    static constexpr std::array<Token, 8> tokens{{{"{move}", Control::StickL},
                                                  {"{turn}", Control::StickR},
                                                  {"{move_click}", Control::ClickL},
                                                  {"{turn_click}", Control::ClickR},
                                                  {"{A}", Control::A},
                                                  {"{B}", Control::B},
                                                  {"{X}", Control::X},
                                                  {"{Y}", Control::Y}}};
    std::string out;
    out.reserve(text.size() + 16);
    std::size_t i = 0;
    while (i < text.size()) {
        bool matched = false;
        if (text[i] == '{') {
            for (const Token& t : tokens) {
                if (text.substr(i, t.name.size()) == t.name) {
                    out += label(Physical(t.logical, swap));
                    i += t.name.size();
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            out += text[i++];
        }
    }
    return out;
}

inline std::string Expand(std::string_view text, bool swap) {
    return Expand(text, swap, Label);
}

} // namespace VR::Thumbs
