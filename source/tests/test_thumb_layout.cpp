// SPDX-License-Identifier: GPL-2.0-or-later

// The controls' scheme tables and "Move with: Right stick" (core/vr/thumb_layout.h): the swap
// itself, the rules of design section 4 for every layout (motion left_scheme 1 and 2, classic),
// the exact tables the player gets with the swap, the labels the VR page and the log take from
// them, and the controllers' state read into its logical slots as the presenter does, driving the
// real stick-click and sprint-latch machines (core/vr/stick_click.h, core/vr/left_locomotion.h).

#include <array>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/vr/left_locomotion.h"
#include "core/vr/stick_click.h"
#include "core/vr/thumb_layout.h"
#include "core/vr/vr_page.h"

namespace {

using namespace VR::Thumbs;
using C = Control;

const std::array<C, ControlCount> AllControls{C::A,      C::B,        C::X,        C::Y,
                                               C::StickL, C::StickR,   C::ClickL,   C::ClickR,
                                               C::TriggerL, C::TriggerR, C::GripL, C::GripR,
                                               C::Menu};

// Every layout the presenter can be in.
std::vector<Layout> AllLayouts() {
    std::vector<Layout> out;
    for (const bool swap : {false, true}) {
        for (const bool inject : {true, false}) {
            out.push_back({1, false, swap, inject});
            out.push_back({2, false, swap, inject});
            out.push_back({2, false, swap, inject, true, true}); // y_menu (SteamVR)
            out.push_back({2, false, swap, inject, true});       // y_menu=on (Meta Link)
            out.push_back({1, false, swap, inject, false, true}); // scheme 1 on SteamVR
            out.push_back({1, true, swap, inject});
        }
    }
    return out;
}

std::string Name(const Layout& l) {
    return std::string{l.classic ? "classic" : (l.left_scheme == 2 ? "scheme 2" : "scheme 1")} +
           (l.swap ? ", move with the right stick" : ", move with the left stick") +
           (l.inject ? "" : ", inject=0") + (l.y_menu ? ", y_menu" : "") +
           (l.menu_runtime ? ", SteamVR's Menu" : "");
}

bool IsThumb(C c) {
    return c == C::A || c == C::B || c == C::X || c == C::Y || c == C::StickL || c == C::StickR ||
           c == C::ClickL || c == C::ClickR;
}

// The stick a click belongs to.
C StickOf(C click) {
    return click == C::ClickL ? C::StickL : click == C::ClickR ? C::StickR : C::None;
}

bool Lower(C c) {
    return c == C::A || c == C::X;
}

// ---- The swap itself ------------------------------------------------------------------------

TEST(ThumbLayout, NoSwapIsTheIdentity) {
    for (const C c : AllControls) {
        EXPECT_EQ(Physical(c, false), c) << Label(c);
    }
}

TEST(ThumbLayout, SwapIsItsOwnInverseAndAPermutation) {
    std::map<C, int> hits;
    for (const C c : AllControls) {
        EXPECT_EQ(Physical(Physical(c, true), true), c) << Label(c);
        ++hits[Physical(c, true)];
    }
    EXPECT_EQ(hits.size(), ControlCount); // no two controls on one
}

TEST(ThumbLayout, OnlyTheThumbsChangeHands) {
    for (const C c : AllControls) {
        const C p = Physical(c, true);
        if (IsThumb(c)) {
            EXPECT_NE(HandOf(p), HandOf(c)) << Label(c) << " -> " << Label(p);
            EXPECT_TRUE(IsThumb(p)) << Label(c);
        } else {
            // The triggers and grips stay with the hunter's hands, Menu on the left.
            EXPECT_EQ(p, c) << Label(c);
        }
    }
    EXPECT_EQ(HandOf(Physical(C::Menu, true)), Hand::Left);
}

TEST(ThumbLayout, FaceButtonsKeepTheirRowAndSticksTakeTheirClicks) {
    for (const C c : {C::A, C::B, C::X, C::Y}) {
        EXPECT_EQ(Lower(Physical(c, true)), Lower(c)) << Label(c);
    }
    for (const C click : {C::ClickL, C::ClickR}) {
        EXPECT_EQ(StickOf(Physical(click, true)), Physical(StickOf(click), true)) << Label(click);
    }
    EXPECT_EQ(Physical(C::A, true), C::X);
    EXPECT_EQ(Physical(C::B, true), C::Y);
    EXPECT_EQ(Physical(C::X, true), C::A);
    EXPECT_EQ(Physical(C::Y, true), C::B);
    EXPECT_EQ(Physical(C::StickL, true), C::StickR);
    EXPECT_EQ(Physical(C::ClickL, true), C::ClickR);
    EXPECT_EQ(HandFor(C::StickL, true), Hand::Right);
    EXPECT_EQ(HandFor(C::StickL, false), Hand::Left);
}

// ---- Design section 4, in every layout --------------------------------------------------------

TEST(ThumbLayout, DesignRulesHoldInEveryLayout) {
    for (const Layout& l : AllLayouts()) {
        SCOPED_TRACE(Name(l));
        const Bound move = Find(l, "move");
        const Bound turn = Find(l, "turn");
        const Bound dodge = Find(l, "dodge");
        const Bound interact = Find(l, "interact");
        const Bound vial = Find(l, "vial");
        const Bound lock_on = Find(l, "lock_on");
        const Bound dpad = Find(l, "dpad");
        const Bound gestures = Find(l, "gestures");
        const Bound adjust = Find(l, "grip_adjust");
        ASSERT_NE(move.first, C::None);
        ASSERT_NE(dodge.first, C::None);
        const Hand move_hand = HandOf(move.first);
        // The stick that walks.
        EXPECT_EQ(move.first, l.swap ? C::StickR : C::StickL);
        EXPECT_EQ(turn.first, l.swap ? C::StickL : C::StickR);
        EXPECT_EQ(Find(l, "item_next").first, turn.first);
        EXPECT_EQ(Find(l, "bullets").first, turn.first);
        // You can hold the sprint and steer: dodge / sprint on the other thumb, interact with it.
        EXPECT_NE(HandOf(dodge.first), move_hand);
        EXPECT_EQ(HandOf(interact.first), HandOf(dodge.first));
        EXPECT_TRUE(Lower(dodge.first));
        EXPECT_FALSE(Lower(interact.first));
        // The vial and the item button on the move stick's hand.
        EXPECT_EQ(HandOf(vial.first), move_hand);
        const bool scheme2 = !l.classic && l.left_scheme == 2;
        // Lock-on on the turn stick's click; the D-pad on the turn stick (scheme 2: its click
        // held + the stick, the weapon switch's; hold Y + right stick is gone).
        EXPECT_EQ(StickOf(lock_on.first), turn.first);
        if (scheme2) {
            EXPECT_EQ(dpad.first, C::None);
            EXPECT_EQ(Find(l, "square").first, C::None); // hold Y + X is gone too
            EXPECT_EQ(Find(l, "weapon_switch").second, turn.first);
        } else {
            EXPECT_EQ(dpad.second, turn.first);
        }
        EXPECT_EQ(gestures.second, interact.first);
        // The grip adjust follows A and B.
        EXPECT_EQ(adjust.first, dodge.first);
        EXPECT_EQ(adjust.second, interact.first);
        // The move stick's click: the arc tap / jump (scheme 1, classic: L3) or the latch (2).
        for (const char* id : {"arc_tap", "jump", "sprint", "leap", "l3"}) {
            const Bound b = Find(l, id);
            if (b.first != C::None) {
                EXPECT_EQ(StickOf(b.first), move.first) << id;
            }
        }
        // The modifier's combos use one control of each... modifier the same in all of them.
        if (!scheme2) {
            EXPECT_EQ(dpad.first, gestures.first);
        }
        if (!l.classic) {
            const Bound sw = Find(l, "weapon_switch");
            EXPECT_EQ(sw.first, lock_on.first);
            EXPECT_EQ(sw.second, turn.first);
            // The modifier is on the move stick's hand (scheme 1: its click; scheme 2: the upper
            // button there), the button it modifies on the other.
            EXPECT_EQ(HandOf(gestures.first), move_hand);
            EXPECT_NE(HandOf(gestures.second), move_hand);
            if (!scheme2) {
                EXPECT_EQ(Find(l, "square").first, dpad.first);
                EXPECT_NE(HandOf(dpad.second), move_hand);
            }
        }
        if (l.y_menu && scheme2) {
            // y_menu: the logical Y (the move stick's hand's upper button) is the menu button,
            // also the gestures' modifier; the left grip opens the arc.
            EXPECT_EQ(Find(l, "menu").first, Physical(C::Y, l.swap));
            EXPECT_EQ(gestures.first, Physical(C::Y, l.swap));
            EXPECT_EQ(Find(l, "arc").first, C::None);
            EXPECT_EQ(Find(l, "take").first, C::GripL);
        } else {
            EXPECT_EQ(Find(l, "menu").first, C::None);
        }
        if (l.menu_runtime) {
            // The runtime owns Menu (SteamVR's dashboard): one row says so, nothing of ours.
            const auto on_menu = On(l, C::Menu);
            ASSERT_EQ(on_menu.size(), 1u);
            EXPECT_STREQ(on_menu.front().entry.id, "runtime_menu");
            EXPECT_EQ(Find(l, "options").first, C::None);
            continue;
        }
        // The Menu combos follow the logical sticks; Menu stays on the left.
        EXPECT_EQ(Find(l, "options").first, C::Menu);
        EXPECT_EQ(Find(l, "eye_distance").second, turn.first);
        EXPECT_EQ(Find(l, "stereo").second, move.first);
        EXPECT_EQ(Find(l, "mark").second, Physical(C::ClickL, l.swap));
        EXPECT_EQ(Find(l, "eye_reset").second, Physical(C::ClickR, l.swap));
    }
}

// y_menu outside left_scheme=2 changes nothing (the presenter never sets it there either). The
// runtime's Menu changes only Menu's rows.
TEST(ThumbLayout, YMenuIsLeftSchemeTwosOnly) {
    for (const Layout& l : AllLayouts()) {
        Layout ours = l;
        ours.menu_runtime = false;
        std::vector<std::string> a, b;
        for (const Bound& x : Resolve(ours)) {
            if (x.first != C::Menu) {
                a.push_back(x.entry.id);
            }
        }
        for (const Bound& x : Resolve(l)) {
            if (x.first != C::Menu) {
                b.push_back(x.entry.id);
            }
        }
        EXPECT_EQ(a, b) << Name(l);
    }
    for (const bool swap : {false, true}) {
        for (const bool classic : {false, true}) {
            Layout plain{1, classic, swap, true};
            Layout y = plain;
            y.y_menu = true;
            EXPECT_EQ(Summary(plain), Summary(y));
        }
    }
}

TEST(ThumbLayout, HandBoundControlsNeverMove) {
    for (const Layout& l : AllLayouts()) {
        SCOPED_TRACE(Name(l));
        Layout plain = l;
        plain.swap = false;
        const auto a = Resolve(plain);
        const auto b = Resolve(l);
        ASSERT_EQ(a.size(), b.size());
        for (size_t i = 0; i < a.size(); ++i) {
            EXPECT_STREQ(a[i].entry.id, b[i].entry.id);
            if (!IsThumb(a[i].first)) {
                EXPECT_EQ(a[i].first, b[i].first) << a[i].entry.id;
            }
            if (a[i].second != C::None && !IsThumb(a[i].second)) {
                EXPECT_EQ(a[i].second, b[i].second) << a[i].entry.id;
            }
        }
        for (const char* id : {"gun", "heavy", "transform", "take", "light"}) {
            const Bound x = Find(l, id);
            if (x.first != C::None) {
                EXPECT_FALSE(IsThumb(x.first)) << id;
            }
        }
    }
}

TEST(ThumbLayout, CombosKeepTheirThumbsApart) {
    // A combo of two thumb controls on different hands before the swap is on different hands
    // after it, and one on a single thumb stays on a single thumb (the same reach as before).
    for (const Layout& l : AllLayouts()) {
        SCOPED_TRACE(Name(l));
        Layout plain = l;
        plain.swap = false;
        const auto a = Resolve(plain);
        const auto b = Resolve(l);
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i].second == C::None) {
                continue;
            }
            EXPECT_NE(b[i].first, b[i].second) << a[i].entry.id;
            if (IsThumb(a[i].first) && IsThumb(a[i].second)) {
                EXPECT_EQ(HandOf(a[i].first) == HandOf(a[i].second),
                          HandOf(b[i].first) == HandOf(b[i].second))
                    << a[i].entry.id;
            }
        }
    }
}

