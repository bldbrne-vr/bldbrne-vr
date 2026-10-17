// SPDX-License-Identifier: GPL-2.0-or-later

// The left stick click (core/vr/stick_click.h): the sprint jump on the press, the hold modifier
// and its combos, and the tap that toggles the quick-item arc like Y (quick_arc_l3) or is L3; and
// the tap carried through the quick-item controller (core/vr/quick_items.h) as the presenter's
// UpdateGameplay does: walking on, the arc opens and closes, an item goes away, L3 where the arc
// cannot answer.

#include <cmath>
#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "core/vr/quick_items.h"
#include "core/vr/stick_click.h"

namespace {

namespace SC = VR::StickClick;
namespace Q = VR::Quick;

constexpr float Dt = 1.0f / 120.0f; // Quest 3 over Link: the presenter's input runs per XR frame

// Drives the click alone.
struct Click {
    SC::LeftClick c;
    SC::Config cfg{};
    SC::Out out{};
    int l3 = 0, arcs = 0, jumps = 0;

    const SC::Out& Step(bool pressed, bool sprinting = false, int frames = 1) {
        for (int i = 0; i < frames; ++i) {
            out = c.Update(cfg, {pressed, sprinting, Dt});
            l3 += out.l3 ? 1 : 0;
            arcs += out.arc ? 1 : 0;
            jumps += out.jump ? 1 : 0;
        }
        return out;
    }
};

TEST(StickClick, TapTogglesTheArcOnTheRelease) {
    Click k;
    for (int i = 0; i < 12; ++i) { // 100 ms held
        const SC::Out& o = k.Step(true);
        EXPECT_FALSE(o.arc);
        EXPECT_FALSE(o.l3);
        EXPECT_TRUE(o.modifier); // held: the modifier until the release decides
    }
    const SC::Out& o = k.Step(false);
    EXPECT_TRUE(o.arc); // the release frame itself: no wait for a double tap or a timeout
    EXPECT_FALSE(o.l3);
    EXPECT_FALSE(o.modifier);
    EXPECT_NEAR(o.held_s, 12 * Dt, 1e-4f);
    k.Step(false, false, 30);
    EXPECT_EQ(k.arcs, 1);
    EXPECT_EQ(k.l3, 0);
}

TEST(StickClick, OneFrameTapCounts) {
    Click k;
    k.Step(true);
    EXPECT_TRUE(k.Step(false).arc);
}

TEST(StickClick, ArcOffTapIsL3AsBefore) {
    Click k;
    k.cfg.arc = false;
    k.Step(true, false, 10);
    const SC::Out& o = k.Step(false);
    EXPECT_TRUE(o.l3);
    EXPECT_FALSE(o.arc);
    // quick_arc_l3=0: a long hold is L3 too (the old rule had no time limit).
    k.Step(true, false, 400);
    EXPECT_TRUE(k.Step(false).l3);
    EXPECT_EQ(k.arcs, 0);
}

TEST(StickClick, SprintJumpsOnThePress) {
    for (const bool arc : {true, false}) {
        Click k;
        k.cfg.arc = arc;
        const SC::Out& p = k.Step(true, true);
        EXPECT_TRUE(p.l3);
        EXPECT_TRUE(p.jump);
        EXPECT_FALSE(p.modifier); // a jump is no modifier: R3, B and Y stay themselves
        k.Step(true, true, 20);
        EXPECT_FALSE(k.out.modifier);
        const SC::Out& r = k.Step(false, true);
        EXPECT_FALSE(r.l3);
        EXPECT_FALSE(r.arc);
        EXPECT_EQ(k.l3, 1);
        EXPECT_EQ(k.jumps, 1);
        EXPECT_EQ(k.arcs, 0);
    }
}

TEST(StickClick, SprintIsDecidedAtThePress) {
    // Clicked while walking, then A held and the stick pushed: still a tap (no jump mid-hold).
    Click k;
    k.Step(true);
    k.Step(true, true, 10);
    EXPECT_TRUE(k.out.modifier);
    EXPECT_TRUE(k.Step(false, true).arc);
    EXPECT_EQ(k.jumps, 0);
    // And the other way round: a jump press stays a jump when the sprint ends mid-hold.
    Click j;
    j.Step(true, true);
    j.Step(true, false, 10);
    EXPECT_FALSE(j.out.modifier);
    j.Step(false);
    EXPECT_EQ(j.arcs, 0);
    EXPECT_EQ(j.l3, 1);
}

TEST(StickClick, ComboReleaseSendsNothing) {
    for (const bool arc : {true, false}) {
        Click k;
        k.cfg.arc = arc;
        k.Step(true, false, 5);
        k.c.NoteCombo(); // + right stick (D-pad), + B (gestures), + Y (Square), Menu (MARK)
        k.Step(true, false, 5);
        EXPECT_TRUE(k.out.modifier); // still the modifier for further combos
        const SC::Out& o = k.Step(false);
        EXPECT_FALSE(o.arc);
        EXPECT_FALSE(o.l3);
        EXPECT_EQ(k.l3 + k.arcs, 0);
    }
}

TEST(StickClick, LongHoldIsNoTap) {
    Click k;
    k.Step(true, false, 59); // 0.49 s
    EXPECT_TRUE(k.Step(false).arc);
    Click l;
    l.Step(true, false, 62); // 0.52 s: a modifier held and given up
    const SC::Out& o = l.Step(false);
    EXPECT_FALSE(o.arc);
    EXPECT_TRUE(o.l3); // L3 as before (nothing in the game unless sprinting)
    Click m;
    m.cfg.tap_max_s = 1.0f; // quick_arc_l3_tap_ms=1000
    m.Step(true, false, 62);
    EXPECT_TRUE(m.Step(false).arc);
}

TEST(StickClick, EachPressStartsFresh) {
    Click k;
    k.Step(true, false, 3);
    k.c.NoteCombo();
    k.Step(false);
    k.Step(true, false, 3); // a new press: the last one's combo does not carry over
    EXPECT_TRUE(k.Step(false).arc);
    EXPECT_EQ(k.arcs, 1);
}

TEST(StickClick, ResetForgetsTheHold) {
    // A context switch mid-hold: the gate holds the click back (it reads released) and the
    // machines were reset: nothing fires.
    Click k;
    k.Step(true, false, 5);
    k.c.Reset();
    const SC::Out& o = k.Step(false);
    EXPECT_FALSE(o.arc);
    EXPECT_FALSE(o.l3);
    EXPECT_FALSE(k.c.Down());
}

// ---- The tap through the quick-item controller, as the presenter's UpdateGameplay ---------------

Q::Snapshot Slots() {
    Q::Snapshot s{};
    s.valid = true;
    s.slot[0] = {80, 1200, 5, 24, 20000, true}; // Molotov
    s.slot[1] = {81, 1120, 3, 18, 20009, true}; // Blue Elixir
    s.slot[2] = {82, 1110, 3, 20, -1, true};    // Beast Blood Pellet
    s.selected = 0;
    return s;
}

// One presenter frame: the click first (its tap goes to the quick items this frame), L3 to the
// game when the arc refuses the tap, L3 + Y noted as a combo.
struct Hands {
    SC::LeftClick click;
    SC::Config click_cfg{};
    Q::Controller quick;
    Q::Config quick_cfg{};
    Q::Input in{};
    Q::Output out{};
    SC::Out l3{};
    double t = 50.0;
    bool stick = false;     // the left stick click
    bool sprinting = false; // A held, the stick past half way
    float move = 0.0f;      // the left stick's push (walking: it never changes with the click)
    int game_l3 = 0;        // L3 presses sent to the game
    int square_frames = 0;
    float walked = 0.0f;    // stick travel sent to the game, in frames x push

