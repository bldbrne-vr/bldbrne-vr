// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/vr/thumb_layout.h"
#include "core/vr/vr_page.h"
#include "core/vr/xr_bindings.h"

// "Show VR controls" (VR page > General / Controls, vr_menu=1): both controllers in line art with
// a callout per control, on three tabs (Gameplay / Menus / Gestures), lit live by what the player
// presses or touches. Header-only and pure (tests/test_controls_view.cpp).
//
// Nothing here names a button by hand. The words come from three tables:
// * what each control does in gameplay: VR::Thumbs (core/vr/thumb_layout.h), the presenter's own
//   scheme tables (motion left_scheme 1 / 2, classic, "Move with: Right stick"), read per
//   physical control with Thumbs::On(); this file only words each entry id for a callout (a
//   short title, a sub-line, a one-line help), refined by the settings the table does not model
//   (quick_arc_l3, the belt vial, melee, menu_hold, ...);
// * where each control sits on the controller in your hands: VR::XrBindings (the suggested
//   bindings of the interaction profile the runtime bound), so an Index, Vive or WMR controller
//   gets its own parts (its trackpad, its A / B, Menu on the trackpad's firm press);
// * the game menus' layout (the presenter's menu branch: A / X confirm, B / Y back, with y_menu
//   B back and Y the menu, the grips Square / Triangle, the triggers L1 / R1, either stick the
//   D-pad), which no swap changes.
namespace VR::ControlsView {

using Thumbs::Control;
using Thumbs::ControlCount;

// What the words depend on (the presenter's settings at the moment the view opens).
struct Config {
    Thumbs::Layout layout{};    // left_scheme, controls=classic, swap_sticks, inject
    bool quick_arc{true};       // quick_items=1 and quick_arc=1: the arc exists
    bool arc_l3{true};          // quick_arc_l3: the move stick's click taps the arc (scheme 1)
    bool belt_vial{true};       // left_scheme=2: the left grip at the hip draws a Blood Vial
    bool right_grip_draw{false}; // right_grip=draw: the old clutch (draw on the right grip)
    bool swings{true};          // gestures=1: swings attack (motion layout)
    bool melee_physical{true};  // melee=physical: the swing is the attack
    bool menu_hold_screen{false}; // menu_hold=screen: the hold toggles the flat screen
    bool physical_evasion{true};
    bool lean_dodge{false};
    bool grip_adjust{true};
    float snap_turn_deg{30.0f}; // 0 = smooth turning
    bool seated{false};
};

enum class Tab : std::uint8_t { Gameplay, Menus, Gestures };
inline constexpr int TabCount = 3;
inline constexpr std::array<const char*, TabCount> TabNames{"Gameplay", "Menus", "Gestures"};

// ---- The controller in your hands ---------------------------------------------------------------

enum class Family : std::uint8_t { Touch, Index, Vive, Wmr, Simple, Frame };

// The family drawn for a bound interaction profile (unknown or none: Touch, the Quest's).
inline Family FamilyOf(std::string_view profile) {
    if (profile == XrBindings::FrameProfile) {
        return Family::Frame;
    }
    if (profile.find("valve/index_controller") != std::string_view::npos) {
        return Family::Index;
    }
    if (profile.find("htc/vive_controller") != std::string_view::npos) {
        return Family::Vive;
    }
    if (profile.find("microsoft/motion_controller") != std::string_view::npos) {
        return Family::Wmr;
    }
    if (profile.find("khr/simple_controller") != std::string_view::npos) {
        return Family::Simple;
    }
    return Family::Touch; // Quest Touch / Touch Plus, HP Reverb G2, PICO: Touch's buttons
}

inline const char* FamilyName(Family f) {
    switch (f) {
    case Family::Frame:
        return "Steam Frame";
    case Family::Touch:
        return "Touch";
    case Family::Index:
        return "Index";
    case Family::Vive:
        return "Vive";
    case Family::Wmr:
        return "Windows MR";
    case Family::Simple:
        return "simple";
    }
    return "";
}

// The profile path the family's binding table is looked up by when none was bound.
inline const char* DefaultProfile(Family f) {
    switch (f) {
    case Family::Frame:
        return XrBindings::FrameProfile;
    case Family::Index:
        return "/interaction_profiles/valve/index_controller";
    case Family::Vive:
        return "/interaction_profiles/htc/vive_controller";
    case Family::Wmr:
        return "/interaction_profiles/microsoft/motion_controller";
    case Family::Simple:
        return "/interaction_profiles/khr/simple_controller";
    case Family::Touch:
        break;
    }
    return "/interaction_profiles/oculus/touch_controller";
}

// A part drawn on a controller.
enum class Part : std::uint8_t { ButtonHigh, ButtonLow, Stick, Pad, Menu, Trigger, Grip };

// How a binding sits on its part.
enum class Manner : std::uint8_t { Main, Click, Force, Up, Down, Centre, Left, Right };

struct Spot {
    bool right{false};
    Part part{Part::Menu};
    Manner manner{Manner::Main};
};

// Where a suggested binding's path sits ("/user/hand/left/input/x/click" -> left, the lower
// face button, pressed). Poses, outputs and unknown components: none.
inline std::optional<Spot> SpotOf(std::string_view path) {
    Spot p;
    constexpr std::string_view L = "/user/hand/left/input/", R = "/user/hand/right/input/";
    if (path.substr(0, L.size()) == L) {
        path.remove_prefix(L.size());
    } else if (path.substr(0, R.size()) == R) {
        path.remove_prefix(R.size());
        p.right = true;
    } else {
        return std::nullopt;
    }
    const size_t slash = path.find('/');
    const std::string_view comp = path.substr(0, slash);
    const std::string_view rest = slash == std::string_view::npos ? std::string_view{}
                                                                   : path.substr(slash + 1);
    if (comp == "thumbstick" || comp == "trackpad") {
        p.part = comp == "thumbstick" ? Part::Stick : Part::Pad;
        p.manner = rest.empty()           ? Manner::Main
                   : rest == "click"      ? Manner::Click
                   : rest == "force"      ? Manner::Force
                   : rest == "dpad_up"    ? Manner::Up
                   : rest == "dpad_down"  ? Manner::Down
                   : rest == "dpad_left"  ? Manner::Left
                   : rest == "dpad_right" ? Manner::Right
                   : rest == "dpad_center" ? Manner::Centre
                                          : Manner::Main;
        if (rest == "touch" || rest == "x" || rest == "y") {
            return std::nullopt;
        }
        return p;
    }
    if (comp == "a" || comp == "x" || comp == "dpad_down") {
        p.part = Part::ButtonLow;
    } else if (comp == "b" || comp == "y" || comp == "dpad_up") {
        p.part = Part::ButtonHigh;
    } else if (comp == "menu" || comp == "view") {
        p.part = Part::Menu;
    } else if (comp == "trigger" || comp == "select") {
        p.part = Part::Trigger;
    } else if (comp == "squeeze") {
        p.part = Part::Grip;
    } else {
        return std::nullopt; // aim / grip poses, the system button
    }
    if (rest == "touch") {
        return std::nullopt;
    }
    return p;
}

// The physical control (Quest Touch names) an action is bound to as: the presenter reads its
// actions into these before any swap.
inline Control ControlOfAct(XrBindings::Act act) {
    using A = XrBindings::Act;
    switch (act) {
    case A::Move:
        return Control::StickL;
    case A::Turn:
        return Control::StickR;
    case A::TriggerLeft:
        return Control::TriggerL;
    case A::TriggerRight:
        return Control::TriggerR;
    case A::GripLeft:
        return Control::GripL;
    case A::GripRight:
        return Control::GripR;
    case A::ButtonA:
        return Control::A;
    case A::ButtonB:
        return Control::B;
    case A::ButtonX:
        return Control::X;
    case A::ButtonY:
        return Control::Y;
    case A::StickLeft:
        return Control::ClickL;
    case A::StickRight:
        return Control::ClickR;
    case A::Menu:
        return Control::Menu;
    default:
        return Control::None;
    }
}

// The map the runtime bound for `profile` (its d-pad map when the runtime accepted it).
inline std::span<const XrBindings::Binding> BindingsOf(std::string_view profile, bool dpad) {
    for (const XrBindings::Profile& p : XrBindings::Profiles) {
        if (profile == p.path) {
            if ((p.needs & XrBindings::NeedsDpad) != 0 && !dpad && !p.fallback.empty()) {
                return p.fallback;
            }
            return p.bindings;
        }
    }
    return XrBindings::Profiles[0].bindings; // Touch
}

struct PartBinding {
    Control control; // physical (Touch names)
    Manner manner;
};

// One part drawn on a family's controller and every control bound to it.
struct DrawnPart {
    bool right{false};
    Part part{Part::Menu};
    std::string name; // "Y", "left stick", "left trackpad", "right menu button"
    std::vector<PartBinding> binds;
};

// The parts each family draws, per hand, top to bottom (the callouts' order).
inline std::vector<Part> DrawingOrder(Family f, bool right) {
    switch (f) {
    case Family::Frame:
    case Family::Touch:
        return right ? std::vector<Part>{Part::ButtonHigh, Part::ButtonLow, Part::Stick,
                                         Part::Trigger, Part::Grip}
                     : std::vector<Part>{Part::ButtonHigh, Part::ButtonLow, Part::Stick, Part::Menu,
                                         Part::Trigger, Part::Grip};
    case Family::Index:
        return {Part::ButtonHigh, Part::ButtonLow, Part::Stick, Part::Pad, Part::Trigger,
                Part::Grip};
    case Family::Vive:
        return {Part::Menu, Part::Pad, Part::Trigger, Part::Grip};
    case Family::Wmr:
        return {Part::Menu, Part::Stick, Part::Pad, Part::Trigger, Part::Grip};
    case Family::Simple:
        return {Part::Menu, Part::Trigger};
    }
    return {};
}

inline std::string PartName(Family f, bool right, Part p) {
    const std::string side = right ? "right " : "left ";
    if (f == Family::Frame && !right) {
        if (p == Part::ButtonHigh) {
            return "D-pad up";
        }
        if (p == Part::ButtonLow) {
            return "D-pad down";
        }
        if (p == Part::Menu) {
            return "View";
        }
    }
    switch (p) {
    case Part::ButtonHigh:
        return f == Family::Touch ? (right ? "B" : "Y") : (right ? "B" : "left B");
    case Part::ButtonLow:
        return f == Family::Touch ? (right ? "A" : "X") : (right ? "A" : "left A");
    case Part::Stick:
        return side + "stick";
    case Part::Pad:
        return side + "trackpad";
    case Part::Menu:
        return f == Family::Touch ? "Menu" : side + "menu button";
    case Part::Trigger:
        return f == Family::Simple ? side + "select" : side + "trigger";
    case Part::Grip:
        return f == Family::Vive || f == Family::Wmr ? side + "grip button" : side + "grip";
    }
    return {};
}

inline std::vector<DrawnPart> PartsOf(Family family, std::string_view profile, bool dpad) {
    const auto bindings = BindingsOf(profile.empty() ? DefaultProfile(family) : profile, dpad);
    std::vector<DrawnPart> out;
    for (const bool right : {false, true}) {
        for (const Part part : DrawingOrder(family, right)) {
            DrawnPart d{right, part, PartName(family, right, part), {}};
            for (const XrBindings::Binding& b : bindings) {
                const Control c = ControlOfAct(b.act);
                const auto place = SpotOf(b.path);
                if (c == Control::None || !place || place->right != right || place->part != part) {
                    continue;
                }
                d.binds.push_back({c, place->manner});
            }
            out.push_back(std::move(d));
        }
    }
    return out;
}

// The name the player knows a physical control by on this family ("Y", "left trackpad").
inline std::string NameOf(const std::vector<DrawnPart>& parts, Control physical) {
    for (const DrawnPart& p : parts) {
        int found = 0;
        Manner manner = Manner::Main;
        for (const PartBinding& b : p.binds) {
            if (b.control == physical) {
                manner = found == 0 ? b.manner : manner;
                ++found;
            }
        }
        if (found == 0) {
            continue;
        }
        if (found > 1 || manner == Manner::Click) {
            return p.part == Part::Stick || p.part == Part::Pad ? p.name + " click" : p.name;
        }
        switch (manner) {
        case Manner::Force:
            return p.name + " pressed";
        case Manner::Up:
            return p.name + " up";
        case Manner::Down:
            return p.name + " down";
        case Manner::Centre:
            return p.name + " centre";
        case Manner::Left:
            return p.name + " left";
        case Manner::Right:
            return p.name + " right";
        default:
            return p.name;
        }
    }
    return Thumbs::Label(physical);
}

// Whether the controller in your hands has a button for a physical control at all (the Vive
// wands without the d-pad extension have no Y, the simple controller no face buttons).
inline bool IsBound(const std::vector<DrawnPart>& parts, Control physical) {
    for (const DrawnPart& p : parts) {
        for (const PartBinding& b : p.binds) {
            if (b.control == physical) {
                return true;
            }
        }
    }
    return false;
}

// ---- The words ------------------------------------------------------------------------------

struct Words {
    std::string title; // the callout ("Fire gun · Parry"); empty: the entry is not shown
    std::string sub;   // its second line ("holding an item: use it")
    std::string help;  // the one-line help when it is lit or selected
};

// "Item arc" -> "item arc" for a sub-line; names ("Blood Vial") and short forms ("L3", "D-pad")
// keep their case.
inline std::string LowerFirst(std::string s) {
    if (s.size() < 2 || s.rfind("Blood", 0) == 0 ||
        !std::islower(static_cast<unsigned char>(s[1]))) {
        return s;
    }
    s[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[0])));
    return s;
}