TEST(ThumbLayout, NoControlDoesTwoUnrelatedPresses) {
    // The single presses of a layout: a thumb button carries one job (a stick's click may carry
    // its scheme's tap / jump / latch, which the click's own machine tells apart).
    for (const Layout& l : AllLayouts()) {
        SCOPED_TRACE(Name(l));
        std::map<C, std::vector<std::string>> on;
        for (const Bound& b : Resolve(l)) {
            if (b.entry.how == How::Press && (b.first == C::A || b.first == C::B ||
                                              b.first == C::X || b.first == C::Y)) {
                on[b.first].push_back(b.entry.id);
            }
        }
        for (const auto& [control, ids] : on) {
            EXPECT_EQ(ids.size(), 1u) << Label(control) << ": " << ids.front();
        }
        EXPECT_EQ(on.size(), 4u);
    }
}

// ---- The exact tables -------------------------------------------------------------------------

std::map<std::string, std::string> Table(const Layout& l) {
    std::map<std::string, std::string> t;
    for (const Bound& b : Resolve(l)) {
        t[b.entry.id] = Describe(b);
    }
    return t;
}

TEST(ThumbLayout, SchemeOneWithTheRightStick) {
    const auto t = Table({1, false, true, true});
    EXPECT_EQ(t.at("move"), "right stick");
    EXPECT_EQ(t.at("turn"), "left stick");
    EXPECT_EQ(t.at("item_next"), "left stick");
    EXPECT_EQ(t.at("bullets"), "left stick");
    EXPECT_EQ(t.at("dodge"), "X");
    EXPECT_EQ(t.at("interact"), "Y");
    EXPECT_EQ(t.at("vial"), "A");
    EXPECT_EQ(t.at("arc"), "B");
    EXPECT_EQ(t.at("arc_tap"), "right stick click");
    EXPECT_EQ(t.at("jump"), "right stick click");
    EXPECT_EQ(t.at("lock_on"), "left stick click");
    EXPECT_EQ(t.at("weapon_switch"), "hold left stick click + left stick");
    EXPECT_EQ(t.at("dpad"), "hold right stick click + left stick");
    EXPECT_EQ(t.at("gestures"), "hold right stick click + Y");
    EXPECT_EQ(t.at("square"), "hold right stick click + B");
    EXPECT_EQ(t.at("grip_adjust"), "X + Y");
    EXPECT_EQ(t.at("gun"), "left trigger");
    EXPECT_EQ(t.at("heavy"), "right trigger");
    EXPECT_EQ(t.at("transform"), "right grip");
    EXPECT_EQ(t.at("take"), "left grip");
    EXPECT_EQ(t.at("options"), "Menu");
    EXPECT_EQ(t.at("eye_distance"), "hold Menu + left stick");
    EXPECT_EQ(t.at("eye_reset"), "hold Menu + left stick click");
    EXPECT_EQ(t.at("stereo"), "hold Menu + right stick");
    EXPECT_EQ(t.at("mark"), "hold Menu + right stick click");
    EXPECT_EQ(t.at("layout"), "right stick click + left stick click");
}