    Hands() {
        in.gameplay = true;
        in.body = true;
        in.arm_weight = 1.0f;
        in.hand_valid = true;
        in.hand_p = {-0.2f, 1.1f, -0.3f};
        in.snapshot_fresh = true;
        in.snapshot = Slots();
    }
    void Step(int frames = 1) {
        for (int i = 0; i < frames; ++i) {
            t += Dt;
            l3 = click.Update(click_cfg, {stick, sprinting, Dt});
            game_l3 += l3.l3 ? 1 : 0;
            in.t = t;
            in.dt = Dt;
            in.l3 = stick;
            in.arc_tap = l3.arc;
            in.hover.steady_ns = static_cast<std::int64_t>(t * 1e9);
            quick.Update(quick_cfg, in, out);
            if (l3.arc && !out.tap_taken) {
                ++game_l3;
            }
            if (l3.modifier && out.square_hold) {
                click.NoteCombo();
            }
            square_frames += out.square_hold ? 1 : 0;
            walked += move; // nothing in the click or the arc touches the stick's axes
        }
    }
    void Tap(int frames = 8) {
        stick = true;
        Step(frames);
        stick = false;
        Step();
    }
};

TEST(StickClickArc, TapWhileWalkingOpensAndClosesTheArc) {
    Hands h;
    h.move = 1.0f;
    h.Step(30);
    h.Tap();
    EXPECT_EQ(h.quick.GetPhase(), Q::Phase::Open);
    EXPECT_TRUE(h.out.tap_taken);
    EXPECT_NE(h.out.log.find("OPEN n=3 sel=0 by L3"), std::string::npos) << h.out.log;
    EXPECT_EQ(h.out.scene.state, Q::SceneState::Open);
    EXPECT_FALSE(h.out.square_hold);
    EXPECT_EQ(h.game_l3, 0);
    h.Step(60);
    EXPECT_EQ(h.quick.GetPhase(), Q::Phase::Open); // stays open while walking
    h.Tap();
    EXPECT_EQ(h.quick.GetPhase(), Q::Phase::Idle);
    EXPECT_NE(h.out.log.find("CLOSE (L3)"), std::string::npos) << h.out.log;
    EXPECT_EQ(h.game_l3, 0);
    EXPECT_EQ(h.square_frames, 0);
    EXPECT_FLOAT_EQ(h.walked, 30.0f + 9.0f + 60.0f + 9.0f); // every frame walked
}

TEST(StickClickArc, TapPutsTheItemAway) {
    Hands h;
    h.Tap();
    ASSERT_EQ(h.quick.GetPhase(), Q::Phase::Open);
    h.in.grip = 1.0f; // at once after the tap without reaching: the selected item (quick draw)
    h.Step();
    ASSERT_EQ(h.quick.GetPhase(), Q::Phase::Holding);
    EXPECT_EQ(h.quick.HeldSlot(), 0);
    h.Step(30);
    h.Tap();
    EXPECT_EQ(h.quick.GetPhase(), Q::Phase::Idle);
    EXPECT_NE(h.out.log.find("PUT (L3)"), std::string::npos) << h.out.log;
    EXPECT_EQ(h.game_l3, 0);
}

TEST(StickClickArc, TapDisarmsTheVial) {
    Hands h;
    h.in.vial_armed = true;
    h.Tap();
    EXPECT_TRUE(h.out.disarm_vial); // the newest intent wins, as with Y
    EXPECT_EQ(h.quick.GetPhase(), Q::Phase::Open);
}

TEST(StickClickArc, SprintStillJumps) {
    Hands h;
    h.move = 1.0f;
    h.sprinting = true;
    h.Tap();
    EXPECT_EQ(h.game_l3, 1); // the jump, on the press
    EXPECT_EQ(h.quick.GetPhase(), Q::Phase::Idle);
}

TEST(StickClickArc, ArcUnavailableTapIsL3) {
    // The classic layout or quick_arc=0, third person (no hunter's hand), no quick-item state, a
    // cinematic or Menu held: the arc cannot open, the tap is L3 as before.
    for (int k = 0; k < 4; ++k) {
        Hands h;
        if (k == 0) {
            h.quick_cfg.enabled = false;
        } else if (k == 1) {
            h.in.body = false;
        } else if (k == 2) {
            h.in.snapshot_fresh = false;
        } else {
            h.in.gameplay = false;
        }
        h.Tap();
        EXPECT_EQ(h.game_l3, 1) << k;
        EXPECT_EQ(h.quick.GetPhase(), Q::Phase::Idle) << k;
        EXPECT_FALSE(h.out.tap_taken) << k;
        EXPECT_EQ(h.square_frames, 0) << k; // never Square
    }
}

TEST(StickClickArc, ArcOffSettingKeepsL3) {
    Hands h;
    h.click_cfg.arc = false; // quick_arc_l3=0
    h.Tap();
    EXPECT_EQ(h.game_l3, 1);
    EXPECT_EQ(h.quick.GetPhase(), Q::Phase::Idle);
}

TEST(StickClickArc, L3PlusYIsStillSquare) {
    Hands h;
    h.stick = true;
    h.Step(10);
    h.in.y = true;
    h.Step(10);
    EXPECT_TRUE(h.out.square_hold);
    h.in.y = false;
    h.Step(3);
    h.stick = false;
    h.Step();
    EXPECT_GT(h.square_frames, 0);
    EXPECT_EQ(h.quick.GetPhase(), Q::Phase::Idle); // no arc on the release
    EXPECT_EQ(h.game_l3, 0);                      // and no L3
}

TEST(StickClickArc, ComboHoldOpensNothing) {
    // L3 + right stick (a weapon swap through the D-pad) or + B: the presenter notes the combo.
    Hands h;
    h.stick = true;
    h.Step(10);
    h.click.NoteCombo();
    h.Step(10);
    h.stick = false;
    h.Step();
    EXPECT_EQ(h.quick.GetPhase(), Q::Phase::Idle);
    EXPECT_EQ(h.game_l3, 0);
}

TEST(StickClickArc, YAndTheTapToggleTheSameArc) {
    Hands h;
    h.in.y = true;
    h.Step();
    h.in.y = false;
    h.Step(20);
    ASSERT_EQ(h.quick.GetPhase(), Q::Phase::Open);
    const std::uint32_t serial = h.quick.OpenSerial();
    h.Tap(); // the tap closes what Y opened ...
    EXPECT_EQ(h.quick.GetPhase(), Q::Phase::Idle);
    h.Tap(); // ... and opens it again (a new open)
    EXPECT_EQ(h.quick.GetPhase(), Q::Phase::Open);
    EXPECT_EQ(h.quick.OpenSerial(), serial + 1);
    h.in.y = true; // Y closes what the tap opened
    h.Step();
    EXPECT_EQ(h.quick.GetPhase(), Q::Phase::Idle);
    EXPECT_NE(h.out.log.find("CLOSE (Y)"), std::string::npos) << h.out.log;
}

} // namespace