// "left B opens ..." -> "Left B opens ...": a help line starting with a control's name.
inline std::string Capital(std::string s) {
    if (!s.empty()) {
        s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    }
    return s;
}

// The fallback for an entry this file has no words for: the table's own text up to "(".
inline Words FallbackWords(const Thumbs::Entry& e) {
    std::string t = e.what != nullptr ? e.what : e.id;
    const size_t cut = t.find(" (");
    if (cut != std::string::npos) {
        t.resize(cut);
    }
    return {t, {}, e.what != nullptr ? std::string{e.what} + "." : std::string{}};
}

// Gameplay words for one entry of the scheme table (VR::Thumbs::Entries ids). `names` names the
// player's controls (on this family) for the help lines.
inline Words EntryWords(const Thumbs::Entry& e, const Config& c,
                        const std::vector<DrawnPart>& parts) {
    const std::string_view id = e.id;
    const bool classic = c.layout.classic;
    const bool scheme2 = !classic && c.layout.left_scheme == 2;
    const bool motion = !classic;
    const auto name = [&](Control logical) {
        return NameOf(parts, Thumbs::Physical(logical, c.layout.swap));
    };
    if (id == "move") {
        return {"Move", {}, "Walk and run; the hunter keeps facing where you look."};
    }
    if (id == "turn") {
        const int deg = static_cast<int>(c.snap_turn_deg + 0.5f);
        return {"Turn", {},
                deg > 0 ? "Snap turn " + std::to_string(deg) + "\xC2\xB0. Flick down: next item. "
                                                                 "Hold up: blood bullets."
                        : "Smooth turn. Flick down: next item. Hold up: blood bullets."};
    }
    if (id == "item_next" || id == "bullets" || id == "leap" || id == "jump") {
        return {}; // told by the stick's or the click's own words
    }
    if (id == "dodge") {
        if (scheme2) {
            return {"Dodge", "sprinting: leap",
                    "Dodge. While sprinting: leap along the run, or dodge where you steer."};
        }
        return {"Dodge", "hold: sprint",
                "Tap: roll toward the stick (none: a backstep). Hold: sprint."};
    }
    if (id == "interact") {
        return {"Interact", {}, "Talk, open doors and gates, pick up items at once."};
    }
    if (id == "vial") {
        if (motion && c.layout.inject) {
            return {"Blood Vial", "then stab your wrist",
                    std::string{"Arms a vial: stab your wrist or body"} +
                        (c.seated ? " (or your lap)" : "") + " to inject. Again: put it back."};
        }
        return {"Blood Vial", {}, "Uses a Blood Vial with its animation (the game's \xE2\x96\xB3)."};
    }
    if (id == "item_use") {
        return {"Use item", {}, "Uses the selected quick item (the game's \xE2\x96\xA1)."};
    }
    if (id == "l3") {
        return {"Jump", {}, "Click while sprinting: jump. Otherwise the game's L3."};
    }
    if (id == "lock_on") {
        return {"Lock on", {},
                classic   ? "Held: lock on (the game's R3)."
                : scheme2 ? "Click: lock on. Held + left / right: switch weapons; + up: blood "
                            "bullets; + down: next item."
                          : "Click: lock on. Held + left / right: switch weapons."};
    }
    if (id == "gun") {
        if (classic) {
            return {"Fire gun \xC2\xB7 Parry", "the game's L2",
                    "Fires the gun (the game's L2). In menus: previous tab."};
        }
        if (scheme2) {
            return {"Fire gun \xC2\xB7 Parry", "on an item card: take it",
                    "Shoots where the gun points. On an item card: takes or uses it."};
        }
        return {"Fire gun \xC2\xB7 Parry", "holding an item: use it",
                "Shoots where the gun points. In menus: previous tab."};
    }
    if (id == "light") {
        return {"Light attack", {}, "The game's R1."};
    }
    if (id == "heavy") {
        if (classic) {
            return {"Heavy attack", "hold: charge", "The game's R2; hold it to charge."};
        }
        if (c.right_grip_draw) {
            return {"Attack", "with the grip: heavy",
                    "Weapon drawn (right grip): the next swing is heavy. Else the game's R1."};
        }
        if (c.melee_physical && c.swings) {
            return {"Heavy attack", "hold, then swing",
                    "Held 0.35 s: the next swing is heavy. Held 1 s: fully charged."};
        }
        return {"Heavy attack", "hold: charge", "Tap: a heavy attack. Hold to charge, then swing."};
    }
    if (id == "transform") {
        if (classic) {
            return {"Transform", {},
                    "Transforms the weapon (the game's L1). Threaded Cane / Beast Cutter: "
                    "hold the left grip 0.08 s, then release before changing back."};
        }
        if (c.right_grip_draw) {
            return {"Draw weapon", "hold: swings attack",
                    "Hold to draw the weapon: only a drawn weapon swings."};
        }
        return {"Transform", {},
                "Squeeze firmly: only the weapon moves. Threaded Cane / Beast Cutter: "
                "hold 0.08 s, then release before changing back."};
    }
    if (id == "arc") {
        if (!c.quick_arc) {
            return {"Use item", {}, "Uses the selected quick item (the game's \xE2\x96\xA1)."};
        }
        if (scheme2) {
            return {"Item arc", "hold: modifier", "Tap: the item arc."}; // + its combos
        }
        return {"Item arc", {}, "Your quick items above your left hand. Again: close it."};
    }
    if (id == "arc_tap") {
        if (!c.quick_arc || !c.arc_l3) {
            return {"Jump", {}, "Click while sprinting: jump."};
        }
        return {"Item arc", {}, "Click: the item arc, as " + name(Control::Y) +
                                    ". Sprinting: jump."};
    }
    if (id == "sprint") {
        return {"Sprint", {},
                "Click: sprint until the stick centres. Click again (or " + name(Control::A) +
                    "): leap."};
    }
    if (id == "take") {
        if (c.right_grip_draw) {
            return {"Transform", {}, "Transforms the weapon (right_grip=draw)."};
        }
        if (!c.quick_arc) {
            return {"Grip", {}, "Nothing in gameplay; in the game's menus: \xE2\x96\xA1."};
        }
        if (scheme2) {
            const bool belt = c.belt_vial && c.layout.inject;
            return {"Item arc", belt ? "at your hip: Blood Vial" : "again: close it",
                    belt ? "Opens the item arc at your hand. At your left hip: a Blood Vial."
                         : "Opens the item arc at your hand; again: closes it."};
        }
        return {"Take item", "from the arc",
                "Squeeze on a card to take it; let go mid-swing to throw it."};
    }
    if (id == "menu") {
        // y_menu (SteamVR by default: its dashboard owns Menu): Y is the menu button. Its held
        // combo (+ B: gestures) comes from ComboHelp.
        return {"Options", "hold: recenter",
                "Tap: the game's menu. Hold 0.7 s: forward is where you face; 2.5 s on a "
                "cinematic's or Tab's screen: back to VR."};
    }
    if (id == "runtime_menu") {
        // Y is the menu only with y_menu (the motion layout's left_scheme=2 and a Y button);
        // otherwise nothing opens the game's menu on this runtime (the presenter's log says so).
        if (c.layout.y_menu && scheme2) {
            return {"SteamVR", "its dashboard",
                    "The runtime's own button (SteamVR's dashboard): " + name(Control::Y) +
                        " opens the game's menu."};
        }
        return {"SteamVR", "its dashboard",
                "The runtime's own button (SteamVR's dashboard). No controller button opens the "
                "game's menu on this runtime: y_menu (vr.ini, with the motion layout's "
                "left_scheme=2) makes Y the menu button."};
    }
    if (id == "options") {
        return {"Options", c.menu_hold_screen ? "hold: flat screen" : "hold: recenter",
                c.menu_hold_screen ? "Tap: the game's menu. Hold: the flat screen."
                                   : "Tap: the game's menu. Hold 0.7 s: forward is where you face."};
    }
    // Combos and chords: on the help lines of their controls.
    if (e.how != Thumbs::How::Press) {
        if (id == "dpad") {
            return {"D-pad", {}, {}};
        }
        if (id == "gestures") {
            return {"gestures", {}, {}};
        }
        if (id == "square") {
            return {"use item", {}, {}};
        }
        if (id == "square_no_arc") {
            // y_menu: Y + X is Square only where the arc cannot open.
            return {c.quick_arc ? "use item where the arc cannot open (third person)" : "use item",
                    {}, {}};
        }
        if (id == "grip_adjust") {
            return {"grip adjust", {}, {}};
        }
        if (id == "weapon_switch" || id == "eye_distance" || id == "eye_reset" || id == "mark" ||
            id == "layout" || id == "stereo") {
            // the weapon switch: on the lock-on's help; the rest: tuning and test combos (docs)
            return {};
        }
    }
    return FallbackWords(e);
}