TEST(ThumbLayout, SchemeOneStandard) {
    const auto t = Table({1, false, false, true});
    EXPECT_EQ(t.at("move"), "left stick");
    EXPECT_EQ(t.at("dodge"), "A");
    EXPECT_EQ(t.at("interact"), "B");
    EXPECT_EQ(t.at("vial"), "X");
    EXPECT_EQ(t.at("arc"), "Y");
    EXPECT_EQ(t.at("arc_tap"), "left stick click");
    EXPECT_EQ(t.at("lock_on"), "right stick click");
    EXPECT_EQ(t.at("dpad"), "hold left stick click + right stick");
    EXPECT_EQ(t.at("gestures"), "hold left stick click + B");
    EXPECT_EQ(t.at("square"), "hold left stick click + Y");
    EXPECT_EQ(t.at("grip_adjust"), "A + B");
    EXPECT_EQ(t.at("eye_distance"), "hold Menu + right stick");
    EXPECT_EQ(t.at("stereo"), "hold Menu + left stick");
    EXPECT_EQ(t.at("mark"), "hold Menu + left stick click");
}

TEST(ThumbLayout, SchemeTwoWithTheRightStick) {
    const auto t = Table({2, false, true, true});
    EXPECT_EQ(t.at("move"), "right stick");
    EXPECT_EQ(t.at("sprint"), "right stick click"); // the latch on the stick that walks
    EXPECT_EQ(t.at("leap"), "right stick click");
    EXPECT_EQ(t.at("dodge"), "X"); // the leap / dodge while sprinting, on the other thumb
    EXPECT_EQ(t.at("interact"), "Y");
    EXPECT_EQ(t.at("vial"), "A");
    EXPECT_EQ(t.at("arc"), "B"); // its tap the arc, held the modifier
    EXPECT_EQ(t.count("dpad"), 0u);   // hold Y + right stick: gone (2026-10-15)
    EXPECT_EQ(t.count("square"), 0u); // hold Y + X: gone
    EXPECT_EQ(t.at("gestures"), "hold B + Y");
    EXPECT_EQ(t.at("lock_on"), "left stick click");
    EXPECT_EQ(t.at("weapon_switch"), "hold left stick click + left stick");
    // The left hand's grip and trigger stay: the arc and the belt vial on the grip, the cards
    // and the gun on the trigger.
    EXPECT_EQ(t.at("take"), "left grip");
    EXPECT_EQ(t.at("gun"), "left trigger");
    EXPECT_EQ(t.count("arc_tap"), 0u);
    EXPECT_EQ(t.count("jump"), 0u);
}

