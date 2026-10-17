// SPDX-License-Identifier: GPL-2.0-or-later

// "Show VR controls" (core/vr/controls_view.h, controls_render.cpp): the callouts come from the
// scheme tables (core/vr/thumb_layout.h) and the bound controller's map (core/vr/xr_bindings.h),
// so they follow left_scheme 1 / 2, the classic layout, "Move with: Right stick" and the
// controller family; the live model (lights, the selection, the D-pad, the pointer); the renderer
// (synthetic art).

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/vr/controls_render.h"
#include "core/vr/controls_view.h"
#include "core/vr/game_font.h"
#include "core/vr/vr_page_draw.h"

namespace {

using namespace VR::ControlsView;
using C = VR::Thumbs::Control;

constexpr const char* TouchProfile = "/interaction_profiles/oculus/touch_controller";
constexpr const char* IndexProfile = "/interaction_profiles/valve/index_controller";
constexpr const char* ViveProfile = "/interaction_profiles/htc/vive_controller";
constexpr const char* WmrProfile = "/interaction_profiles/microsoft/motion_controller";
constexpr const char* SimpleProfile = "/interaction_profiles/khr/simple_controller";

Config Layout(int scheme, bool swap, bool classic = false, bool inject = true) {
    Config c;
    c.layout.left_scheme = scheme;
    c.layout.swap = swap;
    c.layout.classic = classic;
    c.layout.inject = inject;
    return c;
}

std::vector<Config> AllLayouts() {
    std::vector<Config> out;
    for (const bool swap : {false, true}) {
        for (const bool inject : {true, false}) {
            out.push_back(Layout(1, swap, false, inject));
            out.push_back(Layout(2, swap, false, inject));
            out.push_back(Layout(1, swap, true, inject));
            // SteamVR: the runtime owns Menu; y_menu (Y is the menu) in left_scheme=2.
            Config steam = Layout(2, swap, false, inject);
            steam.layout.y_menu = steam.layout.menu_runtime = true;
            out.push_back(steam);
            Config steam1 = Layout(1, swap, false, inject);
            steam1.layout.menu_runtime = true;
            out.push_back(steam1);
        }
    }
    return out;
}

// The gameplay callout drawn on a part of the Touch controller.
const Callout* On(const std::vector<Callout>& cs, const std::vector<DrawnPart>& parts, bool right,
                  Part part) {
    for (const Callout& c : cs) {
        if (c.part >= 0 && parts[static_cast<size_t>(c.part)].right == right &&
            parts[static_cast<size_t>(c.part)].part == part) {
            return &c;
        }
    }
    return nullptr;
}

std::string Title(const std::vector<Callout>& cs, const std::vector<DrawnPart>& parts, bool right,
                  Part part) {
    const Callout* c = On(cs, parts, right, part);
    return c != nullptr ? c->title : "(none)";
}

bool Has(const std::string& s, const std::string& part) {
    return s.find(part) != std::string::npos;
}

// ---- Where the controls sit (the binding table) --------------------------------------------

TEST(ControlsView, PartsComeFromTheBindingTable) {
    // Touch: Y X stick Menu trigger grip on the left, B A stick trigger grip on the right.
    const auto touch = PartsOf(Family::Touch, TouchProfile, false);
    ASSERT_EQ(touch.size(), 11u);
    EXPECT_EQ(touch[0].name, "Y");
    EXPECT_EQ(touch[0].binds.size(), 1u);
    EXPECT_EQ(touch[0].binds[0].control, C::Y);
    // The left stick carries the move stick and its click.
    EXPECT_EQ(touch[2].part, Part::Stick);
    ASSERT_EQ(touch[2].binds.size(), 2u);
    EXPECT_EQ(touch[2].binds[0].control, C::StickL);
    EXPECT_EQ(touch[2].binds[1].control, C::ClickL);
    EXPECT_EQ(touch[2].binds[1].manner, Manner::Click);
    // Index: its left A / B carry X / Y, Menu is the left trackpad pressed firmly, the right
    // trackpad carries nothing.
    const auto index = PartsOf(Family::Index, IndexProfile, false);
    EXPECT_EQ(NameOf(index, C::X), "left A");
    EXPECT_EQ(NameOf(index, C::Y), "left B");
    EXPECT_EQ(NameOf(index, C::Menu), "left trackpad pressed");
    // Vive with the d-pad: the left pad walks, clicks up / down / centre for Y / X / the click.
    const auto vive = PartsOf(Family::Vive, ViveProfile, true);
    EXPECT_EQ(NameOf(vive, C::Y), "left trackpad up");
    EXPECT_EQ(NameOf(vive, C::X), "left trackpad down");
    EXPECT_EQ(NameOf(vive, C::ClickL), "left trackpad centre");
    // ... without it: no Y at all.
    EXPECT_FALSE(IsBound(PartsOf(Family::Vive, ViveProfile, false), C::Y));
    // WMR with the d-pad: A on four regions = a click of the right pad.
    EXPECT_EQ(NameOf(PartsOf(Family::Wmr, WmrProfile, true), C::A), "right trackpad click");
    // The touch sensors and the poses are not parts.
    EXPECT_FALSE(SpotOf("/user/hand/left/input/x/touch").has_value());
    EXPECT_FALSE(SpotOf("/user/hand/left/input/thumbstick/touch").has_value());
    EXPECT_FALSE(SpotOf("/user/hand/right/input/aim/pose").has_value());
    EXPECT_FALSE(SpotOf("/user/hand/left/output/haptic").has_value());
    EXPECT_EQ(FamilyOf("/interaction_profiles/meta/touch_controller_plus"), Family::Touch);
    EXPECT_EQ(FamilyOf(""), Family::Touch);
    EXPECT_EQ(FamilyOf(IndexProfile), Family::Index);
}

TEST(ControlsView, SettingsInstructionsUseBoundNamesWithEitherMovementHand) {
    class ControllerStore final : public VR::Page::Store {
    public:
        bool swap{};
        std::vector<DrawnPart> parts;
        std::string Get(const std::string& key) override {
            return key == "swap_sticks" && swap ? "1" : "";
        }
        void Set(const std::string&, const std::string&) override {}
        void Remove(const std::string&) override {}
        std::string ControlName(C physical) override {
            return NameOf(parts, physical);
        }
    } source;
    VR::Page::GraphicsDraftStore store(source);
    const auto& grip = VR::Page::Rows()[VR::Page::RowIndex("grip_adjust")];
    const VR::Page::Row tokens{.label = "{A}/{B}/{X}/{Y}", .help = "{X}+{Y}"};
    source.parts = PartsOf(Family::Frame, VR::XrBindings::FrameProfile, false);
    EXPECT_EQ(VR::Page::RowLabel(grip, store), "Grip adjust (A+B)");
    EXPECT_EQ(VR::Page::RowLabel(tokens, store), "A/B/D-pad down/D-pad up");
    EXPECT_EQ(VR::Page::RowHelp(tokens, store), "D-pad down+D-pad up");
    source.swap = true;
    EXPECT_EQ(VR::Page::RowLabel(grip, store), "Grip adjust (D-pad down+D-pad up)");
    EXPECT_TRUE(Has(VR::Page::RowHelp(grip, store), "Press D-pad down+D-pad up twice"));
    EXPECT_EQ(VR::Page::RowLabel(tokens, store), "D-pad down/D-pad up/A/B");
    EXPECT_EQ(VR::Page::RowHelp(tokens, store), "A+B");
    source.parts = PartsOf(Family::Touch, TouchProfile, false);
    EXPECT_EQ(VR::Page::RowLabel(grip, store), "Grip adjust (X+Y)");
    EXPECT_EQ(VR::Page::RowLabel(tokens, store), "X/Y/A/B");
    source.swap = false;
    EXPECT_EQ(VR::Page::RowLabel(grip, store), "Grip adjust (A+B)");
    EXPECT_EQ(VR::Page::RowLabel(tokens, store), "A/B/X/Y");
}

TEST(ControlsView, FrameNamesAndControlsFollowTheNativeBindings) {
    const auto profile = VR::XrBindings::FrameProfile;
    EXPECT_EQ(FamilyOf(profile), Family::Frame);
    EXPECT_STREQ(DefaultProfile(Family::Frame), profile);
    EXPECT_STREQ(FamilyName(Family::Frame), "Steam Frame");
    const auto parts = PartsOf(Family::Frame, profile, false);
    EXPECT_EQ(NameOf(parts, C::X), "D-pad down");
    EXPECT_EQ(NameOf(parts, C::Y), "D-pad up");
    EXPECT_EQ(NameOf(parts, C::Menu), "View");
    EXPECT_EQ(NameOf(parts, C::A), "A");
    EXPECT_EQ(NameOf(parts, C::B), "B");
    for (const auto control : {C::X, C::Y, C::Menu, C::A, C::B, C::ClickL, C::ClickR}) {
        EXPECT_TRUE(IsBound(parts, control));
    }
    EXPECT_FALSE(SpotOf("/user/hand/left/input/dpad_up/touch"));
    EXPECT_FALSE(SpotOf("/user/hand/left/input/system/click"));
}

// ---- What they do (the scheme tables) -------------------------------------------------------

TEST(ControlsView, EveryPartsTitleIsTheSchemeTablesFirstJob) {
    for (const Config& c : AllLayouts()) {
        const auto parts = PartsOf(Family::Touch, TouchProfile, false);
        const auto cs = Build(Tab::Gameplay, c, parts);
        for (const Callout& k : cs) {
            if (k.part < 0) {
                continue;
            }
            const DrawnPart& d = parts[static_cast<size_t>(k.part)];
            // The control pressed on its own (the stick pushed, the button pressed).
            const C main = d.binds.front().control;
            const auto entry = MainEntry(main, c, parts);
            ASSERT_TRUE(entry.has_value()) << d.name;
            EXPECT_EQ(k.title, entry->second.title) << d.name;
            // ... and the scheme table has that job on that very control.
            bool found = false;
            for (const auto& b : VR::Thumbs::On(c.layout, main)) {
                found = found || std::string{b.entry.id} == entry->first.entry.id;
            }
            EXPECT_TRUE(found) << d.name;
            // Every control of the part lights it.
            for (const PartBinding& b : d.binds) {
                EXPECT_NE(std::find(k.controls.begin(), k.controls.end(), b.control),
                          k.controls.end());
            }
        }
    }
}

TEST(ControlsView, MoveWithTheRightStickSwapsTheLabels) {
    const auto parts = PartsOf(Family::Touch, TouchProfile, false);
    for (const int scheme : {1, 2}) {
        const auto std_ = Build(Tab::Gameplay, Layout(scheme, false), parts);
        const auto swp = Build(Tab::Gameplay, Layout(scheme, true), parts);
        // Standard: the left stick walks, the right one turns; A dodges, X arms the vial.
        EXPECT_EQ(Title(std_, parts, false, Part::Stick), "Move");
        EXPECT_EQ(Title(std_, parts, true, Part::Stick), "Turn");
        EXPECT_EQ(Title(std_, parts, true, Part::ButtonLow), "Dodge");
        EXPECT_EQ(Title(std_, parts, false, Part::ButtonLow), "Blood Vial");
        EXPECT_EQ(Title(std_, parts, true, Part::ButtonHigh), "Interact");
        EXPECT_EQ(Title(std_, parts, false, Part::ButtonHigh), "Item arc");
        // Move with the right stick: the sticks and their clicks, the face buttons swap hands.
        EXPECT_EQ(Title(swp, parts, false, Part::Stick), "Turn");
        EXPECT_EQ(Title(swp, parts, true, Part::Stick), "Move");
        EXPECT_EQ(Title(swp, parts, false, Part::ButtonLow), "Dodge");
        EXPECT_EQ(Title(swp, parts, true, Part::ButtonLow), "Blood Vial");
        EXPECT_EQ(Title(swp, parts, false, Part::ButtonHigh), "Interact");
        EXPECT_EQ(Title(swp, parts, true, Part::ButtonHigh), "Item arc");
        EXPECT_EQ(On(swp, parts, false, Part::Stick)->sub, "click: lock on");
        EXPECT_EQ(On(swp, parts, true, Part::Stick)->sub,
                  scheme == 2 ? "click: sprint" : "click: item arc");
        // The triggers, grips and Menu stay with the hunter's hands.
        for (const Part p : {Part::Trigger, Part::Grip}) {
            for (const bool right : {false, true}) {
                EXPECT_EQ(Title(std_, parts, right, p), Title(swp, parts, right, p));
            }
        }
        EXPECT_EQ(Title(swp, parts, false, Part::Menu), "Options");
        // The help names the controls where they are now: the modifier (scheme 1: the move
        // stick's click, now on the right stick; scheme 2: Y, now on B) + the turn stick, now the
        // left one, is the D-pad (scheme 1); + interact, now Y, the gestures (scheme 2, whose
        // D-pad is the turn stick's click held + the stick since 2026-10-15).
        const Callout* modifier = On(swp, parts, true, scheme == 1 ? Part::Stick : Part::ButtonHigh);
        if (scheme == 1) {
            EXPECT_TRUE(Has(modifier->help, "+ left stick: D-pad")) << modifier->help;
        } else {
            EXPECT_TRUE(Has(modifier->help, "+ Y: gestures")) << modifier->help;
            EXPECT_FALSE(Has(modifier->help, "D-pad")) << modifier->help;
        }
    }
}

TEST(ControlsView, SchemeTwoGivesTheLeftHandItsNewJobs) {
    const auto parts = PartsOf(Family::Touch, TouchProfile, false);
    const auto one = Build(Tab::Gameplay, Layout(1, false), parts);
    const auto two = Build(Tab::Gameplay, Layout(2, false), parts);
    // left_scheme=1: the click taps the arc, the grip takes, the trigger uses a held item.
    EXPECT_EQ(On(one, parts, false, Part::Stick)->sub, "click: item arc");
    EXPECT_EQ(Title(one, parts, false, Part::Grip), "Take item");
    EXPECT_EQ(On(one, parts, false, Part::Trigger)->sub, "holding an item: use it");
    EXPECT_EQ(On(one, parts, true, Part::ButtonLow)->sub, "hold: sprint");
    EXPECT_TRUE(Has(On(one, parts, false, Part::Stick)->help, "+ right stick: D-pad"))
        << On(one, parts, false, Part::Stick)->help;
    // left_scheme=2: the click latches the sprint, A leaps, the grip opens the arc (the belt
    // vial at the hip), the trigger takes, Y held is the modifier.
    EXPECT_EQ(On(two, parts, false, Part::Stick)->sub, "click: sprint");
    EXPECT_EQ(On(two, parts, true, Part::ButtonLow)->sub, "sprinting: leap");
    EXPECT_EQ(Title(two, parts, false, Part::Grip), "Item arc");
    EXPECT_EQ(On(two, parts, false, Part::Grip)->sub, "at your hip: Blood Vial");
    EXPECT_EQ(On(two, parts, false, Part::Trigger)->sub, "on an item card: take it");
    EXPECT_EQ(On(two, parts, false, Part::ButtonHigh)->sub, "hold: modifier");
    const std::string yhelp = On(two, parts, false, Part::ButtonHigh)->help;
    // Hold Y + right stick (the D-pad) and hold Y + X (Square) went on 2026-10-15.
    EXPECT_FALSE(Has(yhelp, "D-pad")) << yhelp;
    EXPECT_TRUE(Has(yhelp, "+ B: gestures")) << yhelp;
    EXPECT_FALSE(Has(yhelp, "use item")) << yhelp;
    // The right stick click held + the stick: the whole D-pad (blood bullets locked on too).
    EXPECT_TRUE(Has(On(two, parts, true, Part::Stick)->help, "+ up: blood bullets"))
        << On(two, parts, true, Part::Stick)->help;
    // y_menu (SteamVR): Y is Options (hold: recenter), Menu the runtime's.
    Config ym = Layout(2, false);
    ym.layout.y_menu = ym.layout.menu_runtime = true;
    const auto yb = Build(Tab::Gameplay, ym, parts);
    EXPECT_EQ(Title(yb, parts, false, Part::ButtonHigh), "Options");
    EXPECT_EQ(On(yb, parts, false, Part::ButtonHigh)->sub, "hold: recenter");
    EXPECT_TRUE(Has(On(yb, parts, false, Part::ButtonHigh)->help, "+ B: gestures"))
        << On(yb, parts, false, Part::ButtonHigh)->help;
    EXPECT_EQ(Title(yb, parts, false, Part::Menu), "SteamVR");
    EXPECT_TRUE(Has(On(yb, parts, false, Part::Menu)->help, "Y opens the game's menu"))
        << On(yb, parts, false, Part::Menu)->help;
    EXPECT_EQ(Title(yb, parts, false, Part::Grip), "Item arc");
    const auto ymenus = Build(Tab::Menus, ym, parts);
    EXPECT_EQ(Title(ymenus, parts, false, Part::Menu), "SteamVR");
    // ... in the game's menus too Y is the menu (Options, hold: recenter) and B alone goes back.
    EXPECT_EQ(Title(ymenus, parts, false, Part::ButtonHigh), "Options");
    EXPECT_EQ(Title(ymenus, parts, true, Part::ButtonHigh), "Back");
    EXPECT_FALSE(Has(On(ymenus, parts, true, Part::ButtonHigh)->help, "Y does the same"))
        << On(ymenus, parts, true, Part::ButtonHigh)->help;
    EXPECT_TRUE(Has(On(ymenus, parts, false, Part::Menu)->help, "Y opens and closes"))
        << On(ymenus, parts, false, Part::Menu)->help;
    // Hold Y + X: Square only where the arc cannot open (y_menu), told on Y's help; with
    // quick_arc=0 (no arc anywhere) it is simply the item's use.
    EXPECT_TRUE(Has(On(yb, parts, false, Part::ButtonHigh)->help,
                    "+ X: use item where the arc cannot open"))
        << On(yb, parts, false, Part::ButtonHigh)->help;
    Config ym_noarc = ym;
    ym_noarc.quick_arc = false;
    const std::string noarc_help =
        On(Build(Tab::Gameplay, ym_noarc, parts), parts, false, Part::ButtonHigh)->help;
    EXPECT_TRUE(Has(noarc_help, "Held + X: use item,")) << noarc_help;
    // SteamVR without y_menu (left_scheme=1, classic, y_menu=off): no button opens the game's
    // menu, and the Menu callout says so (never that Y does); Y keeps its own jobs.
    for (const bool classic : {false, true}) {
        Config s1 = Layout(1, false, classic);
        s1.layout.menu_runtime = true;
        const auto sb = Build(Tab::Gameplay, s1, parts);
        const std::string mhelp = On(sb, parts, false, Part::Menu)->help;
        EXPECT_FALSE(Has(mhelp, "opens the game's menu.")) << mhelp;
        EXPECT_TRUE(Has(mhelp, "No controller button opens the game's menu")) << mhelp;
        EXPECT_NE(Title(sb, parts, false, Part::ButtonHigh), "Options");
        const auto smenus = Build(Tab::Menus, s1, parts);
        EXPECT_EQ(Title(smenus, parts, false, Part::ButtonHigh), "Back");
        EXPECT_TRUE(Has(On(smenus, parts, false, Part::Menu)->help, "B or Y goes back"))
            << On(smenus, parts, false, Part::Menu)->help;
    }
    Config s2off = Layout(2, false); // y_menu=off on SteamVR
    s2off.layout.menu_runtime = true;
    const std::string offhelp =
        On(Build(Tab::Gameplay, s2off, parts), parts, false, Part::Menu)->help;
    EXPECT_TRUE(Has(offhelp, "No controller button opens the game's menu")) << offhelp;
    // y_menu=on on Meta Link: Y and Menu are both the menu.
    Config yon = Layout(2, false);
    yon.layout.y_menu = true;
    const auto yob = Build(Tab::Gameplay, yon, parts);
    EXPECT_EQ(Title(yob, parts, false, Part::ButtonHigh), "Options");
    EXPECT_EQ(Title(yob, parts, false, Part::Menu), "Options");
    // No belt vial: the grip's second line says it closes the arc again.
    Config nobelt = Layout(2, false);
    nobelt.belt_vial = false;
    EXPECT_EQ(On(Build(Tab::Gameplay, nobelt, parts), parts, false, Part::Grip)->sub,
              "again: close it");
    // quick_arc_l3=0 (scheme 1): the click only jumps.
    Config nol3 = Layout(1, false);
    nol3.arc_l3 = false;
    EXPECT_EQ(On(Build(Tab::Gameplay, nol3, parts), parts, false, Part::Stick)->sub, "click: jump");
}

TEST(ControlsView, ClassicHasItsOwnButtonsAndNoGestures) {
    const auto parts = PartsOf(Family::Touch, TouchProfile, false);
    const auto cs = Build(Tab::Gameplay, Layout(1, false, true), parts);
    EXPECT_EQ(Title(cs, parts, true, Part::Trigger), "Light attack");
    EXPECT_EQ(Title(cs, parts, true, Part::Grip), "Heavy attack");
    EXPECT_EQ(Title(cs, parts, false, Part::Grip), "Transform");
    EXPECT_EQ(Title(cs, parts, false, Part::ButtonHigh), "Use item");
    for (const Callout& c : cs) {
        EXPECT_NE(c.picto, Picto::Swing) << c.title;
    }
    const auto g = Build(Tab::Gestures, Layout(1, false, true), parts);
    std::set<std::string> titles;
    for (const Callout& c : g) {
        titles.insert(c.title);
    }
    EXPECT_FALSE(titles.contains("Swing"));
    EXPECT_FALSE(titles.contains("Blood Vial"));
    EXPECT_FALSE(titles.contains("Item arc"));
    EXPECT_TRUE(titles.contains("Aim the gun"));
    EXPECT_TRUE(Has(Note(Tab::Gestures, Layout(1, false, true), Family::Touch), "Motion"));
}

TEST(ControlsView, MotionAndRightGripSettingsChangeTheWords) {
    const auto parts = PartsOf(Family::Touch, TouchProfile, false);
    Config c = Layout(1, false);
    EXPECT_EQ(On(Build(Tab::Gameplay, c, parts), parts, true, Part::Trigger)->sub,
              "hold, then swing");
    c.melee_physical = false;
    EXPECT_EQ(On(Build(Tab::Gameplay, c, parts), parts, true, Part::Trigger)->sub, "hold: charge");
    c = Layout(1, false);
    c.right_grip_draw = true;
    const auto draw = Build(Tab::Gameplay, c, parts);
    EXPECT_EQ(Title(draw, parts, true, Part::Grip), "Draw weapon");
    EXPECT_EQ(Title(draw, parts, false, Part::Grip), "Transform");
    c = Layout(1, false);
    c.menu_hold_screen = true;
    EXPECT_EQ(On(Build(Tab::Gameplay, c, parts), parts, false, Part::Menu)->sub,
              "hold: flat screen");
    c = Layout(1, false);
    c.snap_turn_deg = 0.0f;
    EXPECT_TRUE(Has(On(Build(Tab::Gameplay, c, parts), parts, true, Part::Stick)->help,
                    "Smooth turn"));
    // The swing callout: motion with swings on.
    const auto cs = Build(Tab::Gameplay, Layout(1, false), parts);
    EXPECT_EQ(std::count_if(cs.begin(), cs.end(),
                            [](const Callout& k) { return k.motion == Motion::RightSwing; }),
              1);
}

TEST(ControlsView, MenusKeepTheStandardLayoutWhateverTheScheme) {
    const auto parts = PartsOf(Family::Touch, TouchProfile, false);
    for (const Config& c : AllLayouts()) {
        const auto cs = Build(Tab::Menus, c, parts);
        EXPECT_EQ(Title(cs, parts, true, Part::ButtonLow), "Confirm");
        EXPECT_EQ(Title(cs, parts, false, Part::ButtonLow), "Confirm");
        EXPECT_EQ(Title(cs, parts, true, Part::ButtonHigh), "Back");
        // y_menu: the left Y is the menu there too (Options; B alone goes back).
        EXPECT_EQ(Title(cs, parts, false, Part::ButtonHigh), c.layout.y_menu ? "Options" : "Back");
        EXPECT_EQ(Title(cs, parts, false, Part::Trigger), "Previous tab");
        EXPECT_EQ(Title(cs, parts, true, Part::Trigger), "Next tab");
        EXPECT_EQ(Title(cs, parts, false, Part::Stick), "Select");
        EXPECT_EQ(Title(cs, parts, true, Part::Stick), "Select");
        // SteamVR's dashboard owns Menu: B (or Y without y_menu) backs out instead.
        EXPECT_EQ(Title(cs, parts, false, Part::Menu), c.layout.menu_runtime ? "SteamVR" : "Close");
    }
}

TEST(ControlsView, GesturesFollowTheSchemeAndTheController) {
    const auto parts = PartsOf(Family::Touch, TouchProfile, false);
    const auto find = [](const std::vector<Callout>& g, const std::string& t) -> const Callout* {
        for (const Callout& c : g) {
            if (c.title == t) {
                return &c;
            }
        }
        return nullptr;
    };
    const auto one = Build(Tab::Gestures, Layout(1, false), parts);
    ASSERT_NE(find(one, "Item arc"), nullptr);
    EXPECT_EQ(find(one, "Item arc")->sub, "Y, then reach a card");
    EXPECT_EQ(find(one, "Throw")->sub, "swing, let go of the grip");
    EXPECT_EQ(find(one, "Belt vial"), nullptr);
    const auto two = Build(Tab::Gestures, Layout(2, false), parts);
    EXPECT_EQ(find(two, "Throw")->sub, "swing, let go of the trigger");
    ASSERT_NE(find(two, "Belt vial"), nullptr);
    EXPECT_TRUE(find(two, "Belt vial")->controls == std::vector<C>{C::GripL});
    // Swapped: the vial's button and the grip adjust's are where the layout put them.
    const auto swp = Build(Tab::Gestures, Layout(1, true), parts);
    EXPECT_EQ(find(swp, "Blood Vial")->sub, "A, then stab");
    EXPECT_EQ(find(swp, "Item arc")->sub, "B, then reach a card");
    EXPECT_EQ(find(swp, "Grip adjust")->sub, "hold X+Y, then let go");
    EXPECT_TRUE(find(swp, "Grip adjust")->chord);
    // The Vive wands without the d-pad have no Y: no arc there (scheme 1).
    const auto vive = Build(Tab::Gestures, Layout(1, false), PartsOf(Family::Vive, ViveProfile, false));
    EXPECT_EQ(find(vive, "Item arc"), nullptr);
    // At most four rows per column.
    for (const Config& c : AllLayouts()) {
        const auto g = Build(Tab::Gestures, c, parts);
        EXPECT_LE(std::count_if(g.begin(), g.end(), [](const Callout& k) { return !k.right; }), 4);
        EXPECT_LE(std::count_if(g.begin(), g.end(), [](const Callout& k) { return k.right; }), 4);
    }
}

TEST(ControlsView, EveryFamilyEveryLayoutReads) {
    struct Fam {
        Family f;
        const char* profile;
        bool dpad;
    };
    const Fam fams[] = {{Family::Touch, TouchProfile, false}, {Family::Index, IndexProfile, false},
                        {Family::Vive, ViveProfile, true},    {Family::Vive, ViveProfile, false},
                        {Family::Wmr, WmrProfile, true},      {Family::Wmr, WmrProfile, false},
                        {Family::Simple, SimpleProfile, false},
                        {Family::Frame, VR::XrBindings::FrameProfile, false}};
    for (const Fam& fam : fams) {
        const auto parts = PartsOf(fam.f, fam.profile, fam.dpad);
        for (const Config& c : AllLayouts()) {
            for (int t = 0; t < TabCount; ++t) {
                const auto cs = Build(static_cast<Tab>(t), c, parts);
                EXPECT_FALSE(cs.empty());
                int left = 0, right = 0;
                for (const Callout& k : cs) {
                    (k.right ? right : left)++;
                    EXPECT_FALSE(k.title.empty()) << fam.profile;
                    EXPECT_LE(k.sub.size(), 34u) << fam.profile << " " << k.sub;
                    EXPECT_FALSE(k.help.empty()) << fam.profile << " " << k.title;
                }
                EXPECT_LE(left, 6) << fam.profile << " tab " << t;
                EXPECT_LE(right, 6) << fam.profile << " tab " << t;
            }
        }
    }
    // Index: Menu is the left trackpad pressed.
    const auto index = PartsOf(Family::Index, IndexProfile, false);
    const auto ic = Build(Tab::Gameplay, Layout(1, false), index);
    EXPECT_EQ(Title(ic, index, false, Part::Pad), "Options");
    EXPECT_TRUE(Has(On(ic, index, false, Part::Pad)->sub, "press")) << On(ic, index, false, Part::Pad)->sub;
    // Vive (d-pad): the left trackpad walks and its clicks are listed.
    const auto vive = PartsOf(Family::Vive, ViveProfile, true);
    const auto vc = Build(Tab::Gameplay, Layout(1, false), vive);
    EXPECT_EQ(Title(vc, vive, false, Part::Pad), "Move");
    EXPECT_TRUE(Has(On(vc, vive, false, Part::Pad)->help, "click up: item arc"));
    // The simple controller: the select buttons and Menu.
    const auto simple = PartsOf(Family::Simple, SimpleProfile, false);
    const auto sc = Build(Tab::Gameplay, Layout(1, false), simple);
    EXPECT_EQ(Title(sc, simple, false, Part::Trigger), "Fire gun \xC2\xB7 Parry");
    EXPECT_EQ(Title(sc, simple, true, Part::Menu), "Options");
}

// ---- The live model ---------------------------------------------------------------------------

Live Pressed(std::initializer_list<C> held, std::initializer_list<C> touched = {}) {
    Live l;
    for (const C c : held) {
        l.held[static_cast<size_t>(c)] = true;
    }
    for (const C c : touched) {
        l.touched[static_cast<size_t>(c)] = true;
    }
    return l;
}

int CalloutTitled(const Model& m, const std::string& t) {
    for (size_t i = 0; i < m.Callouts().size(); ++i) {
        if (m.Callouts()[i].title == t) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

TEST(ControlsViewModel, WhatIsHeldAsItOpensLightsButDoesNotSelect) {
    Model m;
    m.Open(Layout(1, false), Family::Touch, TouchProfile, false, Pressed({C::A}));
    m.Update(Pressed({C::A}));
    EXPECT_EQ(m.Focus(), 0); // the first callout (Y), not A's
    const int dodge = CalloutTitled(m, "Dodge");
    ASSERT_GE(dodge, 0);
    EXPECT_EQ(m.Lights()[static_cast<size_t>(dodge)], Lit::Pressed);
    m.Update(Live{});
    EXPECT_EQ(m.Lights()[static_cast<size_t>(dodge)], Lit::Off);
}

TEST(ControlsViewModel, PressesAndTouchesSelectSticksOnlyLight) {
    Model m;
    m.Open(Layout(1, false), Family::Touch, TouchProfile, false, Live{});
    // A finger onto the left trigger: lit softly and selected.
    m.Update(Pressed({}, {C::TriggerL}));
    const int gun = CalloutTitled(m, "Fire gun \xC2\xB7 Parry");
    EXPECT_EQ(m.Focus(), gun);
    EXPECT_EQ(m.Lights()[static_cast<size_t>(gun)], Lit::Touched);
    EXPECT_EQ(m.Help(), "Shoots where the gun points. In menus: previous tab.");
    // Pulled: lit fully.
    m.Update(Pressed({C::TriggerL}, {C::TriggerL}));
    EXPECT_EQ(m.Lights()[static_cast<size_t>(gun)], Lit::Pressed);
    // A thumb on the right stick (or pushing it: the page's D-pad) lights it, keeps the selection.
    m.Update(Pressed({C::StickR}, {C::TriggerL, C::StickR}));
    const int turn = CalloutTitled(m, "Turn");
    EXPECT_EQ(m.Focus(), gun);
    EXPECT_EQ(m.Lights()[static_cast<size_t>(turn)], Lit::Pressed);
    // Its click selects it.
    m.Update(Pressed({C::ClickR}, {C::StickR}));
    EXPECT_EQ(m.Focus(), turn);
    // A resting finger (no new touch) selects nothing new.
    m.Update(Pressed({}, {C::StickR}));
    EXPECT_EQ(m.Focus(), turn);
    // The thumb onto B: Interact.
    m.Update(Pressed({}, {C::B}));
    EXPECT_EQ(m.Focus(), CalloutTitled(m, "Interact"));
}

TEST(ControlsViewModel, SwingAndChordsLightTheirGestures) {
    Model m;
    m.Open(Layout(1, false), Family::Touch, TouchProfile, false, Live{});
    Live fast;
    fast.right_speed = 3.0f;
    m.Update(fast);
    const int swing = CalloutTitled(m, "Swing: attack");
    EXPECT_EQ(m.Focus(), swing);
    EXPECT_EQ(m.Lights()[static_cast<size_t>(swing)], Lit::Pressed);
    // A swing is a fifth of a second: it stays lit 0.8 s after it, to be seen.
    Live after;
    after.dt = 0.5f;
    m.Update(after);
    EXPECT_EQ(m.Lights()[static_cast<size_t>(swing)], Lit::Pressed);
    m.Update(after);
    EXPECT_EQ(m.Lights()[static_cast<size_t>(swing)], Lit::Off);
    m.SetTab(static_cast<int>(Tab::Gestures));
    const int adjust = CalloutTitled(m, "Grip adjust");
    m.Update(Pressed({C::A}));
    EXPECT_EQ(m.Lights()[static_cast<size_t>(adjust)], Lit::Touched); // one of the two
    m.Update(Pressed({C::A, C::B}));
    EXPECT_EQ(m.Lights()[static_cast<size_t>(adjust)], Lit::Pressed);
    Live thrown;
    thrown.left_speed = 4.0f;
    m.Update(thrown);
    EXPECT_EQ(m.Focus(), CalloutTitled(m, "Throw"));
}

TEST(ControlsViewModel, TabsAndTheDpad) {
    Model m;
    m.Open(Layout(1, false), Family::Touch, TouchProfile, false, Live{});
    EXPECT_EQ(m.GetTab(), Tab::Gameplay);
    m.TabStep(-1);
    EXPECT_EQ(m.GetTab(), Tab::Gestures); // wraps like the page's tabs
    m.TabStep(1);
    EXPECT_EQ(m.GetTab(), Tab::Gameplay);
    // The left grip squeezed steps the tab (the runtime) and lights on the new one; here the
    // left trigger's callout, lit the same way.
    m.TabStep(1);
    m.Update(Pressed({C::TriggerL}));
    EXPECT_EQ(m.Callouts()[static_cast<size_t>(m.Focus())].title, "Previous tab");
    // The D-pad: down the left column, wrapping; right to the right column at the same row.
    m.SetTab(0);
    EXPECT_EQ(m.Focus(), 0);
    m.Move(VR::Page::Dir::Down);
    EXPECT_EQ(m.Focus(), 1);
    m.Move(VR::Page::Dir::Up);
    m.Move(VR::Page::Dir::Up);
    EXPECT_EQ(m.Focus(), 5); // the left column's last
    m.Move(VR::Page::Dir::Right);
    EXPECT_TRUE(m.Callouts()[static_cast<size_t>(m.Focus())].right);
    EXPECT_EQ(m.Focus(), 11);
    m.Move(VR::Page::Dir::Left);
    EXPECT_EQ(m.Focus(), 5);
    // The pointer takes the selection.
    m.SetHover(7);
    EXPECT_EQ(m.Focus(), 7);
    EXPECT_TRUE(m.TakeDirty());
}

// ---- The renderer (synthetic art) -------------------------------------------------------------

TEST(ControlsViewRender, DrawsTheControllersLitAndTheSelectionGlow) {
    std::vector<u8> grey(64 * 64 * 4, 128);
    const VR::Page::Texture t{64, 64, grey.data()};
    VR::GameText::GameFont font;
    font.tag = {0x00, 0x00};
    font.table = 0;
    font.ascent = 18020;
    font.descent = 2460;
    for (u32 c = 32; c < 127; ++c) {
        font.glyph[c] = 0;
    }
    font.offsets = {0};
    font.advance = {10240};
    VR::Page::Assets assets{t, t, t, t, &font};
    Model m;
    m.Open(Layout(1, false), Family::Touch, TouchProfile, false, Live{});
    m.Update(Pressed({C::TriggerL}));
    View v = MakeView(m, VR::Page::Glyphs{});
    ASSERT_EQ(v.tabs.size(), 3u);
    EXPECT_EQ(v.keys.size(), 3u);
    VR::Page::Image page, glow;
    ASSERT_TRUE(Render(assets, v, page, glow));
    EXPECT_EQ(page.width, VR::Page::PageWidth);
    ASSERT_EQ(glow.rgba.size(), page.rgba.size());
    // The lit trigger (the left controller, inside the mock's trigger paddle at (106, 148), the
    // right controller's drawing mirrored): gold.
    const float cx = VR::Page::Draw::Canvas::ToX(VR::Page::Draw::TitleCentreX);
    const int tx = static_cast<int>(cx - 12.0f - 240.0f + (240.0f - 106.0f));
    const int ty = static_cast<int>(227.3f + 148.0f);
    const u8* px = &page.rgba[(static_cast<size_t>(ty) * page.width + tx) * 4];
    EXPECT_GT(px[0], px[2] + 40) << int(px[0]) << " " << int(px[2]); // warm
    // The right trigger: not lit (dark).
    const int rx = static_cast<int>(cx + 12.0f + 106.0f);
    const u8* rp = &page.rgba[(static_cast<size_t>(ty) * page.width + rx) * 4];
    EXPECT_LT(rp[0], 80);
    // The glow: behind the selected callout only (the left column's 5th row).
    long left_glow = 0;
    for (int y = 0; y < page.height; ++y) {
        for (int x = 0; x < static_cast<int>(cx); ++x) {
            left_glow += glow.rgba[(static_cast<size_t>(y) * page.width + x) * 4];
        }
    }
    EXPECT_GT(left_glow, 1000);
    // Hit tests: every callout has a box; the selected one is found at its text.
    const auto boxes = HitBoxes(v, &font);
    EXPECT_EQ(std::count_if(boxes.begin(), boxes.end(),
                            [](const HitBox& b) { return b.kind == HitBox::Kind::Callout; }),
              static_cast<long>(v.callouts.size()));
    EXPECT_EQ(HitTest(boxes, 10.0f, 10.0f), nullptr);
    // Every family draws.
    for (const Family f : {Family::Index, Family::Vive, Family::Wmr, Family::Simple, Family::Frame}) {
        Model mf;
        mf.Open(Layout(2, true), f, DefaultProfile(f), true, Live{});
        mf.Update(Pressed({C::GripL, C::TriggerR}));
        EXPECT_TRUE(Render(assets, MakeView(mf, VR::Page::Glyphs{}), page, glow));
    }
    EXPECT_FALSE(Render(VR::Page::Assets{}, v, page, glow));
}

// The panel under the controllers is the page's exactly: with solid art its middle is opaque (the
// world never shows through: the VR page's flicker fix of 2026-10-09), and with
// VR_PAGE_SEE_THROUGH=1 the game's 88% like the page's.
TEST(ControlsViewRender, ThePanelIsThePagesOpaqueOne) {
    std::vector<u8> solid(64 * 64 * 4, 128);
    for (size_t i = 3; i < solid.size(); i += 4) {
        solid[i] = 255;
    }
    const VR::Page::Texture t{64, 64, solid.data()};
    VR::GameText::GameFont font;
    font.tag = {0x00, 0x00};
    font.table = 0;
    font.ascent = 18020;
    font.descent = 2460;
    for (u32 c = 32; c < 127; ++c) {
        font.glyph[c] = 0;
    }
    font.offsets = {0};
    font.advance = {10240};
    VR::Page::Assets assets{t, t, t, t, &font};
    Model m;
    m.Open(Layout(1, false), Family::Touch, TouchProfile, false, Live{});
    VR::Page::Image page, glow;
    ASSERT_TRUE(Render(assets, MakeView(m, VR::Page::Glyphs{}), page, glow));
    // Inside the frame's left soft edge pieces, away from the art and the text: (430, 880).
    const int x = static_cast<int>(VR::Page::Draw::Canvas::ToX(430.0f));
    const int y = static_cast<int>(VR::Page::Draw::Canvas::ToY(880.0f));
    const u8 a = page.rgba[(static_cast<size_t>(y) * page.width + x) * 4 + 3];
    if (VR::Page::Draw::PanelOpaque()) {
        EXPECT_EQ(a, 255);
    } else {
        EXPECT_LT(a, 250);
        EXPECT_GT(a, 200);
    }
}

} // namespace