// ---- The callouts -----------------------------------------------------------------------------

// The pictograms of the Gestures tab.
enum class Picto : std::uint8_t {
    None,
    Swing,
    Gun,
    Vial,
    Arc,
    Throw,
    Belt,
    Evade,
    GripAdjust,
    Pointer,
};

// Lights from the hands' motion (the Gestures tab, the swing callout).
enum class Motion : std::uint8_t { None, RightSwing, LeftThrow };

struct Callout {
    std::string title;
    std::string sub;
    std::string help;
    bool right{false};      // the right column
    int part{-1};           // index into Parts(); -1 = no part (a gesture)
    std::vector<Control> controls; // physical controls that light it
    bool chord{false};      // lit (pressed) only with all of `controls` held (A + B)
    Picto picto{Picto::None};
    Motion motion{Motion::None};
};

// The help of a control that is also a modifier: its held combos ("Held + right stick: D-pad").
inline std::string ComboHelp(Control physical, const Config& c, const std::vector<DrawnPart>& parts) {
    std::string out;
    for (const Thumbs::Bound& b : Thumbs::Resolve(c.layout)) {
        if (b.entry.how != Thumbs::How::Combo || b.first != physical) {
            continue;
        }
        const Words w = EntryWords(b.entry, c, parts);
        if (w.title.empty()) {
            continue;
        }
        out += (out.empty() ? "Held + " : ", + ") + NameOf(parts, b.second) + ": " + w.title;
    }
    return out.empty() ? out : out + ".";
}