TEST(ThumbLayout, SchemeTwoStandard) {
    const auto t = Table({2, false, false, true});
    EXPECT_EQ(t.at("sprint"), "left stick click");
    EXPECT_EQ(t.at("dodge"), "A");
    EXPECT_EQ(t.at("arc"), "Y");
    EXPECT_EQ(t.count("dpad"), 0u);
    EXPECT_EQ(t.count("square"), 0u);
    EXPECT_EQ(t.at("gestures"), "hold Y + B");
    // The D-pad: the right stick click held + the right stick (weapons left / right, blood
    // bullets up, the next item down), locked on too.
    EXPECT_EQ(t.at("weapon_switch"), "hold right stick click + right stick");
    EXPECT_EQ(t.at("options"), "Menu");
    EXPECT_EQ(t.at("eye_distance"), "hold Menu + right stick");
}

// y_menu (SteamVR, whose dashboard owns Menu): Y is the menu button; the Menu combos are gone.
TEST(ThumbLayout, SchemeTwoYMenu) {
    const auto t = Table({2, false, false, true, true, true});
    EXPECT_EQ(t.at("menu"), "Y");
    EXPECT_EQ(t.at("gestures"), "hold Y + B");
    EXPECT_EQ(t.at("take"), "left grip"); // the arc
    EXPECT_EQ(t.at("vial"), "X");
    EXPECT_EQ(t.at("weapon_switch"), "hold right stick click + right stick");
    EXPECT_EQ(t.at("runtime_menu"), "Menu");
    for (const char* id : {"arc", "dpad", "square", "options", "eye_distance", "eye_reset",
                           "stereo", "mark"}) {
        EXPECT_EQ(t.count(id), 0u) << id;
    }
    // Hold Y + X: the game's Square where the arc cannot open (y_menu only), in the table and so
    // in the log line and the controls view.
    EXPECT_EQ(t.at("square_no_arc"), "hold Y + X");
    const auto s = Table({2, false, true, true, true, true});
    EXPECT_EQ(s.at("menu"), "B"); // the logical Y, with the right stick walking
    EXPECT_EQ(s.at("gestures"), "hold B + Y");
    EXPECT_EQ(s.at("square_no_arc"), "hold B + A");
    // The log line ("VR input: the controls"): the menu on Y, Menu the runtime's.
    const std::string line = Summary({2, false, false, true, true, true});
    EXPECT_NE(line.find("menu: Y"), std::string::npos) << line;
    EXPECT_NE(line.find("runtime_menu: Menu"), std::string::npos) << line;
    EXPECT_EQ(line.find("eye_distance"), std::string::npos) << line;
    // y_menu=on on Meta Link: Y is the menu and Menu still is too, with its combos.
    const auto m = Table({2, false, false, true, true, false});
    EXPECT_EQ(m.at("menu"), "Y");
    EXPECT_EQ(m.at("options"), "Menu");
    EXPECT_EQ(m.at("eye_distance"), "hold Menu + right stick");
    // Meta Link by default: Y is the arc, Menu the menu.
    const auto d = Table({2, false, false, true});
    EXPECT_EQ(d.at("arc"), "Y");
    EXPECT_EQ(d.at("options"), "Menu");
    EXPECT_EQ(d.count("menu"), 0u);
    EXPECT_EQ(d.count("square_no_arc"), 0u); // Meta Link: hold Y + X is nothing of its own
    EXPECT_NE(Summary({2, false, false, true, true, true}).find("square_no_arc: hold Y + X"),
              std::string::npos);
}

