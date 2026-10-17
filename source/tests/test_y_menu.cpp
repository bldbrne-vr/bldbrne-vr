// SPDX-License-Identifier: GPL-2.0-or-later

// Y as the menu button (core/vr/y_menu.h): with y_menu (SteamVR by default) a tap is Options and
// a 0.7 s hold the recenter, as Menu's are on Meta Link; any other button during the hold makes
// it a combo (neither), a stick pushed from the centre cancels only the recenter. Without y_menu
// X and the right stick only make the hold a combo (no arc), where hold Y + X / Y + right stick
// were the game's Square and the D-pad. Driven with the real click machine
// (core/vr/stick_click.h) as the presenter does (the gesture in UpdateInput for every context, the
// click in UpdateGameplay); also the long hold's escape, the recenter's context switch, the one
// y_menu parser and where the machine runs.

#include <gtest/gtest.h>

#include "core/vr/stick_click.h"
#include "core/vr/menu_exit_input.h"
#include "core/vr/y_menu.h"

namespace {

using VR::YMenu::Config;
using VR::YMenu::Frame;
using VR::YMenu::Gesture;
using VR::YMenu::Out;

struct Totals {
    int menu{0};
    int recenter{0};
    int combo{0};
    int cancelled{0};
    int late{0};
    int arc{0}; // the click machine's tap (Meta Link: the quick-item arc)
};

// One script frame: Y, another button's press edge, the sticks.
struct Step {
    bool y{false};
    bool other{false};
    float stick_l{0.0f};
    float stick_r{0.0f};
    bool frozen{false};
};

// Runs `frames` of `step(i)` at `hz`, Y's click machine alongside (its combos noted as the
// presenter does: Out::combo -> NoteCombo).
template <class F>
Totals Drive(bool menu, float hz, int frames, F step) {
    Gesture g;
    VR::StickClick::LeftClick click;
    const Config c{menu, 0.7f};
    const float dt = 1.0f / hz;
    Totals t;
    for (int i = 0; i < frames; ++i) {
        const Step s = step(i);
        const auto ymod = click.Update({!menu, 0.5f}, {s.y, false, dt});
        const Out o = g.Update(c, {ymod.modifier, s.other, s.stick_l, s.stick_r, s.frozen, dt});
        if (o.combo) {
            click.NoteCombo();
        }
        t.menu += o.menu;
        t.recenter += o.recenter;
        t.combo += o.combo;
        t.cancelled += o.hold_cancelled;
        t.late += o.late_release;
        t.arc += ymod.arc;
    }
    return t;
}

int Frames(float seconds, float hz) {
    return static_cast<int>(seconds * hz + 0.5f);
}

TEST(YMenu, ATapIsOptionsOnceAtEveryRate) {
    for (const float hz : {72.0f, 90.0f, 120.0f}) {
        for (const float held : {0.05f, 0.3f, 0.65f}) {
            const int down = Frames(held, hz);
            const Totals t = Drive(true, hz, down + 10, [&](int i) { return Step{i < down}; });
            EXPECT_EQ(t.menu, 1) << hz << " Hz, " << held << " s";
            EXPECT_EQ(t.recenter, 0);
            EXPECT_EQ(t.arc, 0); // never the arc's tap with y_menu
        }
    }
}

TEST(YMenu, AHoldRecentersOnceAndItsReleaseIsNothing) {
    for (const float hz : {72.0f, 90.0f, 120.0f}) {
        const int down = Frames(1.5f, hz);
        const Totals t = Drive(true, hz, down + 10, [&](int i) { return Step{i < down}; });
        EXPECT_EQ(t.recenter, 1) << hz;
        EXPECT_EQ(t.menu, 0) << hz;
        EXPECT_EQ(t.late, 0) << hz;
    }
}

TEST(YMenu, TheRecenterComesAfterSevenTenthsOfASecond) {
    Gesture g;
    const float dt = 1.0f / 90.0f;
    float fired_at = -1.0f;
    for (int i = 0; i < 90; ++i) {
        const Out o = g.Update({true, 0.7f}, {true, false, 0.0f, 0.0f, false, dt});
        if (o.recenter) {
            fired_at = (i + 1) * dt;
        }
    }
    EXPECT_GE(fired_at, 0.7f - 1e-4f); // the 63rd frame at 90 Hz (Menu's > 0.7 s too)
    EXPECT_LT(fired_at, 0.7f + 2 * dt);
}

TEST(YMenu, AnotherButtonDuringTheHoldIsACombo) {
    // Y + B (the gestures), Y + X, Y + A, a trigger, a stick click: neither the menu nor the
    // recenter, before or after 0.7 s.
    for (const float press_at : {0.1f, 0.6f}) {
        for (const float held : {0.4f, 1.5f}) {
            const float hz = 90.0f;
            const int down = Frames(held, hz);
            const int at = Frames(press_at, hz);
            if (at >= down) {
                continue;
            }
            const Totals t = Drive(true, hz, down + 10,
                                 [&](int i) { return Step{i < down, i == at}; });
            EXPECT_EQ(t.menu, 0) << press_at << " " << held;
            EXPECT_EQ(t.recenter, 0) << press_at << " " << held;
            EXPECT_EQ(t.combo, 1);
        }
    }
}

TEST(YMenu, AButtonAfterTheRecenterChangesNothing) {
    const float hz = 90.0f;
    const int down = Frames(1.2f, hz);
    const Totals t =
        Drive(true, hz, down + 10, [&](int i) { return Step{i < down, i == Frames(0.9f, hz)}; });
    EXPECT_EQ(t.recenter, 1);
    EXPECT_EQ(t.menu, 0);
}

TEST(YMenu, AStickPushedFromTheCentreCancelsOnlyTheRecenter) {
    const float hz = 90.0f;
    for (const bool right : {false, true}) {
        // Held 1.2 s with a push at 0.3 s: no recenter, nothing on the release.
        const int down = Frames(1.2f, hz);
        Totals t = Drive(true, hz, down + 10, [&](int i) {
            const float push = i >= Frames(0.3f, hz) && i < Frames(0.5f, hz) ? 0.9f : 0.0f;
            return Step{i < down, false, right ? 0.0f : push, right ? push : 0.0f};
        });
        EXPECT_EQ(t.recenter, 0) << right;
        EXPECT_EQ(t.menu, 0) << right;
        EXPECT_EQ(t.cancelled, 1) << right;
        EXPECT_EQ(t.late, 1) << right;
        // A tap while walking or turning (the push from the centre) still opens the menu.
        const int tap = Frames(0.3f, hz);
        t = Drive(true, hz, tap + 10, [&](int i) {
            const float push = i >= 5 ? 0.9f : 0.0f;
            return Step{i < tap, false, right ? 0.0f : push, right ? push : 0.0f};
        });
        EXPECT_EQ(t.menu, 1) << right;
    }
}

TEST(YMenu, AStickAlreadyPushedWhenYGoesDownCancelsNothing) {
    // Walking (the left stick held forward) or turning: Y held 1 s still recenters.
    const float hz = 90.0f;
    const int down = Frames(1.0f, hz);
    const Totals t = Drive(true, hz, down + 10, [&](int i) { return Step{i < down, false, 1.0f, 0.8f}; });
    EXPECT_EQ(t.recenter, 1);
    EXPECT_EQ(t.cancelled, 0);
}

TEST(YMenu, TheGripAdjustOwnsTheHands) {
    const float hz = 90.0f;
    for (const float held : {0.3f, 1.2f}) {
        const int down = Frames(held, hz);
        const Totals t =
            Drive(true, hz, down + 10, [&](int i) { return Step{i < down, false, 0, 0, true}; });
        EXPECT_EQ(t.menu, 0) << held;
        EXPECT_EQ(t.recenter, 0) << held;
    }
}

TEST(YMenu, AResetForgetsTheHold) {
    // The recenter's context switch: the held Y is never released as far as the machine knows.
    Gesture g;
    const float dt = 1.0f / 90.0f;
    for (int i = 0; i < 20; ++i) {
        g.Update({true, 0.7f}, {true, false, 0, 0, false, dt});
    }
    g.Reset();
    EXPECT_FALSE(g.Down());
    const Out o = g.Update({true, 0.7f}, {false, false, 0, 0, false, dt});
    EXPECT_FALSE(o.menu);
}

TEST(YMenu, MetaLinkKeepsTheArcTapAndNeverMenusOrRecenters) {
    for (const float hz : {72.0f, 90.0f, 120.0f}) {
        // A tap: the arc (the click machine's), no menu.
        int down = Frames(0.2f, hz);
        Totals t = Drive(false, hz, down + 10, [&](int i) { return Step{i < down}; });
        EXPECT_EQ(t.arc, 1) << hz;
        EXPECT_EQ(t.menu + t.recenter, 0) << hz;
        // Held 1.5 s: nothing (Menu recenters on Meta Link).
        down = Frames(1.5f, hz);
        t = Drive(false, hz, down + 10, [&](int i) { return Step{i < down}; });
        EXPECT_EQ(t.arc + t.menu + t.recenter, 0) << hz;
    }
}

TEST(YMenu, MetaLinkYHeldThenXOrTheRightStickIsNoArc) {
    const float hz = 90.0f;
    const int down = Frames(0.3f, hz);
    // Y + X (X arms the vial as always): the release opens no arc (it would disarm that vial).
    Totals t = Drive(false, hz, down + 10, [&](int i) { return Step{i < down, i == 10}; });
    EXPECT_EQ(t.arc, 0);
    EXPECT_EQ(t.combo, 1);
    // Y + the right stick pushed from the centre (it turns or flicks an item): no arc either.
    t = Drive(false, hz, down + 10, [&](int i) {
        return Step{i < down, false, 0.0f, i >= 8 && i < 14 ? 0.9f : 0.0f};
    });
    EXPECT_EQ(t.arc, 0);
    EXPECT_EQ(t.combo, 1);
    // ... but the left stick (walking) is not a combo: the tap still opens the arc.
    t = Drive(false, hz, down + 10, [&](int i) {
        return Step{i < down, false, i >= 8 ? 0.9f : 0.0f, 0.0f};
    });
    EXPECT_EQ(t.arc, 1);
    // A right stick already pushed when Y went down (a turn held) is not a combo.
    t = Drive(false, hz, down + 10, [&](int i) { return Step{i < down, false, 0.0f, 0.9f}; });
    EXPECT_EQ(t.arc, 1);
    EXPECT_EQ(t.combo, 0);
}

TEST(YMenu, TheLongHoldEscapesOnceAndItsReleaseIsNothing) {
    for (const float hz : {72.0f, 90.0f, 120.0f}) {
        Gesture g;
        const float dt = 1.0f / hz;
        int escapes = 0, recenters = 0, menus = 0, late = 0;
        float escaped_at = -1.0f;
        const int down = Frames(3.2f, hz);
        for (int i = 0; i < down + 5; ++i) {
            const Out o = g.Update({true, 0.7f, 2.5f}, {i < down, false, 0, 0, false, dt});
            escapes += o.escape;
            recenters += o.recenter;
            menus += o.menu;
            late += o.late_release;
            if (o.escape) {
                escaped_at = (i + 1) * dt;
            }
        }
        EXPECT_EQ(escapes, 1) << hz;
        EXPECT_EQ(recenters, 1) << hz; // the recenter first, at 0.7 s
        EXPECT_EQ(menus + late, 0) << hz;
        EXPECT_GE(escaped_at, 2.5f - 1e-4f) << hz;
        EXPECT_LT(escaped_at, 2.5f + 2 * dt) << hz;
    }
}

TEST(YMenu, TheEscapeNeedsTheHoldAlone) {
    const float hz = 90.0f;
    const int down = Frames(3.0f, hz);
    // B at 1 s (the gestures after the recenter): no escape.
    Gesture g;
    int escapes = 0;
    for (int i = 0; i < down + 5; ++i) {
        escapes += g.Update({true, 0.7f, 2.5f},
                            {i < down, i == Frames(1.0f, hz), 0, 0, false, 1.0f / hz})
                       .escape;
    }
    EXPECT_EQ(escapes, 0);
    // A stick pushed (walking on the flat screen) cancels the recenter, not the escape (Menu's
    // escape is timed on the button alone too).
    Gesture w;
    escapes = 0;
    int recenters = 0;
    for (int i = 0; i < down + 5; ++i) {
        const float push = i > 10 ? 0.9f : 0.0f;
        const Out o = w.Update({true, 0.7f, 2.5f}, {i < down, false, push, 0, false, 1.0f / hz});
        escapes += o.escape;
        recenters += o.recenter;
    }
    EXPECT_EQ(escapes, 1);
    EXPECT_EQ(recenters, 0);
    // Without y_menu nothing of the kind.
    Gesture link;
    escapes = 0;
    for (int i = 0; i < down + 5; ++i) {
        escapes += link.Update({false, 0.7f, 2.5f}, {i < down, false, 0, 0, false, 1.0f / hz})
                       .escape;
    }
    EXPECT_EQ(escapes, 0);
}

// The presenter's recenter switch (SwitchContext "recentered", at the fade's black): the click
// machine is reset, the gesture kept when it fired the recenter and Y is still down.
TEST(YMenu, YStaysTheModifierThroughItsOwnRecenter) {
    for (const float hz : {72.0f, 90.0f, 120.0f}) {
        Gesture g;
        VR::StickClick::LeftClick click;
        const float dt = 1.0f / hz;
        const int down = Frames(1.4f, hz);
        const int black = Frames(0.9f, hz);
        const int b_at = Frames(1.1f, hz);
        int menus = 0, recenters = 0, escapes = 0, late = 0;
        bool b_modified = false;
        for (int i = 0; i < down + 5; ++i) {
            if (i == black) {
                ASSERT_TRUE(g.KeepThroughRecenter()) << hz;
                const Gesture kept = g;
                click.Reset();
                g.Reset();
                g = kept;
            }
            const bool y = i < down;
            const auto ymod = click.Update({false, 0.5f}, {y, false, dt});
            const Out o = g.Update({true, 0.7f, 2.5f}, {y, i == b_at, 0, 0, false, dt});
            if (g.Spent()) {
                click.NoteCombo();
            }
            if (i == b_at) {
                b_modified = ymod.modifier; // B's press: the gestures, never Cross
            }
            menus += o.menu;
            recenters += o.recenter;
            escapes += o.escape;
            late += o.late_release;
        }
        EXPECT_TRUE(b_modified) << hz;
        EXPECT_EQ(recenters, 1) << hz;
        EXPECT_EQ(menus + escapes + late, 0) << hz;
    }
    // A hold that did not fire the recenter (a combo, or still short of 0.7 s) is not kept.
    Gesture g;
    for (int i = 0; i < 30; ++i) {
        g.Update({true, 0.7f, 2.5f}, {true, false, 0, 0, false, 1.0f / 90.0f});
    }
    EXPECT_FALSE(g.KeepThroughRecenter());
}

// The recenter's switch holds new pad presses back (the presenter's PadMapper::Suppress, counted
// down by XR frames; a press while it runs is dropped). Y kept through its own recenter: a Y + B
// tap whose release (the touchpad press, FireTouch) comes 0.2 s after the swap still reaches the
// game, at any game frame rate (the pad's frame: 1/60 .. 1/15 s). Any other recenter keeps 0.4 s.
TEST(YMenu, YPlusBTappedRightAfterItsRecenterReachesTheTouchpad) {
    using VR::YMenu::RecenterSuppressS;
    for (const float hz : {72.0f, 90.0f, 120.0f}) {
        for (const float game_frame : {1.0f / 60.0f, 1.0f / 30.0f, 1.0f / 15.0f}) {
            const float min_release = 1.5f * game_frame; // PadMapper::MinRelease
            const auto pressable_at = [&](float s, float after_s) {
                const int frames = static_cast<int>(after_s * hz + 0.5f);
                for (int i = 0; i < frames; ++i) {
                    s -= 1.0f / hz;
                }
                return s <= 0.0f;
            };
            EXPECT_TRUE(pressable_at(RecenterSuppressS(true, 0.4f, min_release), 0.2f))
                << hz << " " << game_frame;
            EXPECT_FALSE(pressable_at(RecenterSuppressS(false, 0.4f, min_release), 0.2f));
        }
    }
    EXPECT_FLOAT_EQ(RecenterSuppressS(true, 0.4f, 0.1f), 0.1f);
    EXPECT_FLOAT_EQ(RecenterSuppressS(false, 0.4f, 0.1f), 0.4f);
    EXPECT_FLOAT_EQ(RecenterSuppressS(true, 0.4f, 0.5f), 0.4f); // never longer than normal
}

TEST(YMenu, AComboOnYsFirstFrameStillSpendsTheArcTap) {
    // Meta Link: Y and X pressed on the same frame. The gesture runs before the click machine
    // (UpdateInput, then UpdateGameplay), whose press would forget a NoteCombo made before it:
    // the presenter notes Spent() after the click's update.
    Gesture g;
    VR::StickClick::LeftClick click;
    const float dt = 1.0f / 90.0f;
    int arcs = 0;
    for (int i = 0; i < 30; ++i) {
        const bool y = i < 20;
        g.Update({false, 0.7f}, {y, i == 0, 0, 0, false, dt});
        const auto ymod = click.Update({true, 0.5f}, {y, false, dt});
        if (g.Spent()) {
            click.NoteCombo();
        }
        arcs += ymod.arc;
    }
    EXPECT_EQ(arcs, 0);
}

TEST(YMenu, OneParserForTheIniAndTheEnv) {
    using VR::YMenu::ParseSetting;
    for (const char* w : {"auto", "AUTO", " auto "}) {
        EXPECT_EQ(ParseSetting(w), 0) << w;
    }
    for (const char* w : {"on", "1", "true", "yes", "On", "TRUE", "Yes"}) {
        EXPECT_EQ(ParseSetting(w), 1) << w;
    }
    for (const char* w : {"off", "0", "false", "no", "OFF", "False", "NO"}) {
        EXPECT_EQ(ParseSetting(w), 2) << w;
    }
    for (const char* w : {"", "2", "maybe", "onn", "y"}) {
        EXPECT_EQ(ParseSetting(w), -1) << w;
    }
}

TEST(YMenu, YTakenAwayWhileHeldIsNoTap) {
    // A game menu's closing tail holds every button back; the shoulder switch takes a press: Y
    // reads false while the button is still down. That is no release: no Options, ever.
    Gesture g;
    const float dt = 1.0f / 90.0f;
    int menus = 0, late = 0;
    for (int i = 0; i < 40; ++i) {
        const bool held = i < 30;      // the button
        const bool seen = i < 10;      // ... taken away after 10 frames
        const Out o = g.Update({true, 0.7f, 2.5f},
                               {seen, false, 0, 0, false, dt, held && !seen});
        menus += o.menu;
        late += o.late_release;
    }
    EXPECT_EQ(menus + late, 0);
    EXPECT_FALSE(g.Down());
    // An ordinary release (not taken) is still the tap.
    Gesture t;
    for (int i = 0; i < 20; ++i) {
        menus += t.Update({true, 0.7f, 2.5f}, {i < 10, false, 0, 0, false, dt, false}).menu;
    }
    EXPECT_EQ(menus, 1);
}

TEST(YMenu, TheRightStickClickWorksWhileYIsHeld) {
    using VR::YMenu::RightClickToGame;
    // left_scheme=2: Y held, the click still locks on (and + the stick, the D-pad).
    EXPECT_TRUE(RightClickToGame(true, true));
    EXPECT_TRUE(RightClickToGame(false, true));
    // left_scheme=1 / classic: the left stick click held swallows it (L3 + R3, the layout chord).
    EXPECT_FALSE(RightClickToGame(true, false));
    EXPECT_TRUE(RightClickToGame(false, false));
}

TEST(YMenu, WhereItRuns) {
    using VR::YMenu::Place;
    using VR::YMenu::Where;
    // Gameplay: y_menu, or left_scheme=2's arc combos without it.
    EXPECT_EQ(Place(true, true, true, false), Where::Gameplay);
    EXPECT_EQ(Place(false, true, true, false), Where::Gameplay);
    EXPECT_EQ(Place(false, false, true, false), Where::None);
    // The menu layout (game menus, the VR page, title / loading / cutscene screens): y_menu only.
    EXPECT_EQ(Place(true, true, false, true), Where::Menus);
    EXPECT_EQ(Place(false, true, false, true), Where::None);
    // The virtual screen chosen with Tab (the DS4 layout): Y is Triangle there.
    EXPECT_EQ(Place(true, true, false, false), Where::None);
}

} // namespace