inline const char* MannerWord(Manner m) {
    switch (m) {
    case Manner::Click:
        return "click";
    case Manner::Force:
        return "press firmly";
    case Manner::Up:
        return "click up";
    case Manner::Down:
        return "click down";
    case Manner::Centre:
        return "click centre";
    case Manner::Left:
        return "click left";
    case Manner::Right:
        return "click right";
    case Manner::Main:
        break;
    }
    return "";
}

// The first entry the scheme table has for a physical control pressed on its own.
inline std::optional<std::pair<Thumbs::Bound, Words>> MainEntry(Control physical, const Config& c,
                                                                const std::vector<DrawnPart>& parts) {
    for (const Thumbs::Bound& b : Thumbs::On(c.layout, physical)) {
        if (b.entry.how != Thumbs::How::Press || b.first != physical) {
            continue;
        }
        Words w = EntryWords(b.entry, c, parts);
        if (!w.title.empty()) {
            return std::make_pair(b, std::move(w));
        }
    }
    return std::nullopt;
}

// The controls bound to one part, each once, with how it is worked there (a control on several
// d-pad regions of a trackpad: "click").
struct PartControl {
    Control control;
    Manner manner;
};
inline std::vector<PartControl> GroupBinds(const DrawnPart& d) {
    std::vector<PartControl> out;
    for (const PartBinding& b : d.binds) {
        auto it = std::find_if(out.begin(), out.end(),
                               [&](const PartControl& p) { return p.control == b.control; });
        if (it == out.end()) {
            out.push_back({b.control, b.manner});
        } else if (it->manner != Manner::Main && b.manner != it->manner) {
            it->manner = b.manner == Manner::Main ? Manner::Main : Manner::Click;
        }
    }
    // The part's own job (the stick pushed, the button pressed) first.
    std::stable_partition(out.begin(), out.end(),
                          [](const PartControl& p) { return p.manner == Manner::Main; });
    return out;
}