TEST(ThumbLayout, ClassicWithTheRightStick) {
    const auto t = Table({1, true, true, true});
    EXPECT_EQ(t.at("move"), "right stick");
    EXPECT_EQ(t.at("dodge"), "X");
    EXPECT_EQ(t.at("interact"), "Y");
    EXPECT_EQ(t.at("vial"), "A");
    EXPECT_EQ(t.at("item_use"), "B");
    EXPECT_EQ(t.at("l3"), "right stick click");
    EXPECT_EQ(t.at("lock_on"), "left stick click");
    EXPECT_EQ(t.at("dpad"), "hold right stick click + left stick");
    EXPECT_EQ(t.at("light"), "right trigger");
    EXPECT_EQ(t.at("transform"), "left grip");
    EXPECT_EQ(t.at("heavy"), "right grip");
    EXPECT_EQ(t.count("weapon_switch"), 0u);
    // left_scheme is the motion layout's: classic is the same with either.
    EXPECT_TRUE(Table({2, true, true, true}) == t);
}

std::string Join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) {
        s += (s.empty() ? "" : " ") + x;
    }
    return s;
}

TEST(ThumbLayout, CalloutsListEveryJobOfAControl) {
    const Layout l{2, false, true, true};
    std::vector<std::string> ids;
    for (const Bound& b : On(l, C::B)) {
        ids.push_back(b.entry.id);
    }
    // B: the arc's tap and the modifier of the gestures (the D-pad and Square went on
    // 2026-10-15).
    EXPECT_EQ(Join(ids), "arc gestures");
    ids.clear();
    for (const Bound& b : On(l, C::StickR)) {
        ids.push_back(b.entry.id);
    }
    EXPECT_EQ(Join(ids), "move stereo");
}