// Exercise the menu guard and Y gesture together, including swapped logical slots.
TEST(YMenu, MenuGuardPreservesCancellationAndBackProtection) {
    constexpr std::size_t A = 0, B = 1, X = 2, Y = 3, Grip = 4, Trigger = 5;
    for (bool swapped : {false, true}) {
        const auto menu = swapped ? B : Y;
        const auto back = swapped ? Y : B;
        for (float hz : {72.f, 90.f, 120.f}) {
            for (auto other : {A, X, Grip, Trigger}) {
                for (float duration : {.3f, 3.f}) {
                    VR::YMenu::Gesture gesture;
                    std::array<bool, 6> blocked{}, previous{};
                    int actions = 0;
                    const int held = static_cast<int>(duration * hz);
                    for (int i = 0; i <= held; ++i) {
                        std::array<bool, 6> raw{};
                        raw[menu] = i < held;
                        raw[other] = i >= 5 && i < held;
                        auto input = raw;
                        VR::MenuInput::GuardExit(raw, input, blocked, true, back, back);
                        bool other_press = false;
                        for (std::size_t j = 0; j < input.size(); ++j)
                            other_press |= j != menu && input[j] && !previous[j];
                        previous = input;
                        const auto out = gesture.Update({true, .7f, 2.5f},
                            {input[menu], other_press, 0, 0, false, 1.f / hz,
                             raw[menu] && !input[menu]});
                        actions += out.menu + out.recenter + out.escape;
                    }
                    EXPECT_EQ(actions, 0) << swapped << ' ' << hz << ' ' << other;
                }
            }
        }
        std::array<bool, 6> raw{}, blocked{};
        raw[back] = raw[Grip] = raw[menu] = true;
        auto input = raw;
        VR::MenuInput::GuardExit(raw, input, blocked, true, back, back);
        EXPECT_TRUE(input[back]);
        EXPECT_FALSE(input[Grip]);
        EXPECT_FALSE(input[menu]);
        EXPECT_TRUE(blocked[Grip]);
        input = raw;
        VR::MenuInput::GuardExit(raw, input, blocked, false, back, back);
        for (bool down : input) EXPECT_FALSE(down);
        // With y_menu disabled, both B and Y still protect a closing menu.
        for (auto key : {B, Y}) {
            raw = {}; blocked = {}; raw[key] = raw[Grip] = true; input = raw;
            VR::MenuInput::GuardExit(raw, input, blocked, true, B, Y);
            EXPECT_TRUE(input[key]);
            EXPECT_FALSE(input[Grip]);
        }
    }
}