inline std::string Joined(const std::string& a, const std::string& b) {
    if (a.empty()) {
        return b;
    }
    return b.empty() ? a : a + " " + b;
}

// The gameplay callout of one drawn part: its main job's words; the other jobs on it (a stick's
// click, a trackpad's regions) in the sub-line; the help line tells them all, with the combos of
// a control that is also a modifier.
inline Callout GameplayCallout(const DrawnPart& d, int index, const Config& c,
                               const std::vector<DrawnPart>& parts) {
    Callout out;
    out.right = d.right;
    out.part = index;
    for (const PartBinding& b : d.binds) {
        if (std::find(out.controls.begin(), out.controls.end(), b.control) == out.controls.end()) {
            out.controls.push_back(b.control);
        }
    }
    struct Job {
        PartControl pc;
        Words words;
    };
    std::vector<Job> jobs;
    for (const PartControl& pc : GroupBinds(d)) {
        if (auto entry = MainEntry(pc.control, c, parts)) {
            jobs.push_back({pc, std::move(entry->second)});
        }
    }
    if (jobs.empty()) {
        out.title = "\xE2\x80\x94"; // "—": nothing in gameplay
        out.help = d.name + ": nothing in gameplay.";
        return out;
    }
    const Job& main = jobs.front();
    out.title = main.words.title;
    out.sub = main.words.sub;
    if (main.pc.manner != Manner::Main) {
        // Only clicked or pressed here (Menu on the Index trackpad: pressed firmly).
        out.sub = std::string{MannerWord(main.pc.manner)} +
                  (main.words.sub.empty() ? std::string{} : "; " + main.words.sub);
    }
    std::string help = Joined(main.words.help, ComboHelp(main.pc.control, c, parts));
    if (jobs.size() == 1) {
        out.help = help;
        return out;
    }
    // A part only clicked (a trackpad's regions): "click · up: item arc".
    const bool regions = main.pc.manner != Manner::Main;
    const auto region = [regions](Manner m) {
        std::string w = MannerWord(m);
        if (regions && w.rfind("click ", 0) == 0) {
            w = w.substr(6);
        }
        return w;
    };
    std::string sub = regions ? std::string{MannerWord(main.pc.manner)} : std::string{};
    std::string listed;
    for (size_t i = 1; i < jobs.size(); ++i) {
        const Job& j = jobs[i];
        const std::string piece = region(j.pc.manner) + ": " + LowerFirst(j.words.title);
        if (sub.empty() || sub.size() + piece.size() < 30) {
            sub += (sub.empty() ? "" : " \xC2\xB7 ") + piece;
        }
        listed += (listed.empty() ? "" : ", ") + piece;
    }
    out.sub = sub;
    if (jobs.size() == 2 && jobs[1].pc.manner == Manner::Click) {
        // A stick and its click: the click's own words (and its combos when it is the modifier).
        const Job& j = jobs[1];
        out.help = Joined(help, Joined(j.words.help, ComboHelp(j.pc.control, c, parts)));
    } else {
        std::string clicks = listed;
        clicks[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(clicks[0])));
        out.help = Joined(help, clicks + ".");
    }
    return out;
}