// ---- Labels -----------------------------------------------------------------------------------

TEST(ThumbLayout, ExpandNamesTheControlsOfTheLayout) {
    EXPECT_EQ(Expand("How the {turn} turns you.", false), "How the right stick turns you.");
    EXPECT_EQ(Expand("How the {turn} turns you.", true), "How the left stick turns you.");
    EXPECT_EQ(Expand("Grip adjust ({A}+{B})", false), "Grip adjust (A+B)");
    EXPECT_EQ(Expand("Grip adjust ({A}+{B})", true), "Grip adjust (X+Y)");
    EXPECT_EQ(Expand("{move} {move_click} {turn_click} {X}{Y}", true),
              "right stick right stick click left stick click AB");
    EXPECT_EQ(Expand("no tokens {here} {", true), "no tokens {here} {");
    EXPECT_EQ(Expand("", true), "");
}

TEST(ThumbLayout, SummaryNamesEveryEntry) {
    const Layout l{1, false, true, true};
    const std::string s = Summary(l);
    EXPECT_NE(s.find("move: right stick | turn: left stick"), std::string::npos) << s;
    EXPECT_NE(s.find("dodge: X"), std::string::npos) << s;
    EXPECT_NE(s.find("grip_adjust: X + Y"), std::string::npos) << s;
    size_t bars = 0;
    for (const char ch : s) {
        bars += ch == '|';
    }
    EXPECT_EQ(bars + 1, Resolve(l).size());
}

// ---- The hands read into the logical slots (as UpdateInput does) ------------------------------

struct Hands {
    std::array<bool, ControlCount> pressed{};
    float lx{0.0f}, ly{0.0f}, rx{0.0f}, ry{0.0f}; // the sticks as held
};

struct Logical {
    std::array<bool, ControlCount> pressed{};
    float move_x{0.0f}, move_y{0.0f}, turn_x{0.0f}, turn_y{0.0f};
    bool Is(C c) const {
        return pressed[static_cast<size_t>(c)];
    }
};