// The game's own menus (and these VR pages): one layout for every scheme, never swapped (the
// presenter's menu branch).
inline Words MenuWords(Control physical, const Config& c) {
    switch (physical) {
    case Control::A:
        return {"Confirm", "the game's \xC3\x97", "Confirms (the game's \xC3\x97). X does the same."};
    case Control::X:
        return {"Confirm", "as A", "Confirms, as A (the game's \xC3\x97)."};
    case Control::B:
        if (c.layout.y_menu) {
            return {"Back", "the game's \xE2\x97\x8B", "Goes back (the game's \xE2\x97\x8B)."};
        }
        return {"Back", "the game's \xE2\x97\x8B", "Goes back (the game's \xE2\x97\x8B). Y does the same."};
    case Control::Y:
        if (c.layout.y_menu) {
            // y_menu: Menu's jobs (the presenter's UpdateYGesture); B alone goes back.
            return {"Options", "hold: recenter",
                    "Tap: the game's Options (closes these menus; a cutscene's skip prompt). "
                    "Hold 0.7 s: recenter; 2.5 s on Tab's screen: back to VR."};
        }
        return {"Back", "as B", "Goes back, as B (the game's \xE2\x97\x8B)."};
    case Control::StickL:
    case Control::StickR:
        return {"Select", {}, "Either stick steps through the menu; held, it repeats."};
    case Control::ClickL:
        return {"L3", {}, "The game's L3."};
    case Control::ClickR:
        return {"R3", {}, "The game's R3."};
    // The VR page differs: its grips switch tabs and its triggers change the highlighted value.
    case Control::TriggerL:
        return {"Previous tab", "VR page: previous value",
                "The previous tab or page (the game's L1). On the VR page: the highlighted row's "
                "previous value."};
    case Control::TriggerR:
        return {"Next tab", "VR page: next value",
                "The next tab or page (the game's R1). On the VR page: the highlighted row's next "
                "value."};
    case Control::GripL:
        return {"\xE2\x96\xA1", "VR page: previous tab",
                "The game's \xE2\x96\xA1 (LG in its key guide). On the VR page: the previous tab."};
    case Control::GripR:
        return {"\xE2\x96\xB3", "VR page: next tab",
                "The game's \xE2\x96\xB3 (RG in its key guide). On the VR page: the next tab."};
    case Control::Menu:
        if (c.layout.menu_runtime) {
            return {"SteamVR", "its dashboard",
                    c.layout.y_menu
                        ? "The runtime's own button (SteamVR's dashboard). Y opens and closes the "
                          "game's menu, B goes back."
                        : "The runtime's own button (SteamVR's dashboard). B or Y goes back out "
                          "of the game's menus."};
        }
        return {"Close", c.menu_hold_screen ? "hold: flat screen" : "hold: recenter",
                "Tap: closes the game's menus and these pages. Hold: recenter."};
    default:
        break;
    }
    return {};
}

inline Callout MenuCallout(const DrawnPart& d, int index, const Config& c) {
    Callout out;
    out.right = d.right;
    out.part = index;
    std::string extras;
    bool have_main = false;
    for (const bool main_pass : {true, false}) {
        for (const PartBinding& b : d.binds) {
            if ((b.manner == Manner::Main) != main_pass) {
                continue;
            }
            if (std::find(out.controls.begin(), out.controls.end(), b.control) ==
                out.controls.end()) {
                out.controls.push_back(b.control);
            }
            const Words w = MenuWords(b.control, c);
            if (w.title.empty()) {
                continue;
            }
            if (!have_main) {
                have_main = true;
                out.title = w.title;
                out.sub = b.manner == Manner::Main ? w.sub
                          : w.sub.empty()          ? std::string{MannerWord(b.manner)}
                                                   : std::string{MannerWord(b.manner)} + "; " + w.sub;
                out.help = w.help;
                continue;
            }
            if (d.part == Part::Stick && b.manner == Manner::Click) {
                continue; // the stick clicks: on the help line only
            }
            const std::string piece = std::string{MannerWord(b.manner)} + ": " + LowerFirst(w.title);
            if (extras.size() + piece.size() < 30) {
                extras += (extras.empty() ? "" : " \xC2\xB7 ") + piece;
            }
        }
    }
    if (!extras.empty()) {
        out.sub = extras;
    }
    if (!have_main) {
        out.title = "\xE2\x80\x94";
        out.help = d.name + ": nothing in menus.";
    }
    return out;
}

// The Gestures tab: what the hands and body do without (or beyond) a button.
inline std::vector<Callout> GestureCallouts(const Config& c, const std::vector<DrawnPart>& parts) {
    std::vector<Callout> g;
    const bool motion = !c.layout.classic;
    const bool scheme2 = motion && c.layout.left_scheme == 2;
    const auto phys = [&](Control logical) { return Thumbs::Physical(logical, c.layout.swap); };
    const auto name = [&](Control logical) { return NameOf(parts, phys(logical)); };
    if (motion && c.swings) {
        Callout s;
        s.title = "Swing";
        if (c.melee_physical) {
            s.sub = "fast: light \xC2\xB7 wound up: heavy";
            s.help = "No button: a fast swing is light; wound up through a big arc it is heavy.";
        } else {
            s.sub = "one swing, one attack";
            s.help = "Each swing of the right controller is one attack (melee=vanilla).";
        }
        if (c.right_grip_draw) {
            s.sub = "with the right grip held";
        }
        s.controls = {Control::TriggerR};
        s.picto = Picto::Swing;
        s.motion = Motion::RightSwing;
        g.push_back(s);
    }
    {
        Callout s;
        s.title = "Aim the gun";
        s.sub = "it fires along its barrel";
        s.help = "The shot leaves along the gun's barrel at the pull, not where you face.";
        s.controls = {Control::TriggerL};
        s.picto = Picto::Gun;
        g.push_back(s);
    }
    if (motion && c.layout.inject && IsBound(parts, phys(Control::X))) {
        Callout s;
        s.title = "Blood Vial";
        s.sub = name(Control::X) + ", then stab";
        s.help = std::string{"With a vial armed, stab your wrist, chest, belly or thigh"} +
                 (c.seated ? " or lap" : "") + " to inject it.";
        s.controls = {phys(Control::X)};
        s.picto = Picto::Vial;
        g.push_back(s);
    }
    if (motion && c.quick_arc &&
        IsBound(parts, scheme2 ? Control::GripL : phys(Control::Y))) {
        Callout a;
        a.title = "Item arc";
        if (scheme2) {
            a.sub = NameOf(parts, Control::GripL) + ", reach, trigger";
            a.help = "The left grip opens your items at your hand; the trigger on a card takes it.";
            a.controls = {Control::GripL};
        } else {
            const std::string y = name(Control::Y);
            a.sub = y + (y.size() <= 6 ? ", then reach a card" : ", reach a card");
            a.help = Capital(y) + " opens your items above your hand; reach a card, squeeze "
                                  "the grip.";
            a.controls = {phys(Control::Y)};
        }
        a.picto = Picto::Arc;
        g.push_back(a);
        Callout t;
        t.title = "Throw";
        t.sub = scheme2 ? "swing, let go of the trigger" : "swing, let go of the grip";
        t.help = scheme2 ? "Hold the item with the trigger, swing, and let go of it to throw."
                         : "Hold the item with the grip, swing, and let go of it to throw.";
        t.controls = {scheme2 ? Control::TriggerL : Control::GripL};
        t.picto = Picto::Throw;
        t.motion = Motion::LeftThrow;
        g.push_back(t);
    }
    if (scheme2 && c.belt_vial && c.layout.inject && IsBound(parts, Control::GripL)) {
        Callout b;
        b.title = "Belt vial";
        b.sub = "left grip at your hip";
        b.help = std::string{"Squeeze the left grip at your left hip"} +
                 (c.seated ? " (beside the lap)" : "") + " to draw a Blood Vial.";
        b.controls = {Control::GripL};
        b.picto = Picto::Belt;
        g.push_back(b);
    }
    if (c.physical_evasion || c.lean_dodge) {
        Callout e;
        if (c.physical_evasion) {
            e.title = "Lean & duck";
            e.sub = "your body dodges";
            e.help = "Lean, step or duck: the hunter's body follows yours out of a blow.";
        } else {
            e.title = "Lean to dodge";
            e.sub = "a quick lean rolls";
            e.help = "A quick sideways or backward lean of your head dodges that way.";
        }
        e.picto = Picto::Evade;
        g.push_back(e);
    }
    if (c.grip_adjust && IsBound(parts, phys(Control::A)) && IsBound(parts, phys(Control::B))) {
        Callout a;
        a.title = "Grip adjust";
        const std::string ab = name(Control::A) + "+" + name(Control::B);
        a.sub = ab.size() <= 8 ? "hold " + ab + ", then let go" : "hold both, then let go";
        a.help = "Hold " + name(Control::A) + "+" + name(Control::B) +
                 " 1 s, hold the weapon as you like, and let go to keep it.";
        a.controls = {phys(Control::A), phys(Control::B)};
        a.chord = true;
        a.picto = Picto::GripAdjust;
        g.push_back(a);
    }
    // Two columns: the first half on the left.
    const size_t half = (g.size() + 1) / 2;
    for (size_t i = 0; i < g.size(); ++i) {
        g[i].right = i >= half;
    }
    return g;
}