Logical Read(const Hands& h, bool swap) {
    Logical l;
    for (const C c : AllControls) {
        l.pressed[static_cast<size_t>(c)] = h.pressed[static_cast<size_t>(Physical(c, swap))];
    }
    l.move_x = swap ? h.rx : h.lx;
    l.move_y = swap ? h.ry : h.ly;
    l.turn_x = swap ? h.lx : h.rx;
    l.turn_y = swap ? h.ly : h.ry;
    return l;
}

TEST(ThumbLayout, ReadPutsEveryPhysicalControlInItsSlot) {
    for (const bool swap : {false, true}) {
        for (const C c : AllControls) {
            Hands h;
            h.pressed[static_cast<size_t>(c)] = true;
            const Logical l = Read(h, swap);
            int count = 0;
            for (const C d : AllControls) {
                count += l.Is(d) ? 1 : 0;
            }
            EXPECT_EQ(count, 1);
            EXPECT_TRUE(l.Is(Physical(c, swap))) << Label(c); // its own inverse
        }
    }
    Hands h;
    h.rx = 0.3f;
    h.ry = 0.9f;
    h.lx = -1.0f;
    const Logical l = Read(h, true);
    EXPECT_FLOAT_EQ(l.move_x, 0.3f);
    EXPECT_FLOAT_EQ(l.move_y, 0.9f);
    EXPECT_FLOAT_EQ(l.turn_x, -1.0f);
}

constexpr float Dt = 1.0f / 120.0f;

// left_scheme=2 with the right stick: the right stick pushed and clicked latches the sprint, the
// left thumb steers nothing and X leaps; steering the right stick hard makes X a dodge.
TEST(ThumbLayout, SchemeTwoSprintAndSteerOnTheRightStick) {
    namespace L = VR::Loco;
    for (const bool steer_hard : {false, true}) {
        SCOPED_TRACE(steer_hard ? "steered hard: dodge" : "along the run: leap");
        L::Controller loco;
        const L::Config cfg{};
        Hands h;
        int started = 0, leaps = 0, dodges = 0;
        bool sprinted = false;
        for (int f = 0; f < 240; ++f) {
            const float t = f * Dt;
            h.ry = 1.0f; // the right stick forward the whole time
            h.rx = 0.0f;
            h.pressed[static_cast<size_t>(C::ClickR)] = t >= 0.05f && t < 0.15f;
            if (steer_hard && t >= 1.2f) {
                h.ry = 0.0f;
                h.rx = 1.0f;
            }
            h.pressed[static_cast<size_t>(C::X)] = t >= 1.23f && t < 1.33f;
            // The left stick held off to one side: it turns, it never walks.
            h.lx = 0.2f;
            const Logical l = Read(h, true);
            const L::Out o = loco.Update(
                cfg, {true, l.Is(C::ClickL), l.Is(C::A), l.move_x, l.move_y, Dt});
            started += o.started ? 1 : 0;
            sprinted = sprinted || o.sprint;
            leaps += o.action == L::Action::Leap ? 1 : 0;
            dodges += o.action == L::Action::Dodge ? 1 : 0;
        }
        EXPECT_EQ(started, 1);
        EXPECT_TRUE(sprinted);
        EXPECT_EQ(leaps, steer_hard ? 0 : 1);
        EXPECT_EQ(dodges, steer_hard ? 1 : 0);
    }
}

// The same hands without the swap do nothing of the sort: the right stick click is not the latch.
TEST(ThumbLayout, SchemeTwoRightStickClickIsNoLatchWithoutTheSwap) {
    namespace L = VR::Loco;
    L::Controller loco;
    Hands h;
    int started = 0;
    for (int f = 0; f < 120; ++f) {
        h.ry = 1.0f;
        h.pressed[static_cast<size_t>(C::ClickR)] = f >= 6 && f < 18;
        const Logical l = Read(h, false);
        started += loco.Update({}, {true, l.Is(C::ClickL), l.Is(C::A), l.move_x, l.move_y, Dt})
                           .started
                       ? 1
                       : 0;
    }
    EXPECT_EQ(started, 0);
}

// left_scheme=1 with the right stick: a tap of the right stick click is the quick-item arc while
// the right thumb keeps walking; X held with the right stick pushed is the sprint, and the right
// stick click then jumps.
TEST(ThumbLayout, SchemeOneTapAndJumpOnTheRightStickClick) {
    namespace SC = VR::StickClick;
    {
        SC::LeftClick click;
        Hands h;
        int arcs = 0, jumps = 0;
        for (int f = 0; f < 60; ++f) {
            h.ry = 0.6f; // walking
            h.pressed[static_cast<size_t>(C::ClickR)] = f >= 10 && f < 22; // 100 ms
            const Logical l = Read(h, true);
            const bool sprinting = l.Is(C::A) && std::hypot(l.move_x, l.move_y) > 0.5f;
            const SC::Out o = click.Update({true, 0.5f}, {l.Is(C::ClickL), sprinting, Dt});
            arcs += o.arc ? 1 : 0;
            jumps += o.jump ? 1 : 0;
        }
        EXPECT_EQ(arcs, 1);
        EXPECT_EQ(jumps, 0);
    }
    {
        SC::LeftClick click;
        Hands h;
        int arcs = 0, jumps = 0;
        for (int f = 0; f < 120; ++f) {
            h.ry = 1.0f;
            h.pressed[static_cast<size_t>(C::X)] = true;                   // sprint
            h.pressed[static_cast<size_t>(C::ClickR)] = f >= 80 && f < 92; // the jump
            const Logical l = Read(h, true);
            const bool sprinting = l.Is(C::A) && std::hypot(l.move_x, l.move_y) > 0.5f;
            const SC::Out o = click.Update({true, 0.5f}, {l.Is(C::ClickL), sprinting, Dt});
            arcs += o.arc ? 1 : 0;
            jumps += o.jump ? 1 : 0;
        }
        EXPECT_EQ(jumps, 1);
        EXPECT_EQ(arcs, 0);
    }
}

// ---- The VR page's rows ---------------------------------------------------------------------

class MapStore : public VR::Page::Store {
public:
    std::map<std::string, std::string> values{{"swap_sticks", "0"}};
    std::string Get(const std::string& key) override {
        const auto it = values.find(key);
        return it == values.end() ? std::string{} : it->second;
    }
    void Set(const std::string& key, const std::string& value) override {
        values[key] = value;
    }
    void Remove(const std::string& key) override {
        values[key] = "0"; // swap_sticks' code default
    }
};

TEST(ThumbLayout, PageRowsNameTheControlsOfTheLayout) {
    namespace P = VR::Page;
    MapStore store;
    const P::Row& move_with = P::Rows()[static_cast<size_t>(P::RowIndex("move_with"))];
    const P::Row& adjust = P::Rows()[static_cast<size_t>(P::RowIndex("grip_adjust"))];
    const P::Row& turning = P::Rows()[static_cast<size_t>(P::RowIndex("turning"))];
    EXPECT_FALSE(move_with.placeholder); // live now
    EXPECT_FALSE(move_with.restart);
    EXPECT_EQ(P::RowLabel(adjust, store), "Grip adjust (A+B)");
    EXPECT_EQ(P::RowHelp(turning, store), "How the right stick turns you.");
    EXPECT_EQ(P::ValueText(move_with, store), "Left stick");
    // Move with: Right stick, as the page's right arrow does it.
    const P::Change c = P::Step(move_with, store, 1);
    EXPECT_EQ(c.text, "Right stick");
    ASSERT_EQ(c.set.size(), 1u);
    EXPECT_EQ(c.set[0].first, "swap_sticks");
    EXPECT_EQ(c.set[0].second, "1");
    EXPECT_FALSE(c.placeholder);
    EXPECT_EQ(P::RowLabel(adjust, store), "Grip adjust (X+Y)");
    EXPECT_EQ(P::RowHelp(adjust, store),
              "Press X+Y twice and hold to set how the weapon sits in your hand.");
    EXPECT_EQ(P::RowHelp(turning, store), "How the left stick turns you.");
    // Every row's text expands to no braces in either layout.
    for (const char* v : {"0", "1"}) {
        store.values["swap_sticks"] = v;
        for (const P::Row& r : P::Rows()) {
            EXPECT_EQ(P::RowLabel(r, store).find('{'), std::string::npos) << r.id;
            EXPECT_EQ(P::RowHelp(r, store).find('{'), std::string::npos) << r.id;
        }
    }
}

} // namespace