// Every callout of a tab, left column first (top to bottom), then the right one.
inline std::vector<Callout> Build(Tab tab, const Config& c, const std::vector<DrawnPart>& parts) {
    std::vector<Callout> out;
    if (tab == Tab::Gestures) {
        return GestureCallouts(c, parts);
    }
    for (size_t i = 0; i < parts.size(); ++i) {
        if (parts[i].binds.empty()) {
            continue; // drawn, but nothing is bound to it (the Index's right trackpad)
        }
        out.push_back(tab == Tab::Gameplay ? GameplayCallout(parts[i], static_cast<int>(i), c, parts)
                                           : MenuCallout(parts[i], static_cast<int>(i), c));
    }
    if (tab == Tab::Gameplay && !c.layout.classic && c.swings) {
        Callout s;
        s.right = true;
        s.title = "Swing: attack";
        s.sub = c.right_grip_draw  ? "with the grip held"
                : c.melee_physical ? "no button needed"
                                   : "one swing, one blow";
        s.help = c.melee_physical
                     ? "Swing the blade: fast is light, wound up through a big arc is heavy."
                     : "Each swing of the right controller is one attack.";
        s.picto = Picto::Swing;
        s.motion = Motion::RightSwing;
        out.push_back(s);
    }
    if (tab == Tab::Menus) {
        Callout p;
        p.right = true;
        p.title = "Point";
        p.sub = "trigger clicks on VR pages";
        p.help = "On these VR pages the right controller points; its trigger clicks.";
        p.picto = Picto::Pointer;
        out.push_back(p);
    }
    std::stable_partition(out.begin(), out.end(), [](const Callout& x) { return !x.right; });
    return out;
}

// The quiet line above the help: how the page answers your hands.
inline std::string Note(Tab tab, const Config& c, Family f) {
    if (tab == Tab::Gestures) {
        return c.layout.classic ? "Swings, the vial's stab and the item arc: Motion style only."
                                : "Swing or throw, and it lights up here.";
    }
    // Touch and Index feel a thumb or finger resting on their buttons and triggers.
    return f == Family::Touch || f == Family::Index || f == Family::Frame
               ? "Press or touch any control to find it here."
               : "Press any control to find it here.";
}

// ---- The live model -----------------------------------------------------------------------------

// One frame of the controllers in your hands (physical, Quest Touch names, before any swap).
struct Live {
    std::array<bool, ControlCount> held{};    // pressed, squeezed, pushed past half
    std::array<bool, ControlCount> touched{}; // the capacitive sensors (Touch, Index)
    float right_speed{0.0f};                  // the right hand's speed, m/s
    float left_speed{0.0f};
    float dt{0.0f};                           // seconds since the last frame
};

// How lit a callout is: Touched (a soft gilt edge) or Pressed (gold, with its glow).
enum class Lit : std::uint8_t { Off, Touched, Pressed };

inline constexpr float SwingLightSpeed = 2.0f; // m/s, about a light swing's
inline constexpr float MotionLightHold = 0.8f; // s: a swing or a throw stays lit after it

class Model {
public:
    void Open(const Config& c, Family family, std::string_view profile, bool dpad,
              const Live& now) {
        config = c;
        fam = family;
        parts = PartsOf(family, profile, dpad);
        tab = Tab::Gameplay;
        hover = -1;
        Rebuild();
        focus = callouts.empty() ? -1 : 0;
        prev = now;
        prev_swing = now.right_speed >= SwingLightSpeed;
        prev_throw = now.left_speed >= SwingLightSpeed;
        dirty = true;
    }

    Tab GetTab() const {
        return tab;
    }
    Family GetFamily() const {
        return fam;
    }
    const Config& GetConfig() const {
        return config;
    }
    const std::vector<DrawnPart>& Parts() const {
        return parts;
    }
    const std::vector<Callout>& Callouts() const {
        return callouts;
    }
    const std::vector<Lit>& Lights() const {
        return lights;
    }
    int Focus() const {
        return focus;
    }
    int Hover() const {
        return hover;
    }
    bool TakeDirty() {
        const bool d = dirty;
        dirty = false;
        return d;
    }

    void TabStep(int delta) {
        SetTab(((static_cast<int>(tab) + delta) % TabCount + TabCount) % TabCount);
    }
    void SetTab(int t) {
        if (t < 0 || t >= TabCount || t == static_cast<int>(tab)) {
            return;
        }
        tab = static_cast<Tab>(t);
        Rebuild();
        focus = callouts.empty() ? -1 : 0;
        hover = -1;
        dirty = true;
    }

    // The lights and the selection from one frame of the controllers. A press, or a touch of a
    // face button or a trigger, selects that control's callout (its help line); a stick only
    // lights up (it is the page's D-pad); held and touched controls light their callouts.
    void Update(const Live& in) {
        int newly = -1;
        for (size_t i = 0; i < ControlCount; ++i) {
            const Control ctl = static_cast<Control>(i);
            const bool stick = ctl == Control::StickL || ctl == Control::StickR;
            const bool now = !stick && (in.held[i] || in.touched[i]);
            const bool was = !stick && (prev.held[i] || prev.touched[i]);
            const bool pressed_edge = !stick && in.held[i] && !prev.held[i];
            if ((now && !was) || pressed_edge) {
                const int c = CalloutOf(ctl);
                if (c >= 0) {
                    newly = c;
                }
            }
        }
        const bool swing = in.right_speed >= SwingLightSpeed;
        const bool thrown = in.left_speed >= SwingLightSpeed;
        // A swing lasts a fifth of a second: it stays lit a moment after, to be seen.
        swing_lit = swing ? MotionLightHold : std::max(0.0f, swing_lit - in.dt);
        throw_lit = thrown ? MotionLightHold : std::max(0.0f, throw_lit - in.dt);
        if (swing && !prev_swing) {
            if (const int c = CalloutOfMotion(Motion::RightSwing); c >= 0) {
                newly = c;
            }
        }
        if (thrown && !prev_throw) {
            if (const int c = CalloutOfMotion(Motion::LeftThrow); c >= 0) {
                newly = c;
            }
        }
        prev = in;
        prev_swing = swing;
        prev_throw = thrown;
        if (newly >= 0 && newly != focus) {
            focus = newly;
            dirty = true;
        }
        std::vector<Lit> lit(callouts.size(), Lit::Off);
        for (size_t k = 0; k < callouts.size(); ++k) {
            const Callout& c = callouts[k];
            bool pressed = !c.controls.empty() && c.chord;
            bool touched = false;
            for (const Control ctl : c.controls) {
                const size_t i = static_cast<size_t>(ctl);
                if (c.chord) {
                    pressed = pressed && in.held[i];
                } else {
                    pressed = pressed || in.held[i];
                }
                touched = touched || in.touched[i] || in.held[i];
            }
            if ((c.motion == Motion::RightSwing && swing_lit > 0.0f) ||
                (c.motion == Motion::LeftThrow && throw_lit > 0.0f)) {
                pressed = true;
            }
            lit[k] = pressed ? Lit::Pressed : touched ? Lit::Touched : Lit::Off;
        }
        if (lit != lights) {
            lights = std::move(lit);
            dirty = true;
        }
    }

    // The page's D-pad: up / down within a column (wrapping), left / right to the other column
    // at the same height.
    void Move(Page::Dir d) {
        if (callouts.empty()) {
            return;
        }
        if (focus < 0) {
            focus = 0;
            dirty = true;
            return;
        }
        const bool right = callouts[static_cast<size_t>(focus)].right;
        std::vector<int> col, other;
        for (int i = 0; i < static_cast<int>(callouts.size()); ++i) {
            (callouts[static_cast<size_t>(i)].right == right ? col : other).push_back(i);
        }
        const int at = static_cast<int>(std::find(col.begin(), col.end(), focus) - col.begin());
        int next = focus;
        if (d == Page::Dir::Up || d == Page::Dir::Down) {
            const int n = static_cast<int>(col.size());
            next = col[static_cast<size_t>(((at + (d == Page::Dir::Down ? 1 : -1)) % n + n) % n)];
        } else if (!other.empty() && ((d == Page::Dir::Right) != right)) {
            next = other[static_cast<size_t>(std::min(at, static_cast<int>(other.size()) - 1))];
        }
        if (next != focus) {
            focus = next;
            dirty = true;
        }
    }

    // The pointer on a callout (-1: none): it takes the selection.
    void SetHover(int callout) {
        if (callout == hover) {
            return;
        }
        hover = callout;
        if (callout >= 0 && callout < static_cast<int>(callouts.size())) {
            focus = callout;
        }
        dirty = true;
    }

    // The help line: the selected callout's.
    std::string Help() const {
        if (focus >= 0 && focus < static_cast<int>(callouts.size())) {
            return callouts[static_cast<size_t>(focus)].help;
        }
        return {};
    }

    // The callout a physical control belongs to on this tab (-1: none).
    int CalloutOf(Control ctl) const {
        for (size_t k = 0; k < callouts.size(); ++k) {
            const auto& cs = callouts[k].controls;
            if (std::find(cs.begin(), cs.end(), ctl) != cs.end()) {
                return static_cast<int>(k);
            }
        }
        return -1;
    }
    int CalloutOfMotion(Motion m) const {
        for (size_t k = 0; k < callouts.size(); ++k) {
            if (callouts[k].motion == m) {
                return static_cast<int>(k);
            }
        }
        return -1;
    }

private:
    void Rebuild() {
        callouts = Build(tab, config, parts);
        lights.assign(callouts.size(), Lit::Off);
    }

    Config config{};
    Family fam{Family::Touch};
    std::vector<DrawnPart> parts;
    Tab tab{Tab::Gameplay};
    std::vector<Callout> callouts;
    std::vector<Lit> lights;
    int focus{-1};
    int hover{-1};
    Live prev{};
    bool prev_swing{false};
    bool prev_throw{false};
    float swing_lit{0.0f}; // seconds the swing / throw stay lit
    float throw_lit{0.0f};
    bool dirty{true};
};

} // namespace VR::ControlsView
